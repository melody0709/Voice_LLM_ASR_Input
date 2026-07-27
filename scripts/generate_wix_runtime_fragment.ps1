[CmdletBinding()]
param(
    [Parameter(Mandatory, Position = 0)] [string]$CanonicalPayloadRoot,
    [Parameter(Mandatory, Position = 1)] [string]$ComponentNamespace,
    [Parameter(Mandatory, Position = 2)] [string]$GeneratedOutputPath,
    [switch]$VerifySyntheticIdentity
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Get-AbsolutePath([string]$Path) { [System.IO.Path]::GetFullPath($Path) }
function Test-PathWithinRoot([string]$Path, [string]$Root) {
    $root = $Root.TrimEnd([IO.Path]::DirectorySeparatorChar, [IO.Path]::AltDirectorySeparatorChar)
    return [string]::Equals($Path, $root, [StringComparison]::OrdinalIgnoreCase) -or
        $Path.StartsWith($root + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)
}
function Test-ReparsePoint([IO.FileSystemInfo]$Item) {
    ($Item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0
}
function Get-Sha256([string]$Path) {
    $stream = [IO.File]::OpenRead($Path); $algorithm = [Security.Cryptography.SHA256]::Create()
    try { ([BitConverter]::ToString($algorithm.ComputeHash($stream))).Replace('-', '').ToLowerInvariant() }
    finally { $algorithm.Dispose(); $stream.Dispose() }
}
function Get-TextSha256([string]$Text) {
    $algorithm = [Security.Cryptography.SHA256]::Create()
    try { ([BitConverter]::ToString($algorithm.ComputeHash([Text.Encoding]::UTF8.GetBytes($Text)))).Replace('-', '').ToLowerInvariant() }
    finally { $algorithm.Dispose() }
}
function Convert-GuidToRfc4122Bytes([guid]$Guid) {
    $native = $Guid.ToByteArray()
    [byte[]]@($native[3], $native[2], $native[1], $native[0], $native[5], $native[4], $native[7], $native[6],
        $native[8], $native[9], $native[10], $native[11], $native[12], $native[13], $native[14], $native[15])
}
function Convert-Rfc4122BytesToGuid([byte[]]$Bytes) {
    [guid]::new([byte[]]@($Bytes[3], $Bytes[2], $Bytes[1], $Bytes[0], $Bytes[5], $Bytes[4], $Bytes[7], $Bytes[6],
        $Bytes[8], $Bytes[9], $Bytes[10], $Bytes[11], $Bytes[12], $Bytes[13], $Bytes[14], $Bytes[15]))
}
function Get-UuidV5([guid]$Namespace, [string]$Name) {
    $namespaceBytes = Convert-GuidToRfc4122Bytes $Namespace
    $nameBytes = [Text.Encoding]::UTF8.GetBytes($Name)
    $input = New-Object byte[] ($namespaceBytes.Length + $nameBytes.Length)
    [Array]::Copy($namespaceBytes, 0, $input, 0, $namespaceBytes.Length)
    [Array]::Copy($nameBytes, 0, $input, $namespaceBytes.Length, $nameBytes.Length)
    $sha1 = [Security.Cryptography.SHA1]::Create()
    try { $hash = $sha1.ComputeHash($input) } finally { $sha1.Dispose() }
    $uuid = [byte[]]$hash[0..15]
    $uuid[6] = [byte](($uuid[6] -band 0x0f) -bor 0x50)
    $uuid[8] = [byte](($uuid[8] -band 0x3f) -bor 0x80)
    Convert-Rfc4122BytesToGuid $uuid
}
function ConvertTo-Xml([string]$Value) { [Security.SecurityElement]::Escape($Value) }
function Assert-RelativePayloadPath([string]$Path) {
    if ([string]::IsNullOrWhiteSpace($Path) -or $Path.StartsWith('/') -or $Path.Contains(':') -or $Path.Contains([char]0)) {
        throw "Invalid canonical payload path: $Path"
    }
    foreach ($segment in $Path.Split('/')) {
        if ([string]::IsNullOrEmpty($segment) -or $segment -eq '.' -or $segment -eq '..') { throw "Invalid canonical payload path: $Path" }
        foreach ($char in $segment.ToCharArray()) { if ([int][char]$char -lt 32) { throw "Invalid canonical payload path: $Path" } }
    }
}

function Get-PayloadInventory([string]$Root) {
    $rootPath = Get-AbsolutePath $Root
    if (!(Test-Path -LiteralPath $rootPath -PathType Container)) { throw "Canonical payload is missing: $rootPath" }
    $rootInfo = Get-Item -LiteralPath $rootPath -Force
    if (Test-ReparsePoint $rootInfo) { throw "Canonical payload root is a reparse point: $rootPath" }
    $items = New-Object 'System.Collections.Generic.List[object]'
    $known = New-Object 'System.Collections.Generic.HashSet[string]' ([StringComparer]::OrdinalIgnoreCase)
    $pending = New-Object 'System.Collections.Generic.Stack[string]'; $pending.Push($rootPath)
    while ($pending.Count -gt 0) {
        $directory = $pending.Pop()
        foreach ($entry in Get-ChildItem -LiteralPath $directory -Force) {
            if (Test-ReparsePoint $entry) { throw "Canonical payload reparse point is forbidden: $($entry.FullName)" }
            if ($entry.PSIsContainer) { $pending.Push($entry.FullName); continue }
            if (!(Test-PathWithinRoot $entry.FullName $rootPath)) { throw "Canonical payload file escapes root: $($entry.FullName)" }
            $relative = $entry.FullName.Substring($rootPath.Length).TrimStart('\', '/').Replace('\', '/')
            Assert-RelativePayloadPath $relative
            if (!$known.Add($relative)) { throw "Canonical payload path collision: $relative" }
            $digest = Get-TextSha256 $relative
            $items.Add([PSCustomObject]@{
                Path = $relative
                FileId = 'fil_' + $digest.Substring(0, 32)
                ComponentId = 'cmp_' + $digest.Substring(0, 32)
            })
        }
    }
    @($items | Sort-Object -Property Path)
}

function New-DirectoryNode([string]$Name, [string]$Path) {
    [PSCustomObject]@{
        Name = $Name; Path = $Path
        Directories = New-Object 'System.Collections.Generic.Dictionary[string,object]' ([StringComparer]::Ordinal)
        Files = New-Object 'System.Collections.Generic.List[object]'
    }
}
function Get-DirectoryId([string]$Path) { 'dir_' + (Get-TextSha256 ('directory|' + $Path)).Substring(0, 32) }
function Write-DirectoryContents([object]$Node, [int]$Indent, [Text.StringBuilder]$Builder, [guid]$Namespace) {
    $pad = ' ' * $Indent
    foreach ($file in @($Node.Files | Sort-Object -Property Path)) {
        $guid = Get-UuidV5 $Namespace $file.Path
        $source = '$' + '(var.CanonicalPayloadRoot)' + '\' + $file.Path.Replace('/', '\')
        $name = Split-Path -Path $file.Path -Leaf
        [void]$Builder.AppendLine(('{0}<Component Id="{1}" Guid="{{{2}}}" Bitness="always64">' -f $pad, $file.ComponentId, $guid.ToString().ToUpperInvariant()))
        [void]$Builder.AppendLine(('{0}  <File Id="{1}" Name="{2}" Source="{3}" KeyPath="yes" />' -f $pad, $file.FileId, (ConvertTo-Xml $name), (ConvertTo-Xml $source)))
        [void]$Builder.AppendLine("$pad</Component>")
    }
    foreach ($directoryName in @($Node.Directories.Keys | Sort-Object)) {
        $directory = $Node.Directories[$directoryName]
        [void]$Builder.AppendLine(('{0}<Directory Id="{1}" Name="{2}">' -f $pad, (Get-DirectoryId $directory.Path), (ConvertTo-Xml $directory.Name)))
        Write-DirectoryContents $directory ($Indent + 2) $Builder $Namespace
        [void]$Builder.AppendLine("$pad</Directory>")
    }
}
function Assert-SyntheticIdentity([guid]$Namespace) {
    $stable = (Get-UuidV5 $Namespace 'stable/kept.bin').ToString()
    if ($stable -ne (Get-UuidV5 $Namespace 'stable/kept.bin').ToString()) { throw 'Stable component identity changed' }
    $old = (Get-UuidV5 $Namespace 'moved/from.bin').ToString()
    $new = (Get-UuidV5 $Namespace 'moved/to.bin').ToString()
    if ($old -eq $new) { throw 'Moved files must get a new component identity' }
}

$root = Get-AbsolutePath $CanonicalPayloadRoot
$output = Get-AbsolutePath $GeneratedOutputPath
try { $namespaceGuid = [guid]$ComponentNamespace } catch { throw "Component namespace is not a GUID: $ComponentNamespace" }
if ($VerifySyntheticIdentity) { Assert-SyntheticIdentity $namespaceGuid }
$inventory = @(Get-PayloadInventory $root)
if ($inventory.Count -eq 0) { throw "Canonical payload is empty: $root" }

$tree = New-DirectoryNode '' ''
foreach ($file in $inventory) {
    $node = $tree; $current = ''
    $parts = $file.Path.Split('/')
    for ($i = 0; $i -lt $parts.Length - 1; $i++) {
        $segment = $parts[$i]
        $current = if ($current -eq '') { $segment } else { "$current/$segment" }
        if (!$node.Directories.ContainsKey($segment)) { $node.Directories.Add($segment, (New-DirectoryNode $segment $current)) }
        $node = $node.Directories[$segment]
    }
    $node.Files.Add($file)
}

$builder = New-Object Text.StringBuilder
[void]$builder.AppendLine('<?xml version="1.0" encoding="utf-8"?>')
[void]$builder.AppendLine('<!-- Generated by scripts/generate_wix_runtime_fragment.ps1. Do not hand-edit. -->')
[void]$builder.AppendLine('<Wix xmlns="http://wixtoolset.org/schemas/v4/wxs">')
[void]$builder.AppendLine('  <Fragment>')
[void]$builder.AppendLine('    <DirectoryRef Id="INSTALLFOLDER">')
Write-DirectoryContents $tree 6 $builder $namespaceGuid
[void]$builder.AppendLine('    </DirectoryRef>')
[void]$builder.AppendLine('    <ComponentGroup Id="VoxTypeRuntimeFiles">')
foreach ($file in $inventory) { [void]$builder.AppendLine(('      <ComponentRef Id="{0}" />' -f $file.ComponentId)) }
[void]$builder.AppendLine('    </ComponentGroup>')
[void]$builder.AppendLine('  </Fragment>')
[void]$builder.AppendLine('</Wix>')

New-Item -ItemType Directory -Path ([IO.Path]::GetDirectoryName($output)) -Force | Out-Null
$utf8 = New-Object Text.UTF8Encoding($false)
$temporary = "$output.tmp"; [IO.File]::WriteAllText($temporary, $builder.ToString(), $utf8)
Move-Item -LiteralPath $temporary -Destination $output -Force
Write-Output ("Generated WiX runtime fragment: files={0}, output={1}" -f $inventory.Count, $output)
