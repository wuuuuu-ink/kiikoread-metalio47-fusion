/* SPDX-License-Identifier: LicenseRef-Kiikoread-Personal-Study-1.0
 * 中文：按Metalio公开网络文档统一S31运行期策略；不存取网络凭据。
 * English: Apply the documented Metalio S31 runtime profile without touching credentials.
 * Reference: CloudZao/Metalio-E-INK4-Plus@ad8edf4 docs/ap-file-transfer.md (MIT).
 */
#pragma once
#include <stdbool.h>
#include "esp_wifi.h"
/*
 * 单位 0.25 dBm。官方文档给的 48（12 dBm）在近距离传书够用，但换成公网取章包就露馅了：
 * 实测下行每 16 秒停一次（上行 ACK 到不了路由器，下行只能等重传），3 MB 的章包 120 秒只
 * 挪动 573 KB。72（18 dBm）仍在国标 20 dBm EIRP 之内，把上行拉回来是提升吞吐最直接的一刀。
 * Unit is 0.25 dBm. The documented 48 (12 dBm) is fine for a short local transfer, but fetching
 * chapter bundles over the internet exposed it: the downlink stalls every 16 s because uplink
 * ACKs never reach the router and TCP can only retransmit — a 3 MB bundle moved 573 KB in 120 s.
 * 72 (18 dBm) stays inside the 20 dBm EIRP limit and is the most direct fix for that stall.
 */
#define TRANSFER_S31_MAX_TX_POWER 72
// Short STA sessions (network scan, time sync and low-RAM connection starts) do
// not need an AP receive aggregation window. Keep their DMA-backed RX footprint
// small enough for the S31's 64 KiB internal allocation reserve.
static inline void transfer_s31_constrained_sta_config(wifi_init_config_t *init) {
    if (!init) return;
    if (init->static_rx_buf_num > 3) init->static_rx_buf_num = 3;
    if (init->dynamic_rx_buf_num > 16) init->dynamic_rx_buf_num = 16;
    init->ampdu_rx_enable = false;
    /* 一并关掉 TX 聚合：上行块确认在该 AP 上反复 DELBA(reason:39)，重连要内部 DMA，低内存下
     * 直接 mem fail 把 POST 卡死；非聚合帧不依赖 BA 会话。/ Disable TX aggregation too: the
     * uplink BA session flaps (DELBA reason:39) and its re-setup needs internal DMA, which fails
     * under low memory and stalls the POST; plain MPDUs need no BA session. */
    init->ampdu_tx_enable = false;
}

/*
 * 内部 RAM 不够时逐级瘦身。实测这道坎非常窄：内部堆 68,599 B 时 3 个静态 RX 缓冲刚好能起来，
 * 67,063 B（只少 1.5 KB）就只拿到 2 个，而 esp_wifi_init 失败后的 deinit 并不彻底，用同一份
 * 配置重试只会更差（第三次连一个都拿不到），最后整个联网都起不来——扫描、获取目录全挂。
 * 所以按档下调：每档少要一块内存，才有机会真正起来。
 * Step the pool down when internal RAM is short. The cliff is narrow: 3 static RX buffers fit at
 * 68,599 B free and miss by one at 67,063 B (1.5 KB less). A failed esp_wifi_init does not fully
 * deinit, so retrying the identical config only does worse (the third attempt could not get any),
 * and then nothing network-related starts at all — scans and TOC fetches alike.
 * level 0: 只降静态 RX 与管理短缓冲；level 1: 再降动态 RX 与静态 TX（实测 63 KB 能过的档）；
 * level 2: 收到最小可用。
 * level 0 shrinks static RX and the management pool only; level 1 also trims dynamic RX and
 * static TX (measured to pass at 63 KB); level 2 is the minimum the driver accepts.
 */
static inline void transfer_s31_slim_sta_config(wifi_init_config_t *init, int level) {
    if (!init) return;
    /*
     * 2026-11-19 真机回归：以前在这里把 mgmt_sbuf_num 压到 8，结果管理帧本身发不出去——
     * 串口里每一次认证都打印 `wifi:m f auth`（管理帧分配失败），路由器永远收不到 auth，
     * 复现为 reason=2 认证超时；切到热点模式时信标也要从同一个小池子里拿，直接
     * `wifi:alloc eb len=752 type=4 fail` 之后射频库空指针 panic。
     * v2.1c 从来没动过这个值，在同一块板、同样 61 KB 余量下能连上，所以管理短缓冲必须
     * 保持 SDK 默认（16），要省内存只省接收/发送数据缓冲。
     * 2026-11-19 hardware regression: shrinking mgmt_sbuf_num to 8 made management frames
     * themselves fail to allocate — every auth attempt logged `wifi:m f auth`, the router never
     * saw the auth frame (reason=2 auth timeout), and SoftAP beacons taken from the same small
     * pool failed with `alloc eb len=752 type=4 fail` before the radio library panicked on a null
     * pointer. v2.1c never touched this value and connected on the same board with the same
     * ~61 KB headroom, so keep the SDK default 16 and only trim RX/TX data buffers.
     */
    if (init->static_rx_buf_num > 3) init->static_rx_buf_num = 3;
    if (init->static_tx_buf_num > 3) init->static_tx_buf_num = 3;
    if (init->cache_tx_buf_num > 3) init->cache_tx_buf_num = 3;
    if (init->dynamic_rx_buf_num > 12) init->dynamic_rx_buf_num = 12;
    init->ampdu_rx_enable = false;
    init->ampdu_tx_enable = false;   /* 与 constrained 同理：避免上行 BA 抖动在低内存下卡死 TX。 */
    if (level >= 1) {
        if (init->dynamic_rx_buf_num > 8) init->dynamic_rx_buf_num = 8;
    }
    if (level >= 2) {
        if (init->dynamic_rx_buf_num > 4) init->dynamic_rx_buf_num = 4;
        if (init->static_rx_buf_num > 2) init->static_rx_buf_num = 2;
    }
}

/*
 * 最短可用档（2026-11-19）：仅做 NTP 对时/短请求的临时会话，内部 RAM 只有 5x KB 时，
 * 第一次（干净）就用最小配置，避免先失败一次把驱动留在半初始化状态、之后再怎么降档都起不来。
 * mgmt 仍保持 SDK 默认 16（认证帧要靠它），只把数据缓冲压到驱动可接受的下限。
 * Minimal viable profile for a short NTP-only session: chosen on the first (clean) attempt when
 * internal RAM is only ~50 KB, so a failed first attempt never leaves the driver half-initialised.
 * The management pool stays at the SDK default of 16 because auth frames need it.
 */
static inline void transfer_s31_minimal_sta_config(wifi_init_config_t *init) {
    if (!init) return;
    init->static_rx_buf_num = 2;
    init->dynamic_rx_buf_num = 4;
    init->ampdu_rx_enable = false;
    init->ampdu_tx_enable = false;   /* 短会话同样不依赖聚合，避免 BA 抖动触发 mem fail。 */
    /*
     * 短会话（对时/扫描）关 WiFi 省电，不需要缓存 TX 缓冲。关掉 CACHE_TX 特性并把数量置 0，
     * 跳过驱动里那组固定的内部 DMA 缓存 TX 缓冲。
     * 注意：必须同时清特性位，否则驱动在 SPIRAM 下见到 cache_tx=0 会直接 NOT_SUPPORTED。
     * A short NTP/scan session runs with power save off, so cache TX buffers are unused. Clear the
     * feature bit and zero the count to skip the fixed internal-DMA cache TX pool.
     */
    init->feature_caps &= ~(uint64_t)CONFIG_FEATURE_CACHE_TX_BUF_BIT;
    init->cache_tx_buf_num = 0;
    /*
     * 静态 TX 池是驱动在 init 时一次性申请的一整块连续内部 DMA，是低内存下最大的一块。
     * 改成动态 TX（tx_buf_type=1）后，TX 缓冲改为按需分配、可走 PSRAM，init 不再占这块内部 DMA，
     * 给最后一组固定内部缓冲（type 10）让出空间。动态 TX 是 ESP-IDF 常规低内存配置。
     * The static TX pool is one contiguous internal-DMA block requested at init and the largest
     * single chunk under low memory. Switch to dynamic TX so buffers are allocated on demand (and
     * may use PSRAM), freeing the block at init for the last fixed internal buffer.
     */
    init->tx_buf_type = 1;
    init->static_tx_buf_num = 0;
    /* 动态 TX 必须给一个 >0 的数量，否则驱动报 "dynamic tx buf number 0 is out of range"。
     * 按需分配、可走 PSRAM；短会话给 16 槽足够。/ Dynamic TX needs a positive count; lazily
     * allocated (may use PSRAM), 16 slots suffice for a short session. */
    init->dynamic_tx_buf_num = 16;
}

/*
 * 微信读书整本缓存（network_only）主用档，移植自 v2.0 实测最快档：动态 RX 抬到 64 槽，
 * 接收端缓冲周转有回旋余量，实测 15.37MB 资源包跑到 351.6 KB/s。缓存比如级。
 * 2026-11-19 真机实测修正：只抄 v2 的 static_rx/static_tx/dynamic_rx 三项在 v3 上直接
 * ESP_ERR_NO_MEM——v3 内部堆比 v2 少约 7KB，而 init 时最吃连续内部 DMA 的是 CACHE_TX 池
 * （4 槽一整块）和静态 TX 池（6 槽一整块）。串口证据：thrifty 首次 init 就 NO_MEM，
 * 退到 slim level2（cache 仍=3）依旧 NO_MEM，只有把 cache_tx 清零改用动态 TX（=minimal）
 * 才起来。所以这里把 minimal 已经验证过的两条省内存手法一并带上，再按 level 逐级收
 * 动态 RX（64/48/32/24/16），由调用方做成阶梯，哪一级能起来就用哪一级，绝不回落得比
 * 原来更小（原先失败后落到 dynamic=4，比 constrained 的 16 还差，这正是"改了没变化"的原因）。
 * The v2 cache profile: 64 dynamic RX slots. Copying only static_rx/static_tx/dynamic_rx
 * failed NO_MEM on v3, whose internal heap is ~7 KB tighter than v2's: at init the two
 * biggest contiguous internal-DMA blocks are the CACHE_TX pool and the static TX pool.
 * Serial proof: thrifty NO_MEM on the first attempt, slim level2 (cache still 3) NO_MEM,
 * only cache_tx=0 + dynamic TX (= the minimal profile) started. So carry those two proven
 * savings over, and step dynamic RX down per level; the caller tries 64 first and keeps the
 * highest level that fits instead of silently degrading below the old constrained profile.
 */
static inline void transfer_s31_thrifty_sta_config(wifi_init_config_t *init, int level) {
    if (!init) return;
    /* v3 真机定证（2026-11-20）：static_rx=3 会多吃一块 1700B 内部 DMA，init 后 dma largest
     * 只剩 768B，AP 的关联响应无处安放，反复 reason=4（ASSOC_EXPIRE）根本连不上。v2 能扛 3
     * 是因为它运行时多约 7KB 内部余量。v3 固定 2 槽（与 minimal/slim 一致），关联稳定，
     * 批量吞吐靠下面 64 槽动态 RX（在 PSRAM），不损失速度。/ On v3 the third static RX buffer
     * leaves only 768 B of DMA headroom and the assoc response is dropped (reason=4). Pin it to 2
     * like the proven minimal/slim profiles; bulk throughput comes from the 64 dynamic RX slots. */
    init->static_rx_buf_num = 2;
    /* 关掉 CACHE_TX 池与静态 TX 池：两块都是 init 时一次性申请的连续内部 DMA。 */
    /* Drop the cache-TX and static-TX pools: both are one contiguous internal-DMA block. */
    init->feature_caps &= ~(uint64_t)CONFIG_FEATURE_CACHE_TX_BUF_BIT;
    init->cache_tx_buf_num = 0;
    init->tx_buf_type = 1;
    init->static_tx_buf_num = 0;
    if (init->dynamic_tx_buf_num < 16) init->dynamic_tx_buf_num = 16;
    int rx = 64;
    if (level >= 1) rx = 48;
    if (level >= 2) rx = 32;
    if (level >= 3) rx = 24;
    if (level >= 4) rx = 16;
    init->dynamic_rx_buf_num = rx;
    init->ampdu_rx_enable = false;
    /* 关键：TX 聚合也关掉。真机实锤 readreviews POST 卡在 DELBA(reason:39)+mem fail——上行 BA
     * 重建要内部 DMA，而本档只剩几十 KB；非聚合 POST 不依赖 BA，才能把请求真正发出去。
     * Key: disable TX aggregation too. The readreviews POST was proven to stall on DELBA + mem fail:
     * rebuilding the uplink BA needs internal DMA that this profile does not have; a plain POST does
     * not need the BA and can actually be transmitted. */
    init->ampdu_tx_enable = false;
}

static inline esp_err_t transfer_s31_radio_profile(wifi_interface_t interface) {
    esp_err_t err = esp_wifi_set_ps(WIFI_PS_NONE);
    if (err == ESP_OK) err = esp_wifi_set_protocol(interface,
        WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N);
    if (err == ESP_OK) err = esp_wifi_set_max_tx_power(TRANSFER_S31_MAX_TX_POWER);
    return err;
}

// SDK: AMPDU接收池应覆盖BA窗口；只给独占传书补齐，最大16槽，日常STA/BLE不增占用。
// SDK: cover the BA window with static RX buffers; bulk-only, capped at16, retaining normal STA/BLE memory.
static inline void transfer_s31_bulk_receive_config(wifi_init_config_t *init, bool bulk) {
    if (!init || !bulk || !init->ampdu_rx_enable || init->rx_ba_win <= 0 ||
        init->rx_ba_win > 16 ||
        (init->dynamic_rx_buf_num && init->dynamic_rx_buf_num < init->rx_ba_win)) return;
    int window=init->rx_ba_win;
#ifdef CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP
    // 独占传书统一到SDK建议的16槽；原静态池已经够大也不能跳过BA设置。
    // Bulk PSRAM reception uses the SDK's16-slot recommendation even when the static pool was already large.
    if (!init->dynamic_rx_buf_num || init->dynamic_rx_buf_num >= 16)window=16;
#endif
    init->rx_ba_win=window;
    if(init->static_rx_buf_num<window)init->static_rx_buf_num=window;
}
