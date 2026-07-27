[CmdletBinding()]
param(
    [Parameter(Mandatory)] [string]$PreviousMsi,
    [Parameter(Mandatory)] [string]$CurrentMsi,
    [Parameter(Mandatory)] [string]$InstallFolder,
    [switch]$AllowMachineMutation
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

if (!$AllowMachineMutation) {
    throw 'This lifecycle harness installs and uninstalls MSI packages. Run only in an isolated VM with -AllowMachineMutation.'
}

function Get-AbsolutePath([string]$Path) { [System.IO.Path]::GetFullPath($Path) }
function Invoke-Msi([string[]]$Arguments, [string]$LogPath) {
    $msiexec = Join-Path $env:SystemRoot 'System32\msiexec.exe'
    $process = Start-Process -FilePath $msiexec -ArgumentList ($Arguments + @('/norestart', '/l*v', ('"{0}"' -f $LogPath))) -Wait -PassThru
    if ($process.ExitCode -ne 0) { throw "msiexec failed with exit code $($process.ExitCode); see $LogPath" }
}

$previous = Get-AbsolutePath $PreviousMsi
$current = Get-AbsolutePath $CurrentMsi
$target = Get-AbsolutePath $InstallFolder
if (!(Test-Path -LiteralPath $previous -PathType Leaf)) { throw "Previous MSI is missing: $previous" }
if (!(Test-Path -LiteralPath $current -PathType Leaf)) { throw "Current MSI is missing: $current" }
if (Test-Path -LiteralPath $target) { throw "Choose a new empty VM-only test folder: $target" }

$reportRoot = Join-Path ([System.IO.Path]::GetTempPath()) ('VoxType-msi-lifecycle-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $reportRoot -Force | Out-Null
$markerRoot = Join-Path $env:LOCALAPPDATA 'VoxType'
$marker = Join-Path $markerRoot 'msi-lifecycle-preservation-marker.txt'
if (Test-Path -LiteralPath $marker) { throw "Existing marker found; use a clean test profile: $marker" }

try {
    Invoke-Msi @('/i', ('"{0}"' -f $previous), ('INSTALLFOLDER="{0}"' -f $target), '/qn') (Join-Path $reportRoot 'install-previous.log')
    if (!(Test-Path -LiteralPath (Join-Path $target 'VoxType.exe') -PathType Leaf)) { throw 'Previous MSI did not install VoxType.exe into the chosen directory' }
    $recordedFolder = Get-ItemPropertyValue -Path 'HKLM:\Software\VoxType' -Name InstallFolder -ErrorAction Stop
    if (![string]::Equals($recordedFolder.TrimEnd('\'), $target.TrimEnd('\'), [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "Previous MSI did not persist the chosen install folder: $recordedFolder"
    }

    New-Item -ItemType Directory -Path $markerRoot -Force | Out-Null
    [System.IO.File]::WriteAllText($marker, 'must survive MSI upgrade and uninstall')
    Invoke-Msi @('/i', ('"{0}"' -f $current), '/qn') (Join-Path $reportRoot 'upgrade-current.log')
    if (!(Test-Path -LiteralPath (Join-Path $target 'VoxType.exe') -PathType Leaf)) { throw 'Current MSI did not preserve the selected install directory' }
    $upgradedFolder = Get-ItemPropertyValue -Path 'HKLM:\Software\VoxType' -Name InstallFolder -ErrorAction Stop
    if (![string]::Equals($upgradedFolder.TrimEnd('\'), $target.TrimEnd('\'), [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "Major Upgrade changed the selected install folder: $upgradedFolder"
    }
    if (!(Test-Path -LiteralPath $marker -PathType Leaf)) { throw '%LOCALAPPDATA%\VoxType data was lost during Major Upgrade' }

    Invoke-Msi @('/x', ('"{0}"' -f $current), '/qn') (Join-Path $reportRoot 'uninstall-current.log')
    if (Test-Path -LiteralPath (Join-Path $target 'VoxType.exe')) { throw 'MSI uninstall left a product-owned executable behind' }
    if (!(Test-Path -LiteralPath $marker -PathType Leaf)) { throw '%LOCALAPPDATA%\VoxType data was removed by uninstall' }
    Write-Output "MSI lifecycle verified. Logs: $reportRoot"
}
finally {
    # The marker is the only user-data file this harness creates. It is removed
    # only after the preservation assertion succeeds/fails; the rest of the
    # LocalAppData directory is intentionally never touched.
    Remove-Item -LiteralPath $marker -Force -ErrorAction SilentlyContinue
}
