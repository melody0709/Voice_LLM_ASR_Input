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
$settingsWindowWidth = Get-UiInt 'SettingsWindowW'
$settingsWindowHeight = Get-UiInt 'SettingsWindowH'
$settingsNonClientReserveHeight = Get-UiInt 'SettingsWindowNonClientReserveH'
$shortcutHeight = Get-UiInt 'GeneralShortcutGroupH'
$startupGap = Get-UiInt 'GeneralStartupGroupGap'
$startupHeight = Get-UiInt 'GeneralStartupGroupH'
$diagnosticsGap = Get-UiInt 'GeneralDiagnosticsGroupGap'
$diagnosticsHeight = Get-UiInt 'GeneralDiagnosticsGroupH'
$diagnosticsModeWidth = Get-UiInt 'GeneralDiagnosticsModeW'
$diagnosticsOpenWidth = Get-UiInt 'GeneralDiagnosticsOpenButtonW'
$diagnosticsDeleteWidth = Get-UiInt 'GeneralDiagnosticsDeleteButtonW'
$diagnosticsButtonGap = Get-UiInt 'GeneralDiagnosticsButtonGap'
$diagnosticsHintWidth = Get-UiInt 'GeneralDiagnosticsHintW'
$groupX = Get-UiInt 'GeneralGroupX'
$groupWidth = Get-UiInt 'GeneralGroupW'
$contentLeft = Get-UiInt 'ContentLeft'
$inputLeft = Get-UiInt 'InputLeft'
$hintWidth = Get-UiInt 'GeneralStartupHintW'
$footerMinTop = Get-UiInt 'FooterMinTop'
$footerHeight = Get-UiInt 'FooterHeight'
$margin = Get-UiInt 'Margin'
$qwenAdvancedHintY = Get-UiInt 'QwenAdvancedHintY'
$qwenHintH = Get-UiInt 'QwenHintH'
$qwenDialogW = Get-UiInt 'QwenAdvancedDialogW'
$qwenDialogH = Get-UiInt 'QwenAdvancedDialogH'
$qwenDialogNonClientReserveH = Get-UiInt 'QwenAdvancedDialogNonClientReserveH'
$qwenDialogInputLeft = Get-UiInt 'QwenAdvancedDialogInputLeft'
$qwenDialogInputW = Get-UiInt 'QwenAdvancedDialogInputW'
$qwenDialogSpecialY = Get-UiInt 'QwenAdvancedDialogSpecialY'
$qwenDialogSpecialH = Get-UiInt 'QwenAdvancedDialogSpecialH'
$qwenDialogFooterY = Get-UiInt 'QwenAdvancedDialogFooterY'
$actionButtonH = Get-UiInt 'ActionBtnH'

$startupY = $firstRowY + $shortcutHeight + $startupGap
$startupBottom = $startupY + $startupHeight
$diagnosticsY = $startupBottom + $diagnosticsGap
$diagnosticsBottom = $diagnosticsY + $diagnosticsHeight
if ($diagnosticsBottom -gt ($footerMinTop - 4)) {
    throw "General diagnostics group reaches the footer: bottom=$diagnosticsBottom, contentBottom=$($footerMinTop - 4)"
}
if (($groupX + $groupWidth) -gt ($settingsWindowWidth - $margin)) {
    throw 'General group exceeds the Settings design width'
}
if (($contentLeft + $hintWidth) -gt ($groupX + $groupWidth)) {
    throw 'Startup explanatory text exceeds the General group width'
}
if (($inputLeft + $diagnosticsModeWidth) -gt ($groupX + $groupWidth)) {
    throw 'Diagnostics mode selector exceeds the General group width'
}
if (($contentLeft + $diagnosticsOpenWidth + $diagnosticsButtonGap + $diagnosticsDeleteWidth) -gt ($groupX + $groupWidth)) {
    throw 'Diagnostics action buttons exceed the General group width'
}
if ($diagnosticsOpenWidth -lt 220 -or $diagnosticsDeleteWidth -lt 240) {
    throw 'Diagnostics action buttons do not leave enough room for their English labels'
}
if (($contentLeft + $diagnosticsHintWidth) -gt ($groupX + $groupWidth)) {
    throw 'Diagnostics privacy hint exceeds the General group width'
}
if (($qwenAdvancedHintY + (2 * $qwenHintH)) -gt ($footerMinTop - 4)) {
    throw 'Qwen basic settings reach the footer'
}
if (($qwenDialogInputLeft + $qwenDialogInputW) -gt ($qwenDialogW - $margin)) {
    throw 'Qwen Advanced input fields exceed the dialog width'
}
$qwenDialogClientBottom = $qwenDialogH - $qwenDialogNonClientReserveH
$settingsClientBottom = $settingsWindowHeight - $settingsNonClientReserveHeight
$minimumFooterRoom = $actionButtonH + (2 * $margin)
if (($settingsClientBottom - $footerMinTop) -lt $minimumFooterRoom) {
    throw "Settings window does not leave enough client height for footer buttons: available=$($settingsClientBottom - $footerMinTop), required=$minimumFooterRoom"
}
if ($footerHeight -lt $minimumFooterRoom) {
    throw 'Settings footer height does not leave vertical padding around its buttons'
}
if (($qwenDialogSpecialY + $qwenDialogSpecialH) -gt ($qwenDialogFooterY - 8)) {
    throw 'Qwen Advanced sensitive-word fields overlap the footer buttons'
}
if (($qwenDialogFooterY + $actionButtonH) -gt ($qwenDialogClientBottom - $margin)) {
    throw 'Qwen Advanced footer buttons exceed the estimated client area'
}

foreach ($dpi in @(96, 144, 192, 288)) {
    $scale = $dpi / 144.0
    if ((Scale $diagnosticsBottom $scale) -gt (Scale ($footerMinTop - 4) $scale)) {
        throw "Diagnostics group overlaps the footer at $dpi DPI"
    }
    if ((Scale ($groupX + $groupWidth) $scale) -gt (Scale ($settingsWindowWidth - $margin) $scale)) {
        throw "General group overflows the Settings width at $dpi DPI"
    }
    if ((Scale ($qwenAdvancedHintY + (2 * $qwenHintH)) $scale) -gt (Scale ($footerMinTop - 4) $scale)) {
        throw "Qwen basic settings overlap the footer at $dpi DPI"
    }
    if ((Scale ($qwenDialogInputLeft + $qwenDialogInputW) $scale) -gt (Scale ($qwenDialogW - $margin) $scale)) {
        throw "Qwen Advanced inputs overflow the dialog at $dpi DPI"
    }
    if ((Scale ($qwenDialogSpecialY + $qwenDialogSpecialH) $scale) -gt (Scale ($qwenDialogFooterY - 8) $scale)) {
        throw "Qwen Advanced sensitive-word fields overlap the footer at $dpi DPI"
    }
    if ((Scale ($qwenDialogFooterY + $actionButtonH) $scale) -gt (Scale ($qwenDialogClientBottom - $margin) $scale)) {
        throw "Qwen Advanced footer overflows the estimated client area at $dpi DPI"
    }
}

foreach ($required in @(
        'IDC_START_WITH_WINDOWS',
        'AddGeneralControl(startupCheckbox)',
        'RefreshStartupRegistrationControl(g_settingsWindow, true)',
        'if (!SaveStartupRegistrationControl(hwnd)) return;',
        'IDC_DIAGNOSTIC_AUDIO_MODE',
        'AddGeneralControl(diagnosticsGroup)',
        'g_config.diagnosticAudioMode = DiagnosticAudioModeFromControl(hwnd);',
        'OpenDiagnosticAudioFolder(hwnd)',
        'DeleteDiagnosticAudioFiles(hwnd)',
        'IDC_QWEN_ADVANCED',
        'ShowQwenAdvancedDialog(hwnd, data)')) {
    if (!$globals.Contains($required) -and !$settings.Contains($required)) {
        throw "Startup Settings wiring is missing: $required"
    }
}

Write-Output 'Validated Settings and Qwen Advanced layouts at 96/144/192/288 DPI design scales.'
