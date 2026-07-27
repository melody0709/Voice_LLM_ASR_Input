# download_models.ps1 - 下载 ASR 模型
# 用法:
#   .\download_models.ps1              # 交互式菜单
#   .\download_models.ps1 -Models 1,3  # 命令行选择
#   .\download_models.ps1 -Models all  # 下载全部
#   .\download_models.ps1 -Destination C:\Users\me\AppData\Local\VoxType\models

param(
    [string]$Models = "",
    [string]$Destination = "",
    [string]$Aria2Path = ""
)

$ErrorActionPreference = "Stop"

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$modelsDir = if ([string]::IsNullOrWhiteSpace($Destination)) {
    Join-Path $scriptDir "models"
}
else {
    [System.IO.Path]::GetFullPath($Destination)
}
$aria2c = if ([string]::IsNullOrWhiteSpace($Aria2Path)) {
    Join-Path $scriptDir "aria2c.exe"
}
else {
    [System.IO.Path]::GetFullPath($Aria2Path)
}

if (!(Test-Path $modelsDir)) {
    New-Item -ItemType Directory -Path $modelsDir | Out-Null
}

$allModels = @(
    @{
        Id   = "firered_ctc"
        Name = "FireRedASR2 CTC int8"
        Size = "~742MB"
        Desc = "Fast, good for default"
        Url  = "https://github.com/k2-fsa/sherpa-onnx/releases/download/asr-models/sherpa-onnx-fire-red-asr2-ctc-zh_en-int8-2026-02-25.tar.bz2"
        File = "sherpa-onnx-fire-red-asr2-ctc-zh_en-int8-2026-02-25.tar.bz2"
        Dir  = "sherpa-onnx-fire-red-asr2-ctc-zh_en-int8-2026-02-25"
    },
    @{
        Id   = "firered_aed"
        Name = "FireRedASR2 AED int8"
        Size = "~1.18GB"
        Desc = "Better quality"
        Url  = "https://github.com/k2-fsa/sherpa-onnx/releases/download/asr-models/sherpa-onnx-fire-red-asr2-zh_en-int8-2026-02-26.tar.bz2"
        File = "sherpa-onnx-fire-red-asr2-zh_en-int8-2026-02-26.tar.bz2"
        Dir  = "sherpa-onnx-fire-red-asr2-zh_en-int8-2026-02-26"
    },
    @{
        Id   = "sensevoice"
        Name = "SenseVoiceSmall int8"
        Size = "~229MB"
        Desc = "Lightweight, low resource"
        Url  = "https://github.com/k2-fsa/sherpa-onnx/releases/download/asr-models/sherpa-onnx-sense-voice-zh-en-ja-ko-yue-int8-2024-07-17.tar.bz2"
        File = "sherpa-onnx-sense-voice-zh-en-ja-ko-yue-int8-2024-07-17.tar.bz2"
        Dir  = "sherpa-onnx-sense-voice-zh-en-ja-ko-yue-int8-2024-07-17"
    },
    @{
        Id   = "punct"
        Name = "CT-Transformer Punctuation"
        Size = "~76MB"
        Desc = "Auto punctuation"
        Url  = "https://github.com/k2-fsa/sherpa-onnx/releases/download/punctuation-models/sherpa-onnx-punct-ct-transformer-zh-en-vocab272727-2024-04-12-int8.tar.bz2"
        File = "sherpa-onnx-punct-ct-transformer-zh-en-vocab272727-2024-04-12-int8.tar.bz2"
        Dir  = "sherpa-onnx-punct-ct-transformer-zh-en-vocab272727-2024-04-12-int8"
    }
)

function Get-SelectedModels {
    param([string]$choice)
    
    if ($choice -match '^(A|a|all)$') {
        return $allModels
    }
    
    $selected = @()
    $parts = $choice -split '[,\s]+' | Where-Object { $_ -ne '' }
    
    foreach ($part in $parts) {
        $part = $part.Trim()
        if ($part -match '^\d+$') {
            $i = [int]$part
            if ($i -ge 1 -and $i -le $allModels.Count) {
                $selected += $allModels[$i - 1]
            }
        }
    }
    
    return $selected
}

function Download-Model {
    param($model)
    
    $targetDir = Join-Path $modelsDir $model.Dir
    
    if (Test-Path $targetDir) {
        Write-Host "[SKIP] $($model.Name) - already exists" -ForegroundColor Green
        return $true
    }
    
    $archivePath = Join-Path $modelsDir $model.File
    
    Write-Host "[DOWNLOAD] $($model.Name) ($($model.Size))..." -ForegroundColor Yellow
    
    if (Test-Path $aria2c) {
        & $aria2c -x 4 -s 4 -c "--console-log-level=notice" "--summary-interval=0" -d $modelsDir -o $model.File $model.Url
        if ($LASTEXITCODE -ne 0) {
            Write-Host "[ERROR] Failed to download $($model.Name)" -ForegroundColor Red
            return $false
        }
    }
    else {
        Write-Host "[INFO] aria2c not found, using curl..." -ForegroundColor Gray
        curl.exe -L -C - -o $archivePath $model.Url
        if ($LASTEXITCODE -ne 0) {
            Write-Host "[ERROR] Failed to download $($model.Name)" -ForegroundColor Red
            return $false
        }
    }
    
    Write-Host "[EXTRACT] $($model.File)..." -ForegroundColor Yellow
    
    try {
        tar -xjf $archivePath -C $modelsDir
        if ($LASTEXITCODE -ne 0) {
            throw "tar failed with exit code $LASTEXITCODE"
        }
    }
    catch {
        Write-Host "[ERROR] Failed to extract $($model.Name): $_" -ForegroundColor Red
        return $false
    }
    
    Remove-Item $archivePath -Force -ErrorAction SilentlyContinue
    Write-Host "[DONE] $($model.Name)" -ForegroundColor Green
    return $true
}

Write-Host "=== VoxType - Model Downloader ===" -ForegroundColor Cyan
Write-Host ""

if (Test-Path $aria2c) {
    Write-Host "[OK] aria2c found - using multi-connection download" -ForegroundColor Green
}
else {
    Write-Host "[INFO] aria2c not found - using curl (slower)" -ForegroundColor Yellow
}
Write-Host ""

$selected = @()

if ($Models) {
    $selected = Get-SelectedModels $Models
}
else {
    Write-Host "Select models to download:" -ForegroundColor Yellow
    Write-Host ""
    
    for ($i = 0; $i -lt $allModels.Count; $i++) {
        $m = $allModels[$i]
        $exists = Test-Path (Join-Path $modelsDir $m.Dir)
        $mark = if ($exists) { "[OK]" } else { "[  ]" }
        Write-Host "  $($i+1). $mark $($m.Name)" -ForegroundColor $(if ($exists) { "Green" } else { "White" }) -NoNewline
        Write-Host "  $($m.Size)" -ForegroundColor Gray -NoNewline
        Write-Host "  - $($m.Desc)" -ForegroundColor Gray
    }
    
    Write-Host ""
    Write-Host "  A. Download ALL models (~2.2GB)" -ForegroundColor Cyan
    Write-Host "  Q. Quit" -ForegroundColor Red
    Write-Host ""
    Write-Host "  Enter numbers separated by commas (e.g. 1,3 or 1,2,3,4)" -ForegroundColor Gray
    
    $choice = Read-Host "Enter choice (1-$($allModels.Count), A, or Q)"
    
    if ($choice -match '^(Q|q|quit)$') {
        Write-Host "Cancelled." -ForegroundColor Gray
        exit 0
    }
    
    $selected = Get-SelectedModels $choice
}

$selectedCount = @($selected).Count

if ($selectedCount -eq 0) {
    Write-Host "No models selected or invalid choice." -ForegroundColor Yellow
    exit 1
}

Write-Host ""
Write-Host "Downloading $selectedCount model(s)..." -ForegroundColor Cyan
Write-Host ""

$success = 0
$failed = 0

foreach ($model in $selected) {
    if (Download-Model $model) {
        $success++
    }
    else {
        $failed++
    }
}

Write-Host ""
Write-Host "=== Download Complete ===" -ForegroundColor Cyan
Write-Host "Success: $success, Failed: $failed" -ForegroundColor $(if ($failed -eq 0) { "Green" } else { "Yellow" })
Write-Host "Models directory: $modelsDir" -ForegroundColor Gray
Write-Host ""
Write-Host "Silero VAD and FireRed VAD are already included." -ForegroundColor Gray

if ($failed -gt 0) {
    exit 1
}
