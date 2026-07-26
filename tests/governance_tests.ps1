param(
    [Parameter(Mandatory = $true)]
    [string]$RepositoryRoot
)

$ErrorActionPreference = 'Stop'
$failures = [System.Collections.Generic.List[string]]::new()

function Assert-Present {
    param(
        [string]$Path,
        [string]$Pattern,
        [string]$FailureMessage
    )

    $content = Get-Content -Raw -Encoding UTF8 -LiteralPath $Path
    if ($content -notmatch $Pattern) { $failures.Add($FailureMessage) }
}

function Assert-Absent {
    param(
        [string]$Path,
        [string]$Pattern,
        [string]$FailureMessage
    )

    $content = Get-Content -Raw -Encoding UTF8 -LiteralPath $Path
    if ($content -match $Pattern) { $failures.Add($FailureMessage) }
}

$areaPathLabelPrefix = "$([char]0x533a)$([char]0x57df)$([char]0x8def)$([char]0x5f84)$([char]0x6807)$([char]0x7b7e)$([char]0x7531)"

Assert-Present (Join-Path $RepositoryRoot 'build.bat') '(?s)copy /Y .*?if errorlevel 1 exit /b 1\s+echo BUILD_OK' 'build.bat must stop before BUILD_OK when the candidate copy fails'
Assert-Absent (Join-Path $RepositoryRoot '.github/labeler.yml') '(?m)^(documentation|ci|tests):' 'path labeler must not manage type labels'
Assert-Present (Join-Path $RepositoryRoot 'CONTRIBUTING.md') ([regex]::Escape($areaPathLabelPrefix)) 'CONTRIBUTING must limit automatic labels to area paths'
Assert-Present (Join-Path $RepositoryRoot 'docs/MULTI_FORMAT_RESEARCH.md') 'Product Owner' 'format research must remain a Product Owner recommendation'
Assert-Present (Join-Path $RepositoryRoot 'docs/MULTI_FORMAT_RESEARCH.md') 'Issue #4' 'format research must preserve the Issue #4 implementation gate'
Assert-Present (Join-Path $RepositoryRoot 'README.md') '#1A1A1A' 'README must use the authoritative background color'
Assert-Absent (Join-Path $RepositoryRoot 'README.md') '#1A1A1E|`F2`' 'README must not advertise stale color or F2 behavior'
Assert-Present (Join-Path $RepositoryRoot 'docs/ARCHITECTURE.md') '3[^\r\n]*WIC' 'architecture must match the three-entry preload cache'
Assert-Absent (Join-Path $RepositoryRoot 'docs/ARCHITECTURE.md') '6[^\r\n]*WIC' 'architecture must not advertise a six-entry preload cache'

if ($failures.Count -gt 0) {
    foreach ($failure in $failures) { Write-Output "FAIL: $failure" }
    exit 1
}

Write-Output 'governance tests passed'
