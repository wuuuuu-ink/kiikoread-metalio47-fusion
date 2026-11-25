/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：诊断日志实现。挂载 ESP_LOG -> PSRAM 环形缓冲（剥离 ANSI 颜色码）-> 后台任务落盘，
 * 双文件轮转、跨重启保留。导出时先 flush 再从串口分块打印。
 *
 * English: Diagnostic log implementation. Hooks ESP_LOG into a PSRAM ring buffer (ANSI color
 * codes stripped), flushed to a rotating pair of files by a background task and kept across
 * reboots. Export flushes first then prints over the serial port.
 */
#include "diag_log.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/stat.h>
#include <time.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

static const char *TAG = "diag";

#define DIAG_RING_CAP   (64 * 1024)
#define DIAG_LINE_MAX   512
#define DIAG_FILE_MAX   (192 * 1024)
#define DIAG_DIR        "/sdcard/diag"
#define DIAG_FILE       DIAG_DIR "/diag.log"
#define DIAG_PREV       DIAG_DIR "/diag.log.prev"

/*
 * 日志不能按"谁都能写"来收：充电器这类每 2 秒一条的循环会把整个文件在几十秒内写满，
 * 事后拿到的日志里只剩它自己，要查的微信读书/网络/蓝牙记录一条都不剩。
 * 这里按 tag 做限流：每分钟每个 tag 最多 12 行，超出的计数丢弃，下一分钟开头补一行
 * "[省略 N 行]"。错误和警告给更高额度（40 行/分钟），保证出错不会被饿死。
 * 真正关心的 tag（微读三件套）完全不限流。
 * Logging cannot be a free-for-all: a 2-second charger loop fills the whole file in under a
 * minute and leaves nothing but its own chatter, losing every weread/network/Bluetooth record.
 * So each tag gets a per-minute quota (12 lines; errors and warnings 40), overflow is counted
 * and summarised as "[omitted N lines]" when the next window opens. The tags we actually
 * diagnose are exempt.
 */
#define DIAG_TAG_MAX          24
#define DIAG_TAG_QUOTA_NORM   6
#define DIAG_TAG_QUOTA_ALERT  24
#define DIAG_TAG_WINDOW_MS    60000

typedef struct {
    char name[24];
    int64_t window_start_ms;
    unsigned lines;
    unsigned dropped;
} diag_tag_bucket_t;

/* 限流桶表放 PSRAM（960B），避免占用 WiFi 固定内部 DMA 所需的内部 RAM。
 * The quota bucket table lives in PSRAM (960B) so it does not steal internal RAM needed by the
 * Wi-Fi fixed internal DMA buffers. Lazily allocated; NULL disables filtering (fail open). */
static diag_tag_bucket_t *s_buckets;
static unsigned s_bucket_count;

static char *s_ring;
static SemaphoreHandle_t s_mutex;
static vprintf_like_t s_original;

/* ring 记账：s_ring_seq 是累计写入字节数（单调递增），s_ring_len 是当前有效字节数。
 * Ring accounting: s_ring_seq is the monotonic total written; s_ring_len is valid bytes. */
static uint64_t s_ring_seq;
static size_t s_ring_len;
/* 已落盘到的逻辑字节序号。/ Logical byte sequence already flushed to file. */
static uint64_t s_disk_seq;

static FILE *s_file;
static size_t s_file_size;
static bool s_dumping;
/* USB 读卡接管 TF 卡期间挂起：先排空并关闭文件句柄，期间只在 ring 累积，恢复后再落盘。
   Suspended while USB MSC owns the SD: drain and close the file, keep logs in the ring, reopen on resume. */
static bool s_suspended;

static void ring_byte(char c) {
    s_ring[s_ring_seq % DIAG_RING_CAP] = c;
    s_ring_seq++;
    if (s_ring_len < DIAG_RING_CAP) s_ring_len++;
}

/* 逐字符写入，跳过 ANSI 转义序列（ESP_LOG 颜色码）。/ Write bytes, skipping ANSI escapes. */
static void push_stripped(const char *src, size_t n) {
    enum { ST_NORMAL, ST_ESC, ST_CSI } state = ST_NORMAL;
    for (size_t i = 0; i < n; ++i) {
        unsigned char c = (unsigned char)src[i];
        switch (state) {
        case ST_NORMAL:
            if (c == 0x1b) state = ST_ESC;
            else ring_byte((char)c);
            break;
        case ST_ESC:
            state = (c == '[') ? ST_CSI : ST_NORMAL;
            break;
        case ST_CSI:
            /* CSI 序列以 0x40–0x7E 结束（'m' = 0x6D）。/ CSI ends with a byte 0x40-0x7E. */
            if (c >= 0x40 && c <= 0x7e) state = ST_NORMAL;
            break;
        }
    }
}

/* 从一行 ESP_LOG 文本取出 tag 与级别：格式为 "I (12345) tag: message"，可能带 ANSI 颜色码。
 * Pull the tag and level out of an ESP_LOG line ("I (12345) tag: message"), ANSI codes tolerated. */
static bool parse_tag_level(const char *line, size_t n, char *tag, size_t tag_cap, char *level) {
    char clean[192];
    size_t c = 0;
    enum { PS_NORMAL, PS_ESC, PS_CSI } st = PS_NORMAL;
    for (size_t i = 0; i < n && c + 1 < sizeof(clean); ++i) {
        unsigned char ch = (unsigned char)line[i];
        if (st == PS_NORMAL) {
            if (ch == 0x1b) st = PS_ESC;
            else clean[c++] = (char)ch;
        } else if (st == PS_ESC) {
            st = (ch == '[') ? PS_CSI : PS_NORMAL;
        } else {
            if (ch >= 0x40 && ch <= 0x7e) st = PS_NORMAL;
        }
    }
    clean[c] = 0;

    size_t i = 0;
    while (i < c && (clean[i] == ' ' || clean[i] == '\n' || clean[i] == '\r')) ++i;
    if (i >= c) return false;
    *level = clean[i];
    while (i < c && clean[i] != ')') ++i;   /* 跳过 "I (12345)" */
    if (i >= c) return false;
    ++i;
    while (i < c && clean[i] == ' ') ++i;
    const size_t t0 = i;
    while (i < c && clean[i] != ':') ++i;
    if (i >= c || i == t0) return false;
    size_t len = i - t0;
    if (len >= tag_cap) len = tag_cap - 1;
    memcpy(tag, clean + t0, len);
    tag[len] = 0;
    return true;
}

/* 诊断真正需要的 tag，一律不限额。/ Tags that matter for diagnosis bypass the quota. */
static bool tag_exempt(const char *tag) {
    static const char *const exempt[] = {"WEREAD", "WRconsent", "WRCACHE", "diag"};
    for (size_t i = 0; i < sizeof(exempt) / sizeof(exempt[0]); ++i)
        if (strcmp(tag, exempt[i]) == 0) return true;
    return false;
}

/* 调用方必须已持有 s_mutex。返回 false 表示这行被限流丢掉。
 * Caller must hold s_mutex. Returns false when the line is dropped by the quota. */
static bool diag_allow_locked(const char *line, size_t n) {
    char tag[24];
    char level = 'I';
    if (!parse_tag_level(line, n, tag, sizeof(tag), &level)) return true;
    /*
     * DEBUG/VERBOSE 一律不落文件。充电器"recharge kick/recharge after"这类循环日志自己已经
     * 降到 DEBUG（控制台默认不显示），但 vprintf 钩子照样能收到，于是导出包里全是它、真正的
     * 微读记录被挤掉。控制台该看的照样看，文件只留 INFO 及以上。
     * Never store DEBUG/VERBOSE. The charger's once-per-2-second loop already drops itself to
     * DEBUG, yet the vprintf hook still receives it, so the export was nothing but that chatter.
     * The console keeps showing everything; the file keeps INFO and above only.
     */
    if (level == 'D' || level == 'V') return false;
    if (tag_exempt(tag)) return true;

    const int64_t now = esp_timer_get_time() / 1000;
    if (!s_buckets) {
        s_buckets = heap_caps_calloc(DIAG_TAG_MAX, sizeof(diag_tag_bucket_t),
                                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_buckets) return true; /* 分配失败就不过滤，宁可多写不可漏 / fail open */
    }
    diag_tag_bucket_t *b = NULL;
    for (unsigned i = 0; i < s_bucket_count; ++i)
        if (strcmp(s_buckets[i].name, tag) == 0) { b = &s_buckets[i]; break; }
    if (!b) {
        if (s_bucket_count >= DIAG_TAG_MAX) return true;  /* 表满就不过滤，宁可多写不可漏 */
        b = &s_buckets[s_bucket_count++];
        memset(b, 0, sizeof(*b));
        snprintf(b->name, sizeof(b->name), "%s", tag);
        b->window_start_ms = now;
    }
    if (now - b->window_start_ms >= DIAG_TAG_WINDOW_MS) {
        if (b->dropped) {
            char note[96];
            int k = snprintf(note, sizeof(note), "[diag] %s: 上一分钟省略 %u 行\n",
                             b->name, b->dropped);
            if (k > 0) push_stripped(note, (size_t)k);
        }
        b->window_start_ms = now;
        b->lines = 0;
        b->dropped = 0;
    }
    const unsigned quota = (level == 'E' || level == 'W') ? DIAG_TAG_QUOTA_ALERT : DIAG_TAG_QUOTA_NORM;
    if (b->lines >= quota) { b->dropped++; return false; }
    b->lines++;
    return true;
}

static void file_open_append(void) {
    if (s_file || s_suspended) return;
    mkdir(DIAG_DIR, 0777);
    s_file = fopen(DIAG_FILE, "ab");
    if (s_file) {
        struct stat st;
        s_file_size = (stat(DIAG_FILE, &st) == 0) ? (size_t)st.st_size : 0;
    }
}

/* 写满后轮转：diag.log -> diag.log.prev（FAT rename 遇已存在目标会 EEXIST，先 unlink）。
 * Rotate when full: diag.log -> diag.log.prev (unlink first because FAT rename returns EEXIST). */
static void file_rotate_locked(void) {
    if (s_file) { fclose(s_file); s_file = NULL; }
    (void)unlink(DIAG_PREV);
    (void)rename(DIAG_FILE, DIAG_PREV);
    s_file_size = 0;
    file_open_append();
}

static void file_write_locked(const char *buf, size_t n) {
    if (!n) return;
    file_open_append();
    if (!s_file) return;
    if (s_file_size + n > DIAG_FILE_MAX) file_rotate_locked();
    size_t done = fwrite(buf, 1, n, s_file);
    fflush(s_file);
    s_file_size += done;
}

static void flush_ring_to_file(void) {
    if (!s_ring || s_suspended) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    uint64_t oldest = s_ring_seq - s_ring_len;
    if (s_disk_seq < oldest) {
        /* ring 被覆盖、最旧日志已丢，落一个明确标记。/ Ring overflowed; leave a marker. */
        const char marker[] = "\n[...ring overflow, oldest logs dropped...]\n";
        file_write_locked(marker, sizeof(marker) - 1);
        s_disk_seq = oldest;
    }
    char chunk[512];
    while (s_disk_seq < s_ring_seq) {
        size_t n = 0;
        while (n < sizeof(chunk) && s_disk_seq < s_ring_seq) {
            chunk[n++] = s_ring[s_disk_seq % DIAG_RING_CAP];
            s_disk_seq++;
        }
        file_write_locked(chunk, n);
    }
    xSemaphoreGive(s_mutex);
}

/* USB 读卡接管前调用：置挂起标志，先把 ring 排空落盘，再关闭持久文件句柄，释放 SD 访问。
   Called before USB takeover: mark suspended, drain the ring, then close the persistent file. */
void diag_log_suspend(void) {
    if (!s_ring) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (s_suspended) { xSemaphoreGive(s_mutex); return; }
    xSemaphoreGive(s_mutex);
    /* 先在未挂起状态把 ring 排空落盘（防止 USB 期间掉电丢失），再置挂起标志并关闭文件。
       Drain while still active (persist logs before USB power loss), then suspend and close. */
    flush_ring_to_file();
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_suspended = true;
    if (s_file) { fclose(s_file); s_file = NULL; s_file_size = 0; }
    xSemaphoreGive(s_mutex);
}

/* 本机重新挂载 TF 卡后调用：解除挂起并重新打开持久文件，ring 中积压日志随后由定时任务落盘。
   Called after the local SD remount: clear suspended and reopen; the ring drain follows on the timer. */
void diag_log_resume(void) {
    if (!s_ring) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    bool was_suspended = s_suspended;
    s_suspended = false;
    xSemaphoreGive(s_mutex);
    if (was_suspended) file_open_append();
}

static void diag_task(void *arg) {
    (void)arg;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(3000));
        flush_ring_to_file();
    }
}

static int diag_vprintf(const char *fmt, va_list args) {
    va_list copy;
    va_copy(copy, args);
    int ret = s_original ? s_original(fmt, args) : vprintf(fmt, args);
    if (s_ring && !s_dumping) {
        char line[DIAG_LINE_MAX];
        int n = vsnprintf(line, sizeof(line), fmt, copy);
        if (n > 0) {
            if (n >= (int)sizeof(line)) n = sizeof(line) - 1;
            xSemaphoreTake(s_mutex, portMAX_DELAY);
            if (diag_allow_locked(line, (size_t)n)) push_stripped(line, (size_t)n);
            xSemaphoreGive(s_mutex);
        }
    }
    va_end(copy);
    return ret;
}

/* 走和 ESP_LOG 完全相同的原始输出通道（已确认到 COM27），避免标准 printf 因 VFS 路径不同而丢输出。
 * Use the same original sink as ESP_LOG (confirmed on COM27), avoiding any VFS path mismatch. */
static void sink_printf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    if (s_original) s_original(fmt, ap);
    else vprintf(fmt, ap);
    va_end(ap);
}

static void print_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        sink_printf("(missing) %s\n", path);
        return;
    }
    char buf[512];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        /* sink_printf 走原始 esp_log 通道，s_dumping=true 时不会再被自己捕获落 ring。
         * sink_printf uses the original esp_log sink; while s_dumping it is not re-captured. */
        sink_printf("%.*s", (int)n, buf);
    }
    fclose(f);
}

static void dump_nvs_stats(void) {
    nvs_stats_t stats;
    if (nvs_get_stats(NULL, &stats) == ESP_OK) {
        sink_printf("[nvs] used_entries=%u free_entries=%u total_entries=%u namespace_count=%u\n",
               (unsigned)stats.used_entries, (unsigned)stats.free_entries,
               (unsigned)stats.total_entries, (unsigned)stats.namespace_count);
    }
    /* 遍历全部 entry，按命名空间汇总占用，定位大户。/ Walk every entry and group by namespace. */
    nvs_iterator_t it = NULL;
    if (nvs_entry_find(NVS_DEFAULT_PART_NAME, NULL, NVS_TYPE_ANY, &it) != ESP_OK) return;
    struct ns_row { char name[16]; unsigned entries; } rows[24];
    unsigned nrows = 0;
    do {
        nvs_entry_info_t info;
        if (nvs_entry_info(it, &info) == ESP_OK) {
            int found = -1;
            for (unsigned i = 0; i < nrows; ++i)
                if (strcmp(rows[i].name, info.namespace_name) == 0) { found = (int)i; break; }
            if (found < 0 && nrows < 24) {
                found = (int)nrows;
                memset(&rows[nrows], 0, sizeof(rows[nrows]));
                snprintf(rows[nrows].name, sizeof(rows[nrows].name), "%s", info.namespace_name);
                nrows++;
            }
            if (found >= 0) rows[found].entries++;
        }
    } while (nvs_entry_next(&it) == ESP_OK);
    nvs_release_iterator(it);
    for (unsigned i = 0; i < nrows; ++i)
        sink_printf("  [ns] %-16s entries=%u\n", rows[i].name, rows[i].entries);
}

void diag_log_dump_uart(void) {
    flush_ring_to_file();
    s_dumping = true;
    sink_printf("===== diag dump begin =====\n");
    dump_nvs_stats();
    print_file(DIAG_FILE);
    print_file(DIAG_PREV);
    sink_printf("===== diag dump end =====\n");
    s_dumping = false;
}

void diag_log_clear(void) {
    if (!s_mutex) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (s_file) { fclose(s_file); s_file = NULL; }
    (void)unlink(DIAG_FILE);
    (void)unlink(DIAG_PREV);
    s_file_size = 0;
    s_ring_len = 0;
    s_disk_seq = s_ring_seq;
    file_open_append();
    xSemaphoreGive(s_mutex);
    ESP_LOGI(TAG, "diagnostic logs cleared");
}

void diag_log_init(void) {
    if (s_ring) return;
    s_mutex = xSemaphoreCreateMutex();
    /* 优先放 PSRAM，失败退回内部 RAM。/ Prefer PSRAM, fall back to internal RAM. */
    s_ring = heap_caps_malloc(DIAG_RING_CAP, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_ring) s_ring = heap_caps_malloc(DIAG_RING_CAP, MALLOC_CAP_8BIT);
    if (!s_ring) {
        /* 没有缓冲就只保留默认串口输出，不挂载回调。/ No buffer: keep the default sink only. */
        return;
    }
    memset(s_ring, 0, DIAG_RING_CAP);
    file_open_append();
    xTaskCreate(diag_task, "diag_log", 4608, NULL, 1, NULL);
    s_original = esp_log_set_vprintf(diag_vprintf);
    ESP_LOGI(TAG, "diagnostic logging initialised (ring=%d file_max=%d, quota=%d/min per tag)",
             DIAG_RING_CAP, DIAG_FILE_MAX, DIAG_TAG_QUOTA_NORM);
    /* 开机横幅：把复位原因、是否已对时写进日志本身，导出时不必再靠猜。
     * Boot banner: reset reason and whether the clock is set, recorded in the log itself. */
    ESP_LOGI(TAG, "boot: reset=%d time_set=%d prev_log_kept=%d",
             (int)esp_reset_reason(), (int)(time(NULL) >= 1577836800),
             (int)(access(DIAG_PREV, F_OK) == 0));
}

/* ===== 可通过电脑传输的 .log 导出包 ===== */

#define DIAG_EXPORT_FILE "/sdcard/diag_export.log"

const char *diag_log_export_path(void) { return DIAG_EXPORT_FILE; }

static bool copy_file_to(FILE *out, const char *path) {
    FILE *in = fopen(path, "rb");
    if (!in) { fprintf(out, "(missing) %s\n", path); return false; }
    char buf[512];
    size_t n;
    bool ok = true;
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
        if (fwrite(buf, 1, n, out) != n) ok = false;
    }
    fclose(in);
    return ok;
}

/* 把 NVS 全局统计与逐命名空间占用写进导出包（和 dump_nvs_stats 同一口径，只是输出到文件）。 */
static void write_nvs_stats(FILE *out) {
    nvs_stats_t stats;
    if (nvs_get_stats(NULL, &stats) == ESP_OK) {
        fprintf(out, "[nvs] used_entries=%u free_entries=%u total_entries=%u namespace_count=%u\n",
                (unsigned)stats.used_entries, (unsigned)stats.free_entries,
                (unsigned)stats.total_entries, (unsigned)stats.namespace_count);
    }
    nvs_iterator_t it = NULL;
    if (nvs_entry_find(NVS_DEFAULT_PART_NAME, NULL, NVS_TYPE_ANY, &it) != ESP_OK) return;
    struct ns_row { char name[16]; unsigned entries; } rows[24];
    unsigned nrows = 0;
    do {
        nvs_entry_info_t info;
        if (nvs_entry_info(it, &info) == ESP_OK) {
            int found = -1;
            for (unsigned i = 0; i < nrows; ++i)
                if (strcmp(rows[i].name, info.namespace_name) == 0) { found = (int)i; break; }
            if (found < 0 && nrows < 24) {
                found = (int)nrows;
                memset(&rows[nrows], 0, sizeof(rows[nrows]));
                snprintf(rows[nrows].name, sizeof(rows[nrows].name), "%s", info.namespace_name);
                nrows++;
            }
            if (found >= 0) rows[found].entries++;
        }
    } while (nvs_entry_next(&it) == ESP_OK);
    nvs_release_iterator(it);
    for (unsigned i = 0; i < nrows; ++i)
        fprintf(out, "  [ns] %-16s entries=%u\n", rows[i].name, rows[i].entries);
}

bool diag_log_export(void) {
    /* 先把 ring 全部落盘，保证导出包包含最新日志。/ Flush the ring so the export is current. */
    flush_ring_to_file();
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    FILE *out = fopen(DIAG_EXPORT_FILE, "wb");
    if (!out) {
        ESP_LOGE(TAG, "export open failed errno=%d", errno);
        xSemaphoreGive(s_mutex);
        return false;
    }
    time_t now = time(NULL);
    struct tm tm;
    memset(&tm, 0, sizeof(tm));
    localtime_r(&now, &tm);
    fprintf(out, "===== kiikoreader diagnostic export =====\n");
    fprintf(out, "device local time: %04d-%02d-%02d %02d:%02d:%02d%s\n",
            tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
            tm.tm_hour, tm.tm_min, tm.tm_sec,
            (now < 1577836800) ? "  <-- 系统时间未设置(1970)，日志时间戳不可信，以 uptime 为准" : "");
    fprintf(out, "uptime: %lld s (%lld h %lld min)\n",
            (long long)(esp_timer_get_time() / 1000000),
            (long long)(esp_timer_get_time() / 1000000 / 3600),
            (long long)(esp_timer_get_time() / 1000000 / 60 % 60));
    fprintf(out, "heap: internal=%u psram=%u\n",
            (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
            (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    fprintf(out, "reset reason: %d\n", (int)esp_reset_reason());
    fprintf(out, "----- nvs storage -----\n");
    write_nvs_stats(out);
    fprintf(out, "\n----- previous log (older) -----\n");
    copy_file_to(out, DIAG_PREV);
    fprintf(out, "\n----- current log -----\n");
    copy_file_to(out, DIAG_FILE);
    fflush(out);
    fsync(fileno(out));
    fclose(out);
    xSemaphoreGive(s_mutex);
    ESP_LOGI(TAG, "diagnostic package exported to %s", DIAG_EXPORT_FILE);
    return true;
}
