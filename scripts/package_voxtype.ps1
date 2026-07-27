[CmdletBinding()]
param(
    [Parameter(Mandatory)] [ValidateSet('Portable', 'Msi', 'All')] [string]$Mode,
    [Parameter(Mandatory)] [string]$BuildRoot,
    [Parameter(Mandatory)] [string]$RuntimeDirectory,
    [Parameter(Mandatory)] [string]$InstallManifest,
    [Parameter(Mandatory)] [ValidatePattern('^[0-9]+\.[0-9]+\.[0-9]+$')] [string]$ProductVersion,
    [switch]$RequireSigning
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Get-AbsolutePath([string]$Path) { [System.IO.Path]::GetFullPath($Path) }
function Test-PathWithinRoot([string]$Path, [string]$Root) {
    $path = Get-AbsolutePath $Path
    $root = (Get-AbsolutePath $Root).TrimEnd('\', '/')
    [string]::Equals($path.TrimEnd('\', '/'), $root, [System.StringComparison]::OrdinalIgnoreCase) -or
        $path.StartsWith($root + [System.IO.Path]::DirectorySeparatorChar, [System.StringComparison]::OrdinalIgnoreCase)
}
function Assert-PathWithinRoot([string]$Path, [string]$Root, [string]$Description) {
    if (!(Test-PathWithinRoot $Path $Root)) { throw "$Description escapes its permitted root: $Path" }
}
function Test-ReparsePoint([System.IO.FileSystemInfo]$Item) {
    ($Item.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0
}
function Get-Sha256([string]$Path) {
    $stream = [System.IO.File]::OpenRead($Path)
    $algorithm = [System.Security.Cryptography.SHA256]::Create()
    try { ([System.BitConverter]::ToString($algorithm.ComputeHash($stream))).Replace('-', '').ToLowerInvariant() }
    finally { $algorithm.Dispose(); $stream.Dispose() }
}
function Get-TextSha256([string]$Text) {
    $algorithm = [System.Security.Cryptography.SHA256]::Create()
    try { ([System.BitConverter]::ToString($algorithm.ComputeHash([System.Text.Encoding]::UTF8.GetBytes($Text)))).Replace('-', '').ToLowerInvariant() }
    finally { $algorithm.Dispose() }
}
function Write-Utf8([string]$Path, [string]$Text) {
    [System.IO.File]::WriteAllText($Path, $Text, (New-Object System.Text.UTF8Encoding($false)))
}
function Get-Inventory([string]$Root) {
    $rootPath = Get-AbsolutePath $Root
    if (!(Test-Path -LiteralPath $rootPath -PathType Container)) { throw "Payload directory is missing: $rootPath" }
    $rootItem = Get-Item -LiteralPath $rootPath -Force
    if (Test-ReparsePoint $rootItem) { throw "Payload root is a reparse point: $rootPath" }
    $inventory = New-Object 'System.Collections.Generic.Dictionary[string,object]' ([System.StringComparer]::OrdinalIgnoreCase)
    $pending = New-Object 'System.Collections.Generic.Stack[string]'; $pending.Push($rootPath)
    while ($pending.Count -gt 0) {
        $directory = $pending.Pop()
        foreach ($entry in Get-ChildItem -LiteralPath $directory -Force) {
            if (Test-ReparsePoint $entry) { throw "Payload contains a reparse point: $($entry.FullName)" }
            if ($entry.PSIsContainer) { $pending.Push($entry.FullName); continue }
            Assert-PathWithinRoot $entry.FullName $rootPath 'Payload file'
            $relative = $entry.FullName.Substring($rootPath.TrimEnd('\', '/').Length).TrimStart('\', '/').Replace('\', '/')
            if ([string]::IsNullOrWhiteSpace($relative) -or $relative.Contains(':') -or
                @($relative.Split('/') | Where-Object { $_ -eq '.' -or $_ -eq '..' }).Count -ne 0) {
                throw "Invalid payload relative path: $relative"
            }
            if ($inventory.ContainsKey($relative)) { throw "Payload has a case-insensitive path collision: $relative" }
            $inventory.Add($relative, [PSCustomObject]@{ Path = $relative; Size = [Int64]$entry.Length; Sha256 = Get-Sha256 $entry.FullName })
        }
    }
    $inventory
}
function Assert-SamePayload([string]$ExpectedRoot, [string]$ActualRoot, [string[]]$AllowedActualExtras = @()) {
    $expected = Get-Inventory $ExpectedRoot
    $actual = Get-Inventory $ActualRoot
    foreach ($extra in $AllowedActualExtras) { [void]$actual.Remove($extra) }
    foreach ($path in $expected.Keys) {
        if (!$actual.ContainsKey($path)) { throw "Package payload is missing: $path" }
        if ($expected[$path].Size -ne $actual[$path].Size -or $expected[$path].Sha256 -ne $actual[$path].Sha256) {
            throw "Package payload hash mismatch: $path"
        }
    }
    foreach ($path in $actual.Keys) { if (!$expected.ContainsKey($path)) { throw "Package payload has an unexpected file: $path" } }
}
function Copy-DirectoryContents([string]$Source, [string]$Destination) {
    New-Item -ItemType Directory -Path $Destination -Force | Out-Null
    foreach ($item in Get-ChildItem -LiteralPath $Source -Force) {
        Copy-Item -LiteralPath $item.FullName -Destination $Destination -Recurse -Force
    }
}
function Update-RuntimeManifest([string]$PayloadRoot) {
    $path = Join-Path $PayloadRoot 'runtime-manifest.json'
    $manifest = Get-Content -LiteralPath $path -Raw -Encoding UTF8 | ConvertFrom-Json
    $inventory = Get-Inventory $PayloadRoot
    $entries = New-Object 'System.Collections.Generic.List[object]'
    foreach ($relative in @($inventory.Keys | Sort-Object)) {
        if ($relative -eq 'runtime-manifest.json') { continue }
        $item = $inventory[$relative]
        $entries.Add([PSCustomObject]@{ path = $item.Path; size = $item.Size; sha256 = $item.Sha256 })
    }
    $manifest.files = $entries.ToArray()
    Write-Utf8 $path (($manifest | ConvertTo-Json -Depth 5) + [Environment]::NewLine)
}
function Assert-RuntimeLayout([string]$Root, [string]$Runtime, [string]$Manifest) {
    & (Join-Path $Root 'scripts\validate_build_layout.ps1') -BuildRoot $BuildRoot -RuntimeDirectory $Runtime -InstallManifest $Manifest
    if (!$?) { throw 'Runtime layout validation failed' }
}
function Get-IdentityValue([string]$IdentityPath, [string]$Name) {
    $text = Get-Content -LiteralPath $IdentityPath -Raw -Encoding UTF8
    $match = [regex]::Match($text, '<\?define\s+' + [regex]::Escape($Name) + '\s*=\s*"([^"]+)"\s*\?>')
    if (!$match.Success) { throw "Identity definition is missing: $Name" }
    $match.Groups[1].Value
}
function Get-InstallerInputDigest([string]$Kind, [string]$CanonicalRoot, [string[]]$Inputs) {
    $lines = New-Object 'System.Collections.Generic.List[string]'
    $lines.Add("kind=$Kind")
    $lines.Add("version=$ProductVersion")
    foreach ($relative in @((Get-Inventory $CanonicalRoot).Keys | Sort-Object)) {
        $entry = (Get-Inventory $CanonicalRoot)[$relative]
        $lines.Add("payload/$relative=$($entry.Size):$($entry.Sha256)")
    }
    foreach ($input in $Inputs) {
        if (!(Test-Path -LiteralPath $input -PathType Leaf)) { throw "Installer input is missing: $input" }
        $label = $input.Replace((Get-AbsolutePath $repoRoot) + '\', '').Replace('\', '/')
        $lines.Add("source/$label=$(Get-Sha256 $input)")
    }
    Get-TextSha256 (($lines | Sort-Object) -join "`n")
}
function Find-SevenZip() {
    foreach ($name in @('7z.exe', '7z')) {
        $command = Get-Command $name -ErrorAction SilentlyContinue
        if ($command) { return $command.Source }
    }
    throw '7-Zip command (7z.exe) was not found on PATH'
}
function Initialize-Staging([string]$Path) {
    Assert-PathWithinRoot $Path $BuildRoot 'Package staging directory'
    $sentinel = Join-Path $Path '.voxtype-package-staging-owner'
    if (Test-Path -LiteralPath $Path) {
        $item = Get-Item -LiteralPath $Path -Force
        if (!$item.PSIsContainer -or (Test-ReparsePoint $item) -or !(Test-Path -LiteralPath $sentinel -PathType Leaf)) {
            throw "Refusing to clear an unowned package staging directory: $Path"
        }
        Remove-Item -LiteralPath $Path -Recurse -Force
    }
    New-Item -ItemType Directory -Path $Path -Force | Out-Null
    Write-Utf8 $sentinel 'VoxType package staging v1'
}
function Get-SigningConfiguration() {
    if (!$RequireSigning) { return $null }
    if ([string]::IsNullOrWhiteSpace($env:VOXTYPE_SIGN_CERT_SHA1) -or [string]::IsNullOrWhiteSpace($env:VOXTYPE_SIGN_TIMESTAMP_URL)) {
        throw '--require-signing requires VOXTYPE_SIGN_CERT_SHA1 and VOXTYPE_SIGN_TIMESTAMP_URL'
    }
    $signTool = $env:VOXTYPE_SIGNTOOL
    if ([string]::IsNullOrWhiteSpace($signTool)) {
        $candidate = Get-ChildItem -Path (Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10\bin') -Filter signtool.exe -Recurse -ErrorAction SilentlyContinue |
            Sort-Object -Property FullName -Descending | Select-Object -First 1
        if ($candidate) { $signTool = $candidate.FullName }
    }
    if ([string]::IsNullOrWhiteSpace($signTool) -or !(Test-Path -LiteralPath $signTool -PathType Leaf)) { throw 'signtool.exe was not found' }
    [PSCustomObject]@{ SignTool = $signTool; Thumbprint = $env:VOXTYPE_SIGN_CERT_SHA1; TimestampUrl = $env:VOXTYPE_SIGN_TIMESTAMP_URL }
}
function Sign-AndVerify([object]$Configuration, [string]$Path) {
    & $Configuration.SignTool sign /fd SHA256 /sha1 $Configuration.Thumbprint /tr $Configuration.TimestampUrl /td SHA256 $Path
    if ($LASTEXITCODE -ne 0) { throw "Authenticode signing failed: $Path" }
    & $Configuration.SignTool verify /pa /v $Path
    if ($LASTEXITCODE -ne 0) { throw "Authenticode verification failed: $Path" }
}
function Get-AssetPaths([string]$Name) {
    [PSCustomObject]@{ Asset = Join-Path $packagesRoot $Name; Hash = Join-Path $packagesRoot "$Name.sha256"; Input = Join-Path $packagesRoot "$Name.input.sha256" }
}
function Test-ExistingArtifact([object]$Paths, [string]$InputDigest) {
    $hasAsset = Test-Path -LiteralPath $Paths.Asset -PathType Leaf
    $hasMetadata = (Test-Path -LiteralPath $Paths.Hash -PathType Leaf) -and (Test-Path -LiteralPath $Paths.Input -PathType Leaf)
    if (!$hasAsset -and !$hasMetadata) { return $false }
    if (!$hasAsset -or !$hasMetadata) { throw "Package artifact metadata is incomplete: $($Paths.Asset)" }
    if ((Get-Content -LiteralPath $Paths.Input -Raw -Encoding ASCII).Trim() -ne $InputDigest) {
        throw "A different artifact already exists for version $($ProductVersion): $($Paths.Asset). Increase APP_VERSION_PATCH before packaging."
    }
    $sidecar = (Get-Content -LiteralPath $Paths.Hash -Raw -Encoding ASCII).Trim().Split(' ')[0].ToLowerInvariant()
    if ($sidecar -ne (Get-Sha256 $Paths.Asset)) { throw "Package SHA-256 sidecar does not match: $($Paths.Asset)" }
    $true
}
function Publish-Artifact([string]$Candidate, [object]$Paths, [string]$InputDigest) {
    if (Test-Path -LiteralPath $Paths.Asset) { throw "Refusing to overwrite package: $($Paths.Asset)" }
    Move-Item -LiteralPath $Candidate -Destination $Paths.Asset
    Write-Utf8 $Paths.Hash ((Get-Sha256 $Paths.Asset) + ' *' + [System.IO.Path]::GetFileName($Paths.Asset) + [Environment]::NewLine)
    Write-Utf8 $Paths.Input ($InputDigest + [Environment]::NewLine)
}
function Verify-PortableArtifact([string]$Archive, [string]$TopLevelName, [string]$SevenZip, [string]$ExtractRoot, [string]$CanonicalRoot) {
    & $SevenZip t $Archive | Out-Host
    if ($LASTEXITCODE -ne 0) { throw "7-Zip test failed: $Archive" }
    New-Item -ItemType Directory -Path $ExtractRoot -Force | Out-Null
    & $SevenZip x $Archive ("-o$ExtractRoot") -y | Out-Host
    if ($LASTEXITCODE -ne 0) { throw "7-Zip extraction failed: $Archive" }
    $payload = Join-Path $ExtractRoot $TopLevelName
    $flag = Join-Path $payload 'portable.flag'
    if (!(Test-Path -LiteralPath $flag -PathType Leaf) -or (Get-Item -LiteralPath $flag).Length -ne 0) { throw 'Portable archive is missing an empty portable.flag' }
    Assert-SamePayload $CanonicalRoot $payload @('portable.flag')
}
function Get-MsiPayloadRoot([string]$MsiPath, [string]$ExtractRoot, [string]$ExpectedPayloadRoot) {
    New-Item -ItemType Directory -Path $ExtractRoot -Force | Out-Null
    $msiexec = Join-Path $env:SystemRoot 'System32\msiexec.exe'
    if (!(Test-Path -LiteralPath $msiexec -PathType Leaf)) { throw 'msiexec.exe is missing' }
    $process = Start-Process -FilePath $msiexec -ArgumentList @(
        '/a', ('"{0}"' -f $MsiPath), '/qn', '/norestart', ('TARGETDIR="{0}"' -f $ExtractRoot)) -Wait -PassThru
    if ($process.ExitCode -ne 0) { throw "msiexec /a failed with exit code $($process.ExitCode)" }
    $deadline = [DateTime]::UtcNow.AddSeconds(30)
    $lastPayloadError = 'VoxType.exe was not found'
    do {
        $executables = @(Get-ChildItem -LiteralPath $ExtractRoot -Recurse -Force -File | Where-Object { $_.Name -eq 'VoxType.exe' })
        if ($executables.Count -eq 1) {
            try {
                Assert-SamePayload $ExpectedPayloadRoot $executables[0].DirectoryName
                return $executables[0].DirectoryName
            }
            catch {
                $lastPayloadError = $_.Exception.Message
            }
        }
        Start-Sleep -Milliseconds 200
    }
    while ([DateTime]::UtcNow -lt $deadline)
    throw "Administrative extraction did not finish with the canonical payload: $lastPayloadError"
}
function Invoke-MsiMethod([object]$Object, [string]$Method, [object[]]$Arguments = @()) {
    # Windows Installer's late-bound COM type library is incomplete on some
    # x64 PowerShell hosts; reflection invocation works on both host variants.
    $Object.GetType().InvokeMember($Method, [System.Reflection.BindingFlags]::InvokeMethod, $null, $Object, $Arguments)
}
function Get-MsiProperty([object]$Object, [string]$Property, [object[]]$Arguments = @()) {
    $Object.GetType().InvokeMember($Property, [System.Reflection.BindingFlags]::GetProperty, $null, $Object, $Arguments)
}
function Get-MsiRows([object]$Database, [string]$Sql, [int]$FieldCount) {
    $view = Invoke-MsiMethod -Object $Database -Method 'OpenView' -Arguments @($Sql)
    [void](Invoke-MsiMethod -Object $view -Method 'Execute')
    try {
        $rows = New-Object 'System.Collections.Generic.List[object]'
        while ($record = Invoke-MsiMethod -Object $view -Method 'Fetch') {
            try {
                $fields = New-Object 'System.Collections.Generic.List[string]'
                for ($i = 1; $i -le $FieldCount; $i++) { $fields.Add([string](Get-MsiProperty -Object $record -Property 'StringData' -Arguments @($i))) }
                $rows.Add([PSCustomObject]@{ Fields = $fields.ToArray() })
            }
            finally { [void][Runtime.InteropServices.Marshal]::ReleaseComObject($record) }
        }
        $rows.ToArray()
    }
    finally { [void][Runtime.InteropServices.Marshal]::ReleaseComObject($view) }
}
function Assert-MsiContract([string]$MsiPath, [string]$ExpectedDigest) {
    $authoring = Get-Content -LiteralPath (Join-Path $packagingRoot 'Package.wxs') -Raw -Encoding UTF8
    foreach ($token in @('Schedule="afterInstallInitialize"', 'AllowSameVersionUpgrades="no"', 'Key="Software\VoxType"', 'Name="InstallFolder"', 'Value="[INSTALLFOLDER]"')) {
        if (!$authoring.Contains($token)) { throw "Package.wxs is missing MSI contract token: $token" }
    }
    $installer = New-Object -ComObject WindowsInstaller.Installer
    $database = $null
    try {
        $database = Invoke-MsiMethod -Object $installer -Method 'OpenDatabase' -Arguments @($MsiPath, [int]0)
        $properties = @{}
        foreach ($row in @(Get-MsiRows $database 'SELECT Property, Value FROM Property' 2)) { $properties[$row.Fields[0]] = $row.Fields[1] }
        if ($properties['ProductVersion'] -ne $ProductVersion -or $properties['ALLUSERS'] -ne '1' -or
            $properties['WIXUI_INSTALLDIR'] -ne 'INSTALLFOLDER' -or $properties['VOXTYPE_INSTALLER_INPUT_SHA256'] -ne $ExpectedDigest) {
            throw 'MSI properties do not satisfy the VoxType installation contract'
        }
        $dialogs = @{}
        foreach ($row in @(Get-MsiRows $database 'SELECT `Dialog` FROM `Dialog`' 1)) { $dialogs[$row.Fields[0]] = $true }
        foreach ($dialog in @('VoxTypeWelcomeDlg', 'InstallDirDlg', 'VerifyReadyDlg')) {
            if (!$dialogs.ContainsKey($dialog)) { throw "MSI UI is missing required dialog: $dialog" }
        }
        $events = @(Get-MsiRows $database 'SELECT `Dialog_`, `Control_`, `Event`, `Argument` FROM `ControlEvent`' 4)
        $advancedOpensPicker = $false
        $pickerChangesTarget = $false
        foreach ($event in $events) {
            if ($event.Fields[0] -eq 'VoxTypeWelcomeDlg' -and $event.Fields[1] -eq 'Advanced' -and
                $event.Fields[2] -eq 'NewDialog' -and $event.Fields[3] -eq 'InstallDirDlg') { $advancedOpensPicker = $true }
            if ($event.Fields[0] -eq 'InstallDirDlg' -and $event.Fields[1] -eq 'ChangeFolder' -and
                $event.Fields[2] -eq 'SpawnDialog' -and $event.Fields[3] -eq 'BrowseDlg') { $pickerChangesTarget = $true }
        }
        if (!$advancedOpensPicker -or !$pickerChangesTarget) { throw 'MSI UI does not expose the custom install-directory picker' }
        $directoryFound = $false
        foreach ($row in @(Get-MsiRows $database 'SELECT Directory, Directory_Parent, DefaultDir FROM Directory' 3)) {
            if ($row.Fields[0] -eq 'INSTALLFOLDER' -and $row.Fields[1] -eq 'ProgramFiles64Folder' -and $row.Fields[2] -eq 'VoxType') { $directoryFound = $true }
        }
        if (!$directoryFound) { throw 'MSI default directory is not ProgramFiles64Folder\VoxType' }
        $appSearch = @(Get-MsiRows $database 'SELECT `Property`, `Signature_` FROM `AppSearch`' 2 | Where-Object { $_.Fields[0] -eq 'INSTALLFOLDER' })
        if ($appSearch.Count -ne 1) { throw 'MSI does not AppSearch the previous INSTALLFOLDER' }
        $locator = @(Get-MsiRows $database 'SELECT `Signature_`, `Root`, `Key`, `Name`, `Type` FROM `RegLocator`' 5 |
            Where-Object { $_.Fields[0] -eq $appSearch[0].Fields[1] })
        if ($locator.Count -ne 1 -or $locator[0].Fields[1] -ne '2' -or
            $locator[0].Fields[2] -ne 'Software\VoxType' -or $locator[0].Fields[3] -ne 'InstallFolder' -or
            $locator[0].Fields[4] -ne '18') {
            throw 'MSI INSTALLFOLDER AppSearch is not the required 64-bit raw HKLM registry locator'
        }
        $folderRegistry = @(Get-MsiRows $database 'SELECT `Root`, `Key`, `Name`, `Value`, `Component_` FROM `Registry`' 5 |
            Where-Object {
                $_.Fields[0] -eq '2' -and $_.Fields[1] -eq 'Software\VoxType' -and
                $_.Fields[2] -eq 'InstallFolder' -and $_.Fields[3] -eq '[INSTALLFOLDER]' -and
                $_.Fields[4] -eq 'cmp_VoxTypeInstallFolderRegistry'
            })
        if ($folderRegistry.Count -ne 1) { throw 'MSI does not persist INSTALLFOLDER in the expected HKLM value' }
        $sequence = @{}
        foreach ($row in @(Get-MsiRows $database 'SELECT Action, Sequence FROM InstallExecuteSequence' 2)) { $sequence[$row.Fields[0]] = [int]$row.Fields[1] }
        if (!$sequence.ContainsKey('AppSearch') -or
            !($sequence['AppSearch'] -lt $sequence['InstallInitialize'] -and
                $sequence['InstallInitialize'] -lt $sequence['RemoveExistingProducts'] -and
                $sequence['RemoveExistingProducts'] -lt $sequence['InstallFiles'])) {
            throw 'MSI AppSearch/MajorUpgrade sequence does not restore INSTALLFOLDER before old-product removal'
        }
        $identityUpgradeCode = (Get-IdentityValue (Join-Path $packagingRoot 'ProductIdentity.wxi') 'VoxTypeUpgradeCode')
        $upgradeRows = @(Get-MsiRows $database 'SELECT `UpgradeCode`, `VersionMin`, `VersionMax`, `Attributes`, `ActionProperty` FROM `Upgrade`' 5)
        $sameVersionRejected = $false
        foreach ($row in $upgradeRows) {
            try {
                $sameCode = ([guid]$row.Fields[0]).ToString() -eq ([guid]$identityUpgradeCode).ToString()
            }
            catch { $sameCode = $false }
            if ($sameCode -and $row.Fields[2] -eq $ProductVersion -and (([int]$row.Fields[3] -band 0x200) -eq 0)) {
                $sameVersionRejected = $true
            }
        }
        if (!$sameVersionRejected) { throw 'MSI Upgrade table does not reject a same-version install for the fixed UpgradeCode' }
    }
    finally {
        if ($database) { [void][Runtime.InteropServices.Marshal]::ReleaseComObject($database) }
        if ($installer) { [void][Runtime.InteropServices.Marshal]::ReleaseComObject($installer) }
    }
}
function Build-Msi([string]$CanonicalRoot, [string]$OutputName, [string]$InputDigest, [string]$GeneratedRoot, [string]$MsiRoot, [string]$IntermediateRoot) {
    $identityPath = Join-Path $packagingRoot 'ProductIdentity.wxi'
    $productGenerator = Join-Path $repoRoot 'scripts\generate_wix_product_instance.ps1'
    $runtimeGenerator = Join-Path $repoRoot 'scripts\generate_wix_runtime_fragment.ps1'
    $productInstance = Join-Path $GeneratedRoot 'ProductInstance.generated.wxi'
    $runtimeFragment = Join-Path $GeneratedRoot 'RuntimeFiles.generated.wxs'
    & $productGenerator $identityPath $ProductVersion $productInstance | Out-Null
    & $runtimeGenerator $CanonicalRoot (Get-IdentityValue $identityPath 'VoxTypeComponentNamespace') $runtimeFragment -VerifySyntheticIdentity | Out-Null
    $project = Join-Path $packagingRoot 'VoxType.Installer.wixproj'
    $common = @(
        "-p:GeneratedWixDirectory=$GeneratedRoot",
        "-p:CanonicalPayloadRoot=$CanonicalRoot",
        "-p:VoxTypeInstallerInputSha256=$InputDigest",
        "-p:VoxTypeMsiOutputDirectory=$MsiRoot\",
        "-p:VoxTypeMsiOutputName=$OutputName",
        "-p:VoxTypeWixIntermediateDirectory=$IntermediateRoot\",
        "-p:MSBuildProjectExtensionsPath=$IntermediateRoot\obj\")
    & dotnet restore $project @common | Out-Host
    if ($LASTEXITCODE -ne 0) { throw 'NuGet restore for WixToolset.Sdk failed' }
    & dotnet build $project --no-restore @common | Out-Host
    if ($LASTEXITCODE -ne 0) { throw 'WiX MSI build failed' }
    $msi = Join-Path $MsiRoot "$OutputName.msi"
    if (!(Test-Path -LiteralPath $msi -PathType Leaf)) { throw "WiX did not produce: $msi" }
    $msi
}

$repoRoot = Get-AbsolutePath (Join-Path $PSScriptRoot '..')
$packagingRoot = Join-Path $repoRoot 'packaging\windows'
$buildRootPath = Get-AbsolutePath $BuildRoot
$runtimeRoot = Get-AbsolutePath $RuntimeDirectory
$installManifestPath = Get-AbsolutePath $InstallManifest
if (!(Test-Path -LiteralPath $buildRootPath -PathType Container)) { throw "Build root is missing: $buildRootPath" }
Assert-PathWithinRoot $runtimeRoot $buildRootPath 'Runtime directory'
Assert-PathWithinRoot $installManifestPath $buildRootPath 'CMake install manifest'
if (!(Test-Path -LiteralPath $installManifestPath -PathType Leaf)) { throw "CMake install manifest is missing: $installManifestPath" }
$installManifestBefore = [System.IO.File]::ReadAllBytes($installManifestPath)

$producePortable = $Mode -eq 'All' -or $Mode -eq 'Portable'
$produceMsi = $Mode -eq 'All' -or $Mode -eq 'Msi'
$signing = Get-SigningConfiguration
$packagesRoot = Join-Path $buildRootPath 'packages'; New-Item -ItemType Directory -Path $packagesRoot -Force | Out-Null
$stagingRoot = Join-Path $buildRootPath 'cmake\x64-release\package-staging'
Initialize-Staging $stagingRoot
$canonicalRoot = Join-Path $stagingRoot 'canonical'
Copy-DirectoryContents $runtimeRoot $canonicalRoot
if ($signing) {
    Sign-AndVerify $signing (Join-Path $canonicalRoot 'VoxType.exe')
    Update-RuntimeManifest $canonicalRoot
}
Assert-RuntimeLayout $repoRoot $canonicalRoot $installManifestPath

$variantSuffix = if ($signing) { '' } else { '-unsigned' }
$baseName = "VoxType-v$ProductVersion-win-x64"
if ($producePortable) {
    $sevenZip = Find-SevenZip
    $topLevelName = "$baseName-portable$variantSuffix"
    $paths = Get-AssetPaths "$topLevelName.7z"
    $digest = Get-InstallerInputDigest 'portable' $canonicalRoot @()
    if (Test-ExistingArtifact $paths $digest) {
        Verify-PortableArtifact $paths.Asset $topLevelName $sevenZip (Join-Path $stagingRoot 'verify-portable-existing') $canonicalRoot
        Write-Output "Reused verified Portable package: $($paths.Asset)"
    }
    else {
        $portableRoot = Join-Path (Join-Path $stagingRoot 'portable') $topLevelName
        Copy-DirectoryContents $canonicalRoot $portableRoot
        New-Item -ItemType File -Path (Join-Path $portableRoot 'portable.flag') -Force | Out-Null
        $candidate = Join-Path $stagingRoot "$topLevelName.7z"
        Push-Location (Split-Path -Parent $portableRoot)
        try { & $sevenZip a -t7z -mx=9 $candidate $topLevelName | Out-Host } finally { Pop-Location }
        if ($LASTEXITCODE -ne 0) { throw '7-Zip Portable creation failed' }
        Verify-PortableArtifact $candidate $topLevelName $sevenZip (Join-Path $stagingRoot 'verify-portable') $canonicalRoot
        Publish-Artifact $candidate $paths $digest
        Write-Output "Created verified Portable package: $($paths.Asset)"
    }
}

if ($produceMsi) {
    $outputName = "$baseName$variantSuffix"
    $generatedRoot = Join-Path $stagingRoot 'generated'; $msiRoot = Join-Path $stagingRoot 'msi'; $intermediateRoot = Join-Path $stagingRoot 'wix-intermediate'
    New-Item -ItemType Directory -Path $generatedRoot, $msiRoot, $intermediateRoot -Force | Out-Null
    # Generate first: these deterministic sources are part of the same-version input digest.
    $identityPath = Join-Path $packagingRoot 'ProductIdentity.wxi'
    & (Join-Path $repoRoot 'scripts\generate_wix_product_instance.ps1') $identityPath $ProductVersion (Join-Path $generatedRoot 'ProductInstance.generated.wxi')
    & (Join-Path $repoRoot 'scripts\generate_wix_runtime_fragment.ps1') $canonicalRoot (Get-IdentityValue $identityPath 'VoxTypeComponentNamespace') (Join-Path $generatedRoot 'RuntimeFiles.generated.wxs') -VerifySyntheticIdentity
    $digestInputs = @(
        (Join-Path $generatedRoot 'ProductInstance.generated.wxi'), (Join-Path $generatedRoot 'RuntimeFiles.generated.wxs'),
        (Join-Path $packagingRoot 'Package.wxs'), (Join-Path $packagingRoot 'InstallerUi.wxs'), (Join-Path $packagingRoot 'ProductIdentity.wxi'),
        (Join-Path $packagingRoot 'VoxType.Installer.wixproj'), (Join-Path $repoRoot 'scripts\generate_wix_product_instance.ps1'),
        (Join-Path $repoRoot 'scripts\generate_wix_runtime_fragment.ps1'))
    $digest = Get-InstallerInputDigest 'msi' $canonicalRoot $digestInputs
    $paths = Get-AssetPaths "$outputName.msi"
    if (Test-ExistingArtifact $paths $digest) {
        Assert-MsiContract $paths.Asset $digest
        $existingRoot = Get-MsiPayloadRoot $paths.Asset (Join-Path $stagingRoot 'msi-existing-admin') $canonicalRoot
        Assert-SamePayload $canonicalRoot $existingRoot
        Write-Output "Reused verified MSI package: $($paths.Asset)"
    }
    else {
        # Build-Msi regenerates the sources only to make the invocation independently safe.
        $candidate = Build-Msi $canonicalRoot $outputName $digest $generatedRoot $msiRoot $intermediateRoot
        Assert-MsiContract $candidate $digest
        $adminRoot = Get-MsiPayloadRoot $candidate (Join-Path $stagingRoot 'msi-admin') $canonicalRoot
        Assert-SamePayload $canonicalRoot $adminRoot
        if ($signing) { Sign-AndVerify $signing $candidate }
        Publish-Artifact $candidate $paths $digest
        Write-Output "Created verified MSI package: $($paths.Asset)"
    }
}

$installManifestAfter = [System.IO.File]::ReadAllBytes($installManifestPath)
if ($installManifestBefore.Length -ne $installManifestAfter.Length) { throw 'Packaging modified the CMake install manifest' }
for ($i = 0; $i -lt $installManifestBefore.Length; $i++) {
    if ($installManifestBefore[$i] -ne $installManifestAfter[$i]) { throw 'Packaging modified the CMake install manifest' }
}
