/*
 * book_reader_ai_features.h
 * book_reader_ai_features.inc 的前置声明。
 * 该 .inc 在 app_book.c 末尾被包含，但其中的入口函数在正文更早就被调用，
 * 故在此提前声明，避免隐式声明与 static/non-static 冲突。
 * 本头文件仅被 app_book.c 包含一次，函数与变量均为文件内 static。
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "app.h"

/* 通用结果卡 */
static app_redraw_t aicard_run_tick(app_ctx_t *ctx);
static void aicard_draw(uint8_t *fb);
static app_redraw_t aicard_action(app_ctx_t *ctx, int x, int y);
static bool aic_page_turn(int dir);
static bool aic_page_present(app_ctx_t *ctx, app_redraw_t redraw);
static void aic_follow_draw(uint8_t *fb);
static app_redraw_t aic_follow_action(app_ctx_t *ctx, uint16_t x, uint16_t y);

/* 文字选择 */
static void reader_select_arm(app_ctx_t *ctx);
static bool reader_select_begin_at(app_ctx_t *ctx, int x, int y);
static bool reader_select_handle_tap(app_ctx_t *ctx, int x, int y);
static void select_card_draw(uint8_t *fb);
static app_redraw_t select_card_action(app_ctx_t *ctx, int x, int y);

/* 段评（按某一句话聚合） */
static void reader_crv_open(app_ctx_t *ctx);
static bool reader_crv_handle_tap(app_ctx_t *ctx, int x, int y);
static bool reader_review_handle_tap(app_ctx_t *ctx, int x, int y);
static bool reader_crv_open_at_point(int x, int y, int ordinal);
static app_redraw_t crv_panel_run_tick(app_ctx_t *ctx);
static void crv_panel_draw(uint8_t *fb);
static app_redraw_t crv_panel_action(app_ctx_t *ctx, int x, int y);
static size_t crv_matching_count(void);
static const wrn_chapter_review_t *crv_matched_at(size_t mi);
static int crv_page_count(void);
static void crv_detail_draw(uint8_t *fb);
static app_redraw_t crv_detail_action(app_ctx_t *ctx, int x, int y);
static bool crv_turn_present(app_ctx_t *ctx, app_redraw_t redraw);
static bool crv_card_present(app_ctx_t *ctx, app_redraw_t redraw);
static bool crv_detail_scroll_present(app_ctx_t *ctx, app_redraw_t redraw);
static void reader_crv_cleanup(void);

/* 隔 3 天回顾 */
static void recap_check_on_open(void);
static app_redraw_t recap_run_tick(app_ctx_t *ctx);

/* 分页位置（runtime inc 的手势处理要在 .inc 定义前读写，故提前做静态暂定声明）。 */
static int s_aic_page;            /* 结果卡答案当前页（0 基），翻页替代滚动。 */
static bool s_aic_page_pending;   /* 答案翻页只局部刷新答案区，不整屏闪。 */
static int s_crv_page;   /* 段评面板当前页（0 基），翻页替代滚动。 */
static int s_crv_detail_scroll;    /* 详情页正文滚动行数（长想法可看全）。 */
static bool s_crv_turn_pending;    /* 段评翻页只局部刷新卡片，不整屏闪。 */
static bool s_crv_detail_pending;  /* 详情页滚动只局部刷新正文区。 */
