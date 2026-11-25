/* SPDX-License-Identifier: MIT
 * 离线时长授权（consent）：官方微信 OAuth 通道，独立于网页版 cookie。
 * 协议流程参考开源项目 weread-omni（墨水屏官方 API）。
 */
#include "weread_consent.h"
#include "weread.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_wifi.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "psa/crypto.h"
#include "read_pico_sd.h"
#include "read_pico_sd_access.h"

/* 本文件私有的 SD 访问作用域（weread.c 里的同名宏不对外暴露）。 */
#define CONSENT_SD_SCOPE(path) __attribute__((cleanup(read_pico_sd_access_scope_end))) \
    read_pico_sd_access_scope_t scope = read_pico_sd_access_scope(path)

static const char *TAG = "WRconsent";

/* 官方常量（来自 weread-omni / 墨水屏 2.1.2）。 */
#define MOBILE_BASE   "https://i.weread.qq.com"
#define WX_APPID      "wxab9b71ad2b90ff34"
#define DEVICE_PREFIX "eink334691225"
#define INSTALL_PREFIX "eink31"
#define DEVICE_NAME   "BOOX"
#define MOBILE_UA     "WeRead/2.1.2 WRBrand/Onyx wr_eink Dalvik/2.1.0 (Linux; U; Android 11; BOOX Build/onyx)"
#define CONSENT_FILE  "/sdcard/.weread/consent.json"
#define CONSENT_TEMP  "/sdcard/.weread/consent.part"

typedef struct {
    char vid[48];
    char access_token[384];
    char refresh_token[384];
    char device_id[64];
    char install_id[64];
    int64_t obtained_ms;
} consent_cred_t;

static consent_cred_t s_cred;
static SemaphoreHandle_t s_lock;
static TaskHandle_t s_task;
/* 协作式取消请求：cancel 只置位，由任务在安全点（HTTP 调用之间）自行退出，
 * 避免在 http_call 中途 vTaskDelete 强杀导致 client/socket/select 块泄漏。 */
static volatile bool s_cancel_request;

static volatile wr_consent_state_t s_state = WR_CONSENT_IDLE;
static char s_qr_url[256];
static char s_message[128];

/* ---------- 工具 ---------- */

static void lock(void) { if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY); }
static void unlock(void) { if (s_lock) xSemaphoreGive(s_lock); }

static void set_state(wr_consent_state_t st, const char *msg) {
    lock();
    s_state = st;
    if (msg) snprintf(s_message, sizeof(s_message), "%s", msg);
    unlock();
}

static void sha256_hex(const char *input, char out[65]) {
    uint8_t digest[32];
    size_t written = 0;
    psa_crypto_init();
    if (psa_hash_compute(PSA_ALG_SHA_256, (const uint8_t *)input, strlen(input),
                         digest, sizeof(digest), &written) == PSA_SUCCESS && written == 32) {
        static const char hex[] = "0123456789abcdef";
        for (int i = 0; i < 32; ++i) {
            out[2*i] = hex[digest[i] >> 4];
            out[2*i+1] = hex[digest[i] & 15];
        }
        out[64] = 0;
    } else {
        out[0] = 0;
    }
}

static void random_digits(char *out, unsigned count) {
    for (unsigned i = 0; i < count; ++i) out[i] = (char)('0' + esp_random() % 10);
    out[count] = 0;
}

static void new_device_id(char *out, size_t cap) {
    /* 前缀 + 19 位随机数字。 */
    char digits[20];
    random_digits(digits, 19);
    snprintf(out, cap, "%s%s", DEVICE_PREFIX, digits);
}

static void new_install_id(char *out, size_t cap) {
    char digits[27];
    random_digits(digits, 26);
    snprintf(out, cap, "%s%s", INSTALL_PREFIX, digits);
}

/* ---------- 独立 HTTP 客户端 ---------- */

typedef struct {
    char key[40];
    char value[400];
} http_header_t;

/* 同步发起一次请求，返回响应体（PSRAM），*out_len 为长度。失败返回 NULL。 */
static char *http_call(const char *url, const char *post_body,
                       const http_header_t *headers, unsigned header_count,
                       size_t *out_len, int timeout_ms) {
    esp_http_client_config_t config = {
        .url = url,
        .timeout_ms = timeout_ms,
        .buffer_size = 4096,
        .buffer_size_tx = 2048,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .disable_auto_redirect = true,
        .user_agent = MOBILE_UA,
    };
    esp_http_client_handle_t http = esp_http_client_init(&config);
    if (!http) return NULL;

    /* 官方版本头。 */
    esp_http_client_set_header(http, "baseapi", "30");
    esp_http_client_set_header(http, "appver", "2.1.2.10245900");
    esp_http_client_set_header(http, "basever", "2.1.2.10245900");
    esp_http_client_set_header(http, "osver", "11");
    esp_http_client_set_header(http, "channelId", "900");
    for (unsigned i = 0; i < header_count; ++i)
        esp_http_client_set_header(http, headers[i].key, headers[i].value);

    int body_len = post_body ? (int)strlen(post_body) : 0;
    if (post_body) {
        esp_http_client_set_method(http, HTTP_METHOD_POST);
        esp_http_client_set_header(http, "Content-Type", "application/json; charset=UTF-8");
        esp_http_client_set_post_field(http, post_body, body_len);
    }

    char *result = NULL;
    size_t capacity = 0, used = 0;
    esp_err_t err = esp_http_client_open(http, body_len);
    if (err != ESP_OK) goto done;
    if (post_body) {
        int sent = 0;
        while (sent < body_len) {
            int n = esp_http_client_write(http, post_body + sent, body_len - sent);
            if (n <= 0) break;
            sent += n;
        }
    }
    int content_length = esp_http_client_fetch_headers(http);
    int status = esp_http_client_get_status_code(http);
    (void)status;
    for (;;) {
        if (used + 2048 > capacity) {
            size_t next = capacity ? capacity * 2 : 2048;
            char *grown = heap_caps_realloc(result, next + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (!grown) goto done;
            result = grown; capacity = next;
        }
        int n = esp_http_client_read(http, result + used, (int)(capacity - used));
        if (n <= 0) {
            if (esp_http_client_is_complete_data_received(http)) break;
            if (content_length >= 0 && used >= (size_t)content_length) break;
            if (n < 0) goto done;
        } else {
            used += n;
        }
    }
    if (result) { result[used] = 0; if (out_len) *out_len = used; }

done:
    esp_http_client_cleanup(http);
    if (!result && out_len) *out_len = 0;
    return result;
}

/* ---------- App 协议长连接（underlines/readreviews 复用，避免每请求冷 DNS+TLS） ---------- */

static esp_http_client_handle_t s_app_keepalive;
static int64_t s_app_used_us;
static SemaphoreHandle_t s_ka_lock;
#define APP_KEEPALIVE_IDLE_DROP_US (30 * 1000000LL)

/* 调用方必须持有 s_ka_lock。/ Caller must hold s_ka_lock. */
static void app_keepalive_drop_locked(void) {
    if (s_app_keepalive) {
        esp_http_client_cleanup(s_app_keepalive);
        s_app_keepalive = NULL;
    }
    s_app_used_us = 0;
}

/* 供 perform 之外的调用点使用：自取互斥量再清理。/ For call sites outside perform: take the
 * keepalive mutex, then tear down. */
static void app_keepalive_drop(void) {
    if (!s_ka_lock) { app_keepalive_drop_locked(); return; }
    xSemaphoreTake(s_ka_lock, portMAX_DELAY);
    app_keepalive_drop_locked();
    xSemaphoreGive(s_ka_lock);
}

/* 链路断开（含主动关 WiFi）时，趁 esp_netif 还活着就清理长连接：客户端里残留的 RX 自定义
 * pbuf 会在 cleanup 中释放，此时 netif 的 L2 回收函数指针仍有效。若拖到离线后再清理，netif
 * 已销毁，pbuf 的回收回调变成野指针（实测 Instruction access fault 0x4800003e）。
 * On link loss, tear down the client while the netif (and its L2 free callback) is still valid;
 * deferring it until offline leaves a stale RX pbuf whose custom free function faults. */
static void app_keepalive_link_event(void *arg, esp_event_base_t base, int32_t id, void *data) {
    (void)arg; (void)base; (void)id; (void)data;
    if (!s_ka_lock) return;
    /* 最多等一个在途请求结束；拿不到锁就让下一次断连/perform 兜底，不阻塞事件循环。 */
    if (xSemaphoreTake(s_ka_lock, pdMS_TO_TICKS(20000))) {
        if (s_app_keepalive) ESP_LOGI(TAG, "app keepalive dropped on link down");
        app_keepalive_drop_locked();
        xSemaphoreGive(s_ka_lock);
    }
}

/* 在长连接上同步执行一次请求并复用暖连接，返回 PSRAM 响应体；连接坏了就丢弃，下次重连。
 * Perform one request on the persistent App connection, reusing the warm link; drop it on error. */
static char *app_keepalive_perform(const char *url, const char *post_body,
                                   const http_header_t *headers, unsigned header_count,
                                   size_t *out_len, int timeout_ms) {
    /* 与断连事件回调互斥：防止在途请求使用的客户端被事件回调同时释放。
     * Serialize against the link-down handler so the in-flight client is never freed underneath us. */
    if (s_ka_lock) xSemaphoreTake(s_ka_lock, portMAX_DELAY);
    if (s_app_keepalive && s_app_used_us &&
        esp_timer_get_time() - s_app_used_us > APP_KEEPALIVE_IDLE_DROP_US) {
        ESP_LOGI(TAG, "app keepalive dropped idle");
        app_keepalive_drop_locked();
    }
    if (!s_app_keepalive) {
        esp_http_client_config_t config = {
            .url = url,
            .timeout_ms = timeout_ms,
            .buffer_size = 4096,
            .buffer_size_tx = 2048,
            .crt_bundle_attach = esp_crt_bundle_attach,
            .disable_auto_redirect = true,
            .keep_alive_enable = true,
            .keep_alive_idle = 30,
            .keep_alive_interval = 5,
            .keep_alive_count = 5,
            .user_agent = MOBILE_UA,
        };
        s_app_keepalive = esp_http_client_init(&config);
        if (!s_app_keepalive) {
            if (out_len) *out_len = 0;
            if (s_ka_lock) xSemaphoreGive(s_ka_lock);
            return NULL;
        }
        /* 官方版本头只在建连时设一次，随连接复用。/ Version headers set once, reused thereafter. */
        esp_http_client_set_header(s_app_keepalive, "baseapi", "30");
        esp_http_client_set_header(s_app_keepalive, "appver", "2.1.2.10245900");
        esp_http_client_set_header(s_app_keepalive, "basever", "2.1.2.10245900");
        esp_http_client_set_header(s_app_keepalive, "osver", "11");
        esp_http_client_set_header(s_app_keepalive, "channelId", "900");
    } else if (esp_http_client_set_url(s_app_keepalive, url) != ESP_OK) {
        app_keepalive_drop_locked();
        if (out_len) *out_len = 0;
        if (s_ka_lock) xSemaphoreGive(s_ka_lock);
        return NULL;
    }
    for (unsigned i = 0; i < header_count; ++i)
        esp_http_client_set_header(s_app_keepalive, headers[i].key, headers[i].value);

    int body_len = post_body ? (int)strlen(post_body) : 0;
    if (post_body) {
        esp_http_client_set_method(s_app_keepalive, HTTP_METHOD_POST);
        esp_http_client_set_header(s_app_keepalive, "Content-Type",
                                   "application/json; charset=UTF-8");
        esp_http_client_set_post_field(s_app_keepalive, post_body, body_len);
    } else {
        esp_http_client_set_method(s_app_keepalive, HTTP_METHOD_GET);
        esp_http_client_set_post_field(s_app_keepalive, NULL, 0);
    }

    char *result = NULL;
    size_t capacity = 0, used = 0;
    esp_err_t err = esp_http_client_open(s_app_keepalive, body_len);
    if (err != ESP_OK) { ESP_LOGW(TAG, "app_app open err=%d body=%u", (int)err, (unsigned)body_len); goto fail; }
    if (post_body) {
        int sent = 0;
        while (sent < body_len) {
            int n = esp_http_client_write(s_app_keepalive, post_body + sent, body_len - sent);
            if (n <= 0) { ESP_LOGW(TAG, "app_app write stop sent=%d/%d", sent, body_len); break; }
            sent += n;
        }
    }
    int content_length = esp_http_client_fetch_headers(s_app_keepalive);
    for (;;) {
        if (used + 2048 > capacity) {
            size_t next = capacity ? capacity * 2 : 2048;
            char *grown = heap_caps_realloc(result, next + 1,
                                           MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (!grown) { ESP_LOGW(TAG, "app_app grow fail used=%u", (unsigned)used); goto fail; }
            result = grown; capacity = next;
        }
        int n = esp_http_client_read(s_app_keepalive, result + used, (int)(capacity - used));
        if (n <= 0) {
            if (esp_http_client_is_complete_data_received(s_app_keepalive)) break;
            if (content_length >= 0 && used >= (size_t)content_length) break;
            if (n < 0) { ESP_LOGW(TAG, "app_app read err n=%d used=%u cl=%d", n, (unsigned)used, content_length); goto fail; }
        } else {
            used += n;
        }
    }
    result[used] = 0;
    if (out_len) *out_len = used;
    s_app_used_us = esp_timer_get_time();
    if (s_ka_lock) xSemaphoreGive(s_ka_lock);
    return result;

fail:
    app_keepalive_drop_locked();
    if (result) free(result);
    if (out_len) *out_len = 0;
    if (s_ka_lock) xSemaphoreGive(s_ka_lock);
    return NULL;
}

/* ---------- 凭证持久化 ---------- */

static void cred_save(void) {
    CONSENT_SD_SCOPE(CONSENT_FILE);
    if (!scope.allowed) { ESP_LOGE(TAG, "cred_save: SD scope denied, credentials NOT persisted"); return; }
    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "vid", s_cred.vid);
    cJSON_AddStringToObject(j, "accessToken", s_cred.access_token);
    cJSON_AddStringToObject(j, "refreshToken", s_cred.refresh_token);
    cJSON_AddStringToObject(j, "deviceId", s_cred.device_id);
    cJSON_AddStringToObject(j, "installId", s_cred.install_id);
    cJSON_AddNumberToObject(j, "obtainedMs", (double)s_cred.obtained_ms);
    char *text = cJSON_PrintUnformatted(j);
    bool saved = false;
    if (text) {
        FILE *f = fopen(CONSENT_TEMP, "wb");
        if (f) {
            size_t want = strlen(text), got = fwrite(text, 1, want, f);
            int closed = fclose(f);
            if (got == want && closed == 0) {
                /* FAT 上 rename 不能覆盖已存在目标：先删旧文件（忽略不存在），再转正。 */
                if (unlink(CONSENT_FILE) != 0 && errno != ENOENT) {
                    ESP_LOGE(TAG, "cred_save: unlink old failed errno=%d", errno);
                } else if (rename(CONSENT_TEMP, CONSENT_FILE) == 0) {
                    saved = true;
                } else {
                    ESP_LOGE(TAG, "cred_save: rename failed errno=%d", errno);
                }
            } else {
                ESP_LOGE(TAG, "cred_save: write/close fail got=%u want=%u closed=%d",
                         (unsigned)got, (unsigned)want, closed);
            }
        } else { ESP_LOGE(TAG, "cred_save: fopen temp failed errno=%d", errno); }
        free(text);
    } else { ESP_LOGE(TAG, "cred_save: cJSON print failed"); }
    cJSON_Delete(j);
    ESP_LOGI(TAG, "cred_save: persisted=%d vid=%u at=%u rt=%u bytes path=%s",
             (int)saved, (unsigned)strlen(s_cred.vid),
             (unsigned)strlen(s_cred.access_token), (unsigned)strlen(s_cred.refresh_token),
             CONSENT_FILE);
}

static void cred_restore(void) {
    CONSENT_SD_SCOPE(CONSENT_FILE);
    if (!scope.allowed) { ESP_LOGW(TAG, "cred_restore: SD scope denied"); return; }
    FILE *f = fopen(CONSENT_FILE, "rb");
    if (!f) { ESP_LOGW(TAG, "cred_restore: no file errno=%d (first authorization expected)", errno); return; }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size <= 0 || size > 4096) { fclose(f); ESP_LOGE(TAG, "cred_restore: bad size=%ld", size); return; }
    char *text = heap_caps_malloc((size_t)size + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!text) { fclose(f); ESP_LOGE(TAG, "cred_restore: alloc fail"); return; }
    size_t rd = fread(text, 1, size, f);
    text[rd] = 0;
    fclose(f);
    cJSON *j = cJSON_Parse(text);
    free(text);
    if (!j) { ESP_LOGE(TAG, "cred_restore: parse fail"); return; }
    const cJSON *v;
    if ((v = cJSON_GetObjectItem(j, "vid")) && cJSON_IsString(v))
        snprintf(s_cred.vid, sizeof(s_cred.vid), "%s", v->valuestring);
    if ((v = cJSON_GetObjectItem(j, "accessToken")) && cJSON_IsString(v))
        snprintf(s_cred.access_token, sizeof(s_cred.access_token), "%s", v->valuestring);
    if ((v = cJSON_GetObjectItem(j, "refreshToken")) && cJSON_IsString(v))
        snprintf(s_cred.refresh_token, sizeof(s_cred.refresh_token), "%s", v->valuestring);
    if ((v = cJSON_GetObjectItem(j, "deviceId")) && cJSON_IsString(v))
        snprintf(s_cred.device_id, sizeof(s_cred.device_id), "%s", v->valuestring);
    if ((v = cJSON_GetObjectItem(j, "installId")) && cJSON_IsString(v))
        snprintf(s_cred.install_id, sizeof(s_cred.install_id), "%s", v->valuestring);
    if ((v = cJSON_GetObjectItem(j, "obtainedMs")) && cJSON_IsNumber(v))
        s_cred.obtained_ms = (int64_t)v->valuedouble;
    cJSON_Delete(j);
    ESP_LOGI(TAG, "cred_restore: loaded vid=%u at=%u rt=%u authorized=%d",
             (unsigned)strlen(s_cred.vid), (unsigned)strlen(s_cred.access_token),
             (unsigned)strlen(s_cred.refresh_token), (int)wr_consent_authorized());
}

/* ---------- OAuth 授权任务 ---------- */

static void consent_task(void *arg) {
    (void)arg;
    set_state(WR_CONSENT_PREPARING, NULL);

    /* 1. wxticket。 */
    size_t len = 0;
    if (s_cancel_request) goto finish;
    char *resp = http_call(MOBILE_BASE "/wxticket?nonceStr=weread", NULL, NULL, 0, &len, 15000);
    if (!resp) { set_state(WR_CONSENT_FAILED, "获取授权票据失败，请检查网络"); goto finish; }
    cJSON *ticket = cJSON_Parse(resp);
    free(resp);
    const cJSON *sig = ticket ? cJSON_GetObjectItem(ticket, "signature") : NULL;
    const cJSON *tst = ticket ? cJSON_GetObjectItem(ticket, "timeStamp") : NULL;
    if (!cJSON_IsString(sig) || !cJSON_IsNumber(tst)) {
        if (ticket) cJSON_Delete(ticket);
        set_state(WR_CONSENT_FAILED, "授权票据返回异常"); goto finish;
    }

    /* 2. qrconnect 拿 uuid。 */
    char url[1200];
    snprintf(url, sizeof(url),
             "https://open.weixin.qq.com/connect/sdk/qrconnect?appid=%s&noncestr=weread"
             "&timestamp=%lld&scope=snsapi_userinfo%%2Csnsapi_timeline%%2Csnsapi_friend&signature=%s",
             WX_APPID, (long long)tst->valuedouble, sig->valuestring);
    cJSON_Delete(ticket);
    resp = http_call(url, NULL, NULL, 0, &len, 15000);
    if (!resp) { set_state(WR_CONSENT_FAILED, "获取授权二维码失败"); goto finish; }
    cJSON *qr = cJSON_Parse(resp);
    free(resp);
    const cJSON *uuid = qr ? cJSON_GetObjectItem(qr, "uuid") : NULL;
    const cJSON *errc = qr ? cJSON_GetObjectItem(qr, "errcode") : NULL;
    if (!cJSON_IsString(uuid) || (errc && errc->valueint != 0)) {
        if (qr) cJSON_Delete(qr);
        set_state(WR_CONSENT_FAILED, "二维码生成失败"); goto finish;
    }

    /* 二维码内容（confirm 链接），交给 UI 显示。 */
    lock();
    snprintf(s_qr_url, sizeof(s_qr_url),
             "https://open.weixin.qq.com/connect/confirm?uuid=%s", uuid->valuestring);
    s_state = WR_CONSENT_WAIT_SCAN;
    unlock();

    /* 3. 长轮询直到确认/过期/拒绝。 */
    char wx_code[256] = "";
    int last = 0;
    bool done = false;
    int64_t deadline = esp_timer_get_time() + 5 * 60 * 1000000LL;
    while (!done && !s_cancel_request && esp_timer_get_time() < deadline) {
        if (s_cancel_request) break;
        snprintf(url, sizeof(url),
                 "https://long.open.weixin.qq.com/connect/l/qrconnect?f=json&uuid=%s&last=%d",
                 uuid->valuestring, last);
        resp = http_call(url, NULL, NULL, 0, &len, 15000);
        if (!resp) { vTaskDelay(pdMS_TO_TICKS(1000)); continue; }
        cJSON *poll = cJSON_Parse(resp);
        free(resp);
        int code = -1;
        const cJSON *we = poll ? cJSON_GetObjectItem(poll, "wx_errcode") : NULL;
        if (cJSON_IsNumber(we)) code = we->valueint;
        last = code;
        if (code == 405) {
            const cJSON *wc = cJSON_GetObjectItem(poll, "wx_code");
            if (cJSON_IsString(wc)) snprintf(wx_code, sizeof(wx_code), "%s", wc->valuestring);
            done = true;
        } else if (code == 404) {
            set_state(WR_CONSENT_SCANNED, NULL);
        } else if (code == 402) {
            if (poll) cJSON_Delete(poll);
            if (qr) cJSON_Delete(qr);
            set_state(WR_CONSENT_EXPIRED, "二维码已过期，请重新授权"); goto finish;
        } else if (code == 403) {
            if (poll) cJSON_Delete(poll);
            if (qr) cJSON_Delete(qr);
            set_state(WR_CONSENT_DECLINED, "授权已取消，可重新发起"); goto finish;
        }
        if (poll) cJSON_Delete(poll);
        if (!done) vTaskDelay(pdMS_TO_TICKS(1000));
    }
    cJSON_Delete(qr);
    if (s_cancel_request) goto finish;
    if (!done || !wx_code[0]) { set_state(WR_CONSENT_FAILED, "授权超时，请重试"); goto finish; }

    /* 4. 用 wx_code 换官方凭证。 */
    int64_t ts = esp_timer_get_time() / 1000;
    unsigned rnd = esp_random() % 1000;
    if (!s_cred.device_id[0]) new_device_id(s_cred.device_id, sizeof(s_cred.device_id));
    if (!s_cred.install_id[0]) new_install_id(s_cred.install_id, sizeof(s_cred.install_id));
    char sign_input[256], sign[65];
    snprintf(sign_input, sizeof(sign_input), "%lld%s%u", (long long)ts, s_cred.device_id, rnd);
    sha256_hex(sign_input, sign);

    cJSON *body = cJSON_CreateObject();
    cJSON_AddNumberToObject(body, "appFirstInstall", 1);
    cJSON_AddStringToObject(body, "code", wx_code);
    cJSON_AddStringToObject(body, "deviceId", s_cred.device_id);
    cJSON_AddStringToObject(body, "deviceName", DEVICE_NAME);
    cJSON_AddStringToObject(body, "installId", s_cred.install_id);
    cJSON_AddNumberToObject(body, "isAutoLogout", 0);
    cJSON_AddNumberToObject(body, "isFromQrcode", 1);
    cJSON_AddNumberToObject(body, "random", rnd);
    cJSON_AddStringToObject(body, "signature", sign);
    cJSON_AddNumberToObject(body, "timestamp", (double)ts);
    cJSON_AddStringToObject(body, "trackId", "");
    cJSON_AddNumberToObject(body, "deviceType", 3);
    char *body_text = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    if (s_cancel_request) { free(body_text); goto finish; }

    resp = http_call(MOBILE_BASE "/login", body_text, NULL, 0, &len, 15000);
    free(body_text);
    if (!resp) { set_state(WR_CONSENT_FAILED, "换取授权凭证失败"); goto finish; }
    ESP_LOGI(TAG, "login raw len=%d body=%.400s", (int)len, resp);
    cJSON *login = cJSON_Parse(resp);
    free(resp);
    const cJSON *at = login ? cJSON_GetObjectItem(login, "accessToken") : NULL;
    const cJSON *rt2 = login ? cJSON_GetObjectItem(login, "refreshToken") : NULL;
    const cJSON *vid = login ? cJSON_GetObjectItem(login, "vid") : NULL;
    if (!cJSON_IsString(at) || !cJSON_IsString(rt2)) {
        if (login) cJSON_Delete(login);
        set_state(WR_CONSENT_FAILED, "服务器未返回有效凭证"); goto finish;
    }
    snprintf(s_cred.access_token, sizeof(s_cred.access_token), "%s", at->valuestring);
    snprintf(s_cred.refresh_token, sizeof(s_cred.refresh_token), "%s", rt2->valuestring);
    /* vid 可能是字符串，也可能是数字（官方参考 requireIdentityValue 两种都处理）。 */
    if (cJSON_IsString(vid)) {
        snprintf(s_cred.vid, sizeof(s_cred.vid), "%s", vid->valuestring);
    } else if (cJSON_IsNumber(vid)) {
        snprintf(s_cred.vid, sizeof(s_cred.vid), "%lld", (long long)vid->valuedouble);
    }
    ESP_LOGI(TAG, "login parsed vid='%s' at_len=%u rt_len=%u", s_cred.vid,
             (unsigned)strlen(s_cred.access_token), (unsigned)strlen(s_cred.refresh_token));
    s_cred.obtained_ms = esp_timer_get_time() / 1000;
    cJSON_Delete(login);
    cred_save();
    set_state(WR_CONSENT_OK, "授权成功");

finish:
    s_task = NULL;
    TaskHandle_t self = xTaskGetCurrentTaskHandle();
    vTaskDelete(self);
}

/* ---------- 对外接口 ---------- */

bool wr_consent_authorized(void) {
    lock();
    bool ok = s_cred.vid[0] && s_cred.access_token[0] && s_cred.refresh_token[0];
    unlock();
    return ok;
}

bool wr_consent_vid(char *out, size_t out_size) {
    if (!out || !out_size) return false;
    out[0] = 0;
    lock();
    bool ok = s_cred.vid[0] != 0;
    if (ok) snprintf(out, out_size, "%s", s_cred.vid);
    unlock();
    return ok;
}

/* 一次取 consent 三件套，供 weread.c 调 /web/login/session/init 把 consent 授权换成网页会话。
   任一缓冲不足或未授权返回 false。 */
bool wr_consent_tokens(char *vid, size_t vid_sz,
                       char *access, size_t access_sz,
                       char *refresh, size_t refresh_sz) {
    if (!vid || !vid_sz || !access || !access_sz || !refresh || !refresh_sz) return false;
    vid[0] = access[0] = refresh[0] = 0;
    lock();
    bool ok = s_cred.vid[0] && s_cred.access_token[0] && s_cred.refresh_token[0];
    if (ok) {
        snprintf(vid, vid_sz, "%s", s_cred.vid);
        snprintf(access, access_sz, "%s", s_cred.access_token);
        snprintf(refresh, refresh_sz, "%s", s_cred.refresh_token);
    }
    unlock();
    return ok;
}

void wr_consent_begin(void) {
    if (s_task) return;
    s_cancel_request = false;
    lock();
    s_state = WR_CONSENT_PREPARING;
    s_qr_url[0] = 0;
    s_message[0] = 0;
    unlock();
    /* 任务栈放内部 RAM（含 TLS 与文件操作）；8KB 跑完整 HTTPS+cJSON 偏紧，提到 12KB。 */
    if (xTaskCreate(consent_task, "consent", 12288, NULL, 5, &s_task) != pdPASS) {
        ESP_LOGE(TAG, "consent task create failed");
        set_state(WR_CONSENT_FAILED, "系统资源不足，无法开始授权");
    } else {
        ESP_LOGI(TAG, "consent task started stack=12288");
    }
}

void wr_consent_cancel(void) {
    /* 协作式：只置取消位，任务在当前 HTTP 调用返回后于安全点自行 goto finish，
     * 由 finish 路径把 s_task 置空并 vTaskDelete 自身，保证 client/socket/select 块正常释放。
     * 若任务已不存在（s_task 为空），直接复位状态即可。 */
    if (s_task) {
        s_cancel_request = true;
        set_state(WR_CONSENT_IDLE, NULL);
    } else {
        s_cancel_request = false;
        set_state(WR_CONSENT_IDLE, NULL);
    }
}

void wr_consent_poll(wr_consent_status_t *out) {
    if (!out) return;
    lock();
    out->state = s_state;
    snprintf(out->qr_url, sizeof(out->qr_url), "%s", s_qr_url);
    snprintf(out->message, sizeof(out->message), "%s", s_message);
    unlock();
}

void wr_consent_logout(void) {
    ESP_LOGW(TAG, "wr_consent_logout: clearing credentials (caller path)");
    if (s_task) wr_consent_cancel();
    memset(&s_cred, 0, sizeof(s_cred));
    int u = unlink(CONSENT_FILE);
    ESP_LOGW(TAG, "wr_consent_logout: unlink result=%d errno=%d", u, u ? errno : 0);
    set_state(WR_CONSENT_IDLE, NULL);
}

/* ---------- 官方时长上报 ---------- */

/* 用 refreshToken 换新 accessToken（对应官方 TokenManager.mintAccessToken）。
 * 刷新签名只含 timestamp+deviceId+random，refreshToken 放在 body 里，不参与签名。 */
static bool consent_refresh(void) {
    if (!s_cred.refresh_token[0] || !s_cred.device_id[0]) {
        ESP_LOGW(TAG, "refresh: missing refresh_token/device_id");
        return false;
    }
    int64_t ts = esp_timer_get_time() / 1000;
    unsigned rnd = esp_random() % 1000;
    char sign_input[256], sign[65];
    snprintf(sign_input, sizeof(sign_input), "%lld%s%u", (long long)ts, s_cred.device_id, rnd);
    sha256_hex(sign_input, sign);

    cJSON *body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "deviceId", s_cred.device_id);
    cJSON_AddStringToObject(body, "deviceName", DEVICE_NAME);
    cJSON_AddNumberToObject(body, "inBackground", 0);
    cJSON_AddNumberToObject(body, "kickType", 1);
    cJSON_AddNumberToObject(body, "random", rnd);
    cJSON_AddStringToObject(body, "refCgi", "");
    cJSON_AddStringToObject(body, "refreshToken", s_cred.refresh_token);
    cJSON_AddStringToObject(body, "signature", sign);
    cJSON_AddNumberToObject(body, "timestamp", (double)ts);
    cJSON_AddStringToObject(body, "trackId", "");
    cJSON_AddNumberToObject(body, "deviceType", 3);
    char *body_text = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);

    size_t len = 0;
    char *resp = http_call(MOBILE_BASE "/login", body_text, NULL, 0, &len, 15000);
    free(body_text);
    if (!resp) { ESP_LOGW(TAG, "refresh: no response"); return false; }
    ESP_LOGI(TAG, "refresh resp_len=%u body: %.*s", (unsigned)len,
             (int)(len > 300 ? 300 : len), resp);
    cJSON *j = cJSON_Parse(resp);
    free(resp);
    if (!j) { ESP_LOGW(TAG, "refresh: non-JSON response"); return false; }
    const cJSON *at = cJSON_GetObjectItem(j, "accessToken");
    const cJSON *rt = cJSON_GetObjectItem(j, "refreshToken");
    const cJSON *vid = cJSON_GetObjectItem(j, "vid");
    if (!cJSON_IsString(at)) {
        const cJSON *ec = cJSON_GetObjectItem(j, "errCode");
        const cJSON *ec2 = cJSON_GetObjectItem(j, "errcode");
        const cJSON *em = cJSON_GetObjectItem(j, "errMsg");
        ESP_LOGW(TAG, "refresh rejected errCode=%d errMsg=%s",
                 ec ? ec->valueint : (ec2 ? ec2->valueint : -1),
                 (em && cJSON_IsString(em)) ? em->valuestring : "");
        cJSON_Delete(j);
        return false;
    }
    snprintf(s_cred.access_token, sizeof(s_cred.access_token), "%s", at->valuestring);
    if (cJSON_IsString(rt))
        snprintf(s_cred.refresh_token, sizeof(s_cred.refresh_token), "%s", rt->valuestring);
    if (cJSON_IsString(vid))
        snprintf(s_cred.vid, sizeof(s_cred.vid), "%s", vid->valuestring);
    else if (cJSON_IsNumber(vid))
        snprintf(s_cred.vid, sizeof(s_cred.vid), "%lld", (long long)vid->valuedouble);
    s_cred.obtained_ms = esp_timer_get_time() / 1000;
    cJSON_Delete(j);
    cred_save();
    ESP_LOGI(TAG, "refresh: new accessToken saved vid='%s' at_len=%u",
             s_cred.vid, (unsigned)strlen(s_cred.access_token));
    return true;
}

bool wr_consent_upload(const char *book_id, const char *owner, uint32_t seconds,
                       const char *chapter_id, uint32_t chapter_uid, uint32_t offset,
                       bool resend, uint32_t chapter_idx, uint32_t pct) {
    if (!wr_consent_authorized() || !book_id || !seconds) return false;

    /* 时间戳必须是真实 epoch 毫秒（13 位）。开机相对毫秒会被服务端反作弊静默丢弃，
       所以真实时间未同步（<2024-01-01）时本次先不报，等下一轮时间同步后再补。 */
    time_t now_epoch = time(NULL);
    if ((uint32_t)now_epoch < 1704067200u) {
        ESP_LOGW(TAG, "upload skipped: wall clock not synced now=%lld",
                 (long long)now_epoch);
        return false;
    }

    bool ok = false;
    bool refreshed = false;
    for (int attempt = 0; attempt < 2; ++attempt) {
        /* epoch 秒*1000 + 随机毫秒，与 weread_protocol 实时上报同一口径。 */
        int64_t ts = (int64_t)now_epoch * 1000 + (int64_t)(esp_random() % 1000);
        unsigned rnd = esp_random() % 1000;
        char sign_input[256], sign[65];
        snprintf(sign_input, sizeof(sign_input), "%lld%s%u", (long long)ts, s_cred.device_id, rnd);
        sha256_hex(sign_input, sign);

        cJSON *b = cJSON_CreateObject();
        cJSON_AddStringToObject(b, "bookId", book_id);
        cJSON_AddStringToObject(b, "deviceId", s_cred.device_id);
        cJSON_AddStringToObject(b, "installId", s_cred.install_id);
        cJSON_AddNumberToObject(b, "chapterUid", chapter_uid);
        cJSON_AddNumberToObject(b, "chapterIdx", (double)chapter_idx);
        cJSON_AddNumberToObject(b, "chapterOffset", offset);
        cJSON_AddNumberToObject(b, "readingTime", seconds);
        /* 自动累计时长与本批秒数一致；进度用设备真实百分比，避免缺字段被反作弊判为异常客户端。 */
        cJSON_AddNumberToObject(b, "autoTime", seconds);
        cJSON_AddNumberToObject(b, "progress", (double)pct);
        cJSON_AddNumberToObject(b, "currentProgress", (double)pct);
        /* 正常阅读无风险标记。 */
        cJSON_AddNumberToObject(b, "risk", 0);
        /* 实时阅读=0；离线/断网/重启后的补传=1。恒为 1 会被服务端补传去重，回 succ 却不计时。 */
        cJSON_AddNumberToObject(b, "isResendReadingInfo", resend ? 1 : 0);
        cJSON_AddNumberToObject(b, "timestamp", (double)ts);
        cJSON_AddNumberToObject(b, "random", rnd);
        cJSON_AddStringToObject(b, "signature", sign);
        cJSON_AddStringToObject(b, "recordCreateTimeZone", "UTC+8");
        cJSON_AddNumberToObject(b, "deviceType", 3);
        cJSON_AddNumberToObject(b, "finish", 0);
        char *body = cJSON_PrintUnformatted(b);
        cJSON_Delete(b);
        ESP_LOGI(TAG, "upload request attempt=%d: %s", attempt, body ? body : "(null)");

        http_header_t headers[2];
        snprintf(headers[0].key, sizeof(headers[0].key), "vid");
        snprintf(headers[0].value, sizeof(headers[0].value), "%s", s_cred.vid);
        snprintf(headers[1].key, sizeof(headers[1].key), "accessToken");
        snprintf(headers[1].value, sizeof(headers[1].value), "%s", s_cred.access_token);

        size_t len = 0;
        char *resp = http_call(MOBILE_BASE "/book/read", body, headers, 2, &len, 20000);
        free(body);
        int resp_code = 0;
        bool have_code = false;
        ESP_LOGI(TAG, "upload HTTP resp_len=%u body: %.*s", (unsigned)len,
                 (int)(len > 300 ? 300 : len), resp ? resp : "(null)");
        if (resp) {
            cJSON *j = cJSON_Parse(resp);
            if (j) {
                const cJSON *err = cJSON_GetObjectItem(j, "errCode");
                const cJSON *ec = cJSON_GetObjectItem(j, "errcode");
                const cJSON *e = err ? err : ec;
                if (!e) {
                    ok = true;
                } else {
                    have_code = true;
                    resp_code = e->valueint;
                    ok = (resp_code == 0);
                }
                cJSON_Delete(j);
            } else {
                ESP_LOGW(TAG, "upload response is not JSON");
            }
            free(resp);
        }
        ESP_LOGI(TAG, "upload book=%s seconds=%u attempt=%d code=%d ok=%d",
                 book_id, seconds, attempt, have_code ? resp_code : 0, ok);
        if (ok) break;
        /* 登录超时（-2012）：用 refreshToken 刷新一次，拿到新 accessToken 后重试，仅一次。 */
        if (have_code && !refreshed && resp_code == -2012) {
            refreshed = true;
            ESP_LOGW(TAG, "token expired code=%d, refreshing then retrying", resp_code);
            if (!consent_refresh()) break;
            continue;
        }
        break;
    }
    return ok;
}

/* App 协议（i.weread.qq.com）认证请求：body 非 NULL 走 POST，NULL 走 GET。
 * 返回 PSRAM 响应体（调用方 free），*out_len 为长度；失败返回 NULL。
 * 弱网加固：网络级最多重试 2 次（NULL/空/非 JSON 都算失败），命中 -2012 另用 refreshToken
 * 刷新一次再重试。/ Authenticated App-protocol call: POST with a body, GET without. Retries up
 * to two times on a weak link and refreshes once on -2012; returns a PSRAM response. */
static char *app_call_impl(const char *path, const char *body, size_t *out_len,
                           unsigned attempts, int timeout_ms);

char *wr_consent_app_call(const char *path, const char *body, size_t *out_len) {
    return app_call_impl(path, body, out_len, 2, 20000);
}

/* 快速版：只试一次、超时更短。段评这种"没有就算了"的可选请求用它，别让一次 20 秒 ×2
 * 的超时把阅读器卡住。/ Quick variant: one attempt with a short timeout, for optional reads. */
char *wr_consent_app_call_quick(const char *path, const char *body, size_t *out_len) {
    return app_call_impl(path, body, out_len, 1, 12000);
}

static char *app_call_impl(const char *path, const char *body, size_t *out_len,
                           unsigned attempts, int timeout_ms) {
    if (out_len) *out_len = 0;
    if (!wr_consent_authorized() || !path || path[0] != '/') return NULL;
    char url[160];
    int n = snprintf(url, sizeof(url), "%s%s", MOBILE_BASE, path);
    if (n < 0 || n >= (int)sizeof(url)) return NULL;

    http_header_t headers[2];
    snprintf(headers[0].key, sizeof(headers[0].key), "vid");
    snprintf(headers[1].key, sizeof(headers[1].key), "accessToken");

    char *result = NULL;
    bool refreshed = false;
    for (unsigned attempt = 0; attempt < attempts; ++attempt) {
        snprintf(headers[0].value, sizeof(headers[0].value), "%s", s_cred.vid);
        snprintf(headers[1].value, sizeof(headers[1].value), "%s", s_cred.access_token);

        size_t len = 0;
        result = app_keepalive_perform(url, body, headers, 2, &len, timeout_ms);
        if (!result || !len) {
            ESP_LOGW(TAG, "app_call attempt=%u no response path=%s", attempt, path);
        } else {
            cJSON *j = cJSON_Parse(result);
            if (!j) {
                ESP_LOGW(TAG, "app_call attempt=%u non-JSON len=%u head: %.*s",
                         attempt, (unsigned)len, (int)(len > 120 ? 120 : len), result);
            } else {
                const cJSON *err = cJSON_GetObjectItem(j, "errCode");
                const cJSON *ec = cJSON_GetObjectItem(j, "errcode");
                const cJSON *e = err ? err : ec;
                /* 登录超时（-2012）：刷新一次拿新 accessToken，旧连接作废重连，立刻重试。 */
                if (cJSON_IsNumber(e) && e->valueint == -2012 && !refreshed) {
                    refreshed = true;
                    cJSON_Delete(j);
                    free(result);
                    ESP_LOGW(TAG, "app_call token expired, refreshing then retrying");
                    if (consent_refresh()) {
                        app_keepalive_drop();
                        continue;
                    }
                    return NULL;
                }
                cJSON_Delete(j);
                if (out_len) *out_len = len;
                return result;   /* 合法 JSON 回包：成功（含业务 errCode，交给调用方判断）。 */
            }
        }
        if (result) { free(result); result = NULL; }
        if (attempt + 1 < attempts) vTaskDelay(pdMS_TO_TICKS(500));
    }

    /* 长连接上发不出去（实测 readreviews 的 POST 会瞬间失败、连错误码都拿不到）时，
     * 丢掉长连接，用一次性连接把同一个请求再发一遍。/ If the warm connection cannot carry
     * the request, drop it and retry once on a one-shot connection. */
    app_keepalive_drop();
    snprintf(headers[0].value, sizeof(headers[0].value), "%s", s_cred.vid);
    snprintf(headers[1].value, sizeof(headers[1].value), "%s", s_cred.access_token);
    size_t fb_len = 0;
    result = http_call(url, body, headers, 2, &fb_len, timeout_ms);
    if (result && fb_len) {
        ESP_LOGI(TAG, "app_call cold retry ok path=%s len=%u", path, (unsigned)fb_len);
        if (out_len) *out_len = fb_len;
        return result;
    }
    if (result) free(result);
    return NULL;
}

void wr_consent_init(void) {
    if (!s_lock) s_lock = xSemaphoreCreateMutex();
    if (!s_ka_lock) {
        s_ka_lock = xSemaphoreCreateMutex();
        if (s_ka_lock) {
            /* 断连/关 WiFi 即清理长连接，避免拖到离线后 pbuf 回收回调变成野指针。
             * Drop the warm client on link loss/stop so its RX pbufs are freed while the netif lives. */
            esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED,
                                       app_keepalive_link_event, NULL);
            esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_STOP,
                                       app_keepalive_link_event, NULL);
        }
    }
    ESP_LOGI(TAG, "wr_consent_init: restoring credentials");
    cred_restore();
}

/* ---------- 主动后台续期 ---------- */
/* 后台刷新任务是否在跑：防止多个 tick 重复创建任务。
 * Whether the one-shot refresh worker is currently running (re-entry guard). */
static volatile bool s_refresh_running;

static void refresh_worker(void *arg) {
    (void)arg;
    /* consent_refresh 内部完成 /login 请求、解析新凭证并持久化。它与 consent_task 不会同时存在
     * （wr_consent_ensure 已检查 s_task == NULL）。 */
    bool ok = consent_refresh();
    ESP_LOGI(TAG, "ensure refresh result=%d", (int)ok);
    s_refresh_running = false;
    vTaskDelete(NULL);
}

void wr_consent_ensure(void) {
    if (s_refresh_running) return;        /* 已在刷新 */
    if (s_task != NULL) return;           /* 完整授权流程在跑，不抢 */
    if (wr_status(NULL, 0, NULL, NULL) == WR_BUSY) return; /* worker 正在上报，避免并发改凭证 */
    if (!wr_online()) return;             /* 没网，等联网后下一个 tick 再试 */
    if (!wr_consent_authorized()) return; /* 没有凭证，需先扫码，这里不自动发起 */
    int64_t now = esp_timer_get_time() / 1000;
    /* 之前是 60 秒就刷一次，等于每分钟都打一次 /login、每轮都往存储卡写一份凭证，
     * 既无谓耗电磨损，也让 accessToken 一直在变。改成 30 分钟一次；真的过期了
     * 上传会拿到 -2012，那时再刷新（consent_task 内已有这条路径），不影响补传。
     * Was refreshing every 60 s: one /login plus a credential write to SD every minute,
     * pointless churn that also kept rotating the token. Now 30 minutes; a genuinely expired
     * token still surfaces as -2012 on upload and is refreshed there instead. */
    if (now - s_cred.obtained_ms < 1800000) return;
    s_refresh_running = true;
    if (xTaskCreate(refresh_worker, "wr_refresh", 8192, NULL, 5, NULL) != pdPASS) {
        s_refresh_running = false;
        ESP_LOGW(TAG, "ensure: create refresh task failed");
    }
}
