param(
    [Parameter(Mandatory = $true)]
    [string]$RepositoryRoot,
    [Parameter(Mandatory = $true)]
    [string]$CMakeCommand,
    [Parameter(Mandatory = $true)]
    [string]$CTestCommand,
    [Parameter(Mandatory = $true)]
    [string]$BuildDirectory,
    [string]$Configuration = 'Release'
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

function Get-CTestTestNames {
    param(
        [string]$CTestCommand,
        [string]$BuildDirectory,
        [string]$Configuration
    )
    $arguments = @('--show-only=json-v1', '--test-dir', $BuildDirectory)
    if (-not [string]::IsNullOrWhiteSpace($Configuration)) {
        $arguments += @('-C', $Configuration)
    }
    $outputLines = @(& $CTestCommand @arguments 2>&1)
    $exitCode = $LASTEXITCODE
    $output = [string]::Join(
        "`n",
        @($outputLines | ForEach-Object { $_.ToString() }))
    if ($exitCode -ne 0) {
        throw "ctest show-only failed with exit code $exitCode`: $output"
    }
    try {
        $registry = $output | ConvertFrom-Json
    } catch {
        throw "ctest show-only returned invalid JSON: $($_.Exception.Message)"
    }
    if ($registry.kind -cne 'ctestInfo' -or $null -eq $registry.tests) {
        throw 'ctest show-only returned an unexpected schema'
    }
    $names = [System.Collections.Generic.List[string]]::new()
    foreach ($test in @($registry.tests)) {
        $name = [string]$test.name
        if ([string]::IsNullOrWhiteSpace($name)) {
            throw 'ctest show-only returned a test without a name'
        }
        $names.Add($name)
    }
    return $names.ToArray()
}

function Remove-MarkdownHiddenContent {
    param([string]$Content)
    $withoutHtmlComments = [regex]::Replace(
        $Content, '(?s)<!--.*?(?:-->|\z)', '')
    $visible = [System.Text.StringBuilder]::new()
    $inFence = $false
    $fenceCharacter = ''
    $fenceLength = 0
    foreach ($line in [regex]::Split($withoutHtmlComments, '(?<=\n)')) {
        if ($line.Length -eq 0) { continue }
        $lineText = $line -replace '\r?\n\z', ''
        if (-not $inFence) {
            $fenceStart = [regex]::Match(
                $lineText, '^[ \t]{0,3}(?<fence>`{3,}|~{3,})')
            if ($fenceStart.Success) {
                $marker = $fenceStart.Groups['fence'].Value
                $fenceCharacter = $marker.Substring(0, 1)
                $fenceLength = $marker.Length
                $inFence = $true
                continue
            }
            [void]$visible.Append($line)
            continue
        }
        $fenceEndPattern = '^[ \t]{0,3}' +
            [regex]::Escape($fenceCharacter) +
            '{' + $fenceLength + ',}[ \t]*$'
        if ([regex]::IsMatch($lineText, $fenceEndPattern)) {
            $inFence = $false
        }
    }
    return $visible.ToString()
}

function Get-ArchitectureTestNames {
    param([string]$Content)
    $visibleContent = Remove-MarkdownHiddenContent $Content
    $sectionHeader = ConvertFrom-Utf8Base64 'IyMg5b2T5YmN6Ieq5Yqo5YyW6L6555WM'
    $sectionMatch = [regex]::Match(
        $visibleContent,
        '(?ms)^' + [regex]::Escape($sectionHeader) +
            '\r?\n(?<section>.*?)(?=^##\s|\z)')
    if (-not $sectionMatch.Success) { return @() }
    return @([regex]::Matches(
        $sectionMatch.Groups['section'].Value,
        '(?m)^-\s+`([^`\r\n]+)`[^\r\n]*\r?$') |
        ForEach-Object { $_.Groups[1].Value })
}

function Get-OrdinalSortedStrings {
    param([string[]]$Values)
    [string[]]$sorted = @($Values)
    [System.Array]::Sort($sorted, [System.StringComparer]::Ordinal)
    return $sorted
}

function Test-TestRegistryContract {
    param(
        [string[]]$RegisteredNames,
        [string]$ArchitectureContent
    )
    $registered = @(Get-OrdinalSortedStrings $RegisteredNames)
    $documented = @(Get-OrdinalSortedStrings @(
        Get-ArchitectureTestNames $ArchitectureContent))
    return ($registered.Count -gt 0 -and
        $registered.Count -eq $documented.Count -and
        ($registered -join "`n") -ceq ($documented -join "`n"))
}

function Test-MutationChanged {
    param(
        [string]$Original,
        [string]$Mutated,
        [string]$MutationName
    )
    if ($Original -ceq $Mutated) {
        $failures.Add("$MutationName did not change its input")
        return $false
    }
    return $true
}

function Test-RegistryMultiplicity {
    param(
        [string[]]$Names,
        [string]$ExpectedName,
        [int]$ExpectedCount,
        [string]$MutationName
    )
    $actualCount = @($Names | Where-Object {
        $_ -ceq $ExpectedName
    }).Count
    if ($actualCount -ne $ExpectedCount) {
        $failures.Add(
            "$MutationName expected $ExpectedCount '$ExpectedName' registrations, got $actualCount")
        return $false
    }
    return $true
}

function Invoke-RegistryProbe {
    param(
        [string]$VariantName,
        [string]$CaseName,
        [string]$CMakeContent,
        [hashtable]$AdditionalFiles = @{}
    )
    $probeRoot = Join-Path $BuildDirectory (
        'governance-registry-probes\' + $VariantName + '\' + $CaseName)
    $sourceDirectory = Join-Path $probeRoot 'source'
    $probeBuildDirectory = Join-Path $probeRoot 'build'
    [void][System.IO.Directory]::CreateDirectory($sourceDirectory)
    [void][System.IO.Directory]::CreateDirectory($probeBuildDirectory)
    $utf8WithoutBom = [System.Text.UTF8Encoding]::new($false)
    [System.IO.File]::WriteAllText(
        (Join-Path $sourceDirectory 'CMakeLists.txt'),
        $CMakeContent,
        $utf8WithoutBom)
    $sourcePrefix = [System.IO.Path]::GetFullPath($sourceDirectory).
        TrimEnd('\') + '\'
    foreach ($relativePath in $AdditionalFiles.Keys) {
        $targetPath = [System.IO.Path]::GetFullPath(
            (Join-Path $sourceDirectory $relativePath))
        if (-not $targetPath.StartsWith(
            $sourcePrefix,
            [System.StringComparison]::OrdinalIgnoreCase)) {
            throw "probe file escaped source directory: $relativePath"
        }
        [void][System.IO.Directory]::CreateDirectory(
            [System.IO.Path]::GetDirectoryName($targetPath))
        [System.IO.File]::WriteAllText(
            $targetPath,
            [string]$AdditionalFiles[$relativePath],
            $utf8WithoutBom)
    }
    $writtenContent = [System.IO.File]::ReadAllText(
        (Join-Path $sourceDirectory 'CMakeLists.txt'),
        [System.Text.Encoding]::UTF8)
    if ($writtenContent -cne $CMakeContent) {
        $failures.Add("$VariantName $CaseName probe input changed while writing")
        return @()
    }
    $configureArguments = @(
        '-S', $sourceDirectory,
        '-B', $probeBuildDirectory,
        '-DBUILD_TESTING=ON'
    )
    $configureOutput = @(& $CMakeCommand @configureArguments 2>&1)
    $configureExitCode = $LASTEXITCODE
    if ($configureExitCode -ne 0) {
        $failures.Add(
            "$VariantName $CaseName probe configure failed: " +
            [string]::Join(
                "`n",
                @($configureOutput | ForEach-Object { $_.ToString() })))
        return @()
    }
    try {
        return @(Get-CTestTestNames `
            $CTestCommand `
            $probeBuildDirectory `
            $Configuration)
    } catch {
        $failures.Add(
            "$VariantName $CaseName probe registry failed: $($_.Exception.Message)")
        return @()
    }
}

function Test-TestRegistryMutationGuards {
    param(
        [string[]]$RegisteredNames,
        [string]$ArchitectureContent,
        [string]$LineEnding,
        [string]$VariantName
    )
    $checks = 0
    $bulletMatch = [regex]::Match(
        $ArchitectureContent,
        '(?m)^-\s+`app_state\.unit`[^\r\n]*(?:\r?\n)?')
    if (-not $bulletMatch.Success) {
        $failures.Add("$VariantName mutation setup cannot find app_state.unit bullet")
        return $checks
    }
    $bullet = $bulletMatch.Value -replace '\r?\n\z', ''
    $beforeBullet = $ArchitectureContent.Substring(0, $bulletMatch.Index)
    $afterBullet = $ArchitectureContent.Substring(
        $bulletMatch.Index + $bulletMatch.Length)

    $htmlCommentMutation = $beforeBullet + '<!--' + $LineEnding +
        $bullet + $LineEnding + '-->' + $LineEnding + $afterBullet
    $checks++
    if (Test-MutationChanged $ArchitectureContent $htmlCommentMutation `
        "$VariantName HTML-hidden bullet") {
        if (Test-TestRegistryContract $RegisteredNames $htmlCommentMutation) {
            $failures.Add("$VariantName accepted an HTML-commented test bullet")
        }
    }

    $backtickFenceMutation = $beforeBullet + '```text' + $LineEnding +
        $bullet + $LineEnding + '```' + $LineEnding + $afterBullet
    $checks++
    if (Test-MutationChanged $ArchitectureContent $backtickFenceMutation `
        "$VariantName backtick-fenced bullet") {
        if (Test-TestRegistryContract $RegisteredNames $backtickFenceMutation) {
            $failures.Add("$VariantName accepted a backtick-fenced test bullet")
        }
    }

    $tildeFenceMutation = $beforeBullet + '~~~text' + $LineEnding +
        $bullet + $LineEnding + '~~~' + $LineEnding + $afterBullet
    $checks++
    if (Test-MutationChanged $ArchitectureContent $tildeFenceMutation `
        "$VariantName tilde-fenced bullet") {
        if (Test-TestRegistryContract $RegisteredNames $tildeFenceMutation) {
            $failures.Add("$VariantName accepted a tilde-fenced test bullet")
        }
    }

    $removedBulletMutation = $beforeBullet + $afterBullet
    $checks++
    if (Test-MutationChanged $ArchitectureContent $removedBulletMutation `
        "$VariantName removed bullet") {
        if (Test-TestRegistryContract $RegisteredNames $removedBulletMutation) {
            $failures.Add("$VariantName accepted a removed architecture test")
        }
    }

    $addedBulletMutation = $beforeBullet + $bullet + $LineEnding +
        '- `mutation.extra`: extra.' + $LineEnding + $afterBullet
    $checks++
    if (Test-MutationChanged $ArchitectureContent $addedBulletMutation `
        "$VariantName added bullet") {
        if (Test-TestRegistryContract $RegisteredNames $addedBulletMutation) {
            $failures.Add("$VariantName accepted an extra architecture test")
        }
    }

    $duplicateBulletMutation = $beforeBullet + $bullet + $LineEnding +
        $bullet + $LineEnding + $afterBullet
    $checks++
    if (Test-MutationChanged $ArchitectureContent $duplicateBulletMutation `
        "$VariantName duplicate architecture bullet") {
        if (Test-TestRegistryContract $RegisteredNames $duplicateBulletMutation) {
            $failures.Add("$VariantName accepted a duplicate architecture test")
        }
    }

    $probeSectionHeader =
        ConvertFrom-Utf8Base64 'IyMg5b2T5YmN6Ieq5Yqo5YyW6L6555WM'
    $probeArchitecture = $probeSectionHeader + $LineEnding +
        '- `probe.unit`: probe.' + $LineEnding
    $baseCMake = @(
        'cmake_minimum_required(VERSION 3.20)',
        'project(GovernanceRegistryProbe NONE)',
        'include(CTest)',
        'add_test(NAME probe.unit COMMAND "${CMAKE_COMMAND}" -E echo probe)',
        '# MUTATION_POINT'
    ) -join $LineEnding
    $baseCMake += $LineEnding

    $escapeReplacement = @(
        'set(quoted_escape "literal quote: \" and add_test(NAME quoted.decoy COMMAND false)")',
        'set(unquoted_escape literal\(paren\)\ value\#hash)',
        'add_test(NAME after.quoted.escape COMMAND "${CMAKE_COMMAND}" -E echo quoted)',
        'add_test(NAME after.unquoted.escape COMMAND "${CMAKE_COMMAND}" -E echo unquoted)'
    ) -join $LineEnding
    $escapeMutation = $baseCMake.Replace(
        '# MUTATION_POINT', $escapeReplacement)
    $checks++
    if (Test-MutationChanged $baseCMake $escapeMutation `
        "$VariantName escape registry") {
        $escapeNames = @(Invoke-RegistryProbe `
            $VariantName 'escape' $escapeMutation)
        [void](Test-RegistryMultiplicity $escapeNames `
            'after.quoted.escape' 1 "$VariantName quoted escape")
        [void](Test-RegistryMultiplicity $escapeNames `
            'after.unquoted.escape' 1 "$VariantName unquoted escape")
        [void](Test-RegistryMultiplicity $escapeNames `
            'quoted.decoy' 0 "$VariantName quoted escape decoy")
        if (Test-TestRegistryContract $escapeNames $probeArchitecture) {
            $failures.Add("$VariantName accepted undocumented tests after escapes")
        }
    }

    $quotedMutation = $baseCMake.Replace(
        '# MUTATION_POINT',
        'add_test(NAME "mutation hidden" COMMAND "${CMAKE_COMMAND}" -E echo quoted-name)')
    $checks++
    $quotedNames = @()
    if (Test-MutationChanged $baseCMake $quotedMutation `
        "$VariantName quoted test name") {
        $quotedNames = @(Invoke-RegistryProbe `
            $VariantName 'quoted-name' $quotedMutation)
        [void](Test-RegistryMultiplicity $quotedNames `
            'mutation hidden' 1 "$VariantName quoted test name")
        if (Test-TestRegistryContract $quotedNames $probeArchitecture) {
            $failures.Add("$VariantName ignored an undocumented quoted test name")
        }
    }

    $quotedArchitecture = $probeArchitecture +
        '- `mutation hidden`: quoted name.' + $LineEnding
    $checks++
    if (Test-MutationChanged $probeArchitecture $quotedArchitecture `
        "$VariantName documented quoted test name") {
        if (-not (Test-TestRegistryContract `
            $quotedNames $quotedArchitecture)) {
            $failures.Add("$VariantName rejected a documented quoted test name")
        }
    }

    $bracketMutation = $baseCMake.Replace(
        '# MUTATION_POINT',
        'add_test(NAME [=[mutation bracket]=] COMMAND "${CMAKE_COMMAND}" -E echo bracket-name)')
    $checks++
    $bracketNames = @()
    if (Test-MutationChanged $baseCMake $bracketMutation `
        "$VariantName bracket test name") {
        $bracketNames = @(Invoke-RegistryProbe `
            $VariantName 'bracket-name' $bracketMutation)
        [void](Test-RegistryMultiplicity $bracketNames `
            'mutation bracket' 1 "$VariantName bracket test name")
        if (Test-TestRegistryContract $bracketNames $probeArchitecture) {
            $failures.Add("$VariantName ignored an undocumented bracket test name")
        }
    }

    $bracketArchitecture = $probeArchitecture +
        '- `mutation bracket`: bracket name.' + $LineEnding
    $checks++
    if (Test-MutationChanged $probeArchitecture $bracketArchitecture `
        "$VariantName documented bracket test name") {
        if (-not (Test-TestRegistryContract `
            $bracketNames $bracketArchitecture)) {
            $failures.Add("$VariantName rejected a documented bracket test name")
        }
    }

    $duplicateReplacement = @(
        'add_subdirectory(first)',
        'add_subdirectory(second)'
    ) -join $LineEnding
    $duplicateMutation = $baseCMake.Replace(
        '# MUTATION_POINT', $duplicateReplacement)
    $duplicateArchitecture = $probeArchitecture +
        '- `duplicate.cross`: duplicate probe.' + $LineEnding
    $checks++
    if ((Test-MutationChanged $baseCMake $duplicateMutation `
            "$VariantName duplicate CMake registration") -and
        (Test-MutationChanged $probeArchitecture $duplicateArchitecture `
            "$VariantName duplicate probe architecture")) {
        $subdirectoryContent =
            'add_test(NAME duplicate.cross COMMAND ' +
            '"${CMAKE_COMMAND}" -E echo duplicate)' +
            $LineEnding
        $duplicateNames = @(Invoke-RegistryProbe `
            $VariantName 'duplicate' $duplicateMutation @{
                'first\CMakeLists.txt' = $subdirectoryContent
                'second\CMakeLists.txt' = $subdirectoryContent
            })
        [void](Test-RegistryMultiplicity $duplicateNames `
            'duplicate.cross' 2 "$VariantName duplicate CMake registration")
        if (Test-TestRegistryContract `
            $duplicateNames $duplicateArchitecture) {
            $failures.Add("$VariantName accepted duplicate CMake tests")
        }
    }
    return $checks
}

function Test-NoLegacyMetadataLog {
    param([string]$Content)
    return $Content -notmatch 'meta_debug\.log'
}

$areaPathLabelPrefix = "$([char]0x533a)$([char]0x57df)$([char]0x8def)$([char]0x5f84)$([char]0x6807)$([char]0x7b7e)$([char]0x7531)"
$architecturePath = Join-Path $RepositoryRoot 'docs/ARCHITECTURE.md'
$architectureContent = Get-Content -Raw -Encoding UTF8 -LiteralPath $architecturePath

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

$registeredTests = @()
try {
    $registeredTests = @(Get-CTestTestNames `
        $CTestCommand $BuildDirectory $Configuration)
} catch {
    $failures.Add("configured CTest registry read failed: $($_.Exception.Message)")
}
if (-not (Test-TestRegistryContract `
    $registeredTests $architectureContent)) {
    $failures.Add(
        'architecture test declarations must exactly match the configured CTest registry')
}

# Mutation checks exercise the production live-registry reader and contract.
$lfArchitectureContent = $architectureContent -replace "`r`n", "`n"
$registryVariants = @(
    [pscustomobject]@{
        Name = 'LF'
        LineEnding = "`n"
        Architecture = $lfArchitectureContent
    },
    [pscustomobject]@{
        Name = 'CRLF'
        LineEnding = "`r`n"
        Architecture = $lfArchitectureContent -replace "`n", "`r`n"
    }
)
$registryMutationChecks = 0
foreach ($variant in $registryVariants) {
    if (-not (Test-TestRegistryContract `
        $registeredTests $variant.Architecture)) {
        $failures.Add("$($variant.Name) test registry contract must pass")
    }
    $registryMutationChecks += Test-TestRegistryMutationGuards `
        $registeredTests `
        $variant.Architecture `
        $variant.LineEnding `
        $variant.Name
}
if ($registryMutationChecks -ne 24) {
    $failures.Add('test registry mutation suite must execute 24 checks')
}

$negatedWorkerMutation = '- 1 metadata worker does not exist; UI parsing does not use mutex/condition variable, WM_METADATA_READY, or path expiry.'
if (Test-MetadataWorkerStructure $negatedWorkerMutation) {
    $failures.Add('metadata worker mutation guard accepted a negated worker claim')
}
$legacyLogMutation = $architectureContent + "`r`nmeta_debug.log"
if (Test-NoLegacyMetadataLog $legacyLogMutation) {
    $failures.Add('metadata log mutation guard accepted the legacy debug-log claim')
}

if ($failures.Count -gt 0) {
    foreach ($failure in $failures) { Write-Output "FAIL: $failure" }
    exit 1
}

Write-Output (
    "governance tests passed (live registry $($registeredTests.Count), " +
    "$($registryVariants.Count) registry variants, " +
    "$registryMutationChecks registry mutations)")
