#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* 离线时长授权（consent）：独立于网页版 cookie 的官方微信 OAuth 通道。
 * Offline-duration consent: the official WeChat OAuth channel, separate from the web cookie. */

typedef enum {
    WR_CONSENT_IDLE,        /* 未开始 */
    WR_CONSENT_PREPARING,   /* 正在获取二维码 */
    WR_CONSENT_WAIT_SCAN,   /* 二维码已生成，等待微信扫码 */
    WR_CONSENT_SCANNED,     /* 已扫码，等待手机确认 */
    WR_CONSENT_OK,          /* 授权成功 */
    WR_CONSENT_EXPIRED,     /* 二维码过期 */
    WR_CONSENT_DECLINED,    /* 用户在微信里拒绝 */
    WR_CONSENT_FAILED,      /* 网络或其他失败 */
} wr_consent_state_t;

typedef struct {
    wr_consent_state_t state;
    char qr_url[256];       /* 二维码内容（微信 confirm 链接） */
    char message[128];      /* 可展示的失败/状态原因 */
} wr_consent_status_t;

/* 已有有效授权凭证。/ True when usable consent credentials are stored. */
bool wr_consent_authorized(void);

/* 取授权账号的 vid（写入 out，最多 out_size-1 字节）；未授权返回 false。
 * Return the authorised account vid; false when no credentials are stored. */
bool wr_consent_vid(char *out, size_t out_size);

/* 一次取 consent 三件套（vid/accessToken/refreshToken），供把 consent 授权换成网页会话。
   缓冲不足或未授权返回 false。 */
bool wr_consent_tokens(char *vid, size_t vid_sz,
                       char *access, size_t access_sz,
                       char *refresh, size_t refresh_sz);

/* 启动授权任务（内部专用任务跑完整 OAuth，不阻塞 UI）。
 * Start the consent task; it runs the whole OAuth flow without blocking the UI. */
void wr_consent_begin(void);
/* 取消授权任务。/ Cancel the consent task. */
void wr_consent_cancel(void);
/* 读取当前授权状态（供 UI 刷新）。/ Read the current status for the UI. */
void wr_consent_poll(wr_consent_status_t *out);

/* 清除已保存的授权凭证。/ Forget stored consent credentials. */
void wr_consent_logout(void);

/* 通过官方 /book/read 上报一条离线时长；成功表示服务器已确认。
 * Upload one offline-duration record via the official /book/read; true means the server confirmed. */
bool wr_consent_upload(const char *book_id, const char *owner, uint32_t seconds,
                       const char *chapter_id, uint32_t chapter_uid, uint32_t offset,
                       bool resend, uint32_t chapter_idx, uint32_t pct);

/* 初始化（启动时恢复凭证）。/ Initialize and restore credentials at boot. */
void wr_consent_init(void);

/* App 协议（i.weread.qq.com）认证请求：body 非 NULL 走 POST，NULL 走 GET。
 * 返回 PSRAM 响应体（调用方 free），失败返回 NULL。
 * Authenticated App-protocol call: POST with a body, GET without; returns a PSRAM response. */
char *wr_consent_app_call(const char *path, const char *body, size_t *out_len);

/* 同上，但只试一次、超时 12 秒：段评等"拉不到就算了"的可选请求用它，避免一次 40 秒阻塞。
 * Same as above but one attempt with a 12 s timeout, for optional reviews. */
char *wr_consent_app_call_quick(const char *path, const char *body, size_t *out_len);

/* 联网且本机已有凭证时，在后台确保 accessToken 有效（必要时用 refreshToken 自动续期）。
 * 不阻塞 UI，内部带防重入与节流，可在页面 tick 中反复调用。
 * When online and credentials exist, refresh the access token in a background task if needed.
 * Non-blocking, internally guarded and throttled; safe to call repeatedly from a page tick. */
void wr_consent_ensure(void);
