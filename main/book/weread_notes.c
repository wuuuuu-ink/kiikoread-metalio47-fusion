/* SPDX-FileCopyrightText: 2026 YIYO-Designer */
/* SPDX-License-Identifier: LicenseRef-Kiikoread-Personal-1.0 */

#include "weread_notes.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "weread_anchor.h"
#include "weread_browse.h"
#include "weread_consent.h"
#include "weread_internal.h"

static const char *TAG = "WRNOTES";

/* ------------------------------------------------------------------ */
/* 模块状态（一次只打开一本书）                                         */
/* ------------------------------------------------------------------ */

static char s_book[WRN_ID];
static wrn_bookmark_t *s_bm;
static size_t s_bm_n;
static wrn_review_t *s_rv;
static size_t s_rv_n;
static wrn_best_t *s_best;
static size_t s_best_n;
static wrn_chapter_review_t *s_crv;
static size_t s_crv_n;
static size_t s_crv_cap;
static wrn_queue_item_t *s_q;
static size_t s_q_n;
static uint32_t s_bm_synckey;
static uint32_t s_rv_synckey;

static void *psram_calloc(size_t n, size_t size) {
    return heap_caps_calloc(n, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

/* 确保 s_crv 至少能容纳 need 条；容量按 64 条为一档增长，失败返回 false。
 * 首次只分配 64 条（约 80 KB），没有公开段评的书不再为 400 条空槽付 510 KB PSRAM。
 * Grow in 64-entry steps; the first allocation is 64 entries (~80 KB), so books without public
 * reviews no longer reserve the full 400-slot array (~510 KB of PSRAM) on open. */
static bool crv_ensure(size_t need) {
    if (need <= s_crv_cap) return true;
    size_t cap = s_crv_cap ? s_crv_cap : 64;
    while (cap < need) cap += 64;
    wrn_chapter_review_t *grown =
        heap_caps_realloc(s_crv, cap * sizeof(*s_crv), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!grown) return false;
    s_crv = grown;
    s_crv_cap = cap;
    return true;
}

/* ------------------------------------------------------------------ */
/* 路径与目录                                                          */
/* ------------------------------------------------------------------ */

static void path_dir(char *buf, size_t cap) {
    snprintf(buf, cap, WR_DIR "/notes-%s", s_book);
}
static void path_mine(char *buf, size_t cap) {
    snprintf(buf, cap, WR_DIR "/notes-%s/mine.json", s_book);
}
static void path_best(char *buf, size_t cap) {
    snprintf(buf, cap, WR_DIR "/notes-%s/best.json", s_book);
}
static void path_queue(char *buf, size_t cap) {
    snprintf(buf, cap, WR_DIR "/notes-%s/queue.json", s_book);
}

static bool dir_ensure(const char *path) {
    if (mkdir(path, 0777) == 0) return true;
    struct stat info;
    return stat(path, &info) == 0 && S_ISDIR(info.st_mode);
}

static bool ensure_base_dirs(void) {
    char base[WR_PATH];
    snprintf(base, sizeof(base), WR_DIR);
    (void)dir_ensure(base);
    path_dir(base, sizeof(base));
    return dir_ensure(base);
}

/* ------------------------------------------------------------------ */
/* 小工具                                                              */
/* ------------------------------------------------------------------ */

static void jstr(const cJSON *obj, const char *key, char *out, size_t cap) {
    out[0] = 0;
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsString(v) && v->valuestring) snprintf(out, cap, "%s", v->valuestring);
}

static uint32_t jnum(const cJSON *obj, const char *key) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsNumber(v) ? (uint32_t)v->valuedouble : 0;
}

static bool write_file(const char *path, const char *text) {
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    bool ok = fwrite(text, 1, strlen(text), f) == strlen(text);
    ok = ok && !fflush(f) && !fsync(fileno(f));
    if (fclose(f)) ok = false;
    return ok;
}

static char *read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long size = ftell(f);
    if (size < 0 || size > 4 * 1024 * 1024) { fclose(f); return NULL; }
    rewind(f);
    char *buf = heap_caps_malloc((size_t)size + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) { fclose(f); return NULL; }
    size_t got = fread(buf, 1, (size_t)size, f);
    fclose(f);
    buf[got] = 0;
    return buf;
}

/* ------------------------------------------------------------------ */
/* 持久化                                                              */
/* ------------------------------------------------------------------ */

static void save_mine(void) {
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "bm_synckey", s_bm_synckey);
    cJSON_AddNumberToObject(root, "rv_synckey", s_rv_synckey);
    cJSON *bms = cJSON_AddArrayToObject(root, "bookmarks");
    for (size_t i = 0; i < s_bm_n; ++i) {
        const wrn_bookmark_t *b = &s_bm[i];
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "id", b->bookmark_id);
        cJSON_AddStringToObject(o, "chapter", b->chapter_uid);
        cJSON_AddStringToObject(o, "range", b->range);
        cJSON_AddStringToObject(o, "text", b->mark_text);
        cJSON_AddStringToObject(o, "abstract", b->abstract);
        cJSON_AddNumberToObject(o, "style", b->style);
        cJSON_AddNumberToObject(o, "color", b->color_style);
        cJSON_AddNumberToObject(o, "ctime", b->create_time);
        cJSON_AddNumberToObject(o, "utime", b->update_time);
        cJSON_AddNumberToObject(o, "flags", b->flags);
        cJSON_AddItemToArray(bms, o);
    }
    cJSON *rvs = cJSON_AddArrayToObject(root, "reviews");
    for (size_t i = 0; i < s_rv_n; ++i) {
        const wrn_review_t *r = &s_rv[i];
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "id", r->review_id);
        cJSON_AddStringToObject(o, "chapter", r->chapter_uid);
        cJSON_AddStringToObject(o, "range", r->range);
        cJSON_AddStringToObject(o, "content", r->content);
        cJSON_AddStringToObject(o, "abstract", r->abstract);
        cJSON_AddNumberToObject(o, "ctime", r->create_time);
        cJSON_AddNumberToObject(o, "private", r->is_private);
        cJSON_AddNumberToObject(o, "flags", r->flags);
        cJSON_AddItemToArray(rvs, o);
    }
    char p[WR_PATH];
    path_mine(p, sizeof(p));
    char *text = cJSON_PrintUnformatted(root);
    if (text) { (void)write_file(p, text); free(text); }
    cJSON_Delete(root);
}

static void load_mine(void) {
    char p[WR_PATH];
    path_mine(p, sizeof(p));
    char *raw = read_file(p);
    if (!raw) return;
    cJSON *root = cJSON_Parse(raw);
    free(raw);
    if (!root) return;
    s_bm_synckey = jnum(root, "bm_synckey");
    s_rv_synckey = jnum(root, "rv_synckey");
    const cJSON *bms = cJSON_GetObjectItemCaseSensitive(root, "bookmarks");
    if (cJSON_IsArray(bms)) {
        cJSON *it = NULL;
        cJSON_ArrayForEach(it, bms) {
            if (s_bm_n >= WRN_BOOKMARK_MAX) break;
            wrn_bookmark_t *b = &s_bm[s_bm_n++];
            jstr(it, "id", b->bookmark_id, sizeof(b->bookmark_id));
            jstr(it, "chapter", b->chapter_uid, sizeof(b->chapter_uid));
            jstr(it, "range", b->range, sizeof(b->range));
            jstr(it, "text", b->mark_text, sizeof(b->mark_text));
            jstr(it, "abstract", b->abstract, sizeof(b->abstract));
            b->style = jnum(it, "style");
            b->color_style = jnum(it, "color");
            b->create_time = jnum(it, "ctime");
            b->update_time = jnum(it, "utime");
            b->flags = (uint8_t)jnum(it, "flags");
        }
    }
    const cJSON *rvs = cJSON_GetObjectItemCaseSensitive(root, "reviews");
    if (cJSON_IsArray(rvs)) {
        cJSON *it = NULL;
        cJSON_ArrayForEach(it, rvs) {
            if (s_rv_n >= WRN_REVIEW_MAX) break;
            wrn_review_t *r = &s_rv[s_rv_n++];
            jstr(it, "id", r->review_id, sizeof(r->review_id));
            jstr(it, "chapter", r->chapter_uid, sizeof(r->chapter_uid));
            jstr(it, "range", r->range, sizeof(r->range));
            jstr(it, "content", r->content, sizeof(r->content));
            jstr(it, "abstract", r->abstract, sizeof(r->abstract));
            r->create_time = jnum(it, "ctime");
            r->is_private = (uint8_t)jnum(it, "private");
            r->flags = (uint8_t)jnum(it, "flags");
        }
    }
    cJSON_Delete(root);
}

static void load_best(void) {
    char p[WR_PATH];
    path_best(p, sizeof(p));
    char *raw = read_file(p);
    if (!raw) return;
    cJSON *root = cJSON_Parse(raw);
    free(raw);
    if (!root) return;
    const cJSON *arr = cJSON_GetObjectItemCaseSensitive(root, "items");
    if (cJSON_IsArray(arr)) {
        cJSON *it = NULL;
        cJSON_ArrayForEach(it, arr) {
            if (s_best_n >= WRN_BEST_MAX) break;
            wrn_best_t *b = &s_best[s_best_n++];
            jstr(it, "chapter", b->chapter_uid, sizeof(b->chapter_uid));
            jstr(it, "text", b->mark_text, sizeof(b->mark_text));
            jstr(it, "author", b->author, sizeof(b->author));
            b->heat = jnum(it, "heat");
        }
    }
    cJSON_Delete(root);
}

static void save_queue(void) {
    cJSON *root = cJSON_CreateObject();
    cJSON *arr = cJSON_AddArrayToObject(root, "items");
    for (size_t i = 0; i < s_q_n; ++i) {
        const wrn_queue_item_t *q = &s_q[i];
        cJSON *o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "kind", q->kind);
        cJSON_AddStringToObject(o, "id", q->id);
        cJSON_AddStringToObject(o, "chapter", q->chapter_uid);
        cJSON_AddStringToObject(o, "range", q->range);
        cJSON_AddStringToObject(o, "text", q->text);
        cJSON_AddStringToObject(o, "abstract", q->abstract);
        cJSON_AddNumberToObject(o, "style", q->style);
        cJSON_AddNumberToObject(o, "private", q->is_private);
        cJSON_AddItemToArray(arr, o);
    }
    char p[WR_PATH];
    path_queue(p, sizeof(p));
    char *text = cJSON_PrintUnformatted(root);
    if (text) { (void)write_file(p, text); free(text); }
    cJSON_Delete(root);
}

static void load_queue(void) {
    char p[WR_PATH];
    path_queue(p, sizeof(p));
    char *raw = read_file(p);
    if (!raw) return;
    cJSON *root = cJSON_Parse(raw);
    free(raw);
    if (!root) return;
    const cJSON *arr = cJSON_GetObjectItemCaseSensitive(root, "items");
    if (cJSON_IsArray(arr)) {
        cJSON *it = NULL;
        cJSON_ArrayForEach(it, arr) {
            if (s_q_n >= WRN_QUEUE_MAX) break;
            wrn_queue_item_t *q = &s_q[s_q_n++];
            q->kind = (wrn_queue_kind_t)jnum(it, "kind");
            jstr(it, "id", q->id, sizeof(q->id));
            jstr(it, "chapter", q->chapter_uid, sizeof(q->chapter_uid));
            jstr(it, "range", q->range, sizeof(q->range));
            jstr(it, "text", q->text, sizeof(q->text));
            jstr(it, "abstract", q->abstract, sizeof(q->abstract));
            q->style = jnum(it, "style");
            q->is_private = (uint8_t)jnum(it, "private");
        }
    }
    cJSON_Delete(root);
}

/* ------------------------------------------------------------------ */
/* 生命周期                                                            */
/* ------------------------------------------------------------------ */

bool wrn_open(const char *book_id) {
    if (!book_id || !book_id[0] || strlen(book_id) >= sizeof(s_book)) return false;
    if (!strcmp(s_book, book_id)) return true;
    wrn_close();
    if (!ensure_base_dirs()) {
        ESP_LOGW(TAG, "notes dir unavailable book=%s", book_id);
    }
    snprintf(s_book, sizeof(s_book), "%s", book_id);
    s_bm = psram_calloc(WRN_BOOKMARK_MAX, sizeof(*s_bm));
    s_rv = psram_calloc(WRN_REVIEW_MAX, sizeof(*s_rv));
    s_best = psram_calloc(WRN_BEST_MAX, sizeof(*s_best));
    /* 段评按章按需分配：大多数书没有公开段评，开书时不再预留 400 条空槽。 */
    /* Chapter reviews allocate lazily: most books have none, so opening one no longer reserves
     * the 400-slot array up front. */
    s_crv = NULL;
    s_crv_cap = 0;
    s_q = psram_calloc(WRN_QUEUE_MAX, sizeof(*s_q));
    if (!s_bm || !s_rv || !s_best || !s_q) {
        ESP_LOGE(TAG, "alloc failed");
        return false;
    }
    load_mine();
    load_best();
    load_queue();
    ESP_LOGI(TAG, "open book=%s bm=%u rv=%u best=%u crv=%u queue=%u", s_book,
             (unsigned)s_bm_n, (unsigned)s_rv_n, (unsigned)s_best_n,
             (unsigned)s_crv_n, (unsigned)s_q_n);
    return true;
}

void wrn_close(void) {
    if (s_book[0]) {
        free(s_bm); free(s_rv); free(s_best); free(s_crv); free(s_q);
        s_bm = NULL;
        s_rv = NULL;
        s_best = NULL;
        s_crv = NULL;
        s_q = NULL;
        s_bm_n = s_rv_n = s_best_n = s_crv_n = s_q_n = 0;
        s_crv_cap = 0;
        s_bm_synckey = s_rv_synckey = 0;
        s_book[0] = 0;
    }
}

#define WRN_REV_PAGE       20   /* 单句按需拉取时，每个 range 最多取的公开段评条数（用户要求 ≤20）。 */

/* 把一次 readreviews 回包合并进给定数组，按 reviewId 去重；返回本次落库条数。
 * Merge one readreviews response into the given array, dedup by review id. */
static unsigned merge_reviews(const cJSON *resp, const char *chapter_uid,
                              const char *force_range,
                              wrn_chapter_review_t *arr, size_t *arr_n) {
    unsigned stored = 0;
    const cJSON *r_arr = cJSON_GetObjectItemCaseSensitive(resp, "reviews");
    if (!cJSON_IsArray(r_arr)) return 0;
    const cJSON *entry = NULL;
    cJSON_ArrayForEach(entry, r_arr) {
        const char *range = NULL;
        const cJSON *rj = cJSON_GetObjectItemCaseSensitive(entry, "range");
        if (cJSON_IsString(rj)) range = rj->valuestring;
        /* 单 range 按需拉取：回包可能拆成多个 range 条目，统一锚到请求的 range，避免漏掉多人。 */
        const char *assign_range = force_range ? force_range : range;
        const cJSON *pages = cJSON_GetObjectItemCaseSensitive(entry, "pageReviews");
        if (!cJSON_IsArray(pages)) continue;
        const cJSON *pr = NULL;
        cJSON_ArrayForEach(pr, pages) {
            wrn_chapter_review_t tmp;
            memset(&tmp, 0, sizeof(tmp));
            snprintf(tmp.review_id, sizeof(tmp.review_id), "%u",
                     (unsigned)jnum(pr, "reviewId"));
            snprintf(tmp.chapter_uid, sizeof(tmp.chapter_uid), "%s", chapter_uid);
            if (assign_range) snprintf(tmp.range, sizeof(tmp.range), "%s", assign_range);
            const cJSON *rv = cJSON_GetObjectItemCaseSensitive(pr, "review");
            if (cJSON_IsObject(rv)) {
                jstr(rv, "abstract", tmp.abstract, sizeof(tmp.abstract));
                jstr(rv, "content", tmp.content, sizeof(tmp.content));
                tmp.create_time = jnum(rv, "createTime");
                const cJSON *au = cJSON_GetObjectItemCaseSensitive(rv, "author");
                if (cJSON_IsObject(au)) jstr(au, "name", tmp.author, sizeof(tmp.author));
                else if (cJSON_IsString(au)) jstr(rv, "author", tmp.author, sizeof(tmp.author));
            }
            tmp.likes = jnum(pr, "likesCount");
            if (!tmp.review_id[0] || !tmp.content[0]) continue;
            int slot = -1;
            for (size_t i = 0; i < *arr_n; ++i)
                if (strcmp(arr[i].review_id, tmp.review_id) == 0) { slot = (int)i; break; }
            if (slot < 0) {
                if (!crv_ensure(*arr_n + 1)) return stored;
                slot = (int)(*arr_n)++;
            }
            arr[slot] = tmp;
            ++stored;
        }
    }
    return stored;
}

/* 换章/关书：清空本章段评（只服务当前章）。锚点在磁盘上，不在这里删除。 */
void wrn_crv_clear(void) {
    s_crv_n = 0;
}

/* 渲染侧：从磁盘锚点 r/c<index> 读回本章 abstract 列表，填进 s_crv（伪 id，content 留空）。
 * 离线可用；锚点缺失或损坏时保持空集，不报错、不联网。
 * Render side: load the chapter's abstract list from the on-disk anchor into s_crv.
 * Works offline; a missing/corrupt anchor simply leaves an empty set. */
void wrn_load_anchor_chapter(const char *book_id, const char *chapter_uid, unsigned index) {
    s_crv_n = 0;

    /* 1) 锚点占位条目：每个 range 一条，供虚线 range 反查与句子框（content 留空，不在列表显示）。 */
    wra_entry_t *entries = NULL;
    uint32_t n = 0;
    if (wra_load(book_id, index, &entries, &n)) {
        for (uint32_t i = 0; i < n; ++i) {
            if (!crv_ensure(s_crv_n + 1)) break;
            wrn_chapter_review_t *c = &s_crv[s_crv_n++];
            memset(c, 0, sizeof(*c));
            snprintf(c->review_id, sizeof(c->review_id), "h%u", (unsigned)s_crv_n);
            snprintf(c->chapter_uid, sizeof(c->chapter_uid), "%s", chapter_uid);
            snprintf(c->range, sizeof(c->range), "%s", entries[i].range);
            snprintf(c->abstract, sizeof(c->abstract), "%s", entries[i].abstract);
            c->local_start = entries[i].local_start;
            c->local_end = entries[i].local_end;
        }
        free(entries);
    }
    size_t anchors = s_crv_n;

    /* 2) 书级多人 sidecar：把已持久化的真实想法（含作者/内容/点赞）并入，离线即可见。 */
    wrb_record_t *side = NULL;
    size_t side_n = 0;
    if (wrb_load(book_id, index, &side, &side_n)) {
        for (size_t i = 0; i < side_n; ++i) {
            if (!side[i].content[0] || !side[i].review_id[0]) continue;
            if (!crv_ensure(s_crv_n + 1)) break;
            wrn_chapter_review_t *c = &s_crv[s_crv_n++];
            memset(c, 0, sizeof(*c));
            snprintf(c->review_id, sizeof(c->review_id), "%s", side[i].review_id);
            snprintf(c->chapter_uid, sizeof(c->chapter_uid), "%s", chapter_uid);
            snprintf(c->range, sizeof(c->range), "%s", side[i].range);
            snprintf(c->content, sizeof(c->content), "%s", side[i].content);
            snprintf(c->author, sizeof(c->author), "%s", side[i].author);
            c->likes = side[i].likes;
            c->create_time = side[i].create_time;
        }
        free(side);
    }
    ESP_LOGI(TAG, "anchor loaded ch%u anchors=%u reviews=%u",
             index, (unsigned)anchors, (unsigned)(s_crv_n - anchors));
}


/* 点虚线/选句后，对“这一个 range”在线拉取完整公开段评（≤20 条），合并进内存。
 * On demand after tapping a dashed line: fetch full reviews for the single range. */
bool wrn_fetch_range_reviews(const char *chapter_uid, const char *range) {
    if (!s_book[0] || !chapter_uid || !chapter_uid[0] || !range || !range[0]) return false;

    cJSON *body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "bookId", s_book);
    cJSON_AddNumberToObject(body, "chapterUid", (double)strtoul(chapter_uid, NULL, 10));
    cJSON_AddStringToObject(body, "cht2sMode", "");
    cJSON *revs = cJSON_CreateArray();
    cJSON *one = cJSON_CreateObject();
    cJSON_AddStringToObject(one, "range", range);
    cJSON_AddNumberToObject(one, "maxIdx", 0);
    cJSON_AddNumberToObject(one, "count", WRN_REV_PAGE);
    cJSON_AddNumberToObject(one, "synckey", 0);
    cJSON_AddItemToArray(revs, one);
    cJSON_AddItemToObject(body, "reviews", revs);
    char *body_text = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    if (!body_text) return false;

    size_t resp_len = 0;
    char *resp_text = wr_consent_app_call_quick("/book/readreviews", body_text, &resp_len);
    free(body_text);
    if (!resp_text) {
        ESP_LOGW(TAG, "range readreviews failed chapter=%s range=%s", chapter_uid, range);
        return false;
    }
    cJSON *resp = cJSON_Parse(resp_text);
    free(resp_text);
    if (!resp) { ESP_LOGW(TAG, "range readreviews non-JSON"); return false; }
    /* 段评缓冲懒分配：merge_reviews 每加一条都先 crv_ensure，首次 64 条起，空回包不占内存。 */
    unsigned got = merge_reviews(resp, chapter_uid, range, s_crv, &s_crv_n);
    cJSON_Delete(resp);
    ESP_LOGI(TAG, "range reviews chapter=%s range=%s got=%u", chapter_uid, range, got);
    return got > 0;
}

/* ------------------------------------------------------------------ */
/* 本地改动入队                                                         */
/* ------------------------------------------------------------------ */

static bool queue_push(const wrn_queue_item_t *item) {
    if (s_q_n >= WRN_QUEUE_MAX) { ESP_LOGW(TAG, "queue full"); return false; }
    s_q[s_q_n++] = *item;
    save_queue();
    return true;
}

bool wrn_add_review(const char *chapter_uid, const char *range, const char *content,
                    const char *abstract, bool is_private) {
    if (!s_book[0] || !content || !content[0]) return false;
    if (s_rv_n >= WRN_REVIEW_MAX) return false;
    wrn_review_t *r = &s_rv[s_rv_n++];
    memset(r, 0, sizeof(*r));
    snprintf(r->review_id, sizeof(r->review_id), "local-%lld", (long long)esp_timer_get_time());
    if (chapter_uid) snprintf(r->chapter_uid, sizeof(r->chapter_uid), "%s", chapter_uid);
    if (range) snprintf(r->range, sizeof(r->range), "%s", range);
    snprintf(r->content, sizeof(r->content), "%s", content);
    if (abstract) snprintf(r->abstract, sizeof(r->abstract), "%s", abstract);
    r->is_private = is_private ? 1 : 0;
    r->flags = WRN_F_DIRTY;
    wrn_queue_item_t q = {0};
    q.kind = WRN_Q_ADD_REVIEW;
    snprintf(q.id, sizeof(q.id), "%s", r->review_id);
    snprintf(q.chapter_uid, sizeof(q.chapter_uid), "%s", r->chapter_uid);
    snprintf(q.range, sizeof(q.range), "%s", r->range);
    snprintf(q.text, sizeof(q.text), "%s", r->content);
    snprintf(q.abstract, sizeof(q.abstract), "%s", r->abstract);
    q.is_private = r->is_private;
    if (!queue_push(&q)) return false;
    save_mine();
    return true;
}

/* ------------------------------------------------------------------ */
/* 查询                                                                */
/* ------------------------------------------------------------------ */

static bool chapter_match(const char *a, const char *b) {
    return a[0] && b[0] && !strcmp(a, b);
}

size_t wrn_bookmark_count_chapter(const char *chapter_uid) {
    size_t n = 0;
    for (size_t i = 0; i < s_bm_n; ++i)
        if (chapter_match(s_bm[i].chapter_uid, chapter_uid)) ++n;
    return n;
}

const wrn_bookmark_t *wrn_bookmark_at_chapter(const char *chapter_uid, size_t ordinal) {
    size_t n = 0;
    for (size_t i = 0; i < s_bm_n; ++i) {
        if (!chapter_match(s_bm[i].chapter_uid, chapter_uid)) continue;
        if (n++ == ordinal) return &s_bm[i];
    }
    return NULL;
}

size_t wrn_review_count_chapter(const char *chapter_uid) {
    size_t n = 0;
    for (size_t i = 0; i < s_rv_n; ++i)
        if (chapter_match(s_rv[i].chapter_uid, chapter_uid)) ++n;
    return n;
}

const wrn_review_t *wrn_review_at_chapter(const char *chapter_uid, size_t ordinal) {
    size_t n = 0;
    for (size_t i = 0; i < s_rv_n; ++i) {
        if (!chapter_match(s_rv[i].chapter_uid, chapter_uid)) continue;
        if (n++ == ordinal) return &s_rv[i];
    }
    return NULL;
}

size_t wrn_best_count_chapter(const char *chapter_uid) {
    size_t n = 0;
    for (size_t i = 0; i < s_best_n; ++i)
        if (chapter_match(s_best[i].chapter_uid, chapter_uid)) ++n;
    return n;
}

const wrn_best_t *wrn_best_at_chapter(const char *chapter_uid, size_t ordinal) {
    size_t n = 0;
    for (size_t i = 0; i < s_best_n; ++i) {
        if (!chapter_match(s_best[i].chapter_uid, chapter_uid)) continue;
        if (n++ == ordinal) return &s_best[i];
    }
    return NULL;
}

size_t wrn_crv_count_chapter(const char *chapter_uid) {
    size_t n = 0;
    for (size_t i = 0; i < s_crv_n; ++i)
        if (chapter_match(s_crv[i].chapter_uid, chapter_uid)) ++n;
    return n;
}

const wrn_chapter_review_t *wrn_crv_at_chapter(const char *chapter_uid, size_t ordinal) {
    size_t seen = 0;
    for (size_t i = 0; i < s_crv_n; ++i) {
        if (!chapter_match(s_crv[i].chapter_uid, chapter_uid)) continue;
        if (seen == ordinal) return &s_crv[i];
        ++seen;
    }
    return NULL;
}

/* 锚点占位条目（review_id 以 'h' 开头）按锚点顺序排在 s_crv 前部；按锚点序号取其 range。 */
const char *wrn_anchor_range_at(size_t ordinal) {
    size_t seen = 0;
    for (size_t i = 0; i < s_crv_n; ++i) {
        if (s_crv[i].review_id[0] != 'h') continue;
        if (seen == ordinal) return s_crv[i].range;
        ++seen;
    }
    return NULL;
}

