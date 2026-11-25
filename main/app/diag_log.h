/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：诊断日志。挂载 ESP_LOG 输出，写进内存环形缓冲并落盘（双文件轮转，跨重启保留）。
 * 出问题时在设置里"导出诊断日志"，内容从串口分块打印，用 serial_listen 即可抓回。
 *
 * English: Diagnostic logging. Hooks ESP_LOG output into an in-RAM ring buffer and a rotating
 * pair of files (kept across reboots). "Export diagnostic log" in Settings prints everything over
 * the serial port, captured with serial_listen.
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif

/* 早期初始化：挂载日志捕获、打开落盘文件、启动后台 flush 任务。可在 app_main 最开头调用。
 * Early init: hook logging, open the backing file and start the background flush task. */
void diag_log_init(void);

/* 立即把 ring 落盘，然后把 ring 与两个日志文件从串口分块打印。
 * Flush now, then print the ring and both log files over the serial port. */
void diag_log_dump_uart(void);

/* 清空 ring 与落盘文件。/ Clear the ring and backing files. */
void diag_log_clear(void);

/* USB 读卡接管 TF 卡前挂起（排空并关闭落盘文件），本机重挂后恢复。
   Suspend before USB MSC takes the SD (drain and close the file), resume after the local remount. */
void diag_log_suspend(void);
void diag_log_resume(void);

/* 导出可通过电脑 USB 传输的 .log 包：先 flush，再把 NVS 统计、旧日志、当前日志合并写到
 * SD 卡根目录的 diag_export.log（固定文件名，每次覆盖）。用户在电脑上打开 SD 卡根目录即可
 * 看到并拖走。成功返回 true。
 * Export a computer-readable .log package: flush first, then merge NVS stats and both log
 * files into /sdcard/diag_export.log at the card root so it is visible over USB MSC. */
bool diag_log_export(void);

/* 导出包的完整路径（供界面提示用户）。/ Full path of the exported package. */
const char *diag_log_export_path(void);

#ifdef __cplusplus
}
#endif
