/* SPDX-FileCopyrightText: 2026 YIYO-Designer */
/* SPDX-License-Identifier: LicenseRef-Kiikoread-Personal-Study-1.0 */

#include "weread_browse.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_rom_crc.h>

#include "weread_internal.h"

#define WRB_MAGIC        "WRBROWSE1"
#define WRB_MAGIC_LEN    9u
/* v6：改由 readreviews 按 range 持久化多人；让全部旧 sidecar（含书级端点写的空文件）失效并重建。 */
#define WRB_VERSION      6u
#define WRB_PSRAM        (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)

/* 固定 21 字节文件头：magic[9] + version/count/reserved 各 4 字节。
   magic 长度 9 会让结构体在 4 字节对齐平台上产生填充，故不 overlay 结构体，
   统一用显式字节读写，保证各平台落盘布局确定。 */
#define WRB_HEADER_BYTES  (WRB_MAGIC_LEN + 12u)

static const char *TAG = "WRBROWSE";

/* ---- 小端读写 ---- */

static void put_u16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put_u32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static uint16_t get_u16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t get_u32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* ---- 文件头（依赖上面的小端读写）---- */

static void header_write(uint8_t *dst, uint32_t count) {
    memcpy(dst, WRB_MAGIC, WRB_MAGIC_LEN);
    size_t at = WRB_MAGIC_LEN;
    put_u32(dst + at, WRB_VERSION); at += 4u;
    put_u32(dst + at, count);       at += 4u;
    put_u32(dst + at, 0);           at += 4u;
}

/* 校验 magic/version/reserved，读出 count；任何一项不合法返回 false。 */
static bool header_read(const uint8_t *src, uint32_t *count) {
    if (memcmp(src, WRB_MAGIC, WRB_MAGIC_LEN) != 0) return false;
    size_t at = WRB_MAGIC_LEN;
    uint32_t version = get_u32(src + at); at += 4u;
    *count = get_u32(src + at);            at += 4u;
    uint32_t reserved = get_u32(src + at);
    return version == WRB_VERSION && reserved == 0;
}

/* ---- 路径 ---- */

static bool side_dir(const char *book_id, char *out, size_t cap) {
    char root[WR_PATH];
    if (!wr_stage_root(root, sizeof(root), book_id)) return false;
    int n = snprintf(out, cap, "%s/r", root);
    return n > 0 && (size_t)n < cap;
}

static bool side_path(const char *book_id, unsigned index, char *out, size_t cap) {
    char dir[WR_PATH];
    if (!side_dir(book_id, dir, sizeof(dir))) return false;
    int n = snprintf(out, cap, "%s/c%u.reviews", dir, index);
    return n > 0 && (size_t)n < cap;
}

static bool ensure_dir(const char *path) {
    if (mkdir(path, 0775) == 0) return true;
    return errno == EEXIST;
}

/* ---- 原子写收尾（FAT 先删目标再 rename）；流式写成功后调用 ---- */

static bool side_commit(const char *path, const char *tmp) {
    if (unlink(path) != 0 && errno != ENOENT) { unlink(tmp); return false; }
    if (rename(tmp, path) != 0) { unlink(tmp); return false; }
    return true;
}

/* ---- 单条记录序列化（变长），返回字节数，0 表示失败 ---- */

static size_t append_lp_string(uint8_t *out, size_t at, size_t cap, const char *s, size_t max_chars) {
    size_t len = s ? strlen(s) : 0;
    if (len > max_chars) len = max_chars;
    if (at + 2u + len > cap) return 0;
    put_u16(out + at, (uint16_t)len);
    if (len) memcpy(out + at + 2u, s, len);
    return 2u + len;
}

static size_t serialize_record(const wrb_record_t *r, uint8_t *out, size_t cap) {
    size_t at = 0, step;
    if (!(step = append_lp_string(out, at, cap, r->review_id, WRN_ID - 1u))) return 0;
    at += step;
    if (!(step = append_lp_string(out, at, cap, r->range, WRN_RANGE - 1u))) return 0;
    at += step;
    if (!(step = append_lp_string(out, at, cap, r->author, WRN_AUTHOR - 1u))) return 0;
    at += step;
    if (!(step = append_lp_string(out, at, cap, r->content, WRN_TEXT - 1u))) return 0;
    at += step;
    if (at + 8u > cap) return 0;
    put_u32(out + at, r->likes); at += 4u;
    put_u32(out + at, r->create_time); at += 4u;
    return at;
}

/* 读回一条长度前缀字符串（带边界校验），返回消费字节数，0 表示格式错误。 */
static size_t read_lp_string(const uint8_t *src, size_t at, size_t total, char *out, size_t cap) {
    if (at + 2u > total) return 0;
    uint16_t len = get_u16(src + at);
    if (at + 2u + len > total || (size_t)len >= cap) return 0;
    if (len) memcpy(out, src + at + 2u, len);
    out[len] = '\0';
    return 2u + len;
}

/* ---- 写单章 sidecar：直接序列化已收集好的记录（去重/每 range 上限由调用方保证） ---- */

bool wrb_store_records(const char *book_id, unsigned index, const wrb_record_t *records, size_t n) {
    if (!book_id || !book_id[0] || n > WRB_CHAPTER_CAP) {
        ESP_LOGW(TAG, "store rejected ch%u n=%u", index, (unsigned)n);
        return false;
    }

    char dir[WR_PATH], path[WR_PATH], tmp[WR_PATH + 8];
    if (!side_dir(book_id, dir, sizeof(dir)) || !ensure_dir(dir) ||
        !side_path(book_id, index, path, sizeof(path))) {
        ESP_LOGW(TAG, "store path failed ch%u", index);
        return false;
    }
    if (snprintf(tmp, sizeof(tmp), "%s.tmp", path) >= (int)sizeof(tmp)) return false;

    /* 单条记录的最大可能尺寸，复用为流式写工作缓冲（不再为整章申请大缓冲）。 */
    const size_t rec_cap = WRN_ID + WRN_RANGE + WRN_AUTHOR + WRN_TEXT + 24u;
    uint8_t *rbuf = heap_caps_malloc(rec_cap, WRB_PSRAM);
    if (!rbuf) { ESP_LOGW(TAG, "store rbuf alloc failed ch%u", index); return false; }

    FILE *f = fopen(tmp, "wb");
    if (!f) { free(rbuf); ESP_LOGW(TAG, "store open failed ch%u", index); return false; }

    bool ok = true;
    /* 写头部，CRC 从头部起算。 */
    header_write(rbuf, (uint32_t)n);
    uint32_t crc = esp_rom_crc32_le(0, rbuf, WRB_HEADER_BYTES);
    if (fwrite(rbuf, 1, WRB_HEADER_BYTES, f) != WRB_HEADER_BYTES) ok = false;

    /* 逐条序列化、增量 CRC、直接落盘。 */
    for (size_t i = 0; ok && i < n; ++i) {
        size_t s = serialize_record(&records[i], rbuf, rec_cap);
        if (!s) { ok = false; break; }
        crc = esp_rom_crc32_le(crc, rbuf, (uint32_t)s);
        if (fwrite(rbuf, 1, s, f) != s) ok = false;
    }
    if (ok) {
        uint8_t crcbuf[4];
        put_u32(crcbuf, crc);
        if (fwrite(crcbuf, 1, sizeof(crcbuf), f) != sizeof(crcbuf)) ok = false;
    }
    if (ok && (fflush(f) || fsync(fileno(f)))) ok = false;
    if (fclose(f)) ok = false;
    free(rbuf);

    if (!ok) { unlink(tmp); ESP_LOGW(TAG, "store write failed ch%u", index); return false; }
    if (!side_commit(path, tmp)) { ESP_LOGW(TAG, "store commit failed ch%u", index); return false; }

    ESP_LOGI(TAG, "stored %u reviews ch%u", (unsigned)n, index);
    return true;
}

bool wrb_load(const char *book_id, unsigned index, wrb_record_t **out, size_t *count) {
    if (out) *out = NULL;
    if (count) *count = 0;
    char path[WR_PATH];
    if (!side_path(book_id, index, path, sizeof(path))) return false;

    FILE *f = fopen(path, "rb");
    if (!f) return false;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return false; }
    long size = ftell(f);
    if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); return false; }
    long min_size = (long)(WRB_HEADER_BYTES + sizeof(uint32_t));
    if (size < min_size) { fclose(f); return false; }

    uint8_t *buf = heap_caps_malloc((size_t)size, WRB_PSRAM);
    if (!buf) { fclose(f); return false; }
    bool read_ok = fread(buf, 1, (size_t)size, f) == (size_t)size;
    fclose(f);
    if (!read_ok) { free(buf); return false; }

    size_t payload = (size_t)size - sizeof(uint32_t);
    uint32_t stored_crc = get_u32(buf + payload);
    if (esp_rom_crc32_le(0, buf, (uint32_t)payload) != stored_crc) { free(buf); return false; }

    uint32_t hdr_count = 0;
    if (!header_read(buf, &hdr_count) || hdr_count > WRB_CHAPTER_CAP) { free(buf); return false; }

    /* 按头部声明条数精确分配（空章也至少 1 档，避免零长度分配）。 */
    wrb_record_t *records = heap_caps_calloc(hdr_count ? hdr_count : 1u,
                                             sizeof(wrb_record_t), WRB_PSRAM);
    if (!records) { free(buf); return false; }
    size_t at = WRB_HEADER_BYTES, n = 0;
    bool parse_ok = true;
    while (at < payload) {
        if (n >= hdr_count) { parse_ok = false; break; }
        wrb_record_t *r = &records[n];
        size_t step;
        if (!(step = read_lp_string(buf, at, payload, r->review_id, sizeof(r->review_id)))) { parse_ok = false; break; }
        at += step;
        if (!(step = read_lp_string(buf, at, payload, r->range, sizeof(r->range)))) { parse_ok = false; break; }
        at += step;
        if (!(step = read_lp_string(buf, at, payload, r->author, sizeof(r->author)))) { parse_ok = false; break; }
        at += step;
        if (!(step = read_lp_string(buf, at, payload, r->content, sizeof(r->content)))) { parse_ok = false; break; }
        at += step;
        if (at + 8u > payload) { parse_ok = false; break; }
        r->likes = get_u32(buf + at); at += 4u;
        r->create_time = get_u32(buf + at); at += 4u;
        ++n;
    }

    free(buf);
    /* 文件头声明的条数与实际解析条数必须一致，否则视为损坏。 */
    if (!parse_ok || n != (size_t)hdr_count) { free(records); return false; }

    if (n == 0) {
        free(records);
        *out = NULL;
        *count = 0;
        return true;
    }
    *out = records;
    *count = n;
    return true;
}

bool wrb_exists(const char *book_id, unsigned index) {
    char path[WR_PATH];
    if (!side_path(book_id, index, path, sizeof(path))) return false;
    struct stat st;
    return stat(path, &st) == 0 && st.st_size > 0;
}

/* sidecar 是否为当前版本（magic/version 校验）：旧版本文件据此判失效并触发重抓。 */
bool wrb_is_current(const char *book_id, unsigned index) {
    char path[WR_PATH];
    if (!side_path(book_id, index, path, sizeof(path))) return false;
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    uint8_t hdr[WRB_HEADER_BYTES];
    bool read_ok = fread(hdr, 1, sizeof(hdr), f) == sizeof(hdr);
    fclose(f);
    if (!read_ok) return false;
    uint32_t count = 0;
    return header_read(hdr, &count);
}

bool wrb_drop(const char *book_id, unsigned index) {
    char path[WR_PATH];
    if (!side_path(book_id, index, path, sizeof(path))) return false;
    if (unlink(path) != 0 && errno != ENOENT) return false;
    return true;
}
