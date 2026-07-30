# Builds one voice sample out of several recordings.
#
#   pwsh -File make-voice.ps1 -Out voices/fernando.wav clip1.wav clip2.m4a clip3.mp3
#
# The engine conditions on a single file (and uses at most 30 s of it), so
# several short takes have to be joined. This normalizes each clip to a common
# format and loudness, trims leading/trailing silence, joins them with a short
# gap, and caps the result at 30 s.
#
# Requires ffmpeg on PATH.

# PositionalBinding=$false so that bare arguments all land in -Clips instead of
# spilling into -MaxSeconds/-GapSeconds.
[CmdletBinding(PositionalBinding = $false)]
param(
    [Parameter(Mandatory, Position = 0)][string] $Out,
    [Parameter(Mandatory, ValueFromRemainingArguments)][string[]] $Clips,
    [int] $MaxSeconds = 30,
    [double] $GapSeconds = 0.25,
    # FFT denoiser, for source clips with background hiss. Helps with steady
    # noise; it cannot remove music or another voice.
    [switch] $Denoise
)

$ErrorActionPreference = 'Stop'

if (-not (Get-Command ffmpeg -ErrorAction SilentlyContinue)) {
    throw "ffmpeg not found on PATH"
}

$work = Join-Path ([System.IO.Path]::GetTempPath()) ("make-voice-" + [System.IO.Path]::GetRandomFileName())
New-Item -ItemType Directory -Force $work | Out-Null

try {
    $parts = @()
    $i = 0
    foreach ($clip in $Clips) {
        if (-not (Test-Path $clip)) { throw "no such file: $clip" }
        $part = Join-Path $work ("part{0:d3}.wav" -f $i)

        # 24 kHz mono, silence trimmed at both ends, loudness-normalized so clips
        # recorded at different levels do not fight each other.
        $steps = @()
        if ($Denoise) { $steps += 'afftdn=nf=-25' }
        $steps += 'areverse,silenceremove=start_periods=1:start_silence=0.05:start_threshold=-50dB'
        $steps += 'areverse,silenceremove=start_periods=1:start_silence=0.05:start_threshold=-50dB'
        $steps += 'loudnorm=I=-20:TP=-2:LRA=7'
        $steps += 'aresample=24000'
        $filter = $steps -join ','

        & ffmpeg -y -v error -i $clip -ac 1 -af $filter -c:a pcm_f32le $part
        if ($LASTEXITCODE -ne 0) { throw "ffmpeg failed on $clip" }

        $dur = [double](& ffprobe -v error -show_entries format=duration -of default=nw=1:nk=1 $part)
        Write-Host ("{0,6:N2}s  {1}" -f $dur, (Split-Path $clip -Leaf))
        $parts += $part
        $i++
    }

    # A short silence between takes, so two clips do not butt up mid-syllable.
    $gap = Join-Path $work 'gap.wav'
    & ffmpeg -y -v error -f lavfi -i "anullsrc=r=24000:cl=mono" -t $GapSeconds -c:a pcm_f32le $gap

    $sequence = @()
    for ($j = 0; $j -lt $parts.Count; $j++) {
        if ($j -gt 0) { $sequence += $gap }
        $sequence += $parts[$j]
    }

    $listFile = Join-Path $work 'list.txt'
    $sequence | ForEach-Object { "file '$($_ -replace '\\', '/')'" } | Set-Content $listFile -Encoding utf8

    $outFull = [System.IO.Path]::GetFullPath((Join-Path (Get-Location) $Out))
    New-Item -ItemType Directory -Force (Split-Path $outFull) | Out-Null
    & ffmpeg -y -v error -f concat -safe 0 -i $listFile -t $MaxSeconds -c:a pcm_f32le $outFull
    if ($LASTEXITCODE -ne 0) { throw "ffmpeg failed joining clips" }

    $total = [double](& ffprobe -v error -show_entries format=duration -of default=nw=1:nk=1 $outFull)
    Write-Host ("`nwrote {0} ({1:N2}s, 24 kHz mono)" -f $outFull, $total)
    if ($total -lt 5) {
        Write-Warning "under 5s of speech — cloning quality will suffer, add more takes"
    }
    Write-Host ("try it:  speak.exe --voice `"{0}`" `"Testing my own voice.`"" -f $outFull)
}
finally {
    Remove-Item $work -Recurse -Force -ErrorAction SilentlyContinue
}
