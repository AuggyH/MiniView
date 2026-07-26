## 变更摘要

<!-- 说明用户可观察到的变化，以及为什么需要这次变更。 -->

## 范围

- 目标：
- 非目标：
- 关联 Issue：

## 类型与风险

- 类型：<!-- feat / fix / docs / refactor / test / ci / build / chore -->
- 风险：<!-- low / medium / high -->
- 影响区域：<!-- app / rendering / decoding / indexing / metadata / build -->

## 验证证据

- [ ] `build.bat` 成功（Release、`/W4 /WX`）
- [ ] CTest 全部通过
- [ ] 已按风险执行 `docs/QA_CHECKLIST.md`
- [ ] 未运行或无法验证的检查已在下方说明

QA 环境与结果：

<!-- Windows 版本、DPI、测试数据、步骤、实际结果；截图/日志可作为附件。 -->

## Code Review

- [ ] 改动范围最小且没有混入无关重构
- [ ] 线程、COM、缓存和资源生命周期已检查
- [ ] DPI、中文文案和暗色主题约定已检查
- [ ] 删除、覆盖、注册表等高影响操作有安全边界
- [ ] 新行为有自动化测试，或说明了只能手工验证的原因

## 已知风险与回滚

- 已知风险：
- 回滚方式：<!-- 通常为 revert squash commit；数据迁移需单独说明。 -->
