# Packs the built mods (bin\Release) into a zip for the other players :
#
#   powershell -ExecutionPolicy Bypass -File make-package.ps1 [-Version 0.2.0] [-Full [-Server host:port] [-Room name] [-Name player]]
#
# The zips go to Documents\PTLE Multiplayer\dist, named with the version, with the
# changelog inside (CHANGELOG.txt).
#
# -Version : a build for the play testers. The first time for a version, it's
# released : the code must have no uncommitted changes; version.h gets the version,
# CHANGELOG.md's "Unreleased" section becomes that version's, the mods are built and
# tested (run-autotest.ps1, which must pass), then that's committed ("Release 0.2.0")
# and tagged v0.2.0 (not pushed). Run again with the same version (more zips : an
# update, one per player) : it checks the code is still that commit, builds, packs.
# Without -Version : a build of whatever is there now (version.h's, "...-dev"), for
# trying things out; build first (run-autotest.ps1 does).
#
# Default : an update, PTLE_Multiplayer_<version>_update.zip, with only mods\ (the
# .asi files). Players who have the mod already copy it over their Game\mods folder;
# their settings (config\Multiplayer\Multiplayer.ini) stay as they are.
#
# -Full : a whole install, PTLE_Multiplayer_<version>_<name>.zip, for a new player :
# the mod loader and a Multiplayer.ini set up to join a relay server room. Same layout
# as the regular mod release : run install.bat as administrator and give it the
# Pitfall folder (the one with Start.exe).

param(
	[string] $Version = '',
	[switch] $Full,
	[string] $Server = '15.204.239.19:27777',
	[string] $Room = 'two-pc-test',
	[string] $Name = 'Player 2'
)
$ErrorActionPreference = 'Stop'

$modsDir   = Resolve-Path (Join-Path $PSScriptRoot '..\..')
$bin       = Join-Path $modsDir 'bin\Release'
$outDir    = Join-Path ([Environment]::GetFolderPath('MyDocuments')) 'PTLE Multiplayer\dist'
$stage     = Join-Path $outDir 'PTLE_Multiplayer'
$versionH  = Join-Path $PSScriptRoot 'version.h'
$changelog = Join-Path $PSScriptRoot 'CHANGELOG.md'
$msbuild   = 'C:\Program Files\Microsoft Visual Studio\2022\Enterprise\MSBuild\Current\Bin\MSBuild.exe'

function RunGit { & git.exe -C $PSScriptRoot @args; if ($LASTEXITCODE -ne 0) { throw "git $args failed." } }
function CodeVersion { if ((Get-Content $versionH -Raw) -match '#define MP_VERSION "([^"]+)"') { $Matches[1] } else { throw 'No MP_VERSION in version.h.' } }
function WriteText($path, $text) { [IO.File]::WriteAllText($path, $text, (New-Object Text.UTF8Encoding $false)) }

if ($Version) {
	if ($Version -notmatch '^\d+\.\d+\.\d+$') { throw "Version : three numbers, like 0.2.0 (not '$Version')." }
	$tag = "v$Version"
	if (RunGit status --porcelain) { throw 'There are uncommitted changes : commit them first, so the zip matches a commit.' }
	$tagged = & git -C $PSScriptRoot rev-parse -q --verify "refs/tags/$tag"
	if (-not $tagged) {
		# A new release.
		$log = Get-Content $changelog -Raw
		if ($log -notmatch '(?m)^## Unreleased.*$') { throw 'CHANGELOG.md has no "## Unreleased" section to release.' }
		Write-Host "== Releasing $Version"
		$date = Get-Date -Format 'yyyy-MM-dd'
		WriteText $changelog ([regex]::Replace($log, '(?m)^## Unreleased.*$', "## $Version ($date)", 1))
		WriteText $versionH ([regex]::Replace((Get-Content $versionH -Raw), '#define MP_VERSION "[^"]+"', "#define MP_VERSION `"$Version`""))
		$logFile = Join-Path $env:LOCALAPPDATA 'PTLE Multiplayer\logs\multiplayer.log'
		$started = Get-Date
		try {
			& (Join-Path $PSScriptRoot 'run-autotest.ps1') | Out-Null
			$passed = (Test-Path $logFile) -and (Get-Item $logFile).LastWriteTime -gt $started -and (Select-String -Path $logFile -Pattern 'RESULT : PASS' -Quiet)
			if (-not $passed) { throw "The autotest didn't pass (see $logFile)." }
		}
		catch {
			RunGit checkout -- $versionH $changelog   # Back as they were : nothing released.
			throw
		}
		Write-Host '== Autotest : PASS'
		RunGit add -- $versionH $changelog
		RunGit commit -q -m "Release $Version" -m 'Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>'
		RunGit tag -a $tag -m "Multiplayer $Version"
		Write-Host "== Committed and tagged $tag (not pushed)"
	}
	else {
		# More zips of a release : the same code.
		if ((RunGit rev-parse HEAD) -ne (RunGit rev-parse "$tag^{commit}")) { throw "The code isn't $tag's any more (git checkout $tag to pack it again)." }
		if ((CodeVersion) -ne $Version) { throw "version.h doesn't say $Version." }
		Write-Host "== $tag : building"
		& $msbuild (Join-Path $modsDir 'PTLE_Mods.sln') '/t:ModLoader;Multiplayer;WindowedMode;CheatLab' /p:Configuration=Release /p:Platform=Win32 `
			/p:PlatformToolset=v143 /p:TrackFileAccess=false /v:minimal /nologo | Out-Null
		if ($LASTEXITCODE -ne 0) { throw "Build failed ($LASTEXITCODE)." }
	}
}
$v = CodeVersion
$safeName = ($Name -replace '[^A-Za-z0-9_-]', '')
$zip = Join-Path $outDir $(if ($Full) { "PTLE_Multiplayer_${v}_$safeName.zip" } else { "PTLE_Multiplayer_${v}_update.zip" })

if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
New-Item -ItemType Directory -Force (Join-Path $stage 'mods') | Out-Null
foreach ($mod in 'Multiplayer', 'WindowedMode', 'CheatLab') {
	Copy-Item (Join-Path $bin "$mod.asi") (Join-Path $stage 'mods')
}
(Get-Content $changelog -Raw) -replace "`r?`n", "`r`n" | Set-Content -Encoding utf8 -NoNewline (Join-Path $stage 'CHANGELOG.txt')

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
Write-Host "== $zip (version $v)"
Get-ChildItem $stage -Recurse -File | ForEach-Object { "   $($_.FullName.Substring($stage.Length + 1))  ($($_.Length) bytes)" }
