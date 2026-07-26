param(
    [Parameter(Mandatory = $true)]
    [string]$RepositoryRoot,
    [Parameter(Mandatory = $true)]
    [string]$CMakeCommand,
    [Parameter(Mandatory = $true)]
    [string]$CTestCommand,
    [Parameter(Mandatory = $true)]
    [string]$BuildDirectory,
    [string]$Configuration = 'Release',
    [switch]$RegistryConcurrencyWorker
)

$ErrorActionPreference = 'Stop'
$failures = [System.Collections.Generic.List[string]]::new()
$script:RegistryProbeInvocationRoot = $null
$script:ProductionProcessTimeoutMilliseconds = 30000
$script:MinimumProcessTimeoutMilliseconds = 100
$script:ConcurrencyWorkerTimeoutMilliseconds = 60000
$script:TaskkillTimeoutMilliseconds = 5000
$script:ProcessExitConfirmationTimeoutMilliseconds = 5000
$script:OutputDrainTimeoutMilliseconds = 5000
$script:HangRegressionTimeoutMilliseconds = 1500
$script:HangSentinelDelayMilliseconds = 2000
$script:LargeOutputLength = 262144
$script:GovernanceStopwatch = [System.Diagnostics.Stopwatch]::StartNew()

function ConvertTo-WindowsCommandLineArgument {
    param([string]$Value)
    if ($null -eq $Value) {
        throw 'process arguments must not contain null values'
    }
    if ($Value.Length -eq 0) { return '""' }
    if ($Value -notmatch '[\s"]') { return $Value }

    $quoted = [System.Text.StringBuilder]::new()
    [void]$quoted.Append('"')
    $backslashes = 0
    foreach ($character in $Value.ToCharArray()) {
        if ($character -eq '\') {
            $backslashes++
            continue
        }
        if ($character -eq '"') {
            [void]$quoted.Append(('\' * (2 * $backslashes + 1)))
            [void]$quoted.Append('"')
            $backslashes = 0
            continue
        }
        if ($backslashes -gt 0) {
            [void]$quoted.Append(('\' * $backslashes))
            $backslashes = 0
        }
        [void]$quoted.Append($character)
    }
    if ($backslashes -gt 0) {
        [void]$quoted.Append(('\' * (2 * $backslashes)))
    }
    [void]$quoted.Append('"')
    return $quoted.ToString()
}

function ConvertTo-WindowsCommandLine {
    param([string[]]$Arguments)
    return (@($Arguments | ForEach-Object {
        ConvertTo-WindowsCommandLineArgument $_
    }) -join ' ')
}

function New-RedirectedProcessStartInfo {
    param(
        [string]$FileName,
        [string[]]$Arguments,
        [string]$WorkingDirectory
    )
    $startInfo = [System.Diagnostics.ProcessStartInfo]::new()
    $startInfo.FileName = $FileName
    $startInfo.Arguments = ConvertTo-WindowsCommandLine $Arguments
    $startInfo.WorkingDirectory = $WorkingDirectory
    $startInfo.UseShellExecute = $false
    $startInfo.CreateNoWindow = $true
    $startInfo.RedirectStandardOutput = $true
    $startInfo.RedirectStandardError = $true
    $startInfo.StandardOutputEncoding =
        [System.Text.UTF8Encoding]::new($false)
    $startInfo.StandardErrorEncoding =
        [System.Text.UTF8Encoding]::new($false)
    return $startInfo
}

function Complete-RedirectedTextTask {
    param(
        [object]$Task,
        [string]$StreamName,
        [System.Collections.Generic.List[string]]$Diagnostics
    )
    try {
        if (-not $Task.Wait($script:OutputDrainTimeoutMilliseconds)) {
            $Diagnostics.Add(
                "$StreamName drain exceeded $($script:OutputDrainTimeoutMilliseconds) ms")
            return ''
        }
        return [string]$Task.Result
    } catch {
        $Diagnostics.Add("$StreamName drain failed: $($_.Exception.Message)")
        return ''
    }
}

function Invoke-BoundedTaskkill {
    param([int]$OwnedProcessId)
    $taskkillPath = Join-Path $env:SystemRoot 'System32\taskkill.exe'
    if (-not (Test-Path -LiteralPath $taskkillPath -PathType Leaf)) {
        return [pscustomobject]@{
            Started = $false
            ExitCode = $null
            TimedOut = $false
            OutputDrainSucceeded = $false
            Stdout = ''
            Stderr = ''
            Diagnostic = "taskkill executable not found: $taskkillPath"
        }
    }

    $process = [System.Diagnostics.Process]::new()
    $started = $false
    $stdoutTask = $null
    $stderrTask = $null
    $diagnostics = [System.Collections.Generic.List[string]]::new()
    $timedOut = $false
    $exitCode = $null
    try {
        $process.StartInfo = New-RedirectedProcessStartInfo `
            $taskkillPath `
            @('/PID', [string]$OwnedProcessId, '/T', '/F') `
            $RepositoryRoot
        try {
            $started = $process.Start()
        } catch {
            $diagnostics.Add("taskkill start failed: $($_.Exception.Message)")
        }
        if (-not $started) {
            if ($diagnostics.Count -eq 0) {
                $diagnostics.Add('taskkill start returned false')
            }
            return [pscustomobject]@{
                Started = $false
                ExitCode = $null
                TimedOut = $false
                OutputDrainSucceeded = $false
                Stdout = ''
                Stderr = ''
                Diagnostic = [string]::Join('; ', $diagnostics.ToArray())
            }
        }

        $stdoutTask = $process.StandardOutput.ReadToEndAsync()
        $stderrTask = $process.StandardError.ReadToEndAsync()
        if (-not $process.WaitForExit($script:TaskkillTimeoutMilliseconds)) {
            $timedOut = $true
            $diagnostics.Add(
                "taskkill timed out after $($script:TaskkillTimeoutMilliseconds) ms")
            try {
                $process.Kill()
            } catch {
                $diagnostics.Add(
                    "taskkill fallback Kill failed: $($_.Exception.Message)")
            }
            if (-not $process.WaitForExit(
                    $script:ProcessExitConfirmationTimeoutMilliseconds)) {
                $diagnostics.Add('taskkill process did not exit after fallback Kill')
            }
        }
        if ($process.HasExited) { $exitCode = $process.ExitCode }
        $stdout = Complete-RedirectedTextTask `
            $stdoutTask 'taskkill stdout' $diagnostics
        $stderr = Complete-RedirectedTextTask `
            $stderrTask 'taskkill stderr' $diagnostics
        $drainSucceeded = -not (@($diagnostics | Where-Object {
            $_ -match 'drain (?:exceeded|failed)'
        }).Count -gt 0)
        if ($null -ne $exitCode -and $exitCode -ne 0) {
            $diagnostics.Add("taskkill exited with code $exitCode")
        }
        return [pscustomobject]@{
            Started = $true
            ExitCode = $exitCode
            TimedOut = $timedOut
            OutputDrainSucceeded = $drainSucceeded
            Stdout = $stdout
            Stderr = $stderr
            Diagnostic = [string]::Join('; ', $diagnostics.ToArray())
        }
    } finally {
        $process.Dispose()
    }
}

function Stop-OwnedProcessTree {
    param(
        [System.Diagnostics.Process]$Process,
        [string]$Operation
    )
    try {
        if ($Process.HasExited) {
            return [pscustomobject]@{
                Succeeded = $true
                FallbackUsed = $false
                Diagnostic = 'owned process had already exited'
            }
        }
    } catch {
        return [pscustomobject]@{
            Succeeded = $false
            FallbackUsed = $false
            Diagnostic = "$Operation process state read failed: $($_.Exception.Message)"
        }
    }

    $taskkill = Invoke-BoundedTaskkill $Process.Id
    $details = [System.Collections.Generic.List[string]]::new()
    if (-not [string]::IsNullOrWhiteSpace($taskkill.Diagnostic)) {
        $details.Add($taskkill.Diagnostic)
    }
    if (-not [string]::IsNullOrWhiteSpace($taskkill.Stderr)) {
        $details.Add("taskkill stderr: $($taskkill.Stderr)")
    }
    $taskkillSucceeded = $taskkill.Started -and
        -not $taskkill.TimedOut -and
        $taskkill.OutputDrainSucceeded -and
        $taskkill.ExitCode -eq 0
    $parentExited = $Process.WaitForExit(
        $script:ProcessExitConfirmationTimeoutMilliseconds)
    $fallbackUsed = $false
    if (-not $parentExited) {
        $fallbackUsed = $true
        $details.Add('owned parent did not exit after taskkill; using parent Kill fallback')
        try {
            $Process.Kill()
        } catch {
            $details.Add("owned parent Kill fallback failed: $($_.Exception.Message)")
        }
        $parentExited = $Process.WaitForExit(
            $script:ProcessExitConfirmationTimeoutMilliseconds)
        if (-not $parentExited) {
            $details.Add('owned parent remained after Kill fallback')
        }
    }
    if (-not $taskkillSucceeded) {
        $details.Add('owned process-tree taskkill did not complete successfully')
    }
    return [pscustomobject]@{
        Succeeded = $taskkillSucceeded -and $parentExited -and -not $fallbackUsed
        FallbackUsed = $fallbackUsed
        Diagnostic = [string]::Join('; ', $details.ToArray())
    }
}

function Invoke-BoundedProcess {
    param(
        [string]$Operation,
        [string]$FileName,
        [string[]]$Arguments,
        [string]$WorkingDirectory = $RepositoryRoot,
        [int]$TimeoutMilliseconds =
            $script:ProductionProcessTimeoutMilliseconds
    )
    if ($TimeoutMilliseconds -lt $script:MinimumProcessTimeoutMilliseconds) {
        throw ("process timeout must be at least {0} ms: {1}" -f
            $script:MinimumProcessTimeoutMilliseconds, $TimeoutMilliseconds)
    }

    $stopwatch = [System.Diagnostics.Stopwatch]::StartNew()
    $process = [System.Diagnostics.Process]::new()
    $started = $false
    $stdoutTask = $null
    $stderrTask = $null
    $diagnostics = [System.Collections.Generic.List[string]]::new()
    $timedOut = $false
    $terminationAttempted = $false
    $terminationSucceeded = $true
    $terminationDetails = ''
    $exitCode = $null
    $processId = $null
    try {
        $process.StartInfo = New-RedirectedProcessStartInfo `
            $FileName $Arguments $WorkingDirectory
        try {
            $started = $process.Start()
        } catch {
            $diagnostics.Add("start failed: $($_.Exception.Message)")
        }
        if (-not $started) {
            if ($diagnostics.Count -eq 0) {
                $diagnostics.Add('start returned false')
            }
            $stopwatch.Stop()
            return [pscustomobject]@{
                Operation = $Operation
                FileName = $FileName
                Started = $false
                ProcessId = $null
                ExitCode = $null
                TimedOut = $false
                TimeoutMilliseconds = $TimeoutMilliseconds
                DurationMilliseconds = $stopwatch.ElapsedMilliseconds
                Stdout = ''
                Stderr = ''
                OutputDrainSucceeded = $false
                StartDiagnostic = [string]::Join('; ', $diagnostics.ToArray())
                TerminationAttempted = $false
                TerminationSucceeded = $false
                TerminationDiagnostic = ''
            }
        }

        $processId = $process.Id
        $stdoutTask = $process.StandardOutput.ReadToEndAsync()
        $stderrTask = $process.StandardError.ReadToEndAsync()
        if (-not $process.WaitForExit($TimeoutMilliseconds)) {
            $timedOut = $true
            $terminationAttempted = $true
            $termination = Stop-OwnedProcessTree $process $Operation
            $terminationSucceeded = $termination.Succeeded
            $terminationDetails = $termination.Diagnostic
        }
        if ($process.HasExited) { $exitCode = $process.ExitCode }
        $stdout = Complete-RedirectedTextTask `
            $stdoutTask "$Operation stdout" $diagnostics
        $stderr = Complete-RedirectedTextTask `
            $stderrTask "$Operation stderr" $diagnostics
        $drainSucceeded = -not (@($diagnostics | Where-Object {
            $_ -match 'drain (?:exceeded|failed)'
        }).Count -gt 0)
        $stopwatch.Stop()
        return [pscustomobject]@{
            Operation = $Operation
            FileName = $FileName
            Started = $true
            ProcessId = $processId
            ExitCode = $exitCode
            TimedOut = $timedOut
            TimeoutMilliseconds = $TimeoutMilliseconds
            DurationMilliseconds = $stopwatch.ElapsedMilliseconds
            Stdout = $stdout
            Stderr = $stderr
            OutputDrainSucceeded = $drainSucceeded
            StartDiagnostic = [string]::Join('; ', $diagnostics.ToArray())
            TerminationAttempted = $terminationAttempted
            TerminationSucceeded = $terminationSucceeded
            TerminationDiagnostic = $terminationDetails
        }
    } finally {
        $process.Dispose()
    }
}

function Get-ProcessOutputExcerpt {
    param([string]$Value)
    if ([string]::IsNullOrWhiteSpace($Value)) { return '' }
    $limit = 4096
    if ($Value.Length -le $limit) { return $Value }
    return $Value.Substring(0, $limit) +
        "`n...[truncated $($Value.Length - $limit) characters]"
}

function Get-BoundedProcessFailureDiagnostic {
    param([object]$Result)
    if (-not $Result.Started) {
        return ("{0} start failed: {1}" -f
            $Result.Operation, $Result.StartDiagnostic)
    }
    if ($Result.TimedOut) {
        $terminationState = if ($Result.TerminationSucceeded) {
            'owned process tree terminated'
        } else {
            'owned process-tree termination failed'
        }
        $terminationDetail = if ([string]::IsNullOrWhiteSpace(
                $Result.TerminationDiagnostic)) {
            'no additional termination detail'
        } else {
            $Result.TerminationDiagnostic
        }
        $template =
            "{0} timed out after {1} ms (duration {2} ms); {3}; {4}; " +
            "stdout: {5}; stderr: {6}"
        return ($template -f
            $Result.Operation,
            $Result.TimeoutMilliseconds,
            $Result.DurationMilliseconds,
            $terminationState,
            $terminationDetail,
            (Get-ProcessOutputExcerpt $Result.Stdout),
            (Get-ProcessOutputExcerpt $Result.Stderr))
    }
    if (-not $Result.OutputDrainSucceeded) {
        return ("{0} output drain failed: {1}" -f
            $Result.Operation, $Result.StartDiagnostic)
    }
    if ($null -eq $Result.ExitCode -or $Result.ExitCode -ne 0) {
        return ("{0} exited with code {1}; stdout: {2}; stderr: {3}" -f
            $Result.Operation,
            $Result.ExitCode,
            (Get-ProcessOutputExcerpt $Result.Stdout),
            (Get-ProcessOutputExcerpt $Result.Stderr))
    }
    if (-not [string]::IsNullOrWhiteSpace($Result.Stderr)) {
        return ("{0} wrote to stderr: {1}" -f
            $Result.Operation, (Get-ProcessOutputExcerpt $Result.Stderr))
    }
    return $null
}

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

function Test-ObjectHasExactProperty {
    param(
        [object]$InputObject,
        [string]$Name
    )
    if ($null -eq $InputObject) { return $false }
    foreach ($property in $InputObject.PSObject.Properties) {
        if ($property.Name -ceq $Name) { return $true }
    }
    return $false
}

function ConvertFrom-CTestShowOnlyResult {
    param(
        [string]$Stdout,
        [string]$Stderr,
        [int]$ExitCode
    )
    if ($ExitCode -ne 0) {
        $detailLines = @(@($Stderr, $Stdout) | Where-Object {
            -not [string]::IsNullOrWhiteSpace($_)
        })
        $details = [string]::Join("`n", $detailLines)
        throw ("ctest show-only failed with exit code {0}: {1}" -f
            $ExitCode, $details)
    }
    if (-not [string]::IsNullOrWhiteSpace($Stderr)) {
        throw "ctest show-only wrote to stderr: $Stderr"
    }
    if ([string]::IsNullOrWhiteSpace($Stdout)) {
        throw 'ctest show-only returned empty output'
    }
    try {
        $registry = $Stdout | ConvertFrom-Json
    } catch {
        throw "ctest show-only returned invalid JSON: $($_.Exception.Message)"
    }
    if ($null -eq $registry -or
        -not ($registry -is [System.Management.Automation.PSCustomObject])) {
        throw 'ctest show-only JSON root must be an object'
    }
    if (-not (Test-ObjectHasExactProperty $registry 'kind') -or
        -not ($registry.kind -is [string]) -or
        $registry.kind -cne 'ctestInfo') {
        throw 'ctest show-only JSON kind must be exactly ctestInfo'
    }
    if (-not (Test-ObjectHasExactProperty $registry 'version') -or
        $null -eq $registry.version -or
        -not ($registry.version -is
            [System.Management.Automation.PSCustomObject])) {
        throw 'ctest show-only JSON version must be an object'
    }
    if (-not (Test-ObjectHasExactProperty $registry.version 'major') -or
        -not ($registry.version.major -is [int]) -or
        $registry.version.major -ne 1) {
        throw 'ctest show-only JSON version.major must be supported integer 1'
    }
    if (-not (Test-ObjectHasExactProperty $registry.version 'minor') -or
        -not ($registry.version.minor -is [int]) -or
        $registry.version.minor -ne 0) {
        throw 'ctest show-only JSON version.minor must be supported integer 0'
    }
    if (-not (Test-ObjectHasExactProperty $registry 'tests') -or
        -not ($registry.tests -is [System.Array])) {
        throw 'ctest show-only JSON tests must be a JSON array'
    }
    if ($registry.tests.Count -eq 0) {
        throw 'ctest show-only returned an empty test registry'
    }
    $names = [System.Collections.Generic.List[string]]::new()
    for ($index = 0; $index -lt $registry.tests.Count; $index++) {
        $test = $registry.tests[$index]
        if ($null -eq $test -or
            -not ($test -is [System.Management.Automation.PSCustomObject])) {
            throw "ctest show-only test item at index $index must be an object"
        }
        if (-not (Test-ObjectHasExactProperty $test 'name') -or
            -not ($test.name -is [string]) -or
            [string]::IsNullOrWhiteSpace($test.name)) {
            throw "ctest show-only test item at index $index must have a non-empty name string"
        }
        $names.Add($test.name)
    }
    return $names.ToArray()
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
    $result = Invoke-BoundedProcess `
        'ctest show-only' `
        $CTestCommand `
        $arguments `
        $RepositoryRoot
    $failure = Get-BoundedProcessFailureDiagnostic $result
    if ($null -ne $failure) { throw $failure }
    return @(ConvertFrom-CTestShowOnlyResult `
        $result.Stdout $result.Stderr $result.ExitCode)
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

function Get-FirstOrdinalDuplicate {
    param([string[]]$Values)
    $seen = [System.Collections.Generic.HashSet[string]]::new(
        [System.StringComparer]::Ordinal)
    foreach ($value in @($Values)) {
        if (-not $seen.Add($value)) { return $value }
    }
    return $null
}

function Test-TestRegistryContract {
    param(
        [string[]]$RegisteredNames,
        [string]$ArchitectureContent,
        [System.Collections.Generic.List[string]]$Diagnostics = $null
    )
    $documentedNames = @(Get-ArchitectureTestNames $ArchitectureContent)
    $registeredDuplicate = Get-FirstOrdinalDuplicate $RegisteredNames
    if ($null -ne $registeredDuplicate) {
        if ($null -ne $Diagnostics) {
            $Diagnostics.Add(
                "configured CTest registry contains duplicate '$registeredDuplicate'")
        }
        return $false
    }
    $documentedDuplicate = Get-FirstOrdinalDuplicate $documentedNames
    if ($null -ne $documentedDuplicate) {
        if ($null -ne $Diagnostics) {
            $Diagnostics.Add(
                "architecture test declarations contain duplicate '$documentedDuplicate'")
        }
        return $false
    }
    $registered = @(Get-OrdinalSortedStrings $RegisteredNames)
    $documented = @(Get-OrdinalSortedStrings $documentedNames)
    if ($registered.Count -eq 0) {
        if ($null -ne $Diagnostics) {
            $Diagnostics.Add('configured CTest registry must not be empty')
        }
        return $false
    }
    if ($registered.Count -ne $documented.Count -or
        ($registered -join "`n") -cne ($documented -join "`n")) {
        if ($null -ne $Diagnostics) {
            $Diagnostics.Add(
                'architecture declarations do not match the configured CTest registry')
        }
        return $false
    }
    return $true
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

function Assert-CTestShowOnlyRejected {
    param(
        [string]$Stdout,
        [string]$Stderr,
        [int]$ExitCode,
        [string]$ExpectedPattern,
        [string]$MutationName
    )
    try {
        $null = @(ConvertFrom-CTestShowOnlyResult `
            $Stdout $Stderr $ExitCode)
        $failures.Add("$MutationName was accepted")
    } catch {
        if ($_.Exception.Message -notmatch $ExpectedPattern) {
            $failures.Add(
                "$MutationName returned unexpected diagnostic: $($_.Exception.Message)")
        }
    }
}

function Test-CTestShowOnlySchemaGuards {
    $validSingle =
        '{"kind":"ctestInfo","version":{"major":1,"minor":0},' +
        '"tests":[{"name":"one.unit"}]}'
    $validDuplicate =
        '{"kind":"ctestInfo","version":{"major":1,"minor":0},' +
        '"tests":[{"name":"same.unit"},{"name":"same.unit"}]}'
    try {
        $singleNames = @(ConvertFrom-CTestShowOnlyResult $validSingle '' 0)
        [void](Test-RegistryMultiplicity $singleNames `
            'one.unit' 1 'valid single-item JSON array')
    } catch {
        $failures.Add(
            "valid single-item JSON array was rejected: $($_.Exception.Message)")
    }
    try {
        $duplicateNames = @(ConvertFrom-CTestShowOnlyResult `
            $validDuplicate '' 0)
        [void](Test-RegistryMultiplicity $duplicateNames `
            'same.unit' 2 'valid duplicate-preserving JSON array')
    } catch {
        $failures.Add(
            "valid duplicate-preserving JSON array was rejected: $($_.Exception.Message)")
    }

    $checks = 0
    $checks++
    Assert-CTestShowOnlyRejected 'command failed' '' 7 `
        'exit code 7' 'nonzero CTest exit'
    $checks++
    Assert-CTestShowOnlyRejected $validSingle 'synthetic stderr' 0 `
        'wrote to stderr' 'CTest stderr'

    $mutations = @(
        [pscustomobject]@{
            Name = 'empty output'; Value = ''; Pattern = 'empty output'
        },
        [pscustomobject]@{
            Name = 'whitespace output'; Value = '   '; Pattern = 'empty output'
        },
        [pscustomobject]@{
            Name = 'malformed JSON'; Value = '{'; Pattern = 'invalid JSON'
        },
        [pscustomobject]@{
            Name = 'null JSON root'; Value = 'null'; Pattern = 'root must be an object'
        },
        [pscustomobject]@{
            Name = 'array JSON root'; Value = '[]'; Pattern = 'root must be an object'
        },
        [pscustomobject]@{
            Name = 'missing kind'
            Value = '{"version":{"major":1,"minor":0},"tests":[{"name":"one"}]}'
            Pattern = 'kind'
        },
        [pscustomobject]@{
            Name = 'wrong kind'
            Value = '{"kind":"other","version":{"major":1,"minor":0},"tests":[{"name":"one"}]}'
            Pattern = 'kind'
        },
        [pscustomobject]@{
            Name = 'missing version'
            Value = '{"kind":"ctestInfo","tests":[{"name":"one"}]}'
            Pattern = 'version must be an object'
        },
        [pscustomobject]@{
            Name = 'string version'
            Value = '{"kind":"ctestInfo","version":"1.0","tests":[{"name":"one"}]}'
            Pattern = 'version must be an object'
        },
        [pscustomobject]@{
            Name = 'array version'
            Value = '{"kind":"ctestInfo","version":[1,0],"tests":[{"name":"one"}]}'
            Pattern = 'version must be an object'
        },
        [pscustomobject]@{
            Name = 'missing version major'
            Value = '{"kind":"ctestInfo","version":{"minor":0},"tests":[{"name":"one"}]}'
            Pattern = 'version.major'
        },
        [pscustomobject]@{
            Name = 'string version major'
            Value = '{"kind":"ctestInfo","version":{"major":"1","minor":0},"tests":[{"name":"one"}]}'
            Pattern = 'version.major'
        },
        [pscustomobject]@{
            Name = 'unsupported version major'
            Value = '{"kind":"ctestInfo","version":{"major":2,"minor":0},"tests":[{"name":"one"}]}'
            Pattern = 'version.major'
        },
        [pscustomobject]@{
            Name = 'missing version minor'
            Value = '{"kind":"ctestInfo","version":{"major":1},"tests":[{"name":"one"}]}'
            Pattern = 'version.minor'
        },
        [pscustomobject]@{
            Name = 'string version minor'
            Value = '{"kind":"ctestInfo","version":{"major":1,"minor":"0"},"tests":[{"name":"one"}]}'
            Pattern = 'version.minor'
        },
        [pscustomobject]@{
            Name = 'unsupported version minor'
            Value = '{"kind":"ctestInfo","version":{"major":1,"minor":1},"tests":[{"name":"one"}]}'
            Pattern = 'version.minor'
        },
        [pscustomobject]@{
            Name = 'missing tests'
            Value = '{"kind":"ctestInfo","version":{"major":1,"minor":0}}'
            Pattern = 'tests must be a JSON array'
        },
        [pscustomobject]@{
            Name = 'null tests'
            Value = '{"kind":"ctestInfo","version":{"major":1,"minor":0},"tests":null}'
            Pattern = 'tests must be a JSON array'
        },
        [pscustomobject]@{
            Name = 'object tests'
            Value = '{"kind":"ctestInfo","version":{"major":1,"minor":0},"tests":{"name":"one"}}'
            Pattern = 'tests must be a JSON array'
        },
        [pscustomobject]@{
            Name = 'string tests'
            Value = '{"kind":"ctestInfo","version":{"major":1,"minor":0},"tests":"one"}'
            Pattern = 'tests must be a JSON array'
        },
        [pscustomobject]@{
            Name = 'numeric tests'
            Value = '{"kind":"ctestInfo","version":{"major":1,"minor":0},"tests":1}'
            Pattern = 'tests must be a JSON array'
        },
        [pscustomobject]@{
            Name = 'empty registry'
            Value = '{"kind":"ctestInfo","version":{"major":1,"minor":0},"tests":[]}'
            Pattern = 'empty test registry'
        },
        [pscustomobject]@{
            Name = 'null test item'
            Value = '{"kind":"ctestInfo","version":{"major":1,"minor":0},"tests":[null]}'
            Pattern = 'item at index 0 must be an object'
        },
        [pscustomobject]@{
            Name = 'string test item'
            Value = '{"kind":"ctestInfo","version":{"major":1,"minor":0},"tests":["one"]}'
            Pattern = 'item at index 0 must be an object'
        },
        [pscustomobject]@{
            Name = 'missing test name'
            Value = '{"kind":"ctestInfo","version":{"major":1,"minor":0},"tests":[{}]}'
            Pattern = 'non-empty name string'
        },
        [pscustomobject]@{
            Name = 'empty test name'
            Value = '{"kind":"ctestInfo","version":{"major":1,"minor":0},"tests":[{"name":""}]}'
            Pattern = 'non-empty name string'
        },
        [pscustomobject]@{
            Name = 'whitespace test name'
            Value = '{"kind":"ctestInfo","version":{"major":1,"minor":0},"tests":[{"name":" "}]}'
            Pattern = 'non-empty name string'
        },
        [pscustomobject]@{
            Name = 'numeric test name'
            Value = '{"kind":"ctestInfo","version":{"major":1,"minor":0},"tests":[{"name":1}]}'
            Pattern = 'non-empty name string'
        }
    )
    foreach ($mutation in $mutations) {
        $checks++
        Assert-CTestShowOnlyRejected `
            $mutation.Value '' 0 $mutation.Pattern $mutation.Name
    }
    return $checks
}

function Get-NormalizedFullPath {
    param([string]$Path)
    return [System.IO.Path]::GetFullPath($Path).
        TrimEnd([char[]]@('\', '/'))
}

function Test-IsStrictChildPath {
    param(
        [string]$ParentPath,
        [string]$ChildPath
    )
    $parent = Get-NormalizedFullPath $ParentPath
    $child = Get-NormalizedFullPath $ChildPath
    if ($child -ieq $parent) { return $false }
    $parentPrefix = $parent + [System.IO.Path]::DirectorySeparatorChar
    return $child.StartsWith(
        $parentPrefix,
        [System.StringComparison]::OrdinalIgnoreCase)
}

function Get-TestProbeRoot {
    $repository = Get-NormalizedFullPath $RepositoryRoot
    $build = Get-NormalizedFullPath $BuildDirectory
    if (-not (Test-IsStrictChildPath $repository $build)) {
        throw "build directory is not a strict child of repository root: $build"
    }
    $testProbeRoot = Get-NormalizedFullPath (
        Join-Path $build 'governance-registry-probes')
    if (-not (Test-IsStrictChildPath $build $testProbeRoot)) {
        throw "test probe root is not a strict child of build directory: $testProbeRoot"
    }
    return $testProbeRoot
}

function New-RegistryProbeInvocationRoot {
    $testProbeRoot = Get-TestProbeRoot
    [void][System.IO.Directory]::CreateDirectory($testProbeRoot)
    $invocationName = 'invocation-{0}-{1}' -f
        $PID, [System.Guid]::NewGuid().ToString('N')
    $invocationRoot = Get-NormalizedFullPath (
        Join-Path $testProbeRoot $invocationName)
    if (-not (Test-IsStrictChildPath $testProbeRoot $invocationRoot)) {
        throw "invocation probe root escaped test probe root: $invocationRoot"
    }
    if (Test-Path -LiteralPath $invocationRoot) {
        throw "invocation probe root already exists: $invocationRoot"
    }
    [void][System.IO.Directory]::CreateDirectory($invocationRoot)
    return $invocationRoot
}

function Remove-RegistryProbeInvocationRoot {
    param([string]$InvocationRoot)
    $testProbeRoot = Get-TestProbeRoot
    $resolvedInvocationRoot = Get-NormalizedFullPath $InvocationRoot
    if (-not (Test-IsStrictChildPath $testProbeRoot $resolvedInvocationRoot)) {
        throw "refusing to remove path outside test probe root: $resolvedInvocationRoot"
    }
    if (Test-Path -LiteralPath $resolvedInvocationRoot) {
        Remove-Item -Recurse -Force -LiteralPath $resolvedInvocationRoot
    }
    if (Test-Path -LiteralPath $resolvedInvocationRoot) {
        throw "invocation probe root remains after cleanup: $resolvedInvocationRoot"
    }
}

function Test-ProcessIdRunning {
    param([int]$ProcessId)
    return $null -ne (Get-Process -Id $ProcessId -ErrorAction SilentlyContinue)
}

function Test-BoundedProcessGuards {
    if ([string]::IsNullOrWhiteSpace($script:RegistryProbeInvocationRoot)) {
        throw 'bounded process guard invocation root was not initialized'
    }
    $guardRoot = Get-NormalizedFullPath (
        Join-Path $script:RegistryProbeInvocationRoot 'bounded-process-guards')
    if (-not (Test-IsStrictChildPath `
            $script:RegistryProbeInvocationRoot $guardRoot)) {
        throw "bounded process guard root escaped invocation root: $guardRoot"
    }
    if (Test-Path -LiteralPath $guardRoot) {
        throw "bounded process guard root already exists: $guardRoot"
    }

    $checks = 0
    $hangParentId = $null
    $hangChildId = $null
    $utf8WithoutBom = [System.Text.UTF8Encoding]::new($false)
    try {
        [void][System.IO.Directory]::CreateDirectory($guardRoot)
        $powerShellPath = Join-Path $PSHOME 'powershell.exe'

        $argumentDirectory = Join-Path $guardRoot (
            "argument path $([char]0x8def)$([char]0x5f84)")
        [void][System.IO.Directory]::CreateDirectory($argumentDirectory)
        $argumentScript = Join-Path $argumentDirectory 'echo args.ps1'
        [System.IO.File]::WriteAllText(
            $argumentScript,
            '[Console]::OutputEncoding=[Text.UTF8Encoding]::new($false); ' +
                '[Environment]::GetCommandLineArgs() | ConvertTo-Json -Compress',
            $utf8WithoutBom)
        $unicodeArgument = "Unicode-$([char]0x8def)$([char]0x5f84)"
        $argumentSamples = @(
            'plain',
            'space value',
            $unicodeArgument,
            'quote"inside',
            'trail\',
            'mix\"quote',
            ''
        )
        $argumentResult = Invoke-BoundedProcess `
            'bounded argument round-trip' `
            $powerShellPath `
            (@(
                '-NoProfile',
                '-ExecutionPolicy', 'Bypass',
                '-File', $argumentScript
            ) + $argumentSamples) `
            $guardRoot
        $checks++
        $argumentFailure = Get-BoundedProcessFailureDiagnostic $argumentResult
        if ($null -ne $argumentFailure) {
            $failures.Add("bounded argument round-trip failed: $argumentFailure")
        } else {
            [object[]]$actualArguments =
                $argumentResult.Stdout | ConvertFrom-Json
            if ($actualArguments.Count -lt $argumentSamples.Count) {
                $failures.Add('bounded argument round-trip omitted arguments')
            } else {
                $firstTailIndex =
                    $actualArguments.Count - $argumentSamples.Count
                $actualTail = @($actualArguments[
                    $firstTailIndex..($actualArguments.Count - 1)])
                if (($actualTail -join [char]0) -cne
                    ($argumentSamples -join [char]0)) {
                    $failures.Add(
                        'bounded argument round-trip changed Windows argv values; ' +
                        'expected=' +
                        ($argumentSamples | ConvertTo-Json -Compress) +
                        '; actual=' +
                        ($actualTail | ConvertTo-Json -Compress) +
                        '; stdout=' + $argumentResult.Stdout)
                }
            }
        }

        $largeOutputScript = Join-Path $guardRoot 'large-output.ps1'
        [System.IO.File]::WriteAllText(
            $largeOutputScript,
            'param([int]$Length);' +
                '[Console]::Out.Write((''O'' * $Length));' +
                '[Console]::Error.Write((''E'' * $Length))',
            $utf8WithoutBom)
        $largeOutputResult = Invoke-BoundedProcess `
            'bounded large output' `
            $powerShellPath `
            @(
                '-NoProfile',
                '-ExecutionPolicy', 'Bypass',
                '-File', $largeOutputScript,
                '-Length', [string]$script:LargeOutputLength
            ) `
            $guardRoot
        $checks++
        $largeOutputDiagnostic =
            Get-BoundedProcessFailureDiagnostic $largeOutputResult
        if (-not $largeOutputResult.Started -or
            $largeOutputResult.TimedOut -or
            -not $largeOutputResult.OutputDrainSucceeded -or
            $largeOutputResult.ExitCode -ne 0 -or
            $largeOutputResult.Stdout.Length -ne $script:LargeOutputLength -or
            $largeOutputResult.Stderr.Length -ne $script:LargeOutputLength -or
            $largeOutputDiagnostic -notmatch 'wrote to stderr') {
            $failures.Add(
                'bounded large stdout/stderr guard did not drain or reject stderr')
        }

        $nonzeroScript = Join-Path $guardRoot 'nonzero.ps1'
        [System.IO.File]::WriteAllText(
            $nonzeroScript,
            '[Console]::Out.Write(''NONZERO_OUT'');' +
                '[Console]::Error.Write(''NONZERO_ERR'');exit 7',
            $utf8WithoutBom)
        $nonzeroResult = Invoke-BoundedProcess `
            'bounded nonzero' `
            $powerShellPath `
            @(
                '-NoProfile',
                '-ExecutionPolicy', 'Bypass',
                '-File', $nonzeroScript
            ) `
            $guardRoot
        $checks++
        $nonzeroDiagnostic = Get-BoundedProcessFailureDiagnostic $nonzeroResult
        if ($nonzeroResult.ExitCode -ne 7 -or
            $nonzeroResult.Stdout -cne 'NONZERO_OUT' -or
            $nonzeroResult.Stderr -cne 'NONZERO_ERR' -or
            $nonzeroDiagnostic -notmatch 'exited with code 7') {
            $failures.Add('bounded nonzero guard did not fail closed')
        }

        $startupResult = Invoke-BoundedProcess `
            'bounded startup failure' `
            (Join-Path $guardRoot 'missing executable.exe') `
            @() `
            $guardRoot
        $checks++
        $startupDiagnostic = Get-BoundedProcessFailureDiagnostic $startupResult
        if ($startupResult.Started -or
            $startupDiagnostic -notmatch 'start failed') {
            $failures.Add('bounded startup failure guard did not fail closed')
        }

        $hangRoot = Join-Path $guardRoot 'hang'
        [void][System.IO.Directory]::CreateDirectory($hangRoot)
        $hangChildScript = Join-Path $hangRoot 'child.ps1'
        $hangParentScript = Join-Path $hangRoot 'parent.ps1'
        $hangChildPidPath = Join-Path $hangRoot 'child.pid'
        $hangSentinelPath = Join-Path $hangRoot 'delayed-sentinel.txt'
        [System.IO.File]::WriteAllText(
            $hangChildScript,
            'param([string]$PidPath,[string]$SentinelPath,' +
                '[int]$DelayMilliseconds);' +
                '[IO.File]::WriteAllText($PidPath,[string]$PID);' +
                'Start-Sleep -Milliseconds $DelayMilliseconds;' +
                '[IO.File]::WriteAllText($SentinelPath,''survived'');' +
                'Start-Sleep -Seconds 60',
            $utf8WithoutBom)
        [System.IO.File]::WriteAllText(
            $hangParentScript,
            'param([string]$ChildScript,[string]$ChildPidPath,' +
                '[string]$SentinelPath,[int]$SentinelDelay,' +
                '[int]$FloodLength);' +
                '$child=Start-Process -FilePath ' +
                '(Join-Path $PSHOME ''powershell.exe'') -ArgumentList @(' +
                '''-NoProfile'',''-ExecutionPolicy'',''Bypass'',''-File'',' +
                '$ChildScript,''-PidPath'',$ChildPidPath,' +
                '''-SentinelPath'',$SentinelPath,' +
                '''-DelayMilliseconds'',[string]$SentinelDelay) -PassThru;' +
                '[Console]::Out.Write((''H'' * $FloodLength));' +
                '[Console]::Error.Write((''E'' * $FloodLength));' +
                'Start-Sleep -Seconds 60',
            $utf8WithoutBom)
        $hangResult = Invoke-BoundedProcess `
            'bounded hang regression' `
            $powerShellPath `
            @(
                '-NoProfile',
                '-ExecutionPolicy', 'Bypass',
                '-File', $hangParentScript,
                '-ChildScript', $hangChildScript,
                '-ChildPidPath', $hangChildPidPath,
                '-SentinelPath', $hangSentinelPath,
                '-SentinelDelay',
                    [string]$script:HangSentinelDelayMilliseconds,
                '-FloodLength', [string]$script:LargeOutputLength
            ) `
            $guardRoot `
            $script:HangRegressionTimeoutMilliseconds
        $hangParentId = $hangResult.ProcessId
        if (Test-Path -LiteralPath $hangChildPidPath) {
            $hangChildId = [int][System.IO.File]::ReadAllText(
                $hangChildPidPath)
        }
        $checks++
        $hangDiagnostic = Get-BoundedProcessFailureDiagnostic $hangResult
        $expectedTerminationState = if ($hangResult.TerminationSucceeded) {
            'owned process tree terminated'
        } else {
            'owned process-tree termination failed'
        }
        $expectedTerminationDetail = if ([string]::IsNullOrWhiteSpace(
                $hangResult.TerminationDiagnostic)) {
            'no additional termination detail'
        } else {
            $hangResult.TerminationDiagnostic
        }
        $expectedHangPrefix =
            'bounded hang regression timed out after ' +
            "$($script:HangRegressionTimeoutMilliseconds) ms " +
            "(duration $($hangResult.DurationMilliseconds) ms); " +
            "$expectedTerminationState; $expectedTerminationDetail; "
        $expectedHangStreams =
            'stdout: ' + (Get-ProcessOutputExcerpt $hangResult.Stdout) +
            '; stderr: ' + (Get-ProcessOutputExcerpt $hangResult.Stderr)
        if (-not $hangResult.Started -or
            -not $hangResult.TimedOut -or
            -not $hangResult.TerminationAttempted -or
            -not $hangResult.TerminationSucceeded -or
            -not $hangResult.OutputDrainSucceeded -or
            $hangResult.Stdout.Length -lt $script:LargeOutputLength -or
            $hangResult.Stderr.Length -lt $script:LargeOutputLength -or
            -not $hangDiagnostic.StartsWith(
                $expectedHangPrefix,
                [System.StringComparison]::Ordinal) -or
            -not $hangDiagnostic.EndsWith(
                $expectedHangStreams,
                [System.StringComparison]::Ordinal) -or
            $hangDiagnostic -match '\{[0-6]\}') {
            $failures.Add(
                "bounded hang regression failed: $hangDiagnostic")
        }
        if ($null -eq $hangChildId) {
            $failures.Add('bounded hang regression omitted child PID marker')
        } elseif (Test-ProcessIdRunning $hangChildId) {
            $failures.Add('bounded hang regression left child process running')
        }
        if ($null -ne $hangParentId -and
            (Test-ProcessIdRunning $hangParentId)) {
            $failures.Add('bounded hang regression left parent process running')
        }
        if (Test-Path -LiteralPath $hangChildPidPath) {
            $sentinelDeadline =
                [System.IO.File]::GetLastWriteTimeUtc($hangChildPidPath).
                AddMilliseconds(
                    $script:HangSentinelDelayMilliseconds + 250)
            while ([DateTime]::UtcNow -lt $sentinelDeadline -and
                -not (Test-Path -LiteralPath $hangSentinelPath)) {
                Start-Sleep -Milliseconds 25
            }
        }
        if (Test-Path -LiteralPath $hangSentinelPath) {
            $failures.Add('bounded hang regression allowed delayed sentinel')
        }
    } finally {
        foreach ($ownedProcessId in @($hangParentId, $hangChildId)) {
            if ($null -eq $ownedProcessId) { continue }
            $ownedProcess = Get-Process `
                -Id $ownedProcessId -ErrorAction SilentlyContinue
            if ($null -ne $ownedProcess) {
                $cleanupTermination = Stop-OwnedProcessTree `
                    $ownedProcess 'bounded hang guard cleanup'
                if (-not $cleanupTermination.Succeeded) {
                    $failures.Add(
                        "bounded hang guard cleanup failed for PID " +
                        "$ownedProcessId`: $($cleanupTermination.Diagnostic)")
                }
                $ownedProcess.Dispose()
            }
        }
        if (Test-Path -LiteralPath $guardRoot) {
            Remove-Item -Recurse -Force -LiteralPath $guardRoot
        }
        if (Test-Path -LiteralPath $guardRoot) {
            throw "bounded process guard root remains after cleanup: $guardRoot"
        }
    }
    return $checks
}

function Invoke-RegistryProbe {
    param(
        [string]$VariantName,
        [string]$CaseName,
        [string]$CMakeContent,
        [hashtable]$AdditionalFiles = @{}
    )
    if ([string]::IsNullOrWhiteSpace($script:RegistryProbeInvocationRoot)) {
        throw 'registry probe invocation root was not initialized'
    }
    $probeRoot = Get-NormalizedFullPath (Join-Path (
        Join-Path $script:RegistryProbeInvocationRoot $VariantName) $CaseName)
    if (-not (Test-IsStrictChildPath `
            $script:RegistryProbeInvocationRoot $probeRoot)) {
        throw "registry probe escaped invocation root: $probeRoot"
    }
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
    $configureResult = Invoke-BoundedProcess `
        "$VariantName $CaseName probe configure" `
        $CMakeCommand `
        $configureArguments `
        $RepositoryRoot
    $configureFailure = Get-BoundedProcessFailureDiagnostic $configureResult
    if ($null -ne $configureFailure) {
        $failures.Add(
            "$VariantName $CaseName probe configure failed: $configureFailure")
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
        [void](Test-RegistryMultiplicity $RegisteredNames `
            'app_state.unit' 1 "$VariantName registry-only baseline")
        $duplicateDocumentedNames = @(
            Get-ArchitectureTestNames $duplicateBulletMutation)
        [void](Test-RegistryMultiplicity $duplicateDocumentedNames `
            'app_state.unit' 2 "$VariantName architecture-only duplicate")
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
        $duplicateDocumentedNames = @(
            Get-ArchitectureTestNames $duplicateArchitecture)
        [void](Test-RegistryMultiplicity $duplicateDocumentedNames `
            'duplicate.cross' 1 "$VariantName registry-only duplicate documentation")
        if (Test-TestRegistryContract `
            $duplicateNames $duplicateArchitecture) {
            $failures.Add("$VariantName accepted duplicate CMake tests")
        }

        $jointDuplicateArchitecture = $duplicateArchitecture +
            '- `duplicate.cross`: joint duplicate probe.' + $LineEnding
        $checks++
        if (Test-MutationChanged `
            $duplicateArchitecture `
            $jointDuplicateArchitecture `
            "$VariantName joint duplicate architecture") {
            $jointDocumentedNames = @(
                Get-ArchitectureTestNames $jointDuplicateArchitecture)
            [void](Test-RegistryMultiplicity $duplicateNames `
                'duplicate.cross' 2 "$VariantName joint duplicate registry")
            [void](Test-RegistryMultiplicity $jointDocumentedNames `
                'duplicate.cross' 2 "$VariantName joint duplicate architecture")
            if (Test-TestRegistryContract `
                $duplicateNames $jointDuplicateArchitecture) {
                $failures.Add(
                    "$VariantName accepted matching duplicates on both sides")
            }
        }
    }
    return $checks
}

function Test-NoLegacyMetadataLog {
    param([string]$Content)
    return $Content -notmatch 'meta_debug\.log'
}

function Invoke-RegistryConcurrencyWorkerCheck {
    $lineEnding = "`r`n"
    $sectionHeader =
        ConvertFrom-Utf8Base64 'IyMg5b2T5YmN6Ieq5Yqo5YyW6L6555WM'
    $architecture = $sectionHeader + $lineEnding +
        '- `probe.unit`: concurrency probe.' + $lineEnding
    $cmakeContent = @(
        'cmake_minimum_required(VERSION 3.20)',
        'project(GovernanceConcurrencyProbe NONE)',
        'include(CTest)',
        'add_test(NAME probe.unit COMMAND "${CMAKE_COMMAND}" -E echo probe)'
    ) -join $lineEnding
    $cmakeContent += $lineEnding
    $names = @(Invoke-RegistryProbe `
        'Concurrency' 'shared-case' $cmakeContent)
    [void](Test-RegistryMultiplicity $names `
        'probe.unit' 1 'concurrency worker registry')
    if (-not (Test-TestRegistryContract $names $architecture)) {
        $failures.Add('concurrency worker production predicate rejected its registry')
    }
}

function Test-RegistryProbeConcurrency {
    param([int]$ProcessCount = 4)
    $powerShellPath = Join-Path $PSHOME 'powershell.exe'
    if (-not (Test-Path -LiteralPath $powerShellPath -PathType Leaf)) {
        $failures.Add("Windows PowerShell executable not found: $powerShellPath")
        return 0
    }
    $workerArguments = @(
        '-NoProfile',
        '-ExecutionPolicy', 'Bypass',
        '-File', $PSCommandPath,
        '-RepositoryRoot', $RepositoryRoot,
        '-CMakeCommand', $CMakeCommand,
        '-CTestCommand', $CTestCommand,
        '-BuildDirectory', $BuildDirectory
    )
    if (-not [string]::IsNullOrWhiteSpace($Configuration)) {
        $workerArguments += @('-Configuration', $Configuration)
    }
    $workerArguments += '-RegistryConcurrencyWorker'
    $argumentString = ConvertTo-WindowsCommandLine $workerArguments
    $workers = [System.Collections.Generic.List[object]]::new()
    $workerRoots = [System.Collections.Generic.HashSet[string]]::new(
        [System.StringComparer]::OrdinalIgnoreCase)
    try {
        for ($index = 0; $index -lt $ProcessCount; $index++) {
            $startInfo = [System.Diagnostics.ProcessStartInfo]::new()
            $startInfo.FileName = $powerShellPath
            $startInfo.Arguments = $argumentString
            $startInfo.WorkingDirectory = $RepositoryRoot
            $startInfo.UseShellExecute = $false
            $startInfo.CreateNoWindow = $true
            $startInfo.RedirectStandardOutput = $true
            $startInfo.RedirectStandardError = $true
            $process = [System.Diagnostics.Process]::new()
            $process.StartInfo = $startInfo
            if (-not $process.Start()) {
                throw "concurrency worker $index did not start"
            }
            $workers.Add([pscustomobject]@{
                Index = $index
                Process = $process
                Stdout = $process.StandardOutput.ReadToEndAsync()
                Stderr = $process.StandardError.ReadToEndAsync()
            })
        }
        foreach ($worker in $workers) {
            if (-not $worker.Process.WaitForExit(
                    $script:ConcurrencyWorkerTimeoutMilliseconds)) {
                $termination = Stop-OwnedProcessTree `
                    $worker.Process `
                    "concurrency worker $($worker.Index)"
                $failures.Add(
                    "concurrency worker $($worker.Index) timed out after " +
                    "$($script:ConcurrencyWorkerTimeoutMilliseconds) ms; " +
                    "$($termination.Diagnostic)")
                continue
            }
            $workerDiagnostics =
                [System.Collections.Generic.List[string]]::new()
            $stdout = Complete-RedirectedTextTask `
                $worker.Stdout `
                "concurrency worker $($worker.Index) stdout" `
                $workerDiagnostics
            $stderr = Complete-RedirectedTextTask `
                $worker.Stderr `
                "concurrency worker $($worker.Index) stderr" `
                $workerDiagnostics
            if ($workerDiagnostics.Count -gt 0) {
                $failures.Add(
                    "concurrency worker $($worker.Index) output failed: " +
                    [string]::Join('; ', $workerDiagnostics.ToArray()))
                continue
            }
            if ($worker.Process.ExitCode -ne 0) {
                $failures.Add(
                    "concurrency worker $($worker.Index) exited " +
                    "$($worker.Process.ExitCode): $stderr$stdout")
                continue
            }
            if (-not [string]::IsNullOrWhiteSpace($stderr)) {
                $failures.Add(
                    "concurrency worker $($worker.Index) wrote stderr: $stderr")
                continue
            }
            $marker = [regex]::Match(
                $stdout,
                '(?m)^WORKER_OK\|(?<pid>\d+)\|(?<root>[^\r\n]+)\r?$')
            if (-not $marker.Success) {
                $failures.Add(
                    "concurrency worker $($worker.Index) omitted success marker: $stdout")
                continue
            }
            $workerRoot = $marker.Groups['root'].Value
            $testProbeRoot = Get-TestProbeRoot
            if (-not (Test-IsStrictChildPath $testProbeRoot $workerRoot)) {
                $failures.Add(
                    "concurrency worker reported unsafe probe root: $workerRoot")
                continue
            }
            if (-not $workerRoots.Add($workerRoot)) {
                $failures.Add(
                    "concurrency workers reused probe root: $workerRoot")
            }
            if (Test-Path -LiteralPath $workerRoot) {
                $failures.Add(
                    "concurrency worker left probe root behind: $workerRoot")
            }
        }
    } catch {
        $failures.Add("concurrency harness failed: $($_.Exception.Message)")
    } finally {
        foreach ($worker in $workers) {
            if (-not $worker.Process.HasExited) {
                $termination = Stop-OwnedProcessTree `
                    $worker.Process `
                    "concurrency worker $($worker.Index) cleanup"
                if (-not $termination.Succeeded) {
                    $failures.Add(
                        "concurrency worker $($worker.Index) cleanup failed: " +
                        $termination.Diagnostic)
                }
            }
            $worker.Process.Dispose()
        }
    }
    return $workers.Count
}

if ($RegistryConcurrencyWorker) {
    $workerInvocationRoot = $null
    try {
        $workerInvocationRoot = New-RegistryProbeInvocationRoot
        $script:RegistryProbeInvocationRoot = $workerInvocationRoot
        Invoke-RegistryConcurrencyWorkerCheck
    } catch {
        $failures.Add("concurrency worker failed: $($_.Exception.Message)")
    } finally {
        $script:RegistryProbeInvocationRoot = $null
        if (-not [string]::IsNullOrWhiteSpace($workerInvocationRoot)) {
            try {
                Remove-RegistryProbeInvocationRoot $workerInvocationRoot
            } catch {
                $failures.Add(
                    "concurrency worker cleanup failed for '$workerInvocationRoot': " +
                    "$($_.Exception.Message); retained for diagnosis")
            }
        }
    }
    if ($failures.Count -gt 0) {
        foreach ($failure in $failures) { Write-Output "FAIL: $failure" }
        exit 1
    }
    Write-Output "WORKER_OK|$PID|$workerInvocationRoot"
    exit 0
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
$contractDiagnostics = [System.Collections.Generic.List[string]]::new()
if (-not (Test-TestRegistryContract `
    $registeredTests $architectureContent $contractDiagnostics)) {
    if ($contractDiagnostics.Count -eq 0) {
        $failures.Add(
            'architecture test declarations must exactly match the configured CTest registry')
    } else {
        foreach ($diagnostic in $contractDiagnostics) {
            $failures.Add("test registry contract failed: $diagnostic")
        }
    }
}
$schemaMutationChecks = Test-CTestShowOnlySchemaGuards
if ($schemaMutationChecks -ne 30) {
    $failures.Add('CTest show-only schema suite must execute 30 mutations')
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
$registryConcurrencyProcesses = 0
$boundedProcessChecks = 0
$mainInvocationRoot = $null
try {
    $mainInvocationRoot = New-RegistryProbeInvocationRoot
    $script:RegistryProbeInvocationRoot = $mainInvocationRoot
    $boundedProcessChecks = Test-BoundedProcessGuards
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
    $registryConcurrencyProcesses = Test-RegistryProbeConcurrency 4
} catch {
    $failures.Add("registry probe suite failed: $($_.Exception.Message)")
} finally {
    $script:RegistryProbeInvocationRoot = $null
    if (-not [string]::IsNullOrWhiteSpace($mainInvocationRoot)) {
        try {
            Remove-RegistryProbeInvocationRoot $mainInvocationRoot
        } catch {
            $failures.Add(
                "registry probe cleanup failed for '$mainInvocationRoot': " +
                "$($_.Exception.Message); retained for diagnosis")
        }
    }
}
if ($registryMutationChecks -ne 26) {
    $failures.Add('test registry mutation suite must execute 26 checks')
}
if ($registryConcurrencyProcesses -ne 4) {
    $failures.Add('registry concurrency suite must execute 4 processes')
}
if ($boundedProcessChecks -ne 5) {
    $failures.Add('bounded process suite must execute 5 checks')
}
$currentInvocationResidues = if (
    -not [string]::IsNullOrWhiteSpace($mainInvocationRoot) -and
    (Test-Path -LiteralPath $mainInvocationRoot)) {
    1
} else {
    0
}
if ($currentInvocationResidues -ne 0) {
    $failures.Add('current governance invocation root must be cleaned')
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

$script:GovernanceStopwatch.Stop()
Write-Output (
    "governance tests passed (live registry $($registeredTests.Count), " +
    "$($registryVariants.Count) registry variants, " +
    "$registryMutationChecks registry mutations, " +
    "$schemaMutationChecks schema mutations, " +
    "$registryConcurrencyProcesses concurrent processes, " +
    "$boundedProcessChecks bounded process checks, " +
    "production timeout $($script:ProductionProcessTimeoutMilliseconds) ms, " +
    "hang timeout $($script:HangRegressionTimeoutMilliseconds) ms, " +
    "elapsed $($script:GovernanceStopwatch.ElapsedMilliseconds) ms, " +
    "$currentInvocationResidues current invocation residues)")
