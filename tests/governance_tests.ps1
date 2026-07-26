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

function Get-CMakeTokens {
    param([string]$Content)

    $tokens = [System.Collections.Generic.List[object]]::new()
    $index = 0
    while ($index -lt $Content.Length) {
        $character = $Content[$index]
        if ([char]::IsWhiteSpace($character)) {
            $index++
            continue
        }

        if ($character -eq '#') {
            $commentStart = ([regex]'\G#\[(=*)\[').Match(
                $Content, $index)
            if ($commentStart.Success) {
                $commentEnd = ']' + $commentStart.Groups[1].Value + ']'
                $endIndex = $Content.IndexOf(
                    $commentEnd,
                    $index + $commentStart.Length,
                    [System.StringComparison]::Ordinal)
                if ($endIndex -lt 0) { break }
                $index = $endIndex + $commentEnd.Length
                continue
            }

            while ($index -lt $Content.Length -and
                $Content[$index] -ne "`r" -and
                $Content[$index] -ne "`n") {
                $index++
            }
            continue
        }

        if ($character -eq '"') {
            $value = [System.Text.StringBuilder]::new()
            $index++
            while ($index -lt $Content.Length) {
                $character = $Content[$index]
                if ($character -eq '\\' -and $index + 1 -lt $Content.Length) {
                    $index++
                    [void]$value.Append($Content[$index])
                    $index++
                    continue
                }
                if ($character -eq '"') {
                    $index++
                    break
                }
                [void]$value.Append($character)
                $index++
            }
            [void]$tokens.Add([pscustomobject]@{
                Type = 'Quoted'
                Value = $value.ToString()
            })
            continue
        }

        if ($character -eq '[') {
            $bracketStart = ([regex]'\G\[(=*)\[').Match(
                $Content, $index)
            if ($bracketStart.Success) {
                $bracketEnd = ']' + $bracketStart.Groups[1].Value + ']'
                $valueStart = $index + $bracketStart.Length
                $endIndex = $Content.IndexOf(
                    $bracketEnd,
                    $valueStart,
                    [System.StringComparison]::Ordinal)
                if ($endIndex -lt 0) {
                    $value = $Content.Substring($valueStart)
                    $index = $Content.Length
                } else {
                    $value = $Content.Substring(
                        $valueStart, $endIndex - $valueStart)
                    $index = $endIndex + $bracketEnd.Length
                }
                [void]$tokens.Add([pscustomobject]@{
                    Type = 'Bracket'
                    Value = $value
                })
                continue
            }
        }

        if ($character -eq '(' -or $character -eq ')') {
            [void]$tokens.Add([pscustomobject]@{
                Type = if ($character -eq '(') { 'LeftParen' } else { 'RightParen' }
                Value = [string]$character
            })
            $index++
            continue
        }

        $start = $index
        while ($index -lt $Content.Length) {
            $character = $Content[$index]
            if ([char]::IsWhiteSpace($character) -or
                $character -eq '(' -or
                $character -eq ')' -or
                $character -eq '"' -or
                $character -eq '#') {
                break
            }
            $index++
        }
        if ($index -eq $start) {
            $index++
            continue
        }
        [void]$tokens.Add([pscustomobject]@{
            Type = 'Word'
            Value = $Content.Substring($start, $index - $start)
        })
    }

    return $tokens.ToArray()
}

function Get-CMakeTestNames {
    param([string]$Content)

    $tokens = @(Get-CMakeTokens $Content)
    $names = [System.Collections.Generic.List[string]]::new()
    $depth = 0
    for ($index = 0; $index -lt $tokens.Count; $index++) {
        $token = $tokens[$index]
        if ($depth -eq 0 -and
            $token.Type -eq 'Word' -and
            $token.Value -ieq 'add_test' -and
            $index + 3 -lt $tokens.Count -and
            $tokens[$index + 1].Type -eq 'LeftParen' -and
            $tokens[$index + 2].Type -eq 'Word' -and
            $tokens[$index + 2].Value -ieq 'NAME' -and
            $tokens[$index + 3].Type -in @('Word', 'Quoted', 'Bracket') -and
            $tokens[$index + 3].Value -match '^[A-Za-z0-9_.-]+$') {
            [void]$names.Add($tokens[$index + 3].Value)
        }

        if ($token.Type -eq 'LeftParen') {
            $depth++
        } elseif ($token.Type -eq 'RightParen' -and $depth -gt 0) {
            $depth--
        }
    }

    return @($names | Sort-Object -Unique)
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
        '(?m)^-\s+`([A-Za-z0-9_.-]+)`[^\r\n]*\r?$') |
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

function Test-TestRegistryMutationGuards {
    param(
        [string]$CMakeContent,
        [string]$ArchitectureContent,
        [string]$LineEnding,
        [string]$VariantName
    )

    $checks = 0
    $activeRegistration =
        '    add_test(NAME app_state.unit COMMAND app_state_tests)'
    if (-not $CMakeContent.Contains($activeRegistration)) {
        $failures.Add("$VariantName mutation setup cannot find app_state.unit")
        return $checks
    }

    $commentedOutMutation = $CMakeContent.Replace(
        $activeRegistration,
        '    # add_test(NAME app_state.unit COMMAND app_state_tests)')
    $checks++
    if (Test-TestRegistryContract $commentedOutMutation $ArchitectureContent) {
        $failures.Add("$VariantName accepted a commented-out real CMake test")
    }

    $stringifiedMutation = $CMakeContent.Replace(
        $activeRegistration,
        '    set(app_state_registration "add_test(NAME app_state.unit COMMAND app_state_tests)")')
    $checks++
    if (Test-TestRegistryContract $stringifiedMutation $ArchitectureContent) {
        $failures.Add("$VariantName accepted a stringified replacement test")
    }

    $lineCommentMutation = $CMakeContent + $LineEnding +
        '# add_test(NAME mutation.extra COMMAND mutation_extra)'
    $checks++
    if (-not (Test-TestRegistryContract $lineCommentMutation $ArchitectureContent)) {
        $failures.Add("$VariantName rejected a harmless CMake line comment")
    }

    $bracketCommentMutation = $CMakeContent + $LineEnding + '#[=[' +
        $LineEnding + 'add_test(NAME mutation.extra COMMAND mutation_extra)' +
        $LineEnding + ']=]'
    $checks++
    if (-not (Test-TestRegistryContract $bracketCommentMutation $ArchitectureContent)) {
        $failures.Add("$VariantName rejected a harmless CMake bracket comment")
    }

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
    if (Test-TestRegistryContract $CMakeContent $htmlCommentMutation) {
        $failures.Add("$VariantName accepted an HTML-commented test bullet")
    }

    $fencedMutation = $beforeBullet + '```text' + $LineEnding +
        $bullet + $LineEnding + '```' + $LineEnding + $afterBullet
    $checks++
    if (Test-TestRegistryContract $CMakeContent $fencedMutation) {
        $failures.Add("$VariantName accepted a fenced test bullet")
    }

    $removedBulletMutation = $beforeBullet + $afterBullet
    $checks++
    if (Test-TestRegistryContract $CMakeContent $removedBulletMutation) {
        $failures.Add("$VariantName accepted a removed architecture test")
    }

    $addedTestMutation = $CMakeContent + $LineEnding +
        'add_test(NAME mutation.extra COMMAND mutation_extra)'
    $checks++
    if (Test-TestRegistryContract $addedTestMutation $ArchitectureContent) {
        $failures.Add("$VariantName accepted an undocumented active CMake test")
    }

    return $checks
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

# Mutation checks prove the predicates reject lexical decoys and registry drift.
$lfCMakeContent = $cmakeContent -replace "`r`n", "`n"
$lfArchitectureContent = $architectureContent -replace "`r`n", "`n"
$registryVariants = @(
    [pscustomobject]@{
        Name = 'LF'
        LineEnding = "`n"
        CMake = $lfCMakeContent
        Architecture = $lfArchitectureContent
    },
    [pscustomobject]@{
        Name = 'CRLF'
        LineEnding = "`r`n"
        CMake = $lfCMakeContent -replace "`n", "`r`n"
        Architecture = $lfArchitectureContent -replace "`n", "`r`n"
    }
)
$registryMutationChecks = 0
foreach ($variant in $registryVariants) {
    if (-not (Test-TestRegistryContract $variant.CMake $variant.Architecture)) {
        $failures.Add("$($variant.Name) test registry contract must pass")
    }
    $registryMutationChecks += Test-TestRegistryMutationGuards `
        $variant.CMake `
        $variant.Architecture `
        $variant.LineEnding `
        $variant.Name
}
if ($registryMutationChecks -ne 16) {
    $failures.Add('test registry mutation suite must execute 16 checks')
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

Write-Output "governance tests passed ($($registryVariants.Count) registry variants, $registryMutationChecks registry mutations)"
