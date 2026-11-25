# kiikoread Metalio47 Fusion

在 **Metalio E-Ink4 Plus**（ESP32-S31 墨水屏阅读器）上继续开发的微信读书增强固件。

本仓库基于 [YIYO-Designer/kiikoreader-wereader](https://github.com/YIYO-Designer/kiikoreader-wereader)（微信读书固件框架）继续开发，同时沿用厂长 **kiikoread** 的整机固件底座。上游原有的书架同步、书籍缓存、进度同步等功能保持不变，本仓库是在其上继续补齐阅读体验的部分。

> 本项目为个人学习与研究用途，非腾讯、微信读书、Metalio 或 kiikoread 官方发布。微信读书接口为非公开服务接口，服务端策略变化可能影响功能可用性。

上游原始说明已保留在 [README.upstream.md](README.upstream.md)。

## 相对上游新增的功能

**阅读基础设施**

- **微信读书划线**：上游没有划线能力。本版新增划线功能，实现参考了「沉在阅读」的划线方案。划线在本地持久化，离线也能看到，并与段评、想法联动。
- **全书按百分比跳转 + 单章按页数跳转**：补上整本书的定位能力和单章内的页级定位，不再只能一章一章往后翻。
- **离线阅读时长补传**：阅读时长先本地记账，恢复联网后按真实时间回放补传，避免断网期间的时长丢失。

**登录**

- 登录方式由原来的网页端扫码，改为微信读书 **BOOX 登录**通道，一次扫码即可完成授权并自动续期。

**AI 能力**

- 新增 **AI 设置选项**，可配置模型与调用方式。
- **想法 AI 归纳**：把一章里的零散想法、段评归纳成结构化摘要。
- **长按文本 AI 解释与翻译**：选中即解释、即翻译，不需要跳出阅读。
- **AI 追问**：解释或归纳之后可以继续追问，形成连续对话。
- **高亮选取点击跳回**：在想法/高亮面板里点任意一条，可直接跳回原文对应位置。

**界面**

- **kiikoread 设置页整合**：把原本分散的多项设置归并到几个大类里，不用再靠上下滑动逐个找、点大类再点小项。

## 我们修掉的稳定性问题

这些不是新功能，是开发过程中真实遇到并解决掉的：

- 关机流程卡在 `NOT_FINISHED`：诊断日志的常驻写句柄一直占着 SD 卡，导致关机时的存储转移无法完成。
- 进入微信读书瞬间崩溃：HTTP keepalive 持有了已经失效的 socket，连接状态变化时没有主动断链。
- 微信读书页面整页失效、手势失灵：登录失效分支只标记状态却不清除底层结果，每一轮都触发整页重绘，把主循环拖到几百毫秒。
- 登录态恢复后，失效横幅不消失。

## 硬件与环境

- 设备：Metalio E-Ink4 Plus（ESP32-S31，双核 RISC-V，16 MB Flash + 16 MB Octal PSRAM，1216×684 墨水屏）
- 构建：ESP-IDF **v6.1**

## 构建

在已安装 ESP-IDF v6.1 的环境中，从仓库根目录执行：

```bash
python tools/build_firmware.py --board metalio47 --idf-metalio47 "$IDF_PATH" --python "$(command -v python)" --output build/boards --s31-pie --s31-text-ripple --s31-reader-panel37
```

板级适配位于 `boards/metalio47/`。架构与源码索引见 [`docs/SOURCE_LAYOUT.md`](docs/SOURCE_LAYOUT.md) 与 [`docs/WEREAD_ARCHITECTURE.md`](docs/WEREAD_ARCHITECTURE.md)，S31 构建细节见 [`docs/BUILD_METALIO_S31.md`](docs/BUILD_METALIO_S31.md)。

烧录前请确认板型、分区布局与镜像类型；全片镜像会清除设备设置、账户和缓存。

## 许可

本仓库内容许可范围不统一，按各自原许可执行：

- 上游固件与第三方组件见 [`LICENSE`](LICENSE) 与 [`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md)。其中 `LICENSE` 为 Kiikoread-Personal-Study-1.0，允许个人学习、研究、测试，**未经书面许可不得商用**；底座部分沿用 Apache-2.0 等第三方许可。
- 请勿将整个仓库笼统视为单一许可。

## 致谢

- 整机固件底座：厂长 **kiikoread**
- 微信读书功能框架：**设计君的日常碎碎念**（[kiikoreader-wereader](https://github.com/YIYO-Designer/kiikoreader-wereader)）
- 划线实现参考：**沉在阅读**（GitHub: [cyongxiang](https://github.com/cyongxiang)）

## 安全

请勿在本仓库提交 Cookie、登录二维码、账户凭据、NVS 导出、设备日志或含个人信息与书籍内容的截图。
