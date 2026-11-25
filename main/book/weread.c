#include "weread_internal.h"
#include "weread_notes.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "book_cover.h"
#include "book_epub.h"
#include "book_chapter_cache.h"
#include "book_progress.h"
#include "book_source.h"
#include "book_title.h"
#include "esp_attr.h"
#include "esp_crt_bundle.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_wifi.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"
#include "read_pico_sd.h"
#include "read_pico_sd_access.h"
#include "read_pico_transfer.h"
#include "settings.h"
#include "time_service.h"
#include "weread_cache.h"
#include "weread_consent.h"
#include "zlib.h"

#define WR_MAX_SHELF 8192
/* 书架同步后顺手落盘前 N 本的章节目录：新书第一次打开就不必在 UI 线程上联网取目录。
 * 只做"最近在读"这几本——目录请求是逐本的，整架都做会把同步拖到不可接受。
 * Stage the TOC of the first N books after a shelf sync, so a new book's first open reads the
 * card instead of the network on the UI thread. Only the recent few: TOC requests are per book,
 * and doing the whole shelf would drag sync past anything reasonable. */
#define WR_TOC_WARM_BOOKS 6u
#define WR_INDEX WR_DIR "/shelf.bin"
#define WR_INDEX_TEMP WR_DIR "/shelf.part"
#define WR_CACHED WR_DIR "/cached.bin"
/* 待补传队列的落盘文件：原实现只把待传秒数放在内存数组里，重启即丢，离线补传因此永远
 * 补不满。落盘后断网、换网、关机都不再丢时长。
 * The pending-report queue on disk: the stock code held pending seconds in a RAM array only,
 * so a reboot dropped them and offline top-up never caught up. */
#define WR_QUEUE WR_DIR "/rtime.queue"
#define WR_QUEUE_TEMP WR_DIR "/rtime.part"
#define WR_QUEUE_BACKUP WR_DIR "/rtime.prev"
#define WR_QUEUE_MAGIC "WRQUEUE2"   /* 现行格式：带阅读位置，重启后补传不再归零 */
#define WR_QUEUE_MAGIC_LEGACY "WRQUEUE1" /* 旧格式：仅书目/账号/秒数，仍可读 */
#define WR_QUEUE_MAX 12

/* 退出阅读后的"阅读进度位置"同步队列：与时长队列分开，记录每本书最近一次本地位置。
 * 在线由 worker 立即经 /web/book/read 上报；离线落盘，联网恢复后自动补传。 */
#define WR_POS_FILE WR_DIR "/pos.queue"
#define WR_POS_MAGIC "WRPOS1"
#define WR_POS_MAX 8
typedef struct {
    char book_id[WR_ID];
    uint32_t chapter, offset, pct;
} wr_pos_rec_t;
static wr_pos_rec_t s_pos_pending[WR_POS_MAX];
static unsigned s_pos_count;
static bool s_pos_restored;
/* 前置声明：位置同步函数定义在本文件后部，但 worker_task 等早前代码即需调用。 */
static void pos_persist(void);
static void pos_restore(void);
static void pos_mark(const char *book_id, unsigned chapter, unsigned offset, unsigned pct);
static void pos_flush(void);
/* 旧版单条待补传记录（WRQUEUE1）：只够恢复书目与秒数，位置缺失，重启后只能报 0/0。
 * Legacy record (WRQUEUE1): restores book and seconds but no position, so it reported 0/0. */
typedef struct {
    char book_id[WR_ID];
    char owner[WR_ID];
    uint32_t library;
    uint32_t seconds;
} wr_queue_rec_t;
/* 现行单条待补传记录（WRQUEUE2）：额外持久化入队时的章节/章内偏移/百分比，
 * 重启恢复后仍能按真实推进位置补传，避免被服务端反作弊按冻结/零位置静默丢弃。
 * Current record (WRQUEUE2): also persists chapter/offset/pct so a restored report keeps a
 * real advancing position instead of being silently dropped as a frozen/zero-position report. */
typedef struct {
    char book_id[WR_ID];
    char owner[WR_ID];
    uint32_t library;
    uint32_t seconds;
    uint32_t chapter;
    uint32_t offset;
    uint32_t pct;
} wr_queue_rec2_t;
#define WR_SD_SCOPE(path) __attribute__((cleanup(read_pico_sd_access_scope_end))) \
    read_pico_sd_access_scope_t scope = read_pico_sd_access_scope(path)

typedef struct { char owner[WR_ID], token[384]; uint32_t library; } wr_account_t;
typedef struct { uint32_t day, today; uint64_t total; } wr_stats_t;
typedef struct { char owner[WR_ID]; uint32_t count; } wr_shelf_header_t;
typedef struct {
    wr_job_t kind;
    wr_book_t book;
    unsigned first, count;
} wr_work_t;
typedef struct {
    char book_id[WR_ID], owner[WR_ID];
    uint32_t library;
    unsigned seconds;
    /* 入队时读者所在的位置：云端还没有这本书的进度时，上报就用它，而不是干脆不发。
     * Local position at queue time: when the cloud has no position for this book yet, the report
     * uses this instead of being skipped outright. */
    unsigned chapter, offset, pct;
    unsigned failures;
    int64_t retry_at;
    /* 本批是否为"补传"：实时在线阅读为 false（isResendReadingInfo=0），
       离线累积/落盘重载/断网后补发为 true（isResendReadingInfo=1）。
       仅内存标记，不进磁盘格式；开机从盘上恢复的记录一律视为补传。 */
    bool resend;
    /* 本会话是否已发过"进入阅读"请求：仅内存态，不进磁盘格式，开机/落盘恢复后回到 false，
       回放首拍重新进入。每个活动阅读会话只进入一次。/ Whether the enter-read request was sent
       this session; RAM only, resets to false on boot/restore. */
    bool entered;
    /* 本次回放已确认的累计秒数：仅内存态，整批补完时用于弹窗展示总数，不进磁盘格式。
       Cumulative confirmed seconds for the current drain; RAM only, for the success popup. */
    unsigned replay_done;
} wr_report_t;

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
enum { WR_WORKER_STACK_BYTES = 16 * 1024, WR_HTTP_URL_BYTES = 2080 };
// 缓存和上报都会写闪存，任务栈必须位于内部 RAM。/ Both cache and report write flash, so the task stack must reside in internal RAM.
static DRAM_ATTR StackType_t s_worker_stack[WR_WORKER_STACK_BYTES / sizeof(StackType_t)];
static DRAM_ATTR StaticTask_t s_worker_tcb;
static TaskHandle_t s_worker;
static wr_work_t s_work;
/* 用户任务插队：后台预取正在跑时，同步/缓存/进度这类用户动作先登记在这里，
 * 预取收到取消后立即接着执行——界面不必再弹"请等待当前操作完成"。
 * User jobs jump the queue: while a background prefetch runs, sync/cache/progress are parked
 * here and run as soon as the prefetch backs off, so the UI never has to say "wait for the
 * current operation". */
static wr_work_t s_queued;
static bool s_queued_pending;
/* 预取被"worker 正忙"挡下时记在这里，本轮任务收尾后由 worker 自己补跑。
 * 没有这个登记位，被挡下的请求就永远丢了：等待页空转到 6 秒，只能提示读者退出重进。
 * A prefetch turned away because the worker was busy is parked here and re-run by the worker
 * itself when the round ends. Without it the request is simply lost: the waiting page spins for
 * six seconds and all it can say is "leave and try again". */
static bool s_prefetch_pending;
static wr_book_t s_prefetch_book;
static unsigned s_prefetch_first;
/* 补跑用的任务参数放静态区：worker 栈已经装着 4 KB 的 URL 缓冲与解析上下文，
 * 不该再为一份 700 字节的任务参数增加栈深。只有一个 worker，因此不需要加锁。
 * The retry descriptor lives in static storage: the worker stack already carries 4 KB URL
 * buffers and parse context, so a 700-byte descriptor should not deepen it. One worker only. */
static wr_work_t s_retry_prefetch;
static wr_report_t s_reports[WR_QUEUE_MAX];
/* 待补传队列是否已经尝试过读盘：SD 卡在 wr_init 时可能还没挂载，允许开书时再补一次。
 * Whether the pending queue was already read from SD; the card may not be mounted yet at
 * wr_init, so opening a book gives it one more chance. */
static bool s_queue_restored;
/* 本会话是否已成功把队列写过盘：一旦成功写过，说明 SD 可写且内存状态就是权威，
 * 之后队列清空时可以安全删文件（即使开机没走过读盘恢复，记录是本会话内产生的）。
 * Whether the queue was successfully persisted this session; once written, RAM is authoritative
 * and an empty queue may safely unlink even without a prior boot-time restore. */
static bool s_queue_written;
/* 补传成功通知：每次服务器确认一条离线时长，序号 +1，并记下该次确认秒数，供 UI 弹窗。
 * Confirmation notification: the sequence increments (and the seconds are recorded) each
 * time the server confirms an offline-duration record, so the UI can show a success popup. */
static volatile uint32_t s_confirm_seq;
static volatile unsigned s_last_confirm_seconds;
/* 落盘队列的恢复口在 wr_init 之后定义，先声明给它用。
 * The restore entry point is defined after wr_init, so forward-declare it here. */
static void queue_restore(void);
/* process_report 在落盘函数之前定义，同样先声明。
 * process_report is defined before the persist helper, so forward-declare that too. */
static bool queue_persist(void);
static bool s_work_pending;
static bool s_processing_report;
static bool s_reader_scanning;
static int64_t s_reader_scan_end;
static esp_http_client_handle_t s_http_keepalive;
static char s_http_keepalive_host[128];
/* 上一次复用请求结束的时刻。空闲太久再复用时，服务端其实已经关掉这条连接，而客户端
 * 看不出来，只能等满 15 秒超时（实测跳章因此多等 15.6 秒）。超过阈值就先丢弃重连。
 * When the pooled connection has been idle too long the server has already closed it while the
 * client cannot tell, and the next request waits out the full 15 s timeout (measured: 15.6 s
 * added to a chapter jump). Drop and reconnect past the threshold instead. */
static int64_t s_http_keepalive_used_us;
/* 与 v2.0 的连接池空闲阈值（20s）对齐：连续缓存时章间的写盘/解码间隙不再触发
 * 不必要的重握手；仍保留"超阈值先丢弃重连"，避免服务端已关闭时等满 15s 超时。 */
#define WR_KEEPALIVE_IDLE_DROP_US (20LL * 1000 * 1000)
/*
 * 内部 RAM 的"接近枯竭"水位。这个数字只能用来决定"要不要先丢掉复用连接"，绝不能用来决定
 * "能不能发请求"：这块板常态就是 24–38 KB（实测 open 时 38291、worker 起时 25895），
 * 曾按 44 KB 拒绝请求，结果章包与时长上报全部失败，等于把联网整个关掉。
 * Internal-RAM exhaustion level. This number may only decide whether to drop the pooled
 * connection first — never whether a request may run. The board normally sits at 24–38 KB
 * (38291 at open, 25895 when the worker starts); gating requests at 44 KB rejected every
 * chapter bundle and every reading-time report, effectively turning the network off.
 */
#define WR_TLS_MIN_INTERNAL (16u * 1024u)
/*
 * 连续多少个"15 秒窗口一个字节都没来"才算真的断了。4 轮 = 一分钟的静默，足够跨过链路上
 * 的短暂停顿；再久就不是抖动而是死连接，继续等只会让整章缓存卡住。
 * How many consecutive 15 s windows with zero bytes count as a dead connection. Four is a
 * minute of silence: long enough to ride out a stall, short enough that a dead socket does not
 * freeze the whole chapter job.
 */
#define WR_EMPTY_READ_LIMIT 4u
/*
 * HTTP 客户端的读缓冲。4 KB 时每读满一次就要等一个往返，实测到微读 CDN 的吞吐只有
 * 6 KB/s（370 KB 的章包 63 秒），8 KB 让每个往返搬回双倍数据；再大就要和 TLS、面板抢
 * 内部 RAM，这块板常态只有 40 KB 可用。
 * Size of the HTTP client's read buffer. At 4 KB every fill costs one round trip and the
 * measured throughput to WeRead's CDN is 6 KB/s (63 s for a 370 KB bundle); 8 KB moves twice
 * the data per round trip, and anything larger competes with TLS and the panel for the ~40 KB
 * of internal RAM this board has.
 */
#define WR_HTTP_BUFFER 8192
static struct {
    wr_report_t account_book;
    char chapter[WR_ID];
    wr_reader_t fields;
    int64_t expires;
} s_report_fields;
// Each HTTP request owns redirect and cookie-capture state, so bounded cache
// prefetch requests can overlap without corrupting one another.
typedef struct {
    char absolute[WR_HTTP_URL_BYTES];
    char redirected[WR_HTTP_URL_BYTES];
    char location[WR_HTTP_URL_BYTES];
    char current[WR_HTTP_URL_BYTES];
    char cookie_snapshot[1024];
    bool capture_service_cookies;
    bool capture_body;
    bool body_overflow;
    int status_code;
    bool *post_started;
    size_t body_limit, body_used, body_capacity;
    char *body_data;
} wr_http_context_t;
static StaticSemaphore_t s_cookie_mutex_storage;
static SemaphoreHandle_t s_cookie_mutex;
/* 全书缓存的预取任务会与主任务并发进入 HTTP：用一把粗锁把整条 http_request 串行化，
 * 保护单槽 keepalive 等全局状态。实际收发仍按序执行，预取的下载与主任务的非网络
 * 渲染/写盘在时间上重叠，这正是流水线提速的来源。 */
static StaticSemaphore_t s_http_mutex_storage;
static SemaphoreHandle_t s_http_mutex;
static wr_result_t s_result;
static wr_job_t s_active_job;
static atomic_bool s_cancel;
static unsigned s_done, s_total;
static char s_qr[180];
static wr_account_t s_http_account;
static char s_cookies[1024];
static _Atomic bool s_expired;
static _Atomic bool s_http_open_failed;
static _Atomic(const char *) s_error_message = "操作失败，请检查网络后重试";
static const char *s_result_message = "操作失败，请检查网络后重试";
/* 阅读会话归属于开书时的账号：统计与上报都用这份快照，切号后不会写到新账号。
 * A reading session belongs to the account that opened the book; reports and stats use
 * this snapshot so a later account switch can never bill the new account. */
/* 单页停留计时上限：同一阅读位置（章+页内偏移）累计停留超过该值后不再计时，对齐微信读书
 * 官方口径“单页停留太久计时就停止”，防止设备静止在一页时无限累积时长。墨水屏一屏文字通常
 * 60 秒内可读完，故取 60s；该值为单一可调常量，如需贴合个人阅读速度可在此调整。
 * Per-page dwell cap: timing stops once one position stays open longer than this, matching the
 * official rule that a page left open too long stops accruing reading time. Tunable in one place. */
#define WR_DWELL_CAP_MS 60000

static struct {
    wr_book_t book;
    char owner[WR_ID];
    uint32_t library;
    int64_t last;
    uint32_t ms;
    unsigned chapter, offset, pct;
    /* 单页停留状态：当前停留位置与该位置已累计的停留毫秒数，翻页/跳转即重置。
     * Dwell state: the current static position and its accumulated dwell, reset on page change. */
    unsigned dwell_chapter, dwell_offset;
    int64_t dwell_ms;
    bool attached;
} s_reading;
static bool s_radio_held;
static int s_radio_jobs;      // 正在跑的后台网络任务数 / Background network jobs in flight
static bool s_radio_session;  // 阅读会话要求常收 / Reading session wants always-on RX
/* 下载进度成功后要打开的章节；界面据此"下载即定位、缓存后直接进正文"。
 * Chapter the UI should open after a successful progress download, so a download leads
 * straight into that chapter and its text appears once the chapter lands. */
static unsigned s_progress_target = UINT_MAX;
/*
 * 首开一本一章都没落盘的书时，本地没有进度可言，云端可能有。直接下第一章往往白下一整章，
 * 尤其手机上的进度在几十章之外。这里登记一次"待解析"，由 worker 在第一次下载之前调一次
 * getProgress（一个几十 KB 的 JSON），把结果交给阅读页：阅读页据此把等待的章节换成云端那一章。
 * Opening a book with no cached chapter has no local position to honour, while the cloud may
 * have one — far away in the book. Fetching chapter one first is then a wasted chapter. Register
 * one "to be resolved" marker here; the worker calls getProgress once (a small JSON) before its
 * first download and hands the result to the reader, which re-targets the chapter it waits for.
 */
static wr_seed_t s_seed_state = WR_SEED_IDLE;
static char s_seed_book[WR_ID];
static unsigned s_seed_chapter, s_seed_offset;
/* 预取窗口正在下哪一章（-1 表示没有）。目录徽标与等待页都靠它区分"排着队"和"没缓存"。 */
/* Which chapter the prefetch window is fetching right now (-1 when idle). The TOC badge and
 * the waiting page use it to tell "queued" from "not cached". */
static volatile int s_fetching_chapter = -1;

static void restore_session_cookie(void);
static bool store_book_title(const char *path, const char *title);
static bool wr_url_allowed(const char *url, bool service);
static bool url_needs_cookies(const char *url);
static bool cover_redirect_url(const char *current, const char *location,
                               char out[WR_HTTP_URL_BYTES]);

const char *wr_error_message(void) { return atomic_load(&s_error_message); }
void wr_error_set(const char *message) { if (message && *message) atomic_store(&s_error_message, message); }
const char *wr_result_message(void) {
    portENTER_CRITICAL(&s_lock);
    const char *message = s_result_message;
    portEXIT_CRITICAL(&s_lock);
    return message;
}

static bool read_account(wr_account_t *account) {
    memset(account, 0, sizeof(*account));
    nvs_handle_t nvs;
    if (nvs_open("weread", NVS_READONLY, &nvs) != ESP_OK) return false;
    size_t size = sizeof(*account);
    esp_err_t result = nvs_get_blob(nvs, "account", account, &size);
    nvs_close(nvs);
    return result == ESP_OK && size == sizeof(*account) &&
        memchr(account->owner, 0, sizeof(account->owner)) && memchr(account->token, 0, sizeof(account->token));
}

static bool save_account(const wr_account_t *account) {
    nvs_handle_t nvs;
    if (nvs_open("weread", NVS_READWRITE, &nvs) != ESP_OK) return false;
    esp_err_t result = nvs_set_blob(nvs, "account", account, sizeof(*account));
    if (result == ESP_OK) result = nvs_commit(nvs);
    nvs_close(nvs);
    return result == ESP_OK;
}

static void save_session_cookies(void) {
    if (!s_cookies[0]) return;
    nvs_handle_t nvs;
    if (nvs_open("weread", NVS_READWRITE, &nvs) != ESP_OK) return;
    if (nvs_set_str(nvs, "cookies", s_cookies) == ESP_OK) (void)nvs_commit(nvs);
    nvs_close(nvs);
}

static void cookie_lock(void) {
    if (s_cookie_mutex) (void)xSemaphoreTake(s_cookie_mutex, portMAX_DELAY);
}

static void cookie_unlock(void) {
    if (s_cookie_mutex) (void)xSemaphoreGive(s_cookie_mutex);
}

/* 粗粒度 HTTP 串行锁：预取任务与主任务并发调用 http_request 时，保证单槽 keepalive
 * 及其它全局 HTTP 状态不被同时修改。锁在整个请求期间持有，调用方非网络阶段不占锁。 */
static void http_lock(void) {
    if (s_http_mutex) (void)xSemaphoreTake(s_http_mutex, portMAX_DELAY);
}
static void http_unlock(void) {
    if (s_http_mutex) (void)xSemaphoreGive(s_http_mutex);
}

static bool clear_session_cookies(void) {
    nvs_handle_t nvs;
    if (nvs_open("weread", NVS_READWRITE, &nvs) != ESP_OK) return false;
    esp_err_t result = nvs_erase_key(nvs, "cookies");
    if (result == ESP_ERR_NVS_NOT_FOUND) result = ESP_OK;
    if (result == ESP_OK) result = nvs_commit(nvs);
    nvs_close(nvs);
    memset(s_cookies, 0, sizeof(s_cookies));
    return result == ESP_OK;
}

static bool session_cookie_has(const char *name) {
    const size_t length = strlen(name);
    for (const char *item = s_cookies; item && *item;) {
        while (*item == ' ') ++item;
        const char *end = strstr(item, "; ");
        if (!end) end = item + strlen(item);
        if ((size_t)(end - item) > length && item[length] == '=' && !memcmp(item, name, length)) return true;
        item = *end ? end + 2 : end;
    }
    return false;
}

static bool session_cookie_copy(const char *name, char *out, size_t size) {
    if (!out || !size) return false;
    out[0] = 0;
    const size_t length = strlen(name);
    for (const char *item = s_cookies; item && *item;) {
        while (*item == ' ') ++item;
        const char *end = strstr(item, "; ");
        if (!end) end = item + strlen(item);
        if ((size_t)(end - item) > length && item[length] == '=' && !memcmp(item, name, length)) {
            size_t value_length = (size_t)(end - item) - length - 1;
            if (!value_length || value_length >= size) return false;
            memcpy(out, item + length + 1, value_length);
            out[value_length] = 0;
            return true;
        }
        item = *end ? end + 2 : end;
    }
    return false;
}

static void stats_key(const char *owner, char key[15]) {
    uint64_t hash = UINT64_C(14695981039346656037);
    for (; *owner; ++owner) hash = (hash ^ (unsigned char)*owner) * UINT64_C(1099511628211);
    snprintf(key, 15, "s%013llx", (unsigned long long)(hash & UINT64_C(0xfffffffffffff)));
}

static uint32_t current_day(void) {
    time_t now = time(NULL);
    return now >= 1704067200 ? (uint32_t)((now + 8 * 3600) / 86400) : 0;
}

static wr_stats_t read_stats(nvs_handle_t nvs, const char *key) {
    wr_stats_t stats = {0};
    size_t size = sizeof(stats);
    if (nvs_get_blob(nvs, key, &stats, &size) != ESP_OK || size != sizeof(stats)) stats = (wr_stats_t){0};
    if (stats.day != current_day()) { stats.day = current_day(); stats.today = 0; }
    return stats;
}

bool wr_account(char id[WR_ID], uint32_t *today, uint64_t *total) {
    wr_account_t account;
    const bool logged_in = read_account(&account) && account.token[0];
    if (id) snprintf(id, WR_ID, "%s", account.owner);
    wr_stats_t stats = {0};
    nvs_handle_t nvs;
    if (account.owner[0] && (today || total) && nvs_open("weread", NVS_READONLY, &nvs) == ESP_OK) {
        char key[15];
        stats_key(account.owner, key);
        stats = read_stats(nvs, key);
        nvs_close(nvs);
    }
    if (today) *today = stats.today;
    if (total) *total = stats.total;
    return logged_in;
}

static bool add_stats(const char *owner, unsigned seconds) {
    char key[15];
    stats_key(owner, key);
    nvs_handle_t nvs;
    if (nvs_open("weread", NVS_READWRITE, &nvs) != ESP_OK) return false;
    wr_stats_t stats = read_stats(nvs, key);
    stats.today = seconds > UINT32_MAX-stats.today ? UINT32_MAX : stats.today+seconds;
    stats.total = seconds > UINT64_MAX-stats.total ? UINT64_MAX : stats.total+seconds;
    esp_err_t result = nvs_set_blob(nvs, key, &stats, sizeof(stats));
    if (result == ESP_OK) result = nvs_commit(nvs);
    nvs_close(nvs);
    return result == ESP_OK;
}

bool wr_online(void) {
    read_pico_transfer_status_t status;
    read_pico_transfer_get_status(&status);
    return status.network_ready && status.mode == READ_PICO_TRANSFER_MODE_STA;
}

bool wr_safe(const char *value) {
    if (!value || !*value) return false;
    for (; *value; ++value) if (!isalnum((unsigned char)*value) && !strchr("-_.~", *value)) return false;
    return true;
}

bool wr_value(const cJSON *value, char *out, size_t size) {
    if (cJSON_IsString(value)) return strlcpy(out, value->valuestring, size) < size;
    if (!cJSON_IsNumber(value) || value->valuedouble < 0 || value->valuedouble > 9007199254740991.0) return false;
    int n = snprintf(out, size, "%.0f", value->valuedouble);
    return n > 0 && (size_t)n < size;
}

bool wr_cancelled(void) { return !s_processing_report && atomic_load(&s_cancel); }
void wr_cancel(void) {
    /* 取消要连排队中的用户任务一起丢掉：否则点了取消，预取停下后那个被插队的任务仍会
     * 闷头跑起来，读者看到的是"取消了却还在执行"。
     * Cancel drops the queued user job too: otherwise the parked job still runs after the
     * prefetch stops, which reads as "I cancelled it but it keeps going". */
    portENTER_CRITICAL(&s_lock);
    s_queued_pending = false;
    portEXIT_CRITICAL(&s_lock);
    atomic_store(&s_cancel, true);
}

/*
 * 常收模式有两个主人：后台网络任务（下载/预取/上报）和阅读会话。以前只有整本缓存
 * 会开常收，日常读到的是预取，于是跳章和预取一直在省电模式下跑——章包实测只有
 * 23 KB/s，一个 3 MB 的插图章要 136 秒。现在两边共用引用计数，任一方还要就保持常收。
 * Two owners share always-on RX: background network jobs (fetch/prefetch/report) and the
 * reading session. Only whole-book caching used to raise it, so the prefetch that actually
 * runs while reading stayed in power save — a measured 23 KB/s, i.e. 136 s for one 3 MB
 * illustrated chapter. Both now go through one reference count.
 */
static void radio_apply(void) {
    bool want;
    portENTER_CRITICAL(&s_lock);
    want = s_radio_jobs > 0 || s_radio_session;
    bool changed = want != s_radio_held;
    if (changed) s_radio_held = want;
    portEXIT_CRITICAL(&s_lock);
    if (changed) (void)read_pico_transfer_radio_hold(want);
}

/* 阅读会话：开书时要求常收，退出或息屏时放手。 */
static void radio_hold(bool on) {
    portENTER_CRITICAL(&s_lock);
    s_radio_session = on;
    portEXIT_CRITICAL(&s_lock);
    radio_apply();
}

/* 后台网络任务：进任务时占一位，任务收尾时还一位。 */
static void radio_job(bool busy) {
    portENTER_CRITICAL(&s_lock);
    if (busy) { if (s_radio_jobs < 8) ++s_radio_jobs; }
    else if (s_radio_jobs > 0) --s_radio_jobs;
    portEXIT_CRITICAL(&s_lock);
    radio_apply();
}

static bool account_is(const char *owner, uint32_t library) {
    wr_account_t current;
    return read_account(&current) && current.token[0] &&
        !strcmp(current.owner, owner) && current.library == library;
}

bool wr_reader_fields_cached(const char *book, const char *chapter, wr_reader_t *reader) {
    if (!book || !chapter || !reader || s_report_fields.expires <= esp_timer_get_time() ||
        strcmp(s_http_account.owner, s_report_fields.account_book.owner) ||
        s_http_account.library != s_report_fields.account_book.library ||
        strcmp(book, s_report_fields.account_book.book_id) ||
        strcmp(chapter, s_report_fields.chapter)) return false;
    *reader = s_report_fields.fields;
    ESP_LOGI("WEREAD", "reader fields reused book=%s", book);
    return true;
}

/*
 * 把刚取到的一章签名留给上报复用。下载章节时本来就要读一次 /web/reader 拿 ps/pc/token，
 * 那是整条链路里最贵的一次请求（约 20 秒）；上报同一章时若再读一次，等于把时长统计的代价
 * 翻倍，而且会和后台预取抢同一条链路。
 * Hand the signature just fetched to the reporter. Downloading a chapter already reads
 * /web/reader for ps/pc/token — the single most expensive request in the pipeline (~20 s);
 * reading it again for the same chapter would double the cost of counting reading time and
 * compete with the prefetch for the same link.
 */
void wr_reader_fields_store(const char *book, const char *chapter, const wr_reader_t *fields) {
    if (!book || !chapter || !fields || !fields->ps[0]) return;
    portENTER_CRITICAL(&s_lock);
    memset(&s_report_fields.account_book, 0, sizeof(s_report_fields.account_book));
    snprintf(s_report_fields.account_book.book_id, sizeof(s_report_fields.account_book.book_id), "%s", book);
    snprintf(s_report_fields.account_book.owner, sizeof(s_report_fields.account_book.owner), "%s",
             s_http_account.owner);
    s_report_fields.account_book.library = s_http_account.library;
    snprintf(s_report_fields.chapter, sizeof(s_report_fields.chapter), "%s", chapter);
    s_report_fields.fields = *fields;
    s_report_fields.expires = esp_timer_get_time() + 300000000;
    portEXIT_CRITICAL(&s_lock);
}

static esp_err_t capture_cookie(esp_http_client_event_t *event) {
    wr_http_context_t *context = event->user_data;
    if (!context) return ESP_OK;
    if (event->event_id != HTTP_EVENT_ON_HEADER || !event->header_key || !event->header_value) return ESP_OK;
    if (!strcasecmp(event->header_key,"Location")) {
        size_t n = strnlen(event->header_value, WR_HTTP_URL_BYTES);
        if (n && n < WR_HTTP_URL_BYTES) {
            memcpy(context->location,event->header_value,n);
            context->location[n]=0;
        }
        return ESP_OK;
    }
    if (strcasecmp(event->header_key,"Set-Cookie") ||
        !context->capture_service_cookies) return ESP_OK;
    const char *header = event->header_value;
    const char *equals = strchr(header,'=');
    if (!equals || strncmp(header,"wr_",3) || equals-header > 24) return ESP_OK;
    size_t key_len = equals-header, len = strcspn(header,";\r\n");
    if (len <= key_len+1 || len > 440) return ESP_OK;
    for (size_t i = 0; i < len; ++i) {
        if ((unsigned char)header[i] <= ' ' || (unsigned char)header[i] >= 127) return ESP_OK;
    }
    cookie_lock();
    char *p = s_cookies;
    while (*p) {
        char *end = strstr(p,"; ");
        if (!end) end = p+strlen(p);
        if ((size_t)(end-p) > key_len && p[key_len]=='=' && !strncmp(p,header,key_len)) {
            if (*end) memmove(p,end+2,strlen(end+2)+1);
            else if (p>s_cookies) p[-2]=0;
            else *p=0;
            break;
        }
        p = *end ? end+2 : end;
    }
    size_t used = strlen(s_cookies);
    if (used+len+3 <= sizeof(s_cookies)) {
        if (used) { memcpy(s_cookies+used,"; ",2); used+=2; }
        memcpy(s_cookies+used,header,len);
        s_cookies[used+len]=0;
        save_session_cookies();
    }
    cookie_unlock();
    return ESP_OK;
}

static void http_keepalive_drop(void) {
    if (s_http_keepalive) {
        esp_http_client_cleanup(s_http_keepalive);
        s_http_keepalive = NULL;
    }
    s_http_keepalive_host[0] = 0;
    s_http_keepalive_used_us = 0;
}

/* 断链/关 WiFi 即趁 esp_netif 还活着清理 web 长连接：客户端里残留的 RX 自定义 pbuf 会在
 * cleanup 中释放，此时 netif 的 L2 回收函数指针仍有效。拖到离线后再清理，netif 已销毁，
 * pbuf 的回收回调变成野指针（实测 Instruction access fault，与 consent app keepalive 同类）。
 * Drop the web warm client on link loss/stop while the netif (and its L2 free callback) is valid. */
static void http_keepalive_link_event(void *arg, esp_event_base_t base, int32_t id, void *data) {
    (void)arg; (void)base; (void)id; (void)data;
    if (!s_http_mutex) return;
    /* 最多等一个在途请求结束；拿不到锁就让下一次断连/perform 兜底，不阻塞事件循环。 */
    if (xSemaphoreTake(s_http_mutex, pdMS_TO_TICKS(20000))) {
        if (s_http_keepalive) ESP_LOGI("WEREAD", "web keepalive dropped on link down");
        http_keepalive_drop();
        xSemaphoreGive(s_http_mutex);
    }
}

static esp_err_t capture_keepalive_event(esp_http_client_event_t *event) {
    wr_http_context_t *request = event ? event->user_data : NULL;
    if (!request) return ESP_OK;
    if (event->event_id == HTTP_EVENT_HEADERS_SENT && request->post_started) {
        *request->post_started = true;
    }
    if (event->event_id == HTTP_EVENT_ON_DATA && request->capture_body) {
        if (wr_cancelled()) return ESP_FAIL;
        if (event->data_len < 0 || (size_t)event->data_len > request->body_limit - request->body_used) {
            request->body_overflow = true;
            return ESP_FAIL;
        }
        size_t needed = request->body_used + (size_t)event->data_len;
        if (needed > request->body_capacity) {
            size_t next = request->body_capacity ? request->body_capacity : 4096;
            while (next < needed) {
                size_t grown = next <= request->body_limit / 2 ? next * 2 : request->body_limit;
                if (grown <= next) { next = needed; break; }
                next = grown;
            }
            if (next > request->body_limit) next = request->body_limit;
            char *data = heap_caps_realloc(request->body_data, next + 1,
                                           MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (!data) return ESP_ERR_NO_MEM;
            request->body_data = data;
            request->body_capacity = next;
        }
        if (event->data_len) {
            memcpy(request->body_data + request->body_used, event->data, (size_t)event->data_len);
            request->body_used += (size_t)event->data_len;
        }
        return ESP_OK;
    }
    return capture_cookie(event);
}

static bool http_url_host(const char *url, char host[128]) {
    if (!url || strncmp(url, "https://", 8)) return false;
    const char *start = url + 8;
    const char *end = strpbrk(start, "/?#");
    if (!end) end = start + strlen(start);
    size_t length = (size_t)(end - start);
    if (!length || length >= 128) return false;
    memcpy(host, start, length);
    host[length] = 0;
    return true;
}

static esp_http_client_handle_t http_keepalive_get(const char *url, wr_http_context_t *request) {
    char host[128];
    if (!http_url_host(url, host)) return NULL;
    if (s_http_keepalive && strcasecmp(host, s_http_keepalive_host)) http_keepalive_drop();
    /* 空闲超阈值就主动重连：一次 TLS 建连几百毫秒，比等满 15 秒超时划算得多。 */
    /* Reconnect when the idle window has passed: a TLS handshake costs hundreds of
     * milliseconds, far less than waiting out a fifteen-second timeout. */
    if (s_http_keepalive && s_http_keepalive_used_us) {
        const int64_t idle = esp_timer_get_time() - s_http_keepalive_used_us;
        if (idle > WR_KEEPALIVE_IDLE_DROP_US) {
            ESP_LOGI("WEREAD", "keepalive dropped idle_ms=%lld", (long long)(idle / 1000));
            http_keepalive_drop();
        }
    }
    if (!s_http_keepalive) {
        esp_http_client_config_t config = {
            .url = url, .timeout_ms = 15000, .buffer_size = WR_HTTP_BUFFER, .buffer_size_tx = 2048,
            .crt_bundle_attach = esp_crt_bundle_attach, .disable_auto_redirect = true,
            .keep_alive_enable = true, .keep_alive_idle = 30, .keep_alive_interval = 5,
            .keep_alive_count = 5, .user_agent = WR_AGENT,
            .event_handler = capture_keepalive_event, .user_data = request,
        };
        s_http_keepalive = esp_http_client_init(&config);
        if (!s_http_keepalive) return NULL;
        snprintf(s_http_keepalive_host, sizeof(s_http_keepalive_host), "%s", host);
    } else {
        if (esp_http_client_set_url(s_http_keepalive, url) != ESP_OK ||
            esp_http_client_set_event_handler(s_http_keepalive, capture_keepalive_event) != ESP_OK ||
            esp_http_client_set_user_data(s_http_keepalive, request) != ESP_OK) return NULL;
    }
    return s_http_keepalive;
}

#define WR_GUNZIP_MAX (8u * 1024u * 1024u)

/* Accept-Encoding: gzip 之后由这里解压：IDF 的 http_client 不解压 gzip。
 * zlib inflate 逐段扩容，输出上限取调用方给定的 body_limit。
 * Decompress a gzip body here; the IDF http_client does not inflate on its own. */
static bool gunzip_body(wr_http_context_t *request) {
    if (!request->body_data || request->body_used < 2) return true;
    const uint8_t *head = (const uint8_t *)request->body_data;
    if (head[0] != 0x1f || head[1] != 0x8b) return true;
    const size_t compressed = request->body_used;
    const size_t limit = request->body_limit ? request->body_limit : WR_GUNZIP_MAX;
    size_t capacity = compressed * 2 + 4096;
    if (capacity > limit) capacity = limit;
    char *out = heap_caps_malloc(capacity + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!out) {
        ESP_LOGW("WEREAD", "gunzip allocation failed compressed=%u", (unsigned)compressed);
        return false;
    }
    z_stream stream;
    memset(&stream, 0, sizeof(stream));
    stream.next_in = (Bytef *)request->body_data;
    stream.avail_in = (uInt)compressed;
    if (inflateInit2(&stream, 15 + 32) != Z_OK) { free(out); return false; }
    size_t produced = 0;
    bool ok = false;
    for (;;) {
        stream.next_out = (Bytef *)(out + produced);
        stream.avail_out = (uInt)(capacity - produced);
        int status = inflate(&stream, Z_NO_FLUSH);
        produced = capacity - stream.avail_out;
        if (status == Z_STREAM_END) { ok = true; break; }
        if (status != Z_OK && status != Z_BUF_ERROR) break;
        if (produced < capacity) {
            if (status == Z_BUF_ERROR) break;
            continue;
        }
        if (capacity >= limit) break;
        size_t next = capacity * 2 > limit ? limit : capacity * 2;
        char *grown = heap_caps_realloc(out, next + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!grown) break;
        out = grown;
        capacity = next;
    }
    inflateEnd(&stream);
    if (!ok) {
        ESP_LOGW("WEREAD", "gunzip failed compressed=%u plain=%u", (unsigned)compressed, (unsigned)produced);
        free(out);
        return false;
    }
    out[produced] = 0;
    free(request->body_data);
    request->body_data = out;
    request->body_used = request->body_capacity = produced;
    ESP_LOGI("WEREAD", "gunzip ok compressed=%u plain=%u", (unsigned)compressed, (unsigned)produced);
    return true;
}

/* 最近一次 HTTP 请求的状态码；0 表示还没拿到响应（连接失败/超时）。 */
/* Status code of the last HTTP response; 0 means no response arrived at all. */
static int s_last_http_status;
int wr_last_http_status(void) { return s_last_http_status; }

/*
 * 单次请求的时间预算。实测链路在 3–10 KB/s 之间，而一本漫画书的章包能有 3 MB——按这个速率
 * 是八分钟，期间整章缓存都堵在这里。到点就收手，把已经读到的字节交给调用方（顺序 tar 的
 * 前半段照样能解析出图），剩下的留给下一轮补。
 * Per-request time budget. The measured link runs at 3–10 KB/s while one comic chapter bundle
 * can be 3 MB — eight minutes with the whole chapter job stuck behind it. When the budget runs
 * out we hand back what has arrived (the front of a sequential tar still parses into images)
 * and leave the rest for the next pass.
 */
static int64_t s_stream_deadline_us;
static bool s_stream_partial;
void wr_http_budget(unsigned ms, bool partial) {
    s_stream_deadline_us = ms ? esp_timer_get_time() + (int64_t)ms * 1000 : 0;
    s_stream_partial = partial;
}

static char *http_request_reusable(wr_http_context_t *request, const char *url, const char *body,
                                   const char *referer, size_t limit, size_t *length,
                                   bool *post_started) {
    /*
     * 先确认链路。这条路以前不查在线状态：离线时照样建 TLS 会话卡在 DNS 上三秒才回来
     * （日志里 getaddrinfo() returns 202），界面只能显示"网络连接失败，请检查 Wi-Fi"，
     * 读者看到的是"获取目录失败"，而真正原因是没有网络。现在直接判定，文案与原因一致。
     * Confirm the link first. This path never checked it: offline, it still opened a TLS session
     * and stalled three seconds in DNS (getaddrinfo() returns 202), and the UI could only say
     * "network connection failed, check Wi-Fi" — the reader saw "TOC fetch failed" while the real
     * cause was no network. Decide up front so the message matches the reason.
     */
    if (!wr_online()) {
        http_keepalive_drop();
        wr_error_set("网络未连接，请先在快捷菜单连上 Wi-Fi");
        ESP_LOGW("WEREAD", "request blocked offline url=%s", url);
        return NULL;
    }
    request->capture_body = true;
    request->body_limit = limit;
    request->post_started = post_started;
    request->capture_service_cookies = url[0] == '/';
    bool service = request->capture_service_cookies;
    char *absolute = request->absolute;
    const bool upgrade = !service && !strncmp(url, "http://", 7);
    const char *prefix = service ? WR_HOST : upgrade ? "https://" : "";
    int n = snprintf(absolute, WR_HTTP_URL_BYTES, "%s%s", prefix, upgrade ? url + 7 : url);
    if (n < 0 || n >= WR_HTTP_URL_BYTES || (!service && !wr_url_allowed(absolute, false))) {
        wr_error_set("微信读书请求地址无效");
        return NULL;
    }
    for (const char *p = absolute; *p; ++p) {
        if ((unsigned char)*p <= ' ' || *p == '\\') {
            wr_error_set("微信读书请求地址无效");
            return NULL;
        }
    }
    /*
     * 上报线程和 worker 共用这一池化通路，但每轮任务结束都会清空 s_cookies；上报线程
     * 自己从不恢复会话，于是 getProgress 全部变成"无 Cookie"请求，服务端回 -2010，
     * 时长上报永远卡在 retry。流式通路早就在 URL 以 / 开头时补恢复，这里必须一致。
     * The report thread shares this pooled path, yet every worker round wipes s_cookies and
     * the reporter never restores it, so getProgress went out without cookies, the server
     * answered -2010, and duration reporting retried forever. The streaming path already
     * restores for service URLs; this one has to match.
     */
    const bool carry_cookie = url_needs_cookies(url);
    if (carry_cookie) restore_session_cookie();
    cookie_lock();
    memcpy(request->cookie_snapshot, s_cookies, sizeof(request->cookie_snapshot));
    request->cookie_snapshot[sizeof(request->cookie_snapshot)-1] = 0;
    cookie_unlock();
    /* 服务端对"没有 Cookie"的请求回 errCode=-2010 用户不存在，和"Cookie 失效"(-2012) 不同，
     * 二者在界面上都只显示成模糊的失败；这里把真正的差别记下来。
     * A request without cookies is answered with errCode -2010, unlike an expired cookie (-2012).
     * Both surface as a vague failure, so record which one actually happened. */
    if (!request->cookie_snapshot[0]) ESP_LOGW("WEREAD", "request without cookies url=%s", url);

    char request_host[128] = {0};
    bool reused = http_url_host(absolute, request_host) && s_http_keepalive &&
        !strcasecmp(request_host, s_http_keepalive_host);
    esp_http_client_handle_t http = http_keepalive_get(absolute, request);
    if (!http) { wr_error_set("无法创建网络请求"); http_keepalive_drop(); return NULL; }
    esp_http_client_delete_all_headers(http);
    /* delete_all_headers 会把 IDF 在 set_url 里补的 Host 一并删掉，而 set_url 只在主机变化时
     * 才补回；缺少 Host 的请求会被腾讯网关（stgw）直接判 400。实测复用通路的 POST 全因此失败。
     * delete_all_headers also removes the Host header IDF adds in set_url, and set_url only
     * restores it when the host changes. A request without Host is rejected with 400 by the
     * gateway — this is why every pooled POST failed while the fallback path succeeded. */
    if (request_host[0]) esp_http_client_set_header(http, "Host", request_host);
    esp_http_client_reset_redirect_counter(http);
    esp_http_client_set_method(http, body ? HTTP_METHOD_POST : HTTP_METHOD_GET);
    /* 只有 GET 才请求 gzip。实测 POST 带 Accept-Encoding: gzip 会被网关直接回 400 HTML
     * （/web/book/chapterInfos 必现），而发布版用的是 identity；POST 一律回到 identity。
     * Only GETs ask for gzip: POSTs carrying Accept-Encoding: gzip are rejected by the gateway
     * with a 400 HTML page (/web/book/chapterInfos reproduces this every time). */
    esp_http_client_set_header(http, "Accept-Encoding", (service && !body) ? "gzip" : "identity");
    esp_http_client_set_header(http, "Referer", referer ? referer : WR_HOST "/");
    if (service) {
        esp_http_client_set_header(http, "Accept", "application/json, text/plain, */*");
        esp_http_client_set_header(http, "Origin", WR_HOST);
    }
    if (carry_cookie && request->cookie_snapshot[0])
        esp_http_client_set_header(http, "Cookie", request->cookie_snapshot);
    int body_size = body ? (int)strlen(body) : 0;
    if (body) esp_http_client_set_header(http, "Content-Type", "application/json;charset=UTF-8");
    /* body 为空时这一步只是清掉上一轮 POST 残留的 post_data/post_len；它内部会顺手删除
     * Content-Type，头不存在就返回 ESP_ERR_NOT_FOUND。状态在返回错误前已经清好，因此
     * 只有真正带 body 时才把失败当致命错误——否则每个 GET 都会在"准备请求"处失败。
     * With no body this call only clears leftover post_data/post_len; it also deletes the
     * Content-Type header and reports NOT_FOUND when absent. The state is already cleared by
     * then, so only a real body makes the failure fatal — otherwise every GET dies here. */
    esp_err_t post = esp_http_client_set_post_field(http, body, body_size);
    if (post != ESP_OK && body) {
        wr_error_set("无法准备微信读书请求");
        http_keepalive_drop();
        return NULL;
    }
    int64_t started = esp_timer_get_time();
    int code = 0;
    for (;;) {
        request->location[0] = 0;
        free(request->body_data);
        request->body_data = NULL;
        request->body_used = request->body_capacity = 0;
        request->body_overflow = false;
        snprintf(request->current, sizeof(request->current), "%s", absolute);
        if (wr_cancelled()) { wr_error_set("操作已取消"); http_keepalive_drop(); return NULL; }
        esp_err_t result = esp_http_client_perform(http);
        if (result != ESP_OK) {
            int tls_code = 0, tls_flags = 0;
            esp_err_t tls_error = esp_http_client_get_and_clear_last_tls_error(http, &tls_code, &tls_flags);
            if (request->body_overflow) wr_error_set("微信读书响应超出允许大小");
            else if (wr_cancelled()) wr_error_set("操作已取消");
            else if (tls_error != ESP_OK || result == ESP_ERR_HTTP_CONNECT) {
                s_http_open_failed = true;
                wr_error_set("微信读书网络连接失败，请检查 Wi-Fi 后重试");
            } else wr_error_set("读取微信读书响应失败");
            ESP_LOGW("WEREAD", "HTTP perform failed url=%s err=%s tls=%s code=%d flags=0x%x bytes=%u",
                     url, esp_err_to_name(result), esp_err_to_name(tls_error), tls_code,
                     (unsigned)tls_flags, (unsigned)request->body_used);
            free(request->body_data);
            request->body_data = NULL;
            http_keepalive_drop();
            return NULL;
        }
        code = esp_http_client_get_status_code(http);
        request->status_code = code;
        s_last_http_status = code;
        if (code == 401 || code == 403) {
            if (service) { s_expired = true; wr_error_set("登录已失效，请重新授权"); }
            else wr_error_set("微信读书图片请求未获授权");
        } else if ((code < 200 || code >= 300) && code != 301 && code != 302 && code != 303 &&
                   code != 307 && code != 308) {
            wr_error_set("微信读书接口返回错误");
            /* 非 2xx 以前只改文案不记日志，线上排查只能猜阶段；把状态码、请求和响应摘要打出来。
             * Non-2xx used to change only the message, leaving no trace in the log. */
            char sample[201] = {0};
            if (request->body_data) {
                size_t take = request->body_used < sizeof(sample) - 1 ? request->body_used : sizeof(sample) - 1;
                memcpy(sample, request->body_data, take);
            }
            ESP_LOGW("WEREAD", "HTTP error url=%s code=%d reused=%d req=%d bytes=%u body=%s", absolute,
                     code, (int)reused, body_size, (unsigned)request->body_used, sample);
        }
        /* 重定向不在这里跟随：复用句柄二次 perform 会继承上一跳的连接状态。
         * Redirects fall back to the non-reused path, which owns the redirect loop. */
        if (code == 301 || code == 302 || code == 303 || code == 307 || code == 308) {
            wr_error_set("微信读书接口返回重定向");
            ESP_LOGI("WEREAD", "keepalive redirect url=%s code=%d -> fresh connection", url, code);
            free(request->body_data);
            request->body_data = NULL;
            request->body_used = request->body_capacity = 0;
            http_keepalive_drop();
            return NULL;
        }
        break;
    }
    if (code != 200 || !request->body_data) {
        if (code == 200 && !request->body_data && request->body_used == 0) {
            request->body_data = heap_caps_malloc(1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        }
        if (code != 200 || !request->body_data) {
            free(request->body_data);
            request->body_data = NULL;
            http_keepalive_drop();
            return NULL;
        }
    }
    if (!gunzip_body(request)) {
        /* 解压失败说明服务端压缩方式与预期不符：断开并退回不复用路径。
         * Decompression failed: drop the connection and let the caller retry. */
        wr_error_set("微信读书响应解压失败");
        free(request->body_data);
        request->body_data = NULL;
        http_keepalive_drop();
        return NULL;
    }
    request->body_data[request->body_used] = 0;
    if (length) *length = request->body_used;
    ESP_LOGI("WEREAD", "HTTP complete url=%s bytes=%u elapsed_ms=%lld connection_reused=%d",
             request->current, (unsigned)request->body_used,
             (long long)((esp_timer_get_time() - started) / 1000), reused);
    char *result = request->body_data;
    request->body_data = NULL;
    return result;
}

static void restore_session_cookie(void) {
    cookie_lock();
    if (s_cookies[0]) { cookie_unlock(); return; }
    nvs_handle_t nvs;
    if (nvs_open("weread", NVS_READONLY, &nvs) == ESP_OK) {
        size_t size = sizeof(s_cookies);
        if (nvs_get_str(nvs, "cookies", s_cookies, &size) == ESP_OK && s_cookies[0]) {
            nvs_close(nvs);
            cookie_unlock();
            return;
        }
        nvs_close(nvs);
    }
    wr_account_t account;
    if (!read_account(&account) || !account.owner[0] || !account.token[0]) { cookie_unlock(); return; }
    /* CrossMux persists the same three WeRead cookies across reboot.  QR login
     * gives us the vid and skey; rebuild the durable pair before shelf calls. */
    snprintf(s_cookies, sizeof(s_cookies), "wr_vid=%s; wr_skey=%s", account.owner, account.token);
    cookie_unlock();
}

/*
 * 哪些地址属于"账号级"、必须带 Cookie：
 *  · weread.qq.com 的服务端接口（用 "/path" 形式传入）；
 *  · 章包 res.weread.qq.com/wrco/tar_*——缺 Cookie 一律 401，逐章缓存只能退化成
 *    "每张插图一次请求"，实测 20 秒一张、图文单章 50–75 秒。
 * 只对这两个自家域放行；302 之后拿到的 CDN 主机不会带 Cookie。
 * Which URLs are account-scoped and must carry cookies: the weread.qq.com service API (passed
 * as "/path") and chapter bundles at res.weread.qq.com/wrco/tar_*, which answer 401 without them
 * and force per-chapter caching down to one request per figure (20 s each here, 50–75 s per
 * illustrated chapter). Only those two own hosts qualify; the CDN host reached after a redirect
 * never sees the cookie.
 */
static bool url_needs_cookies(const char *url) {
    if (!url) return false;
    if (url[0] == '/') return true;
    static const char bundle[] = "https://res.weread.qq.com/wrco/";
    return !strncasecmp(url, bundle, sizeof(bundle) - 1);
}

static bool wr_url_allowed(const char *url, bool service) {    if (!url || strncmp(url,"https://",8)) return false;
    const char *host=url+8, *end=strpbrk(host,"/?#");
    if (!end) end=host+strlen(host);
    const char *port = memchr(host, ':', (size_t)(end-host));
    if (port) {
        if (end-port != 4 || memcmp(port, ":443", 4)) return false;
        end = port;
    }
    size_t n=(size_t)(end-host);
    if (!n || n>=128) return false;
    for (const char *p=host;p<end;++p)
        if (!isalnum((unsigned char)*p) && *p!='.' && *p!='-') return false;
    if (service) return n==strlen("weread.qq.com") && !strncasecmp(host,"weread.qq.com",n);
    static const char *const suffixes[]={".qq.com",".qpic.cn",".qlogo.cn",".myqcloud.com",".gfp.tencent-cloud.com"};
    for (size_t i=0;i<sizeof(suffixes)/sizeof(*suffixes);++i) {
        size_t suffix=strlen(suffixes[i]);
        if (n>suffix && !strncasecmp(end-suffix,suffixes[i],suffix)) return true;
    }
    return false;
}

static void log_rejected_image_url(const char *url) {
    const char *scheme = url ? strstr(url, "://") : NULL;
    const char *host = scheme ? scheme + 3 : (url ? url : "");
    const char *end = strpbrk(host, "/?#");
    if (!end) end = host + strlen(host);
    char safe_host[128];
    size_t length = (size_t)(end - host);
    if (length >= sizeof(safe_host)) length = sizeof(safe_host) - 1;
    for (size_t i = 0; i < length; ++i) {
        unsigned char c = (unsigned char)host[i];
        safe_host[i] = (isalnum(c) || c == '.' || c == '-' || c == ':') ? (char)c : '?';
    }
    safe_host[length] = 0;
    ESP_LOGW("WEREAD", "image URL rejected scheme=%s host=%s length=%u",
             scheme ? (size_t)(scheme - url) == 5 ? "https" : (size_t)(scheme - url) == 4 ? "http" : "other" : "none",
             safe_host, (unsigned)(url ? strlen(url) : 0));
}

/* Resolve WeRead CDN redirects ourselves. ESP-IDF rejects HTTPS -> HTTP
 * redirects before callers can normalize them, while WeRead sometimes emits
 * exactly that URL form. Upgrade it to HTTPS, then apply the host allowlist. */
static bool cover_redirect_url(const char *current, const char *location,
                               char out[WR_HTTP_URL_BYTES]) {
    if (!current || !location || !*location) return false;
    char resolved[WR_HTTP_URL_BYTES];
    int n;
    if (!strncasecmp(location,"https://",8) || !strncasecmp(location,"http://",7)) {
        n=snprintf(resolved,sizeof(resolved),"%s",location);
    } else if (!strncmp(location,"//",2)) {
        n=snprintf(resolved,sizeof(resolved),"https:%s",location);
    } else {
        const char *authority_end=strpbrk(current+8,"/?#");
        if (!authority_end) authority_end=current+strlen(current);
        if (location[0]=='/') {
            n=snprintf(resolved,sizeof(resolved),"%.*s%s",(int)(authority_end-current),current,location);
        } else {
            const char *last=strrchr(current,'/');
            if (!last || last<authority_end) last=authority_end;
            n=snprintf(resolved,sizeof(resolved),"%.*s/%s",(int)(last-current),current,location);
        }
    }
    if (n<0 || (size_t)n>=sizeof(resolved)) return false;
    if (!strncmp(resolved,"http://",7)) {
        char *authority_end=strpbrk(resolved+7,"/?#");
        if (!authority_end) authority_end=resolved+strlen(resolved);
        char authority[256];
        size_t host_n=(size_t)(authority_end-(resolved+7));
        if (!host_n || host_n>=sizeof(authority)) return false;
        memcpy(authority,resolved+7,host_n); authority[host_n]=0;
        size_t keep=host_n;
        if (keep>3 && !strcmp(authority+keep-3,":80")) { keep-=3; authority[keep]=0; }
        n=snprintf(out,WR_HTTP_URL_BYTES,"https://%s%s",authority,authority_end);
    } else n=snprintf(out,WR_HTTP_URL_BYTES,"%s",resolved);
    return n>0 && (size_t)n<WR_HTTP_URL_BYTES;
}

void wr_reading_display(bool scanning) {
    int64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&s_lock);
    s_reader_scanning = scanning;
    if (!scanning) s_reader_scan_end = now;
    portEXIT_CRITICAL(&s_lock);
}

static bool report_display_busy(void) {
    portENTER_CRITICAL(&s_lock);
    bool busy = s_reader_scanning;
    portEXIT_CRITICAL(&s_lock);
    return busy;
}

static char *http_request_stream(const char *url, const char *body, const char *referer, size_t limit,
                                 size_t *length, wr_http_chunk_fn chunk, void *context,
                                 bool *fields_received, bool *post_started) {
    const int64_t request_started_us = esp_timer_get_time();
    s_http_open_failed = false;
    if (wr_cancelled()) { s_error_message = "操作已取消"; return NULL; }
    if (!wr_online()) { http_keepalive_drop(); s_error_message = "Wi-Fi 未连接或网络尚未就绪"; return NULL; }
    /*
     * 建 TLS 前先看内部 RAM。前一次改动的教训：这里一旦"余量不够就不发请求"，在这块板上
     * 就是永久失败——实测 worker 起时只有 25 KB，44 KB 的门槛把所有章包和上报都拒了。
     * 所以这里只做一件事：真的接近枯竭时丢掉复用连接（立刻还回一套 TLS 缓冲），让出一下，
     * 然后照常发请求；是否成功交给真正的网络结果去说。
     * Check internal RAM before opening a TLS session. The lesson from the previous attempt:
     * gating on headroom means permanent failure on this board — the worker starts at 25 KB, and
     * a 44 KB gate rejected every bundle and every report. So the only action here is dropping the
     * pooled connection (returning one TLS buffer set) when the margin is truly thin, yielding
     * briefly, and then proceeding; the network result decides success.
     */
    const size_t internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (internal < WR_TLS_MIN_INTERNAL) {
        ESP_LOGW("WEREAD", "low internal heap %u KB before TLS url=%s", (unsigned)(internal / 1024), url);
        http_keepalive_drop();
        vTaskDelay(pdMS_TO_TICKS(60));
    }
    if (url_needs_cookies(url)) restore_session_cookie();
    wr_http_context_t *request = heap_caps_calloc(1, sizeof(*request), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!request) { s_error_message = "内存不足，无法创建微信读书请求"; return NULL; }
    char *absolute = request->absolute;
    const bool service = url[0] == '/';
    request->capture_service_cookies = service;
    bool upgrade = !service && !strncmp(url, "http://", 7);
    const char *prefix = service ? WR_HOST : upgrade ? "https://" : "";
    int url_size = snprintf(absolute, WR_HTTP_URL_BYTES, "%s%s", prefix, upgrade ? url + 7 : url);
    if (url_size < 0 || url_size >= WR_HTTP_URL_BYTES) {
        s_error_message = "微信读书请求地址过长";
        free(request);
        return NULL;
    }
    if (!service && !wr_url_allowed(absolute, false)) {
        log_rejected_image_url(absolute);
        s_error_message="微信读书图片地址不受支持";
        free(request);
        return NULL;
    }
    for (const char *p = absolute; *p; ++p) if ((unsigned char)*p <= ' ' || *p == '\\') { free(request); return NULL; }
    cookie_lock();
    memcpy(request->cookie_snapshot, s_cookies, sizeof(request->cookie_snapshot));
    request->cookie_snapshot[sizeof(request->cookie_snapshot)-1] = 0;
    cookie_unlock();
    /* 服务端对"没有 Cookie"的请求回 errCode=-2010 用户不存在，和"Cookie 失效"(-2012) 不同，
     * 二者在界面上都只显示成模糊的失败；这里把真正的差别记下来。
     * A request without cookies is answered with errCode -2010, unlike an expired cookie (-2012).
     * Both surface as a vague failure, so record which one actually happened. */
    if (!request->cookie_snapshot[0]) ESP_LOGW("WEREAD", "request without cookies url=%s", url);
    /* Keep the proven open/fetch/read path for complete responses. The experimental
     * esp_http_client_perform() keep-alive path returned non-2xx during a real
     * WeRead cache request; do not reuse it until the server response is diagnosed. */
    http_keepalive_drop();
    esp_http_client_config_t config = {
        /* Keep the HTTP/TLS working set modest on S31: the panel and Wi-Fi
         * driver need internal RAM while mbedTLS allocates AES/GCM contexts. */
        .url = absolute, .timeout_ms = 15000, .buffer_size = WR_HTTP_BUFFER, .buffer_size_tx = 2048,
        /* open/fetch_headers/read is the streaming API; it needs an explicit
         * redirect loop because perform() is not used here. */
        .crt_bundle_attach = esp_crt_bundle_attach, .disable_auto_redirect = true,
        .user_agent = WR_AGENT, .event_handler = capture_cookie, .user_data = request,
    };
    esp_http_client_handle_t http = esp_http_client_init(&config);
    if (!http) { s_error_message = "无法创建网络请求"; free(request); return NULL; }
    esp_http_client_set_header(http, "Accept-Encoding", "identity");
    esp_http_client_set_header(http, "Referer", referer ? referer : WR_HOST "/");
    if (service) {
        esp_http_client_set_header(http,"Accept","application/json, text/plain, */*");
        esp_http_client_set_header(http,"Origin",WR_HOST);
    }
    // 章包同样要带 Cookie，否则 401 后只能逐张下载插图。/ Bundles need the cookie too.
    if (url_needs_cookies(url) && request->cookie_snapshot[0])
        esp_http_client_set_header(http,"Cookie",request->cookie_snapshot);
    if (body) {
        esp_http_client_set_method(http, HTTP_METHOD_POST);
        esp_http_client_set_header(http, "Content-Type", "application/json;charset=UTF-8");
    }
    char *data = NULL;
    size_t used = 0, capacity = 0;
    int64_t scan_wait_until = 0;
    int64_t reader_deadline = chunk && s_processing_report ? esp_timer_get_time() + 90000000 : 0;
    bool complete = false;
    bool allocation_failed = false, read_failed = false;
    /*
     * esp_http_client_read() 返回 0 代表"这一次 15 秒没等到数据"，不是对端结束——把两者当成
     * 一回事，任何一次链路抖动都会被判成整个响应失败。实测章包 370 KB 下到 180 KB 时抖一下，
     * 于是每次都从 0 重来，永远下不完，插图也就永远缺席。这里只把 0 当作"再等一轮"，
     * 连续几轮都没有字节才真的放弃。
     * A zero from esp_http_client_read() means "no bytes within this 15 s window", not EOF.
     * Treating it as EOF turned a single link hiccup into a failed response: the measured 370 KB
     * bundle died at 180 KB every attempt and never finished, so its illustrations never landed.
     * Zero now just means "wait another window"; only a run of empty windows gives up.
     */
    unsigned empty_reads = 0;
    bool budget_hit = false;
    // 提前声明：open 失败会 goto cleanup，那里要读 code 决定提示语。
    // Declared up front: the open failure jumps to cleanup, which reads code to pick a message.
    int code = 0;
    int body_size = body ? (int)strlen(body) : 0;
    int64_t declared = -1;
    snprintf(request->current, sizeof(request->current), "%s", absolute);
    for (unsigned redirects = 0;;) {
        request->location[0]=0;
        int64_t open_start = esp_timer_get_time();
        esp_err_t open_error = esp_http_client_open(http, body_size);
        if (open_error != ESP_OK) {
            s_http_open_failed = true;
            s_error_message = "微信读书网络连接失败，请检查 Wi-Fi 后重试";
            int tls_code = 0, tls_flags = 0;
            esp_err_t tls_error = esp_http_client_get_and_clear_last_tls_error(http, &tls_code, &tls_flags);
            ESP_LOGW("WEREAD", "HTTP open failed url=%s err=%s ms=%lld tls=%s code=%d flags=0x%x internal=%u largest=%u",
                     url, esp_err_to_name(open_error), (long long)((esp_timer_get_time()-open_start)/1000),
                     esp_err_to_name(tls_error), tls_code, (unsigned)tls_flags,
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                     (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
            goto cleanup;
        }
        if (post_started && body_size) *post_started = true;
        for (int sent = 0; sent < body_size;) {
            int n = esp_http_client_write(http, body + sent, body_size - sent);
            if (n <= 0) { s_error_message = "网络请求发送失败"; goto cleanup; }
            sent += n;
        }
        declared = esp_http_client_fetch_headers(http);
        code = esp_http_client_get_status_code(http);
        ESP_LOGI("WEREAD", "HTTP %s code=%d declared=%lld chunked=%d", url, code, (long long)declared,
                 (int)esp_http_client_is_chunked_response(http));
        bool redirect = code == 301 || code == 302 || code == 303 || code == 307 || code == 308;
        char *redirected = request->redirected;
        if (!redirect) break;
        if (redirects >= 5) { s_error_message="微信读书封面重定向次数过多"; goto cleanup; }
        if (!service) {
            if (!cover_redirect_url(request->current,request->location,redirected)) {
                ESP_LOGW("WEREAD","cover redirect invalid location='%s'",request->location);
                s_error_message = "微信读书图片重定向地址不受支持";
                goto cleanup;
            }
            if (!wr_url_allowed(redirected,false) || esp_http_client_set_url(http,redirected)!=ESP_OK) {
                ESP_LOGW("WEREAD","cover redirect target rejected location='%s' target='%s'",request->location,redirected);
                s_error_message = "微信读书图片重定向地址不受支持";
                goto cleanup;
            }
        } else {
            if (esp_http_client_set_redirection(http) != ESP_OK ||
                esp_http_client_get_url(http,redirected,WR_HTTP_URL_BYTES) != ESP_OK) {
                s_error_message="微信读书接口重定向失败";
                goto cleanup;
            }
        }
        if (!wr_url_allowed(redirected, service)) {
            ESP_LOGW("WEREAD","redirect target rejected service=%d target='%s'",service,redirected);
            s_error_message = "微信读书图片重定向地址不受支持";
            goto cleanup;
        }
        ESP_LOGI("WEREAD", "HTTP redirect %s -> %s", url, redirected);
        snprintf(request->current,sizeof(request->current),"%s",redirected);
        if (code == 301 || code == 302 || code == 303) {
            body_size = 0;
            esp_http_client_set_method(http, HTTP_METHOD_GET);
        }
        esp_http_client_close(http);
        ++redirects;
    }
    if (service && (code == 401 || code == 403)) { s_expired = true; s_error_message = "登录已失效，请重新授权"; }
    else if (code < 200 || code >= 300) s_error_message = "微信读书接口返回错误";
    /*
     * 把状态码留下来：插图那一步需要区分"服务端说这张图不存在"（404/410，装饰图标就是这种）
     * 和"这次没取到"（超时、断网）。前者该把标签直接丢掉，后者要留占位、等下一轮补。
     * Keep the status code: the illustration step must tell "the server says this picture does not
     * exist" (404/410 — what the reader's own UI ornaments return) from "this attempt failed"
     * (timeout, offline). The first means drop the tag, the second means keep a retryable placeholder.
     */
    s_last_http_status = code;
    if ((declared < 0 && !esp_http_client_is_chunked_response(http)) ||
        (declared > 0 && (uint64_t)declared > limit) || code != 200) goto cleanup;
    if (declared < 0) declared = 0;
    while (!wr_cancelled()) {
        if (s_stream_deadline_us && esp_timer_get_time() >= s_stream_deadline_us) {
            budget_hit = true;
            goto cleanup;
        }
        if (chunk) {
            if (reader_deadline && esp_timer_get_time() >= reader_deadline) {
                ESP_LOGW("WEREAD", "report reader response deadline bytes=%u", (unsigned)used);
                goto cleanup;
            }
            if (s_processing_report && report_display_busy()) {
                if (!scan_wait_until) scan_wait_until = esp_timer_get_time() + 3000000;
                if (esp_timer_get_time() >= scan_wait_until) {
                    ESP_LOGI("WEREAD", "reader fields deferred during display scan bytes=%u", (unsigned)used);
                    goto cleanup;
                }
                vTaskDelay(pdMS_TO_TICKS(20));
                continue;
            }
            scan_wait_until = 0;
            if (!data) {
                /* 读块与客户端缓冲同尺寸：一次 read 吃完一整个 4 KB 缓冲，避免每 2 KB
                 * 一次系统调用把 900 KB 的阅读页拖成 30 秒。尺寸必须与扫描窗口一致。
                 * Read in whole client-buffer units: one 4 KB read instead of a syscall every
                 * 2 KB, which is what stretches a 900 KB reader page into thirty seconds. The
                 * size must match the scan window. */
                data = heap_caps_malloc(WR_FIELD_CHUNK, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
                if (!data) { allocation_failed = true; goto cleanup; }
            }
            size_t remaining = limit - used;
            if (!remaining) break;
            int n = esp_http_client_read(http, data, (int)(remaining < WR_FIELD_CHUNK ? remaining : WR_FIELD_CHUNK));
            /* 读超时是 -ESP_ERR_HTTP_EAGAIN（ridx=0 且 15 秒没等到字节），不是链路故障。 */
            /* A read timeout arrives as -ESP_ERR_HTTP_EAGAIN (no byte within 15 s), not a fault. */
            if (n == -ESP_ERR_HTTP_EAGAIN) n = 0;
            if (n < 0) { read_failed = true; goto cleanup; }
            if (!n) {
                if (esp_http_client_is_complete_data_received(http)) { complete = true; break; }
                if (declared > 0 && used >= (size_t)declared) { complete = true; break; }
                if (++empty_reads >= WR_EMPTY_READ_LIMIT) {
                    ESP_LOGW("WEREAD", "read stalled url=%s bytes=%u declared=%lld", url, (unsigned)used,
                             (long long)declared);
                    goto cleanup;
                }
                continue;
            }
            empty_reads = 0;
            used += (size_t)n;
            wr_warm_bytes((unsigned)used, (unsigned)(declared > 0 ? declared : 0));
            if (chunk(data, (size_t)n, context)) {
                *fields_received = true;
                break;
            }
            if (esp_http_client_is_complete_data_received(http)) { complete = true; break; }
            if (s_processing_report) vTaskDelay(1);
            continue;
        }
        if (used == capacity) {
            if (used == limit) { complete = esp_http_client_is_complete_data_received(http); break; }
            size_t next = capacity ? capacity * 2 : declared ? (size_t)declared : 4096;
            if (next > limit) next = limit;
            char *grown = heap_caps_realloc(data, next + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (!grown) { allocation_failed = true; goto cleanup; }
            data = grown;
            capacity = next;
        }
        int n = esp_http_client_read(http, data + used, (int)(capacity - used));
        if (n == -ESP_ERR_HTTP_EAGAIN) n = 0;
        if (n < 0) { read_failed = true; goto cleanup; }
        if (!n) {
            if (esp_http_client_is_complete_data_received(http)) { complete = true; break; }
            if (declared > 0 && used >= (size_t)declared) { complete = true; break; }
            if (++empty_reads >= WR_EMPTY_READ_LIMIT) {
                ESP_LOGW("WEREAD", "read stalled url=%s bytes=%u declared=%lld", url, (unsigned)used,
                         (long long)declared);
                goto cleanup;
            }
            continue;
        }
        empty_reads = 0;
        used += (size_t)n;
        wr_warm_bytes((unsigned)used, (unsigned)(declared > 0 ? declared : 0));
        if (esp_http_client_is_complete_data_received(http)) { complete = true; break; }
    }
cleanup:
    esp_http_client_cleanup(http);
    free(request);
    if (chunk && (complete || *fields_received) && !wr_cancelled()) {
        *fields_received = true;
        ESP_LOGI("WEREAD", "reader HTML bytes=%u full_response=%d elapsed_ms=%lld", (unsigned)used,
                 complete,
                 (long long)((esp_timer_get_time() - request_started_us) / 1000));
        free(data);
        return NULL;
    }
    if (!complete || !data || wr_cancelled()) {
        /* 预算到点：把已读到的字节交出去。章包是顺序 tar，截断后前面的条目仍然有效；
         * 调用方按 length 使用，不当作完整响应。 */
        /* Budget expired: hand back what arrived. A chapter bundle is a sequential tar, so the
         * entries before the cut remain valid; the caller uses length and knows it is partial. */
        if (budget_hit && s_stream_partial && !chunk && data && used > 1024 && !wr_cancelled()) {
            ESP_LOGW("WEREAD", "HTTP budget reached url=%s bytes=%u declared=%lld, keeping partial",
                     url, (unsigned)used, (long long)declared);
            if (length) *length = used;
            return data;
        }
        if (allocation_failed) s_error_message = "内存不足，无法接收微信读书响应";
        else if (read_failed) s_error_message = "读取微信读书响应失败";
        else if (wr_cancelled()) s_error_message = "操作已取消";
        else if (code != 200) { /* Preserve the more specific HTTP/authentication message. */ }
        else s_error_message = "微信读书响应未完整接收";
        ESP_LOGW("WEREAD", "HTTP read failed url=%s complete=%d bytes=%u", url, (int)complete, (unsigned)used);
        free(data); return NULL;
    }
    data[used] = 0;
    ESP_LOGI("WEREAD", "HTTP complete url=%s bytes=%u elapsed_ms=%lld", url, (unsigned)used,
             (long long)((esp_timer_get_time()-request_started_us)/1000));
    if (length) *length = used;
    return data;
}

/* 整包响应优先走连接复用；失败时退回原有的 open/fetch/read 路径，保证不会因为
 * 连接池复用而让缓存请求整体失败。分块读取（阅读页字段）和进度上报仍走原路径。
 * Complete responses try the pooled connection first and fall back to the proven
 * open/fetch/read path, so a keep-alive regression can never break caching. Chunked
 * field scans and duration reports keep using the original path. */
static char *http_request(const char *url, const char *body, const char *referer, size_t limit,
                          size_t *length, wr_http_chunk_fn chunk, void *context,
                          bool *fields_received, bool *post_started) {
    http_lock();
    char *result = NULL;
    /* 图片等外链几乎都会 302 到 CDN：复用通路遇到重定向只能丢弃连接再重来，等于每张图
     * 多花一次建连。外链直接交给会自行跟随重定向的老通路。
     * External URLs (images) almost always 302 to a CDN; the pooled path can only drop the
     * connection and start over on a redirect, so send them straight to the redirect-following path. */
    const bool external = url[0] != '/';
    const bool complete = !chunk && !post_started && !external;
    if (complete) {
        wr_http_context_t *request = heap_caps_calloc(1, sizeof(*request), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (request) {
            char *data = http_request_reusable(request, url, body, referer, limit, length, NULL);
            const int status = request->status_code;
            free(request);
            /* 记下这条连接空闲起点，供下一次复用前判断是否已经被服务端关掉。 */
            /* Note when the pooled connection went idle, so the next reuse can tell whether the
             * server has already closed it. */
            s_http_keepalive_used_us = esp_timer_get_time();
            if (data) { result = data; goto out; }
            if (wr_cancelled() || s_expired) goto out;
            /* 网关回 4xx 时也要回退：复用通路会把 POST 打成 400（/web/book/chapterInfos 实测必现），
             * 而老通路对同一请求是通的。这里只承接整包读取，都是幂等查询，重试安全。
             * Fall back on gateway 4xx too: the pooled path turns some POSTs into 400s while the
             * original path succeeds for the same request. Only idempotent queries reach here. */
            ESP_LOGW("WEREAD", "pooled request failed url=%s status=%d error=%s; retrying on a fresh connection",
                     url, status, wr_error_message());
            http_keepalive_drop();
        }
    }
    result = http_request_stream(url, body, referer, limit, length, chunk, context, fields_received, post_started);
out:
    http_unlock();
    return result;
}

char *wr_http(const char *url, const char *body, const char *referer, size_t limit, size_t *length) {
    s_last_http_status = 0;
    char *data = http_request(url, body, referer, limit, length, NULL, NULL, NULL, NULL);
    /* 预算是"下一次请求"的一次性设置，用完即弃，不能漏到后面的请求上。 */
    /* The budget is a one-shot setting for the next request; never leak it into later ones. */
    s_stream_deadline_us = 0;
    s_stream_partial = false;
    return data;
}

char *wr_http_report(const char *body, const char *referer, bool *post_started) {
    return http_request("/web/book/read", body, referer, 16384, NULL, NULL, NULL, NULL, post_started);
}

static void *json_alloc(size_t size) { return heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT); }

/* JSON 里的"成功"标记：true / 1 / "1" / "true" 都算。
 * A success flag in JSON: true, 1, "1" and "true" all count. */
static bool json_flag_true(const cJSON *field) {
    return cJSON_IsTrue(field) || (cJSON_IsNumber(field) && field->valueint == 1) ||
           (cJSON_IsString(field) && (!strcmp(field->valuestring, "1") ||
                                      !strcmp(field->valuestring, "true")));
}

static bool web_renewal_attempt(void) {
    if (!session_cookie_has("wr_rt")) {
        ESP_LOGW("WEREAD", "session renewal unavailable refresh_cookie=0");
        return false;
    }
    static const char body[] = "{\"rq\":\"%2Fweb%2Fbook%2Fread\",\"ql\":false}";
    ESP_LOGI("WEREAD", "session renewal start refresh_cookie=1");
    char *raw = wr_http("/web/login/renewal", body, NULL, 16384, NULL);
    if (!raw) {
        ESP_LOGW("WEREAD", "session renewal network/http failure expired=%d", s_expired);
        return false;
    }
    cJSON *json = cJSON_ParseWithOpts(raw, NULL, true);
    /* 这个应答很小（成功就 10 字节），原样留一行，省得下次再靠猜。 */
    /* The answer is tiny (ten bytes on success); keep one line of it instead of guessing. */
    ESP_LOGI("WEREAD", "session renewal body=%.120s", raw);
    free(raw);
    if (!json) {
        ESP_LOGW("WEREAD", "session renewal response parse failed");
        return false;
    }
    const cJSON *data = cJSON_GetObjectItemCaseSensitive(json, "data");
    if (!cJSON_IsObject(data)) data = json;
    /*
     * 服务端这个接口用 succ，不是 succeed：实测成功应答就是 {"succ":1}（10 字节）。
     * 以前只认 succeed，于是每一次续期都被判成失败，进而把 token 抹掉、界面报"登录已失效"——
     * 掉线其实是自己判出来的。两个字段都认，且判失败时保留原凭据，不再擦掉登录态。
     * The endpoint answers with succ, not succeed: a successful renewal is exactly {"succ":1}
     * (ten bytes, measured). Accepting only succeed made every renewal look rejected, which then
     * erased the token and reported "login expired" — the drop was self-inflicted. Accept both
     * and never discard credentials on an inconclusive answer.
     */
    const cJSON *success = cJSON_GetObjectItemCaseSensitive(data, "succeed");
    if (!success) success = cJSON_GetObjectItemCaseSensitive(json, "succeed");
    const cJSON *succ = cJSON_GetObjectItemCaseSensitive(data, "succ");
    if (!succ) succ = cJSON_GetObjectItemCaseSensitive(json, "succ");
    const cJSON *error = cJSON_GetObjectItemCaseSensitive(json, "errcode");
    if (!error) error = cJSON_GetObjectItemCaseSensitive(json, "errCode");
    const bool no_error = !error || (cJSON_IsNumber(error) && error->valueint == 0) ||
        (cJSON_IsString(error) && !strcmp(error->valuestring, "0"));
    const bool flag_ok = json_flag_true(success) || json_flag_true(succ);
    bool ok = no_error && flag_ok;
    cJSON_Delete(json);
    if (!ok) {
        s_expired = true;
        ESP_LOGW("WEREAD", "session renewal rejected");
        return false;
    }
    char token[sizeof(s_http_account.token)];
    if (s_http_account.token[0] && session_cookie_copy("wr_skey", token, sizeof(token)) &&
        strcmp(s_http_account.token, token)) {
        memcpy(s_http_account.token, token, sizeof(s_http_account.token));
        (void)save_account(&s_http_account);
    }
    s_expired = false;
    ESP_LOGI("WEREAD", "session renewal confirmed refresh_cookie=%d", session_cookie_has("wr_rt"));
    return true;
}

/* 网页会话续期入口：先试 wr_rt cookie 续期；走不通（无 wr_rt/被拒/网络失败）时，
 * consent 是登录源，直接用 consent 重新 mint 一个全新网页会话，用户无需重新扫码。
 * Try cookie renewal first; if it cannot proceed and consent is authorized, mint a fresh
 * web session from consent so the reader never has to scan again. */
static bool renew_session(void) {
    if (web_renewal_attempt()) return true;
    if (wr_consent_authorized()) {
        ESP_LOGI("WEREAD", "renewal failed, falling back to consent mint");
        return wr_mint_web_session();
    }
    return false;
}

static cJSON *wr_json_limit(const char *url, const char *body, size_t limit) {
    for (unsigned attempt = 0; attempt < 2 && !wr_cancelled(); ++attempt) {
        char *data = wr_http(url, body, NULL, limit, NULL);
        if (!data) {
            if (attempt == 0 && s_expired && renew_session()) continue;
            return NULL;
        }
        // cJSON hooks are global; the other firmware clients can parse concurrently.
        if (strstr(url, "shelf/sync"))
            ESP_LOGI("WEREAD", "shelf body attempt=%u %.200s", attempt, data);
        cJSON *json = cJSON_ParseWithOpts(data, NULL, true);
        free(data);
        if (!json) { s_error_message = "书架数据解析失败"; return NULL; }
        const cJSON *error = cJSON_GetObjectItemCaseSensitive(json, "errcode");
        if (!error) error = cJSON_GetObjectItemCaseSensitive(json, "errCode");
        if ((cJSON_IsNumber(error) && error->valueint == -2012) ||
            (cJSON_IsString(error) && !strcmp(error->valuestring, "-2012"))) s_expired = true;
        if (attempt == 0 && s_expired && renew_session()) { cJSON_Delete(json); continue; }
        return json;
    }
    return NULL;
}

cJSON *wr_json(const char *url, const char *body) {
    return wr_json_limit(url, body, 3 * 1024 * 1024);
}

static FILE *open_shelf(const char *owner, wr_shelf_header_t *header) {
    if (!read_pico_sd_io_ready() || !owner[0]) return NULL;
    FILE *file = fopen(WR_INDEX, "rb");
    if (!file) file = fopen(WR_DIR "/shelf.prev", "rb");
    if (file && (fread(header, sizeof(*header), 1, file) != 1 ||
                 strncmp(header->owner, owner, sizeof(header->owner)) || header->count > WR_MAX_SHELF)) {
        fclose(file);
        file = NULL;
    }
    return file;
}

static bool owned_path(const char *owner, const char *id, char path[WR_PATH]) {
    if (!wr_safe(owner) || !wr_safe(id)) return false;
    wr_account_t account;
    if (!read_account(&account) || strcmp(account.owner,owner)) return false;
    // 直接落在书架根目录：放子目录会被扫描登记成「分类」，缓存完还要多点一层。
    // Land on the shelf root: a subdirectory turns into a shelf category and hides the book.
    const char *root = app_settings_books_dir();
    int n = snprintf(path, WR_PATH, "%s/wr-%08lx+%s.epub", root, (unsigned long)account.library, id);
    return n > 0 && n < WR_PATH;
}

static FILE *open_cached(const char *owner, const char *mode) {
    FILE *file = fopen(WR_CACHED,mode);
    if (!file) return NULL;
    wr_account_t account;
    uint32_t library;
    if (read_account(&account) && !strcmp(account.owner,owner) &&
        fread(&library,sizeof(library),1,file) == 1 && library == account.library) return file;
    fclose(file);
    return NULL;
}

static bool cached_record(const char *owner, const char *id) {
    FILE *file = open_cached(owner,"rb");
    if (!file) return false;
    wr_book_t item;
    bool found = false;
    for (unsigned i = 0; i < WR_MAX_SHELF && fread(&item,sizeof(item),1,file) == 1; ++i) {
        if (!strncmp(item.id,id,sizeof(item.id))) { found = true; break; }
    }
    fclose(file);
    return found;
}

static bool remember_cached(const wr_book_t *book) {
    if (cached_record(s_http_account.owner,book->id)) return true;
    FILE *file = open_cached(s_http_account.owner,"r+b");
    bool ok = true;
    if (!file) {
        file = fopen(WR_CACHED,"w+b");
        ok = file && fwrite(&s_http_account.library,sizeof(s_http_account.library),1,file) == 1;
    }
    ok = ok && !fseek(file,0,SEEK_END) && fwrite(book,sizeof(*book),1,file) == 1 && !fflush(file) && !fsync(fileno(file));
    if (file && fclose(file)) ok = false;
    return ok;
}

bool wr_book_path(const wr_book_t *book, char path[WR_PATH]) {
    char owner[WR_ID];
    wr_account(owner, NULL, NULL);
    if (!owned_path(owner, book->id, path)) return false;
    WR_SD_SCOPE(path);
    struct stat st;
    return scope.allowed && cached_record(owner,book->id) && stat(path, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0;
}

/*
 * 只算这本书在书架上的路径，不要求它已经在缓存概况里、也不要求文件此刻存在。
 * "删除本地缓存"要用的正是这个：一本书可能只剩章节缓存、书文件已被清掉，或者缓存概况
 * 因为重排丢了这一条——此时 wr_book_path 会失败，界面就报"找不到本地文件"，可它明明有缓存。
 * The book's shelf path alone: no requirement that the cache summary lists it or that the file
 * exists right now. That is exactly what "delete local cache" needs — a book may keep only its
 * chapter cache after its file was removed, or the summary may have lost the entry in a reorder,
 * and wr_book_path then fails and the UI claims there is no local file although cache remains.
 */
bool wr_book_storage(const wr_book_t *book, char path[WR_PATH]) {
    if (!book || !path) return false;
    char owner[WR_ID];
    if (!wr_account(owner, NULL, NULL) || !owner[0]) return false;
    if (!owned_path(owner, book->id, path)) return false;
    WR_SD_SCOPE(path);
    return scope.allowed;
}

/*
 * 孤儿章节缓存回收。
 * 书籍文件被删掉、只剩 stage-<账号>-<书籍> 目录的书，在微读书架上仍会被统计成"已缓存"，
 * 点开也还能读——因为章节正文确实还在 SD 卡上。旧固件删除本地书籍时不会回收这一套，
 * 已经留在地上的数据需要有人清掉；本地书架删除时的回收（wr_forget_book）只管新删除。
 * 判据最简单也最可靠：书籍文件不存在 = 这本书在本地已被删除，缓存就该收回。
 * 开机扫一次，之后每次书架同步再扫一次，覆盖"历史遗留"与"刚删完"两种情况。
 *
 * Orphan chapter-cache sweep. A book whose file is gone but whose stage-<owner>-<book>
 * directory survives still counts as "cached" on the WeRead shelf and still opens, because its
 * chapter bodies really are on the card. Older firmware did not reclaim that data when a local
 * book was deleted, so leftovers need an owner; wr_forget_book only covers new deletions. The
 * criterion is the simplest reliable one: no book file means the book was deleted locally, so
 * its cache goes. Scanned once at boot and again after each shelf sync, covering both leftovers
 * and fresh deletions.
 */
static unsigned collect_orphan_cache(void) {
    char owner[WR_ID] = {0};
    if (!wr_account(owner, NULL, NULL) || !owner[0]) return 0;
    DIR *dir = opendir(WR_DIR);
    if (!dir) return 0;
    char prefix[WR_ID + 8];
    int plen = snprintf(prefix, sizeof(prefix), "stage-%s-", owner);
    unsigned dropped = 0;
    struct dirent *entry;
    while (plen > 0 && (entry = readdir(dir))) {
        if (strncmp(entry->d_name, prefix, (size_t)plen)) continue;
        const char *book = entry->d_name + plen;
        if (!wr_safe(book)) continue;
        char path[WR_PATH];
        struct stat info;
        if (!owned_path(owner, book, path)) continue;
        if (stat(path, &info) == 0) continue; // 书还在，缓存有效
        if (wr_forget_book(path)) ++dropped;
    }
    closedir(dir);
    return dropped;
}

static int visible_shelf(int requested, wr_book_t *book) {    WR_SD_SCOPE(WR_DIR);
    if (!scope.allowed) return 0;
    char owner[WR_ID];
    bool logged = wr_account(owner, NULL, NULL);
    wr_shelf_header_t header;
    FILE *file = logged ? open_shelf(owner,&header) : open_cached(owner,"rb");
    if (!file) return 0;
    int result = 0;
    if (logged && requested < 0) result = (int)header.count;
    else if (logged && requested < (int)header.count &&
             !fseek(file, (long)(sizeof(header) + (size_t)requested*sizeof(*book)), SEEK_SET)) {
        result = fread(book, sizeof(*book), 1, file) == 1;
    } else if (!logged) {
        wr_book_t record;
        char path[WR_PATH];
        struct stat st;
        bool found = false;
        for (unsigned i = 0; i < WR_MAX_SHELF && fread(&record, sizeof(record), 1, file) == 1; ++i) {
            if (!owned_path(owner, record.id, path) || stat(path, &st) || st.st_size <= 0) continue;
            if (requested == result) { *book = record; found = true; break; }
            ++result;
        }
        if (requested >= 0) result = found;
    }
    fclose(file);
    return result;
}

int wr_shelf_count(void) { return visible_shelf(-1, NULL); }
bool wr_shelf_get(int index, wr_book_t *book) { return index >= 0 && visible_shelf(index, book) == 1; }

/*
 * 把刚读过的书移到书架最前面：手机上读一本书，微信读书就把它顶到第一个，返回书架却还停在
 * 旧位置，读者会以为没生效。这里只改本地索引顺序，不发请求；下一次同步仍以服务端的
 * readUpdateTime 为准（那时服务端也已经把它移上来了），两条路不会打架。
 * Move the book just read to the front, matching what WeRead does on the phone. Local index
 * order only, no request; the next sync still follows the server's readUpdateTime, which by then
 * says the same thing, so the two never disagree.
 */
bool wr_shelf_touch(const char *book_id) {
    if (!book_id || !book_id[0] || strnlen(book_id, WR_ID) >= WR_ID) return false;
    WR_SD_SCOPE(WR_DIR);
    if (!scope.allowed) return false;
    char owner[WR_ID];
    if (!wr_account(owner, NULL, NULL)) return false;
    wr_shelf_header_t header;
    FILE *in = open_shelf(owner, &header);
    if (!in || !header.count) { if (in) fclose(in); return false; }
    /* 只找位置，不改内容：真要在第一位就什么都别做，免得每次开书都重写 5 MB 索引。 */
    /* Locate only. Already first means do nothing, so opening a book never rewrites the index. */
    wr_book_t first, touched;
    long touched_offset = -1;
    unsigned index = 0;
    for (; index < header.count; ++index) {
        wr_book_t record;
        if (fread(&record, sizeof(record), 1, in) != 1) break;
        if (!index) first = record;
        if (!strcmp(record.id, book_id)) { touched = record; touched_offset = (long)((size_t)index * sizeof(record)); break; }
    }
    if (touched_offset < 0 || !strcmp(first.id, book_id)) { fclose(in); return false; }
    fclose(in);
    in = open_shelf(owner, &header);
    if (!in) return false;
    bool ok = !fseek(in, (long)sizeof(header), SEEK_SET);
    FILE *out = ok ? fopen(WR_INDEX_TEMP, "wb") : NULL;
    ok = ok && out && fwrite(&header, sizeof(header), 1, out) == 1 &&
        fwrite(&touched, sizeof(touched), 1, out) == 1;
    /* 其余记录原样搬过去，跳过被移到最前的那一条。一次一条，不在 RAM 里排出整个书架。 */
    /* Stream the rest through, skipping the record that moved; one record at a time. */
    wr_book_t record;
    for (unsigned i = 0; ok && i < header.count; ++i) {
        if (fread(&record, sizeof(record), 1, in) != 1) { ok = false; break; }
        if ((long)((size_t)i * sizeof(record)) == touched_offset) continue;
        ok = fwrite(&record, sizeof(record), 1, out) == 1;
    }
    if (out) {
        if (fflush(out) || fsync(fileno(out))) ok = false;
        if (fclose(out)) ok = false;
    }
    fclose(in);
    if (ok) {
        const char *backup = WR_DIR "/shelf.prev";
        unlink(backup);
        bool moved = rename(WR_INDEX, backup) == 0;
        ok = (moved || access(WR_INDEX, F_OK) != 0) && rename(WR_INDEX_TEMP, WR_INDEX) == 0;
        if (ok) unlink(backup);
        else if (moved) rename(backup, WR_INDEX);
    }
    if (!ok) unlink(WR_INDEX_TEMP);
    ESP_LOGI("WEREAD", "shelf touch book=%s result=%d", book_id, ok);
    return ok;
}

static bool cover_path(const char *owner, const wr_book_t *book, char out[WR_PATH]) {
    if (!wr_safe(owner) || !wr_safe(book->id)) return false;
    return snprintf(out, WR_PATH, WR_DIR "/%s+%s.gray", owner, book->id) < WR_PATH;
}

bool wr_cover_get(const wr_book_t *book, uint8_t *pixels) {
    char owner[WR_ID], path[WR_PATH];
    wr_account(owner, NULL, NULL);
    if (!cover_path(owner, book, path)) return false;
    WR_SD_SCOPE(path);
    if (!scope.allowed) return false;
    struct stat st;
    if (stat(path, &st) || st.st_size != BOOK_COVER_W * BOOK_COVER_H) return false;
    FILE *file = fopen(path, "rb");
    if (!file) return false;
    bool ok = fread(pixels, BOOK_COVER_W * BOOK_COVER_H, 1, file) == 1;
    fclose(file);
    return ok;
}

static bool ensure_directory(const char *path) {
    struct stat st;
    if (stat(path,&st)==0) {
        if (S_ISDIR(st.st_mode)) return true;
        errno = ENOTDIR;
        return false;
    }
    if (errno!=ENOENT) return false;
    for (unsigned attempt=0;attempt<3;++attempt) {
        if (mkdir(path,0775)==0) return true;
        int saved=errno;
        if (stat(path,&st)==0 && S_ISDIR(st.st_mode)) return true;
        if (saved!=EBUSY && saved!=EAGAIN && saved!=EINTR && saved!=ENOENT) { errno=saved; return false; }
        vTaskDelay(pdMS_TO_TICKS(40u*(attempt+1)));
    }
    return stat(path,&st)==0 && S_ISDIR(st.st_mode);
}

static void remove_previews(const char *owner) {
    if (!wr_safe(owner)) return;
    DIR *dir = opendir(WR_DIR);
    if (!dir) return;
    char prefix[WR_ID+2], path[WR_PATH];
    size_t prefix_size = snprintf(prefix,sizeof(prefix),"%s+",owner);
    struct dirent *item;
    while ((item = readdir(dir))) {
        size_t n = strlen(item->d_name);
        if (n > prefix_size+5 && !memcmp(item->d_name,prefix,prefix_size) && !strcmp(item->d_name+n-5,".gray") &&
            snprintf(path,sizeof(path),WR_DIR "/%s",item->d_name) < sizeof(path)) unlink(path);
    }
    closedir(dir);
}

static bool write_file(const char *path, const void *data, size_t size) {
    FILE *file = fopen(path, "wb");
    if (!file) return false;
    (void)setvbuf(file, NULL, _IOFBF, 4096);
    bool ok = fwrite(data, 1, size, file) == size && !fflush(file) && !fsync(fileno(file));
    if (fclose(file)) ok = false;
    if (!ok) unlink(path);
    return ok;
}

static bool preview_from_epub(const char *path, uint8_t *pixels) {
    uint8_t *data = NULL;
    size_t size;
    bool png;
    bool ok = book_epub_cover_bounded(path,&data,&size,&png,2*1024*1024) == ESP_OK &&
        book_cover_thumbnail(data,size,png,pixels);
    free(data);
    return ok;
}

static void sync_covers(unsigned first, unsigned count) {
    uint8_t *pixels = json_alloc(BOOK_COVER_W * BOOK_COVER_H);
    if (!pixels) return;
    unsigned downloaded = 0;
    for (unsigned i = 0; i < count && !wr_cancelled(); ++i) {
        int64_t cover_start = esp_timer_get_time();
        wr_book_t book;
        char path[WR_PATH], url[sizeof(book.cover)+8];
        if (!wr_shelf_get((int)(first+i), &book)) break;
        if (!cover_path(s_http_account.owner, &book, path)) continue;
        struct stat st;
        if (!stat(path,&st) && st.st_size == BOOK_COVER_W * BOOK_COVER_H) {
            wr_work_progress(++downloaded,count);
            continue;
        }
        if (!stat(path,&st)) unlink(path); /* discard a partial file left by an interrupted write */
        // 已缓存的书先用本地 EPUB 封面，避免同步时重复请求同一本书的图片。
        // Cached books use their EPUB cover first, avoiding another image request during sync.
        char epub[WR_PATH];
        bool local = wr_book_path(&book,epub);
        int64_t local_start = esp_timer_get_time();
        bool ok = local && preview_from_epub(epub,pixels);
        int64_t local_ms = (esp_timer_get_time() - local_start) / 1000;
        if (!ok && local) ESP_LOGW("WEREAD","cached EPUB has no usable cover book=%s",book.id);
        snprintf(url, sizeof(url), "%s%s", !strncmp(book.cover, "//", 2) ? "https:" : "", book.cover);
        size_t length=0;
        char *image=NULL;
        int64_t net_start = esp_timer_get_time();
        for (unsigned attempt=0;!ok && book.cover[0] && attempt<3 && !image && !wr_cancelled();++attempt) {
            image=wr_http(url,NULL,NULL,4*1024*1024,&length);
            if (!image && attempt<2) vTaskDelay(pdMS_TO_TICKS(300u*(attempt+1)));
        }
        int64_t net_ms = (esp_timer_get_time() - net_start) / 1000;
        int64_t decode_start = esp_timer_get_time();
        if (!ok) ok = image && book_cover_thumbnail((uint8_t *)image,length,length>=8 && (uint8_t)image[0]==0x89,pixels);
        int64_t decode_ms = (esp_timer_get_time() - decode_start) / 1000;
        int64_t write_start = esp_timer_get_time();
        if (ok) {
            char temp[WR_PATH];
            int n=snprintf(temp,sizeof(temp),"%s.part",path);
            ok=n>0 && (size_t)n<sizeof(temp) && write_file(temp,pixels,BOOK_COVER_W*BOOK_COVER_H) &&
                (unlink(path)==0 || errno==ENOENT) && rename(temp,path)==0;
            if (!ok && n>0 && (size_t)n<sizeof(temp)) unlink(temp);
        }
        int64_t write_ms = (esp_timer_get_time() - write_start) / 1000;
        if (ok) {
            /* 本地书架与首页读的是书旁快照：顺手补一份，免得每本书都要先点开一次才有封面。 */
            /* The local shelf and home read a snapshot beside the book, so mirror it here. */
            char book_file[WR_PATH];
            struct stat book_info;
            if (owned_path(s_http_account.owner, book.id, book_file) &&
                !stat(book_file, &book_info) && book_info.st_size > 0)
                (void)wr_ensure_cover(&book, book_file);
        }
        ESP_LOGI("WEREAD", "cover stage book=%s local_ms=%lld net_ms=%lld bytes=%u decode_ms=%lld write_ms=%lld total_ms=%lld result=%d",
                 book.id, (long long)local_ms, (long long)net_ms, (unsigned)length,
                 (long long)decode_ms, (long long)write_ms,
                 (long long)((esp_timer_get_time()-cover_start)/1000), ok);
        if (!ok) ESP_LOGW("WEREAD","cover unavailable book=%s url=%s error=%s",book.id,book.cover,wr_error_message());
        else ++downloaded;
        free(image);
        wr_work_progress(downloaded, count);
    }
    free(pixels);
}

/*
 * 预落章节目录：只有磁盘上没有目录的书才真的联网，已落盘的由 get_chapters 直接命中磁盘
 * （一次读文件就返回），所以重复同步几乎不花时间。逐本串行、复用同一个 worker，随时可取消。
 * Stage chapter lists: only books without a disk TOC really hit the network; the rest are a
 * single file read inside get_chapters, so a repeat sync costs almost nothing. Serial, on the
 * same worker, cancellable between books.
 */
static void sync_toc(unsigned count) {
    unsigned warm = count < WR_TOC_WARM_BOOKS ? count : WR_TOC_WARM_BOOKS;
    unsigned ready = 0;
    for (unsigned i = 0; i < warm && !wr_cancelled(); ++i) {
        wr_book_t book;
        unsigned chapters = 0;
        if (!wr_shelf_get((int)i, &book)) break;
        int64_t started = esp_timer_get_time();
        wr_chapter_t *list = wr_chapter_list(&book, &chapters);
        bool ok = list && chapters;
        free(list);
        if (ok) ++ready;
        ESP_LOGI("WEREAD", "toc warm book=%s chapters=%u elapsed_ms=%lld result=%d", book.id,
                 chapters, (long long)((esp_timer_get_time() - started) / 1000), ok);
        vTaskDelay(1); // 让空闲任务与看门狗在连续的目录请求之间运行
    }
    ESP_LOGI("WEREAD", "toc warm done ready=%u/%u", ready, warm);
}

static bool sync_shelf(void) {
    /* Keep the shelf request independent from the small chapter JSON budget;
     * large libraries may exceed several megabytes.  The SAX adapter is being
     * introduced separately, so this compatibility path uses PSRAM-backed
     * storage with a generous firmware-wide ceiling. */
    cJSON *json = NULL;
    for (unsigned attempt = 0; attempt < 2 && !wr_cancelled(); ++attempt) {
        json = wr_json_limit("/web/shelf/sync", NULL, 16u * 1024u * 1024u);
        if (json || !s_http_open_failed || attempt != 0 || wr_cancelled()) break;
        ESP_LOGW("WEREAD", "shelf sync connect failed before HTTP response; retrying once");
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    if (!json) return false;
    cJSON *books = cJSON_GetObjectItemCaseSensitive(json, "books");
    if (!cJSON_IsArray(books) && cJSON_IsObject(json)) {
        cJSON *data = cJSON_GetObjectItemCaseSensitive(json, "data");
        if (cJSON_IsObject(data)) books = cJSON_GetObjectItemCaseSensitive(data, "books");
    }
    ESP_LOGI("WEREAD", "sync response json=%d books=%d", cJSON_IsObject(json), cJSON_IsArray(books));
    int count = cJSON_GetArraySize(books);
    WR_SD_SCOPE(WR_DIR);
    if (!scope.allowed) { s_error_message = "SD 卡不可用"; cJSON_Delete(json); return false; }
    if (!cJSON_IsArray(books)) {
        const cJSON *error = cJSON_GetObjectItemCaseSensitive(json,"errcode");
        if (!error) error = cJSON_GetObjectItemCaseSensitive(json,"errCode");
        if (cJSON_IsNumber(error) && error->valueint == -2012) {
            s_expired = true; s_error_message = "登录已失效，请重新授权";
        } else s_error_message = "书架响应中缺少书籍列表";
        cJSON_Delete(json); return false;
    }
    if (count > WR_MAX_SHELF) { s_error_message = "书架书籍数量超出支持范围"; cJSON_Delete(json); return false; }
    if (!ensure_directory(WR_DIR)) { s_error_message = "无法创建书架索引目录"; cJSON_Delete(json); return false; }
    wr_book_t *records = json_alloc((size_t)(count+1)*sizeof(*records));
    wr_shelf_header_t header = {0};
    snprintf(header.owner, sizeof(header.owner), "%s", s_http_account.owner);
    cJSON *entry;
    if (records) cJSON_ArrayForEach(entry, books) {
        wr_book_t *record = &records[header.count];
        memset(record, 0, sizeof(*record));
        if (!wr_value(cJSON_GetObjectItemCaseSensitive(entry, "bookId"), record->id, sizeof(record->id)) || !wr_safe(record->id)) {
            continue;
        }
        (void)wr_value(cJSON_GetObjectItemCaseSensitive(entry, "title"), record->title, sizeof(record->title));
        (void)wr_value(cJSON_GetObjectItemCaseSensitive(entry, "author"), record->author, sizeof(record->author));
        if (!wr_value(cJSON_GetObjectItemCaseSensitive(entry, "cover"), record->cover, sizeof(record->cover))) record->cover[0] = 0;
        const cJSON *date = cJSON_GetObjectItemCaseSensitive(entry, "readUpdateTime");
        record->updated = cJSON_IsNumber(date) && date->valuedouble >= 0 && date->valuedouble <= UINT32_MAX ? (uint32_t)date->valuedouble : 0;
        ++header.count;
    }
    cJSON_Delete(json);
    if (!records) { s_error_message = "内存不足，无法读取书架"; return false; }
    wr_work_progress(0, header.count);
    /* WeRead's response may include pinned/unfinished entries in a different
     * order from its visible recent-reading shelf. Sort by its update timestamp,
     * descending; stable ties retain server order. */
    for (unsigned i=1;i<header.count;++i) {
        wr_book_t value=records[i]; unsigned j=i;
        while (j && records[j-1].updated < value.updated) { records[j]=records[j-1]; --j; }
        records[j]=value;
    }
    bool ok = records && !wr_cancelled();
    if (ok) {
        FILE *file = fopen(WR_INDEX_TEMP, "wb");
        ok = file && fwrite(&header, sizeof(header), 1, file) == 1;
        for (unsigned i = 0; ok && i < header.count; ++i) ok = fwrite(&records[i], sizeof(*records), 1, file) == 1;
        if (file) {
            if (fflush(file) || fsync(fileno(file))) ok = false;
            if (fclose(file)) ok = false;
        }
        if (ok && !wr_cancelled()) {
            // FAT rename cannot replace an existing destination.
            const char *backup = WR_DIR "/shelf.prev";
            unlink(backup);
            bool moved = rename(WR_INDEX, backup) == 0;
            ok = (moved || access(WR_INDEX, F_OK) != 0) && rename(WR_INDEX_TEMP, WR_INDEX) == 0;
            if (ok) unlink(backup);
            else if (moved) rename(backup, WR_INDEX);
        } else ok = false;
        if (!ok) unlink(WR_INDEX_TEMP);
    }
    free(records);
    if (!ok && !wr_cancelled()) s_error_message = "书架索引写入 SD 卡失败";
    /* Cover network I/O belongs to explicit shelf sync only. Keep the reader
     * and shelf-entry paths local-only, and report successful cached covers. */
    if (ok && header.count) sync_covers(0, header.count);
    /* 封面之后再把最近这几本的目录落盘：首次打开一本书时，UI 线程就不必再发目录请求。 */
    /* After covers, stage the TOC of the recent few so a first open never fetches it on the UI. */
    if (ok && header.count && !wr_cancelled()) sync_toc(header.count);
    return ok;
}

static bool check_session(void) {
    int64_t started = esp_timer_get_time();
    char path[192];
    int path_size = snprintf(path, sizeof(path), "/web/shelf/sync?onlyBookid=1&cbcount=1&userVid=%s",
                             s_http_account.owner);
    if (path_size < 0 || (size_t)path_size >= sizeof(path) || !wr_safe(s_http_account.owner)) {
        s_error_message = "无法验证微信读书登录状态，请稍后重试";
        return false;
    }
    cJSON *response = wr_json(path, NULL);
    if (!response) return false;
    const cJSON *payload = cJSON_GetObjectItemCaseSensitive(response, "data");
    if (!cJSON_IsObject(payload)) payload = response;
    const char *const index_keys[] = {"bookIds", "bookids", "books", "updated"};
    bool valid = false;
    for (size_t i = 0; i < sizeof(index_keys) / sizeof(index_keys[0]); ++i) {
        const cJSON *index = cJSON_GetObjectItemCaseSensitive(payload, index_keys[i]);
        if (cJSON_IsArray(index) || cJSON_IsObject(index)) { valid = true; break; }
    }
    cJSON_Delete(response);
    if (valid) {
        ESP_LOGI("WEREAD", "session check mode=light elapsed_ms=%lld", (long long)((esp_timer_get_time()-started)/1000));
        return true;
    }
    if (s_expired) return false;
    ESP_LOGW("WEREAD", "session check light response incompatible; falling back to full shelf");
    response = wr_json("/web/shelf/sync", NULL);
    if (!response) return false;
    const cJSON *books = cJSON_GetObjectItemCaseSensitive(response, "books");
    if (!cJSON_IsArray(books)) {
        payload = cJSON_GetObjectItemCaseSensitive(response, "data");
        books = cJSON_GetObjectItemCaseSensitive(payload, "books");
    }
    valid = cJSON_IsArray(books);
    cJSON_Delete(response);
    if (valid) ESP_LOGI("WEREAD", "session check mode=full elapsed_ms=%lld", (long long)((esp_timer_get_time()-started)/1000));
    else if (!s_expired) s_error_message = "无法验证微信读书登录状态，请稍后重试";
    return valid;
}

/* 用 consent 三件套经 POST /web/login/session/init 自动建立网页会话：一次墨水屏扫码即可，
   无需第二次网页扫码。Set-Cookie 的 wr_* 由 capture_cookie 自动捕获并落盘，随后填充账号态。
   网页会话过期时可用已存 consent token 重新 mint，用户无感。返回 true 表示网页会话已就绪。 */
bool wr_mint_web_session(void) {
    char vid[WR_ID], access[384], refresh[384];
    if (!wr_consent_tokens(vid, sizeof(vid), access, sizeof(access), refresh, sizeof(refresh))) {
        ESP_LOGW("WEREAD", "mint unavailable: no consent tokens");
        return false;
    }
    cJSON *jb = cJSON_CreateObject();
    if (!jb) return false;
    cJSON_AddStringToObject(jb, "vid", vid);
    cJSON_AddStringToObject(jb, "skey", access);
    cJSON_AddStringToObject(jb, "rt", refresh);
    cJSON_AddNumberToObject(jb, "pf", 2);
    cJSON_AddBoolToObject(jb, "ql", 0);
    char *body = cJSON_PrintUnformatted(jb);
    cJSON_Delete(jb);
    if (!body) return false;

    char *resp = wr_http("/web/login/session/init", body, NULL, 16384, NULL);
    free(body);
    cookie_lock();
    ESP_LOGI("WEREAD", "post-mint cookies=%.500s", s_cookies);
    cookie_unlock();
    bool success = false;
    if (resp) {
        cJSON *j = cJSON_Parse(resp);
        if (j) {
            success = json_flag_true(cJSON_GetObjectItemCaseSensitive(j, "success"));
            cJSON_Delete(j);
        }
        free(resp);
    }
    if (!success) {
        ESP_LOGW("WEREAD", "mint session/init rejected");
        return false;
    }
    cookie_lock();
    bool have_skey = strstr(s_cookies, "wr_skey=") != NULL;
    cookie_unlock();
    if (!have_skey) {
        ESP_LOGW("WEREAD", "mint ok but wr_skey missing");
        return false;
    }
    /* 填账号态：同号保留原 library，换号才重新生成。 */
    wr_account_t account = {0};
    snprintf(account.owner, sizeof(account.owner), "%s", vid);
    snprintf(account.token, sizeof(account.token), "%s", access);
    account.library = !strcmp(s_http_account.owner, vid) ? s_http_account.library : esp_random();
    if (!save_account(&account)) {
        ESP_LOGW("WEREAD", "mint account save failed");
        return false;
    }
    s_http_account = account;
    /* 新网页会话已建立：清掉续期失败时留下的"已过期"标记，否则 check_session 会在
     * light 应答不兼容时直接短路返回 false，并在收尾处把刚 mint 的账号/cookie 再次抹掉。
     * A fresh web session is valid: clear the stale expired flag so check_session does not
     * short-circuit and wipe the just-minted account/cookies at the end of the job. */
    s_expired = false;
    ESP_LOGI("WEREAD", "web session minted from consent vid=%s", vid);
    return true;
}

bool wr_clear_web_session(void) {
    wr_account_t account;
    if (read_account(&account)) {
        memset(account.token, 0, sizeof(account.token));
        if (!save_account(&account)) return false;
        s_http_account = account;
    }
    return clear_session_cookies();
}

bool wr_logout(bool remove_books) {
    if (wr_status(NULL, 0, NULL, NULL) == WR_BUSY) return false;
    wr_account_t account;
    if (!read_account(&account)) return true;
    if (remove_books) {
        WR_SD_SCOPE(WR_DIR);
        if (!scope.allowed || !wr_safe(account.owner)) return false;
        char path[WR_PATH];
        FILE *files = open_cached(account.owner,"rb");
        bool ok = true;
        wr_book_t item;
        while (files && fread(&item,sizeof(item),1,files) == 1) {
            if (!owned_path(account.owner,item.id,path) || (unlink(path) && errno != ENOENT)) { ok = false; break; }
            (void)book_progress_forget(path);
            (void)book_title_clear(path);
        }
        if (files) { ok = ok && !ferror(files); fclose(files); }
        if (!ok) return false;
        remove_previews(account.owner);
        if (unlink(WR_INDEX) && errno != ENOENT) return false;
        if (unlink(WR_DIR "/shelf.prev") && errno != ENOENT) return false;
        if (unlink(WR_CACHED) && errno != ENOENT) return false;
    }
    memset(account.token, 0, sizeof(account.token));
    if (!save_account(&account)) return false;
    return clear_session_cookies();
}

/*
 * 预取策略：当前章 + 下一章（WR_READ_AHEAD_CHAPTERS = 2）。
 *
 * 读者正在等待的章节永远优先（占位正文被读取时登记），其次是进场章节起、往后一章内
 * 第一个未就绪的章节。窗口以"进场章节"为锚，而不是跟着刚下完的章一路滑走——否则一轮
 * 就会把整本书下完，后台任务长期占着 worker，读者点同步/进度只能被告知"请等待"。
 * 每进入新的一章都会重新发起一次预取，所以滚动阅读时始终领先一章。
 *
 * A failure only ends this round; finished chapters stay, and the next open resumes at the gap.
 *
 * Read-ahead policy: the current chapter plus one. The chapter the reader waits for always
 * wins (registered when its placeholder body is read), otherwise the first not-ready chapter
 * within one chapter after the entry. The window is anchored at the entry chapter instead of
 * following each newly staged chapter — otherwise a single round would fetch the whole book and
 * hold the worker for minutes, which is exactly why taps on sync/progress were answered with
 * "wait for the current operation". Every chapter entry starts a fresh prefetch, so scrolling
 * reading stays exactly one chapter ahead.
 */
/*
 * 首开解析：worker 在第一次下载之前问一次云端进度。成功时把目标章节写进 read_wait，
 * 窗口就从那一章开始，不再是"先白下第一章再跳走"。
 * First-open resolution: ask the cloud once, before the first download. On success the target
 * chapter becomes the awaited one, so the window starts there instead of wasting chapter one.
 * 返回解析出的章节，没有可解析的返回 UINT_MAX。
 * Returns the resolved chapter, or UINT_MAX when nothing was pending for this book.
 */
static unsigned resolve_cloud_seed(const wr_book_t *book, unsigned fallback) {
    bool pending;
    portENTER_CRITICAL(&s_lock);
    pending = s_seed_state == WR_SEED_PENDING && !strcmp(s_seed_book, book->id);
    portEXIT_CRITICAL(&s_lock);
    if (!pending) return UINT_MAX;
    char owner[WR_ID] = {0}, path[WR_PATH];
    bool found = false;
    unsigned chapter = 0, offset = 0;
    const char *why = NULL;
    /* 路径按账号算：进度记录与标记文件都挂在这条路径上，解析完顺带把本地进度更新成云端位置。 */
    /* The path is account-derived: the progress record hangs off it, and resolving also
     * rewrites the local position to the cloud one. */
    if (wr_account(owner, NULL, NULL) && owned_path(owner, book->id, path)) {
        unsigned target = 0;
        if (wr_progress(book, path, false, &why, &target)) {
            struct stat info;
            book_progress_t saved;
            if (!stat(path, &info) &&
                book_progress_load(path, (uint32_t)info.st_size, &saved)) {
                chapter = saved.chapter;
                offset = saved.byte_off;
            } else chapter = target;
            found = true;
        } else ESP_LOGW("WEREAD", "cloud seed failed book=%s why=%s", book->id, why ? why : "");
    }
    bool still;
    portENTER_CRITICAL(&s_lock);
    /* 解析期间读者可能已经换书或退出：结果只对"仍是这次登记"的情况生效。 */
    /* The reader may have switched books meanwhile: publish only for this very request. */
    still = s_seed_state == WR_SEED_PENDING && !strcmp(s_seed_book, book->id);
    if (still) {
        s_seed_state = found ? WR_SEED_FOUND : WR_SEED_ABSENT;
        s_seed_chapter = chapter;
        s_seed_offset = offset;
    }
    portEXIT_CRITICAL(&s_lock);
    if (!still) return UINT_MAX;
    ESP_LOGI("WEREAD", "cloud seed book=%s found=%d chapter=%u offset=%u",
             book->id, found, chapter + 1, offset);
    if (!found) {
        /* 云端也没有进度：等第一章。这一句不能省——等待标记是全局的，可能还指着上一本书的章。 */
        /* No cloud position either: wait for the fallback chapter. Not optional — the wait mark
         * is global and may still point at the previous book's chapter. */
        wr_read_wait(book->id, fallback);
        return UINT_MAX;
    }
    return chapter;
}

static bool prefetch_window(const wr_work_t *work, unsigned first) {
    unsigned count = 0;
    wr_chapter_t *chapters = wr_chapter_list(&work->book, &count);
    if (!chapters) return false;
    /* 封面先落盘：本地书架与首页"继续阅读"读的是书旁的灰阶快照，缺它只能显示默认封面。
     * 放在后台任务里做——书架点开是即时的，不能为一张图卡住阅读线程。
     * Land the cover first: the local shelf reads a grayscale snapshot beside the book. Do it
     * here, in the background, because opening from the shelf must never wait on an image. */
    /* 概况文件缺失或章数对不上（换过版本、删过章节）时全量重算一次；之后每落一章加一。 */
    /* Recompute when the summary is missing or stale; afterwards each landed chapter adds one. */
    unsigned summary_ready = 0, summary_total = 0;
    if (!wr_cache_summary(&work->book, &summary_ready, &summary_total) || summary_total != count)
        wr_cache_summary_refresh(&work->book, count);
    char cover_owner[WR_ID] = {0}, cover_for[WR_PATH];
    if (wr_book_path(&work->book, cover_for) ||
        (wr_account(cover_owner, NULL, NULL) && owned_path(cover_owner, work->book.id, cover_for)))
        (void)wr_ensure_cover(&work->book, cover_for);
    bool ok = true;
    const unsigned anchor = first < count ? first : 0;
    unsigned cursor = anchor;
    const unsigned limit = anchor + WR_READ_AHEAD_CHAPTERS < count ?
        anchor + WR_READ_AHEAD_CHAPTERS : count;
    // 上界防止读到等待章节反复失败时死循环：每章最多尝试一次，另加窗口外的一次机会。
    for (unsigned guard = 0; guard < count + 1 && !wr_cancelled(); ++guard) {
        unsigned target = UINT_MAX;
        int waiting = wr_read_waiting(work->book.id);
        if (waiting >= 0 && (unsigned)waiting < count &&
            wr_cache_state(&work->book, (unsigned)waiting) != WR_CS_READY) {
            target = (unsigned)waiting;
        } else {
            // 锚定在进场章节上：跨过 limit 就不再往后下，剩下的留给读者翻到那里再补。
            // Anchored at the entry chapter: nothing past limit, the rest waits for the reader.
            for (unsigned i = cursor; i < limit; ++i) {
                if (wr_cache_state(&work->book, i) != WR_CS_READY) { target = i; break; }
            }
            if (target == UINT_MAX) {
                /*
                 * 窗口正文全齐，但插图可能还欠着：断网那一轮章包没下来，图被记在待补清单里。
                 * 每章"可读"只由正文决定，所以这些章状态是 READY，正式循环再也不会回到它们；
                 * 趁这次窗口顺手补一遍，否则"已缓存"的章会一直空着插图。
                 * The window's bodies are all here, but pictures may still be owed: an outage
                 * left them in the deferred list while the body still counted as readable, so the
                 * main loop never returns to those chapters. Sweep them now instead of leaving
                 * "cached" chapters permanently image-less.
                 */
                if (wr_online())
                    for (unsigned i = anchor; i < limit; ++i)
                        if (wr_stage_images_pending(&work->book, i))
                            (void)wr_stage_chapter_images(&work->book, i);
                break; // 窗口已满足
            }
        }
        wr_work_progress(target + 1, count);
        /* 正在下哪一章要让目录知道：徽标据此显示"下载中"，读者不会以为它还没排队。 */
        /* Tell the TOC which chapter is in flight, so its badge reads "下载中" rather than
         * looking like nothing was queued. */
        s_fetching_chapter = (int)target;
        bool staged = wr_stage_chapter(&work->book, &chapters[target], target);
        s_fetching_chapter = -1;
        if (!staged) { ok = false; break; }
        wr_read_mark_ready(target);
        cursor = target;
        // 让空闲任务与看门狗在密集的章节转换之间运行。
        vTaskDelay(1);
    }
    free(chapters);
    if (wr_cancelled()) wr_error_set("缓存已取消，已完成的章节已保留");
    return ok;
}

static void run_work(const wr_work_t *work) {
    char path[WR_PATH];
    const int64_t work_started_us = esp_timer_get_time();
    bool ok = false;
    const bool radio_boost = wr_online();
    if (radio_boost) {
        radio_job(true);
        ESP_LOGI("WEREAD", "work radio performance mode enabled kind=%d", work->kind);
        /*
         * 顺手对时：这条路径用的是微读已经建好的 STA 连接，只发一次 NTP 交换，
         * 不另起 Wi-Fi、不分配接收缓冲。闲置态那套临时初始化在内存紧张时起不来
         * （实测每 5 分钟一次 ESP_ERR_NO_MEM），所以"联网时校一次"才是可靠的兜底。
         * Sync while already online: this rides the STA link microread has up, costs one NTP
         * exchange, and allocates no Wi-Fi buffers. The idle path's temporary bring-up fails
         * when internal RAM is tight (measured: ESP_ERR_NO_MEM every five minutes), so
         * calibrating whenever we happen to be online is the dependable fallback.
         */
        if (app_time_service_sync_due(1800)) {
            ESP_LOGI("WEREAD", "work online, clock calibration requested");
            app_time_service_request_online_sync();
        }
    }
    ESP_LOGI("WEREAD", "work start kind=%d internal_free=%u largest=%u", work->kind,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    s_expired = false;
    s_error_message = "操作失败，请检查网络后重试";
    read_account(&s_http_account);
    s_cookies[0] = 0;
    restore_session_cookie();
    if (!s_cookies[0] && s_http_account.token[0])
        snprintf(s_cookies,sizeof(s_cookies),"wr_vid=%s; wr_skey=%s",s_http_account.owner,s_http_account.token);
    ESP_LOGI("WEREAD", "session cookies restored vid=%d skey=%d refresh=%d",
             session_cookie_has("wr_vid"), session_cookie_has("wr_skey"), session_cookie_has("wr_rt"));
    if (time(NULL) < 1704067200 && wr_online()) {
        uint32_t utc;
        (void)read_pico_transfer_sync_time_online(&utc);
    }
    if (s_http_account.token[0] && !wr_cancelled()) {
        switch (work->kind) {
        case WR_CHECK: ok = check_session(); break;
        case WR_SYNC: {
            ok = sync_shelf();
            // 同步成功后顺手清扫孤儿章节缓存：书籍文件不在的，缓存一并收回。
            if (ok) {
                WR_SD_SCOPE(WR_DIR);
                unsigned dropped = scope.allowed ? collect_orphan_cache() : 0;
                if (dropped) ESP_LOGI("WEREAD", "orphan chapter cache reclaimed books=%u", dropped);
            }
            break;
        }
        case WR_CACHE: {
            WR_SD_SCOPE(WR_DIR);
            if (!scope.allowed) { s_error_message="缓存失败：SD 卡当前不可用"; ok=false; }
            else if (!ensure_directory(WR_DIR)) { s_error_message="缓存失败：无法创建缓存目录"; ok=false; }
            else if (!owned_path(s_http_account.owner,work->book.id,path)) { s_error_message="缓存失败：书籍标识无效"; ok=false; }
            else if (!ensure_directory(app_settings_books_dir())) { s_error_message="缓存失败：无法创建书籍保存目录"; ok=false; }
            /* "缓存全本书籍"按章把全书落盘，不再组装单个 EPUB：组装是整条链路最容易整盘
             * 失败的一步，而且标记文件被 EPUB 替换后长度改变，本地进度会整本失效。
             * "Cache the whole book" stages every chapter instead of assembling one EPUB:
             * assembly is the most failure-prone step, and replacing the marker changes the file
             * length, which would invalidate the whole size-keyed local progress record. */
            else ok = wr_cache_range(&work->book, path, work->first, work->count);
            if (!ok) ESP_LOGE("WEREAD","cache failed book=%s phase=stage-full error=%s errno=%d",work->book.id,s_error_message,errno);
            if (ok) {
                /* 分章落盘的书以标记文件留在原路径，不算"整本 EPUB"：只有真被整包替换成
                 * EPUB 时才写缓存索引，否则书架会按 EPUB 去解一个 96 字节的占位文件。
                 * A chapter-staged book stays a marker at that path and is not a whole EPUB:
                 * only the whole-ZIP fallback produces one, so only that case joins the index;
                 * otherwise the shelf would try to parse a 96-byte placeholder as an EPUB. */
                if (!wr_mark_read(path, NULL, NULL)) {
                    ok = remember_cached(&work->book);
                    if (!ok) s_error_message="缓存失败：书籍已缓存，但本地缓存索引写入失败";
                }
                if (work->book.title[0]) (void)store_book_title(path,work->book.title);
            }
            break;
        }
        case WR_PREFETCH: {
            WR_SD_SCOPE(WR_DIR);
            if (!scope.allowed) { s_error_message = "缓存失败：SD 卡当前不可用"; ok = false; }
            else {
                /* 首开未缓存的书：先解析云端进度，窗口直接锚在那一章；没有就照旧从第一章起。
                 * 解析失败（断网等）也照旧——读者不会因为一次 getProgress 拿不到就卡住。 */
                /* First open of an uncached book: resolve the cloud position first and anchor the
                 * window there; without one, chapter one stands. A failed resolution (offline)
                 * changes nothing, so a reader never stalls on one small request. */
                unsigned seed = resolve_cloud_seed(&work->book, work->first);
                if (seed != UINT_MAX) {
                    wr_read_wait(work->book.id, seed);
                    ESP_LOGI("WEREAD", "prefetch anchored at cloud chapter=%u", seed + 1);
                }
                ok = prefetch_window(work, seed != UINT_MAX ? seed : work->first);
            }
            break;
        }
        case WR_UPLOAD: case WR_DOWNLOAD: {
            const char *why = NULL;
            unsigned target = UINT_MAX;
            /* 进度同步不再要求整本已缓存：分章缓存的书在磁盘上只是标记文件，本地进度按
             * "路径 + 长度"记录，章节正文从章节缓存目录取，两边都能同步。
             * Progress sync no longer needs a complete cache: a chapter-cached book is only a
             * marker, the local record is keyed by path and length, and the chapter body comes
             * from the chapter cache — both directions work. */
            if (!wr_book_path(&work->book, path)) {
                char owner[WR_ID] = {0};
                struct stat info;
                if (!wr_account(owner, NULL, NULL) || !owned_path(owner, work->book.id, path)) {
                    s_error_message = "书籍标识无效";
                    break;
                }
                if (stat(path, &info) || !S_ISREG(info.st_mode)) {
                    if (work->kind == WR_UPLOAD) {
                        s_error_message = "这本书还没有本地阅读进度，先读一段再上传";
                        break;
                    }
                    /* 下载进度不该要求先有缓存：把标记文件先落盘，顺带把章节目录写进磁盘
                     * 缓存，wr_progress 于是有路径可写，界面也能直接打开这本书。
                     * Downloading progress must not require a cache first: land the marker here
                     * (the chapter list goes into the disk TOC cache as a side effect), so
                     * wr_progress has a path to write and the UI can open the book right away. */
                    unsigned chapters = 0;
                    wr_chapter_t *list = wr_chapter_list(&work->book, &chapters);
                    bool marked = list && chapters && wr_mark_write(&work->book, path, chapters);
                    if (marked && work->book.title[0]) (void)store_book_title(path, work->book.title);
                    free(list);
                    if (!marked) {
                        s_error_message = "这本书还没有缓存，无法获取云端进度";
                        break;
                    }
                }
            }
            ok = wr_progress(&work->book, path, work->kind == WR_UPLOAD, &why, &target);
            // 需求要求区分「没有本地进度」「无法定位章节」，不再统一报网络错误。
            if (!ok && why) s_error_message = why;
            portENTER_CRITICAL(&s_lock);
            s_progress_target = work->kind == WR_DOWNLOAD && ok ? target : UINT_MAX;
            portEXIT_CRITICAL(&s_lock);
            break;
        }
        default: break;
        }
    } else if (!wr_cancelled()) {
        s_error_message = "登录已失效，请重新授权";
        s_expired = true;
    }
    if (radio_boost) {
        radio_job(false);
        ESP_LOGI("WEREAD", "work radio performance mode restored kind=%d", work->kind);
    }
    if (s_expired && s_http_account.token[0]) {
        memset(s_http_account.token, 0, sizeof(s_http_account.token));
        (void)save_account(&s_http_account);
    }
    memset(&s_http_account, 0, sizeof(s_http_account));
    memset(s_cookies, 0, sizeof(s_cookies));
    ESP_LOGI("WEREAD","work done kind=%d ok=%d canceled=%d elapsed_ms=%lld stack_hwm=%u internal_free=%u psram_free=%u",
             work->kind,ok,wr_cancelled(),(long long)((esp_timer_get_time()-work_started_us)/1000),
             (unsigned)uxTaskGetStackHighWaterMark(NULL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT));
    portENTER_CRITICAL(&s_lock);
    s_qr[0] = 0;
    s_result_message = s_error_message;
    s_result = s_expired ? WR_EXPIRED : wr_cancelled() ? WR_CANCELLED : ok ? WR_OK : WR_FAILED;
    portEXIT_CRITICAL(&s_lock);
}

void wr_work_progress(unsigned done, unsigned total) {
    portENTER_CRITICAL(&s_lock);
    s_done = done;
    s_total = total;
    portEXIT_CRITICAL(&s_lock);
}

bool wr_start(wr_job_t job, const wr_book_t *book, unsigned first, unsigned count) {
    portENTER_CRITICAL(&s_lock);
    bool accepted = s_worker && s_result != WR_BUSY;
    bool queued = false;
    if (accepted) {
        s_work.kind = job;
        s_work.first = first;
        s_work.count = count;
        if (book) s_work.book = *book;
        else memset(&s_work.book, 0, sizeof(s_work.book));
        s_work_pending = true;
        atomic_store(&s_cancel, false);
        s_result = WR_BUSY;
        s_active_job = job;
        s_done = s_total = 0;
        s_qr[0] = 0;
    } else if (s_worker && s_active_job == WR_PREFETCH &&
               (job != WR_PREFETCH || !book || !book->id[0] || strcmp(book->id, s_work.book.id))) {
        /*
         * 只有后台预取会挡路，而它随时可以放弃：登记用户任务并让预取收手，
         * 下一轮 worker 立刻执行用户任务。读者点一下就该有反应，不该被告知"请等待"。
         * 换书时新书的预取同样走这条路：旧书的预取还在下别的章节，不让它收手，
         * 新书的等待页就永远等不到自己的章。
         * Only the background prefetch can get in the way, and it can always yield: park the
         * user job and tell the prefetch to back off; the worker runs it on the next round.
         * A tap deserves a reaction, not a "please wait". A prefetch for a different book takes
         * the same path: the old job is fetching another book's chapters, and if it never
         * yields, the new book's waiting page would never see its own chapter.
         */
        s_queued.kind = job;
        s_queued.first = first;
        s_queued.count = count;
        if (book) s_queued.book = *book;
        else memset(&s_queued.book, 0, sizeof(s_queued.book));
        s_queued_pending = true;
        atomic_store(&s_cancel, true);
        s_qr[0] = 0;
        queued = true;
    } else if (!s_worker) {
        s_result_message = "微信读书后台未初始化，请重启设备";
    } else {
        s_result_message = "请等待当前操作完成";
    }
    portEXIT_CRITICAL(&s_lock);
    if (accepted) xTaskNotifyGive(s_worker);
    // 插队时状态保持 WR_BUSY，界面从点击那刻起就显示任务页，不用先看到一句拒绝。
    // While queued the status stays WR_BUSY, so the task screen appears from the tap itself
    // instead of showing a rejection first.
    return accepted || queued;
}

wr_result_t wr_status(char *qr, size_t qr_size, unsigned *done, unsigned *total) {
    portENTER_CRITICAL(&s_lock);
    wr_result_t result = s_result;
    if (qr && qr_size) {
        size_t n = strnlen(s_qr, qr_size-1);
        memcpy(qr, s_qr, n);
        qr[n] = 0;
    }
    if (done) *done = s_done;
    if (total) *total = s_total;
    portEXIT_CRITICAL(&s_lock);
    return result;
}

bool wr_reporting_book(const char *path, wr_book_t *book) {
    char expected[WR_PATH], prefix[16];
    wr_account_t account;
    if (!read_account(&account) || !account.token[0]) return false;
    const char *base = strrchr(path, '/');
    if (!base) return false;
    snprintf(prefix, sizeof(prefix), "/wr-%08lx+", (unsigned long)account.library);
    if (strncmp(base, prefix, strlen(prefix))) return false;
    const char *id = base + strlen(prefix), *ext = strrchr(id, '.');
    if (!ext || strcmp(ext, ".epub") || ext == id || (size_t)(ext-id) >= WR_ID) return false;
    memset(book, 0, sizeof(*book));
    memcpy(book->id, id, ext-id);
    /* 分章缓存的书只写入了标记文件，还没进入"已缓存"索引；阅读时长上报同样需要
     * 认出它，否则按章阅读的书会整本丢失时长统计。
     * A chapter-cached book only carries the marker and is not in the cached index yet; the
     * reading-time reporter must recognise it too, or duration would vanish for such books. */
    return owned_path(account.owner, book->id, expected) && !strcmp(expected, path) &&
           (cached_record(account.owner, book->id) || wr_mark_read(path, NULL, NULL));
}

typedef enum { REPORT_RETRY, REPORT_CONFIRMED, REPORT_UNKNOWN, REPORT_ACCOUNT_CHANGED } report_result_t;

/* 离线时长回放：把攒下的补传秒数切成 30 秒的心跳小段，按真实节奏（间隔 30s）一拍一拍补报，
 * 每拍 ct 取当前时间、rt=本拍秒数，先进入会话再上报，和真人实时阅读的口径一致。
 * Replay offline duration as 30 s real-time heartbeats, spaced 30 s apart, ct=now each. */
#define WR_REPLAY_CHUNK_SECONDS 30u
#define WR_REPLAY_INTERVAL_US   INT64_C(30000000)

static bool same_report_book(const wr_report_t *a, const wr_report_t *b) {
    return a->library == b->library && !strcmp(a->owner, b->owner) && !strcmp(a->book_id, b->book_id);
}

/*
 * 云端没有这本书的进度时，用磁盘目录按章节序号取出 uid：上报请求体要的是章节 uid + 章内偏移，
 * 而"手机上没读过这本"并不意味着设备上没读。只读磁盘缓存，绝不联网——这条路径在 worker 里，
 * 它的正事是一次很小的 POST，不该再赔一次取目录。
 * When the cloud has no position for this book, take the chapter uid from the on-disk TOC by
 * index. The report body needs a chapter uid plus an in-chapter offset, and "never read on the
 * phone" does not mean "not read on this device". Disk only, never the network.
 */
static bool report_local_chapter(const char *book_id, unsigned index, wr_chapter_t *chapter) {
    wr_book_t book;
    memset(&book, 0, sizeof(book));
    snprintf(book.id, sizeof(book.id), "%s", book_id);
    unsigned count = 0;
    wr_chapter_t *list = wr_chapter_list_cached(&book, &count);
    if (!list) return false;
    bool ok = index < count && list[index].id[0];
    if (ok) *chapter = list[index];
    free(list);
    return ok;
}

static report_result_t run_report(const wr_report_t *job, const char **stage, unsigned *sent_seconds) {
    const bool consent_ok = wr_consent_authorized();
    *stage = "account";
    /* consent 通道用独立 vid+accessToken 认证、不依赖网页 cookie；只有网页路径才要求完整账号匹配。 */
    if (!consent_ok && !account_is(job->owner, job->library)) return REPORT_ACCOUNT_CHANGED;
    *stage = "offline";
    if (!wr_online()) return REPORT_RETRY;

    /* 离线补传回放：每拍只发 30 秒（最后一拍带零头），首拍先进入会话；在线实时任务整拍发。
     * 本拍真正上报的秒数经 sent_seconds 交回 process_report，确认后只扣这一拍。 */
    unsigned send_seconds = job->resend && job->seconds > WR_REPLAY_CHUNK_SECONDS
                            ? WR_REPLAY_CHUNK_SECONDS : job->seconds;
    bool enter = job->resend && !job->entered;
    *sent_seconds = send_seconds;

    *stage = "wifi";
    wr_chapter_t chapter;
    unsigned offset = 0, pct = 0;
    bool absent = false;
    memset(&chapter, 0, sizeof(chapter));
    /* 离线补传（resend）：直接用本地真实章节位置，不取云端 getProgress 的过期旧位置，
     * 避免"旧章节+大秒数"被服务端反作弊判不可信。实时在线路径仍按原逻辑取云端位置。 */
    bool located = false;
    bool local_position = job->resend;
    if (local_position) {
        if (report_local_chapter(job->book_id, job->chapter, &chapter)) {
            offset = job->offset;
            pct = job->pct > 100 ? 100 : job->pct;
            ESP_LOGI("WEREAD", "resend uses local position chapter=%u offset=%u pct=%u",
                     job->chapter + 1, offset, pct);
        } else {
            local_position = false;
        }
    }
    if (!local_position) {
        *stage = "get_progress";
        located = wr_remote_position_with_chapters(job->book_id, &chapter, &offset, &pct,
                                                   NULL, 0, &absent);
        /* 云端无此书进度（absent）时改用本地章节位置；网络/解析失败保持无位置，交给 consent 兜底。 */
        local_position = !located && absent;
    }
    bool have_position = located || local_position;
    if (local_position && !job->resend && report_local_chapter(job->book_id, job->chapter, &chapter)) {
        offset = job->offset;
        pct = job->pct > 100 ? 100 : job->pct;
        ESP_LOGI("WEREAD", "report uses local position chapter=%u offset=%u pct=%u",
                 job->chapter + 1, offset, pct);
    }

    /* ===== 主路径：网页 /web/book/read（v2 实证可计时；wr_* cookie 由 consent mint 自动建立）。
     * 完整双签名，ct 取当前时间、首拍先进入会话，离线时长按 30 秒一拍实时回放，服务端才真正计时。===== */
    if (have_position) {
        bool cached = s_report_fields.expires > esp_timer_get_time() &&
            same_report_book(job, &s_report_fields.account_book) &&
            !strcmp(chapter.id, s_report_fields.chapter);
        ESP_LOGI("WEREAD", "report reader fields cache_hit=%d seconds=%u", cached, job->seconds);
        bool fields_ready = cached;
        if (!cached) {
            *stage = "reader_fields";
            if (!report_display_busy()) {
                s_report_fields.expires = 0;
                if (wr_reader(job->book_id, chapter.id, &s_report_fields.fields)) {
                    s_report_fields.account_book = *job;
                    memcpy(s_report_fields.chapter, chapter.id, sizeof(chapter.id));
                    s_report_fields.expires = esp_timer_get_time() + 300000000;
                    fields_ready = true;
                }
            }
        }
        if (fields_ready) {
            /* 云端位置需复核未被改动；本地位置（云端本无此书）无基准，跳过复核。 */
            bool progress_ok = local_position;
            if (!local_position) {
                *stage = "check_progress";
                wr_chapter_t current;
                unsigned current_offset, current_pct;
                if (wr_remote_position(job->book_id, &current, &current_offset, &current_pct) &&
                    !strcmp(current.id, chapter.id) && current_offset == offset &&
                    current_pct == pct)
                    progress_ok = true;
            }
            if (progress_ok) {
                *stage = "submit";
                if (!report_display_busy()) {
                    bool post_started = false;
                    if (wr_send_duration(job->book_id, &chapter, offset, pct, send_seconds,
                                         &s_report_fields.fields, enter, &post_started)) {
                        if (!add_stats(job->owner, send_seconds))
                            ESP_LOGW("WEREAD", "report confirmed but local stats write failed seconds=%u",
                                     send_seconds);
                        return REPORT_CONFIRMED;
                    }
                    if (post_started) {
                        s_report_fields.expires = 0;
                        if (!consent_ok) return REPORT_UNKNOWN;
                    }
                }
            }
        }
        ESP_LOGW("WEREAD", "web report unavailable; falling back to consent=%d", consent_ok);
    }

    /* ===== 回退：consent 移动通道（web cookie/位置/字段不可用，或 web 未确认）。===== */
    if (consent_ok) {
        wr_chapter_t local_ch;
        bool have_local = report_local_chapter(job->book_id, job->chapter, &local_ch);
        char fallback_chapter[WR_ID] = {0};
        if (!located) {
            if (same_report_book(job, &s_report_fields.account_book) && s_report_fields.chapter[0])
                snprintf(fallback_chapter, sizeof(fallback_chapter), "%s", s_report_fields.chapter);
            ESP_LOGW("WEREAD", "consent fallback chapter='%s' offset=0 (get_progress unavailable)",
                     fallback_chapter[0] ? fallback_chapter : "0");
        }
        const char *chapter_id;
        unsigned use_offset;
        if (have_local) {
            chapter_id = local_ch.id;
            use_offset = job->offset;
        } else {
            chapter_id = located ? chapter.id : fallback_chapter;
            use_offset = located ? offset : 0;
        }
        uint32_t chapter_uid = (uint32_t)strtoul(chapter_id, NULL, 10);
        ESP_LOGI("WEREAD", "consent position local=%u chapter=%s offset=%u",
                 have_local, chapter_id, use_offset);
        *stage = "submit";
        if (wr_consent_upload(job->book_id, job->owner, send_seconds,
                              chapter_id, chapter_uid, use_offset,
                              job->resend, job->chapter, job->pct)) {
            if (!add_stats(job->owner, send_seconds))
                ESP_LOGW("WEREAD", "consent confirmed but local stats write failed seconds=%u",
                         send_seconds);
            return REPORT_CONFIRMED;
        }
    }
    return REPORT_RETRY;
}

static void process_report(unsigned slot, const wr_report_t *job) {
    const char *stage;
    unsigned sent_seconds = 0;
    s_processing_report = true;
    report_result_t result = run_report(job, &stage, &sent_seconds);
    s_processing_report = false;
    memset(s_cookies, 0, sizeof(s_cookies));
    int64_t now = esp_timer_get_time();
    const bool offline = result == REPORT_RETRY && !strcmp(stage, "offline");
    portENTER_CRITICAL(&s_lock);
    wr_report_t *pending = &s_reports[slot];
    if (result == REPORT_RETRY) {
        if (offline) {
            // 没网不动失败计数：等网络回来要立刻补发，而不是先退避五分钟。
            // Leave the failure budget alone while offline: once the link returns the queued
            // seconds should go out at once instead of waiting out a five-minute backoff.
            pending->failures = 0;
            /* 离线只等 5s：worker 在有待传时本就每 500ms 轮询一次，链路一恢复、retry_at
             * 到期，最多 5s 就补发；离线期间的每次检查只是读状态变量，不发起任何网络请求。
             * Wait only 5 s offline: with pending seconds the worker polls every 500 ms, so once
             * the link is back and retry_at is due the flush happens within 5 s. Each offline
             * check merely reads a state variable and makes no network request. */
            pending->retry_at = now + 5000000;
        } else {
            if (pending->failures < 5) ++pending->failures;
            pending->retry_at = now + (2000000LL << pending->failures);
        }
    } else {
        /* 只扣本拍确认的秒数；账号切换则整条丢弃。 */
        pending->seconds -= sent_seconds;
        pending->failures = 0;
        if (result == REPORT_ACCOUNT_CHANGED) {
            pending->seconds = 0;
            pending->retry_at = 0;
        } else if (job->resend && pending->seconds) {
            /* 回放：本拍已确认，标记进入过会话、累计已补秒数，按真实节奏隔 30s 再补下一拍。
             * This chunk is confirmed; mark entered and space the next chunk 30 s out. */
            pending->entered = true;
            pending->replay_done += sent_seconds;
            pending->retry_at = now + WR_REPLAY_INTERVAL_US;
        } else {
            pending->retry_at = 0;
            if (result == REPORT_CONFIRMED) pending->replay_done += sent_seconds;
        }
    }
    unsigned remaining = pending->seconds;
    unsigned drain_total = pending->replay_done;
    bool drain_finished = result == REPORT_CONFIRMED && remaining == 0;
    portEXIT_CRITICAL(&s_lock);
    /* 只有秒数真的变化才写盘：重试只挪内存里的退避时间，不必反复磨损 SD 卡。
     * Only persist when the seconds changed; a retry just moves the in-RAM backoff. */
    if (result != REPORT_RETRY) queue_persist();
    /*
     * 离线只留一条"挂起"日志：以前每个退避周期都刷一条 retry，十分钟下来一百多行，
     * 真正的失败反而被淹掉。这里五分钟一次，恢复联网后也不清屏。
     * Offline logs once every five minutes: the old per-backoff "retry" line produced hundreds of
     * rows an hour and drowned real failures.
     */
    if (offline) {
        static int64_t s_offline_logged_us;
        if (!s_offline_logged_us || now - s_offline_logged_us > 300000000LL) {
            s_offline_logged_us = now;
            ESP_LOGI("WEREAD", "report parked offline seconds=%u pending=%u", job->seconds, remaining);
        }
        return;
    }
    const char *outcome = result == REPORT_CONFIRMED ? "confirmed" :
        result == REPORT_RETRY ? "retry" : result == REPORT_UNKNOWN ? "unknown" : "account_changed";
    ESP_LOGI("WEREAD", "report result=%s stage=%s chunk=%u pending=%u",
             outcome, stage, sent_seconds, remaining);
    if (drain_finished && job->resend) {
        /* 仅离线回放（resend）整批补完才记录确认、唤醒 UI 弹"离线补传成功"；
         * 联网实时报告（resend=false）确认后静默处理，绝不弹离线补传窗。
         * Only an offline replay pops the success dialog; online real-time reports stay silent. */
        s_last_confirm_seconds = drain_total;
        ++s_confirm_seq;
    }
}

/* 登记/取回被挡下的预取。放在 worker 里补跑，而不是让阅读页反复重试：
 * 阅读页的 tick 只读本地状态，不该为了"再排一次队"去碰任务队列。 */
/* Park and take back the turned-away prefetch. The worker re-runs it instead of the reader
 * retrying: the reader's tick only reads local state and should not touch the job queue. */
static void park_prefetch(const wr_book_t *book, unsigned chapter) {
    portENTER_CRITICAL(&s_lock);
    s_prefetch_pending = true;
    s_prefetch_book = *book;
    s_prefetch_first = chapter;
    portEXIT_CRITICAL(&s_lock);
}

static bool take_parked_prefetch(wr_work_t *out) {
    bool found = false;
    portENTER_CRITICAL(&s_lock);
    if (s_prefetch_pending) {
        s_prefetch_pending = false;
        out->kind = WR_PREFETCH;
        out->book = s_prefetch_book;
        out->first = s_prefetch_first;
        out->count = 0;
        found = true;
    }
    portEXIT_CRITICAL(&s_lock);
    return found;
}

static void worker_task(void *arg) {
    (void)arg;
    /*
     * 开机清扫一次孤儿章节缓存：旧固件删除本地书籍时没有回收章节数据，留下的缓存会让
     * 微读书架一直显示"已缓存"。等 SD 与账号就绪后再扫，只做几次 stat，不占 UI 时间。
     * Sweep orphan chapter caches once at boot: older firmware left chapter data behind when a
     * local book was deleted, which kept the WeRead shelf reporting "cached". Wait for the card
     * and the account, then scan — a handful of stat() calls, none on the UI's time.
     */
    vTaskDelay(pdMS_TO_TICKS(400));
    {
        WR_SD_SCOPE(WR_DIR);
        unsigned dropped = scope.allowed ? collect_orphan_cache() : 0;
        if (dropped) ESP_LOGI("WEREAD", "boot orphan sweep reclaimed books=%u", dropped);
    }
    /* 开机先恢复离线落盘的阅读位置，等网络就绪后由 pos_flush 自动补传。 */
    pos_restore();
    for (;;) {
        int64_t now = esp_timer_get_time();
        int slot = -1;
        bool pending = false;
        wr_report_t report;
        portENTER_CRITICAL(&s_lock);
        bool work = s_work_pending;
        if (work) s_work_pending = false;
        if (!work) {
            bool reader_idle = !s_reader_scanning && (!s_reader_scan_end || now - s_reader_scan_end >= 1200000);
            for (unsigned i = 0; i < sizeof(s_reports)/sizeof(s_reports[0]); ++i) {
                if (!s_reports[i].seconds) continue;
                pending = true;
                if (reader_idle && s_reports[i].retry_at <= now &&
                    (slot < 0 || s_reports[i].retry_at < report.retry_at)) {
                    slot = (int)i;
                    report = s_reports[i];
                }
            }
        }
        portEXIT_CRITICAL(&s_lock);

        if (work) {
            // 发布完成前，WR_BUSY 保证任务参数不能被覆盖。/ WR_BUSY protects the job until its result is published.
            run_work(&s_work);
            /* 被插队的用户任务在这里接班：状态一直保持 WR_BUSY，界面不需要重试。 */
            /* The queued user job takes over here; status never leaves WR_BUSY, so the UI needs no retry. */
            portENTER_CRITICAL(&s_lock);
            bool queued_job = s_queued_pending;
            if (queued_job) {
                s_work = s_queued;
                s_queued_pending = false;
                s_work_pending = true;
                atomic_store(&s_cancel, false);
                s_result = WR_BUSY;
                s_active_job = s_work.kind;
                s_done = s_total = 0;
            }
            portEXIT_CRITICAL(&s_lock);
            /* 有插队任务立刻接班时保留暖连接，省掉一次 TLS 握手；无接班任务才断开回收内存。
             * 主机不符或连接老化仍由 http_keepalive_get 的 host 与 20s 空闲检查兜底。
             * Keep the warm link when a queued job takes over immediately; otherwise drop it to
             * reclaim memory. Host mismatch and stale links are still handled by the guards. */
            if (!queued_job) http_keepalive_drop();
        } else if (take_parked_prefetch(&s_retry_prefetch)) {
            /*
             * 之前被"任务忙"挡下的预取在这里补跑。放在上报之前：读者正停在等待页上，
             * 一章节的补下比一次时长上报更该先走。
             * The prefetch that was turned away runs here. Ahead of reporting: the reader is
             * sitting on the waiting page, and one chapter matters more than a duration post.
             */
            if (!wr_start(WR_PREFETCH, &s_retry_prefetch.book, s_retry_prefetch.first, 0)) {
                park_prefetch(&s_retry_prefetch.book, s_retry_prefetch.first); // 另一个任务占着，稍后再试
                vTaskDelay(pdMS_TO_TICKS(300));
            }
        } else if (slot >= 0) {
            process_report((unsigned)slot, &report);
            http_keepalive_drop();
        } else if (s_pos_count) {
            /* 有待同步阅读位置：在线即传，离线则最多等 1s 再检查网络状态。 */
            pos_flush();
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));
        } else {
            ulTaskNotifyTake(pdTRUE, pending ? pdMS_TO_TICKS(500) : portMAX_DELAY);
        }
    }
}

/* ------------------------------------------------------------------ */
/* 分章阅读：打开交接与虚拟书源                                        */
/* ------------------------------------------------------------------ */

/*
 * 打开交接：章节目录在处理"打开书籍"的那一刻就准备好，阅读器真正打开文件时只做内存交接。
 * 虚拟书源绝不能在自己的回调里联网——那条路径跑在阅读页主任务上，一次请求就能冻住翻页。
 * Open handoff: the chapter list is prepared when the shelf opens the book, so the reader's
 * actual open only exchanges memory. The virtual source must never fetch inside its own
 * callbacks: that path runs on the reader's main task, where one request would freeze paging.
 */
static struct {
    char book[WR_ID];
    wr_chapter_t *chapters;
    unsigned count;
} s_open;

static bool wr_virtual_resolver(const char *path, book_epub_virtual_t *src) {
    char book_id[WR_ID] = {0};
    if (!path || !src || !wr_mark_read(path, book_id, NULL)) return false;
    if (!s_open.chapters || strcmp(s_open.book, book_id)) {
        /*
         * 从本地书架（或重启后）直接打开：内存里没有交接，但磁盘上通常还留着章节目录缓存。
         * 这里只读文件，不联网——开书发生在 UI 线程，一次请求就是一次明显卡顿。
         * Opened from the local shelf (or after a reboot): no in-memory handoff exists, but the
         * chapter list is usually still cached on disk. Read files only, never the network:
         * opening runs on the UI thread, where a request is a visible stall.
         */
        wr_book_t book;
        unsigned count = 0;
        memset(&book, 0, sizeof(book));
        snprintf(book.id, sizeof(book.id), "%s", book_id);
        wr_chapter_t *chapters = wr_chapter_list_cached(&book, &count);
        if (!chapters) {
            /*
             * 2026-11-19：目录缓存丢了（清理、换卡、异常）也不能让整本书打不开。标记文件里有
             * 全书章数，按它合成一份目录：标题用"第 N 章"占位，已缓存的章节照样能读，没缓存的
             * 走占位正文并触发后台补下。等下次联网同步到真目录，标题会自动恢复。
             * 2026-11-19: a lost TOC cache (cleanup, swapped card, crash) must not make the whole
             * book unopenable. The mark file holds the chapter count, so synthesize a TOC: generic
             * "第 N 章" titles, cached chapters still read, uncached ones use placeholders and wake
             * the background fetch. Real titles return on the next online sync.
             */
            unsigned mark_chapters = 0;
            if (wr_mark_read(path, NULL, &mark_chapters) && mark_chapters) {
                chapters = calloc(mark_chapters, sizeof(wr_chapter_t));
                if (chapters) {
                    for (unsigned i = 0; i < mark_chapters; ++i) {
                        chapters[i].index = i;
                        snprintf(chapters[i].title, sizeof(chapters[i].title), "第 %u 章", i + 1);
                    }
                    count = mark_chapters;
                    ESP_LOGW("WEREAD", "virtual open synthetic toc book=%s chapters=%u", book_id, count);
                }
            }
        }
        if (!chapters) {
            ESP_LOGW("WEREAD", "virtual open without handoff book=%s cached_toc=0", book_id);
            return false;
        }
        free(s_open.chapters);
        s_open.chapters = chapters;
        s_open.count = count;
        snprintf(s_open.book, sizeof(s_open.book), "%s", book_id);
        ESP_LOGI("WEREAD", "virtual open from disk toc book=%s chapters=%u", book_id, count);
        /* 从本地书架首次打开一本一章都没缓存的书，与从微读书架打开是同一个场景：
         * 本地没有位置可续，云端可能有。只登记，联网交给 worker 在第一次下载之前。 */
        /* Opening a book with nothing cached from the local shelf is the same situation as
         * opening it from the WeRead shelf: no local position to resume, maybe a cloud one. Just
         * register it; the network call belongs to the worker, before its first download. */
        unsigned ready = 0, total = 0;
        if (!wr_cache_summary(&book, &ready, &total) || !ready) (void)wr_cloud_seed_begin(book_id);
    }
    return wr_virtual_open(path, s_open.chapters, s_open.count, src);
}

/*
 * 目录与阅读页只通过这一组回调问"这一章在不在本地"，因此 UI 层无需知道微信读书的存在，
 * 普通 EPUB/TXT 也不会被问到这些问题（书源层按虚拟源身份过滤）。
 * The TOC and reader ask these callbacks whether a chapter is local, so the UI layer needs no
 * knowledge of WeRead, and plain EPUB/TXT are filtered out by the source layer.
 */
static book_ch_cache_t chapter_cache_state(size_t chapter) {
    if (!s_open.chapters || chapter >= s_open.count) return BOOK_CH_CACHE_UNKNOWN;
    wr_book_t book;
    memset(&book, 0, sizeof(book));
    snprintf(book.id, sizeof(book.id), "%s", s_open.book);
    wr_cache_state_t state = wr_cache_state(&book, (unsigned)chapter);
    if (state == WR_CS_READY) return BOOK_CH_CACHE_READY;
    if (state == WR_CS_FAILED) return BOOK_CH_CACHE_FAILED;
    /* 磁盘只记录"有没有"；"排着队/正在下"由等待标记与窗口的在途章表达。 */
    /* The disk only records presence; "queued/in flight" comes from the wait mark and the
     * chapter the window is fetching. */
    if (wr_read_waiting(s_open.book) == (int)chapter || s_fetching_chapter == (int)chapter)
        return BOOK_CH_CACHE_PENDING;
    return BOOK_CH_CACHE_MISSING;
}
static bool chapter_cache_request(size_t chapter) {
    if (!s_open.chapters || chapter >= s_open.count) return false;
    wr_prefetch_nudge((unsigned)chapter);
    return true;
}
/* 后台补下的章要让虚拟源就地认账，否则目录跳过去仍旧读到占位正文。 */
/* A chapter staged in the background must be adopted by the virtual source, or a TOC jump
 * still reads the placeholder body. */
static bool chapter_cache_refresh(size_t chapter) {
    return wr_virtual_refresh((unsigned)chapter);
}
static unsigned chapter_cache_epoch(void) { return wr_cache_ready_epoch(); }
static bool chapter_cache_progress(unsigned *done, unsigned *total, unsigned *percent) {
    (void)wr_status(NULL, 0, done, total);
    unsigned index = 0, stage = 0;
    /*
     * 只有"读者正等的那一章"的百分比能用：s_warm_index 是上一章留下的时会显示成上一章
     * 的 100%，卡片一出现就满格。对不上就让调用方按 done/total 显示整本书的进度。
     * Only the awaited chapter's figure may be shown: a stale index from the previous chapter
     * would appear as its 100% and the card would open already full. On mismatch the caller
     * falls back to done/total, i.e. the book's overall progress.
     */
    const int awaited = wr_read_waiting(s_open.book);
    if (percent) {
        *percent = (wr_warm_progress(&index, &stage) && (int)index == awaited) ? stage : 0;
    }
    return true;
}
static const book_chapter_cache_ops_t s_chapter_cache_ops = {
    .state = chapter_cache_state,
    .request = chapter_cache_request,
    .refresh = chapter_cache_refresh,
    .epoch = chapter_cache_epoch,
    .progress = chapter_cache_progress,
};

/* 还没被服务器确认的累计秒数（供"待补传"显示）。
 * Seconds still waiting for server confirmation, for the "pending top-up" readout. */
unsigned wr_pending_seconds(void) {
    unsigned total = 0;
    portENTER_CRITICAL(&s_lock);
    for (unsigned i = 0; i < WR_QUEUE_MAX; ++i) total += s_reports[i].seconds;
    portEXIT_CRITICAL(&s_lock);
    return total;
}

/* 手动补传入口：清掉退避时间并立刻唤醒 worker，用户点"立即同步"不必等 64 秒。
 * Manual top-up: clear backoff and wake the worker, so tapping "sync now" need not wait
 * out the retry ceiling. */
void wr_pending_kick(void) {
    portENTER_CRITICAL(&s_lock);
    for (unsigned i = 0; i < WR_QUEUE_MAX; ++i) if (s_reports[i].seconds) s_reports[i].retry_at = 0;
    portEXIT_CRITICAL(&s_lock);
    if (s_worker) xTaskNotifyGive(s_worker);
}

/* 锁屏/关机前强制把待补传队列落盘：浅睡虽保留 RAM，但 10 分钟后的硬关机（或意外掉电）会丢
 * 内存，所以在熄屏入口确保整队列已写盘，断网/关机都不丢时长。每次锁屏只写一次，不按 tick 磨损。
 * Force the pending queue to disk before lock/shutdown: light sleep retains RAM, but the later
 * hard power-off (or power loss) drops memory, so the lock path guarantees a durable copy. One
 * write per lock, never per tick. */
void wr_pending_persist(void) { queue_persist(); }

/* 在线且仍有未确认时长或未上报位置（含 worker 正在发送）；离线不门控。
 * Online with unconfirmed duration or unsynced position still queued/in-flight; no gate offline. */
bool wr_upload_pending(void) {
    if (!wr_online()) return false;
    if (wr_pending_seconds() > 0) return true;
    if (s_pos_count > 0) return true;
    return false;
}

/* 真正断电的关机前：补登当前阅读位置（在线 worker 立即传），并确保整队列落盘。
 * 锁屏浅睡保留 RAM、不调本函数。
 * Before a hard power-off, mark the current position (worker sends at once online) and make the
 * whole queue durable. Light-sleep lock retains RAM and does not call this. */
void wr_prepare_poweroff(void) {
    if (s_reading.attached && s_reading.book.id[0])
        pos_mark(s_reading.book.id, s_reading.chapter, s_reading.offset, s_reading.pct);
    queue_persist();
}

/* 关机/锁屏前等待上传排空：仅在线等待，kick 清退避并唤醒 worker，最多 wait_ms。
 * 返回 true=已排空；false=超时仍有残留（数据已落盘，下次开机续传，不致丢失）。
 * Wait for uploads to drain before lock/power-off: only waits online, kick clears backoff and
 * wakes the worker, bounded by wait_ms. true when drained; false on timeout (data is durable and
 * resumes next boot, so it is not lost). */
bool wr_wait_uploads_idle(int64_t wait_ms) {
    if (!wr_online()) return true;
    int64_t deadline = esp_timer_get_time() + wait_ms * 1000;
    wr_pending_kick();
    while (wr_upload_pending()) {
        if (esp_timer_get_time() >= deadline) return false;
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    return true;
}

uint32_t wr_confirm_seq(void) { return s_confirm_seq; }
unsigned wr_last_confirm_seconds(void) { return s_last_confirm_seconds; }

/* 待补传队列落盘。调用方不持锁，内部只做一次极短快照。
 * 原实现把待传秒数只放在内存里，重启即丢；落盘后断网/关机都不再丢时长。
 * Persist the pending queue. Called without the lock; only a very short snapshot inside.
 * The stock code kept pending seconds in RAM only, so a reboot lost them. */
static bool queue_persist(void) {
    WR_SD_SCOPE(WR_QUEUE);
    wr_queue_rec2_t rec[WR_QUEUE_MAX];
    unsigned count = 0;
    portENTER_CRITICAL(&s_lock);
    for (unsigned i = 0; i < WR_QUEUE_MAX; ++i) {
        if (!s_reports[i].seconds) continue;
        memcpy(rec[count].book_id, s_reports[i].book_id, sizeof(rec[count].book_id));
        memcpy(rec[count].owner, s_reports[i].owner, sizeof(rec[count].owner));
        rec[count].library = s_reports[i].library;
        rec[count].seconds = s_reports[i].seconds;
        rec[count].chapter = s_reports[i].chapter;
        rec[count].offset = s_reports[i].offset;
        rec[count].pct = s_reports[i].pct;
        ++count;
    }
    portEXIT_CRITICAL(&s_lock);
    if (!ensure_directory(WR_DIR)) return false;
    /* 队列空了就把文件删掉，别留空壳。开机还没成功读盘、且本会话也没成功写过盘时，内存里的
     * "空"可能只是没加载到，绝不能据此删掉盘上真实存在的待补传文件；只要本会话已成功写过
     * 一次盘，内存状态就是权威，清空即可安全删（记录是本会话内产生的情形）。
     * An empty queue removes the file. Do not unlink only when neither a boot-time restore nor a
     * successful this-session write has happened, since an in-RAM empty state may mean "not loaded". */
    if (!count) {
        if (!s_queue_restored && !s_queue_written) {
            ESP_LOGW("WEREAD", "skip empty-queue unlink before first successful restore");
            return true;
        }
        unlink(WR_QUEUE);
        return true;
    }
    FILE *file = fopen(WR_QUEUE_TEMP, "wb");
    if (!file) return false;
    bool ok = fwrite(WR_QUEUE_MAGIC, 1, 8, file) == 8 &&
              fwrite(&count, sizeof(count), 1, file) == 1 &&
              fwrite(rec, sizeof(wr_queue_rec2_t), count, file) == count;
    if (fflush(file) || fsync(fileno(file))) ok = false;
    if (fclose(file)) ok = false;
    if (ok) {
        /* FAT 的 rename 不能覆盖已存在目标，沿用书架索引的两步替换法。
         * FAT rename cannot replace an existing destination; reuse the two-step swap. */
        const char *backup = WR_QUEUE_BACKUP;
        unlink(backup);
        bool moved = rename(WR_QUEUE, backup) == 0;
        ok = (moved || access(WR_QUEUE, F_OK) != 0) && rename(WR_QUEUE_TEMP, WR_QUEUE) == 0;
        if (ok) unlink(backup);
        else if (moved) rename(backup, WR_QUEUE);
    }
    if (!ok) unlink(WR_QUEUE_TEMP);
    if (ok) {
        /* 标记本会话已成功写盘：此后内存状态权威，队列清空时可安全删文件。 */
        s_queue_written = true;
        ESP_LOGI("WEREAD", "pending queue saved items=%u seconds=%u",
                 count, wr_pending_seconds());
    }
    return ok;
}

/* 启动时把落盘的待补传队列装回内存槽位，并立刻唤一次 worker 去补传。
 * Restore the persisted queue into the RAM slots at boot and wake the worker once. */
static void queue_restore(void) {
    WR_SD_SCOPE(WR_QUEUE);
    if (s_queue_restored) return;
    FILE *file = fopen(WR_QUEUE, "rb");
    /* 打不开多半是 SD 还没挂载，留着下次再试，不要把"没读到"当成"队列为空"。 */
    if (!file) return;
    s_queue_restored = true;
    char magic[8];
    unsigned count = 0;
    /* 两种格式共用一块缓冲：现行 WRQUEUE2 记录更大，union 按它对齐，不额外占栈。
     * One shared buffer for both formats: the union is sized to the larger WRQUEUE2 record. */
    union {
        wr_queue_rec2_t cur[WR_QUEUE_MAX];
        wr_queue_rec_t legacy[WR_QUEUE_MAX];
    } rec;
    bool ok = fread(magic, 1, 8, file) == 8;
    bool is_current = false, is_legacy = false;
    if (ok) {
        is_current = !memcmp(magic, WR_QUEUE_MAGIC, 8);
        is_legacy = !memcmp(magic, WR_QUEUE_MAGIC_LEGACY, 8);
        ok = (is_current || is_legacy) &&
             fread(&count, sizeof(count), 1, file) == 1 && count <= WR_QUEUE_MAX;
        if (ok && count) {
            if (is_current)
                ok = fread(rec.cur, sizeof(wr_queue_rec2_t), count, file) == count;
            else
                ok = fread(rec.legacy, sizeof(wr_queue_rec_t), count, file) == count;
        }
    }
    fclose(file);
    if (!ok) {
        /* 读取失败不直接删除：先改名备份（.prev），保住排查与手工恢复的可能。
         * Back up instead of deleting on a bad read so the data is not lost outright. */
        ESP_LOGW("WEREAD", "pending queue unreadable, backing up rather than dropping");
        unlink(WR_QUEUE_BACKUP);
        if (rename(WR_QUEUE, WR_QUEUE_BACKUP) != 0) {
            ESP_LOGW("WEREAD", "backup rename failed errno=%d, removing", errno);
            unlink(WR_QUEUE);
        }
        return;
    }
    if (!count) return;
    portENTER_CRITICAL(&s_lock);
    memset(s_reports, 0, sizeof(s_reports));
    for (unsigned i = 0; i < count; ++i) {
        memcpy(s_reports[i].book_id, is_current ? rec.cur[i].book_id : rec.legacy[i].book_id,
               sizeof(s_reports[i].book_id));
        memcpy(s_reports[i].owner, is_current ? rec.cur[i].owner : rec.legacy[i].owner,
               sizeof(s_reports[i].owner));
        if (is_current) {
            s_reports[i].library = rec.cur[i].library;
            s_reports[i].seconds = rec.cur[i].seconds;
            /* 现行格式恢复真实位置；旧格式没有，保持 memset 的 0（与旧行为一致）。
             * Current format restores the real position; legacy keeps the zeroed 0/0 as before. */
            s_reports[i].chapter = rec.cur[i].chapter;
            s_reports[i].offset = rec.cur[i].offset;
            s_reports[i].pct = rec.cur[i].pct;
        } else {
            s_reports[i].library = rec.legacy[i].library;
            s_reports[i].seconds = rec.legacy[i].seconds;
        }
        /* 从盘上恢复的记录都是上一轮没发出去的，按补传上报。 */
        s_reports[i].resend = true;
    }
    portEXIT_CRITICAL(&s_lock);
    unsigned total = 0;
    for (unsigned i = 0; i < count; ++i)
        total += is_current ? rec.cur[i].seconds : rec.legacy[i].seconds;
    ESP_LOGI("WEREAD", "pending queue restored items=%u seconds=%u format=%s",
             count, total, is_current ? "v2" : "legacy");
    if (s_worker) xTaskNotifyGive(s_worker);
}

void wr_init(void) {
    /* 离线时长授权凭证在 SD 卡上，尽早恢复一次，后面的计时/补传才有凭据可用。 */
    wr_consent_init();
    /* 落盘的待补传队列也尽早装回来，否则重启就丢时长（这正是以前补不满的根因）。 */
    queue_restore();
    book_set_chapter_cache_ops(&s_chapter_cache_ops);
    if (s_worker) return;
    if (!s_cookie_mutex) s_cookie_mutex = xSemaphoreCreateMutexStatic(&s_cookie_mutex_storage);
    if (!s_cookie_mutex) {
        ESP_LOGE("WEREAD", "cookie mutex initialization failed");
        return;
    }
    if (!s_http_mutex) s_http_mutex = xSemaphoreCreateMutexStatic(&s_http_mutex_storage);
    if (!s_http_mutex) {
        ESP_LOGE("WEREAD", "http mutex initialization failed");
        return;
    }
    /* 断连/关 WiFi 即清理 web 长连接，避免拖到离线后 pbuf 回收回调变野指针。
     * Drop the warm web client on link loss/stop while the netif lives. */
    esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED,
                               http_keepalive_link_event, NULL);
    esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_STOP,
                               http_keepalive_link_event, NULL);
    book_set_virtual_resolver(wr_virtual_resolver);
    s_worker = xTaskCreateStatic(worker_task, "weread", sizeof(s_worker_stack),
                                 NULL, 2, s_worker_stack, &s_worker_tcb);
    if (s_worker) ESP_LOGI("WEREAD", "worker ready stack=%u", (unsigned)sizeof(s_worker_stack));
    else ESP_LOGE("WEREAD", "worker initialization failed");
}

/* ---- 阅读进度位置同步（退出即标记，离线落盘，联网自动传）。 ---- */
static void pos_persist(void) {
    WR_SD_SCOPE(WR_POS_FILE);
    if (!scope.allowed) return;
    if (!ensure_directory(WR_DIR)) return;
    FILE *file = fopen(WR_POS_FILE, "wb");
    if (!file) return;
    bool ok = fwrite(WR_POS_MAGIC, 1, 8, file) == 8 &&
              fwrite(&s_pos_count, sizeof(s_pos_count), 1, file) == 1 &&
              (!s_pos_count || fwrite(s_pos_pending, sizeof(*s_pos_pending), s_pos_count, file) == s_pos_count);
    fclose(file);
    if (!ok) unlink(WR_POS_FILE);
}

static void pos_restore(void) {
    if (s_pos_restored) return;
    s_pos_restored = true;
    WR_SD_SCOPE(WR_POS_FILE);
    if (!scope.allowed) return;
    FILE *file = fopen(WR_POS_FILE, "rb");
    if (!file) return;
    char magic[8];
    unsigned count = 0;
    if (fread(magic, 1, 8, file) == 8 && !memcmp(magic, WR_POS_MAGIC, 8) &&
        fread(&count, sizeof(count), 1, file) == 1 && count <= WR_POS_MAX) {
        if (fread(s_pos_pending, sizeof(*s_pos_pending), count, file) == count) s_pos_count = count;
    }
    fclose(file);
}

/* 记录一本书最近一次阅读位置：同书覆盖、异书追加，队列满则丢弃最旧一条。 */
static void pos_mark(const char *book_id, unsigned chapter, unsigned offset, unsigned pct) {
    pos_restore();
    for (unsigned i = 0; i < s_pos_count; ++i) {
        if (!strcmp(s_pos_pending[i].book_id, book_id)) {
            s_pos_pending[i].chapter = chapter;
            s_pos_pending[i].offset = offset;
            s_pos_pending[i].pct = pct > 100 ? 100 : pct;
            pos_persist();
            if (s_worker) xTaskNotifyGive(s_worker);
            return;
        }
    }
    if (s_pos_count >= WR_POS_MAX) {
        memmove(&s_pos_pending[0], &s_pos_pending[1],
                (WR_POS_MAX - 1) * sizeof(*s_pos_pending));
        s_pos_count = WR_POS_MAX - 1;
    }
    wr_pos_rec_t *rec = &s_pos_pending[s_pos_count++];
    memset(rec, 0, sizeof(*rec));
    snprintf(rec->book_id, sizeof(rec->book_id), "%s", book_id);
    rec->chapter = chapter; rec->offset = offset;
    rec->pct = pct > 100 ? 100 : pct;
    pos_persist();
    if (s_worker) xTaskNotifyGive(s_worker);
}

/* worker 调用：在线时把待传位置逐本上报，成功即移除；离线或失败则保留等下一轮。 */
static void pos_flush(void) {
    if (!s_pos_count) return;
    if (!wr_online()) return;
    bool changed = false;
    for (int i = (int)s_pos_count - 1; i >= 0; --i) {
        const wr_pos_rec_t *rec = &s_pos_pending[i];
        wr_chapter_t chapter;
        if (!report_local_chapter(rec->book_id, rec->chapter, &chapter)) continue;
        if (wr_send_position(rec->book_id, &chapter, rec->offset, rec->pct)) {
            unsigned after = (unsigned)(s_pos_count - 1 - i);
            memmove(&s_pos_pending[i], &s_pos_pending[i + 1],
                    after * sizeof(*s_pos_pending));
            --s_pos_count;
            changed = true;
            ESP_LOGI("WEREAD", "position synced book=%s chapter=%u offset=%u",
                     rec->book_id, rec->chapter + 1, rec->offset);
        }
    }
    if (changed) pos_persist();
}

void wr_reading_start(const char *path, bool enabled, int64_t now_ms) {
    wr_reading_stop(true);
    /* SD 卡此时一定已挂载，补一次读盘，防止 wr_init 时卡还没就绪。
     * The card is certainly mounted by now; retry the restore in case wr_init ran too early. */
    queue_restore();
    if (!enabled || !wr_reporting_book(path, &s_reading.book)) return;
    wr_account_t account;
    bool have_web = read_account(&account) && account.token[0];
    if (have_web) {
        snprintf(s_reading.owner, sizeof(s_reading.owner), "%s", account.owner);
        s_reading.library = account.library;
    } else {
        /* 网页会话失效（cookie token 为空）但离线 consent 授权还在：改用 consent 账号 vid
         * 继续离线计时，之后通过官方 /book/read 补传，不再因为网页 token 没了就一秒都不记。
         * Web session gone but consent valid: count against the consent vid and upload via
         * the official channel instead of refusing to time anything at all. */
        char vid[48];
        if (!wr_consent_authorized() || !wr_consent_vid(vid, sizeof(vid))) {
            memset(&s_reading, 0, sizeof(s_reading));
            return;
        }
        snprintf(s_reading.owner, sizeof(s_reading.owner), "%s", vid);
        s_reading.library = 0;
        ESP_LOGI("WEREAD", "reading attached via consent vid=%s (web token absent)", vid);
    }
    s_reading.attached = true;
    s_reading.last = now_ms;
    // 需求：这次阅读的上报期间保持 Wi-Fi 启用。
    radio_hold(true);
}

static void report_reading(void) {
    const unsigned seconds = s_reading.ms / 1000;
    if (!seconds || !s_reading.owner[0] || !s_worker) return;
    wr_report_t job = { .library = s_reading.library, .seconds = seconds,
                        .chapter = s_reading.chapter, .offset = s_reading.offset,
                        .pct = s_reading.pct, .resend = !wr_online() };
    memcpy(job.book_id, s_reading.book.id, sizeof(job.book_id));
    memcpy(job.owner, s_reading.owner, sizeof(job.owner));
    portENTER_CRITICAL(&s_lock);
    wr_report_t *empty = NULL, *entry = NULL;
    for (unsigned i = 0; i < sizeof(s_reports)/sizeof(s_reports[0]); ++i) {
        if (!s_reports[i].seconds) {
            if (!empty) empty = &s_reports[i];
        } else if (same_report_book(&job, &s_reports[i])) {
            entry = &s_reports[i];
            break;
        }
    }
    bool accepted = false;
    if (entry && seconds <= UINT32_MAX - entry->seconds) {
        entry->seconds += seconds;
        /* 合并多批时把位置刷新到最近一次阅读处，避免长离线后仍报最早的冻结位置。 */
        entry->chapter = job.chapter;
        entry->offset = job.offset;
        entry->pct = job.pct;
        /* 任一批来自离线/补传，合并后整批按补传上报。 */
        entry->resend = entry->resend || job.resend;
        accepted = true;
    } else if (!entry && empty) {
        *empty = job;
        accepted = true;
    }
    portEXIT_CRITICAL(&s_lock);
    if (!accepted) return;
    s_reading.ms %= 1000;
    xTaskNotifyGive(s_worker);
    /* 离线入队即落盘：没网时这批秒数没法立刻发，先写进队列，硬复位/断电（电池耗尽）也不丢。
     * 仅在离线时写，在线链路保持原样、不增加任何写盘；每批约 60s 才触发一次，不磨损 SD。
     * Persist only while offline: with no link the seconds cannot be sent now, so save them first
     * and survive a hard reset or battery death. The online path is untouched (no extra write);
     * this fires once per ~60 s batch, so it does not wear the SD. */
    if (!wr_online()) {
        /* 断网落盘：把仍在排队的批次一律标补传，覆盖"在线产生、尚未发出就断网"的情况。 */
        portENTER_CRITICAL(&s_lock);
        for (unsigned i = 0; i < sizeof(s_reports)/sizeof(s_reports[0]); ++i)
            if (s_reports[i].seconds) s_reports[i].resend = true;
        portEXIT_CRITICAL(&s_lock);
        queue_persist();
    }
}

void wr_reading_tick(bool active, int64_t now_ms, unsigned chapter, unsigned offset, unsigned pct) {
    if (!s_reading.attached) return;
    if (active) {
        // 位置变化（翻页/跳转）：重置单页停留计时；位置不变则继续累计本页停留。
        // Reset the dwell accumulator when the reading position changes.
        if (s_reading.dwell_chapter != chapter || s_reading.dwell_offset != offset) {
            s_reading.dwell_chapter = chapter;
            s_reading.dwell_offset = offset;
            s_reading.dwell_ms = 0;
        }
        s_reading.chapter = chapter; s_reading.offset = offset; s_reading.pct = pct;
    }
    if (!s_reading.last) { s_reading.last = now_ms; return; }
    int64_t elapsed = now_ms - s_reading.last;
    s_reading.last = now_ms;
    // 断网只停发送不停计时：连接恢复后把这段一起补上（需求 4「暂停发送，恢复后继续」）。
    if (active && elapsed > 0 && elapsed <= 5000) {
        // 单页停留上限：只计入本页剩余额度，超出部分丢弃，实现“停留太久计时停止”。
        // Count only the remaining dwell budget for this page; excess is dropped.
        int64_t room = (int64_t)WR_DWELL_CAP_MS - s_reading.dwell_ms;
        if (room > 0) {
            int64_t counted = elapsed < room ? elapsed : room;
            s_reading.dwell_ms += counted;
            s_reading.ms += (unsigned)counted;
        }
    }
    if (!active) radio_hold(false);
    else if (wr_online()) radio_hold(true);
    if (s_reading.ms >= 60000) report_reading();
}

void wr_reading_stop(bool close) {
    bool was_attached = s_reading.attached;
    if (was_attached) report_reading();
    if (close && s_reading.ms >= 1000)
        ESP_LOGW("WEREAD", "report queue unavailable at close seconds=%u", s_reading.ms / 1000);
    /* 退出阅读：把本地真实位置登记为待同步（在线 worker 立即传，离线落盘、有网自动传）。
     * 必须在 memset 之前，且用 s_reading 里的最新位置，不看云端过期进度。 */
    if (close && was_attached && s_reading.book.id[0])
        pos_mark(s_reading.book.id, s_reading.chapter, s_reading.offset, s_reading.pct);
    radio_hold(false);
    s_reading.last = 0;
    if (close) memset(&s_reading, 0, sizeof(s_reading));
}

/* ------------------------------------------------------------------ */
/* 分章阅读：书架直接打开                                              */
/* ------------------------------------------------------------------ */

/* 书名记录只接受 ≤120 字节的合法 UTF-8；微读书名常是长中文，直接传会被静默拒绝，
 * 书库就退回显示文件名。这里按字符边界截断并记录失败原因。
 * The title record only accepts valid UTF-8 up to 120 bytes; long Chinese titles were being
 * rejected silently, so the library fell back to the on-disk name. */
static bool store_book_title(const char *path, const char *title) {
    if (!path || !path[0] || !title || !title[0]) return false;
    char clean[121];
    size_t n = strlen(title);
    if (n > sizeof(clean) - 1) {
        n = sizeof(clean) - 1;
        while (n > 0 && ((unsigned char)title[n] & 0xc0) == 0x80) --n; // 退到字符起始
    }
    memcpy(clean, title, n);
    clean[n] = 0;
    while (n > 0 && clean[n - 1] == ' ') clean[--n] = 0;
    esp_err_t err = book_title_set(path, clean);
    if (err != ESP_OK) ESP_LOGW("WEREAD", "title store failed book=%s err=%s", path, esp_err_to_name(err));
    return err == ESP_OK;
}

bool wr_open_book(const wr_book_t *book, char path[WR_PATH], unsigned *start) {
    if (!book || !book->id[0] || !path) return false;
    char owner[WR_ID] = {0};
    if (!wr_account(owner, NULL, NULL)) { wr_error_set("需要先登录微信读书"); return false; }
    /* 这条路径跑在 UI 线程，绕过了 worker 每轮开头的会话恢复；缺 Cookie 的请求会被服务端
     * 判成"用户不存在"（errCode -2010），界面上只能显示成模糊的目录获取失败。
     * 恢复函数自带锁且已有值就返回，重复调用无副作用。
     * This runs on the UI thread and skips the worker's per-job session restore; a request
     * without cookies is answered with -2010 and surfaces as a vague TOC failure. The restore
     * helper takes the lock and returns early when cookies are already present. */
    restore_session_cookie();
    if (!owned_path(owner, book->id, path)) { wr_error_set("书籍标识无效"); return false; }
    WR_SD_SCOPE(path);
    if (!scope.allowed) { wr_error_set("SD 卡当前不可用"); return false; }
    unsigned count = 0;
    wr_chapter_t *chapters = wr_chapter_list(book, &count);
    if (!chapters || !count) {
        free(chapters);
        /* 目录已经在盘上的书离线也能打开；只有第一次打开、必须联网取目录时才拦下，并且说清
         * 是"没网络"而不是含糊的"目录获取失败"。
         * A book whose TOC is already on disk opens offline; only a first open that needs the TOC
         * over the network is refused, and it says "no network" instead of a vague fetch failure. */
        wr_error_set(wr_online() ? "章节目录获取失败，请稍后重试"
                                 : "网络未连接，请先在快捷菜单连上 Wi-Fi");
        return false;
    }
    if (!wr_cache_dir(book)) {
        wr_error_set("无法创建章节缓存目录");
        free(chapters);
        return false;
    }
    // 标记文件必须先落盘：书架、历史与进度都按"路径存在且有长度"判断这本书能否打开。
    // The marker must land first: shelf, history and progress all gate on an existing path.
    if (!wr_mark_write(book, path, count)) {
        wr_error_set("无法写入书籍占位文件，请检查 SD 卡");
        free(chapters);
        return false;
    }
    /* 书名必须显式写进书目记录：标记文件本身没有 OPF 元数据，不写的话阅读器和书架
     * 只能显示磁盘文件名（wr-<owner>+<bookid>.epub）。与整书缓存成功后的写法保持一致。
     * The marker carries no OPF metadata, so the shelf title has to be stored explicitly;
     * otherwise the reader falls back to the on-disk name (wr-<owner>+<id>.epub). */
    if (book->title[0]) (void)store_book_title(path, book->title);
    free(s_open.chapters);
    s_open.chapters = chapters;
    s_open.count = count;
    snprintf(s_open.book, sizeof(s_open.book), "%s", book->id);
    if (start) {
        *start = 0;
        book_progress_t local;
        struct stat info;
        if (!stat(path, &info) && book_progress_load(path, (uint32_t)info.st_size, &local) &&
            local.chapter < count) *start = local.chapter;
    }
    /*
     * 一章都没落盘的书：本地没有什么可续读的位置，而手机上的进度可能在几十章之外，
     * 先让后台问一次云端进度，阅读页据此把等待的章节换成云端那一章；云端也没有就落在第一章。
     * 已经缓存过章节的书不走这条：本地进度就是读者在这台机器上的真实位置。
     * A book with nothing cached has no local position worth honouring, while the phone may be
     * dozens of chapters ahead: let the background ask the cloud once and let the reader move its
     * wait there; without a cloud position, chapter one stands. Books that already have chapters
     * cached keep the local position, which is where this device actually stopped.
     */
    wr_cloud_seed_ack();
    unsigned ready = 0, total = 0;
    if (!wr_cache_summary(book, &ready, &total) || !ready) {
        if (wr_cloud_seed_begin(book->id))
            ESP_LOGI("WEREAD", "first open uncached book=%s, cloud position queued", book->id);
    }
    ESP_LOGI("WEREAD", "open book=%s chapters=%u start=%u", book->id, count, start ? *start : 0);
    return true;
}

/*
 * 取走"下载进度后要打开的章节"。一次有效：界面读完就清空，避免下次进书架又跳一次。
 * Take the chapter to open after a progress download. One shot: the UI clears it on read, so
 * returning to the shelf later never jumps again.
 */
bool wr_progress_take_target(unsigned *chapter) {
    bool found;
    portENTER_CRITICAL(&s_lock);
    found = s_progress_target != UINT_MAX;
    if (found && chapter) *chapter = s_progress_target;
    s_progress_target = UINT_MAX;
    portEXIT_CRITICAL(&s_lock);
    return found;
}

bool wr_cloud_seed_begin(const char *book_id) {
    if (!book_id || !book_id[0] || strnlen(book_id, WR_ID) >= WR_ID) return false;
    /* 离线时无从解析：直接让阅读页按第一章开始，不要让它等一个永远不来的结果。 */
    /* Nothing can be resolved offline: let the reader start at chapter one instead of waiting
     * for an answer that will never come. */
    if (!wr_online()) return false;
    portENTER_CRITICAL(&s_lock);
    s_seed_state = WR_SEED_PENDING;
    s_seed_chapter = s_seed_offset = 0;
    portEXIT_CRITICAL(&s_lock);
    snprintf(s_seed_book, sizeof(s_seed_book), "%s", book_id);
    return true;
}

wr_seed_t wr_cloud_seed_state(unsigned *chapter, unsigned *offset) {
    portENTER_CRITICAL(&s_lock);
    wr_seed_t state = s_seed_state;
    if (state == WR_SEED_FOUND) {
        if (chapter) *chapter = s_seed_chapter;
        if (offset) *offset = s_seed_offset;
    }
    portEXIT_CRITICAL(&s_lock);
    return state;
}

void wr_cloud_seed_ack(void) {
    portENTER_CRITICAL(&s_lock);
    s_seed_state = WR_SEED_IDLE;
    portEXIT_CRITICAL(&s_lock);
}

bool wr_prefetch_running(void) {    portENTER_CRITICAL(&s_lock);
    bool running = s_result == WR_BUSY && s_active_job == WR_PREFETCH;
    portEXIT_CRITICAL(&s_lock);
    return running;
}

void wr_prefetch_nudge(unsigned chapter) {
    /* 首开还在等云端进度时不登记这一章：真实目标马上会由解析结果给出，
     * 先登记反而让 worker 抢着下第一章，正是要避免的浪费。 */
    /* While a first-open resolution is pending, do not register this chapter: the real target
     * arrives moments later, and registering now is what sends the worker after chapter one —
     * exactly the waste the resolution exists to avoid. */
    bool seeding;
    portENTER_CRITICAL(&s_lock);
    seeding = s_seed_state == WR_SEED_PENDING && !strcmp(s_seed_book, s_open.book);
    portEXIT_CRITICAL(&s_lock);
    if (!seeding) wr_read_wait(s_open.book, chapter);
    if (!s_open.chapters) return;
    wr_book_t book;
    memset(&book, 0, sizeof(book));
    snprintf(book.id, sizeof(book.id), "%s", s_open.book);
    if (wr_prefetch_running()) {
        /* 同一本书的预取已经在跑：它每轮都会重新读"读者等待章"，窗口会自己重锚，
         * 再排一次只会把正在下的一章掐断重来。换了书则必须交给 wr_start 去抢占。 */
        /* The same book's prefetch is already running: it re-reads the awaited chapter every
         * round, so the window re-anchors by itself and queueing again would only cut the
         * chapter in flight. A different book has to go through wr_start and preempt. */
        char active_id[WR_ID] = {0};
        portENTER_CRITICAL(&s_lock);
        snprintf(active_id, sizeof(active_id), "%s", s_work.book.id);
        portEXIT_CRITICAL(&s_lock);
        if (!strcmp(active_id, book.id)) return;
    }
    if (wr_start(WR_PREFETCH, &book, chapter, 0)) return;
    /* 排不进去（worker 正跑着别的任务）就登记下来，由 worker 收尾时补跑。
     * 早先这里是直接 return，"等它结束"从来没有实现——读者只能看着等待页空转到 6 秒。 */
    /* If it cannot be queued (the worker is on another job), park it and let the worker re-run
     * it when the round ends. This used to be a bare return, and "wait for it to finish" was
     * never implemented: all the reader saw was the waiting page spinning for six seconds. */
    park_prefetch(&book, chapter);
}

/*
 * 退出书籍：停掉这本书的缓存任务。
 *   · 预取/整书缓存正在下这本书 → 让它在本章边界收手，worker 立刻空出来；
 *   · 排队中的、登记待补的、以及内存里的开书交接一并清掉，避免旧书的任务抢在新书前面。
 * 只针对这一本书：换书时新书的预取可能已经起来了，不能被当成旧任务杀掉。非微读书籍是空操作。
 *
 * Leaving the book: stop its cache jobs. A prefetch or whole-book cache running for this book
 * backs off at the next chapter boundary, freeing the worker at once; a queued, parked or
 * open-handoff reference to the same book is dropped so it cannot run ahead of the next book.
 * Scoped to this one book: when switching books the new book's prefetch may already be running
 * and must not be killed as if it were the old one. A non-WeRead path is a no-op.
 */
bool wr_cache_repair(const char *path, unsigned index) {
    wr_book_t book;
    if (!path || !wr_reporting_book(path, &book)) return false;
    return wr_stage_chapter_repair(&book, index);
}

void wr_cache_pause(const char *path) {
    wr_book_t book;
    if (!path || !wr_reporting_book(path, &book)) return;
    bool active = false;
    portENTER_CRITICAL(&s_lock);
    if (s_queued_pending && !strcmp(s_queued.book.id, book.id)) s_queued_pending = false;
    if (s_prefetch_pending && !strcmp(s_prefetch_book.id, book.id)) s_prefetch_pending = false;
    active = s_result == WR_BUSY && !strcmp(s_work.book.id, book.id) &&
             (s_active_job == WR_PREFETCH || s_active_job == WR_CACHE);
    portEXIT_CRITICAL(&s_lock);
    /* 内存里的开书交接只在这本书自己身上清。换书时 s_open 已经指向新书，
     * 一并清掉就等于把新书刚备好的目录扔了——阅读器还能退回磁盘目录，但白费一次读盘。 */
    /* Only drop the in-memory handoff when it belongs to this book. On a book switch s_open
     * already points at the new book, and clearing it would throw away the chapter list just
     * prepared for it (the reader falls back to the disk TOC: works, but a wasted read). */
    if (s_open.chapters && !strcmp(s_open.book, book.id)) {
        wr_read_wait_clear();
        free(s_open.chapters);
        s_open.chapters = NULL;
        s_open.count = 0;
        s_open.book[0] = 0;
    }
    if (active) {
        atomic_store(&s_cancel, true);
        ESP_LOGI("WEREAD", "cache paused for closed book=%s", book.id);
    }
}
