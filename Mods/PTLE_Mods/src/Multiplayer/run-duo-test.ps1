# Two-player (or more) test : runs copies of the game in autotest mode, in the
# background, joined to the same room of a relay server. Each one shows every other
# player as an Evil Harry. Uses whatever run-autotest.ps1 installed last (run it
# first after changes).
#
#   powershell -ExecutionPolicy Bypass -File run-duo-test.ps1 [-Server 127.0.0.1:27777] [-Players 2] [-TimeoutSec 480]
#
# Results : Documents\PTLE Multiplayer\logs\a\, logs\b\... (log + screenshots).

param(
	[string] $Server = '127.0.0.1:27777',
	[ValidateRange(2, 8)] [int] $Players = 2,
	[int] $TimeoutSec = 480
)

$ErrorActionPreference = 'Stop'

$gameDir = 'C:\Program Files (x86)\Aspyr\PITFALL The Lost Expedition\Game'
$gameExe = Join-Path $gameDir 'PITFALL The Lost Expedition.exe'
$logDir  = Join-Path ([Environment]::GetFolderPath('MyDocuments')) 'PTLE Multiplayer\logs'

if (Get-Process -Name 'PITFALL The Lost Expedition' -ErrorAction SilentlyContinue) {
	throw 'The game is already running. Close it first.'
}

$names = @('a', 'b', 'c', 'd', 'e', 'f', 'g', 'h')[0..($Players - 1)]
$room = "duo-$PID"
$procs = @{}
foreach ($name in $names) {
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
	Start-Sleep -Seconds 3   # Not all loading at the very same moment.
}

foreach ($name in $names) {
	$p = $procs[$name]
	if (-not $p.WaitForExit($TimeoutSec * 1000)) {
		Write-Host "== Game $name timed out, killing it"
		$p.Kill(); $p.WaitForExit()
	}
	Write-Host "== Game $name exited with code $($p.ExitCode)"
}

Add-Type -AssemblyName System.Drawing
foreach ($name in $names) {
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
	Get-Content (Join-Path $logDir "$name\multiplayer.log") | Select-String 'Joined|Showing|left|forgetting|spawned \(|Sent [0-9]+ snapshots|  Player \d+ :' | Select-Object -Last (4 + 2 * $Players)
}

# Each game must have heard every other player, kept receiving from them and spawned
# a puppet for each. (The autotest's own item checks fail here : they expect Evil
# Harry to replay the local player, and the other games run a few seconds apart.)
$ok = $true
foreach ($name in $names) {
	$log = @(Get-Content (Join-Path $logDir "$name\multiplayer.log"))
	$me = ($log | Select-String 'Joined room .* as player (\d+)' | Select-Object -Last 1).Matches.Groups[1].Value
	# The stats lines (every 10 s, one per player heard) : the most received from each
	# other player, and whether their puppet was ever shown. (The games finish at
	# different times : the last lines may have forgotten the ones that quit.)
	$byPlayer = @{}
	foreach ($l in @($log | Select-String '\[NET\]   Player (\d+) : received (\d+), lost (\d+).*Puppet (.*)\.$')) {
		$g = $l.Matches.Groups
		if ($g[1].Value -eq $me) { continue }
		$id = $g[1].Value
		if (-not $byPlayer.ContainsKey($id)) { $byPlayer[$id] = @{ received = 0; lost = 0; shown = $false } }
		$e = $byPlayer[$id]
		if ([int] $g[2].Value -ge $e.received) { $e.received = [int] $g[2].Value; $e.lost = [int] $g[3].Value }
		if ($g[4].Value -eq 'shown') { $e.shown = $true }
	}
	$others = @($byPlayer.Keys | Sort-Object)
	$good = $me -ne $null -and $others.Count -eq ($Players - 1)
	# Their names (PACKET_INFO) : every other game's "player-x".
	$named = @($log | Select-String 'Player (\d+) is "(player-[a-h])"' | ForEach-Object { $_.Matches.Groups[2].Value } | Sort-Object -Unique)
	$expected = @($names | Where-Object { $_ -ne $name } | ForEach-Object { "player-$_" })
	$missing = @($expected | Where-Object { $named -notcontains $_ })
	Write-Host ("== Game {0} : knows the names {1} : {2}" -f $name, ($named -join ', '), $(if ($missing.Count -eq 0) { 'ok' } else { 'WRONG' }))
	if ($missing.Count -ne 0) { $good = $false }
	foreach ($id in $others) {
		$e = $byPlayer[$id]
		$fine = $e.received -gt 2500 -and $e.lost -lt $e.received / 20 -and $e.shown
		Write-Host ("== Game {0} (player {1}) : player {2} received {3}, lost {4}, puppet shown {5} : {6}" -f $name, $me, $id, $e.received, $e.lost, $e.shown, $(if ($fine) { 'ok' } else { 'WRONG' }))
		if (-not $fine) { $good = $false }
	}
	Write-Host ("== Game {0} : heard {1} of {2} other players : {3}" -f $name, $others.Count, ($Players - 1), $(if ($good) { 'ok' } else { 'WRONG' }))
	if (-not $good) { $ok = $false }
}
Write-Host $(if ($ok) { '== RESULT : PASS' } else { '== RESULT : FAIL' })
if (-not $ok) { exit 1 }
