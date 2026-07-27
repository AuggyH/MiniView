# MinView Native 贡献与交付流程

本仓库采用轻量主干开发：`master` 始终代表通过仓库门禁的可发布代码，所有变更通过短生命周期分支和 Pull Request 合入。不维护长期 `develop` 分支。

## 权威来源

- 项目行为、DPI、中文化和构建约定：`AGENTS.md`
- 当前用户可观察交互：`docs/PRODUCT_SPEC.md`
- 性能预算、正式数据集和测量方法：`docs/PERFORMANCE_BUDGET.md`
- 构建目标与编译选项：`CMakeLists.txt`
- Git、PR、CI、QA、CR 门禁：本文
- 手工回归用例：`docs/QA_CHECKLIST.md`
- 当前源码结构和风险边界：`docs/ARCHITECTURE.md`

聊天、Issue 和 PR 描述可以解释上下文，但不能静默替代以上当前文件。

## 分支与提交

从最新远端 `master` 创建短分支：

```powershell
git fetch --prune origin
git switch -c <类型>/<主题> origin/master
```

允许的分支前缀：

| 前缀 | 用途 |
|---|---|
| `feature/` | 用户可见的新能力 |
| `fix/`、`hotfix/` | 常规修复、生产紧急修复 |
| `refactor/` | 不改变预期行为的结构调整 |
| `test/` | 测试和 QA 工具 |
| `docs/` | 仅文档 |
| `ci/`、`build/` | Actions、CMake 和构建脚本 |
| `chore/` | 其他维护工作 |
| `codex/` | Codex worktree 中的任务分支 |

提交和 PR 标题使用 Conventional Commits：

```text
feat(grid): 优化 justified 布局
fix(dpi): 修复跨屏后标题栏尺寸
test(indexer): 覆盖递归扫描
ci: 增加 Windows Release 门禁
```

允许的类型为 `feat`、`fix`、`docs`、`refactor`、`test`、`ci`、`build`、`chore`。一次提交只表达一个可回滚意图，不混入无关格式化或重构。

## 标准 Git 流程

1. 明确目标、非目标、验证方法、风险边界和停止条件。
2. 确认分支、远端、工作树和基线构建；保留来源不明的本地改动。
3. 从 `origin/master` 建短分支，实施最小必要修改。
4. 增加或更新能直接证明行为的测试；GUI 行为同步更新手工 QA 范围。
5. 运行 `build.bat`。脚本执行 Release 编译、`/W4 /WX` 和 CTest。
6. 推送分支并尽早创建 Draft PR，填写范围、风险和验证证据。
7. CI 通过后，根据风险完成 QA，并把环境、数据和实际结果写入 PR。
8. 完成 Code Review；所有阻塞意见解决后将 PR 标为 Ready。
9. 满足合并门禁后 squash merge，并删除远端分支。
10. 只有经过发布验收的提交才创建 `vMAJOR.MINOR.PATCH` 标签；合并不等于发布。

## 独立角色与持续交接

复杂交付最多同时使用三个短生命周期角色：PM/协调者、唯一 Implementation/Package Owner、独立 Validator/Reviewer。同一分支和 worktree 同时只有一个实现 Owner 可写；其他角色保持只读，直到显式交接。

- 角色转换使用独立、可见的 worker/任务，并从当前仓库、远端与运行证据独立回读；聊天结论只用于路由，不直接当作事实。
- 受权接管后，Owner 持续推进到已声明的下一门禁；普通构建、测试和 CI 失败由 Owner 做最小判别和修复。同一症状出现两次后，先做能区分剩余假设的实验。
- 只有产品范围/交互/版本/语义、风险接受、稳定版覆盖、发布或 Owner 验收、新依赖/大架构、不可恢复破坏或权限阻塞才 STOP 并等待决策。
- GUI 前台、稳定版二进制、用户数据、独占夹具等共享资源必须串行操作；可并行的只读检查不得隐式获得写权。
- 候选证据绑定精确 commit/tree、产物 SHA-256 与绝对路径；GUI/QA 还应记录 PID/窗口身份、环境、DPI 和数据集。未绑定的旧证据不得晋级为当前候选结论。
- 交接时若仍有未提交工作，必须在原 checkout 之外只读封存 HEAD/tree/status、tracked diff、允许的 untracked 文件和直接证据，生成不自引用的 SHA-256 manifest；后继角色逐项验哈希后才能 ACK。
- 正式交接只包含七项：精确身份、实际变化、直接证据、当前状态、风险/保护、回滚、唯一下一门禁。后继完成独立回读并正式 ACK 后，前序 worker 立即归档。

## PR 标签

每个 PR 应有：一个类型标签、至少一个区域标签和一个风险标签。状态标签必须反映当前门禁；`status: needs-review` 与 `status: needs-qa` 可以同时存在，`status: blocked` 与它们互斥。

| 维度 | 标签 |
|---|---|
| 类型 | `bug`、`enhancement`、`documentation`、`refactor`、`tests`、`ci`、`chore` |
| 区域 | `area: app`、`area: rendering`、`area: decoding`、`area: indexing`、`area: metadata`、`area: build` |
| 风险 | `risk: low`、`risk: medium`、`risk: high` |
| 状态 | `status: blocked`、`status: needs-qa`、`status: needs-review` |

区域路径标签由 `.github/workflows/pr-labeler.yml` 自动维护；类型、风险和状态由作者根据实际变更设置。标签定义可用以下命令同步，先 dry-run：

```powershell
.\tools\sync-labels.ps1 -WhatIf
.\tools\sync-labels.ps1
```

风险判定：

- `low`：仅文档、注释或不改变产物的仓库配置。
- `medium`：局部逻辑、布局、格式支持、构建或可快速回滚的 UI 变化。
- `high`：删除/覆盖文件、注册表、线程与缓存生命周期、COM/D2D 资源、DPI/全屏状态机、大规模扫描或发布链路。

## 状态与门禁

状态词必须对应证据，不能互相替代：

| 状态 | 必要证据 |
|---|---|
| 已实现 | 目标代码和测试已落地，不代表验证通过 |
| 本地验证通过 | `build.bat` 成功，且 PR 记录运行环境 |
| CI 通过 | 当前 HEAD 的 `PR / policy` 与 `Windows / build-and-test` 成功 |
| QA 通过 | `docs/QA_CHECKLIST.md` 中与风险匹配的用例有实际结果 |
| CR 通过 | 阻塞意见已解决；多人仓库还需要批准 review |
| 可合并 | CI、QA、CR 均通过，PR 无未解决线程且分支最新 |
| 已交付 | 变更已合入 `master` |
| 已发布 | 指定提交已打版本标签并完成发布验收 |

候选方案、计划、实现、验证、合并、交付和发布是不同阶段。PR 中只声明当前已取得证据的阶段。

## CI 门禁

`.github/workflows/ci.yml` 在 PR 和 `master` push 上执行：

- 校验 PR 标题格式。
- 使用 Windows Server 2022、Visual Studio 2022 和 x64 Release 配置。
- 以 `/W4 /WX` 编译 `MinView`、诊断工具和自动化测试。
- 运行 CTest；当前注册测试及各自覆盖边界以 `docs/ARCHITECTURE.md` 的“当前自动化边界”为唯一清单；CI 对该清单与 `CMakeLists.txt` 的 `add_test(NAME ...)` 注册集合做精确一致性校验。
- 上传 `MinView.exe` 候选产物，保留 7 天供 QA 使用。

CI 证明可重复构建和已注册的自动化测试通过，不证明 GUI、DPI、GPU、可选系统 codec 或文件删除行为正确。

## QA 流程

作者先按 `docs/QA_CHECKLIST.md` 做与变更相关的冒烟测试；`medium` 和 `high` 风险 PR 必须记录 Windows 版本、DPI、测试数据规模和实际结果。`high` 风险涉及删除时只能使用可恢复的临时副本。

若发现失败：

1. 记录最小复现、环境、期望和实际结果。
2. 将 PR 标记为 `status: blocked`，不要把产物存在当作 QA 通过。
3. 同一症状连续出现两次后，停止猜测，设计能区分剩余假设的最小实验。
4. 修复后只重跑受影响用例和必要回归，并更新当前 HEAD 的证据。

## Code Review 流程

作者先完成自查，再请求 reviewer。CR 关注：

- 正确性、边界输入和失败路径。
- UI 线程与后台线程间的数据所有权、停止条件和 COM 初始化。
- D2D/DWrite/WIC 对象生命周期、设备丢失和 DPI 缩放。
- 删除、拖拽、剪贴板、注册表和文件关联等高影响操作。
- 中文文案、暗色主题、快捷键与既有交互是否回归。
- 测试是否直接证明声明的行为，是否遗漏手工验证边界。

Review 严重级别：

| 级别 | 含义 | 合并要求 |
|---|---|---|
| `P0` | 数据丢失、安全或必现崩溃 | 必须修复并重跑相关 QA |
| `P1` | 主要功能错误、竞态、资源泄漏或门禁失效 | 必须修复 |
| `P2` | 有限场景缺陷、可维护性风险或测试缺口 | 必须处理或由 reviewer 明确接受 |
| `P3` | 非阻塞建议 | 可后续跟进 |

所有 review thread 必须在代码或解释得到确认后解决。作者不得仅用“CI 通过”关闭行为性问题。

## 合并、保护与发布

推荐仓库设置：

- 默认分支 `master`，要求通过 PR 合入。
- 必需状态检查：`PR / policy`、`Windows / build-and-test`。
- 要求分支合并前最新、解决所有 review thread、禁止 force-push 和删除。
- 仅允许 squash merge，合并后自动删除分支。
- 当前只有单一维护者时批准数设为 0，以免无法批准自己的 PR；增加第二位 reviewer 后改为至少 1 个批准并启用 stale review dismissal。

首次启用分支保护应在 CI 工作流已经合入 `master` 并成功运行后执行，避免引用尚不存在的 required check。

发布采用语义化版本：破坏性变更提升 major，兼容功能提升 minor，兼容修复提升 patch。发布标签必须指向已通过 CI、QA、CR 和发布验收的 `master` 提交。

## Hotfix

生产紧急问题从 `origin/master` 创建 `hotfix/<主题>`，仍需 PR、CI 和最小风险 QA。只能缩短非相关回归范围，不能跳过受影响功能、文件操作安全和回滚验证。合入后按 patch 版本发布，并把完整回归作为后续明确任务。
