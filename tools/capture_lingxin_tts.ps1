[CmdletBinding()]
param(
    [string]$Serial = "",
    [string]$Text = "hello",
    [string]$OutputPath = "tmp\lingxin_tts.mp3",
    [int]$TimeoutSeconds = 60,
    [switch]$KeepDeviceCopy
)

$ErrorActionPreference = "Stop"

if ($Text -match "\s") {
    throw "Text must not contain spaces because the device command accepts one token."
}

$repoRoot = Split-Path -Parent $PSScriptRoot
$adb = Join-Path $PSScriptRoot "adb\adb.exe"
if (-not (Test-Path -LiteralPath $adb -PathType Leaf)) {
    throw "Bundled adb not found: $adb"
}

$devices = @(& $adb devices | Select-Object -Skip 1 | ForEach-Object {
    if ($_ -match "^(\S+)\s+device$") { $Matches[1] }
})
if ([string]::IsNullOrWhiteSpace($Serial)) {
    if ($devices.Count -ne 1) {
        throw "Expected exactly one online device, found $($devices.Count). Pass -Serial explicitly."
    }
    $Serial = $devices[0]
} elseif ($Serial -notin $devices) {
    throw "Device is not online: $Serial"
}

if (-not [System.IO.Path]::IsPathRooted($OutputPath)) {
    $OutputPath = Join-Path $repoRoot $OutputPath
}
$outputDir = Split-Path -Parent $OutputPath
New-Item -ItemType Directory -Force -Path $outputDir | Out-Null
$partialPath = "$OutputPath.part"
$remotePath = "/NAND:/lingxin_tts.mp3"

# Remove stale evidence before starting a new request.
& $adb -s $Serial shell "rm $remotePath" 2>$null | Out-Null
Remove-Item -LiteralPath $partialPath -Force -ErrorAction SilentlyContinue

Write-Host "Starting Lingxin text-to-speech request on $Serial..."
& $adb -s $Serial shell "lingxin text $Text" | Out-Host
if ($LASTEXITCODE -ne 0) {
    throw "Failed to start Lingxin text request."
}

$deadline = (Get-Date).AddSeconds($TimeoutSeconds)
$lastSize = -1L
$stableCount = 0
while ((Get-Date) -lt $deadline) {
    & $adb -s $Serial pull $remotePath $partialPath 2>$null | Out-Null
    if ($LASTEXITCODE -eq 0 -and (Test-Path -LiteralPath $partialPath -PathType Leaf)) {
        $size = (Get-Item -LiteralPath $partialPath).Length
        if ($size -gt 0 -and $size -eq $lastSize) {
            $stableCount++
        } else {
            $stableCount = 0
        }
        $lastSize = $size
        if ($stableCount -ge 2) {
            break
        }
    }
    Start-Sleep -Seconds 1
}

if (-not (Test-Path -LiteralPath $partialPath -PathType Leaf) -or
    (Get-Item -LiteralPath $partialPath).Length -le 0 -or $stableCount -lt 2) {
    Remove-Item -LiteralPath $partialPath -Force -ErrorAction SilentlyContinue
    throw "No complete Lingxin MP3 was received within $TimeoutSeconds seconds."
}

$stream = [System.IO.File]::OpenRead($partialPath)
try {
    $header = New-Object byte[] 3
    $read = $stream.Read($header, 0, $header.Length)
} finally {
    $stream.Dispose()
}
$isId3 = $read -ge 3 -and $header[0] -eq 0x49 -and $header[1] -eq 0x44 -and $header[2] -eq 0x33
$isFrame = $read -ge 2 -and $header[0] -eq 0xFF -and (($header[1] -band 0xE0) -eq 0xE0)
if (-not $isId3 -and -not $isFrame) {
    throw ("Captured data is not an MP3 stream (header {0:X2}{1:X2}{2:X2})." -f $header[0], $header[1], $header[2])
}

Move-Item -LiteralPath $partialPath -Destination $OutputPath -Force
$file = Get-Item -LiteralPath $OutputPath
$hash = (Get-FileHash -Algorithm SHA256 -LiteralPath $OutputPath).Hash.ToLowerInvariant()

if (-not $KeepDeviceCopy) {
    & $adb -s $Serial shell "rm $remotePath" 2>$null | Out-Null
}

Write-Host "Lingxin MP3 captured to: $($file.FullName)"
Write-Host "Bytes: $($file.Length)"
Write-Host "SHA256: $hash"
