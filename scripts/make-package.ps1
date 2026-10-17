# Builds dist/NoteForge-Windows.zip (plugin + installer + quick start + FL link script) from a Release build.
# Usage (from the repository root, after building):  powershell -ExecutionPolicy Bypass -File scripts\make-package.ps1
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$plugin = Join-Path $root 'build\NoteForge_artefacts\Release\VST3\NoteForge.vst3'
if (-not (Test-Path $plugin)) { throw "Build the plugin first (missing $plugin)" }

$stage = Join-Path $root 'dist\package\NoteForge'
if (Test-Path (Split-Path $stage)) { Remove-Item -Recurse -Force (Split-Path $stage) }
New-Item -ItemType Directory -Force $stage, (Join-Path $stage 'FL link (optional)') | Out-Null

Copy-Item -Recurse $plugin $stage
Copy-Item (Join-Path $root 'installer\*') $stage
Copy-Item (Join-Path $root 'Tools\fl-bridge\device_NoteForgeBridge.py') (Join-Path $stage 'FL link (optional)')
Copy-Item (Join-Path $root 'LICENSE') (Join-Path $stage 'LICENSE.txt')

$zip = Join-Path $root 'dist\NoteForge-Windows.zip'
if (Test-Path $zip) { Remove-Item $zip }
Compress-Archive -Path $stage -DestinationPath $zip
Remove-Item -Recurse -Force (Split-Path $stage)
Write-Host ("Created {0} ({1:N1} MB)" -f $zip, ((Get-Item $zip).Length / 1MB))
