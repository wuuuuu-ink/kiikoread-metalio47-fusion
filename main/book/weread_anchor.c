/* SPDX-FileCopyrightText: 2026 YIYO-Designer */
/* SPDX-License-Identifier: LicenseRef-Kiikoread-Personal-Study-1.0 */

#include "weread_anchor.h"

#include "weread_browse.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_rom_crc.h"

#include "html_text.h"
#include "weread_consent.h"
#include "weread_internal.h"

#define WRA_MAGIC        "WRANCHR1"
#define WRA_MAGIC_LEN    8u
/* v4：锚点改为按 range 全量落盘（不再依赖可选的 abstract 字段），并据此重建历史 count=0 空锚点。
 * v5：修复 html_text 多点云→本地映射的门控（标记识别误以 mapped 为总开关，导致 local 全 0）。
 *     存量 v4 锚点的本地区间都是 0，必须升版本强制重建，修复才能对已缓存书生效。
 * v6：实测 v5 启用的确定性多点映射坐标口径有偏差，会把划线落到无段评句子（误划线）。最终回退
 *     该路径休眠（html_text 门控改回仅 mapped），且 book_layout 划线统一只走 abstract 文本匹配、
 *     不再读取确定性本地区间；v4/v5/v6 布局一致，均直接兼容加载，不强制重建、开书即刻有划线。 */
#define WRA_VERSION      6u
/* 渲染侧可读的最低版本：v4→v6 的 wra_entry_t 与文件二进制布局完全一致，且划线已统一只走
 * abstract 文本匹配（book_layout 不再使用确定性本地区间），故 v4/v5/v6 锚点都可直接加载，
 * 开书即刻有划线、无需等待重建。wra_is_current 同样把该区间视为当前，避免开书触发重型
 * readreviews 重建（实测在 160~276KB JSON 重处理期间会崩溃）。新写入仍用 WRA_VERSION。 */
#define WRA_MIN_READ_VERSION 4u
/* 弱网下 range 越多服务端响应越慢、易超 12s 超时；小批（10）请求体小、响应快，成功率更高，
   代价是批次数变多（52 个 range 约 6 批），仍在同一 worker 串行。 */
#define WRA_BATCH        10u
#define WRA_PSRAM        (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)

typedef struct {
    char magic[WRA_MAGIC_LEN];
    uint32_t version;
    uint32_t count;
    uint32_t reserved;
} wra_header_t;

_Static_assert(sizeof(wra_header_t) == 20u, "wra header layout");
_Static_assert(sizeof(wra_entry_t) == (size_t)(WRN_RANGE + WRN_ABSTRACT + 8), "wra entry layout");

static const char *TAG = "WRANCHOR";

/* ---- 路径 ---- */

static bool anchor_dir(const char *book_id, char *out, size_t cap) {
    char root[WR_PATH];
    if (!wr_stage_root(root, sizeof(root), book_id)) return false;
    int n = snprintf(out, cap, "%s/r", root);
    return n > 0 && (size_t)n < cap;
}

static bool anchor_path(const char *book_id, unsigned index, char *out, size_t cap) {
    char dir[WR_PATH];
    if (!anchor_dir(book_id, dir, sizeof(dir))) return false;
    int n = snprintf(out, cap, "%s/c%u.anchor", dir, index);
    return n > 0 && (size_t)n < cap;
}

static bool ensure_dir(const char *path) {
    if (mkdir(path, 0775) == 0) return true;
    return errno == EEXIST;
}

/* ---- 原子写（FAT 上先删目标再 rename，避免 rename 覆盖失败） ---- */

static bool anchor_write(const char *path, const void *data, size_t size) {
    char tmp[WR_PATH + 8];
    if (snprintf(tmp, sizeof(tmp), "%s.tmp", path) >= (int)sizeof(tmp)) return false;
    FILE *f = fopen(tmp, "wb");
    if (!f) return false;
    bool ok = !size || fwrite(data, 1, size, f) == size;
    ok = ok && !fflush(f) && !fsync(fileno(f));
    if (fclose(f)) ok = false;
    if (!ok) { unlink(tmp); return false; }
    if (unlink(path) != 0 && errno != ENOENT) { unlink(tmp); return false; }
    if (rename(tmp, path) != 0) { unlink(tmp); return false; }
    return true;
}

/* ---- 网络解析 ---- */

/* 从 readreviews 回包确保每个带 pageReviews 的 range 都有锚点条目：
   - 已存在同 range 的条目：abstract 为空时用回包 abstract 补上；
   - 新 range：建条目，abstract 缺失也保留（靠确定性本地区间绘制）。
   按 range 去重；非空 abstract 再按 abstract 去重，避免同一句重复画虚线。 */
static uint32_t parse_abstract_batch(const cJSON *resp, wra_entry_t *entries, uint32_t have) {
    const cJSON *r_arr = cJSON_GetObjectItemCaseSensitive(resp, "reviews");
    if (!cJSON_IsArray(r_arr)) return have;
    const cJSON *item = NULL;
    cJSON_ArrayForEach(item, r_arr) {
        if (have >= WRA_MAX_ENTRIES) break;
        const cJSON *rj = cJSON_GetObjectItemCaseSensitive(item, "range");
        if (!cJSON_IsString(rj) || !rj->valuestring || !rj->valuestring[0]) continue;
        const cJSON *pages = cJSON_GetObjectItemCaseSensitive(item, "pageReviews");
        if (!cJSON_IsArray(pages)) continue;
        const cJSON *pr0 = cJSON_GetArrayItem(pages, 0);
        const cJSON *rv0 = pr0 ? cJSON_GetObjectItemCaseSensitive(pr0, "review") : NULL;
        const cJSON *ab = cJSON_IsObject(rv0) ? cJSON_GetObjectItemCaseSensitive(rv0, "abstract") : NULL;
        const char *ab_text = (cJSON_IsString(ab) && ab->valuestring) ? ab->valuestring : NULL;

        /* 找同 range 已有条目：存在则只在 abstract 为空时补填。 */
        int existing = -1;
        for (uint32_t k = 0; k < have; ++k) {
            if (strcmp(entries[k].range, rj->valuestring) == 0) { existing = (int)k; break; }
        }
        if (existing >= 0) {
            if (ab_text && !entries[existing].abstract[0])
                snprintf(entries[existing].abstract, WRN_ABSTRACT, "%s", ab_text);
            continue;
        }

        /* 新 range：非空 abstract 按 abstract 去重。 */
        if (ab_text) {
            bool dup = false;
            for (uint32_t k = 0; k < have; ++k)
                if (strcmp(entries[k].abstract, ab_text) == 0) { dup = true; break; }
            if (dup) continue;
        }

        wra_entry_t *e = &entries[have++];
        snprintf(e->range, WRN_RANGE, "%s", rj->valuestring);
        if (ab_text) snprintf(e->abstract, WRN_ABSTRACT, "%s", ab_text);
    }
    return have;
}

/* 从 readreviews 回包里收集每条 range 的全部多人想法（pageReviews 全量），追加到动态数组。
   按 review_id 去重、每 range 限 50；返回 false 表示内存分配失败。 */
static bool parse_review_records(const cJSON *resp, wrb_record_t **arr, size_t *arr_n, size_t *arr_cap) {
    const cJSON *r_arr = cJSON_GetObjectItemCaseSensitive(resp, "reviews");
    if (!cJSON_IsArray(r_arr)) return true;
    const cJSON *entry = NULL;
    cJSON_ArrayForEach(entry, r_arr) {
        const cJSON *rj = cJSON_GetObjectItemCaseSensitive(entry, "range");
        if (!cJSON_IsString(rj) || !rj->valuestring || !rj->valuestring[0]) continue;
        const char *range = rj->valuestring;
        const cJSON *pages = cJSON_GetObjectItemCaseSensitive(entry, "pageReviews");
        if (!cJSON_IsArray(pages)) continue;
        const cJSON *pr = NULL;
        cJSON_ArrayForEach(pr, pages) {
            wrb_record_t tmp;
            memset(&tmp, 0, sizeof(tmp));
            /* reviewId 在 pageReview 上（数字，兼容字符串）。 */
            const cJSON *jid = cJSON_GetObjectItemCaseSensitive(pr, "reviewId");
            if (cJSON_IsNumber(jid)) {
                snprintf(tmp.review_id, sizeof(tmp.review_id), "%lld", (long long)jid->valuedouble);
            } else if (cJSON_IsString(jid) && jid->valuestring) {
                snprintf(tmp.review_id, sizeof(tmp.review_id), "%s", jid->valuestring);
            }
            snprintf(tmp.range, sizeof(tmp.range), "%s", range);
            const cJSON *rv = cJSON_GetObjectItemCaseSensitive(pr, "review");
            if (cJSON_IsObject(rv)) {
                const cJSON *jc = cJSON_GetObjectItemCaseSensitive(rv, "content");
                if (cJSON_IsString(jc) && jc->valuestring)
                    snprintf(tmp.content, sizeof(tmp.content), "%s", jc->valuestring);
                const cJSON *jct = cJSON_GetObjectItemCaseSensitive(rv, "createTime");
                if (cJSON_IsNumber(jct)) tmp.create_time = (uint32_t)jct->valuedouble;
                const cJSON *jau = cJSON_GetObjectItemCaseSensitive(rv, "author");
                if (cJSON_IsObject(jau)) {
                    const cJSON *jn = cJSON_GetObjectItemCaseSensitive(jau, "name");
                    if (cJSON_IsString(jn) && jn->valuestring)
                        snprintf(tmp.author, sizeof(tmp.author), "%s", jn->valuestring);
                } else if (cJSON_IsString(jau) && jau->valuestring) {
                    snprintf(tmp.author, sizeof(tmp.author), "%s", jau->valuestring);
                }
            }
            const cJSON *jlike = cJSON_GetObjectItemCaseSensitive(pr, "likesCount");
            if (!cJSON_IsNumber(jlike)) jlike = cJSON_GetObjectItemCaseSensitive(pr, "likeCount");
            if (cJSON_IsNumber(jlike)) tmp.likes = (uint32_t)jlike->valuedouble;

            if (!tmp.review_id[0] || !tmp.content[0]) continue;

            /* 按 review_id 去重（全量拉取，不再限制每 range 条数）。 */
            bool dup = false;
            for (size_t k = 0; k < *arr_n; ++k)
                if (strcmp((*arr)[k].review_id, tmp.review_id) == 0) { dup = true; break; }
            if (dup) continue;
            if (*arr_n >= WRB_TOTAL_CAP) return true;

            if (*arr_n >= *arr_cap) {
                size_t new_cap = *arr_cap ? *arr_cap * 2u : 64u;
                if (new_cap > WRB_TOTAL_CAP) new_cap = WRB_TOTAL_CAP;
                wrb_record_t *grown = heap_caps_realloc(*arr, new_cap * sizeof(wrb_record_t), WRA_PSRAM);
                if (!grown) return false;
                *arr = grown;
                *arr_cap = new_cap;
            }
            (*arr)[(*arr_n)++] = tmp;
        }
    }
    return true;
}

/* 单条 range 的续抓游标：maxIdx/synckey 随响应推进，has_more 决定是否继续。 */
typedef struct {
    char range[WRN_RANGE];
    uint32_t max_idx;
    uint32_t synckey;
    bool has_more;
} rr_cursor_t;

static rr_cursor_t *cursor_find(rr_cursor_t *c, size_t n, const char *range) {
    for (size_t i = 0; i < n; ++i)
        if (strcmp(c[i].range, range) == 0) return &c[i];
    return NULL;
}

/* 用响应里每条 entry 的 maxIdx/synckey/hasMore 推进对应游标。 */
static void cursor_update_from_resp(rr_cursor_t *c, size_t n, const cJSON *resp) {
    const cJSON *r_arr = cJSON_GetObjectItemCaseSensitive(resp, "reviews");
    if (!cJSON_IsArray(r_arr)) return;
    const cJSON *item = NULL;
    cJSON_ArrayForEach(item, r_arr) {
        const cJSON *rj = cJSON_GetObjectItemCaseSensitive(item, "range");
        if (!cJSON_IsString(rj) || !rj->valuestring) continue;
        rr_cursor_t *cur = cursor_find(c, n, rj->valuestring);
        if (!cur) continue;
        const cJSON *jmi = cJSON_GetObjectItemCaseSensitive(item, "maxIdx");
        if (cJSON_IsNumber(jmi)) cur->max_idx = (uint32_t)jmi->valuedouble;
        const cJSON *jsk = cJSON_GetObjectItemCaseSensitive(item, "synckey");
        if (cJSON_IsNumber(jsk)) cur->synckey = (uint32_t)jsk->valuedouble;
        const cJSON *jhm = cJSON_GetObjectItemCaseSensitive(item, "hasMore");
        cur->has_more = cJSON_IsNumber(jhm) && jhm->valuedouble != 0;
    }
}

/* 读取整个文件到 PSRAM（调用方 free），失败返回 NULL。/ Read a whole file into PSRAM. */
static char *read_whole_file(const char *path, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long sz = ftell(f);
    if (sz <= 0 || (uint64_t)sz > HTML_TEXT_MAX_BYTES) { fclose(f); return NULL; }
    if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); return NULL; }
    char *data = heap_caps_malloc((size_t)sz + 1, WRA_PSRAM);
    if (!data) { fclose(f); return NULL; }
    size_t got = fread(data, 1, (size_t)sz, f);
    fclose(f);
    if (got != (size_t)sz) { free(data); return NULL; }
    data[got] = 0;
    if (out_len) *out_len = got;
    return data;
}

static int uint_cmp(const void *a, const void *b) {
    unsigned x = *(const unsigned *)a, y = *(const unsigned *)b;
    return (x > y) - (x < y);
}

/* 在升序 targets 中按值取对应 mapped（找不到返回 0）。/ Binary-search the mapped value. */
static unsigned mapped_for(const unsigned *targets, const unsigned *mapped,
                           size_t n, unsigned value) {
    size_t lo = 0, hi = n;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (targets[mid] < value) lo = mid + 1;
        else hi = mid;
    }
    return (lo < n && targets[lo] == value) ? mapped[lo] : 0;
}

/* 用下载阶段的章节 HTML，把每个云端 range 确定性换算成正文本地字节区间（与 ranges 平行）。
 * HTML 缺失或解析失败时返回 false，渲染侧退回 abstract 文本匹配。
 * Deterministically map each cloud range to local body offsets via the staged chapter HTML. */
static bool map_ranges_to_local(const char *book_id, unsigned index,
                                char *const ranges[], size_t nrange,
                                unsigned *local_start, unsigned *local_end) {
    wr_book_t book = {0};
    snprintf(book.id, sizeof(book.id), "%s", book_id);
    char html_path[WR_PATH];
    if (!wr_stage_path(&book, index, html_path)) return false;
    size_t html_len = 0;
    char *html = read_whole_file(html_path, &html_len);
    if (!html) {
        ESP_LOGW(TAG, "range map: staged html unavailable ch%u", index);
        return false;
    }
    unsigned *targets = heap_caps_calloc(nrange * 2 + 1, sizeof(unsigned), WRA_PSRAM);
    unsigned *mapped = heap_caps_calloc(nrange * 2 + 1, sizeof(unsigned), WRA_PSRAM);
    if (!targets || !mapped) {
        free(html); free(targets); free(mapped);
        return false;
    }
    size_t nt = 0;
    for (size_t i = 0; i < nrange; ++i) {
        const char *dash = strchr(ranges[i], '-');
        if (!dash) continue;
        targets[nt++] = (unsigned)strtoul(ranges[i], NULL, 10);
        targets[nt++] = (unsigned)strtoul(dash + 1, NULL, 10);
    }
    qsort(targets, nt, sizeof(unsigned), uint_cmp);
    size_t uniq = 0;
    for (size_t i = 0; i < nt; ++i)
        if (!uniq || targets[uniq - 1] != targets[i]) targets[uniq++] = targets[i];
    bool ok = html_weread_map_cloud(html, html_len, targets, uniq, mapped);
    free(html);
    if (!ok) { free(targets); free(mapped); return false; }
    for (size_t i = 0; i < nrange; ++i) {
        const char *dash = strchr(ranges[i], '-');
        if (!dash) continue;
        unsigned a = (unsigned)strtoul(ranges[i], NULL, 10);
        unsigned b = (unsigned)strtoul(dash + 1, NULL, 10);
        local_start[i] = mapped_for(targets, mapped, uniq, a);
        local_end[i] = mapped_for(targets, mapped, uniq, b);
    }
    free(targets);
    free(mapped);
    return true;
}

/* ---- bestbookmarks 全量热门划线（前缀分页） ---- */

typedef struct {
    char range[WRN_RANGE];
    char mark[WRN_ABSTRACT];
} wra_best_t;

/* 逐章拉取 bestbookmarks 全量热门划线。服务端按"前缀"返回（每次回 0..maxIdx+count 的整段），
   故只取索引 >= maxIdx 的新条目，按新条目数推进 maxIdx，直到无新条目或达 totalCount。
   弱网失败不致命：调用方退回 underlines 单源，后续 repair 再补。 */
static bool fetch_best_chapter(const char *book_id, uint32_t chapter_uid_num,
                               wra_best_t *out, size_t cap, size_t *out_n) {
    *out_n = 0;
    size_t max_idx = 0;
    for (unsigned page = 0; page < 40u; ++page) {
        char get[WR_PATH];
        if (snprintf(get, sizeof(get),
                     "/book/bestbookmarks?bookId=%s&chapterUid=%u&count=100&maxIdx=%u&synckey=0",
                     book_id, (unsigned)chapter_uid_num, (unsigned)max_idx) >= (int)sizeof(get))
            return false;
        size_t len = 0;
        char *text = wr_consent_app_call_quick(get, NULL, &len);
        if (!text) return false;
        cJSON *json = cJSON_Parse(text);
        free(text);
        if (!json) return false;
        const cJSON *items = cJSON_GetObjectItemCaseSensitive(json, "items");
        unsigned total_in_resp = cJSON_IsArray(items) ? (unsigned)cJSON_GetArraySize(items) : 0u;
        unsigned added = 0;
        for (unsigned idx = (unsigned)max_idx; idx < total_in_resp; ++idx) {
            const cJSON *it = cJSON_GetArrayItem(items, idx);
            const cJSON *rj = cJSON_GetObjectItemCaseSensitive(it, "range");
            if (!cJSON_IsString(rj) || !rj->valuestring || !rj->valuestring[0])
                rj = cJSON_GetObjectItemCaseSensitive(it, "simplifiedRange");
            if (!cJSON_IsString(rj) || !rj->valuestring || !rj->valuestring[0]) continue;
            const cJSON *mk = cJSON_GetObjectItemCaseSensitive(it, "markText");
            if (!cJSON_IsString(mk) || !mk->valuestring || !mk->valuestring[0]) continue;
            if (*out_n >= cap) { cJSON_Delete(json); goto done; }
            wra_best_t *b = &out[(*out_n)++];
            snprintf(b->range, sizeof(b->range), "%s", rj->valuestring);
            snprintf(b->mark, sizeof(b->mark), "%s", mk->valuestring);
            ++added;
        }
        uint32_t total_count = 0;
        const cJSON *tc = cJSON_GetObjectItemCaseSensitive(json, "totalCount");
        if (cJSON_IsNumber(tc)) total_count = (uint32_t)tc->valuedouble;
        cJSON_Delete(json);
        if (added == 0) break;
        max_idx += added;
        if (total_count && max_idx >= total_count) break;
        if (total_in_resp <= max_idx) break;
    }
done:
    ESP_LOGI(TAG, "bestbookmarks uid=%u n=%u", (unsigned)chapter_uid_num, (unsigned)*out_n);
    return true;
}

bool wra_fetch_and_store(const char *book_id, const char *chapter_uid, unsigned index) {
    if (!book_id || !book_id[0] || !chapter_uid || !chapter_uid[0]) return false;
    if (!wr_online() || !wr_consent_authorized()) return false;

    /* 1. underlines：拿到本章全部公开段评 range。 */
    char get[WR_PATH];
    if (snprintf(get, sizeof(get), "/book/underlines?bookId=%s&chapterUid=%s",
                 book_id, chapter_uid) >= (int)sizeof(get)) return false;
    size_t ul_len = 0;
    char *ul_text = wr_consent_app_call_quick(get, NULL, &ul_len);
    if (!ul_text) {
        ESP_LOGW(TAG, "underlines unavailable ch%u", index);
        return false;
    }
    ESP_LOGI(TAG, "underlines resp ch%u len=%u head: %.*s", index, (unsigned)ul_len,
             (int)(ul_len > 160 ? 160 : ul_len), ul_text);
    cJSON *ul = cJSON_Parse(ul_text);
    free(ul_text);
    if (!ul) return false;

    char **ranges = heap_caps_calloc(WRA_MAX_ENTRIES, sizeof(char *), WRA_PSRAM);
    if (!ranges) { cJSON_Delete(ul); return false; }   /* 分配失败不写空锚点，避免误判"确认空" */
    size_t nrange = 0;
    {
        const cJSON *arr = cJSON_GetObjectItemCaseSensitive(ul, "underlines");
        if (cJSON_IsArray(arr)) {
            const cJSON *u = NULL;
            cJSON_ArrayForEach(u, arr) {
                if (nrange >= WRA_MAX_ENTRIES) break;
                const cJSON *rj = cJSON_GetObjectItemCaseSensitive(u, "range");
                if (!cJSON_IsString(rj) || !rj->valuestring || !rj->valuestring[0]) continue;
                const cJSON *cnt = cJSON_GetObjectItemCaseSensitive(u, "count");
                if (cJSON_IsNumber(cnt) && cnt->valuedouble == 0) continue;
                char *copy = heap_caps_malloc(strlen(rj->valuestring) + 1, WRA_PSRAM);
                if (!copy) break;
                strcpy(copy, rj->valuestring);
                ranges[nrange++] = copy;
            }
        }
    }
    cJSON_Delete(ul);
    ESP_LOGI(TAG, "underlines ranges ch%u n=%u", index, (unsigned)nrange);

    /* 1b. bestbookmarks 全量热门划线：与 underlines 取并集去重，markText 预填 abstract。
       弱网失败不阻断，退回 underlines 单源，后续 repair 再补。 */
    wra_best_t *best = heap_caps_calloc(WRA_MAX_ENTRIES, sizeof(wra_best_t), WRA_PSRAM);
    size_t nbest = 0;
    if (best) {
        if (!fetch_best_chapter(book_id, (uint32_t)strtoul(chapter_uid, NULL, 10),
                                best, WRA_MAX_ENTRIES, &nbest)) {
            ESP_LOGW(TAG, "bestbookmarks failed ch%u, underlines-only", index);
            nbest = 0;
        }
        for (size_t bi = 0; bi < nbest; ++bi) {
            bool seen = false;
            for (size_t i = 0; i < nrange; ++i)
                if (strcmp(ranges[i], best[bi].range) == 0) { seen = true; break; }
            if (seen || nrange >= WRA_MAX_ENTRIES) continue;
            char *copy = heap_caps_malloc(strlen(best[bi].range) + 1, WRA_PSRAM);
            if (!copy) break;
            strcpy(copy, best[bi].range);
            ranges[nrange++] = copy;
        }
    }
    ESP_LOGI(TAG, "merged ranges ch%u n=%u (best=%u)", index, (unsigned)nrange, (unsigned)nbest);

    /* 2. 游标分页 readreviews：每批 10 条 range、每请求 count=50，hasMore=1 的 range 按
          maxIdx/synckey 续抓直到取全；某批网络失败只保留已拿到的部分，不阻断落盘。 */
    wra_entry_t *entries = heap_caps_calloc(WRA_MAX_ENTRIES, sizeof(wra_entry_t), WRA_PSRAM);
    uint32_t nent = 0;
    /* 每个云端 range 都落一条锚点：是否画虚线由 range 本身决定，不依赖 abstract 是否存在
       （readreviews 的 abstract 为可选字段，可能缺失；此时仅靠确定性本地区间绘制）。
       bestbookmarks 的 markText 作为 abstract 预填，其余 range 先留空，由 readreviews 补。 */
    if (entries) {
        for (size_t i = 0; i < nrange && nent < WRA_MAX_ENTRIES; ++i) {
            wra_entry_t *e = &entries[nent++];
            snprintf(e->range, WRN_RANGE, "%s", ranges[i]);
            if (best) {
                for (size_t bi = 0; bi < nbest; ++bi) {
                    if (strcmp(best[bi].range, ranges[i]) == 0) {
                        snprintf(e->abstract, WRN_ABSTRACT, "%s", best[bi].mark);
                        break;
                    }
                }
            }
        }
    }
    /* 多人想法累积数组（PSRAM，动态扩容），与 abstract 同批采集。 */
    wrb_record_t *rev_records = NULL;
    size_t rev_n = 0, rev_cap = 0;

    rr_cursor_t *cursors = heap_caps_calloc(nrange ? nrange : 1u, sizeof(rr_cursor_t), WRA_PSRAM);
    if (entries && cursors) {
        for (size_t i = 0; i < nrange; ++i) {
            snprintf(cursors[i].range, sizeof(cursors[i].range), "%s", ranges[i]);
            cursors[i].max_idx = 0;
            cursors[i].synckey = 0;
            cursors[i].has_more = true;
        }
        /* 单章续抓轮数安全上限，防止异常回包导致空转（真实书每 range 仅需 1~2 轮）。 */
        for (size_t round_no = 0; round_no < 60; ++round_no) {
            size_t *todo = heap_caps_malloc((nrange ? nrange : 1u) * sizeof(size_t), WRA_PSRAM);
            if (!todo) break;
            size_t ntodo = 0;
            for (size_t i = 0; i < nrange; ++i)
                if (cursors[i].has_more) todo[ntodo++] = i;
            if (!ntodo) { free(todo); break; }

            bool round_failed = false;
            for (size_t b = 0; b < ntodo && !round_failed; b += WRA_BATCH) {
                size_t take = ntodo - b;
                if (take > WRA_BATCH) take = WRA_BATCH;

                /* 官方 readreviews 入参：chapterUid 数字，reviews 对象数组，每条带当前游标。 */
                cJSON *jb = cJSON_CreateObject();
                cJSON_AddStringToObject(jb, "bookId", book_id);
                cJSON_AddNumberToObject(jb, "chapterUid", (double)strtoul(chapter_uid, NULL, 10));
                cJSON_AddStringToObject(jb, "cht2sMode", "");
                cJSON *jarr = cJSON_CreateArray();
                for (size_t k = 0; k < take; ++k) {
                    const rr_cursor_t *cur = &cursors[todo[b + k]];
                    cJSON *one = cJSON_CreateObject();
                    cJSON_AddStringToObject(one, "range", cur->range);
                    cJSON_AddNumberToObject(one, "maxIdx", (double)cur->max_idx);
                    cJSON_AddNumberToObject(one, "count", WRB_PER_RANGE_CAP);
                    cJSON_AddNumberToObject(one, "synckey", (double)cur->synckey);
                    cJSON_AddItemToArray(jarr, one);
                }
                cJSON_AddItemToObject(jb, "reviews", jarr);
                char *body = cJSON_PrintUnformatted(jb);
                cJSON_Delete(jb);
                if (!body) { ESP_LOGW(TAG, "body build failed ch%u", index); round_failed = true; break; }

                size_t rr_len = 0;
                char *rr = wr_consent_app_call_quick("/book/readreviews", body, &rr_len);
                free(body);
                if (rr) {
                    ESP_LOGI(TAG, "readreviews resp ch%u round%u len=%u head: %.*s",
                             index, (unsigned)round_no, (unsigned)rr_len,
                             (int)(rr_len > 160 ? 160 : rr_len), rr);
                    cJSON *parsed = cJSON_Parse(rr);
                    free(rr);
                    if (parsed) {
                        nent = parse_abstract_batch(parsed, entries, nent);
                        if (!parse_review_records(parsed, &rev_records, &rev_n, &rev_cap))
                            ESP_LOGW(TAG, "review records accumulate failed ch%u", index);
                        cursor_update_from_resp(cursors, nrange, parsed);
                        cJSON_Delete(parsed);
                    }
                } else {
                    round_failed = true;
                    ESP_LOGW(TAG, "readreviews failed ch%u round%u", index, (unsigned)round_no);
                }
            }
            free(todo);
            if (round_failed) break;
        }
    }
    bool buffers_ok = (entries != NULL) && (cursors != NULL);
    uint32_t mapped_ok = 0;

    /* 用章节 HTML 把每个云端 range 确定性换算成本地正文区间，写进对应 entry；失败则保留 0，
     * 渲染侧自动退回 abstract 文本匹配。/ Map cloud ranges to local offsets before freeing. */
    if (buffers_ok && nrange > 0) {
        unsigned *ls = heap_caps_calloc(nrange, sizeof(unsigned), WRA_PSRAM);
        unsigned *le = heap_caps_calloc(nrange, sizeof(unsigned), WRA_PSRAM);
        if (ls && le && map_ranges_to_local(book_id, index, ranges, nrange, ls, le)) {
            for (uint32_t k = 0; k < nent; ++k) {
                for (size_t i = 0; i < nrange; ++i) {
                    if (strcmp(entries[k].range, ranges[i]) == 0) {
                        entries[k].local_start = ls[i];
                        entries[k].local_end = le[i];
                        break;
                    }
                }
            }
        } else {
            ESP_LOGW(TAG, "local range mapping unavailable ch%u, abstract fallback used", index);
        }
        free(ls);
        free(le);
        for (uint32_t k = 0; k < nent; ++k)
            if (entries[k].local_start < entries[k].local_end) ++mapped_ok;
    }

    for (size_t i = 0; i < nrange; ++i) free(ranges[i]);
    free(ranges);
    free(cursors);
    free(best);

    if (!buffers_ok) { free(entries); free(rev_records); return false; }

    /* 3. 组装文件（header + entries + CRC32）并原子写。 */
    char dir[WR_PATH], path[WR_PATH];
    if (!anchor_dir(book_id, dir, sizeof(dir)) || !anchor_path(book_id, index, path, sizeof(path))) {
        free(entries);
        return false;
    }
    if (!ensure_dir(dir)) {
        free(entries);
        return false;
    }

    size_t payload = sizeof(wra_header_t) + (size_t)nent * sizeof(wra_entry_t);
    size_t total = payload + sizeof(uint32_t);
    uint8_t *buf = heap_caps_malloc(total, WRA_PSRAM);
    if (!buf) { free(entries); free(rev_records); return false; }

    wra_header_t *hf = (wra_header_t *)buf;
    memcpy(hf->magic, WRA_MAGIC, WRA_MAGIC_LEN);
    hf->version = WRA_VERSION;
    hf->count = nent;
    hf->reserved = 0;
    memcpy(buf + sizeof(wra_header_t), entries, (size_t)nent * sizeof(wra_entry_t));
    uint32_t crc = esp_rom_crc32_le(0, buf, (uint32_t)payload);
    memcpy(buf + payload, &crc, sizeof(crc));

    bool ok = anchor_write(path, buf, total);
    free(buf);
    free(entries);
    ESP_LOGI(TAG, "stored %u anchors ch%u mapped=%u", nent, index, mapped_ok);

    /* 4. 同批多人想法落 .reviews：拿到新想法、或原本没有 sidecar 时才写；重抓失败（rev_n=0）
          保留旧 sidecar，避免一次网络抖动把已离线的多人想法清空。anchor 失败则不写。 */
    if (ok && (rev_n > 0 || !wrb_exists(book_id, index))) {
        bool side_ok = wrb_store_records(book_id, index, rev_records, rev_n);
        if (!side_ok) ESP_LOGW(TAG, "store reviews sidecar failed ch%u", index);
    }
    free(rev_records);
    return ok;
}

bool wra_load(const char *book_id, unsigned index, wra_entry_t **out, uint32_t *count) {
    if (out) *out = NULL;
    if (count) *count = 0;
    char path[WR_PATH];
    if (!anchor_path(book_id, index, path, sizeof(path))) return false;

    FILE *f = fopen(path, "rb");
    if (!f) return false;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return false; }
    long size = ftell(f);
    if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); return false; }
    long min_size = (long)(sizeof(wra_header_t) + sizeof(uint32_t));
    long max_size = min_size + (long)(WRA_MAX_ENTRIES * sizeof(wra_entry_t));
    if (size < min_size || size > max_size) { fclose(f); return false; }

    uint8_t *buf = heap_caps_malloc((size_t)size, WRA_PSRAM);
    if (!buf) { fclose(f); return false; }
    bool read_ok = fread(buf, 1, (size_t)size, f) == (size_t)size;
    fclose(f);
    if (!read_ok) { free(buf); return false; }

    const wra_header_t *hf = (const wra_header_t *)buf;
    /* 兼容读取上一版（布局一致），但 wra_is_current 仍严格判过期以触发重建。 */
    if (memcmp(hf->magic, WRA_MAGIC, WRA_MAGIC_LEN) != 0 ||
        hf->version < WRA_MIN_READ_VERSION || hf->version > WRA_VERSION ||
        hf->reserved != 0 || hf->count > WRA_MAX_ENTRIES) {
        free(buf);
        return false;
    }
    size_t expected = sizeof(wra_header_t) + (size_t)hf->count * sizeof(wra_entry_t) + sizeof(uint32_t);
    if ((size_t)size != expected) { free(buf); return false; }

    size_t payload = expected - sizeof(uint32_t);
    uint32_t stored = 0;
    memcpy(&stored, buf + payload, sizeof(stored));
    if (esp_rom_crc32_le(0, buf, (uint32_t)payload) != stored) {
        free(buf);
        return false;
    }

    uint32_t n = hf->count;
    wra_entry_t *res = heap_caps_malloc((size_t)n * sizeof(wra_entry_t) + 1, WRA_PSRAM);
    if (!res) { free(buf); return false; }
    if (n) memcpy(res, buf + sizeof(wra_header_t), (size_t)n * sizeof(wra_entry_t));
    free(buf);
    *out = res;
    *count = n;
    return true;
}

/* 锚点是否为当前版本（magic/version 校验）：旧版本文件据此判失效并触发 repair 重抓。
 * Whether the anchor is the current version; a stale version triggers a rebuild. */
bool wra_is_current(const char *book_id, unsigned index) {
    char path[WR_PATH];
    if (!anchor_path(book_id, index, path, sizeof(path))) return false;
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    wra_header_t hf;
    bool read_ok = fread(&hf, 1, sizeof(hf), f) == sizeof(hf);
    fclose(f);
    return read_ok && memcmp(hf.magic, WRA_MAGIC, WRA_MAGIC_LEN) == 0 &&
           hf.version >= WRA_MIN_READ_VERSION && hf.version <= WRA_VERSION &&
           hf.reserved == 0;
}

bool wra_drop(const char *book_id, unsigned index) {
    char path[WR_PATH];
    if (!anchor_path(book_id, index, path, sizeof(path))) return false;
    if (unlink(path) != 0 && errno != ENOENT) return false;
    return true;
}
