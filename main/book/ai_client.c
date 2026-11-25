/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 */
#include "ai_client.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <errno.h>

#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "read_pico_transfer.h"
#include "weread.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define AI_CONFIG_FILE "/sdcard/.weread/ai.json"
#define PSRAM_CAPS (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)

static const char *TAG = "ai_client";

/* 最近一次失败的可读原因。/ Human-readable reason for the most recent failure.
 * 200 字节足以容纳 ai_chat 内 detail[192] 拼出的"HTTP N：服务端消息"，避免截断告警。 */
static char s_last_error[200];

const char *ai_last_error(void) { return s_last_error[0] ? s_last_error : NULL; }

static void ai_set_error(const char *msg) {
    if (!msg) { s_last_error[0] = 0; return; }
    snprintf(s_last_error, sizeof(s_last_error), "%s", msg);
}

typedef struct {
    ai_preset_t preset;
    const char *base_url;
    const char *model;
} ai_preset_def_t;

static const ai_preset_def_t s_presets[] = {
    {AI_PRESET_CUSTOM,   "", ""},
    {AI_PRESET_DOUBAO,   "https://ark.cn-beijing.volces.com/api/v3",
                         "doubao-seed-1-6-flash-250828"},
    {AI_PRESET_DEEPSEEK, "https://api.deepseek.com/v1",
                         "deepseek-chat"},
    {AI_PRESET_QWEN,     "https://dashscope.aliyuncs.com/compatible-mode/v1",
                         "qwen-turbo"},
};

void ai_config_defaults(ai_config_t *cfg) {
    if (!cfg) return;
    memset(cfg, 0, sizeof(*cfg));
    cfg->preset = AI_PRESET_DEEPSEEK;
    /* temperature_x10 是"温度×10"：默认 0.2 存 2（低温度、答案稳定，归纳场景够用）。
     * 发送前仍会夹到 0–2，避免越界被 400 拒绝。 */
    cfg->temperature_x10 = 2;
    cfg->max_tokens = 1024;
    snprintf(cfg->system_prompt, sizeof(cfg->system_prompt),
             "你是一名严谨的中文阅读助手，回答简洁准确，直接给结论。");
    ai_config_apply_preset(cfg, AI_PRESET_DEEPSEEK);
}

void ai_config_apply_preset(ai_config_t *cfg, ai_preset_t preset) {
    if (!cfg || preset >= sizeof(s_presets) / sizeof(s_presets[0])) return;
    const ai_preset_def_t *p = &s_presets[preset];
    cfg->preset = (uint8_t)preset;
    if (p->base_url[0]) {
        snprintf(cfg->base_url, sizeof(cfg->base_url), "%s", p->base_url);
        snprintf(cfg->model, sizeof(cfg->model), "%s", p->model);
    }
}

bool ai_config_save(const ai_config_t *cfg) {
    if (!cfg) return false;
    cJSON *root = cJSON_CreateObject();
    if (!root) return false;
    cJSON_AddNumberToObject(root, "preset", cfg->preset);
    cJSON_AddStringToObject(root, "base_url", cfg->base_url);
    cJSON_AddStringToObject(root, "api_key", cfg->api_key);
    cJSON_AddStringToObject(root, "model", cfg->model);
    cJSON_AddNumberToObject(root, "temperature_x10", cfg->temperature_x10);
    cJSON_AddNumberToObject(root, "max_tokens", cfg->max_tokens);
    cJSON_AddStringToObject(root, "system_prompt", cfg->system_prompt);
    cJSON_AddNumberToObject(root, "answer_mode", cfg->answer_mode);
    char *printed = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!printed) return false;
    bool ok = false;
    FILE *f = fopen(AI_CONFIG_FILE, "wb");
    if (f) {
        ok = fwrite(printed, 1, strlen(printed), f) == strlen(printed);
        fclose(f);
    }
    free(printed);
    return ok;
}

bool ai_config_load(ai_config_t *cfg) {
    if (!cfg) return false;
    ai_config_defaults(cfg);
    FILE *f = fopen(AI_CONFIG_FILE, "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    if (n <= 0 || n > 8192) { fclose(f); return false; }
    fseek(f, 0, SEEK_SET);
    char *buf = heap_caps_malloc((size_t)n + 1, PSRAM_CAPS);
    if (!buf) { fclose(f); return false; }
    size_t got = fread(buf, 1, (size_t)n, f);
    fclose(f);
    buf[got] = 0;
    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) return false;
    cJSON *j;
    if ((j = cJSON_GetObjectItem(root, "preset")) && cJSON_IsNumber(j))
        cfg->preset = (uint8_t)j->valuedouble;
    if ((j = cJSON_GetObjectItem(root, "base_url")) && cJSON_IsString(j))
        snprintf(cfg->base_url, sizeof(cfg->base_url), "%s", j->valuestring);
    if ((j = cJSON_GetObjectItem(root, "api_key")) && cJSON_IsString(j))
        snprintf(cfg->api_key, sizeof(cfg->api_key), "%s", j->valuestring);
    if ((j = cJSON_GetObjectItem(root, "model")) && cJSON_IsString(j))
        snprintf(cfg->model, sizeof(cfg->model), "%s", j->valuestring);
    if ((j = cJSON_GetObjectItem(root, "temperature_x10")) && cJSON_IsNumber(j))
        cfg->temperature_x10 = (int)j->valuedouble;
    if ((j = cJSON_GetObjectItem(root, "max_tokens")) && cJSON_IsNumber(j))
        cfg->max_tokens = (int)j->valuedouble;
    if ((j = cJSON_GetObjectItem(root, "system_prompt")) && cJSON_IsString(j))
        snprintf(cfg->system_prompt, sizeof(cfg->system_prompt), "%s", j->valuestring);
    if ((j = cJSON_GetObjectItem(root, "answer_mode")) && cJSON_IsNumber(j))
        cfg->answer_mode = (uint8_t)j->valuedouble;
    cJSON_Delete(root);
    return true;
}

/* 确保 STA 联网；已连直接返回，否则用已保存网络临时拉起。 */
static esp_err_t ai_ensure_network(bool *owned) {
    read_pico_transfer_status_t status;
    read_pico_transfer_get_status(&status);
    if (status.mode == READ_PICO_TRANSFER_MODE_STA && status.network_ready &&
        status.state != READ_PICO_TRANSFER_UPLOADING) return ESP_OK;
    if (status.state != READ_PICO_TRANSFER_STOPPED) return ESP_ERR_INVALID_STATE;
    char name[33];
    bool saved = false;
    if (read_pico_transfer_get_saved_wifi(name, &saved) != ESP_OK || !saved)
        return ESP_ERR_NOT_FOUND;
    read_pico_transfer_cfg_t cfg = {
        .mode = READ_PICO_TRANSFER_MODE_STA, .network_only = true};
    esp_err_t err = read_pico_transfer_start(&cfg);
    if (err != ESP_OK) return err;
    *owned = true;
    int64_t limit = esp_timer_get_time() + 30000000;
    while (esp_timer_get_time() < limit) {
        read_pico_transfer_service_poll();
        read_pico_transfer_get_status(&status);
        if (status.network_ready) return ESP_OK;
        if (status.state == READ_PICO_TRANSFER_ERROR) break;
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    return ESP_ERR_TIMEOUT;
}

/* 等待后台 weread 任务（含离线补传/consent 上传）空闲，避免与 AI 请求并发争抢内部 RAM，
   导致 TLS 握手分配失败（-0x4C）。有界等待，超时则照常尝试，不无限阻塞 UI。 */
static void ai_wait_worker_idle(void) {
    int64_t deadline = esp_timer_get_time() + 20000000;
    while (esp_timer_get_time() < deadline) {
        if (wr_status(NULL, 0, NULL, NULL) != WR_BUSY) return;
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

/* 单次 HTTP 尝试在后面定义，这里前置声明，避免隐式声明。 */
static bool ai_chat_http_once(const ai_config_t *cfg, const char *url,
                              const char *body, char **answer);

/* 把任意字节串清洗成合法 UTF-8：丢弃非法/残缺/越界/代理码点与超长编码，输出绝不长于输入。
   修复选中文本按字节截断或源文本坏字节导致服务端 400 “invalid unicode code point”。
   返回 PSRAM 新分配的 NUL 结尾字符串，调用者 free；分配失败返回 NULL。 */
static char *ai_sanitize_utf8(const char *src) {
    if (!src) return NULL;
    size_t len = strlen(src);
    char *out = heap_caps_malloc(len + 1, PSRAM_CAPS);
    if (!out) return NULL;
    const unsigned char *p = (const unsigned char *)src;
    const unsigned char *end = p + len;
    size_t w = 0;
    while (p < end) {
        unsigned char b0 = *p;
        if (b0 < 0x80) {                       /* ASCII 原样保留（控制字符交给 cJSON 转义） */
            out[w++] = (char)b0;
            ++p;
            continue;
        }
        size_t need;
        uint32_t cp;
        if (b0 >= 0xF0)      { need = 4; cp = b0 & 0x07; }
        else if (b0 >= 0xE0) { need = 3; cp = b0 & 0x0F; }
        else if (b0 >= 0xC2) { need = 2; cp = b0 & 0x1F; }
        else { ++p; continue; }                /* 0x80–0xBF 孤立续接、0xC0/0xC1 超长，丢弃 */
        if ((size_t)(end - p) < need) break;   /* 末尾残缺序列，丢弃 */
        bool ok = true;
        for (size_t k = 1; k < need; ++k) {
            unsigned char bc = p[k];
            if (bc < 0x80 || bc > 0xBF) { ok = false; break; }
            cp = (cp << 6) | (bc & 0x3F);
        }
        if (!ok) { ++p; continue; }
        if ((cp >= 0xD800 && cp <= 0xDFFF) || cp > 0x10FFFF) { p += need; continue; }
        if ((need == 2 && cp < 0x80) || (need == 3 && cp < 0x800) ||
            (need == 4 && cp < 0x10000)) { p += need; continue; }
        memcpy(out + w, p, need);
        w += need;
        p += need;
    }
    out[w] = '\0';
    return out;
}

bool ai_chat_limit(const ai_config_t *cfg, const char *system, const char *user,
                   char **answer, int max_tokens_override) {
    if (answer) *answer = NULL;
    if (!cfg || !cfg->base_url[0] || !cfg->model[0] || !user || !answer) return false;

    /* 发送前清洗为合法 UTF-8，避免坏字节/截断被服务端 400 拒绝；分配失败则回退原文。 */
    char *safe_system = NULL, *safe_user = NULL;
    const char *sys_in = system, *user_in = user;
    if (system && system[0]) {
        safe_system = ai_sanitize_utf8(system);
        if (safe_system) sys_in = safe_system;
    }
    safe_user = ai_sanitize_utf8(user);
    if (safe_user) user_in = safe_user;

    cJSON *root = cJSON_CreateObject();
    cJSON *msgs = cJSON_AddArrayToObject(root, "messages");
    if (system && system[0]) {
        cJSON *sm = cJSON_CreateObject();
        cJSON_AddStringToObject(sm, "role", "system");
        cJSON_AddStringToObject(sm, "content", sys_in);
        cJSON_AddItemToArray(msgs, sm);
    }
    cJSON *um = cJSON_CreateObject();
    cJSON_AddStringToObject(um, "role", "user");
    cJSON_AddStringToObject(um, "content", user_in);
    cJSON_AddItemToArray(msgs, um);
    cJSON_AddStringToObject(root, "model", cfg->model);
    /* 发送前把温度夹到各厂商通用的 0–2：旧配置里的 70（=7.0）会被 DeepSeek 400 拒绝。
     * Clamp temperature to the universally accepted 0–2 range before sending; a legacy 70
     * (7.0) otherwise makes DeepSeek reject with HTTP 400. */
    int temp_x10 = cfg->temperature_x10;
    if (temp_x10 < 0) temp_x10 = 0;
    if (temp_x10 > 20) temp_x10 = 20;
    cJSON_AddNumberToObject(root, "temperature", temp_x10 / 10.0);
    /* 归纳等场景可传入更小的 token 上限，缩短生成时间；不传（<=0）用配置里的全局值。 */
    int req_max_tokens = max_tokens_override > 0 ? max_tokens_override : cfg->max_tokens;
    if (req_max_tokens < 1) req_max_tokens = 1;
    cJSON_AddNumberToObject(root, "max_tokens", req_max_tokens);
    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    free(safe_system);
    free(safe_user);
    if (!body) return false;

    char url[AI_URL_MAX + 24];
    size_t blen = strlen(cfg->base_url);
    snprintf(url, sizeof(url), "%s%s/chat/completions", cfg->base_url,
             blen && cfg->base_url[blen - 1] == '/' ? "" : "");

    bool owned = false, ok = false;
    esp_err_t net = ai_ensure_network(&owned);
    if (net != ESP_OK) {
        ESP_LOGW(TAG, "network unavailable: %s", esp_err_to_name(net));
        goto out;
    }

    /* 后台补传/上传在跑时先等它结束，避免并发把内部 RAM 挤爆。 */
    ai_wait_worker_idle();

    /* 弱网冷连接偶发传输失败（日志里的 http status -1）：短延迟后自动重试一次，
       无需用户手动再点；两次都失败才把最后一次原因留在错误里。 */
    for (int attempt = 0; attempt < 2; ++attempt) {
        if (attempt > 0) vTaskDelay(pdMS_TO_TICKS(500));
        ok = ai_chat_http_once(cfg, url, body, answer);
        if (ok) break;
        if (attempt == 0)
            ESP_LOGW(TAG, "request failed, retrying once: %s",
                     ai_last_error() ? ai_last_error() : "");
    }
out:
    free(body);
    if (ok) ai_set_error(NULL);
    if (owned) (void)read_pico_transfer_stop();
    return ok;
}

/* 单次 HTTP 尝试：成功时把答案写入 *answer（PSRAM），返回 true。
   自带 client/resp 清理，失败时通过 ai_set_error 留可读原因。 */
static bool ai_chat_http_once(const ai_config_t *cfg, const char *url,
                              const char *body, char **answer) {
    esp_http_client_handle_t client = NULL;
    char *resp = NULL;
    bool ok = false;

    esp_http_client_config_t hcfg = {
        .url = url,
        .method = HTTP_METHOD_POST,
        /* 小 token 上限下健康响应通常 3–10s；30s 足够覆盖弱网，超时也更快暴露失败。 */
        .timeout_ms = 30000,
        .buffer_size = 2048,
        .buffer_size_tx = 2048,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    client = esp_http_client_init(&hcfg);
    if (!client) goto done;
    char auth[AI_KEY_MAX + 8];
    snprintf(auth, sizeof(auth), "Bearer %s", cfg->api_key);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "Authorization", auth);
    int post_len = (int)strlen(body);
    if (esp_http_client_open(client, post_len) != ESP_OK) goto done;
    int written = esp_http_client_write(client, body, post_len);
    if (written < 0 || written != post_len) goto done;
    (void)esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    if (status != 200) {
        /* 读错误体并提取服务端消息，便于定位（如温度越界、模型名无效、密钥错误）。
         * Read the error body and extract the server message for diagnosis. */
        size_t ecap = 2048, en = 0;
        char *ebody = heap_caps_malloc(ecap, PSRAM_CAPS);
        if (ebody) {
            while (en < ecap - 1) {
                int er = esp_http_client_read(client, ebody + en, ecap - en - 1);
                if (er < 0) break;
                if (er == 0) {
                    if (esp_http_client_is_complete_data_received(client)) break;
                    vTaskDelay(pdMS_TO_TICKS(10));
                    continue;
                }
                en += (size_t)er;
            }
            ebody[en] = 0;
            ESP_LOGW(TAG, "http status %d body=%s", status, ebody);
            cJSON *ej = cJSON_Parse(ebody);
            if (ej) {
                cJSON *err = cJSON_GetObjectItem(ej, "error");
                cJSON *msg = err ? cJSON_GetObjectItem(err, "message") : NULL;
                if (msg && cJSON_IsString(msg) && msg->valuestring[0]) {
                    char detail[192];
                    snprintf(detail, sizeof(detail), "HTTP %d：%s", status, msg->valuestring);
                    ai_set_error(detail);
                }
                cJSON_Delete(ej);
            }
            free(ebody);
        }
        if (!ai_last_error()) {
            char detail[48];
            snprintf(detail, sizeof(detail), "HTTP %d：服务拒绝了请求", status);
            ai_set_error(detail);
        }
        goto done;
    }

    size_t cap = 8192, n = 0;
    resp = heap_caps_malloc(cap, PSRAM_CAPS);
    if (!resp) goto done;
    while (n < AI_ANSWER_MAX) {
        if (n + 2048 >= cap) {
            size_t grown_cap = cap + 4096;
            char *grown = heap_caps_realloc(resp, grown_cap, PSRAM_CAPS);
            if (!grown) break;
            resp = grown; cap = grown_cap;
        }
        int r = esp_http_client_read(client, resp + n, cap - n - 1);
        if (r < 0) break;
        if (r == 0) {
            if (esp_http_client_is_complete_data_received(client)) break;
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        n += (size_t)r;
    }
    resp[n] = 0;

    cJSON *jr = cJSON_Parse(resp);
    if (jr) {
        cJSON *choices = cJSON_GetObjectItem(jr, "choices");
        cJSON *first = choices && cJSON_IsArray(choices)
                           ? cJSON_GetArrayItem(choices, 0) : NULL;
        cJSON *msg = first ? cJSON_GetObjectItem(first, "message") : NULL;
        cJSON *content = msg ? cJSON_GetObjectItem(msg, "content") : NULL;
        if (content && cJSON_IsString(content) && content->valuestring[0]) {
            size_t clen = strlen(content->valuestring) + 1;
            char *dup = heap_caps_malloc(clen, PSRAM_CAPS);
            if (dup) {
                memcpy(dup, content->valuestring, clen);
                *answer = dup; ok = true;
            }
        }
        cJSON_Delete(jr);
    }
    if (!ok && !ai_last_error())
        ai_set_error("网络连接失败或响应无法解析，请检查 Wi-Fi 与接口地址");
done:
    if (client) esp_http_client_cleanup(client);
    free(resp);
    return ok;
}

bool ai_chat(const ai_config_t *cfg, const char *system, const char *user, char **answer) {
    /* 不覆盖 token 上限：沿用配置里的全局 max_tokens。 */
    return ai_chat_limit(cfg, system, user, answer, 0);
}

bool ai_ask(const ai_config_t *cfg, ai_action_t action,
            const char *book_title, const char *chapter_title,
            const char *selection, const char *question, char **answer) {
    const char *bt = book_title && book_title[0] ? book_title : "当前书籍";
    const char *ct = chapter_title && chapter_title[0] ? chapter_title : "当前章节";
    char user[4096];
    user[0] = 0;
    switch (action) {
    case AI_SUMMARY:
        snprintf(user, sizeof(user),
                 "请总结《%s》中“%s”的核心内容，分点列出，不超过 200 字。\n\n正文：\n%s",
                 bt, ct, selection && selection[0] ? selection : "");
        break;
    case AI_EXPLAIN:
        snprintf(user, sizeof(user),
                 "背景：《%s》“%s”。请解释下面这段文字的含义、涉及的概念或人物，通俗准确：\n\n%s",
                 bt, ct, selection && selection[0] ? selection : "");
        break;
    case AI_TRANSLATE:
        snprintf(user, sizeof(user),
                 "请把下面的文字翻译成通顺的中文（若已是中文则译为英文），只给译文：\n\n%s",
                 selection && selection[0] ? selection : "");
        break;
    case AI_POLISH:
        snprintf(user, sizeof(user),
                 "请润色下面的文字，保持原意、修正语病，先给润色结果再简述改动：\n\n%s",
                 selection && selection[0] ? selection : "");
        break;
    default:
        snprintf(user, sizeof(user),
                 "背景：我正在读《%s》的“%s”。%s%s请回答：%s",
                 bt, ct,
                 selection && selection[0] ? "相关原文：\n" : "",
                 selection && selection[0] ? selection : "",
                 question && question[0] ? question : "");
        break;
    }
    const char *sys = cfg->system_prompt[0] ? cfg->system_prompt : NULL;
    return ai_chat(cfg, sys, user, answer);
}
