# Packs the built mods (bin\Release) into a zip for the other players :
#
#   powershell -ExecutionPolicy Bypass -File make-package.ps1 [-Full [-Server host:port] [-Room name] [-Name player]]
#
# Build first (run-autotest.ps1 does). The zip goes to Documents\PTLE Multiplayer\dist.
#
# Default : an update, PTLE_Multiplayer_update.zip, with only mods\ (the .asi files).
# Players who have the mod already copy it over their Game\mods folder; their
# settings (config\Multiplayer\Multiplayer.ini, or the old mods\Multiplayer.ini,
# which the mod moves there) stay as they are.
#
# -Full : a whole install, PTLE_Multiplayer.zip, for a new player : the mod loader and
# a Multiplayer.ini set up to join a relay server room. Same layout as the regular
# mod release : run install.bat as administrator and give it the Pitfall folder (the
# one with Start.exe).

param(
	[switch] $Full,
	[string] $Server = '15.204.239.19:27777',
	[string] $Room = 'two-pc-test',
	[string] $Name = 'Player 2'
)
$ErrorActionPreference = 'Stop'

$modsDir = Resolve-Path (Join-Path $PSScriptRoot '..\..')
$bin     = Join-Path $modsDir 'bin\Release'
$outDir  = Join-Path ([Environment]::GetFolderPath('MyDocuments')) 'PTLE Multiplayer\dist'
$stage   = Join-Path $outDir 'PTLE_Multiplayer'
$zip     = Join-Path $outDir $(if ($Full) { 'PTLE_Multiplayer.zip' } else { 'PTLE_Multiplayer_update.zip' })

if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
New-Item -ItemType Directory -Force (Join-Path $stage 'mods') | Out-Null
foreach ($mod in 'Multiplayer', 'WindowedMode', 'CheatLab') {
	Copy-Item (Join-Path $bin "$mod.asi") (Join-Path $stage 'mods')
}

if ($Full) {
	New-Item -ItemType Directory -Force (Join-Path $stage 'config\Multiplayer') | Out-Null
	Copy-Item (Join-Path $bin 'binkw32.dll') $stage                       # The mod loader.
	Copy-Item (Join-Path $modsDir 'src\ModLoader\config.ini') $stage
	Copy-Item (Join-Path $modsDir 'patch\*') $stage                       # install.bat, uninstall.bat.

	@"
; Multiplayer mod settings.
[Network]
; Relay server (host:port). Empty : no network, Evil Harry (F5) replays you.
Server=$Server
; Players in the same room see each other.
Room=$Room
Name=$Name
; 1 : the server sends your own moves back to you (Evil Harry replays you
; through the internet, to test alone).
Echo=0
"@ -replace "`r?`n", "`r`n" | Set-Content -Encoding ascii (Join-Path $stage 'config\Multiplayer\Multiplayer.ini')
}

if (Test-Path $zip) { Remove-Item $zip -Force }
Compress-Archive -Path (Join-Path $stage '*') -DestinationPath $zip
Write-Host "== $zip"
Get-ChildItem $stage -Recurse -File | ForEach-Object { "   $($_.FullName.Substring($stage.Length + 1))  ($($_.Length) bytes)" }
