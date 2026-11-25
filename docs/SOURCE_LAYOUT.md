<!-- SPDX-FileCopyrightText: 2026 YIYO-Designer -->
<!-- SPDX-License-Identifier: LicenseRef-Kiikoread-Personal-Study-1.0 -->

# 源码目录索引

本仓库以标准 ESP-IDF 项目结构组织，默认开发目标为 kiikoreader Metalio S31（ESP32-S31，固件基线 0.3.3-rc113）。共享代码仍保留 Pico 板级支持；构建目标由 `--board` 明确选择。

```text
.
├── .github/workflows/   GitHub Actions 构建与源码检查
├── boards/              板级配置与驱动；metalio47 为本项目目标
├── cmake/               ESP-IDF / CMake 辅助配置
├── components/          可复用 ESP-IDF 组件、显示、存储、传输和传感器驱动
├── docs/                硬件、构建、用户文档及功能架构说明
├── licenses/            项目补充许可文本
├── main/
│   ├── app/              主循环、导航、页面生命周期
│   ├── apps/             首页、书架、微信读书、文件和设置页面
│   ├── book/             书籍解析、阅读存储、微信读书协议与缓存
│   ├── font/             字体加载、排版字形和在线字体
│   ├── ui/               共享 UI 绘制和交互组件
│   └── assets/           固件嵌入资源
├── tools/                构建、打包、主机侧检查和开发工具
├── CMakeLists.txt        ESP-IDF 顶层工程入口
├── dependencies.lock     ESP-IDF 组件依赖锁定
└── sdkconfig.defaults    由板级配置选用的默认构建选项
```

## 阅读代码建议

1. 从 `boards/metalio47/board.json`、板级 `sdkconfig.defaults` 和 `main/CMakeLists.txt` 确认目标配置与源文件集合。
2. 从 `main/app/app_registry.c`、`main/app/app_loop.c` 理解导航注册和生命周期。
3. 阅读普通阅读器 `main/apps/app_book.c`，再看 [微信读书架构](WEREAD_ARCHITECTURE.md) 中列出的分层实现。
4. 查找硬件模块时进入 `components/` 对应组件，并查看其 `README.md`、`idf_component.yml` 和 `LICENSE`。

## 仓库边界

本仓库专注固件源码与构建说明。网站、在线刷机页面、历史固件镜像、设备日志、账号数据及本机交接记录不属于源码发布内容。构建生成物应留在 `build/` 并由 `.gitignore` 排除。
