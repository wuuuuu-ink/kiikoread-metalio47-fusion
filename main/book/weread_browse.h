/* SPDX-FileCopyrightText: 2026 YIYO-Designer */
/* SPDX-License-Identifier: LicenseRef-Kiikoread-Personal-Study-1.0 */
/*
 * 微信读书「按 range 多人段评」离线持久化。
 * Per-range multi-person review persistence, fed by POST /book/readreviews.
 *
 * 下载阶段（weread_anchor）对每条 underline range 调 readreviews（count=50），把该 range
 * 返回的全部公开想法（review_id/range/author/content/likes/create_time）用紧凑变长格式
 * 持久化到 stage 的 r/c<index>.reviews，离线点虚线即可见。与 .anchor（画虚线用的
 * range+abstract）互补，同生命周期（重下重建、drop/forget/repair 连带删、缺则补抓）。
 *
 * 说明：书级 GET /review/list 经真机字段审计，其记录不带可用 range/chapterUid，
 * 锚不到具体句子，故改用 readreviews 按 range 拉取。
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "weread_notes.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WRB_PER_RANGE_CAP 50          /* 单次 readreviews 请求的页大小（count），不是总条数上限。 */
#define WRB_TOTAL_CAP     8000        /* 单章防失控安全上限（远高于真实数据），累积与落盘共用。 */
#define WRB_CHAPTER_CAP   WRB_TOTAL_CAP

/// 内存中的一条多人想法（on-disk 用紧凑变长格式，这里是固定内存结构）。
typedef struct {
    char review_id[WRN_ID];
    char range[WRN_RANGE];
    char author[WRN_AUTHOR];
    char content[WRN_TEXT];
    uint32_t likes;
    uint32_t create_time;
} wrb_record_t;

/*
 * 把已在 PSRAM 收集好的单章多人想法写入 per-chapter sidecar（原子写、CRC32）。
 * 调用方负责按 review_id 去重与每 range 50 条上限；n=0 写合法空 sidecar。
 * 写失败返回 false，不影响正文与缓存计数。
 */
bool wrb_store_records(const char *book_id, unsigned index, const wrb_record_t *records, size_t n);

/// 读回单章 sidecar（magic/version/CRC 校验），返回 PSRAM 数组（调用方 free）；缺失/损坏返回 false。
bool wrb_load(const char *book_id, unsigned index, wrb_record_t **out, size_t *count);

/// sidecar 是否存在（非空文件）。
bool wrb_exists(const char *book_id, unsigned index);

/// sidecar 是否为当前版本（magic/version 校验），旧版本文件判失效以触发重抓。
bool wrb_is_current(const char *book_id, unsigned index);

/// 删除单章 sidecar（忽略不存在）。
bool wrb_drop(const char *book_id, unsigned index);

#ifdef __cplusplus
}
#endif
