# MinView Native

高性能 Windows 图片浏览器 — C++20 / Direct2D + WIC 原生实现，对标 HoneyView。

## 特性

- **GPU 加速渲染** — Direct2D 驱动，网格/大图切换 60fps 平滑过渡
- **缩略图飞行动画** — 网格与大图模式间带缓出的缩略图缩放 + 淡入淡出遮罩
- **即时缩放** — 鼠标滚轮 / Ctrl+滚轮 / 触摸板，支持自适应窗口（`Ctrl+0`）
- **全屏模式** — F11 切换，自动隐藏标题栏和任务栏
- **网格视图** — 自适应列数、智能缩略图预加载、滚动条、右侧信息面板
- **AIGC 元数据** — 解析 ComfyUI PNG 内嵌 prompt/workflow 并展示
- **右键菜单** — 打开文件、复制图片、删除、打开所在文件夹
- **键盘快捷键** — 全键盘操作，传统图片浏览器快捷键布局
- **暗色主题** — 全局 #1A1A1E 深色背景，清晰易读

## 技术栈

| 层 | 技术 |
|---|---|
| 语言 | C++20 |
| 渲染 | Direct2D 1.1 + DirectWrite |
| 图片解码 | WIC (Windows Imaging Component) |
| 窗口 | Win32 (WS_OVERLAPPEDWINDOW) |
| 构建 | MSBuild (Visual Studio 2022 BuildTools) |

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

# 构建 Release
build.bat

# 可执行文件
# build/Release/MinView.exe
```

## 快捷键

| 键 | 网格模式 | 大图模式 |
|---|---|---|
| `Space` | 进入预览 | 退回网格 |
| `Esc` | — | 退回网格 / 退出全屏 |
| `F11` | 全屏 | 全屏 |
| `←` `→` | 上下选图 | 上一张/下一张 |
| `Ctrl+O` | 打开文件 | 打开文件 |
| `Ctrl+0` | — | 适应窗口 |
| `Ctrl++/-` | — | 缩放 |
| `F2` | 重命名 | — |
| `Del` | 删除 | 删除 |
| `Ctrl+C` | 复制 | 复制 |
| `I` | — | 查看 AIGC 信息 |
| `R` | 递归浏览子文件夹 | — |

## 架构

```
minview-native/
├── src/
│   ├── main.cpp          # 入口 + GDI 回退渲染
│   ├── app.cpp/h         # 应用主逻辑（~2700 行）
│   ├── renderer.cpp/h    # D2D/DWrite 渲染器
│   ├── window.cpp/h      # Win32 窗口封装
│   ├── decoder.cpp/h     # WIC 图片解码
│   ├── indexer.cpp/h     # 文件索引 + 排序
│   └── metadata.cpp/h    # PNG/AIGC 元数据解析
├── build/                # 构建输出（.gitignore）
├── build.bat             # 构建脚本
├── CHANGELOG.md
└── README.md
```

## 灵感

MinView Native 是 MinView（PySide6 原型）的 C++ 重写，目标是对标 HoneyView 的性能：瞬间启动、零延迟渲染、平滑动画。
