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

function ConvertFrom-Utf8Base64 {
    param([string]$Value)
    return [System.Text.Encoding]::UTF8.GetString(
        [System.Convert]::FromBase64String($Value))
}

function Test-MetadataWorkerStructure {
    param([string]$Content)

    # Exact UTF-8 architecture line, encoded to keep this script ASCII-only.
    $expectedLine = ConvertFrom-Utf8Base64 'LSDnlJ/miJDkv6Hmga/nlLEgMSDkuKogYG1ldGFkYXRhIHdvcmtlcmAg5ZyoIFVJIOe6v+eoi+Wkluino+aekO+8m+ivt+axguOAgee7k+aenOWSjCByZWFkeSDnirbmgIHnlLEgbXV0ZXgvY29uZGl0aW9uIHZhcmlhYmxlIOS/neaKpO+8jOmAmui/hyBgV01fTUVUQURBVEFfUkVBRFlgIOWbnuWIsOeql+WPo+e6v+eoi++8jOW5tuaMieW9k+WJjemdouadvyBwYXRoIOS4ouW8g+i/h+acn+e7k+aenOOAgg=='
    return [regex]::IsMatch(
        $Content, '(?m)^' + [regex]::Escape($expectedLine) + '\r?$')
}

function Get-CMakeTestNames {
    param([string]$Content)

    return @([regex]::Matches(
        $Content,
        '(?is)add_test\s*\(\s*NAME\s+"?([A-Za-z0-9_.-]+)"?(?=\s|\))') |
        ForEach-Object { $_.Groups[1].Value } |
        Sort-Object -Unique)
}

function Get-ArchitectureTestNames {
    param([string]$Content)

    $sectionHeader = ConvertFrom-Utf8Base64 'IyMg5b2T5YmN6Ieq5Yqo5YyW6L6555WM'
    $sectionMatch = [regex]::Match(
        $Content,
        '(?ms)^' + [regex]::Escape($sectionHeader) +
            '\r?\n(?<section>.*?)(?=^##\s|\z)')
    if (-not $sectionMatch.Success) { return @() }

    return @([regex]::Matches(
        $sectionMatch.Groups['section'].Value,
        '(?m)^-\s+`([A-Za-z0-9_.-]+)`[^\r\n]*$') |
        ForEach-Object { $_.Groups[1].Value } |
        Sort-Object -Unique)
}

function Test-TestRegistryContract {
    param(
        [string]$CMakeContent,
        [string]$ArchitectureContent
    )

    $registered = @(Get-CMakeTestNames $CMakeContent)
    $documented = @(Get-ArchitectureTestNames $ArchitectureContent)
    return ($registered.Count -gt 0 -and
        ($registered -join "`n") -ceq ($documented -join "`n"))
}

function Test-NoLegacyMetadataLog {
    param([string]$Content)
    return $Content -notmatch 'meta_debug\.log'
}

$areaPathLabelPrefix = "$([char]0x533a)$([char]0x57df)$([char]0x8def)$([char]0x5f84)$([char]0x6807)$([char]0x7b7e)$([char]0x7531)"
$architecturePath = Join-Path $RepositoryRoot 'docs/ARCHITECTURE.md'
$cmakePath = Join-Path $RepositoryRoot 'CMakeLists.txt'
$architectureContent = Get-Content -Raw -Encoding UTF8 -LiteralPath $architecturePath
$cmakeContent = Get-Content -Raw -Encoding UTF8 -LiteralPath $cmakePath

Assert-Present (Join-Path $RepositoryRoot 'build.bat') '(?s)copy /Y .*?if errorlevel 1 exit /b 1\s+echo BUILD_OK' 'build.bat must stop before BUILD_OK when the candidate copy fails'
Assert-Absent (Join-Path $RepositoryRoot '.github/labeler.yml') '(?m)^(documentation|ci|tests):' 'path labeler must not manage type labels'
Assert-Present (Join-Path $RepositoryRoot 'CONTRIBUTING.md') ([regex]::Escape($areaPathLabelPrefix)) 'CONTRIBUTING must limit automatic labels to area paths'
Assert-Present (Join-Path $RepositoryRoot 'docs/MULTI_FORMAT_RESEARCH.md') 'Product Owner' 'format research must remain a Product Owner recommendation'
Assert-Present (Join-Path $RepositoryRoot 'docs/MULTI_FORMAT_RESEARCH.md') 'Issue #4' 'format research must preserve the Issue #4 implementation gate'
Assert-Present (Join-Path $RepositoryRoot 'README.md') '#1A1A1A' 'README must use the authoritative background color'
Assert-Absent (Join-Path $RepositoryRoot 'README.md') '#1A1A1E|`F2`' 'README must not advertise stale color or F2 behavior'
Assert-Present $architecturePath '3[^\r\n]*WIC' 'architecture must match the three-entry preload cache'
Assert-Absent $architecturePath '6[^\r\n]*WIC' 'architecture must not advertise a six-entry preload cache'
if (-not (Test-NoLegacyMetadataLog $architectureContent)) {
    $failures.Add('architecture must not retain the removed metadata debug-log claim')
}
if (-not (Test-MetadataWorkerStructure $architectureContent)) {
    $failures.Add('architecture must preserve the complete positive metadata worker contract')
}
if (-not (Test-TestRegistryContract $cmakeContent $architectureContent)) {
    $failures.Add('architecture test declarations must exactly match CMake add_test registrations')
}

# Mutation checks prove the predicates reject keyword-preserving denials and drift.
$negatedWorkerMutation = '- 1 metadata worker does not exist; UI parsing does not use mutex/condition variable, WM_METADATA_READY, or path expiry.'
if (Test-MetadataWorkerStructure $negatedWorkerMutation) {
    $failures.Add('metadata worker mutation guard accepted a negated worker claim')
}

$registeredTests = @(Get-CMakeTestNames $cmakeContent)
if ($registeredTests.Count -gt 0) {
    $removedTestPattern = '(?m)^-\s+`' +
        [regex]::Escape($registeredTests[0]) + '`[^\r\n]*(?:\r?\n)?'
    $removedTestMutation = [regex]::Replace(
        $architectureContent, $removedTestPattern, '', 1)
    if (Test-TestRegistryContract $cmakeContent $removedTestMutation) {
        $failures.Add('test registry mutation guard accepted a removed architecture test')
    }
}

$addedTestMutation = $cmakeContent +
    "`r`nadd_test(NAME mutation.extra COMMAND mutation_extra)"
if (Test-TestRegistryContract $addedTestMutation $architectureContent) {
    $failures.Add('test registry mutation guard accepted an undocumented CMake test')
}

$legacyLogMutation = $architectureContent + "`r`nmeta_debug.log"
if (Test-NoLegacyMetadataLog $legacyLogMutation) {
    $failures.Add('metadata log mutation guard accepted the legacy debug-log claim')
}

if ($failures.Count -gt 0) {
    foreach ($failure in $failures) { Write-Output "FAIL: $failure" }
    exit 1
}

Write-Output 'governance tests passed'
