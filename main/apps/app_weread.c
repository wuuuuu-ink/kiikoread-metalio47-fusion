#include "app_weread.h"

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "app_content_open.h"
#include "app_transfer_mode.h"
#include "book_cover.h"
#include "book_progress.h"
#include "book_store.h"
#include "esp_heap_caps.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "settings.h"
#include "ui_gesture.h"
#include "ui_kit.h"
#include "ui_nav.h"
#include "ui_root_style.h"
#include "ui_wifi_qr.h"
#include "weread.h"
#include "weread_cache.h"
#include "weread_consent.h"
#include "weread_internal.h"

extern const app_desc_t app_book, app_transfer;

typedef enum { SHELF, ACCOUNT, ACTIONS, LOGOUT, ASK_WIFI, WORK, CONFIRM_DEL, SYNC, CONSENT,
               CACHE_SCOPE } wr_view_t;
#define WR_TAG "WEREAD"
/* 书架槽位由样式决定：网格 9、列表 8。数组按上限开，用前先夹紧，改样式不会越界。 */
#define WR_SLOTS 9
static wr_view_t s_view;
static wr_job_t s_job, s_after_wifi;
static bool s_wait_wifi, s_wifi_open, s_from_home, s_back_to_weread, s_return_once;
/*
 * "网络未连接，要不要连一下"这张卡片：读者点了阅读/缓存，但这本书还没落到本地书架、目录也只能
 * 从云端取，离线时那条路必然失败。以前直接去取，取不到就报"获取目录失败"，读者不知道要连网。
 * s_open_after_wifi 记住"连上之后把刚才那次打开补上"。
 * The "no network — connect?" sheet: the reader asked to read or cache, but the book is not on the
 * local shelf yet and its TOC only lives in the cloud, so an offline attempt cannot succeed. It
 * used to try anyway and report "TOC fetch failed", which never said "you are offline".
 * s_open_after_wifi remembers to resume the open once the link is up.
 */
static bool s_ask_connect, s_open_after_wifi;
/* 连接倒计时的起点：卡片上写着"正在连接 Wi-Fi…"，可它不会自己结束。20 秒还没连上就把按钮
 * 放回来，读者能再试一次或取消，而不是盯着一句永远不变的提示。
 * Start of the connect countdown: the sheet says "connecting…" and never ends by itself. After
 * 20 s with no link the buttons come back so the reader can retry or cancel instead of staring at
 * a message that never changes. */
static int64_t s_wifi_wait_ms;
static app_redraw_t open_selected(app_ctx_t *ctx);
static bool s_check_pending;
/* 后台静默登录验证：s_bg_check 标记 WR_CHECK 正在后台跑（不切 WORK 视图、不弹窗）；
   s_login_invalid 为已确认的登录失效，驱动书架顶部那行提示。 */
static bool s_bg_check, s_login_invalid;
static int64_t s_session_verified_us;
static int s_page, s_count;
static unsigned s_selected_index;
/* 书架列表仅在普通任务中访问（无指针/无 DMA），放 PSRAM 腾出约 6.7KB 内部 RAM。 */
static EXT_RAM_BSS_ATTR wr_book_t s_books[WR_SLOTS], s_selected;
/// 缓存范围子页里算出来的起始章：进页时读一次本地进度，渲染/点击都不再碰 SD。
/// Start chapter for the cache-scope sheet, read once on entry so redraws never touch the SD.
static unsigned s_scope_chapter;
/// 范围任务的起始章与目标章数，完成时用来拼"已新下 N 章（第 X 章起）"这类实话。
/// Start chapter and target count of the ranged job, for an honest completion message.
static unsigned s_range_from, s_range_span;
static char s_done_message[72];
/*
 * 书架的缓存状态不再只有"有/没有"两档：分章缓存的书可能只下了一部分。
 * 逐本逐章 stat 太重，所以读后台维护的概况文件（已就绪章数 / 全书章数）。
 * The shelf no longer has just cached/not-cached: a chapter-cached book may be partial.
 * Per-chapter stat() is too heavy, so read the summary the background job maintains.
 */
typedef enum { WR_SHELF_NONE = 0, WR_SHELF_FULL, WR_SHELF_PARTIAL } wr_shelf_cache_t;
static wr_shelf_cache_t s_cache_kind[WR_SLOTS];
static unsigned s_cache_ready[WR_SLOTS], s_cache_total[WR_SLOTS];
static bool s_selected_full, s_logged;
static int s_pressed = -1;          /* 0 同步 / 1 账户：右上控件的按下反馈 */
static int64_t s_pressed_until;
static int s_pending_action = -1;   /* 按下反馈画完后要执行的动作 */
static wr_job_t s_retry_job;        /* 失败后允许一键重试的动作 */
static bool s_has_retry;
static char s_open[WR_PATH], s_offline[WR_PATH], s_qr[180];static const char *s_message;
static uint8_t *s_cover_pixels;
static unsigned s_done, s_total;

/* 补传成功弹窗：格式化文案缓冲，以及上次已提示过的确认序号。
 * Success popup: static buffer for the formatted text and the confirmation sequence already
 * shown, so a new confirmation pops exactly once. */
static char s_confirm_text[96];
static uint32_t s_confirm_seen;
static bool s_confirm_seen_init;
/* 同步页上一次画出去的待补传秒数：只有数字真变了才重绘整屏。
 * The pending-seconds value last painted on the sync page; repaint only when it changes. */
static unsigned s_pending_shown;
/* 授权页上一次绘制的状态与二维码，用于检测变化、避免空刷。
 * Last painted consent state and QR, used to detect changes and avoid blank refreshes. */
static wr_consent_state_t s_consent_shown = WR_CONSENT_IDLE;
static char s_consent_qr[256];
/* 自动补传的限频时刻。/ Timestamp for rate-limiting the automatic top-up kick. */
static int64_t s_kick_at;
/* consent 自动 mint 网页会话的限频时刻，失败时按间隔重试，不反复发请求。 */
static int64_t s_mint_at;

#define WR_SESSION_VERIFY_TTL_US (60LL * 1000LL * 1000LL)

static bool session_verified_recently(void) {
    int64_t verified = s_session_verified_us;
    return verified > 0 && esp_timer_get_time() - verified < WR_SESSION_VERIFY_TTL_US;
}

static void session_verification_result(wr_result_t status) {
    if (status == WR_OK) s_session_verified_us = esp_timer_get_time();
    else if (status == WR_EXPIRED) s_session_verified_us = 0;
}

static int pages(void) {
    int slots = app_book_shelf_slots();
    return s_count ? (s_count+slots-1)/slots : 1;
}

static void refresh_shelf(void) {
    s_logged = wr_account(NULL, NULL, NULL);
    s_count = wr_shelf_count();
    if (s_page >= pages()) s_page = pages()-1;
    int slots = app_book_shelf_slots();
    if (slots > WR_SLOTS) slots = WR_SLOTS;
    for (int i = 0; i < slots; ++i) {
        char path[WR_PATH];
        memset(&s_books[i], 0, sizeof(s_books[i]));
        s_cache_kind[i] = WR_SHELF_NONE;
        s_cache_ready[i] = s_cache_total[i] = 0;
        if (!wr_shelf_get(s_page*slots+i, &s_books[i])) continue;
        if (wr_book_path(&s_books[i], path)) s_cache_kind[i] = WR_SHELF_FULL; // 整本 EPUB
        else if (wr_cache_summary(&s_books[i], &s_cache_ready[i], &s_cache_total[i]))
            s_cache_kind[i] = s_cache_ready[i] >= s_cache_total[i] ? WR_SHELF_FULL : WR_SHELF_PARTIAL;
    }
}

/* 进入 consent 登录（唯一登录入口）：未授权即发起墨水屏扫码，已授权则显示管理页。 */
static app_redraw_t enter_consent_login(void) {
    s_view = CONSENT;
    s_consent_shown = WR_CONSENT_IDLE;
    s_consent_qr[0] = 0;
    ui_wifi_qr_clear();
    if (!wr_consent_authorized()) wr_consent_begin();
    return APP_REDRAW_PAGE;
}

/* 强制发起一次新的扫码（无论是否已授权）：用于已授权但换会话失败时给用户明确出路。 */
static app_redraw_t begin_fresh_consent(void) {
    s_view = CONSENT;
    s_consent_shown = WR_CONSENT_IDLE;
    s_consent_qr[0] = 0;
    ui_wifi_qr_clear();
    wr_consent_begin();
    return APP_REDRAW_PAGE;
}

/* 账户页"扫码登录"：已有 consent 先尝试用它换网页会话（免再扫码），换不成或未授权才给二维码，
   不再把已授权用户带进"离线时长已授权"管理页这个死胡同。 */
static app_redraw_t begin_login(app_ctx_t *ctx) {
    (void)ctx;
    if (wr_consent_authorized()) {
        if (wr_status(NULL,0,NULL,NULL) != WR_BUSY) {
            if (wr_mint_web_session()) {
                s_login_invalid = false;
                refresh_shelf();
                return APP_REDRAW_PAGE;
            }
            /* 用已有授权换会话失败：重新扫码，给明确出路。 */
            return begin_fresh_consent();
        }
        /* 后台正忙：让 tick 的 mint 块尽快接手，不重复发请求。 */
        s_mint_at = 0;
        return APP_REDRAW_NONE;
    }
    return enter_consent_login();
}

/* 账户页"续期会话"：先 wr_rt 续期，失败用 consent 重新 mint，免扫码；都失败才给重新扫码出路。
 * "Renew session" on the account page: wr_rt renewal then consent mint, no QR; re-scan only as fallback. */
static app_redraw_t renew_now(app_ctx_t *ctx) {
    (void)ctx;
    if (wr_status(NULL, 0, NULL, NULL) == WR_BUSY) {
        s_message = "后台任务进行中，请稍候";
        return APP_REDRAW_PAGE;
    }
    if (wr_renew_session()) {
        s_login_invalid = false;
        s_session_verified_us = 0;
        s_check_pending = true;
        refresh_shelf();
        s_message = "会话已续期，可继续下载/同步";
        return APP_REDRAW_PAGE;
    }
    s_message = "续期失败，请重新扫码授权";
    return begin_fresh_consent();
}

static void connect_wifi(app_ctx_t *ctx) {
    app_transfer_request_wifi_then_return();
    ctx->request_app = &app_transfer;
}

static void open_book(app_ctx_t *ctx, bool offline) {
    if (offline) snprintf(s_offline, sizeof(s_offline), "%s", s_open);
    if (s_from_home ? app_book_request_open_from_home(s_open) : app_book_request_open(s_open)) {
        s_return_once = s_back_to_weread;
        ctx->request_app = &app_book;
    }
    s_open[0] = 0;
    s_view = SHELF;
}

bool app_weread_take_offline(const char *path) {
    if (!s_offline[0] || strcmp(s_offline, path)) return false;
    s_offline[0] = 0;
    return true;
}

bool app_weread_take_return(void) {
    bool back = s_return_once;
    s_return_once = false;
    return back;
}

bool app_weread_confirm_open(app_ctx_t *ctx, const char *path, bool from_home) {
    /* 已移除"是否启用阅读时长上报"弹窗：离线开书直接进入，阅读时长由离线补传通道处理。
     * The "enable reading-time upload" prompt is removed: offline books open directly and
     * the backfill channel reports time after reconnecting. Kept as a no-op so callers stay. */
    (void)ctx; (void)path; (void)from_home;
    return false;
}

static app_redraw_t start_range(app_ctx_t *ctx, wr_job_t job, unsigned first, unsigned count) {
    /*
     * 不再在这里挡"请等待当前操作完成"：后台预取占着任务时，wr_start 会把用户任务登记
     * 下来并让预取收手，界面上直接进任务页。只有真正不可插队的情况（例如另一个用户任务
     * 正在跑）才在 wr_start 里被拒绝，文案也由它给出。
     * Dropping the "wait for the current operation" guard: when a background prefetch holds the
     * worker, wr_start parks the user job and makes the prefetch yield, so the UI goes straight
     * to the task screen. Only genuinely non-preemptable cases (another user job running) are
     * rejected inside wr_start, which also supplies the message.
     */
    if (!wr_online()) {
        s_wait_wifi = true;
        s_after_wifi = job;
        connect_wifi(ctx);
        return APP_REDRAW_NONE;
    }
    s_message = NULL;
    s_has_retry = false;
    s_job = job;
    s_done = s_total = 0;
    s_qr[0] = 0;
    s_view = WORK;
    /* first/count 只对分章缓存任务有意义；其余任务沿用原来的起始章。 */
    /* first/count only matter for ranged jobs (cache span, review span). */
    if (!wr_start(job, &s_selected, first, count)) {
        s_view = SHELF;
        s_message = wr_result_message();
    }
    return APP_REDRAW_PAGE;
}
static app_redraw_t start(app_ctx_t *ctx, wr_job_t job) {
    return start_range(ctx, job, s_selected_index, 0);
}

/* 后台静默验证：只把 WR_CHECK 投给后台 worker，不切 WORK 视图、不弹任何页面；
   结果由 tick 轮询。worker 忙或被拒时返回 false，调用方保留 s_check_pending 稍后重试。 */
static bool start_background_check(void) {
    if (wr_status(NULL, 0, NULL, NULL) == WR_BUSY) return false;
    if (!wr_start(WR_CHECK, NULL, 0, 0)) return false;
    s_job = WR_CHECK;
    s_bg_check = true;
    return true;
}

static void enter(app_ctx_t *ctx) {
    refresh_shelf();
    if (!s_cover_pixels) s_cover_pixels = heap_caps_malloc(BOOK_COVER_W*BOOK_COVER_H,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    if (s_wifi_open) {
        if (wr_online()) {
            s_wifi_open = false;
            if (s_open_after_wifi) {
                s_open_after_wifi = false; s_ask_connect = false; s_view = ACTIONS;
                (void)open_selected(ctx);
                return;
            }
            open_book(ctx,false); return;
        }
        s_view = ASK_WIFI;
    } else if (s_wait_wifi) {
        s_wait_wifi = false;
        if (wr_online()) (void)start(ctx,s_after_wifi);
        else s_message = "Wi-Fi 未连接";
    } else if (s_logged && !session_verified_recently()) {
        s_check_pending = true;
        if (wr_online()) {
            s_check_pending = false;
            (void)start_background_check();
        }
    }
}

static app_redraw_t nav_reselect(app_ctx_t *ctx) {
    (void)ctx;
    s_page = 0;
    s_view = SHELF;
    s_message = NULL;
    s_has_retry = false;
    refresh_shelf();
    if (s_logged && !session_verified_recently()) {
        s_check_pending = true;
        if (wr_online()) {
            s_check_pending = false;
            (void)start_background_check();
        }
    }
    return APP_REDRAW_PAGE;
}

static void leave(app_ctx_t *ctx) {
    (void)ctx;
    /* 换页面/离开本标签：顶部失效提示与后台检查态一并清掉（符合"换页面后消失"）。
       若 WR_CHECK 仍在 worker 上跑，结果回到这里时被忽略，下次进页再按需重查。 */
    s_login_invalid = false;
    s_bg_check = false;
    /*
     * 这两个标记不能在离页时清掉：点"连接 Wi-Fi"会切到传书页去，回来时正是靠它们把刚才那次
     * 打开补上。它们只在"取消"和"补上之后"两处复位。
     * These two must survive leaving the page: tapping "connect Wi-Fi" switches to the transfer
     * app, and they are what lets the interrupted open resume on return. They are cleared only on
     * cancel, or once the open has been resumed.
     */
    if (s_view == WORK) { wr_cancel(); s_view = SHELF; }
    free(s_cover_pixels);
    s_cover_pixels = NULL;
}

// 六个按钮要塞进同一张卡片：70 高、84 行距，最后一个底边仍在卡片内。
// Six buttons must fit one card: 70 tall with an 84 row pitch keeps the last inside it.
static EpdRect action_rect(int i) { return (EpdRect){96, 386+i*84, 492, 70}; }
/* ACTIONS 弹窗七格专用：66 高、76 行距，七个按钮最后一个底边仍在卡片内（卡片底边 y=925）。
 * Compact layout for the seven-action sheet: 66 tall with a 76 pitch keeps the last button
 * inside the card (bottom edge y=925). */
static EpdRect sheet_rect(int i) { return (EpdRect){96, 380+i*76, 492, 66}; }
static EpdRect retry_rect(void) { return (EpdRect){190, 566, 300, 68}; }
/* 同步页按钮从 y=556 起，给上面的统计和说明留出位置。
 * Sync-page buttons start at y=556, leaving room for the stat lines above. */
static EpdRect sync_button_rect(int i) { return (EpdRect){96, 556+i*92, 492, 72}; }
/* 授权页二维码区域与按钮。/ Consent-page QR area and buttons. */
static EpdRect consent_qr_area(void) { return (EpdRect){162, 200, 360, 360}; }
static EpdRect consent_button_rect(int i) { return (EpdRect){96, 760+i*92, 492, 72}; }
/* 右上控件位置由书架统一给出，两个书架不再各存一份常量。 */
static EpdRect sync_rect(void) { return app_book_shelf_action_rect(false); }
static EpdRect account_rect(void) { return app_book_shelf_action_rect(true); }

static void shelf_title_fit(const char *src, char dst[160], int px, int width) {
    size_t used=0;
    dst[0]=0;
    while (src[used]) {
        unsigned char c=(unsigned char)src[used];
        size_t bytes=(c<0x80)?1:(c<0xe0)?2:(c<0xf0)?3:4;
        size_t remain=strlen(src+used);
        if (bytes>remain) bytes=1;
        size_t next=used+bytes;
        bool more=src[next]!=0;
        char trial[160];
        if (next+sizeof("…")>sizeof(trial)) break;
        memcpy(trial,src,next);
        if (more) memcpy(trial+next,"…",sizeof("…")); else trial[next]=0;
        if (ui_text_fixed_context_width_px(px,trial,src)>width) break;
        memcpy(dst,trial,strlen(trial)+1);
        used=next;
    }
    if (src[used] && !dst[0] && ui_text_fixed_context_width_px(px,"…",src)<=width)
        memcpy(dst,"…",sizeof("…"));
}

static void button_px(uint8_t *fb, EpdRect r, const char *label, bool enabled, int px) {
    ui_fill_round_rect(fb,r,16,enabled?UI_GRAY_WHITE:0xe0);
    ui_draw_control_frame(fb,r,16,enabled?0x50:0xa0);
    if (enabled) ui_text_box(fb,(EpdRect){r.x+8,r.y+4,r.width-16,r.height-8},px,label,EPD_DRAW_ALIGN_CENTER,false);
    else ui_text_fixed_ink_vc(fb,r.x+r.width/2,r.y+r.height/2,ui_text_effective_px(px),label,EPD_DRAW_ALIGN_CENTER,0x80);
}
static void button(uint8_t *fb, EpdRect r, const char *label, bool enabled) {
    button_px(fb,r,label,enabled,30);
}

/*
 * 按可用宽度把一段话拆成多行，逐行居中，字号固定不变。
 * ui_text_box 只画单行：宽度不够时它把字号一路压到能塞下为止——这正是这段说明怎么调都显小
 * 的原因，写 36 实际画出来只有 17。这里字号给定后不再变，行数随宽度走，系统字号放大时自动
 * 多排一行，而不是把字缩小。
 * Split a passage into lines that fit the box, each centred, at a fixed type size. ui_text_box
 * draws one line and shrinks the type until it fits — which is why this explanation never grew:
 * asking for 36 produced 17. Here the size is fixed and the line count follows the width, so a
 * larger system size wraps onto more lines instead of shrinking back down.
 */
static void text_block(uint8_t *fb, EpdRect box, int px, const char *text) {
    if (!fb || !text || !text[0] || box.width <= 0 || box.height <= 0) return;
    const int size = ui_text_effective_px(px);
    const int line_h = size + size / 3;
    int y = box.y;
    const char *p = text;
    while (*p && y + line_h <= box.y + box.height) {
        char line[192];
        size_t used = 0;
        const char *q = p;
        while (*q) {
            unsigned len = 1;
            const unsigned char c = (unsigned char)*q;
            if (c >= 0xf0) len = 4; else if (c >= 0xe0) len = 3; else if (c >= 0xc0) len = 2;
            if (used + len >= sizeof(line)) break;
            memcpy(line + used, q, len);
            line[used + len] = 0;
            if (ui_text_fixed_width_px(size, line) > box.width) {
                line[used] = 0; // 这个字符进不去，留到下一行
                break;
            }
            used += len;
            line[used] = 0;
            q += len;
        }
        if (!used) break;
        ui_text_fixed_vc(fb, box.x + box.width / 2, y + line_h / 2, size, line,
                         EPD_DRAW_ALIGN_CENTER, false);
        y += line_h;
        p = q;
    }
}

static void title(uint8_t *fb, const char *label) {
    ui_text_box(fb,(EpdRect){36,86,370,70},UI_PX_LABEL,label,EPD_DRAW_ALIGN_LEFT,false);
}

static void draw_cover(uint8_t *fb, const wr_book_t *book, EpdRect rect) {
    if (s_cover_pixels && wr_cover_get(book, s_cover_pixels)) {
        for (int y = 0; y < rect.height; ++y) {
            const uint8_t *row = s_cover_pixels+(size_t)(y*BOOK_COVER_H/rect.height)*BOOK_COVER_W;
            for (int x = 0; x < rect.width; ++x) epd_draw_pixel(rect.x+x,rect.y+y,row[x*BOOK_COVER_W/rect.width],fb);
        }
    } else {
        ui_fill_rect_fast(rect,0xe0,fb);
        ui_draw_icon(fb,rect.x+rect.width/2,rect.y+rect.height/2,rect.width/2,UI_ICON_BOOK_OPEN,0x60);
    }
    ui_draw_round_rect(fb,rect,0,UI_GRAY_BLACK);
}

static void shelf(uint8_t *fb) {
    title(fb,"微信读书");
    /* 登录失效提示：放在状态栏（中心 y=40）与标题（y=86）之间的空闲条带，单行不与任何控件重叠。 */
    if (s_login_invalid && s_view == SHELF)
        ui_text_vc(fb, UI_LOCK_WIDTH / 2, 70, 20,
                   "登录状态失效，请前往账户续期会话或重新扫码", EPD_DRAW_ALIGN_CENTER, false);
    app_book_shelf_header(fb,false,"同步",s_pressed==0);
    app_book_shelf_header(fb,true,"账户",s_pressed==1);
    bool list = app_settings_shelf_style() == 5;
    int slots = app_book_shelf_slots();
    if (slots > WR_SLOTS) slots = WR_SLOTS;
    for (int i = 0; i < slots && s_books[i].id[0]; ++i) {
        EpdRect r = app_book_shelf_slot(i), cover = app_book_shelf_cover(i);
        char cache_text[32];
        if (s_cache_kind[i] == WR_SHELF_FULL) snprintf(cache_text, sizeof(cache_text), "已缓存");
        else if (s_cache_kind[i] == WR_SHELF_PARTIAL)
            snprintf(cache_text, sizeof(cache_text), "%u/%u", s_cache_ready[i], s_cache_total[i]);
        else snprintf(cache_text, sizeof(cache_text), "未缓存");
        const char *cached = cache_text;
        char short_title[160];
        int title_px=ui_text_effective_px(24);
        if (title_px>30) title_px=30;
        shelf_title_fit(s_books[i].title,short_title,title_px,list?492:r.width-8);
        draw_cover(fb,&s_books[i],cover);
        if (list) {
            ui_text_fixed_context_vc(fb,114,r.y+27,title_px,short_title,s_books[i].title,EPD_DRAW_ALIGN_LEFT,false);
            ui_text_box(fb,(EpdRect){114,r.y+50,365,34},20,s_books[i].author,EPD_DRAW_ALIGN_LEFT,false);
            ui_text_box(fb,(EpdRect){479,r.y+50,150,34},20,cached,EPD_DRAW_ALIGN_RIGHT,false);
            ui_hairline(fb,r.y+r.height-1,r.x,r.width,0xc0);
        } else {
            ui_text_fixed_context_vc(fb,r.x+r.width/2,r.y+242,title_px,short_title,s_books[i].title,EPD_DRAW_ALIGN_CENTER,false);
            ui_fill_round_rect(fb,(EpdRect){cover.x+4,cover.y+4,cover.width-8,39},7,0xf0);
            ui_text_box(fb,(EpdRect){cover.x+6,cover.y+5,cover.width-12,37},18,cached,EPD_DRAW_ALIGN_CENTER,false);
        }
    }
    app_book_shelf_furniture(fb);
    if (!s_count) ui_text_vc(fb,342,430,28,s_logged?"点击同步获取微信读书书架":"请在账户中扫码登录",EPD_DRAW_ALIGN_CENTER,false);
    char footer[64];
    snprintf(footer,sizeof(footer),"%d 本 · 第 %d / %d 页",s_count,s_page+1,pages());
    ui_pager_draw(fb,footer,s_page,pages(),false,UI_PAGER_NONE);
}

static bool action_enabled(int action) {
    /* 阅读不再要求整本已缓存：未缓存的书由虚拟书源打开，缺的章节按窗口补齐。
     * 上传/下载进度同样不再要求缓存：分章缓存的书只有标记文件也能同步，下载到的章节
     * 若不在本地，会边缓存边打开。
     * Reading no longer requires a complete cache: an uncached book opens through the virtual
     * source and fills chapters from the background window. Progress sync drops the same
     * requirement: a chapter-cached book (only a marker on disk) syncs too, and a downloaded
     * chapter that is not local yet caches as it opens. */
    /* 已缓存的书也允许点"缓存全本书籍"：worker 会跳过全部 READY 章（不计数、不重下），
       只跑自愈——补缺失锚点、重抓过期 sidecar（wrb_is_current 判版本）。
       A fully cached book can still run this action: the worker skips every READY chapter and
       only heals missing anchors and stale sidecars. */
    if (action == 1) return s_logged;
    /* 六格顺序：0 阅读 / 1 缓存 / 2 上传 / 3 获取 / 4 删除 / 5 关闭。
     * 0 阅读与 5 关闭永远可用，其余都需要登录。
     * Six-slot order: 0 read and 5 close are always available; the rest need an account. */
    return action == 0 || action == 5 || s_logged;
}

static void render(app_ctx_t *ctx, uint8_t *fb) {
    (void)ctx;
    /*
     * 微读的每一页都走系统字号：标题、按钮、提示、辅助信息都用 ui_text_effective_px，
     * 读者在系统设置里调大字号，这里跟着变，和设置页、书架保持同一档。
     * Every WeRead screen follows the system type size: titles, buttons, notices and captions all
     * pass through ui_text_effective_px, so changing the size in settings moves this screen with
     * the rest of the system.
     */
    ui_text_set_system_scale(true);
    ui_clear_page(fb);
    if ((s_view == SHELF || s_view == ACTIONS) && app_settings_shelf_style() != 5) ui_root_background(fb);
    ui_nav_status(fb);
    if (s_view == SHELF || s_view == ACTIONS) shelf(fb);
    if (s_view == ACCOUNT || s_view == LOGOUT) {
        title(fb,"微信读书账户");
        uint32_t today;
        uint64_t total;
        bool logged = wr_account(NULL,&today,&total);
        char stats[96];
        ui_text_vc(fb,342,263,30,logged?"已登录":"未登录",EPD_DRAW_ALIGN_CENTER,false);
        snprintf(stats,sizeof(stats),"今日上报  %u 分 %u 秒",(unsigned)today/60,(unsigned)today%60);
        ui_text_vc(fb,342,351,28,stats,EPD_DRAW_ALIGN_CENTER,false);
        snprintf(stats,sizeof(stats),"累计上报  %llu 分 %llu 秒",(unsigned long long)(total/60),(unsigned long long)(total%60));
        ui_text_vc(fb,342,436,28,stats,EPD_DRAW_ALIGN_CENTER,false);
        if (s_view == ACCOUNT) {
            /* 待补传单独一行：这是"离线读书的时长还在本机等着"的唯一可见证据。
             * A dedicated line for pending seconds: the only visible proof that durations read
             * while offline are still waiting on this device. */
            unsigned pending = wr_pending_seconds();
            snprintf(stats,sizeof(stats),"待补传  %u 分 %u 秒",pending/60,pending%60);
            ui_text_vc(fb,342,510,27,stats,EPD_DRAW_ALIGN_CENTER,false);
        }
        button(fb,action_rect(2),logged?"退出登录":"扫码登录",true);
        button(fb,action_rect(3),"返回书架",true);
        if (s_view == ACCOUNT) button(fb,action_rect(4),"时长同步",true);
        if (s_view == ACCOUNT && logged)
            button(fb,action_rect(5), s_login_invalid ? "续期会话（当前失效）" : "续期会话", true);
    }
    if (s_view == SYNC) {
        title(fb,"时长同步");
        unsigned pending = wr_pending_seconds();
        uint32_t today;
        uint64_t total;
        (void)wr_account(NULL,&today,&total);
        s_pending_shown = pending;
        char stats[96];
        bool online = wr_online();
        ui_text_vc(fb,342,225,30,online?"网络已连接":"网络未连接",EPD_DRAW_ALIGN_CENTER,false);
        snprintf(stats,sizeof(stats),"待补传  %u 分 %u 秒",pending/60,pending%60);
        ui_text_vc(fb,342,288,30,stats,EPD_DRAW_ALIGN_CENTER,false);
        snprintf(stats,sizeof(stats),"今日已确认  %u 分 %u 秒",(unsigned)today/60,(unsigned)today%60);
        ui_text_vc(fb,342,350,27,stats,EPD_DRAW_ALIGN_CENTER,false);
        /* 离线时长授权状态。/ Offline-duration consent status. */
        bool authorized = wr_consent_authorized();
        ui_text_vc(fb,342,412,27,
                   authorized?"离线时长已授权，补传被服务器认可":"未授权离线时长，建议先授权",
                   EPD_DRAW_ALIGN_CENTER,false);
        ui_text_box(fb,(EpdRect){96,452,492,52},22,
                    online?"断网时照常计时，联网后自动补传":"连上 Wi-Fi 后自动补传，无需守着",
                    EPD_DRAW_ALIGN_CENTER,false);
        button(fb,sync_button_rect(0),pending?"立即补传":"暂无待补传",pending&&online);
        button(fb,sync_button_rect(1),authorized?"授权管理":"授权离线补传",online);
        button(fb,sync_button_rect(2),
               app_settings_reader_fast_replay()?"快速补传：开":"快速补传：关",true);
        button(fb,sync_button_rect(3),"返回账户",true);
    }
    if (s_view == CONSENT) {
        /* 登录与离线补传已合并为同一次扫码授权，标题不再写"授权离线时长"，用中性名称。 */
        title(fb,"微信读书");
        wr_consent_status_t st;
        wr_consent_poll(&st);
        bool authorized = wr_consent_authorized() || st.state == WR_CONSENT_OK;
        if (authorized) {
            ui_text_vc(fb,342,330,34,"离线时长已授权",EPD_DRAW_ALIGN_CENTER,false);
            ui_text_box(fb,(EpdRect){96,400,492,56},24,
                        "断网照常计时，联网后自动补传，服务器认可",
                        EPD_DRAW_ALIGN_CENTER,false);
            button(fb,consent_button_rect(0),"重新授权",true);
            button(fb,consent_button_rect(1),"取消授权",true);
            button(fb,consent_button_rect(2),"返回",true);
        } else {
            if (s_consent_qr[0]) ui_wifi_qr_draw(fb,consent_qr_area());
            else ui_fill_round_rect(fb,consent_qr_area(),16,0xe0);
            const char *line;
            switch (st.state) {
                case WR_CONSENT_PREPARING: line = "正在获取二维码…"; break;
                case WR_CONSENT_SCANNED: line = "已扫码，请在手机上确认"; break;
                case WR_CONSENT_EXPIRED: line = "二维码已过期，点下方重新获取"; break;
                case WR_CONSENT_DECLINED: line = "授权已取消，可重新发起"; break;
                case WR_CONSENT_FAILED: line = st.message[0] ? st.message : "授权失败，请重试"; break;
                default: line = "用微信扫一扫，在手机上确认授权"; break;
            }
            ui_text_box(fb,(EpdRect){86,580,512,60},26,line,EPD_DRAW_ALIGN_CENTER,false);
            if (st.state == WR_CONSENT_EXPIRED || st.state == WR_CONSENT_DECLINED ||
                st.state == WR_CONSENT_FAILED)
                button(fb,consent_button_rect(0),"重新获取",true);
            else
                button(fb,consent_button_rect(0),"取消并返回",true);
        }
    }
    if (s_view == ACTIONS || s_view == ASK_WIFI || s_view == LOGOUT || s_view == CONFIRM_DEL ||
        s_view == CACHE_SCOPE) {
        EpdRect box = {58,268,568,657};
        ui_fill_round_rect(fb,box,20,UI_GRAY_WHITE);
        ui_draw_control_frame(fb,box,20,0x50);
        const char *heading = s_view==ASK_WIFI?(s_wifi_open?"正在连接 Wi-Fi…":
                                                s_ask_connect?"网络未连接，先连 Wi-Fi？":
                                                "网络未连接，是否先连 Wi-Fi？"):s_view==LOGOUT?"是否保留所有缓存书籍":s_view==CONFIRM_DEL?"删除本地缓存？":s_selected.title;
        // 删除确认这件事不可逆：标题与说明都比其它卡片大一档，读者不用凑近看。
        // Deleting is irreversible, so this card's title and explanation both run one step
        // larger than the other sheets; nobody should have to lean in to read it.
        ui_text_box(fb,(EpdRect){83,298,518,75},s_view==CONFIRM_DEL?36:32,heading,
                    EPD_DRAW_ALIGN_CENTER,false);
        if (s_view == ACTIONS) {
            static const char *const actions[] = {"阅读书籍","缓存全本书籍","上传本地进度","获取云端进度","删除本地缓存","关闭"};
            for (int i = 0; i < 6; ++i) button(fb,sheet_rect(i),actions[i],action_enabled(i));
        } else if (s_view == CACHE_SCOPE) {
            /*
             * 范围页：整本 / 未读 5·10·20 章。章号以 1 起显示，读者不用换算。
             * 按钮沿用 sheet 的格子，返回留在最后，误触代价最小。
             * The span sheet: whole book, next 5/10/20 chapters. Chapter numbers are
             * 1-based for the reader; Back stays last where a mis-tap costs least.
             */
            char labels[4][40];
            snprintf(labels[0], sizeof(labels[0]), "整本缓存");
            for (int n = 0; n < 3; ++n) {
                unsigned span = n == 0 ? 5u : n == 1 ? 10u : 20u;
                snprintf(labels[1 + n], sizeof(labels[1 + n]), "未读 %u 章（第 %u 章起）",
                         span, s_scope_chapter + 1);
            }
            for (int i = 0; i < 4; ++i) button_px(fb,action_rect(i),labels[i],true,26);
            button(fb,action_rect(5),"返回",true);
        } else if (s_view == ASK_WIFI && s_ask_connect) {
            // 这张卡片只有一个目的：把网连上再继续；"继续阅读"在这里没有意义（书还没落地）。
            // One purpose only: get the link up and carry on. "Continue reading" is meaningless
            // here because the book has not landed yet.
            text_block(fb,(EpdRect){83,344,518,84},26,
                       "这本书还没缓存到本地，需要联网才能取到目录。");
            button(fb,action_rect(0),"连接 Wi-Fi",true);
            button(fb,action_rect(1),"取消",true);
        } else if (s_view == ASK_WIFI) {
            button(fb,action_rect(0),"启用，连接 Wi-Fi",true);
            button(fb,action_rect(1),"不启用，继续阅读",true);
            button(fb,action_rect(2),"取消",true);
        } else if (s_view == CONFIRM_DEL) {
            // 说明文字与标题同一档，并且用能换行的画法：单行控件会把它压回小字。
            // Same size as the title, drawn with the wrapping helper — the single-line control
            // would shrink it straight back down.
            text_block(fb,(EpdRect){83,366,518,214},36,
                       "将删除本书的章节缓存与本地书架文件，微信读书里的书不受影响。");
            button_px(fb,action_rect(3),"删除",true,34);
            button_px(fb,action_rect(4),"取消",true,34);
        } else {
            button(fb,action_rect(0),"保留并退出",true);
            button(fb,action_rect(1),"删除并退出",true);
            button(fb,action_rect(2),"取消",true);
        }
    }
    if (s_view == WORK) {
        title(fb,"微信读书");
        /* 索引必须与 wr_job_t 枚举逐一对齐（WR_SYNC=0 … WR_PREFETCH=6），否则任务页显示
         * 与实际任务无关的状态。
         * Each index matches the wr_job_t enum order, or the task page shows an unrelated status. */
        static const char *const labels[] = {"正在同步书架与封面","正在缓存章节","正在上传本地进度","正在获取云端进度","正在上报阅读时长","正在验证登录状态","正在预取当前章节"};
        ui_text_vc(fb,342,228,30,labels[s_job],EPD_DRAW_ALIGN_CENTER,false);
        if (s_qr[0]) ui_wifi_qr_draw(fb,(EpdRect){137,300,410,410});
        if (s_total || s_job == WR_SYNC) {
            char progress[64];
            if (s_job == WR_SYNC && !s_total) snprintf(progress,sizeof(progress),"正在连接并获取书架数据…");
            else if (s_job == WR_SYNC) snprintf(progress,sizeof(progress),"%u / %u 本封面已缓存",s_done,s_total);
            else snprintf(progress,sizeof(progress),"%u / %u",s_done,s_total);
            EpdRect bar = {122,482,440,24};
            ui_fill_round_rect(fb,bar,12,0xd8);
            unsigned pct = !s_total ? 0 : s_done >= s_total ? 100 : (unsigned)(((uint64_t)s_done*100u)/s_total);
            int fill = (int)((bar.width-4)*pct/100u);
            if (fill > 0) ui_fill_round_rect(fb,(EpdRect){bar.x+2,bar.y+2,fill,bar.height-4},10,0x50);
            ui_text_vc(fb,342,542,30,progress,EPD_DRAW_ALIGN_CENTER,false);
        }
        button(fb,action_rect(4),"取消",true);
    }
    /* Status messages are modal: keep them off the shelf artwork so covers and
     * text never show through the notification.  A tap dismisses the notice. */
    if (s_message) {
        /* 真正的模态遮罩：先把状态栏以下、底部导航以上的整段内容区铺白，
         * 遮住下层页面的标题、说明文字与按钮（含下层"重新授权"等），避免与弹窗叠字。
         * True modal scrim: white out the whole content area between the status bar and the
         * bottom nav so the underlying page's captions and buttons never overlap the card. */
        const EpdRect scrim = {0,76,UI_LOCK_WIDTH,UI_NAV_TOP-76};
        ui_fill_rect_fast(scrim,UI_GRAY_WHITE,fb);
        EpdRect notice = {72,430,540,230};
        ui_fill_round_rect(fb,notice,22,UI_GRAY_WHITE);
        ui_draw_control_frame(fb,notice,22,0x50);
        ui_text_vc(fb,342,s_has_retry?468:480,30,"微信读书",EPD_DRAW_ALIGN_CENTER,false);
        // 失败时留一个重试入口：缓存中断后不必退出弹窗重来。
        text_block(fb,(EpdRect){104,s_has_retry?486:500,476,s_has_retry?96:110},28,s_message);
        if (s_has_retry) {
            button(fb,retry_rect(),"重试",true);
            ui_text_vc(fb,342,640,18,"轻点其它位置关闭",EPD_DRAW_ALIGN_CENTER,false);
        } else {
            ui_text_vc(fb,342,622,20,"轻点任意位置关闭",EPD_DRAW_ALIGN_CENTER,false);
        }
    }
    ui_nav_draw(fb,UI_TAB_WEREAD);
    ui_text_set_system_scale(false);
}

static app_redraw_t open_selected(app_ctx_t *ctx);

static app_redraw_t tick(app_ctx_t *ctx) {
    ESP_LOGI("WEREAD", "TICKDBG view=%d bg=%d logged=%d online=%d status=%d pending=%u pressed=%d wifiopen=%d ask=%d",
             (int)s_view, (int)s_bg_check, (int)s_logged, (int)wr_online(),
             (int)wr_status(NULL,0,NULL,NULL), wr_pending_seconds(),
             (int)s_pressed, (int)s_wifi_open, (int)s_ask_connect);
    if (s_ask_connect && s_wifi_open && s_view == ASK_WIFI && !wr_online() &&
        ctx->now_ms - s_wifi_wait_ms > 20000) {
        s_wifi_open = false;
        return APP_REDRAW_PAGE;
    }
    if (s_wifi_open && s_view == ASK_WIFI && wr_online()) {
        s_wifi_open = false;
        if (s_open_after_wifi) {
            /* 连上就把刚才那次打开补上：读者点的是"阅读书籍"，不是"去连 Wi-Fi"。
             * Resume the open that was interrupted: the reader asked to read, not to fix Wi-Fi. */
            s_open_after_wifi = false; s_ask_connect = false; s_view = ACTIONS;
            return open_selected(ctx);
        }
        open_book(ctx,false);
        return APP_REDRAW_NONE;
    }
    /* 联网且本机已有 consent 凭证时，在后台确保 accessToken 有效（必要时用 refreshToken 自动
     * 续期），不阻塞 UI。内部有防重入和节流，可在每个 tick 安全调用。
     * Keep the consent access token fresh in a background task while online; non-blocking and
     * internally guarded, so it is safe to call on every tick. */
    wr_consent_ensure();
    /* consent 已授权、联网但网页会话还没建立：自动 mint（一次扫码即同时拿到网页会话），
       失败按 10s 节流重试，不反复发请求。成功后刷新书架，s_logged 转为已登录。 */
    if (wr_consent_authorized() && wr_online() && !s_logged &&
        ctx->now_ms >= s_mint_at) {
        if (wr_mint_web_session()) {
            /* 重新拿到网页会话即视为登录成功：清掉失效横幅再刷新。 */
            s_login_invalid = false;
            refresh_shelf();
        } else {
            s_mint_at = ctx->now_ms + 10000;
        }
    }
    /* 补传成功弹窗：发现新的确认序号（且不在 WORK 进度页）就弹一次"离线补传成功"。
     * 首次只同步当前序号、不弹，避免开机把历史确认重新弹一遍。/ Pop up once when a new
     * confirmation arrives outside the WORK page; the first call only syncs the sequence. */
    if (!s_confirm_seen_init) { s_confirm_seen_init = true; s_confirm_seen = wr_confirm_seq(); }
    uint32_t cseq = wr_confirm_seq();
    if (cseq != s_confirm_seen) {
        s_confirm_seen = cseq;
        unsigned cs = wr_last_confirm_seconds();
        snprintf(s_confirm_text, sizeof(s_confirm_text), "离线补传成功，已上报 %u 秒", cs);
        if (s_view != WORK) {
            s_message = s_confirm_text;
            s_has_retry = false;
            return APP_REDRAW_PAGE;
        }
    }
    /* 有网就把待补传推上去，并且限频：worker 被唤醒后会立刻试，退避不必等满。
     * 限频是为了不把 worker 从长睡里反复叫醒白耗电。
     * With a network, push the pending queue — rate-limited so the worker is not woken
     * out of its long sleep over and over for nothing. */
    if (wr_online() && wr_pending_seconds() && ctx->now_ms - s_kick_at > 10000) {
        s_kick_at = ctx->now_ms;
        wr_pending_kick();
    }
    /* 同步页只有数字变了才重绘，避免空刷整屏。
     * The sync page repaints only when the number changed, never for nothing. */
    if (s_view == SYNC && wr_pending_seconds() != s_pending_shown) return APP_REDRAW_PAGE;
    /* 授权页：状态或二维码变化才重绘。/ Consent page: repaint only when state or QR changes. */
    if (s_view == CONSENT) {
        wr_consent_status_t st;
        wr_consent_poll(&st);
        bool changed = false;
        if (st.state != s_consent_shown) {
            s_consent_shown = st.state;
            changed = true;
            if (st.state == WR_CONSENT_OK) {
                /* 授权刚成功：立即用 consent 换网页会话并刷新，s_logged 当场翻转，无需重启。
                   consent 任务此刻已不再发 HTTP；worker 忙则让 tick 的 mint 块尽快接手。 */
                if (wr_status(NULL,0,NULL,NULL) != WR_BUSY && wr_mint_web_session()) {
                    s_login_invalid = false;
                    refresh_shelf();
                }
                else
                    s_mint_at = 0;
            }
        }
        if (strcmp(st.qr_url, s_consent_qr) != 0) {
            snprintf(s_consent_qr, sizeof(s_consent_qr), "%s", st.qr_url);
            if (st.qr_url[0]) (void)ui_wifi_qr_prepare_text(st.qr_url);
            changed = true;
        }
        if (changed) return APP_REDRAW_PAGE;
    }
    if (s_logged && !wr_online()) {
        s_session_verified_us = 0;
        s_check_pending = true;
    }
    if (s_check_pending && wr_online() &&
        wr_status(NULL,0,NULL,NULL) != WR_BUSY) {
        s_check_pending = false;
        (void)start_background_check();
    }
    if (s_pressed >= 0 && ctx->now_ms >= s_pressed_until) {
        // 按下反馈先画一帧，再执行动作，和本地书架头部按钮的手感一致。
        const int pending = s_pending_action;
        s_pressed = -1;
        s_pending_action = -1;
        if (pending == 0) {
            if (s_logged) return start(ctx,WR_SYNC);
            s_view = ACCOUNT;
            s_message = "请先登录";
            s_has_retry = false;
        } else if (pending == 1) {
            s_view = ACCOUNT;
            s_message = NULL;
            s_has_retry = false;
        }
        return APP_REDRAW_PAGE;
    }
    unsigned done, total;
    char qr[sizeof(s_qr)];
    wr_result_t status = wr_status(qr,sizeof(qr),&done,&total);
    /* 任一成功的网页操作都证明当前网页会话有效（含后台 WR_CHECK 的 OK）：统一清除失效横幅，
       保证重新登录/会话恢复后横幅立即消失，而不是一直挂在书架顶部。书架页当场重绘抹掉字。 */
    if (status == WR_OK) {
        s_bg_check = false;
        if (s_login_invalid) {
            s_login_invalid = false;
            session_verification_result(status);
            if (s_view == SHELF) {
                refresh_shelf();
                return APP_REDRAW_PAGE;
            }
        }
    }
    if (s_view == WORK) {
        if (status != WR_BUSY) {
            ui_wifi_qr_clear();
            session_verification_result(status);
            if (status == WR_OK && s_job == WR_CACHE) book_store_notify_changed();
            /*
             * 下载云端进度成功后直接进书并定位到那一章。章节若不在本地，阅读页会先画缓存
             * 进度卡片，落盘后自动换成正文——"下载进度 → 继续阅读"因此是一条连贯动作，
             * 不需要用户再回书架点一次。
             * A successful progress download goes straight into the book at that chapter. If the
             * chapter is not local yet the reader shows the cache-progress card and swaps in the
             * text once it lands, so "download progress, keep reading" is one continuous action
             * instead of a trip back to the shelf.
             */
            if (s_job == WR_DOWNLOAD && status == WR_OK) {
                unsigned target = 0;
                const bool has_target = wr_progress_take_target(&target);
                if (has_target) ESP_LOGI(WR_TAG, "cloud progress landed, opening chapter=%u", target + 1);
                if (has_target && !s_selected_full) {
                    refresh_shelf();
                    return open_selected(ctx);
                }
            }
            if (status == WR_EXPIRED) {
                /* 显式任务撞上登录过期：不弹阻塞窗，回书架并置顶部失效提示。 */
                session_verification_result(status);
                s_login_invalid = true;
                s_view = SHELF;
                s_message = NULL;
                s_has_retry = false;
                refresh_shelf();
                return APP_REDRAW_PAGE;
            }
            s_view = SHELF;
            if (status == WR_OK && s_job == WR_CACHE) {
                /* 说清楚这次到底新下了几章；"未读 5 章"跳过已缓存章后可能不是 5。
                 * Say how many chapters were actually staged: "next 5" may skip cached ones. */
                if (s_range_span == 0)
                    snprintf(s_done_message, sizeof(s_done_message), "整本缓存完成，已就绪 %u 章", done);
                else
                    snprintf(s_done_message, sizeof(s_done_message), "已新下 %u 章（第 %u 章起，目标 %u 章）",
                             done, s_range_from + 1, s_range_span);
                s_message = s_done_message;
            } else {
                s_message = status == WR_OK ? "操作完成" :
                            status == WR_CANCELLED ? "已取消" : wr_result_message();
            }
            // 失败时给出重试入口；登录失效走顶部提示，不给一键重试。
            s_has_retry = status == WR_FAILED &&
                (s_job == WR_CACHE || s_job == WR_UPLOAD || s_job == WR_DOWNLOAD || s_job == WR_SYNC);
            if (s_has_retry) s_retry_job = s_job;
            refresh_shelf();
            return APP_REDRAW_PAGE;
        }
        if (strcmp(qr,s_qr)) {
            snprintf(s_qr,sizeof(s_qr),"%s",qr);
            if (*qr) (void)ui_wifi_qr_prepare_text(qr);
            return APP_REDRAW_PAGE;
        }
        if (done != s_done || total != s_total) { s_done=done; s_total=total; return APP_REDRAW_PAGE; }
        return APP_REDRAW_NONE;
    }
    /* 后台静默验证收尾：不切视图、不弹窗，只更新顶部失效提示。
       WR_OK 已在本函数开头统一清除横幅，这里只需处理过期与非认证失败。 */
    if (s_bg_check && status != WR_BUSY) {
        s_bg_check = false;
        session_verification_result(status);
        if (status == WR_EXPIRED) {
            s_login_invalid = true;
            refresh_shelf();
            return APP_REDRAW_PAGE;
        }
        /* 网络抖动/取消等非认证失败：不改变失效态，置 pending 稍后重试验证。 */
        s_check_pending = true;
        return APP_REDRAW_NONE;
    }
    /* 其它任务（同步/缓存等）在非 WORK 视图返回过期：同样只置顶部提示，不弹阻塞窗。
     * 只在"刚进入过期"时处理一次：s_result 不会自动清除，若每轮都重绘整页，主循环会被
     * 连续全屏刷新拖到约 580ms，触摸手势来不及完成，表现为整页交互失效。 */
    if (status == WR_EXPIRED && s_logged && !s_login_invalid) {
        session_verification_result(status);
        s_login_invalid = true;
        refresh_shelf();
        return APP_REDRAW_PAGE;
    }
    return APP_REDRAW_NONE;
}

/*
 * 打开当前选中书籍：整书 EPUB 就绪时按原路径打开，否则先落一份标记文件，由阅读器的
 * 虚拟书源直接打开，章节在后台按窗口补齐。用户点击即刻进书。阅读与"下载进度后进书"
 * 共用这一段，避免两套打开逻辑各自演化。
 * Open the selected book: a complete EPUB still opens as before, otherwise a marker is
 * written and the reader's virtual source opens the book at once while chapters fill in from
 * the background window. Reading and "open after downloading progress" share this path so the
 * two can never drift apart.
 */
static app_redraw_t open_selected(app_ctx_t *ctx) {
    unsigned start_chapter = 0;
    /* 先顶到书架第一位，和手机上的微信读书一致：返回书架时这本书就该在最前面。
     * 只改本地索引顺序，不联网；失败不影响打开。 */
    /* Float it to the top first, as WeRead does on the phone. Local index order only, no
     * network call, and a failure never blocks opening. */
    (void)wr_shelf_touch(s_selected.id);
    /*
     * 离线且这本书还不在本地书架（没有标记文件，目录也只能从云端取）时，先问要不要连网再继续。
     * 以前直接去取目录，取不到就把"获取目录失败"丢给读者，而真因是没有网络。已经在书架上、
     * 目录也缓存过的书照旧离线打开——那是能读的。
     * Offline and the book is not on the local shelf yet (no marker, TOC only in the cloud): ask
     * about connecting first. It used to fetch the TOC anyway and hand the reader "TOC fetch
     * failed" while the real cause was no network. Books already on the shelf with a cached TOC
     * still open offline — those are readable.
     */
    /* 分章缓存的书没有整本 EPUB（书文件只是标记），wr_book_path 会误判成"未缓存"；
       再用缓存概况确认：只要有已就绪章节，离线照样放行。/ A chapter-cached book has no
       whole-book EPUB (its shelf file is only a marker), so wr_book_path alone wrongly blocks
       it; the cache summary's ready count lets any book with landed chapters open offline. */
    unsigned offline_ready = 0;
    bool offline_cache = wr_cache_summary(&s_selected, &offline_ready, NULL)
                         && offline_ready > 0;
    if (!wr_online() && !wr_book_path(&s_selected, s_open) && !offline_cache) {
        s_wait_wifi = false; s_ask_connect = true; s_view = ASK_WIFI;
        return APP_REDRAW_PAGE;
    }
    if (!wr_book_path(&s_selected, s_open) &&
        !wr_open_book(&s_selected, s_open, &start_chapter)) {
        s_message = wr_error_message(); // 留在操作菜单里给出原因，不静默回书架
        s_has_retry = false;
        s_view = ACTIONS;
        return APP_REDRAW_PAGE;
    }
    // 无论从第几章进入都先把窗口任务拉起来：首章命中缓存时不会读到占位正文，
    // 也就没有别的机会触发预取。
    // Start the window job whichever chapter we enter at: when the first chapter is already
    // cached no placeholder is ever read, so nothing else would trigger prefetching.
    wr_prefetch_nudge(start_chapter);
    s_from_home = false;
    s_back_to_weread = true;
    /* 离线开书不再弹"是否启用阅读时长上报"：直接进阅读页，时长在联网后由补传通道处理。
     * No "enable reading-time upload" prompt when offline: open the reader directly; the
     * offline backfill channel reports the time once the network is back. */
    open_book(ctx, false);
    return APP_REDRAW_NONE;
}

/*
 * 删除本地缓存：章节缓存目录、目录缓存、概况文件、书籍旁的封面快照，以及本地书架上这本书
 * 的源文件（分章缓存的书就是一个标记文件）。微信读书里的书不受影响，微读封面也保留。
 * Removing the local cache: chapter tree, TOC cache, summary, the cover snapshot beside the book
 * and the book's own file on the local shelf (a marker for a chapter-cached book). The book in
 * WeRead is untouched, and its WeRead cover is kept.
 */
static void delete_local_cache(app_ctx_t *ctx) {
    (void)ctx;
    char path[WR_PATH] = {0};
    /*
     * 这里不能用 wr_book_path：它要求这本书同时"已登记缓存"且"文件存在"，而删除要处理的
     * 恰恰是这两种残缺状态——只剩章节缓存、书文件已不在，或者缓存概况重排后丢了这一条。
     * 用它就会报"找不到本地文件"，读者明明看得见这本书的缓存。
     * wr_book_path must not be used here: it requires both a registered cache entry and an
     * existing file, while deletion is exactly about the broken cases — only chapters left, the
     * file already gone, or the entry lost in a shelf reorder. Using it reports "no local file"
     * for a book whose cache the reader can plainly see.
     */
    if (!wr_book_storage(&s_selected, path) || !path[0]) {
        s_message = "尚未登录微信读书，无法定位本地缓存";
        s_view = SHELF;
        refresh_shelf();
        return;
    }
    /* 先停掉这本书的缓存任务：删目录的同时后台还在往里写，可能删不干净或者把书又落回来。
     * Stop this book's cache jobs first: deleting the tree while the background still writes into
     * it either leaves debris or lands the chapter again right after the delete. */
    wr_cache_pause(path);
    bool removed = false;
    (void)book_store_delete(path, &removed);
    const bool cache = wr_forget_book(path);
    /* 按"实际删到了什么"回话：书文件没了但章节缓存还在，也算清理成功，不能报找不到文件。 */
    /* Report what was actually removed: with the file gone but chapters still on the card the
     * cleanup still succeeded, and must not answer "file not found". */
    s_message = removed ? "已删除本地缓存" : cache ? "缓存已清理" : "这本书本地没有缓存";
    s_view = SHELF;
    refresh_shelf();
}

/*
 * 选中书本地记住的阅读章节（0 起）。取不到就当 0，也就是"从第一章起"，行为和以前一致。
 * Locally remembered chapter of the selected book (0-based); 0 when unknown, which is exactly
 * the old "start from chapter one" behaviour.
 */
static unsigned selected_chapter(void) {
    char path[WR_PATH];
    struct stat info;
    book_progress_t progress;
    if (!wr_book_storage(&s_selected, path)) return 0;
    if (stat(path, &info) != 0 || !S_ISREG(info.st_mode) || info.st_size <= 0) return 0;
    if (!book_progress_load(path, (uint32_t)info.st_size, &progress)) return 0;
    return progress.chapter;
}

static app_redraw_t activate(app_ctx_t *ctx,int index) {
    if (s_view == ACTIONS) {
        if (!action_enabled(index)) return APP_REDRAW_NONE;
        if (index == 5) { s_view = SHELF; return APP_REDRAW_PAGE; } // 关闭
        if (index == 4) { s_view = CONFIRM_DEL; return APP_REDRAW_PAGE; } // 删除本地缓存
        /* "缓存全本书籍"先问范围：整本 / 未读 5·10·20 章，见 CACHE_SCOPE。 */
        if (index == 1) { s_scope_chapter = selected_chapter(); s_view = CACHE_SCOPE; return APP_REDRAW_PAGE; }
        if (index == 2) return start(ctx, WR_UPLOAD);
        if (index == 3) return start(ctx, WR_DOWNLOAD);
        return open_selected(ctx);
    }
    if (s_view == CACHE_SCOPE) {
        if (index == 5) { s_view = ACTIONS; return APP_REDRAW_PAGE; }
        /*
         * 这里的起始章不是"书签"而是"本地记住的阅读位置"：未读 N 章的语义就是
         * 从当前位置往后 N 章，整本则完全按老路从第一章起。
         * The start chapter is the locally remembered reading position: "next N chapters"
         * means this position onward, and the whole book keeps its chapter-one behaviour.
         */
        unsigned ch = s_scope_chapter;
        /* 记住范围，完成时按"真正新下几章"如实汇报。/ Remember the span for an honest report. */
        s_range_from = index == 0 ? 0 : ch;
        s_range_span = index == 0 ? 0 : index == 1 ? 5 : index == 2 ? 10 : 20;
        if (index == 0) return start_range(ctx, WR_CACHE, 0, 0);
        if (index == 1) return start_range(ctx, WR_CACHE, ch, 5);
        if (index == 2) return start_range(ctx, WR_CACHE, ch, 10);
        return start_range(ctx, WR_CACHE, ch, 20);
    }
    if (s_view == CONFIRM_DEL) {
        if (index == 3) { delete_local_cache(ctx); return APP_REDRAW_PAGE; }
        s_view = SHELF;
        return APP_REDRAW_PAGE;
    }
    if (s_view == ASK_WIFI && s_ask_connect) {
        if (index == 0) {
            s_wifi_open = true; s_open_after_wifi = true; s_wifi_wait_ms = ctx->now_ms;
            connect_wifi(ctx);
            return APP_REDRAW_NONE;
        }
        s_ask_connect = false; s_open_after_wifi = false; s_view = ACTIONS;
        return APP_REDRAW_PAGE;
    }
    if (s_view == ASK_WIFI) {
        if (index == 0) { s_wifi_open=true; connect_wifi(ctx); return APP_REDRAW_NONE; }
        s_wifi_open=false;
        if (index == 1) { open_book(ctx,true); return APP_REDRAW_NONE; }
        s_open[0]=0;
        s_view=SHELF;
        if (!s_back_to_weread) { ctx->request_return=true; return APP_REDRAW_NONE; }
        return APP_REDRAW_PAGE;
    }
    if (s_view == LOGOUT) {
        if (index == 2) { s_view=ACCOUNT; return APP_REDRAW_PAGE; }
        if (!wr_logout(index==1)) { s_message="退出失败，请重试"; return APP_REDRAW_PAGE; }
        /* consent 才是真正的登录态：一并清掉，否则下一个 tick 会用 consent 重新 mint，退出形同虚设。 */
        wr_consent_logout();
        s_session_verified_us = 0;
        if (index == 1) book_store_notify_changed();
        s_message=NULL;
        s_view=ACCOUNT;
        refresh_shelf();
        return APP_REDRAW_PAGE;
    }
    return APP_REDRAW_NONE;
}

static app_redraw_t gesture(app_ctx_t *ctx,const ui_gesture_event_t *event) {
    const int x=event->x0,y=event->y0;
    if (s_view == SHELF && (event->type == UI_GESTURE_SWIPE_U || event->type == UI_GESTURE_SWIPE_D)) {
        int page = s_page + (event->type==UI_GESTURE_SWIPE_U ? 1 : -1);
        if (page>=0 && page<pages()) { s_page=page; refresh_shelf(); return APP_REDRAW_PAGE; }
    }
    if (event->type != UI_GESTURE_TAP) return APP_REDRAW_NONE;
    if (s_message && s_view != WORK) {
        const bool retry = s_has_retry && ui_rect_hit(retry_rect(),x,y);
        const wr_job_t job = s_retry_job;
        s_message = NULL;
        s_has_retry = false;
        if (retry) return start(ctx,job);
        return APP_REDRAW_PAGE;
    }
    if (s_view == WORK) {
        if (ui_rect_hit(action_rect(4),x,y)) { wr_cancel(); s_message="正在取消"; return APP_REDRAW_PAGE; }
        return APP_REDRAW_NONE;
    }
    if (s_view == ACTIONS || s_view == CACHE_SCOPE || s_view == ASK_WIFI || s_view == LOGOUT || s_view == CONFIRM_DEL) {
        const int count = s_view==ACTIONS?6:s_view==CACHE_SCOPE?6:s_view==CONFIRM_DEL?5:3;
        for (int i=0; i<count; ++i) {
            EpdRect r = s_view==ACTIONS ? sheet_rect(i) : action_rect(i);
            if (ui_rect_hit(r,x,y)) return activate(ctx,i);
        }
        /*
         * 点到空白处也关掉弹窗。以前只有"关闭"按钮能退出，读者点到卡片外面什么也不会发生，
         * 看起来就像页面卡住了。
         * A tap outside closes the sheet as well. Before, only the Close button worked, and a tap
         * on the card's margins did nothing — the page read as stuck.
         */
        if (s_view == ACTIONS || s_view == CONFIRM_DEL) { s_view = SHELF; return APP_REDRAW_PAGE; }
        if (s_view == CACHE_SCOPE) { s_view = ACTIONS; return APP_REDRAW_PAGE; }
        return activate(ctx,2);
    }
    int tab=ui_nav_hit(x,y);
    if (tab>=0) { ui_nav_request(ctx,tab); return APP_REDRAW_NONE; }
    if (s_view == ACCOUNT) {
        if (ui_rect_hit(action_rect(2),x,y)) {
            if (!s_logged) return begin_login(ctx);
            s_view=LOGOUT;
            return APP_REDRAW_PAGE;
        }
        if (ui_rect_hit(action_rect(3),x,y)) { s_view=SHELF; s_message=NULL; return APP_REDRAW_PAGE; }
        if (ui_rect_hit(action_rect(4),x,y)) { s_view=SYNC; s_message=NULL; return APP_REDRAW_PAGE; }
        if (ui_rect_hit(action_rect(5),x,y)) return renew_now(ctx);
        return APP_REDRAW_NONE;
    }
    if (s_view == SYNC) {
        if (ui_rect_hit(sync_button_rect(0),x,y)) {
            if (wr_pending_seconds()) {
                wr_pending_kick();
                s_message = wr_online() ? "已开始补传，补传完成后数字会自动减少"
                                        : "当前没有网络，连上 Wi-Fi 后会自动补传";
                s_has_retry = false;
            }
            return APP_REDRAW_PAGE;
        }
        if (ui_rect_hit(sync_button_rect(1),x,y)) {
            /* 进入授权页：已授权则显示管理，未授权则发起扫码。 */
            return enter_consent_login();
        }
        if (ui_rect_hit(sync_button_rect(2),x,y)) {
            /* 开关直接反映在按钮文字上，不弹模态提示，避免每次切换都多一次点击。
             * The button label carries the state; no modal, so toggling stays one tap. */
            if (!app_settings_set_reader_fast_replay(!app_settings_reader_fast_replay()))
                s_message = "保存设置失败，请重试";
            return APP_REDRAW_PAGE;
        }
        if (ui_rect_hit(sync_button_rect(3),x,y)) { s_view=ACCOUNT; s_message=NULL; return APP_REDRAW_PAGE; }
        return APP_REDRAW_NONE;
    }
    if (s_view == CONSENT) {
        wr_consent_status_t st;
        wr_consent_poll(&st);
        if (wr_consent_authorized() || st.state == WR_CONSENT_OK) {
            if (ui_rect_hit(consent_button_rect(0),x,y)) {
                /* 重新授权。/ Re-authorize. */
                s_consent_shown = WR_CONSENT_IDLE;
                s_consent_qr[0] = 0;
                ui_wifi_qr_clear();
                wr_consent_begin();
                return APP_REDRAW_PAGE;
            }
            if (ui_rect_hit(consent_button_rect(1),x,y)) {
                wr_consent_logout();
                /* 网页会话也要一起失效，否则 consent 取消后旧 wr_* cookie 仍能访问书架/正文。 */
                (void)wr_clear_web_session();
                s_view = SYNC; s_message = "已取消授权，待发时长仍保留";
                return APP_REDRAW_PAGE;
            }
            if (ui_rect_hit(consent_button_rect(2),x,y)) {
                s_view = SYNC; s_message = NULL; return APP_REDRAW_PAGE;
            }
            return APP_REDRAW_NONE;
        }
        if (ui_rect_hit(consent_button_rect(0),x,y)) {
            if (st.state == WR_CONSENT_EXPIRED || st.state == WR_CONSENT_DECLINED ||
                st.state == WR_CONSENT_FAILED) {
                /* 重新获取二维码。/ Fetch a fresh QR code. */
                s_consent_shown = WR_CONSENT_IDLE;
                s_consent_qr[0] = 0;
                ui_wifi_qr_clear();
                wr_consent_begin();
                return APP_REDRAW_PAGE;
            }
            /* 取消并返回。/ Cancel and go back. */
            wr_consent_cancel();
            ui_wifi_qr_clear();
            s_view = SYNC; s_message = NULL;
            return APP_REDRAW_PAGE;
        }
        return APP_REDRAW_NONE;
    }
    if (ui_rect_hit(account_rect(),x,y)) {
        s_pressed = 1; s_pressed_until = ctx->now_ms+140; s_pending_action = 1;
        return APP_REDRAW_PAGE;
    }
    if (ui_rect_hit(sync_rect(),x,y)) {
        s_pressed = 0; s_pressed_until = ctx->now_ms+140; s_pending_action = 0;
        return APP_REDRAW_PAGE;
    }
    int pager=ui_pager_hit(x,y);
    if (pager==UI_PAGER_PREVIOUS || pager==UI_PAGER_NEXT) {
        int next=s_page+(pager==UI_PAGER_PREVIOUS?-1:1);
        if (next>=0 && next<pages()) { s_page=next; refresh_shelf(); return APP_REDRAW_PAGE; }
    }
    int slots = app_book_shelf_slots();
    if (slots > WR_SLOTS) slots = WR_SLOTS;
    for (int i=0; i<slots && s_books[i].id[0]; ++i) if (ui_rect_hit(app_book_shelf_slot(i),x,y)) {
        s_selected=s_books[i];
        s_selected_index=s_page*slots+i;
        s_selected_full=s_cache_kind[i] == WR_SHELF_FULL;
        s_view=ACTIONS;
        s_message=NULL;
        return APP_REDRAW_PAGE;
    }
    return APP_REDRAW_NONE;
}

static bool root(app_ctx_t *ctx) { (void)ctx; return s_view==SHELF; }
static bool navigation(app_ctx_t *ctx) { (void)ctx; return s_view==SHELF || s_view==ACCOUNT || s_view==SYNC; }
static app_redraw_t back(app_ctx_t *ctx) {
    if (s_view==ASK_WIFI || s_view==LOGOUT) return activate(ctx,2);
    if (s_view==WORK) { wr_cancel(); return APP_REDRAW_NONE; }
    if (s_view==SHELF) { ui_nav_request(ctx,UI_TAB_HOME); return APP_REDRAW_NONE; }
    /* 授权页退回同步页，同步页退回账户页，账户页退回书架，逐级往回，不一次跳到底。
     * The consent page steps back to the sync page, which steps back to the account page,
     * which steps back to the shelf — one level at a time. */
    if (s_view==CONSENT) {
        wr_consent_cancel();
        ui_wifi_qr_clear();
        s_view=SYNC; s_message=NULL; return APP_REDRAW_PAGE;
    }
    if (s_view==SYNC) { s_view=ACCOUNT; s_message=NULL; return APP_REDRAW_PAGE; }
    if (s_view==CACHE_SCOPE) { s_view=ACTIONS; s_message=NULL; return APP_REDRAW_PAGE; }
    s_view=SHELF;
    return APP_REDRAW_PAGE;
}
static app_redraw_t key(app_ctx_t *ctx,int key) { return key==1?back(ctx):APP_REDRAW_NONE; }
static void media(app_ctx_t *ctx) { (void)ctx; refresh_shelf(); }
static void lost(app_ctx_t *ctx) { wr_cancel(); media(ctx); }
static void locking(app_ctx_t *ctx) { (void)ctx; if (s_view == WORK) wr_cancel(); }

const app_desc_t app_weread = {
    .title="微信读书", .detail="微信读书书架", .enter_full=true,
    .persistent_navigation=true, .navigation_visible=navigation, .navigation_enabled=navigation,
    .main_page_visible=root,
    .on_enter=enter, .on_exit=leave, .on_tick=tick, .render=render,
    .on_nav_reselect=nav_reselect,
    .on_gesture=gesture, .on_key=key, .on_media_lost=lost, .on_media_ready=media,
    .on_before_lock=locking,
};
