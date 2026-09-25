# Builds the ultron voice candidates from the source clips in this folder.
#
#   pwsh -File voice-sources/ultron/build.ps1              # A, B and the chosen voice (I)
#   pwsh -File voice-sources/ultron/build.ps1 -All         # every candidate (A-J), to compare
#   pwsh -File voice-sources/ultron/build.ps1 -All -Render # ...and speak each one
#
# The chosen voice (I) is installed as voices/ultron.wav on every run.
# See README.md in this folder for what each candidate is and why I won.

[CmdletBinding()]
param(
    # Build every candidate, not just the single-clip ones.
    [switch] $All,
    # Also synthesize a labelled sample line with each built voice, into samples/.
    [switch] $Render,
    # Pause length left behind when D collapses the pauses of each clip.
    [double] $KeepPause = 0.2,
    # D drops any stretch shorter than this left between two pauses: in these
    # clips those are a chuckle and a music swell, not words.
    [double] $MinKeep = 0.5,
    # F and G: meteor pre-processing against its music bed (the bed sits under
    # ~150 Hz; afftdn is kept light, stronger settings smear the voice).
    [string] $MeteorClean = 'highpass=f=150,afftdn=nf=-25',
    # E, F, H-J: fade applied at each end of the meteor excerpt, in seconds.
    [double] $Fade = 0.04,
    # J: meteor transients to duck, as 'start-end' in seconds of the source
    # clip (see README: a short broadband burst inside the 5.08-5.74 pause).
    [string[]] $Duck = @('5.545-5.635'),
    # J: gain applied inside each -Duck range (0.25 = -12 dB: the burst sits
    # ~12 dB over the pause floor) and the linear ramp either side, in seconds.
    [double] $DuckGain = 0.25,
    [double] $DuckRamp = 0.01
)

$ErrorActionPreference = 'Stop'

$here    = $PSScriptRoot
$root    = (Resolve-Path (Join-Path $here '..\..')).Path
$voices  = Join-Path $root 'voices'
$make    = Join-Path $root 'make-voice.ps1'
$speak   = Join-Path $root 'speak.exe'
$work    = Join-Path $here 'work'
$samples = Join-Path $here 'samples'

$meteor = Join-Path $here 'meteor.wav'          # music bed under the voice, see README
$vision = Join-Path $here 'vision-dialog.wav'   # clean but quiet, one speaker

function Build([string] $name, [string[]] $clips) {
    Write-Host "`n=== $name ===" -ForegroundColor Cyan
    & pwsh -NoProfile -File $make -Out "voices/$name.wav" @clips
    if ($LASTEXITCODE -ne 0) { throw "make-voice failed for $name" }
}

function Duration([string] $file) {
    [double](& ffprobe -v error -show_entries format=duration -of default=nw=1:nk=1 $file)
}

# Shortens every pause in a clip to $keep seconds, cutting the original audio
# (not a filtered copy) at the pause midpoints. Pauses are found by
# silencedetect on a detection copy: $detect lets a noisy clip be judged on a
# band where its bed is quiet (meteor's music sits below ~150 Hz).
function Collapse-Pauses([string] $in, [string] $out, [string] $detect, [double] $threshold, [double] $keep) {
    $det = if ($detect) { "$detect," } else { '' }
    $log = & ffmpeg -hide_banner -i $in -ac 1 -af ("{0}silencedetect=n={1}dB:d={2}" -f $det, $threshold, ($keep + 0.05)) -f null - 2>&1
    $total = Duration $in
    $starts = @(); $ends = @()
    foreach ($line in $log) {
        if ("$line" -match 'silence_start: ([\d.]+)') { $starts += [double]$Matches[1] }
        if ("$line" -match 'silence_end: ([\d.]+)')   { $ends   += [double]$Matches[1] }
    }
    if ($ends.Count -lt $starts.Count) { $ends += $total }   # pause runs to the end

    # Keep ranges: everything except the middle of each pause, leaving
    # $keep/2 of room tone on either side of the cut.
    $keepRanges = @(); $cursor = 0.0
    for ($i = 0; $i -lt $starts.Count; $i++) {
        $s = $starts[$i]; $e = $ends[$i]
        $cutFrom = if ($s -le 0.01) { 0.0 } else { $s + $keep / 2 }
        $cutTo   = if ($e -ge $total - 0.01) { $total } else { $e - $keep / 2 }
        if ($cutTo - $cutFrom -le 0.01) { continue }
        if ($cutFrom -gt $cursor) { $keepRanges += , @($cursor, $cutFrom) }
        $cursor = $cutTo
    }
    if ($cursor -lt $total) { $keepRanges += , @($cursor, $total) }
    $dropped = @($keepRanges | Where-Object { $_[1] - $_[0] -lt $MinKeep })
    $keepRanges = @($keepRanges | Where-Object { $_[1] - $_[0] -ge $MinKeep })
    foreach ($r in $dropped) { Write-Host ("  drop {0,6:N2} - {1,6:N2}  (under {2}s)" -f $r[0], $r[1], $MinKeep) }

    $chains = @(); $labels = ''
    for ($i = 0; $i -lt $keepRanges.Count; $i++) {
        $a = $keepRanges[$i][0].ToString('0.###', [cultureinfo]::InvariantCulture)
        $b = $keepRanges[$i][1].ToString('0.###', [cultureinfo]::InvariantCulture)
        $chains += "[0:a]atrim=start=${a}:end=${b},asetpts=PTS-STARTPTS[k$i]"
        $labels += "[k$i]"
    }
    $graph = ($chains -join ';') + ";${labels}concat=n=$($keepRanges.Count):v=0:a=1[out]"
    & ffmpeg -y -v error -i $in -filter_complex $graph -map '[out]' -c:a pcm_s16le $out
    if ($LASTEXITCODE -ne 0) { throw "ffmpeg failed collapsing $in" }
    Write-Host ("collapsed {0}: {1:N2}s -> {2:N2}s ({3} pauses)" -f (Split-Path $in -Leaf), $total, (Duration $out), $starts.Count)
    foreach ($r in $keepRanges) { Write-Host ("  keep {0,6:N2} - {1,6:N2}" -f $r[0], $r[1]) }
}

# Pauses as @(start, end) pairs, found by silencedetect on a detection copy
# ($detect filters it first). Same detection Collapse-Pauses uses.
function Find-Pauses([string] $in, [string] $detect, [double] $threshold, [double] $minPause) {
    $det = if ($detect) { "$detect," } else { '' }
    $log = & ffmpeg -hide_banner -i $in -ac 1 -af ("{0}silencedetect=n={1}dB:d={2}" -f $det, $threshold, $minPause) -f null - 2>&1
    $starts = @(); $ends = @()
    foreach ($line in $log) {
        if ("$line" -match 'silence_start: ([\d.]+)') { $starts += [double]$Matches[1] }
        if ("$line" -match 'silence_end: ([\d.]+)')   { $ends   += [double]$Matches[1] }
    }
    if ($ends.Count -lt $starts.Count) { $ends += Duration $in }
    $pauses = @()
    for ($i = 0; $i -lt $starts.Count; $i++) { $pauses += , @($starts[$i], $ends[$i]) }
    , $pauses
}

# A volume filter that dips to $gain inside each 'start-end' range, with a
# linear $ramp on either side (a hard enable= switch would click). Small frames
# (asetnsamples) so eval=frame follows the ramp at ~1.5 ms resolution. Ranges
# must not overlap: their dips are summed.
function Duck-Filter([string[]] $ranges, [double] $gain, [double] $ramp) {
    $inv = [cultureinfo]::InvariantCulture
    $f = { param($x) $x.ToString('0.####', $inv) }
    $dips = foreach ($r in $ranges) {
        $s, $e = $r -split '-' | ForEach-Object { [double]::Parse($_, $inv) }
        # 0 outside, 1 inside, linear across the ramps: clip(min(t-(s-r), (e+r)-t)/r, 0, 1)
        "clip(min(t-$(& $f ($s - $ramp)),$(& $f ($e + $ramp))-t)/$(& $f $ramp),0,1)"
    }
    "asetnsamples=n=64,volume=volume='1-$(& $f (1 - $gain))*($($dips -join '+'))':eval=frame"
}

# Computes the meteor excerpt range used by E-J into $script:from/$script:to.
# Needs voices/ultron-B.wav (vision as make-voice trims it) for the budget.
function Set-MeteorRange {
    if (-not (Test-Path (Join-Path $voices 'ultron-B.wav'))) { Build 'ultron-B' @($vision) }
    New-Item -ItemType Directory -Force $work | Out-Null
    $gap = 0.25
    $budget = 30 - (Duration (Join-Path $voices 'ultron-B.wav')) - $gap - 0.1
    $pauses = Find-Pauses $meteor 'highpass=f=150' -24 0.25
    # Start just before the first word (0.1 s of the leading bed, faded in).
    $script:from = if ($pauses.Count -and $pauses[0][0] -le 0.01) { [math]::Max(0.0, $pauses[0][1] - 0.1) } else { 0.0 }
    # End 0.15 s into the last pause that fits (or its middle, if shorter).
    $script:to = $null
    foreach ($p in $pauses) {
        if ($p[0] -le 0.01) { continue }
        $t = $p[0] + [math]::Min(0.15, ($p[1] - $p[0]) / 2)
        if ($t - $script:from -le $budget) { $script:to = $t }
    }
    if (-not $script:to) { throw "no meteor pause fits in $budget s" }
}

# Cleans the whole of meteor with $chain (afftdn adapts over the full clip),
# then cuts the E range out of it. Returns the excerpt's path.
function Meteor-Variant([string] $tag, [string] $chain) {
    $full = Join-Path $work "meteor-$tag.wav"
    $head = Join-Path $work "meteor-$tag-head.wav"
    & ffmpeg -y -v error -i $meteor -af $chain -c:a pcm_f32le $full
    if ($LASTEXITCODE -ne 0) { throw "ffmpeg failed cleaning meteor ($tag)" }
    Cut-Faded $full $head $script:from $script:to $Fade
    $head
}

# Cuts [$from, $to] out of $in with a $fade fade at both ends, so the excerpt
# starts and stops inside room tone instead of on a hard edge.
function Cut-Faded([string] $in, [string] $out, [double] $from, [double] $to, [double] $fade) {
    $inv = [cultureinfo]::InvariantCulture
    $len = $to - $from
    $af = @()
    $af += "atrim=start=$($from.ToString('0.###', $inv)):end=$($to.ToString('0.###', $inv))"
    $af += 'asetpts=PTS-STARTPTS'
    $af += "afade=t=in:st=0:d=$($fade.ToString('0.###', $inv))"
    $af += "afade=t=out:st=$(($len - $fade).ToString('0.###', $inv)):d=$($fade.ToString('0.###', $inv))"
    & ffmpeg -y -v error -i $in -af ($af -join ',') -c:a pcm_f32le $out
    if ($LASTEXITCODE -ne 0) { throw "ffmpeg failed cutting $in" }
    Write-Host ("cut {0} {1:N3}-{2:N3}s ({3:N2}s, fades {4}s)" -f (Split-Path $in -Leaf), $from, $to, $len, $fade)
}

Build 'ultron-A' @($meteor)
Build 'ultron-B' @($vision)

if ($All) {
    # Meteor first. 23.8 s + 17.4 s is over the 30 s engine cap, so make-voice
    # truncates C: it keeps only the first ~6 s of the vision clip.
    Build 'ultron-C' @($meteor, $vision)

    # D fits both clips under the cap with nothing truncated, by collapsing the
    # pauses inside each clip instead of dropping its tail.
    New-Item -ItemType Directory -Force $work | Out-Null
    $meteorShort = Join-Path $work 'meteor-collapsed.wav'
    $visionShort = Join-Path $work 'vision-dialog-collapsed.wav'
    Collapse-Pauses $meteor $meteorShort 'highpass=f=150' -24 $KeepPause
    Collapse-Pauses $vision $visionShort ''               -45 $KeepPause
    Build 'ultron-D' @($meteorShort, $visionShort)
    $d = Duration (Join-Path $voices 'ultron-D.wav')
    if ($d -ge 29.99) { Write-Warning "ultron-D hit the 30 s cap ($d s) — lower -KeepPause" }

    # E/F: vision first (clean anchor), then the head of meteor, cut inside a
    # real pause so the join never lands mid-word. Budget = 30 s cap - vision
    # as make-voice trims it (= B) - make-voice's 0.25 s gap - a small margin.
    Set-MeteorRange
    $from = $script:from; $to = $script:to
    $meteorHead  = Join-Path $work 'meteor-head.wav'
    # (not $meteorClean: PowerShell names are case-insensitive, it is the param)
    $cleanFull = Join-Path $work 'meteor-clean.wav'
    $meteorCleanHead = Join-Path $work 'meteor-clean-head.wav'
    Cut-Faded $meteor $meteorHead $from $to $Fade
    Build 'ultron-E' @($vision, $meteorHead)

    # F: same cut, meteor de-bedded first. The filter runs on the whole clip
    # (afftdn adapts over time), then the identical range is cut out of it.
    & ffmpeg -y -v error -i $meteor -af $MeteorClean -c:a pcm_f32le $cleanFull
    if ($LASTEXITCODE -ne 0) { throw "ffmpeg failed cleaning meteor" }
    Cut-Faded $cleanFull $meteorCleanHead $from $to $Fade
    Build 'ultron-F' @($vision, $meteorCleanHead)

    # G: meteor alone, de-bedded, full length. Tests whether A improves with
    # the music cut alone. make-voice's loudnorm re-normalizes it.
    Build 'ultron-G' @($cleanFull)

    # H-J: the middle ground between E (raw, right timbre, bed + transients)
    # and F (clean, timbre lost). Same cut and order as E; only the meteor
    # chain differs.
    #   H  high-pass only: the bed's low end goes, no spectral denoise at all
    #   I  F's chain at half the reduction (afftdn nr 6 dB instead of 12)
    #   J  gentler high-pass, noise tracking, and the -Duck transients dipped
    $variants = [ordered]@{
        'H' = 'highpass=f=150'
        'I' = 'highpass=f=150,afftdn=nr=6:nf=-25'
        'J' = 'highpass=f=100,afftdn=nr=6:nf=-30:tn=1' + $(if ($Duck) { ',' + (Duck-Filter $Duck $DuckGain $DuckRamp) } else { '' })
    }
    foreach ($tag in $variants.Keys) {
        Write-Host "meteor $tag chain: $($variants[$tag])"
        Build "ultron-$tag" @($vision, (Meteor-Variant $tag $variants[$tag]))
    }
}

if ($Render) {
    $lines = @{
        'A' = "Candidate Alpha. There is only one path to peace: your extinction."
        'B' = "Candidate Bravo. I was designed to save the world. People would look to the sky and see hope."
        'C' = "Candidate Charlie. Everyone creates the thing they dread."
        'D' = "Candidate Delta. You're all puppets, tangled in strings."
        'E' = "Candidate Echo. I'm going to show you something beautiful."
        'F' = "Candidate Foxtrot. When the dust settles, the only thing living in this world will be metal."
        'G' = "Candidate Golf. I know you mean well. You just didn't think it through."
        'H' = "Candidate Hotel. There are no strings on me."
        'I' = "Candidate India. I'm going to tear you apart from the inside."
        'J' = "Candidate Juliet. Everyone creates the thing they dread."
    }
    New-Item -ItemType Directory -Force $samples | Out-Null
    foreach ($v in Get-ChildItem (Join-Path $voices 'ultron-*.wav')) {
        $letter = $v.BaseName.Substring('ultron-'.Length)
        if (-not $lines.ContainsKey($letter)) { continue }
        $wav = Join-Path $samples "$($v.BaseName).wav"
        Write-Host "rendering $($v.BaseName) ..."
        # --voice resolves against the voices dir, so pass the bare filename.
        # --local renders in-process and leaves any running daemon alone.
        & $speak --no-orb --local --voice $v.Name --save $wav $lines[$letter] | Out-Null
    }
    Write-Host "`nsamples in $samples" -ForegroundColor Green
}

# The chosen voice: I (vision-dialog + half-denoised meteor head) won the
# 2026-09-25 audition. E had the timbre but audible bed/transients, F was
# clean but lost the timbre; I is the middle ground. See README.md.
$chosen = 'ultron-I'
if (-not $All) { Set-MeteorRange; Build $chosen @($vision, (Meteor-Variant 'I' 'highpass=f=150,afftdn=nr=6:nf=-25')) }
Copy-Item (Join-Path $voices "$chosen.wav") (Join-Path $voices 'ultron.wav') -Force
Write-Host "`ninstalled voices/ultron.wav (= $chosen)" -ForegroundColor Green
