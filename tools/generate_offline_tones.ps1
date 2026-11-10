param(
    [Parameter(Mandatory=$true)][string]$Ffmpeg,
    [string]$Voice = 'Microsoft Huihui Desktop',
    [string]$OutputDirectory = (Join-Path $PSScriptRoot '../res/arcs-mini/tone'),
    [switch]$Replace
)
$ErrorActionPreference = 'Stop'
if (-not (Test-Path -LiteralPath $Ffmpeg -PathType Leaf)) { throw "FFmpeg not found: $Ffmpeg" }
Add-Type -AssemblyName System.Speech
$synth = New-Object System.Speech.Synthesis.SpeechSynthesizer
$synth.SelectVoice($Voice)
$synth.Rate = 0
$synth.Volume = 100
$clips = [ordered]@{
    '117_offline_mode.mp3' = '已进入离线模式'
    '118_online_mode.mp3' = '正在连接网络'
    '119_system_version.mp3' = '当前系统版本'
    '120_decimal_point.mp3' = '点'
    '121_digit_0.mp3' = '零'
    '122_digit_1.mp3' = '一'
    '123_digit_2.mp3' = '二'
    '124_digit_3.mp3' = '三'
    '125_digit_4.mp3' = '四'
    '126_digit_5.mp3' = '五'
    '127_digit_6.mp3' = '六'
    '128_digit_7.mp3' = '七'
    '129_digit_8.mp3' = '八'
    '130_digit_9.mp3' = '九'
}
$outputPath = [IO.Path]::GetFullPath($OutputDirectory)
New-Item -ItemType Directory -Force -Path $outputPath | Out-Null
$temporaryDirectory = Join-Path ([IO.Path]::GetTempPath()) ('offline-tones-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $temporaryDirectory | Out-Null
try {
    # Preflight every destination before producing any output; never silently
    # overwrite operator-provided replacement recordings.
    foreach ($name in $clips.Keys) {
        if ((Test-Path -LiteralPath (Join-Path $outputPath $name)) -and -not $Replace) {
            throw "Destination already exists: $name. Use -Replace deliberately."
        }
    }
    foreach ($entry in $clips.GetEnumerator()) {
        $wav = Join-Path $temporaryDirectory ($entry.Key + '.wav')
        $mp3 = Join-Path $temporaryDirectory $entry.Key
        $synth.SetOutputToWaveFile($wav)
        $synth.Speak($entry.Value)
        $synth.SetOutputToNull()
        # Mono speech at 32 kbps leaves headroom in the fixed 1 MiB tone partition.
        & $Ffmpeg -hide_banner -loglevel error -y -i $wav -ac 1 -ar 16000 -codec:a libmp3lame -b:a 32k -write_xing 0 $mp3
        if ($LASTEXITCODE -ne 0 -or (Get-Item -LiteralPath $mp3).Length -eq 0) {
            throw "MP3 conversion failed: $($entry.Key)"
        }
        Copy-Item -LiteralPath $mp3 -Destination (Join-Path $outputPath $entry.Key) -Force:$Replace
        Write-Host "$($entry.Key): $($entry.Value)"
    }
}
finally {
    $synth.Dispose()
    # Only remove explicit generated files inside the unique temporary directory.
    Get-ChildItem -LiteralPath $temporaryDirectory -File | Remove-Item
    Remove-Item -LiteralPath $temporaryDirectory
}
