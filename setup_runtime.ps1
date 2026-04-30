# setup_runtime.ps1
# Build embedded Python runtime for Voice LLM ASR Input
# Usage: .\setup_runtime.ps1

$ErrorActionPreference = "Stop"

$PythonVersion = "3.12.10"
$RuntimeDir = Join-Path $PSScriptRoot "runtime"

Write-Host "=== Voice LLM ASR Input Runtime Setup ===" -ForegroundColor Cyan

# Step 1: Download Python embeddable
Write-Host "`n[1/5] Downloading Python $PythonVersion embeddable..." -ForegroundColor Yellow
$PythonZip = Join-Path $PSScriptRoot "python-embed.zip"
$PythonUrl = "https://www.python.org/ftp/python/$PythonVersion/python-$PythonVersion-embed-amd64.zip"

if (Test-Path $RuntimeDir) {
    Write-Host "  runtime/ already exists, removing..." -ForegroundColor DarkGray
    Remove-Item -Recurse -Force $RuntimeDir
}

if (-not (Test-Path $PythonZip)) {
    Invoke-WebRequest -Uri $PythonUrl -OutFile $PythonZip -UseBasicParsing
    Write-Host "  Downloaded: python-embed.zip" -ForegroundColor Green
} else {
    Write-Host "  Using cached: python-embed.zip" -ForegroundColor DarkGray
}

Expand-Archive -Path $PythonZip -DestinationPath $RuntimeDir -Force
Remove-Item $PythonZip
Write-Host "  Extracted to runtime/" -ForegroundColor Green

# Step 2: Configure ._pth to enable site-packages
Write-Host "`n[2/5] Configuring Python import paths..." -ForegroundColor Yellow
$PthFile = Join-Path $RuntimeDir "python312._pth"
(Get-Content $PthFile) -replace '#import site', 'import site' | Set-Content $PthFile
Write-Host "  Enabled site-packages import" -ForegroundColor Green

# Step 3: Install pip
Write-Host "`n[3/5] Installing pip..." -ForegroundColor Yellow
$getPip = Join-Path $RuntimeDir "get-pip.py"
Invoke-WebRequest -Uri "https://bootstrap.pypa.io/get-pip.py" -OutFile $getPip -UseBasicParsing
& (Join-Path $RuntimeDir "python.exe") $getPip --no-warn-script-location
Remove-Item $getPip
Write-Host "  pip installed" -ForegroundColor Green

# Step 4: Install dependencies
Write-Host "`n[4/5] Installing dependencies (this may take a few minutes)..." -ForegroundColor Yellow

$pip = Join-Path $RuntimeDir "python.exe"
$commonArgs = @("-m", "pip", "install", "--no-cache-dir", "--no-warn-script-location")

Write-Host "  Installing sherpa-onnx, numpy..." -ForegroundColor DarkGray
& $pip $commonArgs sherpa-onnx numpy
if ($LASTEXITCODE -ne 0) { Write-Error "Failed to install sherpa-onnx"; exit 1 }

Write-Host "  Installing torch (CPU-only)..." -ForegroundColor DarkGray
& $pip $commonArgs torch --index-url https://download.pytorch.org/whl/cpu
if ($LASTEXITCODE -ne 0) { Write-Error "Failed to install torch"; exit 1 }

Write-Host "  Installing transformers, sentencepiece..." -ForegroundColor DarkGray
& $pip $commonArgs transformers sentencepiece
if ($LASTEXITCODE -ne 0) { Write-Error "Failed to install transformers"; exit 1 }

Write-Host "  All dependencies installed" -ForegroundColor Green

# Step 5: Cleanup
Write-Host "`n[5/5] Cleaning up..." -ForegroundColor Yellow
& $pip -m pip cache purge 2>$null
Remove-Item -Recurse -Force (Join-Path $RuntimeDir "Scripts") -ErrorAction SilentlyContinue
Remove-Item -Recurse -Force (Join-Path $RuntimeDir "pip") -ErrorAction SilentlyContinue

# Verify
Write-Host "`n=== Verification ===" -ForegroundColor Cyan
$testResult = & (Join-Path $RuntimeDir "python.exe") -c "import sherpa_onnx; import torch; import transformers; print('OK: all imports successful')" 2>&1
if ($LASTEXITCODE -eq 0) {
    Write-Host "  $testResult" -ForegroundColor Green
} else {
    Write-Host "  Verification failed: $testResult" -ForegroundColor Red
    exit 1
}

# Show size
$size = (Get-ChildItem -Recurse $RuntimeDir | Measure-Object -Property Length -Sum).Sum / 1MB
Write-Host ("  Runtime size: {0:N0} MB" -f $size) -ForegroundColor Cyan
Write-Host "`n=== Done === Run build.bat to compile the exe, then start VoiceLLMASRInput.exe" -ForegroundColor Cyan
