[CmdletBinding(SupportsShouldProcess)]
param(
    [string]$Repository = "AuggyH/minview-native"
)

$ErrorActionPreference = "Stop"

if ($Repository -notmatch '^[^/]+/[^/]+$') {
    throw "Repository must use owner/name format."
}

if (-not (Get-Command gh -ErrorAction SilentlyContinue)) {
    throw "GitHub CLI (gh) is required."
}

& gh auth status --hostname github.com | Out-Null
if ($LASTEXITCODE -ne 0) {
    throw "GitHub CLI is not authenticated."
}

$labels = @(
    @{ Name = "refactor";             Color = "8250DF"; Description = "不改变预期行为的结构调整" },
    @{ Name = "tests";                Color = "1D76DB"; Description = "自动化测试或 QA 工具" },
    @{ Name = "ci";                   Color = "0E8A16"; Description = "CI、Actions 或仓库门禁" },
    @{ Name = "chore";                Color = "BFD4F2"; Description = "其他维护工作" },
    @{ Name = "area: app";            Color = "5319E7"; Description = "窗口、应用状态机和平台集成" },
    @{ Name = "area: rendering";      Color = "D4C5F9"; Description = "Direct2D、DWrite、D3D 和布局渲染" },
    @{ Name = "area: decoding";       Color = "C2E0C6"; Description = "WIC 解码、缩放和图片探测" },
    @{ Name = "area: indexing";       Color = "FEF2C0"; Description = "文件扫描、过滤、排序和路径索引" },
    @{ Name = "area: metadata";       Color = "F9D0C4"; Description = "ComfyUI、SD WebUI 和图片元数据" },
    @{ Name = "area: build";          Color = "0052CC"; Description = "CMake、本地构建和 GitHub Actions" },
    @{ Name = "risk: low";            Color = "C2E0C6"; Description = "低风险，局部且易回滚" },
    @{ Name = "risk: medium";         Color = "FBCA04"; Description = "中风险，需要受影响区域回归" },
    @{ Name = "risk: high";           Color = "B60205"; Description = "高风险，需要完整 QA 和明确回滚" },
    @{ Name = "status: blocked";      Color = "D93F0B"; Description = "存在阻塞，当前不可合并" },
    @{ Name = "status: needs-qa";     Color = "FBCA04"; Description = "等待手工 QA 证据" },
    @{ Name = "status: needs-review"; Color = "0E8A16"; Description = "等待独立 Code Review" }
)

foreach ($label in $labels) {
    $target = "$Repository label '$($label.Name)'"
    if ($PSCmdlet.ShouldProcess($target, "Create or update")) {
        & gh label create $label.Name `
            --repo $Repository `
            --color $label.Color `
            --description $label.Description `
            --force
        if ($LASTEXITCODE -ne 0) {
            throw "Failed to synchronize label '$($label.Name)'."
        }
    }
}
