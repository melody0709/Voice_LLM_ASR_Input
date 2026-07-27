[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [string]$BuildRoot,
    [Parameter(Mandatory)]
    [string]$RuntimeDirectory,
    [Parameter(Mandatory)]
    [string]$InstallManifest,
    [switch]$AllowIncompleteRuntime,
    [switch]$SkipRuntime
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

function Get-AbsolutePath([string]$Path) {
    return [System.IO.Path]::GetFullPath($Path)
}

function Test-PathWithinRoot([string]$Path, [string]$Root) {
    $normalizedPath = (Get-AbsolutePath $Path).TrimEnd('\', '/')
    $normalizedRoot = (Get-AbsolutePath $Root).TrimEnd('\', '/')
    return [string]::Equals($normalizedPath, $normalizedRoot,
        [System.StringComparison]::OrdinalIgnoreCase) -or
        $normalizedPath.StartsWith($normalizedRoot + [System.IO.Path]::DirectorySeparatorChar,
            [System.StringComparison]::OrdinalIgnoreCase)
}

function Assert-BuildRootLayout([string]$Root) {
    # Keep the generated root predictable. CMake owns its build tree; only the
    # installed runtime is checked recursively against runtime-manifest.json.
    $allowed = @{
        'cmake' = 'directory'
        'run' = 'directory'
        'artifacts' = 'directory'
        'logs' = 'directory'
        'packages' = 'directory'
        'README.txt' = 'file'
    }

    foreach ($item in Get-ChildItem -LiteralPath $Root -Force) {
        $expectedType = $allowed[$item.Name]
        if ($null -eq $expectedType) {
            $names = ($allowed.Keys | Sort-Object) -join ', '
            throw "Unexpected top-level build item: $($item.FullName). Allowed items: $names"
        }
        if ($expectedType -eq 'directory' -and !$item.PSIsContainer) {
            throw "Build layout expects a directory at: $($item.FullName)"
        }
        if ($expectedType -eq 'file' -and $item.PSIsContainer) {
            throw "Build layout expects a file at: $($item.FullName)"
        }
    }
}

function Get-Sha256([string]$LiteralPath) {
    $algorithm = [System.Security.Cryptography.SHA256]::Create()
    $stream = [System.IO.File]::OpenRead($LiteralPath)
    try {
        return ([System.BitConverter]::ToString($algorithm.ComputeHash($stream))).Replace('-', '').ToLowerInvariant()
    }
    finally {
        $stream.Dispose()
        $algorithm.Dispose()
    }
}

function Get-RuntimeInventory([string]$Root) {
    $rootPath = Get-AbsolutePath $Root
    $items = Get-ChildItem -LiteralPath $rootPath -Recurse -Force -File
    $byPath = @{}
    foreach ($item in $items) {
        if (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
            throw "Runtime payload contains a reparse point: $($item.FullName)"
        }
        if (!(Test-PathWithinRoot $item.FullName $rootPath)) {
            throw "Runtime payload escapes its root: $($item.FullName)"
        }
        $relative = $item.FullName.Substring($rootPath.TrimEnd('\', '/').Length).
            TrimStart('\', '/').Replace('\', '/')
        $invalidSegment = @($relative.Split('/') | Where-Object { $_ -eq '.' -or $_ -eq '..' }).Count -ne 0
        if ([string]::IsNullOrWhiteSpace($relative) -or
            $relative.Contains(':') -or
            $invalidSegment) {
            throw "Invalid runtime relative path: $relative"
        }
        $key = $relative.ToLowerInvariant()
        if ($byPath.ContainsKey($key)) {
            throw "Runtime payload has a case-colliding path: $relative"
        }
        $byPath[$key] = [PSCustomObject]@{
            Path = $relative
            Size = [Int64]$item.Length
            Sha256 = Get-Sha256 $item.FullName
        }
    }
    return $byPath
}

$buildRootPath = Get-AbsolutePath $BuildRoot
$runtimeRootPath = Get-AbsolutePath $RuntimeDirectory
$installManifestPath = Get-AbsolutePath $InstallManifest

if (!(Test-Path -LiteralPath $buildRootPath -PathType Container)) {
    throw "Build root is missing: $buildRootPath"
}
if ($AllowIncompleteRuntime -and $SkipRuntime) {
    throw 'AllowIncompleteRuntime and SkipRuntime cannot be used together'
}
if (!(Test-PathWithinRoot $runtimeRootPath $buildRootPath)) {
    throw "Runtime directory escapes build root: $runtimeRootPath"
}
if (!(Test-PathWithinRoot $installManifestPath $buildRootPath)) {
    throw "CMake Runtime install manifest escapes build root: $installManifestPath"
}
Assert-BuildRootLayout $buildRootPath

if ($SkipRuntime) {
    Write-Output "Validated build root layout: $buildRootPath"
    return
}
if (!(Test-Path -LiteralPath $runtimeRootPath -PathType Container)) {
    if ($AllowIncompleteRuntime) {
        Write-Output "Validated build root layout; runtime is not present yet: $buildRootPath"
        return
    }
    throw "Runtime directory is missing: $runtimeRootPath"
}
if (!(Test-Path -LiteralPath $installManifestPath -PathType Leaf)) {
    if ($AllowIncompleteRuntime) {
        Write-Output "Validated build root layout; CMake Runtime install manifest is not present yet: $buildRootPath"
        return
    }
    throw "CMake Runtime install manifest is missing: $installManifestPath"
}

foreach ($required in @(
        "VoxType.exe",
        "sherpa-onnx-cxx-api.dll",
        "sherpa-onnx-c-api.dll",
        "onnxruntime.dll",
        "kaldi-native-fbank-core.dll",
        "aria2c.exe",
        "download_models.ps1",
        "models/silero_vad.int8.onnx",
        "models/fireredvad_stream_vad_with_cache.onnx",
        "runtime-manifest.json")) {
    $path = Join-Path $runtimeRootPath $required.Replace('/', '\')
    if (!(Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Required runtime file is missing: $required"
    }
}

$manifestPath = Join-Path $runtimeRootPath "runtime-manifest.json"
try {
    $manifest = Get-Content -LiteralPath $manifestPath -Raw -Encoding UTF8 | ConvertFrom-Json
}
catch {
    throw "runtime-manifest.json is not valid JSON: $($_.Exception.Message)"
}
if ($manifest.schema_version -ne 1 -or $manifest.product -ne "VoxType" -or
    [string]::IsNullOrWhiteSpace([string]$manifest.version)) {
    throw "runtime-manifest.json has an unsupported identity"
}

$actual = Get-RuntimeInventory $runtimeRootPath
$expected = @{}
foreach ($entry in @($manifest.files)) {
    $relative = [string]$entry.path
    $key = $relative.ToLowerInvariant()
    if ([string]::IsNullOrWhiteSpace($relative) -or $expected.ContainsKey($key)) {
        throw "runtime-manifest.json has an invalid or duplicate path: $relative"
    }
    $expected[$key] = $entry
}

foreach ($key in $expected.Keys) {
    if (!$actual.ContainsKey($key)) {
        throw "Runtime manifest file is missing: $($expected[$key].path)"
    }
    $actualEntry = $actual[$key]
    $expectedEntry = $expected[$key]
    if ([Int64]$expectedEntry.size -ne $actualEntry.Size -or
        [string]$expectedEntry.sha256 -ne $actualEntry.Sha256) {
        throw "Runtime manifest hash mismatch: $($expectedEntry.path)"
    }
}
foreach ($key in $actual.Keys) {
    if ($actual[$key].Path -eq "runtime-manifest.json") { continue }
    if (!$expected.ContainsKey($key)) {
        throw "Runtime payload has a file missing from its manifest: $($actual[$key].Path)"
    }
}

Write-Output ("Validated runtime layout: {0} files, version {1}" -f
    $expected.Count, $manifest.version)
