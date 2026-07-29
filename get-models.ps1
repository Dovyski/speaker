# Downloads the INT8 ONNX models speak.exe needs into .\models
#
#   pwsh -File get-models.ps1
#
# These are the pre-exported ONNX weights for Kyutai's Pocket TTS, so no Python,
# torch or ONNX export step is required. ~200 MB total.

$ErrorActionPreference = 'Stop'

$repo   = 'https://huggingface.co/KevinAHM/pocket-tts-onnx/resolve/main/onnx'
$outDir = Join-Path $PSScriptRoot 'models'

# name = path under $repo
$files = [ordered]@{
    'flow_lm_main_int8.onnx' = 'flow_lm_main_int8.onnx'
    'flow_lm_flow_int8.onnx' = 'flow_lm_flow_int8.onnx'
    'mimi_decoder_int8.onnx' = 'mimi_decoder_int8.onnx'
    'mimi_encoder.onnx'      = 'mimi_encoder.onnx'        # fp32: used for voice encoding
    'text_conditioner.onnx'  = 'text_conditioner.onnx'
    'tokenizer.model'        = 'english_2026-04/tokenizer.model'
}

New-Item -ItemType Directory -Force $outDir | Out-Null

foreach ($name in $files.Keys) {
    $dest = Join-Path $outDir $name
    if (Test-Path $dest) {
        Write-Host "have  $name"
        continue
    }
    Write-Host "fetch $name ..."
    Invoke-WebRequest -Uri "$repo/$($files[$name])" -OutFile "$dest.part"
    Move-Item "$dest.part" $dest -Force
}

$total = (Get-ChildItem $outDir | Measure-Object -Property Length -Sum).Sum
Write-Host ("`nmodels ready in {0} ({1:N0} MB)" -f $outDir, ($total / 1MB))
Write-Host "Next: drop a voice sample WAV in .\voices (see README), then run speak.exe"
