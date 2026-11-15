# Packs the built mods (bin\Release) into a zip to install on another PC, set up to
# join a relay server room :
#
#   powershell -ExecutionPolicy Bypass -File make-package.ps1 [-Server host:port] [-Room name] [-Name player]
#
# Build first (run-autotest.ps1 does). The zip goes to Documents\PTLE Multiplayer\dist.
# It has the same layout as the regular mod release : run install.bat as administrator
# on the other PC and give it the Pitfall folder (the one with Start.exe).
# The settings go in as config\Multiplayer\Multiplayer.new.ini : the mod uses them only
# if the player has none yet, so an update keeps their name, outfit and colours.

param(
	[string] $Server = '15.204.239.19:27777',
	[string] $Room = 'two-pc-test',
	[string] $Name = 'Player 2'
)
$ErrorActionPreference = 'Stop'

$modsDir = Resolve-Path (Join-Path $PSScriptRoot '..\..')
$bin     = Join-Path $modsDir 'bin\Release'
$outDir  = Join-Path ([Environment]::GetFolderPath('MyDocuments')) 'PTLE Multiplayer\dist'
$stage   = Join-Path $outDir 'PTLE_Multiplayer'
$zip     = Join-Path $outDir 'PTLE_Multiplayer.zip'

if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
New-Item -ItemType Directory -Force (Join-Path $stage 'mods') | Out-Null
New-Item -ItemType Directory -Force (Join-Path $stage 'config\Multiplayer') | Out-Null

Copy-Item (Join-Path $bin 'binkw32.dll') $stage                       # The mod loader.
Copy-Item (Join-Path $modsDir 'src\ModLoader\config.ini') $stage
Copy-Item (Join-Path $modsDir 'patch\*') $stage                       # install.bat, uninstall.bat.
foreach ($mod in 'Multiplayer', 'WindowedMode', 'CheatLab') {
	Copy-Item (Join-Path $bin "$mod.asi") (Join-Path $stage 'mods')
}

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
"@ -replace "`r?`n", "`r`n" | Set-Content -Encoding ascii (Join-Path $stage 'config\Multiplayer\Multiplayer.new.ini')

if (Test-Path $zip) { Remove-Item $zip -Force }
Compress-Archive -Path (Join-Path $stage '*') -DestinationPath $zip
Write-Host "== $zip"
Get-ChildItem $stage -Recurse -File | ForEach-Object { "   $($_.FullName.Substring($stage.Length + 1))  ($($_.Length) bytes)" }
