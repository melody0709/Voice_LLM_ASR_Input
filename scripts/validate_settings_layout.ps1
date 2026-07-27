[CmdletBinding()]
param(
    [Parameter(Mandatory)] [string]$SourceRoot
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$globalsPath = Join-Path $SourceRoot 'src\app\globals.h'
$settingsPath = Join-Path $SourceRoot 'src\ui\settings.cpp'
foreach ($path in @($globalsPath, $settingsPath)) {
    if (!(Test-Path -LiteralPath $path -PathType Leaf)) { throw "Settings layout source is missing: $path" }
}
$globals = Get-Content -LiteralPath $globalsPath -Raw -Encoding UTF8
$settings = Get-Content -LiteralPath $settingsPath -Raw -Encoding UTF8

function Get-UiInt([string]$Name) {
    $match = [regex]::Match($globals, 'constexpr\s+int\s+' + [regex]::Escape($Name) + '\s*=\s*([0-9]+)\s*;')
    if (!$match.Success) { throw "UiStyle::$Name must remain a numeric layout constant" }
    [int]$match.Groups[1].Value
}
function Scale([int]$DesignPx, [double]$Scale) { [int][Math]::Ceiling($DesignPx * $Scale) }

$firstRowY = Get-UiInt 'FirstRowY'
$shortcutHeight = Get-UiInt 'GeneralShortcutGroupH'
$startupGap = Get-UiInt 'GeneralStartupGroupGap'
$startupHeight = Get-UiInt 'GeneralStartupGroupH'
$groupX = Get-UiInt 'GeneralGroupX'
$groupWidth = Get-UiInt 'GeneralGroupW'
$contentLeft = Get-UiInt 'ContentLeft'
$hintWidth = Get-UiInt 'GeneralStartupHintW'
$footerMinTop = Get-UiInt 'FooterMinTop'
$margin = Get-UiInt 'Margin'

$startupY = $firstRowY + $shortcutHeight + $startupGap
$startupBottom = $startupY + $startupHeight
if ($startupBottom -gt ($footerMinTop - 4)) {
    throw "General startup group reaches the footer: bottom=$startupBottom, contentBottom=$($footerMinTop - 4)"
}
if (($groupX + $groupWidth) -gt (850 - $margin)) {
    throw 'General group exceeds the Settings design width'
}
if (($contentLeft + $hintWidth) -gt ($groupX + $groupWidth)) {
    throw 'Startup explanatory text exceeds the General group width'
}

foreach ($dpi in @(96, 144, 192, 288)) {
    $scale = $dpi / 144.0
    if ((Scale $startupBottom $scale) -gt (Scale ($footerMinTop - 4) $scale)) {
        throw "Startup group overlaps the footer at $dpi DPI"
    }
    if ((Scale ($groupX + $groupWidth) $scale) -gt (Scale (850 - $margin) $scale)) {
        throw "General group overflows the Settings width at $dpi DPI"
    }
}

foreach ($required in @(
        'IDC_START_WITH_WINDOWS',
        'AddGeneralControl(startupCheckbox)',
        'RefreshStartupRegistrationControl(g_settingsWindow, true)',
        'if (!SaveStartupRegistrationControl(hwnd)) return;')) {
    if (!$globals.Contains($required) -and !$settings.Contains($required)) {
        throw "Startup Settings wiring is missing: $required"
    }
}

Write-Output 'Validated Settings startup layout at 96/144/192/288 DPI design scales.'
