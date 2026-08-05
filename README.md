# MinView Native

高性能 Windows 图片浏览器 — C++20 / Direct2D + WIC 原生实现，对标 HoneyView 的启动速度与渲染流畅度。

[![Build](https://github.com/AuggyH/minview-native/actions/workflows/ci.yml/badge.svg)](https://github.com/AuggyH/minview-native/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://github.com/AuggyH/minview-native/blob/master/LICENSE)

## 特性

**浏览体验**
- GPU 加速渲染 — Direct2D 驱动，网格/大图切换 60fps 平滑过渡
- 缩略图飞行动画 — 四次方缓出缩放 + 遮罩淡入淡出
- 即时缩放 — 鼠标滚轮 / Ctrl+滚轮，支持自适应窗口（`Ctrl+0`）
- 全屏模式 — F11 切换，自动隐藏标题栏和任务栏

**漫画阅读**
- 纵向连续阅读 — `M` 开关，无需新增顶层模式，保持既有快捷键语义
- 阅读宽度 50%–200% 可调，支持无缝页距（查看菜单）
- 巡航模式 — `P` 开/关，`[` `]` 调速 0.5x–2.0x
- 中键自动滚动、页面锚点保持（视口变化不丢阅读位置）

**实用能力**
- AIGC 元数据 — 解析 ComfyUI PNG 内嵌 prompt/workflow 并展示
- 打开错误反馈 — 缺失、不支持、损坏输入的中文错误提示，不丢当前浏览状态
- 网格视图 — 自适应列数、智能缩略图预加载、自绘暗色滚动条
- 右键菜单 — 打开文件、复制图片、删除、打开所在文件夹
- 拖拽导出 — 左键按住拖拽，OLE 导出图片到桌面或其他应用
- 键盘全操作 — 传统图片浏览器快捷键布局

**工程**
- 暗色主题 — 全局 #1A1A1A 深色背景
- 4K 200% DPI 适配 — Per-monitor DPI awareness V2，全部尺寸 DPI 缩放
- 内存软上限 512 MiB — 统一 LRU 淘汰，漫画缓存独立预算
- 零外部依赖 — 仅系统 DLL，Release 二进制约 500 KB

## 技术栈

| 层 | 技术 |
|---|---|
| 语言 | C++20（MSVC，/W4 /WX 零警告门禁） |
| 渲染 | Direct2D 1.1 + DirectWrite |
| 图片解码 | WIC (Windows Imaging Component) |
| 窗口 | Win32 (WS_OVERLAPPEDWINDOW) |
| 构建 | CMake + MSBuild（Visual Studio 2022 BuildTools） |

## 构建

**前置条件：**
- Windows 10/11
- Visual Studio 2022 BuildTools（或完整 VS）
  - MSVC v143 编译器
  - Windows 10/11 SDK

```bash
# 克隆
git clone https://github.com/AuggyH/minview-native.git
cd minview-native

# 构建 Release 并运行全部测试
build.bat

# 可执行文件
# build/Release/MinView.exe
```

## 快捷键

| 键 | 网格模式 | 大图模式 |
|---|---|---|
| `Space` | 进入大图 | 退回网格 |
| `Esc` | — | 退回网格 / 退出全屏 |
| `Enter` / `F11` | 全屏 | 全屏 |
| `←` `→` | 选图 | 上一张/下一张 |
| `Ctrl+O` | 打开文件 | 打开文件 |
| `Ctrl+0` | — | 适应窗口 |
| `Ctrl++` / `Ctrl+-` | — | 缩放 |
| `Del` | 删除 | 删除 |
| `Ctrl+C` | 复制 | 复制 |
| `I` | 展开/收起信息面板 | 展开/收起信息面板 |
| `L` | 显示/隐藏文件名标签 | 显示/隐藏文件名标签 |
| `A` | 方形缩略图切换 | — |
| `N` / `D` / `S` / `R` | 排序切换 | — |
| `Ctrl+R` | 递归浏览子文件夹 | — |

**漫画模式（大图内）：**

| 键 | 作用 |
|---|---|
| `M` | 开/关漫画阅读 |
| `P` | 巡航开/关 |
| `[` `]` | 巡航速度 |
| 中键拖拽 | 自动滚动 |

`G` 键已移除，不用于网格与大图切换。完整交互定义见 [docs/PRODUCT_SPEC.md](docs/PRODUCT_SPEC.md)。

## 架构

```
minview-native/
├── src/
│   ├── main.cpp                # 入口
│   ├── app.cpp/h               # 应用主逻辑与窗口消息处理
│   ├── app_state.h             # 交互路由/状态纯函数（可单测）
│   ├── renderer.cpp/h          # D2D/DWrite 渲染器
│   ├── renderer_state.h        # 渲染布局/计划纯函数
│   ├── window.cpp/h            # Win32 窗口封装
│   ├── viewer.cpp/h            # 大图视图
│   ├── decoder.cpp/h           # WIC 图片解码
│   ├── indexer.cpp/h           # 文件索引 + 排序
│   ├── metadata.cpp/h          # PNG/AIGC 元数据解析
│   ├── file_operation.cpp/h    # 删除/导出等文件操作
│   ├── file_operation_windows.cpp  # 平台实现
│   ├── comic_reader_model.h    # 漫画阅读模型（布局/锚点/预算）
│   ├── comic_reader_loader.cpp/h   # 漫画页异步加载
│   └── open_error.h            # 打开错误分类
├── tests/                      # CTest 单元测试
├── tools/                      # 开发辅助工具
├── docs/                       # 规格、性能预算、QA 文档
├── build.bat                   # 一键构建 + 测试
├── LICENSE
└── README.md
```

## 开发与质量

`build.bat` 会在当前 checkout/worktree 中执行 x64 Release 构建、`/W4 /WX` 警告门禁和 CTest。提交代码前请阅读 [CONTRIBUTING.md](CONTRIBUTING.md)，产品交互、性能预算、架构与回归范围分别见 [docs/PRODUCT_SPEC.md](docs/PRODUCT_SPEC.md)、[docs/PERFORMANCE_BUDGET.md](docs/PERFORMANCE_BUDGET.md)、[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) 和 [docs/QA_CHECKLIST.md](docs/QA_CHECKLIST.md)。提交内容会经过 [gitleaks](https://github.com/gitleaks/gitleaks) 预提交密钥扫描（`git config core.hooksPath .githooks`）。

## License

[MIT](LICENSE)

## 灵感

MinView Native 是 MinView（PySide6 原型）的 C++ 重写，目标是对标 HoneyView 的性能：瞬间启动、零延迟渲染、平滑动画。
