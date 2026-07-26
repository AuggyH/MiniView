# MinView Native 源码结构与风险地图

本文记录当前源码事实，用于确定 PR 影响范围、CI 覆盖和 QA 深度。它不是未来重构计划。

## 运行结构

| 模块 | 当前职责 | 主要风险 |
|---|---|---|
| `src/main.cpp` | DPI awareness、COM、单实例、参数转发、配置目录、文件关联 | 注册表副作用、单实例消息、启动/退出清理 |
| `src/window.*` | Win32 窗口注册、创建、消息转发、暗色 DWM 属性 | Windows 版本兼容、自绘非客户区、窗口生命周期 |
| `src/app.*` | 应用状态机、快捷键、菜单、网格/大图、动画、删除/复制/拖拽、后台加载 | 集中度最高；线程、缓存、输入状态和文件操作相互影响 |
| `src/renderer.*` | D3D11/DXGI swap chain、Direct2D、DirectWrite、网格和面板绘制 | 设备丢失、资源重建、DPI 单位、裁剪栈与高负载 |
| `src/decoder.*` | WIC 解码、缩放、探测、主色提取 | 系统 codec 差异、超大/损坏图片、COM apartment |
| `src/indexer.*` | 文件扫描、过滤、排序、路径索引 | 递归权限、格式集合、排序稳定性、大目录性能 |
| `src/metadata.*` | PNG tEXt、JPEG/WebP 注释、ComfyUI 与 SD WebUI 信息解析 | 自定义解析器、损坏输入、编码与受预算约束的元数据 |
| `src/viewer.*` | 当前仅为空壳 | 不应把它当作现有缩放/全屏实现；实际逻辑在 `App`/`Renderer` |

主要执行链：

```text
wWinMain
  -> App::run
     -> Window 消息循环
        -> App::handle_message
           -> ImageIndex / Decoder / metadata
           -> Renderer
```

## 状态与并发

- UI 状态主要由 `App` 持有并在窗口线程修改。
- 相邻大图预加载使用 1 个后台线程，队列和最多 3 项的 WIC 缓存由 mutex 保护。
- 网格缩略图使用 4 个后台线程；WIC 缓存受 mutex 保护，D2D bitmap 只在主线程创建和使用。
- 生成信息由 1 个 `metadata worker` 在 UI 线程外解析；请求、结果和 ready 状态由 mutex/condition variable 保护，通过 `WM_METADATA_READY` 回到窗口线程，并按当前面板 path 丢弃过期结果。
- 后台 WIC 线程各自初始化 COM 并各自创建 `Decoder`。
- `App` 析构时停止并 join metadata、预加载与缩略图线程；网格退出刻意保留缩略图缓存。

涉及 `ImageIndex`、`m_thumbs`、队列或排序的变更必须检查后台 worker 是否仍持有按索引访问的数据。涉及 D2D 对象的变更必须保持它们在渲染线程创建和消费。

## 当前自动化边界

CTest 当前注册六项自动化测试：

- `indexer.unit`：扫描、过滤、排序、相对路径、Windows invariant path identity 和批量移除。
- `app_state.unit`：缩放/拖动状态、滚动夹紧、布局重建选择可见性和删除后 current identity 事务。
- `metadata.unit`：正常、恶意长度、截断和超预算 PNG tEXt 输入。
- `renderer_state.unit`：D2D/DXGI 失败分类、设备重建和 App 缓存 generation 失效。
- `file_operation.unit`：删除后磁盘存在性与 Shell 完成/取消/部分完成判定。
- `governance.unit`：构建失败传播、标签边界和权威文档约束。

Windows CI 还验证所有目标能在 MSVC x64 Release、`/W4 /WX` 下构建。这些窄接口测试不启动窗口，也不替代下列人工 QA。

以下行为仍需手工 QA 或未来通过更窄接口增加测试：

- Win32 消息、快捷键、全屏、大图模式和动画。
- 100%/200% DPI 及跨显示器 `WM_DPICHANGED`。
- D3D hardware/WARP、设备丢失和窗口 resize。
- WIC 可选 codec、损坏图片和超大图片。
- OLE 拖拽、剪贴板、回收站/永久删除和文件关联。
- 后台加载的竞态、退出等待、缓存命中和大目录性能。
- ComfyUI/SD WebUI 元数据的多种真实样本。

## 已发现但未在治理变更中修复的源码风险

这些发现应使用独立 Issue/PR 处理，以免流程建设与行为修复混在同一提交：

1. `src/main.cpp` 注册的图片扩展名多于 `src/indexer.cpp` 扫描允许的扩展名，文件关联能力与目录浏览能力并不一致。
2. `CMakeLists.txt` 的项目版本、`src/app.cpp` 关于对话框版本和 `CHANGELOG.md` 可能不是同一版本来源，发布前需要收敛。
3. `App` 同时承担窗口状态机、业务逻辑、后台任务和部分平台集成；修改时应按风险拆小 PR，而不是在流程建设中顺手重构。

“已发现”不等于已复现或已修复。处理每项风险时仍需最小复现实验和直接证据。
