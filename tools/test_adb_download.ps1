$ErrorActionPreference = "Stop"

$repoRoot = Split-Path -Parent (Split-Path -Parent $PSCommandPath)
$scriptPath = Join-Path $repoRoot "adb_download.ps1"
$tempRoot = Join-Path ([System.IO.Path]::GetTempPath()) ("adb-download-test-" + [guid]::NewGuid().ToString("N"))
$fakeAdb = Join-Path $tempRoot "fake-adb.cmd"
$failures = 0

New-Item -ItemType Directory -Path $tempRoot | Out-Null

function Write-FakeAdb {
    param([string[]]$DeviceLines)

    $lines = @(
        "@echo off",
        "if `%1==version (echo Android Debug Bridge version 1.0.41& exit /b 0)",
        "if `%1==devices ("
        "echo List of devices attached"
    )
    foreach ($line in $DeviceLines) {
        $lines += "echo $line"
    }
    $lines += @(
        "exit /b 0",
        ")",
        "exit /b 0"
    )
    [System.IO.File]::WriteAllLines($fakeAdb, $lines, [System.Text.Encoding]::ASCII)
}

function Invoke-Diagnose {
    param([string[]]$ExtraArgs = @())

    $oldAdb = $env:ADB_CMD
    try {
        $env:ADB_CMD = $fakeAdb
        $allArgs = @(
            "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", $scriptPath,
            "-S", "res/arcs-mini", "-B", "build", "-Diagnose"
        ) + $ExtraArgs
        $output = @(& powershell @allArgs 2>&1) -join "`n"
        return [pscustomobject]@{ ExitCode = $LASTEXITCODE; Output = $output }
    } finally {
        $env:ADB_CMD = $oldAdb
    }
}

function Assert-Case {
    param([bool]$Condition, [string]$Message, [string]$Output = "")
    if ($Condition) {
        Write-Host "[PASS] $Message" -ForegroundColor Green
        return
    }
    $script:failures++
    Write-Host "[FAIL] $Message" -ForegroundColor Red
    if ($Output) { Write-Host $Output }
}

try {
    Write-FakeAdb @("SERIAL001`tdevice usb:1-1 product:arcs model:mini")
    $noTid = Invoke-Diagnose
    Assert-Case ($noTid.ExitCode -eq 0) "normal device without transport_id is usable" $noTid.Output
    Assert-Case ($noTid.Output -match "normal mode devices:\s+1") "serial-only device is counted" $noTid.Output
    Assert-Case ($noTid.Output -match "selector:\s+-s SERIAL001") "serial-only device uses -s" $noTid.Output

    Write-FakeAdb @(
        "SERIAL001`tdevice usb:1-1 product:arcs model:mini transport_id:7",
        "SERIAL002`tdevice usb:1-2 product:arcs model:mini"
    )
    $selected = Invoke-Diagnose -ExtraArgs @("-Serial", "SERIAL002")
    Assert-Case ($selected.ExitCode -eq 0) "explicit serial selects one device" $selected.Output
    Assert-Case ($selected.Output -match "normal mode devices:\s+1") "explicit serial narrows target count" $selected.Output
    Assert-Case ($selected.Output -match "selector:\s+-s SERIAL002") "selected serial-only target uses -s" $selected.Output
    Assert-Case ($selected.Output -notmatch "selector:\s+-t 7") "unselected transport target is excluded" $selected.Output

    $transport = Invoke-Diagnose -ExtraArgs @("-Serial", "SERIAL001")
    Assert-Case ($transport.ExitCode -eq 0) "transport-id device is usable" $transport.Output
    Assert-Case ($transport.Output -match "selector:\s+-t 7") "transport-id is preferred when present" $transport.Output

    $scriptSource = Get-Content -LiteralPath $scriptPath -Raw
    $shellScriptSource = Get-Content -LiteralPath (Join-Path $repoRoot "adb_download.sh") -Raw
    Write-FakeAdb @("SERIAL003`tdevice product:arcs model:mini transport_id:9")
    $identityless = Invoke-Diagnose
    Assert-Case ($identityless.ExitCode -eq 0) `
        "one identity-less normal device is accepted" $identityless.Output
    Assert-Case ($identityless.Output -match "single-device recovery fallback: enabled") `
        "diagnosis reports deterministic single-device fallback" $identityless.Output

    Write-FakeAdb @(
        "SERIAL003`tdevice product:arcs model:mini transport_id:9",
        "SERIAL004`tdevice product:arcs model:mini transport_id:10"
    )
    $ambiguous = Invoke-Diagnose
    Assert-Case ($ambiguous.ExitCode -ne 0) `
        "multiple identity-less devices remain rejected" $ambiguous.Output
    Assert-Case ($ambiguous.Output -match "cannot be matched safely") `
        "ambiguous recovery matching reports a clear safety error" $ambiguous.Output

    Assert-Case ($scriptSource -match "newBootCandidates") `
        "wait loop contains the single-device BOOT fallback" $scriptSource
    Assert-Case ($scriptSource -match "NormalTargets\.Count\s*-eq\s*1") `
        "single-device fallback is explicitly count-gated" $scriptSource
    Assert-Case ($scriptSource -match "function Get-RawWritePath") `
        "raw flash writes include an explicit expected size" $scriptSource
    Assert-Case ($scriptSource -match 'push \$localPath \$writePath') `
        "adb push uses the size-qualified raw path" $scriptSource
    Assert-Case ($scriptSource -match "function Verify-PushedFiles") `
        "every flashed partition has a readback verification stage" $scriptSource
    Assert-Case ($scriptSource -match "info rawflash") `
        "push failures collect boot raw-flash diagnostics" $scriptSource
    Assert-Case ($scriptSource -match "/NULL/adb-sync-probe") `
        "PowerShell downloader probes recovery ADB Sync before raw writes" $scriptSource
    Assert-Case ($scriptSource -match "boot\.bin.*0x000000") `
        "old recovery reports the one-time boot upgrade recovery step" $scriptSource
    Assert-Case ($scriptSource -match "device remains in recovery") `
        "verification failure never automatically reboots the target" $scriptSource
    Assert-Case ($shellScriptSource -match "raw_write_path") `
        "Linux raw flash writes include an explicit expected size" $shellScriptSource
    Assert-Case ($shellScriptSource -match "md5sum") `
        "Linux downloads verify the written partition digest" $shellScriptSource
    Assert-Case ($shellScriptSource -match "/NULL/adb-sync-probe") `
        "Linux downloader probes recovery ADB Sync before raw writes" $shellScriptSource
} finally {
    Remove-Item -LiteralPath $tempRoot -Recurse -Force -ErrorAction SilentlyContinue
}

if ($failures -ne 0) {
    Write-Host "$failures adb_download test(s) failed" -ForegroundColor Red
    exit 1
}

Write-Host "All adb_download tests passed" -ForegroundColor Green
