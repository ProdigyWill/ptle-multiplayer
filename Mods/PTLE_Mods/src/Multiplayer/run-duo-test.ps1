# Two-player test : runs two copies of the game in autotest mode, in the background,
# joined to the same room of a relay server. Each one's Evil Harry shows the other
# player. Uses whatever run-autotest.ps1 installed last (run it first after changes).
#
#   powershell -ExecutionPolicy Bypass -File run-duo-test.ps1 [-Server 127.0.0.1:27777] [-TimeoutSec 480]
#
# Results : Documents\PTLE Multiplayer\logs\a\ and logs\b\ (log + screenshots).

param(
	[string] $Server = '127.0.0.1:27777',
	[int] $TimeoutSec = 480
)

$ErrorActionPreference = 'Stop'

$gameDir = 'C:\Program Files (x86)\Aspyr\PITFALL The Lost Expedition\Game'
$gameExe = Join-Path $gameDir 'PITFALL The Lost Expedition.exe'
$logDir  = Join-Path ([Environment]::GetFolderPath('MyDocuments')) 'PTLE Multiplayer\logs'

if (Get-Process -Name 'PITFALL The Lost Expedition' -ErrorAction SilentlyContinue) {
	throw 'The game is already running. Close it first.'
}

$room = "duo-$PID"
$procs = @{}
foreach ($name in 'a', 'b') {
	$dir = Join-Path $logDir $name
	if (Test-Path (Join-Path $dir 'shots')) { Remove-Item (Join-Path $dir 'shots\*') -Force }
	$vars = @{
		PTLE_MP_AUTOTEST = '1'; PTLE_MP_INSTANCE = $name; GIZMOD_NO_CONSOLE = '1'; GIZMOD_BACKGROUND = '1'
		PTLE_MP_SERVER = $Server; PTLE_MP_ROOM = $room; PTLE_MP_NAME = "player-$name"; PTLE_MP_ECHO = '0'
	}
	foreach ($k in $vars.Keys) { Set-Item "Env:$k" $vars[$k] }
	try {
		$procs[$name] = Start-Process -FilePath $gameExe -WorkingDirectory $gameDir -PassThru
	} finally {
		foreach ($k in $vars.Keys) { Remove-Item "Env:$k" }
	}
	Write-Host "== Started game $name (pid $($procs[$name].Id)) in room $room on $Server"
	Start-Sleep -Seconds 3   # Not both loading at the very same moment.
}

foreach ($name in 'a', 'b') {
	$p = $procs[$name]
	if (-not $p.WaitForExit($TimeoutSec * 1000)) {
		Write-Host "== Game $name timed out, killing it"
		$p.Kill(); $p.WaitForExit()
	}
	Write-Host "== Game $name exited with code $($p.ExitCode)"
}

Add-Type -AssemblyName System.Drawing
foreach ($name in 'a', 'b') {
	$shots = Join-Path $logDir "$name\shots"
	if (Test-Path $shots) {
		Get-ChildItem $shots -Filter *.bmp | ForEach-Object {
			$img = [System.Drawing.Image]::FromFile($_.FullName)
			try { $img.Save([IO.Path]::ChangeExtension($_.FullName, '.png'), [System.Drawing.Imaging.ImageFormat]::Png) }
			finally { $img.Dispose() }
			Remove-Item $_.FullName
		}
	}
	Write-Host "== Game $name"
	Get-Content (Join-Path $logDir "$name\multiplayer.log") | Select-String 'Joined|Showing|left|forgetting|Sent [0-9]+ snapshots' | Select-Object -Last 4
}

# Each game must have shown the other player and kept receiving from them. (The
# autotest's own item checks fail here : they expect Evil Harry to replay the local
# player, and the other game runs a few seconds apart.)
$ok = $true
foreach ($name in 'a', 'b') {
	$log = Get-Content (Join-Path $logDir "$name\multiplayer.log")
	$me = ($log | Select-String 'Joined room .* as player (\d+)' | Select-Object -Last 1).Matches.Groups[1].Value
	$shown = ($log | Select-String 'Showing player (\d+)' | Select-Object -Last 1).Matches.Groups[1].Value
	$stats = ($log | Select-String 'Player \d+ : received (\d+), lost (\d+)' | Select-Object -Last 1).Matches
	$received = if ($stats) { [int] $stats.Groups[1].Value } else { 0 }
	$lost = if ($stats) { [int] $stats.Groups[2].Value } else { 0 }
	$good = $me -ne $null -and $shown -ne $null -and $me -ne $shown -and $received -gt 2500 -and $lost -lt $received / 20
	Write-Host ("== Game {0} : player {1}, showed player {2}, received {3}, lost {4} : {5}" -f $name, $me, $shown, $received, $lost, $(if ($good) { 'ok' } else { 'WRONG' }))
	if (-not $good) { $ok = $false }
}
Write-Host $(if ($ok) { '== DUO RESULT : PASS' } else { '== DUO RESULT : FAIL' })
if (-not $ok) { exit 1 }
