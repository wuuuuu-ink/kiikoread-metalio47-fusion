<!-- SPDX-FileCopyrightText: 2026 YIYO-Designer -->
<!-- SPDX-License-Identifier: LicenseRef-Kiikoread-Personal-Study-1.0 -->

# Metalio S31 0.3.3-rc113 最终版：16 MiB 全闪存包

此发行版提供针对 Metalio 4.7 V1.5 / ESP32-S31 的 16 MiB 原始闪存镜像。它不是普通应用升级包，只能从闪存地址 `0x0` 写入。

## 下载

打开 [GitHub 最新发行版](https://github.com/YIYO-Designer/kiikoreader-wereader/releases/latest)，下载 `kiikoread-metalio47-0.3.3-rc113-final-full-flash-16MB.bin` 和 `SHA256SUMS.txt`。发行版同时提供 ZIP 压缩包和此说明。

## 注意数据清除

全闪存镜像覆盖整颗 16 MiB 闪存。刷写后设备里的 Wi-Fi 和微信读书账户、阅读进度、内部缓存及其他设置都会清除。刷写前请备份需要保留的数据。TF 卡不会因刷写闪存而被写入，但设备的 SD 卡缓存数据可能与新固件不兼容。

请勿将镜像写到 `0x10000`，也不要用于其他板型、其他闪存容量或仅接受 OTA 应用镜像的刷写流程。

## Windows 刷写

安装 esptool 5.6.0，并将 `COM12` 替换成设备当前端口。在镜像所在目录打开 PowerShell：

```powershell
python -m esptool --chip esp32s31 --port COM12 --baud 115200 --before default-reset --after hard-reset --no-stub write-flash --flash-mode dio --flash-size 16MB --flash-freq 80m 0x0 .\kiikoread-metalio47-0.3.3-rc113-final-full-flash-16MB.bin
```

如果刷写器提示芯片未进入下载模式，请按设备的 BOOT/复位方式重新进入下载模式后重试。确保使用可靠的数据线，刷写期间不要拔线或断电。

## 校验

SHA-256 校验文件随发行版提供。PowerShell 校验命令：

```powershell
(Get-FileHash .\kiikoread-metalio47-0.3.3-rc113-final-full-flash-16MB.bin -Algorithm SHA256).Hash.ToLower()
```

结果应与 `SHA256SUMS.txt` 中的镜像摘要一致。镜像长度必须是 `16,777,216` 字节。

## 镜像布局

| 内容 | 地址 |
| --- | ---: |
| Bootloader | `0x2000` |
| 分区表 | `0x8000` |
| OTA 初始化数据 | `0xD000` |
| 应用 | `0x10000` |
| 其余地址 | `0xFF` 填充 |

此固件为 kiikoreader Metalio S31 社区构建，不是腾讯、微信读书、Metalio 或 kiikoread 官方发行版。
