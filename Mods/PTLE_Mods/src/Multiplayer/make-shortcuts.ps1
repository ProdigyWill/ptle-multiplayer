# Desktop shortcuts to play two copies of the game on this computer, both online in
# the room of config\Multiplayer\Multiplayer.ini (to test what the others see) :
#
#   powershell -ExecutionPolicy Bypass -File make-shortcuts.ps1
#
# "Pitfall MP - Player 1" : the game as usual (Multiplayer.ini). Open it first.
# "Pitfall MP - Player 2" : a second copy (PTLE_MP_INSTANCE=2), with its own
# settings, config\Multiplayer\Multiplayer-2.ini : made from Multiplayer.ini the first
# time, named "<your name> 2". Its log : %LOCALAPPDATA%\PTLE Multiplayer\logs\2\.
# Only the window in front takes the keyboard and gamepad : click the other to play it.

$ErrorActionPreference = 'Stop'

$gameDir  = 'C:\Program Files (x86)\Aspyr\PITFALL The Lost Expedition\Game'
$gameExe  = Join-Path $gameDir 'PITFALL The Lost Expedition.exe'
$desktop  = [Environment]::GetFolderPath('Desktop')
$shell    = New-Object -ComObject WScript.Shell

function New-Shortcut($name, $target, $arguments, $window = 1) {
	$s = $shell.CreateShortcut((Join-Path $desktop "$name.lnk"))
	$s.TargetPath = $target
	$s.Arguments = $arguments
	$s.WorkingDirectory = $gameDir   # The game needs its own folder as the current one.
	$s.IconLocation = "$gameExe,0"
	$s.WindowStyle = $window
	$s.Save()
	Write-Host "== $name"
}
New-Shortcut 'Pitfall MP - Player 1' $gameExe ''
# Player 2 : cmd sets the variable and starts the game, all in the shortcut (no
# script file next to it), its window minimized (7) so only the game shows.
New-Shortcut 'Pitfall MP - Player 2' (Join-Path $env:WINDIR 'System32\cmd.exe') "/c set PTLE_MP_INSTANCE=2&& start `"`" `"$gameExe`"" 7
