# 多格式支持调研与实施预估

状态：调研完成，尚未扩大正式支持范围。
基线：C++20、Direct2D/WIC/DWrite、单文件部署、当前不引入第三方运行时 DLL。

## 1. 当前事实

程序目前只索引 `PNG/JPEG/BMP/GIF/WebP/TIFF`，但文件关联还包含 ICO、HEIC、HEIF、AVIF、SVG、PSD、TGA、DDS 等扩展名。因此“可被系统关联”不等于“能在网格和大图中可靠解码”。在扩大索引范围前，必须先补上能力检测与失败态，否则不受支持的文件会被计入网格并长期停在骨架图。

Windows Imaging Component（WIC）会自动使用系统已安装的编解码器；系统原生提供 BMP、GIF、ICO、JPEG、JPEG XR、PNG、TIFF、DDS 等解码器，HEIF/AVIF 和 WebP 可由扩展编解码器提供。[Microsoft：WIC 概览](https://learn.microsoft.com/en-us/windows/win32/wic/-wic-about-windows-imaging-codec)、[Microsoft：WIC 编解码器列表](https://learn.microsoft.com/en-us/windows/win32/wic/native-wic-codecs)

真实目录 `D:\AIGC\Assets` 在 2026-07-26 的相关格式分布：

| 格式 | 数量 | 当前机器 WIC 实测 | 说明 |
|---|---:|---|---|
| AVIF | 424 | 可探测、可缩放解码 | 代表样本 1200×800，首次探测+解码约 313 ms |
| SVG | 5,968 | 不可解码 | 业务价值最高的待支持格式 |
| PSD | 2 | 不可解码 | 数量很少，优先级低 |
| WebP | 4,120 | 可探测、可缩放解码 | 已在正式索引范围内；代表样本约 18 ms |
| HEIC/HEIF | 0 | 无真实样本 | 系统编解码器不保证每台电脑都安装 |
| TGA | 0 | 无真实样本 | 暂无本地业务收益 |
| DDS | 0 | 无真实样本 | WIC 有原生支持，但格式子集有限 |

上述机器实测可用 `build\Release\wic_format_probe.exe <文件...>` 重复执行；工具直接复用产品的 `Decoder`，不是根据扩展名猜测。

## 2. 推荐路线与工作量

工作量以 1 名熟悉当前代码的工程师有效工作日估算，包含实现、单元/集成测试和一轮真实目录 GUI 回归，不包含等待用户验收。

| 优先级 | 格式/能力 | 推荐方案 | 预计工期 | 主要风险 |
|---|---|---|---:|---|
| P0 | 解码能力层 | 枚举 WIC 解码器；将“关联扩展名、可索引格式、当前机器可解码格式”分离；为解码失败提供明确错误卡片 | 1.5–2.5 天 | 编解码器按机器变化；不能用单个样本代表整个格式 |
| P1 | AVIF/HEIC/HEIF | 第一阶段复用系统 WIC 扩展；启动时检测能力，未安装时给出中文说明，不静默失败 | 1–2 天 | HEVC/AV1 编解码器并非所有电脑都有；不同位深、HDR、动画、多帧兼容性不同。Microsoft 也明确说明 HEVC/AV1 可能不可用：[HEIF WIC 编解码器](https://learn.microsoft.com/en-us/windows/win32/wic/heif-codec) |
| P1 | SVG | 使用 `ID2D1DeviceContext5::CreateSvgDocument` 解析并在离屏目标中光栅化，再接入统一缩略图/大图缓存 | 基础 4–7 天；稳健 7–12 天 | SVG 特性覆盖、字体、滤镜、外部资源、超大画布与恶意 XML；Direct2D 提供原生 SVG 文档入口：[CreateSvgDocument](https://learn.microsoft.com/en-us/windows/win32/api/d2d1_3/nf-d2d1_3-id2d1devicecontext5-createsvgdocument) |
| P2 | DDS | 先走 WIC，并以真实 DDS 语料验证压缩格式、mipmap、数组、cubemap；不支持的子格式显示原因 | 1–2 天 | WIC 支持的是 DDS 子集；官方资料对 BC1–BC3 与较新文档的 BC1–BC5 表述存在版本差异，必须以目标 Windows 实测为准：[Windows 图像格式支持](https://learn.microsoft.com/en-us/windows/apps/develop/media-authoring-processing/supported-codecs) |
| P3 | PSD/TGA | 若接受源码级第三方依赖，可仅引入 `stb_image` 的 PSD/TGA 解码器；PSD 只读取合成预览 | 2–4 天 | 改变“零外部依赖”的治理定义；PSD 不支持图层语义、额外通道，TGA 子格式兼容性有限；需做解码尺寸上限和模糊测试。[stb_image 能力说明](https://github.com/nothings/stb/blob/master/stb_image.h) |
| 备选 | 内置 HEIC/AVIF | 集成 libheif + HEVC/AV1 解码后端，随程序发布 | 5–10 天起，另加长期维护 | DLL/体积、许可证组合、安全更新、构建矩阵明显增加，不符合当前零依赖方向；libheif 本身还需要具体编解码后端：[libheif](https://github.com/strukturag/libheif) |

## 3. 推荐实施顺序

1. 先完成 P0 能力层和错误态，让“支持”成为可验证的运行时事实。
2. 在不引入第三方依赖的前提下开放 AVIF/HEIC/HEIF；以当前 424 张 AVIF 做全量索引、缩略图、复制和大图回归。
3. 单独实现 SVG 光栅化管线。SVG 数量最多，收益明显，但不能混入普通 WIC 小改动。
4. 用真实语料再决定 DDS；当前目录为 0，不应提前为完整 DDS 生态引入 DirectXTex。
5. PSD/TGA 暂缓。若以后数量增长，再由用户确认“零外部依赖”是否允许第三方源码静态编入。

## 4. 验收门槛

- 无对应编解码器时，不崩溃、不无限重试、不永久显示为“加载中”；必须显示可理解的中文失败原因。
- 格式支持必须同时覆盖：索引、探测尺寸、网格缩略图、大图、缩放、复制图片数据、文件拖放、冷缓存和损坏文件。
- 4K AVIF/HEIC 首次解码不能阻塞滚动；仍遵守 512 MiB 私有内存软上限与可见项优先加载。
- SVG 禁止隐式访问网络和外部文件；对极端尺寸、深层嵌套和解析失败设置明确上限。
- 每种格式至少建立：正常、透明、超大尺寸、损坏/截断、错误扩展名五类测试样本；多帧格式另测帧选择策略。

## 5. 当前建议结论

建议 Product Owner 批准 `P0 + 系统 WIC 的 AVIF/HEIC/HEIF`，预计 2.5–4.5 天；SVG 建议独立立项，预计 7–12 天达到可发布质量。在 Issue #4 作出范围决策前不实施。暂不建议引入 libheif、stb_image 或 DirectXTex，以免在需求量不足时破坏单文件、零运行时依赖和安全维护边界。
