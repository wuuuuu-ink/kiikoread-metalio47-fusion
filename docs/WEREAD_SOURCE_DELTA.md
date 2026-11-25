<!-- SPDX-FileCopyrightText: 2026 YIYO-Designer -->
<!-- SPDX-License-Identifier: LicenseRef-Kiikoread-Personal-Study-1.0 -->

# 微信读书缓存：相对 GitHub 原版的源码改动梳理

基线是 GitHub 发行版 `6ec1b45`（tag `metalio-s31-0.3.3-rc113-final`）。本文逐项说明当前工作树相对该原版做了什么、为什么做、以及每个开关现在的位置。

## 0. 差异总览

| 文件 | 增删行 | 改动性质 |
| --- | ---: | --- |
| `main/book/weread.c` | +474 / −239 | gzip 流式解压、HTTP 连接池、重试与超时参数 |
| `main/book/weread_epub.c` | +599 / −17 | 常驻预取、按章 tar 资源包、缓存流程重组 |
| `main/apps/app_weread.c` | +127 / −13 | 进度的两档刷新实现、收尾帧竞争修复 |
| `main/apps/app_transfer.c` | +26 / −0 | 射频重初始化前释放长连接 |
| `main/book/weread_protocol.c` | +29 / −0 | `ps` 合成与复用 API（当前未启用） |
| `main/app/app_loop.c` | +19 / −1 | `on_tick` 门控诊断告警 |
| `main/book/weread.h` | +14 / −0 | 章节结构新增 `tar` 字段、`wr_http_release()` |
| `main/book/weread_internal.h` | +2 / −0 | 新增两个 reader 构造函数声明 |

合计 **+1290 / −270**。原版里 `PREFETCH`、`tar_fill_slots`、`s_http_pool`、`PROGRESS_AREA`、`gzip` 这些关键词的命中数**全部为 0**，即下列机制在 GitHub 版中一个都不存在。

## 1. gzip 流式解压 reader 页

`main/book/weread.c:714-1000`

原版每章都要完整下载约 170 KB 的明文 HTML reader 页。现在只在 chunked 路径上请求压缩体：

```c
/* weread.c:828 */
esp_http_client_set_header(http, "Accept-Encoding", chunk ? "gzip" : "identity");
```

判定依据是**响应体魔数**而不是 `Content-Encoding` 头（`weread.c:978`）：

```c
if (n >= 2 && (uint8_t)data[0] == 0x1f && (uint8_t)data[1] == 0x8b) {
    inflateInit2(&gz_stream, 16 + MAX_WBITS)
```

解压器只在真的收到压缩响应时才建立，解压出的数据流式喂给字段扫描回调（`gz_feed_scanner`），不落地整页缓冲。实测 `wire_bytes` 从约 170 KB 降到 96–100 KB。

## 2. 按章 tar 资源包

`main/book/weread_epub.c:486`（`tar_fill_slots()`）、`main/book/weread.h`（`wr_chapter_t.tar[192]`）

原版逐张图请求 `wrepub/epub_<book>_<n>`，一章十几张图就是十几次往返。章节目录接口的 `chapterInfos` 里本来就带一个 `tar` 字段，指向本章全部插图的打包地址，现在直接用它：

```c
/* weread.h */
char tar[192];   /* 一章一个 tar，包里就是这一章的全部插图 */
```

`tar_fill_slots()` 一次拉到整包后按槽位填图。实测 tar 通道吞吐 **347 KB/s**，图片解包 18–63 ms。

## 3. 常驻预取任务

`main/book/weread_epub.c:40-41`、`115-180`

```c
#ifndef WR_PREFETCH_CHAPTER
#define WR_PREFETCH_CHAPTER 1
#endif
```

一个常驻任务，栈分配在 PSRAM（`xTaskCreateWithCaps`），只通过 `job` / `done` 两个二值信号量同步。主任务在 tar 下载期间把下一章的 reader 页与正文喂满，把 HTTP 串行工作量压进下载的墙钟时间里。

日志里每章带 `prefetched=0/1` 标记，实测命中 **30/32** 到 **211/213**。

关键取舍：任务常驻不销毁。每本书建一次再删，会在收尾处和正在退出的任务抢信号量。

## 4. HTTP 连接池

`main/book/weread.c:372-500`

原版只有一个 `s_http_keepalive` 句柄（`weread.c` 原 418-422 行）加每次请求新建 client。现在是三槽按 host 复用的池：

```c
#define WR_HTTP_POOL_SLOTS 3
#define WR_POOL_IDLE_US (20LL * 1000LL * 1000LL)
```

- **按 host 匹配**：`strcasecmp(host, s_http_pool[i].host)`，只有同 host 才能复用
- **空闲过期**：超过 `WR_POOL_IDLE_US`（20 s）的连接主动淘汰，避免拿到服务端已关闭的 socket
- **双层结构**：`http_request_once()` 负责单次收发，`http_request()` 在外层做重定向与重试
- **失效兜底**：非流式请求失败时换一条新连接重试一次

实测这条兜底有效：全程 8 次 `read failed code=-1` 全部靠它恢复，每次都是 `reused=0 code=200`。

超时参数（`weread.c:678-683`）：

| 宏 | 值 | 用途 |
| --- | ---: | --- |
| `WR_HTTP_BODY_TIMEOUT_MS` | 8000 | 响应体阶段的 socket 超时 |
| `WR_HTTP_STALL_WINDOW_US` | 6 s | 停滞判定窗口 |
| `WR_HTTP_STALL_MIN_BYTES` | 24576 | 窗口内低于此字节数视为停滞 |

## 5. 射频档位

`components/read_pico_transfer/transfer_s31_radio_profile.h`

原版走默认档。现在固定为 thrifty：

```
static_rx=3  static_tx=3  dynamic_rx=64  ampdu=0  ba=12
```

启动日志会打印这一行，可用来确认烧进去的是哪一档。**不要换回 AMPDU 平衡档**：实测 RX6/TX3/DYN64/AMPDU 开会从 352 KB/s 退到 277 KB/s。真正的瓶颈是 dynamic RX 的 64 个槽，不是 AMPDU。

## 6. 进度的两档刷新

`main/apps/app_weread.c:55-74`、`467-494`、`615-647`

```c
#ifndef WR_PROGRESS_AREA
#define WR_PROGRESS_AREA 1      /* 当前档：局部平滑 */
#endif
```

| | `0` 整屏档 | `1` 局部档（当前） |
| --- | --- | --- |
| 触发 | 跨 5% 才重绘 | 每章 `done/total` 变化，250 ms 限频 |
| 推屏 | `APP_REDRAW_PAGE`，整屏 48 帧 580–700 ms，带反相闪屏 | `APP_REDRAW_AREA`，460×84 块，`MODE_DU` 约 100 ms，无反相 |
| 面板耗时 | 32 章约 13.1 s | 每章约 100 ms，另每 16 次补一次 GL16 沉淀（650 ms） |
| 观感 | 每约 2 章闪一下，进度条跳格 | 逐章平滑推进，全程不闪 |

局部档的两个支撑件：`present()` 负责把 tick 已经画好的进度块推上屏，`area_hint()` 告诉框架这次重绘只涉及 `{112,478,460,84}`。两者在 `#if WR_PROGRESS_AREA` 分支里注册：

```c
#if WR_PROGRESS_AREA
    .present=present, .area_hint=area_hint,
#endif
```

节流基准必须是**已经画出去的百分比** `s_drawn_pct`，不能跟上一次 tick 的进度比。每章只推进约 3%，跟上一 tick 比永远跨不过 5% 门槛，进度条会一次都不重绘——这个 bug 真实出现过。

## 7. 收尾帧竞争修复

`main/apps/app_weread.c:482-493`

整屏档下进度分支**故意跳过最后一格**，把 100% 交给完成分支：

```c
/* 以前两边都画，两次整屏只隔 63 ms，后一帧抢不到面板被丢，
 * 屏幕就永久停在进度页。 */
const unsigned step = total ? done * 100u / total : 0;
if (step < s_drawn_pct + 5u) return APP_REDRAW_NONE;
```

## 8. on_tick 门控诊断与射频互斥

`main/app/app_loop.c:1735-1762`

`on_tick` 是唯一能自己发起重绘的通道。原版那道门任意一项为真时静默跳过，后台任务跑完界面就不动，直到用户点一下——实测卡过 125 秒。现在改成先算 `tick_gated`，被挡住时打一条限频 5 s 的告警，把具体是哪一项挂住暴露出来：

```
W (24418) APP: on_tick gated power=0 quick=0 quick_owned=0 feedback=1 menu=0 req_app=1 req_menu=0 req_ret=0
```

`main/apps/app_transfer.c` 加了 4 处 `wr_http_release()`，位置是：网络连接、Wi-Fi 扫描、热点/传输服务启动之前。原因有二——Wi-Fi 驱动 deinit 后池里的 socket 已是野句柄；每条 pooled TLS 连接占几 KB 内部 RAM，而 Wi-Fi 初始化在 75 KB 空闲时都可能分配失败。另外扫网前加了 `wr_network_busy()` 判断，微信读书还在下载时先别起射频，等任务收尾下一轮再扫。

## 9. 未启用的残留：ps 复用

`main/book/weread_protocol.c:135-163`

新增了两个函数：

- `wr_reader_synthetic()` —— 合成一个比 `pc` 早 600 s 的 `ps`，省掉每章一次 reader 页下载
- `wr_reader_from_ps()` —— 复用真实 reader 页拿到的 `ps`

**当前未启用**，日志会打印 `cache reader reuse hits=0 rejects=0 enabled=0`。实测服务端对复用的 `ps` 会 7.4 s 超时，两次命中净亏 12 s，所以默认关闭。函数保留是为将来换账号或服务端策略变动时能快速重开。

## 10. 已知遗留问题：空章节导致整本作废

`main/book/weread_protocol.c:254-261`

```c
char *data = wr_http(epub[0], json, reader->referer, limit, &n);
...
if (!shard_valid(data, n)) { free(data); return NULL; }
```

`shard_valid()` 要求 `length > 33`（32 字节 MD5 头 + 至少 1 字节内容）。当服务端对某章返回 HTTP 200 但正文只有 2 字节（`[]`）时，这一章被判为非法，重试 3 次结果相同，最终 `package_book()` 中断，`wr_cache()` 在 `weread_epub.c:1082` 把整本临时文件 `unlink` 掉——已下完的章节全部删除。

2026-11-16 实测出现过两种形态，不是同一个原因：

| | 第 215 章「画眉」 | 第 109 章「少年有话说」 |
| --- | --- | --- |
| `e_0` 响应体 | 2 字节（`[]`） | 49 字节（JSON 错误体） |
| 重试结果 | 4 次全失败 | 4 次全失败 |
| 同时的 reader 页 | 正常 | 缩到 9096 字节（正常 90–100 KB） |

第 54 章也曾撞上 49 字节，但第三次重试恢复了正常（`declared=13146`），说明这类短响应**不一定是永久失败**，也可能只是服务端在连续请求下给了降级响应。所以"214 章免费试读边界"的解释站不住——同一本书第 109 章就触发，权限边界不会换位置。

错误文案 `微信读书接口返回错误` 来自 `weread.c:940` 的通用兜底 `code < 200 || code >= 300`，其中 `code=-1` 的网络失败也会命中，因此容易误判为网络故障。实际情况是 HTTP 层 200 成功、业务体为空。

**诊断已加，跳过未加。** `shard_reject()` 会在分片校验失败时打印真实响应体（可打印 ASCII、上限 200 字节），`cache retry` 打印每次重试的 attempt 与退避，用来区分"限流 / 下架 / 会话失效"。日志里现在会出现：

```
W WEREAD: shard reject endpoint=/web/book/chapter/e_0 bytes=49 body={"errcode":...}
W WEREAD: cache retry index=109 attempt=1 delay_ms=400 error=微信读书接口返回错误
```

重试退避仍是 `400 ms × (attempt+1)`，总共不到 1.5 s——若是限流，这个退避偏急，四次会撞在同一窗口里。**拿到真实 errcode 之后再决定**是加大退避，还是改成跳过并计数。

## 11. 16 MiB 全闪存固件

按 `docs/FULL_FLASH_16MB.md` 的规范生成，字节数必须精确等于 `16,777,216`，其余地址 `0xFF` 填充。

归档位置：`releases/metalio-s31-16MB/`（已在 `.gitignore` 中忽略，不进版本库）

| 文件 | 大小 | SHA-256 |
| --- | ---: | --- |
| `kiikoread-metalio47-0.3.3-rc113-progress-local-diag-full-flash-16MB.bin`（当前） | 16,777,216 | `6d39709662f9c7038ea7b4580230a941a889643a3fd6e559da687e5026ecf860` |
| `app-progress-local-diag.bin`（当前） | 5,217,568 | `d14f39057ef5569fad9e4b374733bad0a9eb7b90469351cf4c30e57d58554f81` |
| `kiikoread-metalio47-0.3.3-rc113-progress-local-full-flash-16MB.bin`（留档） | 16,777,216 | `c6510941f23b56872b32836365ad4ce7d922e381ebc0259b67a97266216620e3` |
| `app-progress-local.bin`（留档） | 5,217,056 | `930fdbfd4c1c85ff9f546337b91f40159c9c9a575b738c5200b73895fd9995b6` |
| `bootloader.bin` | 22,448 | `98cee3c27a35fab3ac72f97fe9a5ae554db6c10e15957399ed855d30256bc139` |
| `partition-table.bin` | 3,072 | `6e76473023131e9afce369fa87e1871558f344049950565110d9879d4d9304e0` |
| `ota_data_initial.bin` | 8,192 | `7d2c7ac4888bfd75cd5f56e8d61f69595121183afc81556c876732fd3782c62f` |

两版共用同一份 bootloader / 分区表 / ota_data（这两者自 `metalio-s31-0.3.3-rc113-final` 起未变）。

生成命令：

```bash
esptool --chip esp32s31 merge-bin \
  --output kiikoread-metalio47-0.3.3-rc113-progress-local-diag-full-flash-16MB.bin \
  --pad-to-size 16MB --flash-mode dio --flash-freq 80m --flash-size 16MB \
  0x2000 bootloader.bin 0x8000 partition-table.bin 0xd000 ota_data_initial.bin \
  0x10000 app-progress-local-diag.bin
```

烧录（从 `0x0` 起，整颗覆盖）：

```bash
esptool --chip esp32s31 -p /dev/cu.usbmodem1101 -b 115200 --no-stub \
  write-flash --flash-mode dio --flash-size 16MB --flash-freq 80m \
  0x0 kiikoread-metalio47-0.3.3-rc113-progress-local-diag-full-flash-16MB.bin
```

镜像布局：bootloader `0x2000` / 分区表 `0x8000` / OTA 初始化 `0xD000` / 应用 `0x10000`，其余 `0xFF`。
