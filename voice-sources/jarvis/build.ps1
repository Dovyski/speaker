# Rebuilds the jarvis voice candidates from the source clips in this folder.
#
#   pwsh -File voice-sources/jarvis/build.ps1              # the chosen voice only
#   pwsh -File voice-sources/jarvis/build.ps1 -All         # every candidate, to compare
#   pwsh -File voice-sources/jarvis/build.ps1 -All -Render # ...and speak each one
#
# See README.md in this folder for what was tried and how each one sounded.

[CmdletBinding()]
param(
    # Build every historical candidate, not just the chosen voice.
    [switch] $All,
    # Also synthesize a sample sentence with each built voice, into samples/.
    [switch] $Render
)

$ErrorActionPreference = 'Stop'

$here   = $PSScriptRoot
$root   = (Resolve-Path (Join-Path $here '..\..')).Path
$voices = Join-Path $root 'voices'
$make   = Join-Path $root 'make-voice.ps1'
$speak  = Join-Path $root 'speak.exe'

$message = Join-Path $here 'jarvis-voice-message.mp3'
$clean   = Join-Path $here 'jarvis-clean-few-seconds.mp3'
$battery = Join-Path $here 'jarvis-low-battery.mp3'
$aswish  = Join-Path $here 'jarvis-as-you-wish-sir.mp3'
$intro   = Join-Path $here 'jarvis-intro-1.mp3'   # noisy: excluded on purpose
$restaurant = Join-Path $here 'jarvis-restaurant.wav'

function Build([string] $name, [string[]] $clips, [switch] $Denoise) {
    Write-Host "`n=== $name ===" -ForegroundColor Cyan
    $out = "voices/$name.wav"
    if ($Denoise) { & pwsh -NoProfile -File $make -Out $out -Denoise @clips }
    else          { & pwsh -NoProfile -File $make -Out $out @clips }
}

# The chosen voice: the restaurant clip alone beat every merged candidate by
# ear. The intro clip stays deliberately excluded — see README.md.
$chosen = 'jarvis-I'
Build $chosen @($restaurant)
Copy-Item (Join-Path $voices "$chosen.wav") (Join-Path $voices 'jarvis.wav') -Force
Write-Host "`ninstalled voices/jarvis.wav (= $chosen)" -ForegroundColor Green

if ($All) {
    Build 'jarvis-A' @($aswish, $battery)
    Build 'jarvis-B' @($intro)
    Build 'jarvis-C' @($intro) -Denoise
    Build 'jarvis-D' @($intro, $aswish, $battery) -Denoise
    Build 'jarvis-E' @($clean, $battery, $aswish)
    Build 'jarvis-F' @($clean, $battery, $aswish, $intro) -Denoise
    Build 'jarvis-G' @($message, $clean, $battery, $aswish)
    Build 'jarvis-H' @($message)
    Build 'jarvis-J' @($restaurant, $message, $clean, $battery, $aswish)
    Build 'jarvis-K' @($message, $clean, $battery, $aswish, $restaurant)
}

if ($Render) {
    $line = "Good morning, sir. All agents have finished. I've prepared a report for you."
    $dir  = Join-Path $root 'samples'
    New-Item -ItemType Directory -Force $dir | Out-Null
    foreach ($v in Get-ChildItem (Join-Path $voices 'jarvis-*.wav')) {
        $wav = Join-Path $dir ("sample-" + $v.BaseName + ".wav")
        Write-Host "rendering $($v.BaseName) ..."
        # --voice resolves against the voices dir, so pass the bare filename.
        & $speak --no-orb --local --voice $v.Name --save $wav $line | Out-Null
    }
    Write-Host "`nsamples in $dir" -ForegroundColor Green
}

Write-Host "`nRestart the daemon to pick up a new voices/jarvis.wav:"
Write-Host "  speak.exe --stop; Start-Process speak.exe --serve -WindowStyle Hidden"
