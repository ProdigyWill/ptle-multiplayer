# Builds the Multiplayer plugin, installs it, runs the game in autotest mode and
# collects the results (log + screenshots converted to PNG).
#
#   powershell -ExecutionPolicy Bypass -File run-autotest.ps1 [-NoBuild] [-TimeoutSec 420]
#
# Results : Documents\PTLE Multiplayer\logs\multiplayer.log and logs\shots\*.png

param(
	[switch] $NoBuild,
	[int] $TimeoutSec = 420
)

$ErrorActionPreference = 'Stop'

$modsDir  = Resolve-Path (Join-Path $PSScriptRoot '..\..')
$gameDir  = 'C:\Program Files (x86)\Aspyr\PITFALL The Lost Expedition\Game'
$gameExe  = Join-Path $gameDir 'PITFALL The Lost Expedition.exe'
$msbuild  = 'C:\Program Files\Microsoft Visual Studio\2022\Enterprise\MSBuild\Current\Bin\MSBuild.exe'
$logDir   = Join-Path ([Environment]::GetFolderPath('MyDocuments')) 'PTLE Multiplayer\logs'
$logFile  = Join-Path $logDir 'multiplayer.log'
$shotsDir = Join-Path $logDir 'shots'

if (Get-Process -Name 'PITFALL The Lost Expedition' -ErrorAction SilentlyContinue) {
	throw 'The game is already running. Close it first.'
}

if (-not $NoBuild) {
	Write-Host '== Building Multiplayer plugin'
	& $msbuild (Join-Path $modsDir 'PTLE_Mods.sln') /t:Multiplayer /p:Configuration=Release /p:Platform=Win32 `
		/p:PlatformToolset=v143 /p:TrackFileAccess=false /v:minimal /nologo | Out-Host
	if ($LASTEXITCODE -ne 0) { throw "Build failed ($LASTEXITCODE)." }
}

Write-Host '== Installing'
Copy-Item (Join-Path $modsDir 'bin\Release\Multiplayer.asi') (Join-Path $gameDir 'mods\Multiplayer.asi') -Force

if (Test-Path $shotsDir) { Remove-Item (Join-Path $shotsDir '*') -Force }

Write-Host '== Running game in autotest mode'
$env:PTLE_MP_AUTOTEST = '1'
try {
	$proc = Start-Process -FilePath $gameExe -WorkingDirectory $gameDir -PassThru
} finally {
	Remove-Item Env:PTLE_MP_AUTOTEST
}

if (-not $proc.WaitForExit($TimeoutSec * 1000)) {
	Write-Host "== Timed out after $TimeoutSec s, killing the game"
	$proc.Kill()
	$proc.WaitForExit()
}
Write-Host "== Game exited with code $($proc.ExitCode)"

if (Test-Path $shotsDir) {
	Add-Type -AssemblyName System.Drawing
	Get-ChildItem $shotsDir -Filter *.bmp | ForEach-Object {
		$img = [System.Drawing.Image]::FromFile($_.FullName)
		try { $img.Save([IO.Path]::ChangeExtension($_.FullName, '.png'), [System.Drawing.Imaging.ImageFormat]::Png) }
		finally { $img.Dispose() }
		Remove-Item $_.FullName
	}
	Write-Host "== Screenshots : $shotsDir"
	Get-ChildItem $shotsDir -Filter *.png | ForEach-Object { "   $($_.Name)" }
}

Write-Host '== Log'
if (Test-Path $logFile) { Get-Content $logFile } else { Write-Host '(no log file)' }
