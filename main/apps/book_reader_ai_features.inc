/*
 * book_reader_ai_features.inc
 * 三个仿月读 AI 功能（2026-11-19）：
 *   1) 本章段评面板（READER_PANEL_CRV）+ 四角星一键 AI 归纳
 *   2) 文字选择（READER_PANEL_SELECT）+ AI 解释/翻译（READER_PANEL_AICARD）
 *   3) 隔 3 天重开自动 AI 回顾（AIC_RECAP）
 * 真实“开书时间”不进 NVS（NVS 的 last_open_s 是排序序号），改用 SD 文件 openstamps。
 */
#include <sys/stat.h>
#include <errno.h>

#define AIC_PSRAM_CAPS (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#define AIC_OPENSTAMP_DIR WR_DIR "/openstamps"
#define AIC_RECAP_GAP_S   (3u * 24u * 3600u)

static char *aic_psram_dup(const char *s) {
    if (!s) return NULL;
    size_t n = strlen(s) + 1;
    char *p = heap_caps_malloc(n, AIC_PSRAM_CAPS);
    if (p) memcpy(p, s, n);
    return p;
}

/* 折行后绘制单行：只画 [st[k], st[k+1]) 这一段，末行画到字符串结尾。
   用精确区间而非"画整串再裁剪"，避免上一行末字被下一行首字重复。 */
static void aic_draw_wrapped_line(uint8_t *fb, const char *text, const uint16_t *st,
                                  size_t lines, size_t k, int x, int y, int px,
                                  int clip_right) {
    size_t e = (k + 1 < lines) ? (size_t)st[k + 1] : strlen(text);
    ttf_draw_text_px_segment(fb, x, y, px, text, st[k], e, 0, 15, clip_right);
}

/* ================================================================
 * 开书时间戳（真实 epoch，SD 文件）
 * ================================================================ */
static void aic_stamp_path(const char *book_path, char *out, size_t cap) {
    uint32_t h = esp_rom_crc32_le(0, (const uint8_t *)book_path, strlen(book_path));
    snprintf(out, cap, "%s/%08x.stamp", AIC_OPENSTAMP_DIR, (unsigned)h);
}

static uint32_t aic_stamp_load(const char *book_path) {
    char p[WR_PATH];
    aic_stamp_path(book_path, p, sizeof(p));
    FILE *f = fopen(p, "rb");
    if (!f) return 0;
    uint32_t v = 0;
    if (fread(&v, 1, sizeof(v), f) != sizeof(v)) v = 0;
    fclose(f);
    return v;
}

static void aic_stamp_save(const char *book_path, uint32_t epoch) {
    (void)mkdir(WR_DIR, 0777);
    if (mkdir(AIC_OPENSTAMP_DIR, 0777) && errno != EEXIST) return;
    char p[WR_PATH];
    aic_stamp_path(book_path, p, sizeof(p));
    FILE *f = fopen(p, "wb");
    if (!f) return;
    (void)fwrite(&epoch, 1, sizeof(epoch), f);
    fclose(f);
}

/* ================================================================
 * 通用 AI 结果卡（READER_PANEL_AICARD）
 * ================================================================ */
typedef enum {
    AIC_DIGEST = 0,
    AIC_EXPLAIN,
    AIC_TRANSLATE,
    AIC_RECAP,
} aic_mode_t;

static ai_config_t s_aic_cfg;
static aic_mode_t s_aic_mode;
static char *s_aic_answer;       /* PSRAM，可空 */
static char *s_aic_sys;          /* PSRAM，可空 */
static char *s_aic_user;         /* PSRAM，可空 */
static char *s_aic_context;      /* PSRAM，可空 */
static bool s_aic_busy, s_aic_pending;
static int s_aic_page;
static bool s_aic_page_pending;
static int s_aic_max_tokens;   /* >0 时本次请求覆盖全局 max_tokens（归纳用小上限），0=用配置值 */
static char s_aic_title[40];
static char s_aic_follow[128];
static ui_text_edit_t s_aic_follow_input;

static EpdRect aic_card_rect(void) { return notes_card_rect(); }

/* 回答模式（设置页可选）：0=日常问答（简洁直接），1=深度问答（专业分点、不冗长）。
 * 影响解释类系统提示与本次 token 上限，从而控制生成速度与详略。 */
static const char *aic_mode_suffix(void) {
    return s_aic_cfg.answer_mode ?
        "回答专业严谨，可分点说明并给出必要依据，但不要冗长。" :
        "回答简洁直接，不展开，不堆砌。";
}
static int aic_mode_token_cap(void) {
    return s_aic_cfg.answer_mode ? 900 : 380;
}

static bool aic_has_tabs(void) {
    return s_aic_mode == AIC_EXPLAIN || s_aic_mode == AIC_TRANSLATE;
}

/* 顶部只保留两个标签：AI / 翻译，按钮加宽、间距舒适。 */
static EpdRect aic_tab_rect(int i) {
    EpdRect c = aic_card_rect();
    int bw = (c.width - 48 - 16) / 2;
    return (EpdRect){c.x + 24 + i * (bw + 16), c.y + 48, bw, 48};
}

static EpdRect aic_answer_rect(void) {
    EpdRect c = aic_card_rect();
    int top = c.y + (aic_has_tabs() ? 104 : 52);
    /* 底部留出页码行 + 三个操作按钮的位置。 */
    int bottom = c.y + c.height - 100;
    return (EpdRect){c.x + 24, top, c.width - 48, bottom - top};
}

/* 答案页码行（翻页提示）。 */
static EpdRect aic_page_indicator_rect(void) {
    EpdRect c = aic_card_rect();
    return (EpdRect){c.x + 24, c.y + c.height - 92, c.width - 48, 28};
}

static EpdRect aic_bottom_rect(int i) {
    EpdRect c = aic_card_rect();
    int bw = (c.width - 48 - 22) / 3;
    return (EpdRect){c.x + 24 + i * (bw + 11), c.y + c.height - 54, bw, 46};
}

static void aicard_reset_buffers(void) {
    free(s_aic_answer); s_aic_answer = NULL;
    free(s_aic_sys); s_aic_sys = NULL;
    free(s_aic_user); s_aic_user = NULL;
    free(s_aic_context); s_aic_context = NULL;
}

static void aicard_open(aic_mode_t mode, const char *title, const char *context,
                        const char *sys, const char *user) {
    ai_config_load(&s_aic_cfg);
    aicard_reset_buffers();
    s_aic_context = aic_psram_dup(context);
    s_aic_sys = aic_psram_dup(sys);
    s_aic_user = aic_psram_dup(user);
    s_aic_mode = mode;
    s_aic_busy = false; s_aic_pending = true;
    s_aic_page = 0; s_aic_page_pending = false;
    s_aic_max_tokens = 0; s_aic_follow[0] = 0;
    snprintf(s_aic_title, sizeof(s_aic_title), "%s", title ? title : "AI");
    s_reader_panel = READER_PANEL_AICARD;
}

/* 用同一选区切换 AI/翻译 标签并重新请求（词典/百科已退役）。 */
static void aicard_rearm_tab(aic_mode_t mode) {
    const char *ctx = s_aic_context ? s_aic_context : "";
    char sys[200], user[1200];
    if (mode == AIC_TRANSLATE) {
        snprintf(sys, sizeof(sys), "你是翻译，中文与英文互译，只输出译文，不要解释。");
        snprintf(user, sizeof(user), "请翻译下面内容：%s", ctx);
    } else {
        snprintf(sys, sizeof(sys), "你是阅读助手，结合上下文通俗解释，简洁准确。%s",
                 aic_mode_suffix());
        snprintf(user, sizeof(user), "请通俗解释这段文字的含义、作用与关键信息：%s", ctx);
    }
    free(s_aic_sys); s_aic_sys = aic_psram_dup(sys);
    free(s_aic_user); s_aic_user = aic_psram_dup(user);
    free(s_aic_answer); s_aic_answer = NULL;
    s_aic_mode = mode; s_aic_pending = true;
    s_aic_page = 0; s_aic_page_pending = false;
    s_aic_max_tokens = (mode == AIC_TRANSLATE) ? 300 : aic_mode_token_cap();
}

static app_redraw_t aicard_run_tick(app_ctx_t *ctx) {
    if (!s_aic_pending) return APP_REDRAW_NONE;
    s_aic_pending = false;
    s_aic_busy = true;
    render(ctx, ctx->fb); /* 先把“正在请求”刷出来 */
    char *ans = NULL;
    bool ok = ai_chat_limit(&s_aic_cfg, s_aic_sys, s_aic_user, &ans, s_aic_max_tokens);
    if (ok && ans) { free(s_aic_answer); s_aic_answer = ans; }
    else free(ans);
    s_aic_busy = false;
    if (!ok) {
        /* 明确的失败弹窗（含归纳），停留更久，避免静默无反馈。 */
        rc_notice(ctx, "失败了，请再试一次");
        s_reader_notice_until = ctx->now_ms + 3200;
    }
    return APP_REDRAW_PAGE;
}

static void aicard_draw(uint8_t *fb) {
    EpdRect card = aic_card_rect();
    ui_fill_round_rect(fb, card, 16, UI_GRAY_WHITE);
    ui_draw_control_frame(fb, card, 16, 0x60);
    ui_text(fb, card.x + 24, card.y + 12, 24, s_aic_title, EPD_DRAW_ALIGN_LEFT, false);
    EpdRect close = {card.x + card.width - 112, card.y + 10, 88, 44};
    ui_draw_button(fb, close, "关闭", false);

    if (aic_has_tabs()) {
        static const char *const tabs[] = {"AI", "翻译"};
        const aic_mode_t modes[] = {AIC_EXPLAIN, AIC_TRANSLATE};
        for (int i = 0; i < 2; ++i) {
            EpdRect r = aic_tab_rect(i);
            bool active = modes[i] == s_aic_mode;
            ui_fill_round_rect(fb, r, 12, active ? UI_GRAY_BLACK : 0xf0);
            ui_draw_control_frame(fb, r, 12, active ? UI_GRAY_BLACK : 0x98);
            ui_text_vc(fb, r.x + r.width / 2, r.y + 12, 24, tabs[i],
                       EPD_DRAW_ALIGN_CENTER, active);
        }
    }

    EpdRect area = aic_answer_rect();
    if (s_aic_busy) {
        ui_text(fb, area.x, area.y, NOTES_CARD_PX, "正在请求，请稍候（最长约 30 秒）…",
                EPD_DRAW_ALIGN_LEFT, false);
    } else if (s_aic_answer && s_aic_answer[0]) {
        /* 分页显示：按答案区每页行数切页，上下滑动/点按翻页，不再滚动。 */
        uint16_t *st = NULL;
        size_t lines = ai_wrap_lines(s_aic_answer, area.width - 4, &st);
        int per_page = area.height / NOTES_PITCH;
        if (per_page < 1) per_page = 1;
        int pages = ((int)lines + per_page - 1) / per_page;
        if (pages < 1) pages = 1;
        if (s_aic_page >= pages) s_aic_page = pages - 1;
        if (s_aic_page < 0) s_aic_page = 0;
        const int asc = ttf_ascender_px(NOTES_CARD_PX);
        int start_line = s_aic_page * per_page;
        int y = area.y;
        for (int k = start_line;
             k < (int)lines && y + NOTES_PITCH <= area.y + area.height; ++k) {
            aic_draw_wrapped_line(fb, s_aic_answer, st, lines, (size_t)k,
                                  area.x, y + asc, NOTES_CARD_PX, area.x + area.width);
            y += NOTES_PITCH;
        }
        free(st);

        /* 页码行：以指示条中心为锚点绘制，避免左侧文字超出卡片。 */
        EpdRect ind = aic_page_indicator_rect();
        char pg[40];
        snprintf(pg, sizeof(pg), "第 %d / %d 页 · 上下滑动翻页",
                 s_aic_page + 1, pages);
        ui_text(fb, ind.x + ind.width / 2, ind.y, 20, pg,
                EPD_DRAW_ALIGN_CENTER, false);
    } else {
        /* 答案尚未生成：统一提示请稍候，不再显示暂无内容。 */
        ui_text(fb, area.x, area.y, NOTES_CARD_PX, "请稍候……",
                EPD_DRAW_ALIGN_LEFT, false);
    }

    static const char *const bottoms[] = {"复制", "存为笔记", "追问"};
    for (int i = 0; i < 3; ++i)
        ui_draw_button(fb, aic_bottom_rect(i), bottoms[i], s_aic_busy);
}

static void aicard_write_clipboard(app_ctx_t *ctx) {
    const char *path = WR_DIR "/clipboard.txt";
    FILE *f = fopen(path, "wb");
    if (!f) { rc_notice(ctx, "复制失败"); return; }
    const char *t = s_aic_answer ? s_aic_answer : "";
    (void)fwrite(t, 1, strlen(t), f);
    fclose(f);
    rc_notice(ctx, "已复制到剪贴板文件");
}

static void aicard_save_note(app_ctx_t *ctx) {
    if (!s_reader_note_chapter[0]) { rc_notice(ctx, "本地书暂不支持存笔记"); return; }
    const char *content = s_aic_answer ? s_aic_answer : "";
    const char *abstract = s_aic_context ? s_aic_context : "";
    bool ok = wrn_add_review(s_reader_note_chapter, "", content, abstract, true);
    rc_notice(ctx, ok ? "已存为私有笔记" : "保存失败");
}

static void aicard_open_follow(void) {
    s_aic_follow[0] = 0;
    ui_text_edit_init(&s_aic_follow_input, s_aic_follow, sizeof(s_aic_follow));
    /* 必须绑定键盘到输入框，否则 ui_keyboard_tap 因 s_edit 为空而全部无响应。 */
    ui_keyboard_begin(&s_aic_follow_input, false);
    s_view = AIC_FOLLOW;
}

static void aicard_submit_follow(app_ctx_t *ctx) {
    if (!s_aic_follow[0]) return;
    size_t cap = 2048;
    char *user = heap_caps_malloc(cap, AIC_PSRAM_CAPS);
    if (!user) { rc_notice(ctx, "内存不足"); return; }
    snprintf(user, cap,
             "原文片段：%s\n\n上一轮回答：%s\n\n请基于以上内容继续回答我的追问：%s",
             s_aic_context ? s_aic_context : "(无)",
             s_aic_answer ? s_aic_answer : "(无)", s_aic_follow);
    free(s_aic_user); s_aic_user = user;
    s_aic_pending = true; s_aic_page = 0; s_aic_page_pending = false;
    s_view = READING; s_reader_panel = READER_PANEL_AICARD;
}

static app_redraw_t aicard_action(app_ctx_t *ctx, int x, int y) {
    EpdRect card = aic_card_rect();
    /* 点卡片以外区域：直接关闭弹窗。 */
    if (!ui_rect_hit(card, x, y)) {
        s_reader_panel = READER_PANEL_NONE;
        return APP_REDRAW_PAGE;
    }
    EpdRect close = {card.x + card.width - 112, card.y + 10, 88, 44};
    if (ui_rect_hit(close, x, y)) { s_reader_panel = READER_PANEL_NONE; return APP_REDRAW_PAGE; }
    if (s_aic_busy) return APP_REDRAW_NONE;
    if (aic_has_tabs()) {
        const aic_mode_t modes[] = {AIC_EXPLAIN, AIC_TRANSLATE};
        for (int i = 0; i < 2; ++i)
            if (ui_rect_hit(aic_tab_rect(i), x, y) && modes[i] != s_aic_mode) {
                aicard_rearm_tab(modes[i]);
                return APP_REDRAW_PAGE;
            }
    }
    /* 点答案区：上半屏上一页、下半屏下一页，局部刷新不闪屏。 */
    EpdRect ans = aic_answer_rect();
    if (ui_rect_hit(ans, x, y)) {
        int dir = (y < ans.y + ans.height / 2) ? -1 : 1;
        if (aic_page_turn(dir)) { s_aic_page_pending = true; return APP_REDRAW_AREA; }
        return APP_REDRAW_NONE;
    }
    if (ui_rect_hit(aic_bottom_rect(0), x, y)) { aicard_write_clipboard(ctx); return APP_REDRAW_PAGE; }
    if (ui_rect_hit(aic_bottom_rect(1), x, y)) { aicard_save_note(ctx); return APP_REDRAW_PAGE; }
    if (ui_rect_hit(aic_bottom_rect(2), x, y)) { aicard_open_follow(); return APP_REDRAW_PAGE; }
    return APP_REDRAW_NONE;
}

/* 答案区每页可容纳行数。 */
static int aic_per_page(void) {
    int per = aic_answer_rect().height / NOTES_PITCH;
    return per < 1 ? 1 : per;
}

/* 当前答案的总页数。 */
static int aic_total_pages(void) {
    if (!s_aic_answer || !s_aic_answer[0]) return 1;
    EpdRect area = aic_answer_rect();
    uint16_t *st = NULL;
    size_t lines = ai_wrap_lines(s_aic_answer, area.width - 4, &st);
    free(st);
    int pages = ((int)lines + aic_per_page() - 1) / aic_per_page();
    return pages < 1 ? 1 : pages;
}

/* 翻页：dir +1 下一页 / -1 上一页；返回是否真正移动。 */
static bool aic_page_turn(int dir) {
    int pages = aic_total_pages();
    int np = s_aic_page + dir;
    if (np < 0) np = 0;
    if (np >= pages) np = pages - 1;
    if (np == s_aic_page) return false;
    s_aic_page = np;
    return true;
}

/* 答案翻页局部刷新：只重画并短刷答案区+页码行，不整屏闪。 */
static bool aic_page_present(app_ctx_t *ctx, app_redraw_t redraw) {
    if (!s_aic_page_pending) return false;
    s_aic_page_pending = false;
    if (redraw != APP_REDRAW_AREA) return false;
    EpdRect area = aic_answer_rect();
    EpdRect ind = aic_page_indicator_rect();
    lock_draw();
    aicard_draw(ctx->fb);
    unlock_draw();
    EpdRect region = {area.x, area.y, area.width,
                      (ind.y + ind.height) - area.y};
    enum EpdDrawError err = update_display_area_diff_with(
        ctx->hl, &E0470_WAVEFORM, MODE_GL16, region);
    guard_draw_result(ctx->hl, err);
    return true;
}

/* 追问全屏键盘（仿 AI_ASK）。 */
static void aic_follow_draw(uint8_t *fb) {
    ui_clear_page(fb);
    ui_nav_status(fb);
    ui_nav_back(fb, 36, 79);
    ui_text_vc(fb, UI_LOCK_WIDTH / 2, 107, 31, "追问 AI",
               EPD_DRAW_ALIGN_CENTER, false);
    ui_text_vc(fb, 646, 107, 26, "完成", EPD_DRAW_ALIGN_RIGHT, false);
    ui_hairline(fb, 133, 36, 612, UI_GRAY_LIGHT);
    ui_text(fb, 36, 200, 22, "结合原文与上一轮回答继续提问",
            EPD_DRAW_ALIGN_LEFT, false);
    ui_text_input_draw(fb, &s_aic_follow_input,
                       (EpdRect){36, 244, 612, 84}, 28, false, NULL);
    ui_keyboard_draw(fb, 560);
}

static app_redraw_t aic_follow_action(app_ctx_t *ctx, uint16_t x, uint16_t y) {
    if (y < 134) {
        if (x < 160) {
            ui_keyboard_end();
            s_view = READING; s_reader_panel = READER_PANEL_AICARD;
            return APP_REDRAW_PAGE;
        }
        if (x > 510) goto submit;
        return APP_REDRAW_NONE;
    }
    if (ui_text_input_tap(&s_aic_follow_input,
                          (EpdRect){36, 244, 612, 84}, 28, false, NULL, x, y))
        return ai_keyboard_paint(ctx, true);
    ui_keyboard_result_t result = ui_keyboard_tap(x, y, 560, ctx->now_ms);
    if (result == UI_KEYBOARD_DONE) goto submit;
    return result == UI_KEYBOARD_CHANGED ? ai_keyboard_paint(ctx, false) : APP_REDRAW_NONE;
submit:
    if (ui_keyboard_pending()) return APP_REDRAW_NONE;
    if (!s_aic_follow[0]) return APP_REDRAW_NONE;
    ui_keyboard_end();
    aicard_submit_follow(ctx);
    return APP_REDRAW_PAGE;
}

/* ================================================================
 * 文字选择（READER_PANEL_SELECT）
 * ================================================================ */
static bool s_select_armed;
static size_t s_sel_start, s_sel_end;
static char *s_sel_text;

/* 解码 i 处 UTF-8 码点，*adv 给出占用字节数；非法序列按单字节处理。 */
static uint32_t aic_utf8_decode(size_t i, size_t *adv) {
    if (i >= s_text_len) { if (adv) *adv = 0; return 0; }
    unsigned char b0 = (unsigned char)s_text[i];
    if (b0 < 0x80) { if (adv) *adv = 1; return b0; }
    int need; uint32_t cp;
    if ((b0 & 0xE0) == 0xC0) { need = 2; cp = b0 & 0x1F; }
    else if ((b0 & 0xF0) == 0xE0) { need = 3; cp = b0 & 0x0F; }
    else if ((b0 & 0xF8) == 0xF0) { need = 4; cp = b0 & 0x07; }
    else { if (adv) *adv = 1; return b0; }
    size_t a = 1;
    for (; a < (size_t)need && i + a < s_text_len; ++a) {
        unsigned char b = (unsigned char)s_text[i + a];
        if ((b & 0xC0) != 0x80) break;
        cp = (cp << 6) | (b & 0x3F);
    }
    if (adv) *adv = a;
    if (a < (size_t)need) return b0; /* 残缺序列：退回原始首字节。 */
    return cp;
}

/* 词字符分类：0=非词（空白/标点），1=拉丁系词，2=中日韩词。 */
static int aic_word_cat(uint32_t cp) {
    if (cp == 0) return 0;
    /* ASCII 字母数字、下划线/连字符、撇号（含中文弯撇号），保证 don't 等不被切断。 */
    if ((cp >= 'a' && cp <= 'z') || (cp >= 'A' && cp <= 'Z') ||
        (cp >= '0' && cp <= '9') || cp == '_' || cp == '-' ||
        cp == 0x27 || cp == 0x2019) return 1;
    /* Latin-1 补充及拉丁扩展字母（带重音的欧洲语言）。 */
    if (cp >= 0x00C0 && cp <= 0x024F) return 1;
    /* CJK 统一表意文字及扩展 A。 */
    if ((cp >= 0x4E00 && cp <= 0x9FFF) || (cp >= 0x3400 && cp <= 0x4DBF)) return 2;
    /* 日文平假名/片假名。 */
    if (cp >= 0x3040 && cp <= 0x30FF) return 2;
    /* 韩文音节与字母。 */
    if ((cp >= 0xAC00 && cp <= 0xD7AF) || (cp >= 0x1100 && cp <= 0x11FF)) return 2;
    return 0;
}

/* 中文功能字（代词/助词/虚词/常见量词与方位）：在无词典的设备上把它们当作高置信度的
 * 单字词与断词边界，避免"词语"把一整句连续中文全选。内容字仍由 compute_range 的长度上限兜底。 */
static bool aic_cjk_function(uint32_t cp) {
    static const uint32_t kFn[] = {
        /* 结构助词/语气助词。 */
        0x7684,0x5730,0x5F97,0x4E86,0x7740,0x8FC7,0x5417,0x5462,0x5427,0x554A,
        0x5440,0x54E6,0x561B,0x5457,0x55BD,0x4E48,0x4EEC,
        /* 代词/指示/限定。 */
        0x6211,0x4F60,0x4ED6,0x5979,0x5B83,0x60A8,0x54B1,0x8FD9,0x90A3,0x54EA,
        0x8C01,0x6BCF,0x5404,0x67D0,0x53E6,0x4EC0,0x600E,
        /* 连词/介词/副词/判断及常见虚词。 */
        0x548C,0x4E0E,0x53CA,0x6216,0x800C,0x4F46,0x5374,0x4E5F,0x8FD8,0x53C8,
        0x5C31,0x90FD,0x5DF2,0x66FE,0x6B63,0x5C06,0x624D,0x4FBF,0x5373,0x5219,
        0x662F,0x5728,0x6709,0x4E0D,0x6CA1,0x5F88,0x6700,0x66F4,0x628A,0x88AB,
        0x8BA9,0x7ED9,0x5411,0x4ECE,0x5BF9,0x4E3A,0x4EE5,0x4E8E,0x4E4B,0x5176,
        0x6B64,0x6240,0x53EF,0x80FD,0x4F1A,0x8981,0x4E0A,0x4E0B,0x4E2D,0x91CC,
        0x7B49,
        /* 高频数词与量词（在测试句中明显成边界）。 */
        0x4E00,0x4E8C,0x4E09,0x4E2A,0x5177
    };
    for (size_t i = 0; i < sizeof(kFn) / sizeof(kFn[0]); ++i)
        if (kFn[i] == cp) return true;
    return false;
}

/* 句/段边界：换行对句、段都成立；句还额外识别中英文句末标点。 */
static bool aic_is_boundary(size_t i, bool sentence) {
    if (i >= s_text_len) return true;
    unsigned char c = (unsigned char)s_text[i];
    if (c == '\n' || c == '\r') return true;
    if (!sentence) return false;
    if (c == '!' || c == '?' || c == ';') return true;
    if (c == 0xE3 && i + 2 < s_text_len &&
        (unsigned char)s_text[i + 1] == 0x80 &&
        (unsigned char)s_text[i + 2] == 0x82) return true; /* 。 */
    if (c == 0xEF && i + 2 < s_text_len &&
        (unsigned char)s_text[i + 1] == 0xBC &&
        ((unsigned char)s_text[i + 2] == 0x81 ||
         (unsigned char)s_text[i + 2] == 0x9F ||
         (unsigned char)s_text[i + 2] == 0x9B)) return true; /* 全角！？； */
    return false;
}

/* 以 anchor 为基准，按 mode（0词/1句/2段）求选区字节范围。 */
/* 回退到 p 之前最近一个码点的首字节（p 本身不计）。 */
static size_t aic_prev_lead(size_t p) {
    if (p == 0) return 0;
    --p;
    while (p > 0 && ((unsigned char)s_text[p] & 0xC0) == 0x80) --p;
    return p;
}

/* 以 anchor 为基准，按 mode（0词/1句/2段）求选区字节范围。 */
static void aic_compute_range(size_t anchor, int mode) {
    size_t st = anchor, en = anchor;
    if (s_text_len && anchor >= s_text_len) anchor = s_text_len - 1;
    if (mode == 0) {
        size_t adv;
        uint32_t cp = aic_utf8_decode(anchor, &adv);
        int cat = aic_word_cat(cp);
        if (cat == 0) {
            /* 落在标点/空白：就近找一个词字作为新锚点，避免选到纯空白被裁空。 */
            size_t q = anchor;
            bool found = false;
            for (unsigned step = 0; step < 24 && q < s_text_len; ++step) {
                size_t qa; uint32_t qcp = aic_utf8_decode(q, &qa);
                if (aic_word_cat(qcp) != 0) { found = true; break; }
                if (!qa) break;
                q += qa;
            }
            if (found) { anchor = q; cp = aic_utf8_decode(anchor, &adv); cat = aic_word_cat(cp); }
        }
        if (cat == 1) {
            /* 拉丁系：连续字母数字串。 */
            st = anchor;
            while (st > 0) {
                size_t p = aic_prev_lead(st);
                size_t pa;
                if (aic_word_cat(aic_utf8_decode(p, &pa)) != cat) break;
                st = p;
            }
            en = anchor + adv;
            for (;;) {
                size_t na;
                uint32_t ncp = aic_utf8_decode(en, &na);
                if (!na || aic_word_cat(ncp) != cat) break;
                en += na;
            }
        } else if (cat == 2) {
            if (aic_cjk_function(cp)) {
                /* 功能字（代词/助词/虚词/量词）单字成词。 */
                st = anchor; en = anchor + adv;
            } else {
                /* 内容字：跨相邻内容字扩展，遇功能字/标点/非CJK即停，总字数封顶 4。 */
                st = anchor; en = anchor + adv;
                unsigned total = 1;
                while (st > 0 && total < 4) {
                    size_t p = aic_prev_lead(st);
                    size_t pa; uint32_t pcp = aic_utf8_decode(p, &pa);
                    if (aic_word_cat(pcp) != 2 || aic_cjk_function(pcp)) break;
                    st = p; ++total;
                }
                while (en < s_text_len && total < 4) {
                    size_t na; uint32_t ncp = aic_utf8_decode(en, &na);
                    if (!na || aic_word_cat(ncp) != 2 || aic_cjk_function(ncp)) break;
                    en += na; ++total;
                }
            }
        } else {
            /* 无邻近词字：退化为选中锚点字符本身。 */
            st = anchor; en = anchor + (adv ? adv : 1);
        }
    } else {
        const bool sentence = (mode == 1);
        while (st > 0 && !aic_is_boundary(st - 1, sentence)) --st;
        en = anchor;
        while (en < s_text_len && !aic_is_boundary(en, sentence)) ++en;
        // 句末标点（含多字节 UTF-8）整体纳入选区。
        if (sentence && en < s_text_len &&
            s_text[en] != '\n' && s_text[en] != '\r') {
            ++en;
            while (en < s_text_len &&
                   ((unsigned char)s_text[en] & 0xc0) == 0x80) ++en;
        }
    }
    // 跳过首尾空白。
    while (st < en && (s_text[st] == ' ' || s_text[st] == '\t' ||
                       s_text[st] == '\n' || s_text[st] == '\r')) ++st;
    while (en > st && (s_text[en - 1] == ' ' || s_text[en - 1] == '\t' ||
                       s_text[en - 1] == '\n' || s_text[en - 1] == '\r')) --en;
    // 非空保证：裁剪后若塌缩为零，整体纳入锚点所在码点（先把可能的续字节对齐到首字节）。
    if (st == en) {
        size_t a = anchor;
        while (a > 0 && ((unsigned char)s_text[a] & 0xC0) == 0x80) --a;
        size_t aa; (void)aic_utf8_decode(a, &aa);
        st = a; en = a + (aa ? aa : 1);
        if (en > s_text_len) en = s_text_len;
    }
    s_sel_start = st; s_sel_end = en;
    free(s_sel_text);
    size_t len = en - st;
    s_sel_text = heap_caps_malloc(len + 1, AIC_PSRAM_CAPS);
    if (s_sel_text) { memcpy(s_sel_text, s_text + st, len); s_sel_text[len] = 0; }
}

static void reader_select_arm(app_ctx_t *ctx) {
    s_select_armed = true;
    s_reader_panel = READER_PANEL_NONE;
    rc_notice(ctx, "请点选要处理的文字");
}

/* 直接在指定坐标进入文字选择（长按入口）：命中词/字并打开选择卡。 */
static bool reader_select_begin_at(app_ctx_t *ctx, int x, int y) {
    EpdRect body = body_rect();
    size_t off = book_layout_point_to_offset(s_page, body, s_px, x, y);
    if (off > s_text_len) off = s_text_len;
    aic_compute_range(off, 0);
    s_select_armed = false;
    s_reader_panel = READER_PANEL_SELECT;
    (void)ctx;
    return true;
}

/* 点选命中：在 rc_body_tap 最前面调用，返回是否已接管。 */
static bool reader_select_handle_tap(app_ctx_t *ctx, int x, int y) {
    if (!s_select_armed) return false;
    s_select_armed = false;
    return reader_select_begin_at(ctx, x, y);
}

/* 选择卡“高亮”入口：把当前选区按章节内偏移落盘，重复位置幂等。 */
static bool reader_highlight_add(app_ctx_t *ctx) {
    (void)ctx;
    if (!s_sel_text || !s_sel_text[0]) return false;
    reader_highlights_t *hl = highlight_alloc();
    if (!hl) return false;
    bool have = highlight_load(hl);
    if (!have) memset(hl, 0, sizeof(*hl));
    bool dup = false;
    for (size_t i = 0; i < hl->count; ++i)
        if (hl->entries[i].chapter == s_chapter &&
            hl->entries[i].start_off == (uint32_t)s_sel_start) { dup = true; break; }
    bool ok = dup;
    if (!dup && hl->count < HIGHLIGHT_MAX) {
        reader_highlight_entry_t *e = &hl->entries[hl->count];
        memset(e, 0, sizeof(*e));
        e->chapter = (uint16_t)s_chapter;
        e->start_off = (uint32_t)s_sel_start;
        e->end_off = (uint32_t)s_sel_end;
        size_t tl = strlen(s_sel_text);
        if (tl >= HIGHLIGHT_TEXT_MAX) tl = HIGHLIGHT_TEXT_MAX - 1;
        memcpy(e->text, s_sel_text, tl);
        e->text[tl] = 0;
        hl->magic = HIGHLIGHT_MAGIC;
        hl->file_size = s_file_size;
        hl->count++;
        ok = highlight_save(hl);
    }
    free(hl);
    return ok;
}

/* 选择卡几何：预览框与标题/关闭按钮留出安全间距，按钮分两行、互不拥挤。 */
static EpdRect sel_preview_rect(void) {
    EpdRect c = notes_card_rect();
    return (EpdRect){c.x + 24, c.y + 66, c.width - 48, 220};
}
static EpdRect sel_range_rect(int i) {
    EpdRect c = notes_card_rect();
    const int gap = 11;
    int bw = (c.width - 48 - 2 * gap) / 3;
    return (EpdRect){c.x + 24 + i * (bw + gap), c.y + 306, bw, 54};
}
static EpdRect sel_action_rect(int i) {
    EpdRect c = notes_card_rect();
    const int gap = 11;
    int bw = (c.width - 48 - 3 * gap) / 4;
    return (EpdRect){c.x + 24 + i * (bw + gap), c.y + 382, bw, 54};
}

static void select_card_draw(uint8_t *fb) {
    EpdRect card = notes_card_rect();
    ui_fill_round_rect(fb, card, 16, UI_GRAY_WHITE);
    ui_draw_control_frame(fb, card, 16, 0x60);
    ui_text(fb, card.x + 24, card.y + 12, 24, "选择文字", EPD_DRAW_ALIGN_LEFT, false);
    EpdRect close = {card.x + card.width - 112, card.y + 10, 88, 44};
    ui_draw_button(fb, close, "关闭", false);

    EpdRect prev = sel_preview_rect();
    ui_fill_round_rect(fb, prev, 10, 0xf0);
    ui_draw_control_frame(fb, prev, 10, 0x98);
    if (s_sel_text && s_sel_text[0]) {
        uint16_t *st = NULL;
        size_t lines = ai_wrap_lines(s_sel_text, prev.width - 24, &st);
        const int asc = ttf_ascender_px(NOTES_CARD_PX);
        int y = prev.y + 10;
        for (size_t k = 0; k < lines && y + NOTES_PITCH <= prev.y + prev.height; ++k) {
            aic_draw_wrapped_line(fb, s_sel_text, st, lines, k,
                                  prev.x + 10, y + asc, NOTES_CARD_PX,
                                  prev.x + prev.width - 10);
            y += NOTES_PITCH;
        }
        free(st);
    }

    static const char *const ranges[] = {"词语", "整句", "整段"};
    for (int i = 0; i < 3; ++i) ui_draw_button(fb, sel_range_rect(i), ranges[i], false);

    static const char *const acts[] = {"AI解释", "AI翻译", "复制", "高亮"};
    for (int i = 0; i < 4; ++i) ui_draw_button(fb, sel_action_rect(i), acts[i], false);
}

/* 选择卡只接 AI解释 / AI翻译 两种结果卡。 */
static void select_open_aicard(aic_mode_t mode) {
    const char *t = s_sel_text ? s_sel_text : "";
    char sys[240], user[1200];
    const char *title;
    if (mode == AIC_TRANSLATE) {
        snprintf(sys, sizeof(sys), "你是翻译，中英互译，只给译文，不要解释。");
        snprintf(user, sizeof(user), "请翻译：%s", t);
        title = "AI 翻译";
    } else {
        snprintf(sys, sizeof(sys), "你是阅读助手，结合上下文通俗解释，简洁准确。%s",
                 aic_mode_suffix());
        snprintf(user, sizeof(user), "请通俗解释这段文字的含义与作用：%s", t);
        title = "AI 解释";
    }
    aicard_open(mode, title, s_sel_text, sys, user);
}

static app_redraw_t select_card_action(app_ctx_t *ctx, int x, int y) {
    EpdRect card = notes_card_rect();
    /* 点击卡片外任意位置：关闭选择卡。 */
    if (!ui_rect_hit(card, x, y)) { s_reader_panel = READER_PANEL_NONE; return APP_REDRAW_PAGE; }
    EpdRect close = {card.x + card.width - 112, card.y + 10, 88, 44};
    if (ui_rect_hit(close, x, y)) { s_reader_panel = READER_PANEL_NONE; return APP_REDRAW_PAGE; }
    for (int i = 0; i < 3; ++i)
        if (ui_rect_hit(sel_range_rect(i), x, y)) {
            size_t anchor = s_sel_start < s_text_len ? s_sel_start : 0;
            aic_compute_range(anchor, i);
            return APP_REDRAW_PAGE;
        }
    if (ui_rect_hit(sel_action_rect(0), x, y)) {
        select_open_aicard(AIC_EXPLAIN);
        return APP_REDRAW_PAGE;
    }
    if (ui_rect_hit(sel_action_rect(1), x, y)) {
        select_open_aicard(AIC_TRANSLATE);
        return APP_REDRAW_PAGE;
    }
    if (ui_rect_hit(sel_action_rect(2), x, y)) {
        const char *p = WR_DIR "/clipboard.txt";
        FILE *f = fopen(p, "wb");
        if (f) { (void)fwrite(s_sel_text ? s_sel_text : "", 1,
                        strlen(s_sel_text ? s_sel_text : ""), f); fclose(f);
                 rc_notice(ctx, "已复制到剪贴板文件"); }
        return APP_REDRAW_PAGE;
    }
    if (ui_rect_hit(sel_action_rect(3), x, y)) {
        /* 高亮：记录位置并本地持久化（#141 接线）。 */
        bool ok = reader_highlight_add(ctx);
        rc_notice(ctx, ok ? "已添加高亮" : "高亮失败");
        return APP_REDRAW_PAGE;
    }
    return APP_REDRAW_NONE;
}

/* ================================================================
 * 段评面板（READER_PANEL_CRV）——按“某一句话”聚合，不是整章
 * 流程：工具栏「段评」→ 点选句子 → 显示该句的全部公开段评
 *       → 左上角「AI归纳」只总结这一句的所有想法
 * ================================================================ */
static bool s_crv_armed;            /* 等待点选句子 */
static bool s_crv_fetch_pending;
static char *s_crv_sentence;        /* PSRAM，该句原文 */
static char s_crv_active_range[WRN_RANGE]; /* 本次面板锚定的服务端 range；空串=走模糊兜底 */

/* 段评面板专用紧凑卡片：比通用 notes 卡更小、居中，外围留空，点外部即可关闭。 */
static EpdRect crv_card_rect(void) {
    EpdRect b = body_rect();
    return (EpdRect){b.x + 40, b.y + 60, b.width - 80, b.height - 120};
}

static EpdRect crv_star_rect(void) {
    EpdRect c = crv_card_rect();
    return (EpdRect){c.x + 20, c.y + 12, 124, 48};
}

static EpdRect crv_close_rect(void) {
    EpdRect c = crv_card_rect();
    return (EpdRect){c.x + c.width - 112, c.y + 12, 92, 48};
}

/* 句框高度按实际折行动态计算（最多 4 行），从根上避免原文超出固定框。 */
#define CRV_SENT_MAX_LINES 4
static EpdRect crv_sentence_rect(void) {
    EpdRect c = crv_card_rect();
    int w = c.width - 48;
    size_t lines = 0;
    if (s_crv_sentence && s_crv_sentence[0]) {
        uint16_t *st = NULL;
        lines = ai_wrap_lines(s_crv_sentence, w - 20, &st);
        free(st);
    }
    if (lines > CRV_SENT_MAX_LINES) lines = CRV_SENT_MAX_LINES;
    if (lines < 1) lines = 1;
    return (EpdRect){c.x + 24, c.y + 70, w, (int)lines * NOTES_PITCH + 20};
}

/* 列表区：句框下方到分页按钮上方。 */
static EpdRect crv_list_rect(void) {
    EpdRect c = crv_card_rect();
    EpdRect s = crv_sentence_rect();
    int top = s.y + s.height + 14;
    int bottom = c.y + c.height - 66;
    return (EpdRect){c.x + 24, top, c.width - 48, bottom - top};
}

static EpdRect crv_prev_rect(void) {
    EpdRect c = crv_card_rect();
    return (EpdRect){c.x + 24, c.y + c.height - 56, 104, 44};
}

static EpdRect crv_next_rect(void) {
    EpdRect c = crv_card_rect();
    return (EpdRect){c.x + c.width - 128, c.y + c.height - 56, 104, 44};
}

/* ================================================================
 * 想法单列流式布局（每条加框；正文最多两行，超出才省略并点开详情）
 * ================================================================ */
static int s_crv_detail_index = -1;   /* 详情页正在展示的“匹配序号”，-1=未打开 */
static bool s_crv_card_pending;       /* 列表↔详情切换：整卡局部刷新，不整屏闪。 */
/* s_crv_detail_scroll / s_crv_turn_pending / s_crv_detail_pending 在 .h 里提前声明，
   供 controls_runtime.inc 的手势处理在本 .inc 之前使用。 */

#define CRV_GAP          12   /* 相邻想法框之间的垂直间距 */
#define CRV_MAX_LINES    2    /* 列表里每条想法最多显示的正文行数 */
#define CRV_AUTHOR_PX    22   /* 作者·赞行字号 */
#define CRV_AUTHOR_PITCH 30   /* 作者行占用高度 */
#define CRV_PAD          8    /* 想法框上下内边距 */
#define CRV_FLOW_CAP     40   /* 单页流式数组容量（实际一页远小于此，仅作栈上安全上限） */

/* 该想法正文实际折行数（用于判断是否超过两行、需要点开详情）。 */
static size_t crv_item_total_lines(size_t mi) {
    const wrn_chapter_review_t *r = crv_matched_at(mi);
    if (!r || !r->content[0]) return 0;
    uint16_t *st = NULL;
    size_t lines = ai_wrap_lines(r->content, crv_list_rect().width - 20, &st);
    free(st);
    return lines;
}

/* 列表里实际展示的正文行数（1..2）。 */
static int crv_item_shown_lines(size_t mi) {
    size_t lines = crv_item_total_lines(mi);
    if (lines < 1) lines = 1;
    if (lines > CRV_MAX_LINES) lines = CRV_MAX_LINES;
    return (int)lines;
}

/* 该想法框的高度：作者行 + 1~2 行正文 + 上下内边距。 */
static int crv_item_height(size_t mi) {
    return CRV_PAD + CRV_AUTHOR_PITCH + crv_item_shown_lines(mi) * NOTES_PITCH + CRV_PAD;
}

/* 从 from 开始流式排列，把能放进列表区的条目矩形写进 rects/idxs，返回本页条数；
   next_from 为下一页起点，cap 为调用方数组容量。 */
static int crv_flow(int from, int *next_from, EpdRect *rects, size_t *idxs, int cap) {
    EpdRect area = crv_list_rect();
    size_t M = crv_matching_count();
    int y = area.y;
    int n = 0;
    size_t i = (size_t)from;
    while (i < M && n < cap) {
        int h = crv_item_height(i);
        /* 本页已有内容且下一条放不下就翻页；第一条即使偏高也强制放（正文有裁剪不会越界）。 */
        if (n > 0 && y + h > area.y + area.height) break;
        rects[n] = (EpdRect){area.x, y, area.width, h};
        idxs[n] = i;
        ++n;
        y += h + CRV_GAP;
        ++i;
    }
    *next_from = (int)i;
    return n;
}

/* 总页数：从头流式走一遍统计。 */
static int crv_page_count(void) {
    size_t M = crv_matching_count();
    if (!M) return 1;
    EpdRect rs[CRV_FLOW_CAP];
    size_t ix[CRV_FLOW_CAP];
    int pages = 0, from = 0;
    while ((size_t)from < M) {
        int next = from;
        int n = crv_flow(from, &next, rs, ix, CRV_FLOW_CAP);
        if (n < 1 || next <= from) break;
        from = next;
        ++pages;
    }
    return pages < 1 ? 1 : pages;
}

/* 当前页起点条目序号：按页流式重放。 */
static int crv_page_start(int page) {
    EpdRect rs[CRV_FLOW_CAP];
    size_t ix[CRV_FLOW_CAP];
    int from = 0;
    for (int p = 0; p < page; ++p) {
        int next = from;
        if (crv_flow(from, &next, rs, ix, CRV_FLOW_CAP) < 1 || next <= from) break;
        from = next;
    }
    return from;
}

/* 把 src 截断到 max_w 以内并以“…”结尾；本来就放得下则原样返回。
   按 UTF-8 字符边界回退，不会切坏多字节汉字。out 需至少 strlen(src)+1。 */
static bool crv_ellipsize(int px, const char *src, int max_w, char *out, size_t outcap) {
    size_t n = strlen(src);
    if (n + 1 > outcap) n = outcap - 1;
    memcpy(out, src, n); out[n] = 0;
    if (ttf_text_width_px(px, out) <= max_w) return false;
    int ell_w = ttf_text_width_px(px, "…");
    while (n > 0) {
        /* 回退一个完整 UTF-8 字符。 */
        size_t back = 1;
        while (back < n && ((unsigned char)out[n - back] & 0xc0) == 0x80) ++back;
        n -= back;
        out[n] = 0;
        if (ttf_text_width_px(px, out) + ell_w <= max_w) {
            if (n + 4 <= outcap) { memcpy(out + n, "…", 3); out[n + 3] = 0; }
            return true;
        }
    }
    /* 极端情况连一个字加省略号都放不下：只放省略号。 */
    snprintf(out, outcap, "…");
    return true;
}

/* 工具栏入口：先进入“点选句子”状态。 */
static void reader_crv_open(app_ctx_t *ctx) {
    s_crv_armed = true;
    s_reader_panel = READER_PANEL_NONE;
    rc_notice(ctx, "请点选要看段评的句子");
}

/* 点选句子：在 rc_body_tap 最前面调用，返回是否接管。 */
static bool reader_crv_handle_tap(app_ctx_t *ctx, int x, int y) {
    if (!s_crv_armed || !reading_tap_settled(ctx)) return false;
    s_crv_armed = false;
    return reader_crv_open_at_point(x, y, -1);
}

/* 点中原文下的段评虚线（无需先 armed）：直接打开该句段评面板。 */
static bool reader_review_handle_tap(app_ctx_t *ctx, int x, int y) {
    if (!reading_tap_settled(ctx)) return false;
    int ordinal = book_layout_page_review_hit(s_page, x, y);
    if (ordinal < 0) return false;
    return reader_crv_open_at_point(x, y, ordinal);
}

/* 下面两个判定在 crv_review_matches 定义前就要用，先做静态前置声明。 */
static bool crv_has_loaded_matching(void);
static const char *crv_matching_hotspot_range(void);

/* 按触摸点求整句范围并打开段评面板，armed 与虚线两条入口共用。
 * ordinal>=0：点中虚线，直接锚定该条 range（精确）；ordinal=-1：armed 任意点选，稍后模糊兜底。 */
static bool reader_crv_open_at_point(int x, int y, int ordinal) {
    EpdRect body = body_rect();
    size_t off = book_layout_point_to_offset(s_page, body, s_px, x, y);
    if (off > s_text_len) off = s_text_len;
    aic_compute_range(off, 1); /* 复用选区逻辑，按整句求范围 */
    free(s_crv_sentence);
    s_crv_sentence = aic_psram_dup(s_sel_text);
    s_crv_page = 0;
    s_crv_detail_index = -1;
    s_crv_turn_pending = false;
    s_crv_active_range[0] = 0;
    if (ordinal >= 0) {
        const char *r = wrn_anchor_range_at((size_t)ordinal);
        if (r) snprintf(s_crv_active_range, sizeof(s_crv_active_range), "%s", r);
    }
    /* 这一句还没有已持久化段评（content 为空）时，仅在线情况下一 tick 单 range 拉取；
       离线不发起网络请求，直接由面板显示离线兜底，避免空等超时与“正在拉取”闪烁。 */
    s_crv_fetch_pending = (s_reader_note_chapter[0] && !crv_has_loaded_matching() && wr_online());
    s_reader_panel = READER_PANEL_CRV;
    return true;
}

/* range 是服务端内部编码，不能拿来比字节偏移；热点靠服务端回的原句 abstract 认句子。
 * Hotspot ranges are wire-internal, so a hotspot is matched by the sentence it carries. */
static bool crv_review_matches(const wrn_chapter_review_t *r) {
    if (!r->abstract[0] || !s_crv_sentence || !s_crv_sentence[0]) return false;
    if (strstr(s_crv_sentence, r->abstract)) return true;
    if (strstr(r->abstract, s_crv_sentence)) return true;
    /* 原句被 WRN_ABSTRACT 截断时长句包含不上：退一步比开头一小段（跨行点选更常见）。 */
    char head[25];
    size_t n = strlen(r->abstract);
    if (n > sizeof(head) - 1) n = sizeof(head) - 1;
    memcpy(head, r->abstract, n);
    head[n] = 0;
    return n >= 8 && strstr(s_crv_sentence, head) != NULL;
}

/* 面板归属：已锚定 range 时精确比 range（多人段评全部纳入）；armed 模糊路径退回 abstract 匹配。 */
static bool crv_belongs_to_active(const wrn_chapter_review_t *r) {
    if (s_crv_active_range[0])
        return r->range[0] && strcmp(r->range, s_crv_active_range) == 0;
    return crv_review_matches(r);
}

/* 这一句是否已经拉到真正的段评（热点占位 content 为空，不算）。 */
static bool crv_has_loaded_matching(void) {
    size_t total = wrn_crv_count_chapter(s_reader_note_chapter);
    for (size_t i = 0; i < total; ++i) {
        const wrn_chapter_review_t *r = wrn_crv_at_chapter(s_reader_note_chapter, i);
        if (r && r->content[0] && crv_belongs_to_active(r)) return true;
    }
    return false;
}

/* 取这一句对应热点的 range 字符串（单 range 拉取时原样带回），没有则返回 NULL。 */
static const char *crv_matching_hotspot_range(void) {
    if (s_crv_active_range[0]) return s_crv_active_range;
    size_t total = wrn_crv_count_chapter(s_reader_note_chapter);
    for (size_t i = 0; i < total; ++i) {
        const wrn_chapter_review_t *r = wrn_crv_at_chapter(s_reader_note_chapter, i);
        if (r && crv_review_matches(r)) return r->range;
    }
    return NULL;
}

/* 标题计数：只数真正拉到内容的段评，不数热点占位。 */
static size_t crv_matching_count(void) {
    size_t total = wrn_crv_count_chapter(s_reader_note_chapter);
    size_t n = 0;
    for (size_t i = 0; i < total; ++i) {
        const wrn_chapter_review_t *r = wrn_crv_at_chapter(s_reader_note_chapter, i);
        if (r && r->content[0] && crv_belongs_to_active(r)) ++n;
    }
    return n;
}

/* 按"匹配序号"取第 mi 条已归属段评（跳过占位与非归属）。 */
static const wrn_chapter_review_t *crv_matched_at(size_t mi) {
    size_t total = wrn_crv_count_chapter(s_reader_note_chapter);
    size_t seen = 0;
    for (size_t i = 0; i < total; ++i) {
        const wrn_chapter_review_t *r = wrn_crv_at_chapter(s_reader_note_chapter, i);
        if (!r || !r->content[0] || !crv_belongs_to_active(r)) continue;
        if (seen == mi) return r;
        ++seen;
    }
    return NULL;
}

/* 点虚线后延迟拉取：只对这一个 range 在线要完整段评，不整章批量。 */
static app_redraw_t crv_panel_run_tick(app_ctx_t *ctx) {
    (void)ctx;
    if (!s_crv_fetch_pending) return APP_REDRAW_NONE;
    s_crv_fetch_pending = false;
    const char *range = crv_matching_hotspot_range();
    if (range)
        (void)wrn_fetch_range_reviews(s_reader_note_chapter, range);
    return APP_REDRAW_PAGE;
}

/* 归纳提示：纳入的想法条数与单条长度都做上限，保持请求体小、生成快，弱网不易超时。
   （完整想法仍全量持久化在 sidecar，这里只影响发给模型的归纳上下文。） */
/* 弱网上传是归纳超时主因：24 条足以支撑 2–4 点概括，同时显著减小请求体。 */
#define CRV_DIGEST_MAX_ITEMS 24
#define CRV_DIGEST_ITEM_CHARS 120
/* 归纳请求的 token 上限：答案简短（2-4 点、≤100 字），同时缩短生成、减少超时。 */
#define AIC_DIGEST_MAX_TOKENS 320

static void crv_request_digest(app_ctx_t *ctx) {
    size_t cap = 8192;
    char *user = heap_caps_malloc(cap, AIC_PSRAM_CAPS);
    if (!user) return;
    int used = snprintf(user, cap,
        "以下是微信读书读者对同一句话“%s”发表的想法，请用2-4个简短要点概括主流看法，每点不超过20字、总共不超过100字，直接给结论，不要客套、不要展开、不要总结整章：\n",
        s_crv_sentence ? s_crv_sentence : "");
    size_t total = wrn_crv_count_chapter(s_reader_note_chapter);
    size_t shown = 0;
    char item[CRV_DIGEST_ITEM_CHARS + 1];
    for (size_t i = 0; i < total && shown < CRV_DIGEST_MAX_ITEMS; ++i) {
        const wrn_chapter_review_t *r = wrn_crv_at_chapter(s_reader_note_chapter, i);
        if (!r || !r->content[0] || !crv_belongs_to_active(r)) continue;
        snprintf(item, sizeof(item), "%s", r->content);
        ++shown;
        int add = snprintf(user + used, cap - (size_t)used,
            "\n%u. %s（%s，%u赞）", (unsigned)shown, item,
            r->author[0] ? r->author : "匿名", (unsigned)r->likes);
        if (add < 0 || (size_t)add >= cap - (size_t)used) break;
        used += add;
    }
    if (!shown) {
        free(user);
        rc_notice(ctx, wr_online() ? "还没拉到可归纳的段评，请稍后再试" : "离线无法归纳段评，请先联网查看");
        return;
    }
    const char *sys = "你是阅读助手，简洁归纳读者对同一句话的想法，只给2-4个短要点，不要展开、不要总结整章。";
    aicard_open(AIC_DIGEST, "段评想法归纳", s_crv_sentence, sys, user);
    /* 归纳用小 token 上限，缩短生成时间、降低弱网超时失败，且强制答案简短。 */
    s_aic_max_tokens = AIC_DIGEST_MAX_TOKENS;
    free(user);
}

static void crv_panel_draw(uint8_t *fb) {
    EpdRect card = crv_card_rect();
    ui_fill_round_rect(fb, card, 16, UI_GRAY_WHITE);
    ui_draw_control_frame(fb, card, 16, 0x60);
    ui_draw_button(fb, crv_star_rect(), "AI归纳", false);
    ui_draw_button(fb, crv_close_rect(), "关闭", false);
    char title[32];
    snprintf(title, sizeof(title), "段评·共%u条", (unsigned)crv_matching_count());
    ui_text(fb, card.x + 150, card.y + 14, 24, title, EPD_DRAW_ALIGN_LEFT, false);

    /* 该句原文：句框高度自适应折行，最多 4 行，文字只在框内绘制，不溢出。 */
    EpdRect sbox = crv_sentence_rect();
    ui_fill_round_rect(fb, sbox, 10, 0xf0);
    ui_draw_control_frame(fb, sbox, 10, 0x98);
    if (s_crv_sentence && s_crv_sentence[0]) {
        uint16_t *st = NULL;
        size_t lines = ai_wrap_lines(s_crv_sentence, sbox.width - 24, &st);
        const int asc = ttf_ascender_px(NOTES_CARD_PX);
        int y = sbox.y + 10;
        for (size_t k = 0; k < lines && y + NOTES_PITCH <= sbox.y + sbox.height; ++k) {
            aic_draw_wrapped_line(fb, s_crv_sentence, st, lines, k,
                                  sbox.x + 10, y + asc, NOTES_CARD_PX,
                                  sbox.x + sbox.width - 10);
            y += NOTES_PITCH;
        }
        free(st);
    }

    EpdRect area = crv_list_rect();
    if (s_crv_fetch_pending) {
        ui_text(fb, area.x, area.y, NOTES_CARD_PX, "正在拉取段评…",
                EPD_DRAW_ALIGN_LEFT, false);
        return;
    }

    size_t M = crv_matching_count();
    if (!M) {
        ui_text(fb, area.x, area.y, NOTES_CARD_PX,
                wr_online() ? "这句话暂无公开段评" : "离线状态无法查看段评",
                EPD_DRAW_ALIGN_LEFT, false);
        return;
    }

    /* 单列流式分页：每条想法一个全宽框，正文最多两行、超出才省略并点开详情。 */
    int npages = crv_page_count();
    if (s_crv_page >= npages) s_crv_page = npages - 1;
    if (s_crv_page < 0) s_crv_page = 0;

    EpdRect boxes[CRV_FLOW_CAP];
    size_t bix[CRV_FLOW_CAP];
    int next_from = 0;
    int shown = crv_flow(crv_page_start(s_crv_page), &next_from, boxes, bix, CRV_FLOW_CAP);
    const int asc = ttf_ascender_px(NOTES_CARD_PX);
    int inner_w = area.width - 20;
    for (int n = 0; n < shown; ++n) {
        const wrn_chapter_review_t *r = crv_matched_at(bix[n]);
        if (!r) continue;
        EpdRect box = boxes[n];
        ui_fill_round_rect(fb, box, 12, 0xf4);
        ui_draw_control_frame(fb, box, 12, 0x98);

        /* 作者 · 赞（小字单行）。 */
        char head[128];
        snprintf(head, sizeof(head), "%s · %u赞",
                 r->author[0] ? r->author : "匿名", (unsigned)r->likes);
        ui_text(fb, box.x + 10, box.y + CRV_PAD, CRV_AUTHOR_PX, head,
                EPD_DRAW_ALIGN_LEFT, false);

        /* 正文：最多两行，精确区间绘制；第二行且还有更多内容时截断加省略号。 */
        uint16_t *st = NULL;
        size_t lines = ai_wrap_lines(r->content, inner_w, &st);
        int draw_lines = crv_item_shown_lines(bix[n]);
        int content_top = box.y + CRV_PAD + CRV_AUTHOR_PITCH;
        char linebuf[320], ell[320];
        for (int k = 0; k < draw_lines; ++k) {
            int s = (int)st[k];
            int e = (k + 1 < (int)lines) ? (int)st[k + 1] : (int)strlen(r->content);
            const char *seg = r->content;
            int ds = s, de = e;
            if (k == CRV_MAX_LINES - 1 && lines > CRV_MAX_LINES) {
                size_t seg_len = (size_t)(e - s);
                if (seg_len >= sizeof(linebuf)) seg_len = sizeof(linebuf) - 1;
                memcpy(linebuf, r->content + s, seg_len);
                linebuf[seg_len] = 0;
                crv_ellipsize(NOTES_CARD_PX, linebuf, inner_w, ell, sizeof(ell));
                seg = ell; ds = 0; de = (int)strlen(ell);
            }
            ttf_draw_text_px_segment(fb, box.x + 10, content_top + k * NOTES_PITCH + asc,
                                     NOTES_CARD_PX, seg, ds, de, 0, 15,
                                     box.x + box.width - 10);
        }
        free(st);
    }

    /* 分页按钮 + 页码。 */
    ui_draw_button(fb, crv_prev_rect(), "上一页", false);
    ui_draw_button(fb, crv_next_rect(), "下一页", false);
    char pg[32];
    snprintf(pg, sizeof(pg), "第%d/%d页", s_crv_page + 1, npages);
    ui_text(fb, card.x + card.width / 2, card.y + card.height - 50,
            NOTES_CARD_PX - 4, pg, EPD_DRAW_ALIGN_CENTER, false);
}

static app_redraw_t crv_panel_action(app_ctx_t *ctx, int x, int y) {
    EpdRect card = crv_card_rect();
    /* 点卡片以外区域：直接关闭面板。 */
    if (!ui_rect_hit(card, x, y)) {
        s_reader_panel = READER_PANEL_NONE;
        return APP_REDRAW_PAGE;
    }
    if (ui_rect_hit(crv_close_rect(), x, y)) {
        s_reader_panel = READER_PANEL_NONE;
        return APP_REDRAW_PAGE;
    }
    int npages = crv_page_count();

    /* 翻页：只局部刷新卡片区域，标记 pending 由 present 出口做短刷，不整屏闪。 */
    if (ui_rect_hit(crv_prev_rect(), x, y)) {
        if (s_crv_page > 0) { --s_crv_page; s_crv_turn_pending = true; }
        return APP_REDRAW_AREA;
    }
    if (ui_rect_hit(crv_next_rect(), x, y)) {
        if (s_crv_page + 1 < npages) { ++s_crv_page; s_crv_turn_pending = true; }
        return APP_REDRAW_AREA;
    }
    if (ui_rect_hit(crv_star_rect(), x, y)) {
        crv_request_digest(ctx);
        return APP_REDRAW_PAGE;
    }

    /* 点想法框：只有正文超过两行（被省略）的才进详情看全文；短想法不做跳转。 */
    EpdRect boxes[CRV_FLOW_CAP];
    size_t bix[CRV_FLOW_CAP];
    int next_from = 0;
    int shown = crv_flow(crv_page_start(s_crv_page), &next_from, boxes, bix, CRV_FLOW_CAP);
    for (int n = 0; n < shown; ++n) {
        if (ui_rect_hit(boxes[n], x, y)) {
            if (crv_item_total_lines(bix[n]) > CRV_MAX_LINES) {
                s_crv_detail_index = (int)bix[n];
                s_crv_detail_scroll = 0;
                s_crv_detail_pending = false;
                s_crv_card_pending = true;
                s_reader_panel = READER_PANEL_CRV_DETAIL;
                return APP_REDRAW_AREA;
            }
            return APP_REDRAW_NONE;
        }
    }
    return APP_REDRAW_NONE;
}

/* ================================================================
 * 想法详情页（READER_PANEL_CRV_DETAIL）：显示该想法全部文字
 * ================================================================ */
static EpdRect crv_detail_back_rect(void) {
    EpdRect c = crv_card_rect();
    return (EpdRect){c.x + 20, c.y + 12, 124, 48};
}

static void crv_detail_draw(uint8_t *fb) {
    EpdRect card = crv_card_rect();
    ui_fill_round_rect(fb, card, 16, UI_GRAY_WHITE);
    ui_draw_control_frame(fb, card, 16, 0x60);
    ui_draw_button(fb, crv_detail_back_rect(), "返回", false);
    ui_text(fb, card.x + 150, card.y + 14, 24, "想法详情",
            EPD_DRAW_ALIGN_LEFT, false);

    const wrn_chapter_review_t *r =
        (s_crv_detail_index >= 0) ? crv_matched_at((size_t)s_crv_detail_index) : NULL;
    if (!r) return;

    /* 作者 + 点赞数。 */
    char head[160];
    snprintf(head, sizeof(head), "%s · %u赞",
             r->author[0] ? r->author : "匿名", (unsigned)r->likes);
    ui_text(fb, card.x + 24, card.y + 78, NOTES_CARD_PX, head,
            EPD_DRAW_ALIGN_LEFT, false);

    /* 全文：在正文区域内逐行折行，长想法可上下滚动看全，不丢内容。 */
    int text_top = card.y + 78 + NOTES_PITCH + 16;
    int text_bottom = card.y + card.height - 28;
    int text_w = card.width - 48;
    uint16_t *st = NULL;
    size_t lines = ai_wrap_lines(r->content, text_w, &st);
    int visible = (text_bottom - text_top) / NOTES_PITCH;
    if (visible < 1) visible = 1;
    if (s_crv_detail_scroll < 0) s_crv_detail_scroll = 0;
    if ((size_t)visible < lines && s_crv_detail_scroll > (int)lines - visible)
        s_crv_detail_scroll = (int)lines - visible;
    if ((size_t)visible >= lines) s_crv_detail_scroll = 0;
    const int asc = ttf_ascender_px(NOTES_CARD_PX);
    int y = text_top;
    for (int k = s_crv_detail_scroll;
         k < (int)lines && y + NOTES_PITCH <= text_bottom; ++k) {
        aic_draw_wrapped_line(fb, r->content, st, lines, (size_t)k,
                              card.x + 24, y + asc, NOTES_CARD_PX,
                              card.x + card.width - 24);
        y += NOTES_PITCH;
    }
    free(st);
}

/* 详情页正文区域（滚动局部刷新用）。 */
static EpdRect crv_detail_text_rect(void) {
    EpdRect card = crv_card_rect();
    int text_top = card.y + 78 + NOTES_PITCH + 16;
    return (EpdRect){card.x, text_top, card.width,
                     card.y + card.height - 28 - text_top};
}

/* 详情页滚动局部刷新：只短刷正文区，不整屏闪。 */
static bool crv_detail_scroll_present(app_ctx_t *ctx, app_redraw_t redraw) {
    if (!s_crv_detail_pending) return false;
    s_crv_detail_pending = false;
    if (redraw != APP_REDRAW_AREA) return false;
    EpdRect area = crv_detail_text_rect();
    lock_draw();
    crv_detail_draw(ctx->fb);
    unlock_draw();
    /* 段评详情用 37 帧 GL16 灰阶：灰阶完整驱动残影小，文字清晰。
       Review detail uses the 37-frame GL16 grayscale for low ghosting and crisp text. */
    enum EpdDrawError err = update_display_area_diff_with(
        ctx->hl, &E0470_WAVEFORM, MODE_GL16, area);
    guard_draw_result(ctx->hl, err);
    return true;
}

static app_redraw_t crv_detail_action(app_ctx_t *ctx, int x, int y) {
    (void)ctx;
    EpdRect card = crv_card_rect();
    /* 返回格子列表；点卡片外也返回列表（不直接关闭，避免误触丢失位置）。 */
    if (ui_rect_hit(crv_detail_back_rect(), x, y) || !ui_rect_hit(card, x, y)) {
        s_crv_detail_index = -1;
        s_crv_card_pending = true;
        s_reader_panel = READER_PANEL_CRV;
        return APP_REDRAW_AREA;
    }
    return APP_REDRAW_NONE;
}

/* 列表↔详情切换局部刷新：只重画并短刷整张卡片，不整屏重画，避免全屏闪。 */
static bool crv_card_present(app_ctx_t *ctx, app_redraw_t redraw) {
    if (!s_crv_card_pending) return false;
    s_crv_card_pending = false;
    if (redraw != APP_REDRAW_AREA) return false;
    EpdRect card = crv_card_rect();
    lock_draw();
    if (s_reader_panel == READER_PANEL_CRV_DETAIL)
        crv_detail_draw(ctx->fb);
    else
        crv_panel_draw(ctx->fb);
    unlock_draw();
    /* 列表↔详情切换走 37 帧 GL16 灰阶，残影小、不整屏闪。
       List/detail toggle uses the 37-frame GL16 grayscale for low ghosting. */
    enum EpdDrawError err = update_display_area_diff_with(
        ctx->hl, &E0470_WAVEFORM, MODE_GL16, card);
    guard_draw_result(ctx->hl, err);
    return true;
}

/* 段评翻页局部刷新：只重画并短刷卡片区域，不整屏重画，避免全屏闪。 */
static bool crv_turn_present(app_ctx_t *ctx, app_redraw_t redraw) {
    if (!s_crv_turn_pending) return false;
    s_crv_turn_pending = false;
    if (redraw != APP_REDRAW_AREA) return false;
    EpdRect list = crv_list_rect();
    EpdRect card = crv_card_rect();
    lock_draw();
    crv_panel_draw(ctx->fb);
    unlock_draw();
    /* 刷新范围：从格子区顶部到卡片底部（含页码），句框与标题不动。 */
    EpdRect area = {card.x, list.y, card.width, card.y + card.height - list.y};
    /* 段评翻页用 37 帧 GL16 灰阶，残影小、不整屏闪。
       Review page turn uses the 37-frame GL16 grayscale for low ghosting. */
    enum EpdDrawError err = update_display_area_diff_with(
        ctx->hl, &E0470_WAVEFORM, MODE_GL16, area);
    guard_draw_result(ctx->hl, err);
    return true;
}

static void reader_crv_cleanup(void) {
    free(s_crv_sentence);
    s_crv_sentence = NULL;
    s_crv_armed = false;
    s_crv_fetch_pending = false;
    s_crv_active_range[0] = 0;
    wrn_crv_clear();   /* 关书/换书：清空本章内存段评（磁盘锚点保留） */
}

/* ================================================================
 * 隔 3 天 AI 回顾（AIC_RECAP）
 * ================================================================ */
static bool s_recap_pending;

/* 开书后调用：判断时间间隔，决定是否需要回顾。 */
static void recap_check_on_open(void) {
    s_recap_pending = false;
    if (!s_path[0] || !s_text) return;
    uint32_t now = (uint32_t)time(NULL);
    if (now < 1577836800u) { /* 时间未同步，无法判断 */
        aic_stamp_save(s_path, now);
        return;
    }
    uint32_t last = aic_stamp_load(s_path);
    aic_stamp_save(s_path, now);
    if (last > 0 && now - last >= AIC_RECAP_GAP_S) s_recap_pending = true;
}

static app_redraw_t recap_run_tick(app_ctx_t *ctx) {
    if (!s_recap_pending) return APP_REDRAW_NONE;
    s_recap_pending = false;
    // 取本章已读部分（当前页之前），作为回顾上下文，封顶约 3000 字节。
    size_t cur = reader_page_offset(s_page);
    if (cur > s_text_len) cur = s_text_len;
    size_t from = cur > 3000 ? cur - 3000 : 0;
    size_t len = cur - from;
    char *context = heap_caps_malloc(len + 1, AIC_PSRAM_CAPS);
    if (!context) return APP_REDRAW_NONE;
    memcpy(context, s_text + from, len); context[len] = 0;

    char *user = heap_caps_malloc(len + 256, AIC_PSRAM_CAPS);
    if (user) {
        snprintf(user, len + 256,
                 "我隔了几天没读这本书，请帮我快速回顾下面已读内容的前情要点，分点简洁（不超过 200 字）：\n%s",
                 context);
        const char *sys = "你是阅读助手，帮读者快速回忆前文，分点、简洁、只给关键信息。";
        aicard_open(AIC_RECAP, "前情回顾", context, sys, user);
        free(user);
    }
    free(context);
    return APP_REDRAW_PAGE;
}
