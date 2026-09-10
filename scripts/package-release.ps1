<#
Assembles the release artifact: dist\speak-win-x64.zip plus dist\SHA256SUMS.txt.

    pwsh -File scripts\package-release.ps1 -Version 1.0.0
    pwsh -File scripts\package-release.ps1 -Version 1.0.0 -NoBuild   # reuse speak.exe

What lands in the zip is everything needed to *run* speak.exe except the
models: the binary, onnxruntime.dll, the default voice, the model fetcher, the
Claude Code hooks and skill, and a short INSTALL.txt. The models are ~200 MB of
third-party weights from HuggingFace, so users run get-models.ps1 themselves
(see docs\references\install.md).

The default voice is not committed (voices\ is gitignored), so it is rebuilt
here from the tracked source clip in voice-sources\jarvis\ via make-voice.ps1,
which needs ffmpeg. An already-built voices\jarvis.wav is used as-is.
#>
[CmdletBinding()]
param(
    # Release version, without the leading "v". Must match SPEAK_VERSION in
    # version.h, so a tag can never ship a binary reporting another number.
    [Parameter(Mandatory)][string]$Version,

    # Package an existing speak.exe instead of building one.
    [switch]$NoBuild,

    # Where the zip and the checksum file go.
    [string]$OutDir
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
if (-not $OutDir) { $OutDir = Join-Path $root 'dist' }

function Step([string]$m) { Write-Host "==> $m" -ForegroundColor Cyan }
function Note([string]$m) { Write-Host "    $m" -ForegroundColor DarkGray }

# ── the version is the one in version.h ─────────────────────────────────────
if ($Version -notmatch '^\d+\.\d+\.\d+(-[0-9A-Za-z.-]+)?$') {
    throw "-Version '$Version' is not a semver like 1.0.0 or 1.1.0-rc.1"
}

$versionHeader = Join-Path $root 'version.h'
$declared = (Select-String -Path $versionHeader -Pattern '^\s*#define\s+SPEAK_VERSION\s+"([^"]+)"' `
    | Select-Object -First 1).Matches[0].Groups[1].Value
if (-not $declared) { throw "could not read SPEAK_VERSION from $versionHeader" }
if ($declared -ne $Version) {
    throw "version mismatch: -Version $Version but version.h says $declared. Bump version.h first."
}
Step "packaging speak $Version"

# ── build ───────────────────────────────────────────────────────────────────
$exe = Join-Path $root 'speak.exe'
$dll = Join-Path $root 'onnxruntime.dll'

if ($NoBuild) {
    if (-not (Test-Path $exe)) { throw "-NoBuild given but $exe does not exist" }
    Note "reusing $exe"
} else {
    Step 'building speak.exe'
    & cmd /c "`"$(Join-Path $root 'build.bat')`""
    if ($LASTEXITCODE -ne 0) { throw "build.bat failed with exit code $LASTEXITCODE" }
}

foreach ($f in @($exe, $dll)) {
    if (-not (Test-Path $f)) { throw "missing build output: $f" }
}

# The binary must agree with the version we are packaging. This catches a stale
# speak.exe under -NoBuild, which is the whole point of having --version.
$reported = (& $exe --version 2>&1 | Out-String).Trim()
if ($reported -ne "speak $Version") {
    throw "speak.exe reports '$reported', expected 'speak $Version' — rebuild (drop -NoBuild)"
}
Note "speak.exe reports: $reported"

# ── the default voice ───────────────────────────────────────────────────────
# voices\ is gitignored, so on a fresh clone (CI) there is nothing to ship.
# Rebuild the chosen voice from its tracked source clip.
$voicesDir = Join-Path $root 'voices'
$defaultVoice = Join-Path $voicesDir 'jarvis.wav'

if (Test-Path $defaultVoice) {
    Note "using existing voices\jarvis.wav"
} else {
    Step 'building the default voice from voice-sources\jarvis'
    if (-not (Get-Command ffmpeg -ErrorAction SilentlyContinue)) {
        throw "voices\jarvis.wav is absent and ffmpeg is not on PATH, so it cannot be built. Install ffmpeg, or build the voice by hand (see voice-sources\jarvis\README.md)."
    }
    # jarvis-I, the chosen candidate: the restaurant clip on its own.
    $src = Join-Path $root 'voice-sources\jarvis\jarvis-restaurant.wav'
    if (-not (Test-Path $src)) { throw "missing voice source: $src" }
    Push-Location $root
    try {
        & pwsh -NoProfile -File (Join-Path $root 'make-voice.ps1') -Out 'voices/jarvis.wav' $src
        if ($LASTEXITCODE -ne 0) { throw "make-voice.ps1 failed with exit code $LASTEXITCODE" }
    } finally { Pop-Location }
    if (-not (Test-Path $defaultVoice)) { throw "make-voice.ps1 did not produce $defaultVoice" }
}

# ── stage ───────────────────────────────────────────────────────────────────
$stage = Join-Path ([System.IO.Path]::GetTempPath()) ("speak-pkg-" + [System.IO.Path]::GetRandomFileName())
New-Item -ItemType Directory -Force $stage | Out-Null

try {
    Step 'staging files'

    # Top-level files, flat in the zip root.
    foreach ($f in @('speak.exe', 'onnxruntime.dll', 'get-models.ps1', 'make-voice.ps1', 'LICENSE')) {
        $p = Join-Path $root $f
        if (-not (Test-Path $p)) { throw "missing file for the package: $p" }
        Copy-Item $p (Join-Path $stage $f)
    }

    # voices\: only the built voice samples, never the conditioning cache (it is
    # keyed to a local path and is regenerated on first use anyway).
    New-Item -ItemType Directory -Force (Join-Path $stage 'voices') | Out-Null
    Copy-Item $defaultVoice (Join-Path $stage 'voices\jarvis.wav')

    # hooks\: every .ps1, including the installer.
    New-Item -ItemType Directory -Force (Join-Path $stage 'hooks') | Out-Null
    $hooks = Get-ChildItem (Join-Path $root 'hooks') -Filter *.ps1 -File
    if (-not $hooks) { throw "no .ps1 files found in hooks\" }
    $hooks | ForEach-Object { Copy-Item $_.FullName (Join-Path $stage 'hooks') }

    # skill\: the agent skill, directory structure intact.
    Copy-Item (Join-Path $root 'skill') (Join-Path $stage 'skill') -Recurse

    # The one piece of prose in the zip: a signpost, not documentation.
    $install = @"
speak $Version — text to speech with an on-screen orb
https://github.com/Dovyski/speaker

This archive has everything except the models, which are ~200 MB of
third-party weights and are downloaded separately.

Quick start
-----------
  1. Unzip this folder somewhere permanent, e.g. %LOCALAPPDATA%\speak
  2. pwsh -File get-models.ps1          (downloads the models, ~200 MB)
  3. .\speak.exe "Hello world."         (first run loads the model, ~5 s)
  4. Start-Process .\speak.exe --serve -WindowStyle Hidden
                                        (the daemon: ~100 ms per line after this)

Windows will warn that the binary is unsigned the first time you run it.

The full instructions — autostart, upgrading, verifying the download, and the
Claude Code hooks in hooks\ and the agent skill in skill\ — are here:

  https://github.com/Dovyski/speaker/blob/main/docs/references/install.md
"@
    Set-Content (Join-Path $stage 'INSTALL.txt') $install -Encoding utf8

    # ── zip ─────────────────────────────────────────────────────────────────
    Step 'compressing'
    New-Item -ItemType Directory -Force $OutDir | Out-Null
    $zip = Join-Path $OutDir 'speak-win-x64.zip'
    if (Test-Path $zip) { Remove-Item $zip -Force }
    Compress-Archive -Path (Join-Path $stage '*') -DestinationPath $zip -CompressionLevel Optimal

    # ── checksums ───────────────────────────────────────────────────────────
    # sha256sum-compatible: "<hash>  <name>", so `sha256sum -c` works too.
    $hash = (Get-FileHash $zip -Algorithm SHA256).Hash.ToLower()
    $sums = Join-Path $OutDir 'SHA256SUMS.txt'
    # LF, no trailing newline noise: Set-Content emits CRLF on Windows, which makes
    # `sha256sum -c` on Linux/Git Bash look for "speak-win-x64.zip".
    $zipName = Split-Path $zip -Leaf
    $sumsLine = $hash + '  ' + $zipName + "`n"
    [IO.File]::WriteAllText($sums, $sumsLine, [Text.UTF8Encoding]::new($false))

    $mb = [math]::Round((Get-Item $zip).Length / 1MB, 1)
    Write-Host ""
    Step "wrote $zip ($mb MB)"
    Note "sha256 $hash"
    Note "sums   $sums"
    Write-Host ""
    Get-ChildItem $stage -Recurse -File |
        ForEach-Object { "{0,10:N0}  {1}" -f $_.Length, $_.FullName.Substring($stage.Length + 1) }
}
finally {
    Remove-Item $stage -Recurse -Force -ErrorAction SilentlyContinue
}
