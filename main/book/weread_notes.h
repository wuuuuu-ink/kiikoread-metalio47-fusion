/* SPDX-FileCopyrightText: 2026 YIYO-Designer */
/* SPDX-License-Identifier: LicenseRef-Kiikoread-Personal-Study-1.0 */
/*
 * 微信读书「划线 / 想法」模块（2026-11-23 融合固件，consent 登录）。
 * WeRead highlights (bookmarks) and thoughts (reviews) module for the fusion firmware.
 *
 * 公开段评/热门划线在下载时由 weread_anchor 走 consent 通道抓取并落盘，本模块负责渲染侧
 * 读回、点虚线按需拉取，以及 AI 卡「存为笔记」的本地私有笔记。早期网页扫码那套联网拉取、
 * 写同步队列已随 consent-only 登录移除。
 *
 * 本地落盘在 /sdcard/.weread/notes-<bookId>/，离线可读。
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WRN_ID        48
#define WRN_CHAPTER   48
#define WRN_RANGE     64
#define WRN_TEXT      600
#define WRN_ABSTRACT  400
#define WRN_AUTHOR    96
#define WRN_BOOKMARK_MAX  400
#define WRN_REVIEW_MAX    200
#define WRN_BEST_MAX      300
#define WRN_CRV_MAX       400   /* 其他读者公开段评（readreviews），PSRAM 存放，按点赞去重 */
#define WRN_QUEUE_MAX     64

/* 同步状态位。/ Sync flags. */
#define WRN_F_SYNCED    0x01  // 已与服务器一致
#define WRN_F_DIRTY     0x02  // 本地新增/修改，待上传
#define WRN_F_DELETED   0x04  // 本地已删，待通知服务器
#define WRN_F_PUBLIC    0x08  // 公开想法（否则私密）

/// 我的划线（bookmarklist.updated）。/ My highlight.
typedef struct {
    char bookmark_id[WRN_ID];
    char chapter_uid[WRN_CHAPTER];
    char range[WRN_RANGE];
    char mark_text[WRN_TEXT];
    char abstract[WRN_ABSTRACT];
    uint32_t style;
    uint32_t color_style;
    uint32_t create_time;
    uint32_t update_time;
    uint8_t flags;
} wrn_bookmark_t;

/// 我的想法（review/list mine=1）。/ My thought.
typedef struct {
    char review_id[WRN_ID];
    char chapter_uid[WRN_CHAPTER];
    char range[WRN_RANGE];
    char content[WRN_TEXT];
    char abstract[WRN_ABSTRACT];
    uint32_t create_time;
    uint8_t is_private;
    uint8_t flags;
} wrn_review_t;

/// 热门划线（bestbookmarks.items）。/ Popular highlight from other readers.
typedef struct {
    char chapter_uid[WRN_CHAPTER];
    char mark_text[WRN_TEXT];
    char author[WRN_AUTHOR];
    uint32_t heat;
} wrn_best_t;

/* 其他读者在某一章的公开段评（App 协议：underlines + readreviews）。 */
typedef struct {
    char review_id[WRN_ID];
    char chapter_uid[WRN_CHAPTER];
    char range[WRN_RANGE];
    char abstract[WRN_ABSTRACT];   /* 被评论的原文片段 */
    char content[WRN_TEXT];        /* 段评内容 */
    char author[WRN_AUTHOR];
    uint32_t likes;
    uint32_t create_time;
    uint32_t local_start;          /* 云端 range 确定性换算的正文本地字节区间 */
    uint32_t local_end;
} wrn_chapter_review_t;

/// 本地改动队列动作。/ Queued local mutation.
typedef enum {
    WRN_Q_ADD_BOOKMARK = 1,
    WRN_Q_UPDATE_BOOKMARK,
    WRN_Q_REMOVE_BOOKMARK,
    WRN_Q_ADD_REVIEW,
    WRN_Q_EDIT_REVIEW,
    WRN_Q_DELETE_REVIEW,
} wrn_queue_kind_t;

typedef struct {
    wrn_queue_kind_t kind;
    char id[WRN_ID];
    char chapter_uid[WRN_CHAPTER];
    char range[WRN_RANGE];
    char text[WRN_TEXT];
    char abstract[WRN_ABSTRACT];
    uint32_t style;
    uint8_t is_private;
} wrn_queue_item_t;

/* ---- 生命周期：阅读器/微读界面按 bookId 打开，关书时释放 ---- */

/// 从磁盘加载某本书的全部划线/想法；不存在则建立空集。返回 false 仅当分配失败。
bool wrn_open(const char *book_id);
void wrn_close(void);

/* ---- 公开段评锚点（下载即落盘，离线可读）---- */

/*
 * 公开段评（下载即锚定）：下载阶段由 weread_anchor 抓 underlines->readreviews 换 abstract
 * 并持久化到 stage 的 r/ 目录，渲染时用 wrn_load_anchor_chapter 读回，离线可见。
 * Public reviews are anchored at download time into the stage r/ directory and loaded
 * back for rendering, so they stay visible offline.
 */
/// 渲染侧：从磁盘锚点读回本章 abstract 列表填进内存（伪 id，content 留空，离线可用）。
void wrn_load_anchor_chapter(const char *book_id, const char *chapter_uid, unsigned index);
/// 换章/关书：清空本章内存段评（只服务当前章，不删磁盘锚点）。
void wrn_crv_clear(void);

bool wrn_fetch_range_reviews(const char *chapter_uid, const char *range); // 点虚线后单 range 拉完整段评

/* ---- 本地写入（UI 线程可用，只动内存与磁盘，不联网）---- */

bool wrn_add_review(const char *chapter_uid, const char *range, const char *content,
                    const char *abstract, bool is_private);

/* ---- 查询（阅读器/想法窗口使用）---- */

/// 某一章内我的划线数；逐条通过 wrn_bookmark_at_chapter 取。
size_t wrn_bookmark_count_chapter(const char *chapter_uid);
const wrn_bookmark_t *wrn_bookmark_at_chapter(const char *chapter_uid, size_t ordinal);
/// 某一章内我的想法数；逐条通过 wrn_review_at_chapter 取。
size_t wrn_review_count_chapter(const char *chapter_uid);
const wrn_review_t *wrn_review_at_chapter(const char *chapter_uid, size_t ordinal);
/// 某一章热门划线数。
size_t wrn_best_count_chapter(const char *chapter_uid);
const wrn_best_t *wrn_best_at_chapter(const char *chapter_uid, size_t ordinal);
/// 本章公开段评：总数与按序取条目（已按点赞数降序）。
size_t wrn_crv_count_chapter(const char *chapter_uid);
const wrn_chapter_review_t *wrn_crv_at_chapter(const char *chapter_uid, size_t ordinal);
/// 按锚点序号取该条划线的服务端 range（占位条目，离线可用）；越界返回 NULL。
const char *wrn_anchor_range_at(size_t ordinal);

#ifdef __cplusplus
}
#endif
