# MinView Native — Changelog

## 2024-07-19: 转场动画 & 快捷键重构

### 动画系统

**缩略图飞行动画 (`draw_anim_thumb`)**
- 从网格原位缩放到大图位置，中心点 + 面积双线性插值
- 四次方缓出曲线 (`1-(1-t)^4`)，300ms
- 宽高比处理：起点 rect 用原图 `orig_w/orig_h` 纠正后居中于网格 cell；目标 rect 用预存的 `m_anim_iw/ih` 计算，避免 `image_size()` 过期
- 渲染期间不重算 src/dst（首帧锁定），打断才更新

**遮罩 (`draw_fade_overlay`)**
- 颜色与大图背景一致 `(0.102, 0.102, 0.102)` ≈ #1A1A1E
- 与缩略图动画同缓动曲线，同步淡入/淡出
- 进场：0→1 暗度渐增覆盖网格；退场：1→0 渐透明露出网格

**帧驱动**
- `QueryPerformanceCounter` 时间驱动，不依赖定时器精度
- 定时器 16ms（~60fps），避免 1ms 洪水阻塞消息队列影响缩略图加载

**状态机: `ACT_ENTER_IMAGE` / `ACT_EXIT_GRID` / `ACT_NONE`**
- 进场：动画在网格上播放→结束执行 `open_image()`
- 退场：立即 `toggle_grid()` 切换为网格→动画在网格上播放

### 快捷键

| 键 | 网格模式 | 大图模式 |
|---|---|---|
| 空格 | → 预览 / 再次→退回 | — |
| 双击 | → 大图 | → 退回网格 |
| 回车 | → 全屏 | ⇄ 全屏 |
| Esc | → 退回网格 | → 退回网格 | → 退出全屏 |
| ←→ | 选图 | 上一张/下一张 |

- **G 键已移除**（之前切网格）
- **空格不再切下一张**（去掉 `navigate_to(next)` fallthrough）

### 打断机制 (已禁用)

- 三处入口均 `if (m_animating) return 0;` 屏蔽
- 原理按 Apple `UIViewPropertyAnimator`：中断时取当前帧实际渲染 rect 为新 src，`m_anim_alt` 储存对侧端点供 toggle
- **已知问题**：快速连按偶发状态错乱/动画跳过；`m_from_grid` 清理不彻底导致黑屏
- 恢复方式：把 `return 0;` 替换为 Apple-style 打断代码

### 性能优化

- **缩略图加载**: 4 线程（原 2），去除锁内冗余 `probe`（每张省一次磁盘 IO + 锁竞争）
- **`m_anim_src` 缓存**: 只在 `m_grid_sel` 变化时重算（`m_last_cached_sel`），不每帧遍历
- **位置修正**: `m_anim_src` Y 坐标含 `- m_grid_scroll_y`，滚动后正确

### 已知问题

- 打断不稳定（已禁用）
- 缩略图加载偶发停滞（可能为 COM/Shell thumbnail 超时）
- 快速滚动时缩略图加载跟不上（可考虑 `m_scroll_active` 优化或虚拟化）
