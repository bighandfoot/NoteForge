# Installs NoteForge: the VST3 plugin into the standard VST3 folder, and the optional
# "NoteForge Bridge" script into FL Studio's hardware-script folder.
$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path

# The VST3 folder is in Program Files, so this needs administrator rights: ask once, then continue.
$admin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole(
    [Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $admin) {
    Start-Process powershell -Verb RunAs -ArgumentList @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', "`"$($MyInvocation.MyCommand.Path)`"")
    exit
}

try {
    Write-Host ''
    Write-Host 'Installing NoteForge...' -ForegroundColor Cyan

    # always the 64-bit Common Files folder, even if this was started from a 32-bit program
    $common = if ($env:CommonProgramW6432) { $env:CommonProgramW6432 } else { $env:CommonProgramFiles }
    $vst3 = Join-Path $common 'VST3'
    New-Item -ItemType Directory -Force $vst3 | Out-Null
    $target = Join-Path $vst3 'NoteForge.vst3'
    if (Test-Path $target) { Remove-Item -Recurse -Force $target }
    Copy-Item -Recurse -Force (Join-Path $here 'NoteForge.vst3') $vst3
    Write-Host "  Plugin installed:   $target"

    # FL Studio keeps user data (including hardware scripts) where its own settings say
    $data = $null
    try { $data = (Get-ItemProperty 'HKCU:\Software\Image-Line\Shared\Paths' -ErrorAction Stop).'Shared data' } catch {}
    if (-not $data) { $data = Join-Path ([Environment]::GetFolderPath('MyDocuments')) 'Image-Line' }
    $hw = Join-Path $data 'FL Studio\Settings\Hardware\NoteForge Bridge'
    New-Item -ItemType Directory -Force $hw | Out-Null
    Copy-Item -Force (Join-Path $here 'FL link (optional)\device_NoteForgeBridge.py') $hw
    Write-Host "  FL link script:     $hw"

    Write-Host ''
    Write-Host 'Done! Now open FL Studio, go to Options > Manage plugins and click "Find installed plugins".' -ForegroundColor Green
}
catch {
    Write-Host ''
    Write-Host "Something went wrong: $($_.Exception.Message)" -ForegroundColor Red
    Write-Host 'If FL Studio is open, close it and run the installer again.'
}
Write-Host ''
Read-Host 'Press Enter to close'
