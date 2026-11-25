/* SPDX-FileCopyrightText: 2026 YIYO-Designer */
/* SPDX-License-Identifier: LicenseRef-Kiikoread-Personal-Study-1.0 */
/*
 * 微信读书「公开段评」锚点模块（下载即锚定，ThingOS 式）。
 * WeRead public review anchor module: fetched and persisted at download time.
 *
 * 锚点文件随章节正文一起落盘，离线可读；与正文同生命周期（重下重建、drop/forget 连带删、
 * repair 刷新）。完整评论仍在点虚线时按需拉取，本文件只持久化画虚线所需的 range+abstract。
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "weread_notes.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WRA_MAX_ENTRIES WRN_CRV_MAX   /* 单章最多持久化的锚点条数。/ Max anchors per chapter. */

/// 单条锚点（固定尺寸，确定性落盘）：服务端 range、被评论原文 abstract，以及由云端 range
/// 确定性换算出的正文本地字节区间（local_start/end），渲染直接使用，abstract 匹配仅作兜底。
typedef struct {
    char range[WRN_RANGE];
    char abstract[WRN_ABSTRACT];
    uint32_t local_start;
    uint32_t local_end;
} wra_entry_t;

/*
 * 下载侧：同一 worker 串行抓 underlines -> readreviews(count=1) 换 abstract，原子写
 * r/c<index>.anchor。underlines 网络失败不写文件（返回 false，留给 repair/重下）；
 * underlines 成功但无有效 range 时写一个合法的空锚点（确认空，避免重复联网）。
 */
bool wra_fetch_and_store(const char *book_id, const char *chapter_uid, unsigned index);

/// 读回并校验锚点（magic/version/尺寸/CRC），返回 PSRAM 条目（调用方 free）。缺失或损坏返回 false。
bool wra_load(const char *book_id, unsigned index, wra_entry_t **out, uint32_t *count);

/// 锚点是否为当前版本（旧版本返回 false，触发 repair 重抓）。/ Current-version check.
bool wra_is_current(const char *book_id, unsigned index);

/// 删除单章锚点（忽略不存在）。/ Remove one chapter's anchor.
bool wra_drop(const char *book_id, unsigned index);

#ifdef __cplusplus
}
#endif
