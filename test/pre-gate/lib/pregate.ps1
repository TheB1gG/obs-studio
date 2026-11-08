# ---------------------------------------------------------------------------
# pre-gate suite - shared helpers.
#
# Why this exists: every harness used while building the streaming-destination
# feature ended its run by TerminateProcess-ing OBS. A killed process never runs
# obs_shutdown(), so nothing tested the teardown of a session - and quitting OBS
# while a multi-destination session is live crashes. Every OBS case here therefore
# ends with a real quit through the OBS_PREGATE_ACTIONS hook, and the assertions
# below are written so that a run which never reached obs_shutdown cannot pass.
# ---------------------------------------------------------------------------

$ErrorActionPreference = 'Stop'

function pg_paths {
	$libDir = $PSScriptRoot                                  # test/pre-gate/lib
	$caseDir = Split-Path $libDir -Parent                     # test/pre-gate
	$root = Split-Path (Split-Path $caseDir -Parent) -Parent   # repo root

	$run = Join-Path $root 'build_x64\rundir\RelWithDebInfo'
	@{
		Root     = $root
		Case     = $caseDir
		Fixtures = Join-Path $caseDir 'fixtures'
		RunDir   = $run
		Wd       = (Join-Path $run 'bin\64bit')
		Exe      = (Join-Path $run 'bin\64bit\obs64.exe')
		Cfg      = (Join-Path $run 'config\obs-studio')
		Logs     = (Join-Path $run 'config\obs-studio\logs')
		Crashes  = (Join-Path $run 'config\obs-studio\crashes')
		Profiles = (Join-Path $run 'config\obs-studio\basic\profiles')
		Scenes   = (Join-Path $run 'config\obs-studio\basic\scenes')
		Global   = (Join-Path $run 'config\obs-studio\global.ini')
		Sentinel = (Join-Path $run 'disable_updater')
		Work     = (Join-Path $root 'scratch\tmp\pregate')
		Rec      = (Join-Path $root 'scratch\tmp\pregate\rec')
		MtxExe   = if ($env:OBS_PREGATE_MEDIAMTX) { $env:OBS_PREGATE_MEDIAMTX } else {
			'E:\Downloads\mediamtx_v1.21.0_windows_amd64\mediamtx.exe'
		}
		MtxConf  = (Join-Path $caseDir 'mediamtx-api.yml')
		Media    = if ($env:OBS_PREGATE_MEDIA) { $env:OBS_PREGATE_MEDIA } else {
			'D:/obs/2026-10-27_22-10-05.mp4'
		}
		Api      = 'http://127.0.0.1:9997'
	}
}

$PG = pg_paths

# --- result plumbing: every check lands here, the runner reads the exit code ---
$script:pgFails = @()
$script:pgSkipped = $false

function pg_say([string]$text) { Write-Output $text }

function pg_check([bool]$ok, [string]$what) {
	if ($ok) { pg_say "  ok    $what" } else { pg_say "  FAIL  $what"; $script:pgFails += $what }
}

function pg_skip([string]$why) { pg_say "  SKIP  $why"; $script:pgSkipped = $true }

function pg_verdict([string]$name) {
	if ($script:pgSkipped) {
		$global:PGCODE = 2
		pg_say "RESULT $name SKIP"
	} elseif ($script:pgFails.Count) {
		$global:PGCODE = 1
		pg_say "RESULT $name FAIL ($($script:pgFails.Count)): $($script:pgFails -join ' | ')"
	} else {
		$global:PGCODE = 0
		pg_say "RESULT $name PASS"
	}
}

# --- process lifecycle -------------------------------------------------------
function pg_kill_strays {
	Get-Process obs64, mediamtx -ErrorAction SilentlyContinue | ForEach-Object {
		try { $_.Kill(); $null } catch {}
	}
	Start-Sleep -Seconds 2
}

function pg_crash_names {
	@(Get-ChildItem $PG.Crashes -Filter 'Crash *.txt' -ErrorAction SilentlyContinue | ForEach-Object { $_.Name })
}

function pg_new_crashes([string[]]$before) {
	@(pg_crash_names | Where-Object { $before -notcontains $_ })
}

# --- portable tree and fixtures ---------------------------------------------
function pg_prepare_tree {
	if (-not (Test-Path $PG.Exe)) { throw "no OBS build in the rundir: $($PG.Exe)" }
	New-Item -ItemType Directory -Force -Path $PG.Work, $PG.Rec, $PG.Logs, $PG.Crashes | Out-Null
	if (-not (Test-Path $PG.Sentinel)) { New-Item -ItemType File -Path $PG.Sentinel | Out-Null }

	# ConfirmOnExit defaults to true, so a modal "really quit?" dialog would sit between the scripted quit
	# and the shutdown path, and the case would then fail for a reason unrelated to what it tests.
	if (Test-Path $PG.Global) {
		$raw = [System.IO.File]::ReadAllText($PG.Global)
		$eol = if ($raw.Contains("`r`n")) { "`r`n" } else { "`n" }
		$g = $raw -replace "`r`n", "`n"
		if ($g -match '(?m)^ConfirmOnExit=') {
			$g = $g -replace '(?m)^ConfirmOnExit=.*$', 'ConfirmOnExit=false'
		} elseif ($g -match '(?m)^\[General\]\s*$') {
			$g = $g -replace '(?m)^(\[General\])\s*$', "`$1`nConfirmOnExit=false"
		} else {
			$g = "[General]`nConfirmOnExit=false`n" + $g
		}
		[System.IO.File]::WriteAllText($PG.Global, ($g -split "`n" -join $eol))
	}
}

# 'a' follows the profile service (the layout a user gets by importing a service), 'b' carries a service
# file of its own - the two shapes the destination engine has to keep apart.
function pg_install_profile {
	param([string]$Name, [int]$Destinations = 2, [int]$SharedKbps = 3000,
		[string]$OverrideOn = '', [int]$OverrideKbps = 500, [string]$Server = 'rtmp://127.0.0.1:1935/live',
		[switch]$Reconnect = $true)

	$dir = Join-Path $PG.Profiles $Name
	if (Test-Path $dir) { Remove-Item $dir -Recurse -Force }
	New-Item -ItemType Directory -Force -Path $dir | Out-Null

	$override = if ($OverrideKbps) { "`nVideoBitrateOverride=true`nVideoBitrate=$OverrideKbps" } else { '' }
	$list = 'a'
	$sections = "[Stream-a]`nName=Primary`nEnabled=true`nFollowsProfileService=true"
	if ($OverrideOn -eq 'a') { $sections += $override }
	if ($Destinations -ge 2) {
		$list += ',b'
		$b = "[Stream-b]`nName=Backup`nEnabled=true`nOwnService=true"
		if ($OverrideOn -eq 'b') { $b += $override }
		$sections += "`n`n$b"
	}

	$ini = [System.IO.File]::ReadAllText((Join-Path $PG.Fixtures 'profile-basic.ini'))
	$ini = $ini.Replace('{{NAME}}', $Name).Replace('{{REC}}', ($PG.Rec -replace '\\', '/'))
	$ini = $ini.Replace('{{DESTLIST}}', $list).Replace('{{SECTIONS}}', $sections)
	$ini = $ini.Replace('{{RECONNECT}}', $(if ($Reconnect) { 'true' } else { 'false' }))
	[System.IO.File]::WriteAllText((Join-Path $dir 'basic.ini'), ($ini -replace "`r`n", "`n"))

	[System.IO.File]::WriteAllText((Join-Path $dir 'streamEncoder.json'),
		"{`"bitrate`": $SharedKbps, `"rate_control`": `"CBR`"}")
	[System.IO.File]::WriteAllText((Join-Path $dir 'service.json'),
		"{`"type`":`"rtmp_custom`",`"settings`":{`"server`":`"$Server`",`"key`":`"pregate-a`"}}")
	[System.IO.File]::WriteAllText((Join-Path $dir 'service_b.json'),
		"{`"type`":`"rtmp_custom`",`"settings`":{`"server`":`"$Server`",`"key`":`"pregate-b`"}}")
	return $Name
}

function pg_install_collection {
	param([string]$Name, [switch]$WithMedia)

	$fixture = if ($WithMedia) { 'collection-media.json' } else { 'collection-empty.json' }
	if ($WithMedia -and -not (Test-Path $PG.Media)) { return $false }

	$path = Join-Path $PG.Scenes "$Name.json"
	if (Test-Path $path) { Remove-Item $path -Force }
	$json = [System.IO.File]::ReadAllText((Join-Path $PG.Fixtures $fixture))
	$json = $json.Replace('{{NAME}}', $Name).Replace('{{MEDIA}}', ($PG.Media -replace '\\', '/'))
	[System.IO.File]::WriteAllText($path, $json)
	return $true
}

# --- one OBS run ------------------------------------------------------------
function pg_log_names {
	@(Get-ChildItem $PG.Logs -Filter '*.txt' -ErrorAction SilentlyContinue | ForEach-Object { $_.Name })
}

function pg_newest_log([string[]]$before) {
	$new = @(Get-ChildItem $PG.Logs -Filter '*.txt' -ErrorAction SilentlyContinue |
		Where-Object { $before -notcontains $_.Name } | Sort-Object LastWriteTime)
	if ($new.Count) { return $new[-1] }
	return $null
}

function pg_start_obs {
	param([string]$Profile, [string]$Collection, [string]$Actions, [switch]$Stream, [switch]$Record)

	$argv = @('--portable', '--profile', $Profile, '--collection', $Collection,
		'--disable-shutdown-check', '--disable-updater')
	if ($Stream) { $argv += '--startstreaming' }
	if ($Record) { $argv += '--startrecording' }

	if ($Actions) { $env:OBS_PREGATE_ACTIONS = $Actions }
	$proc = Start-Process -FilePath $PG.Exe -ArgumentList $argv -WorkingDirectory $PG.Wd -PassThru
	Remove-Item Env:OBS_PREGATE_ACTIONS -ErrorAction SilentlyContinue
	return $proc
}

# OBS has to close itself: the shutdown path is what is under test, so a case that ends by killing
# the process never ran the code it is supposed to check.
function pg_wait_self_exit($proc, [int]$timeoutSec) {
	$deadline = (Get-Date).AddSeconds($timeoutSec)
	while ((Get-Date) -lt $deadline) {
		if ($proc.WaitForExit(2000)) { return $true }
	}
	return $false
}

# "Freeing OBS context data" is what obs_free_data() logs (libobs/obs.c). Any output still registered
# at that point is torn down by libobs while its core video data goes away - the crash this suite
# exists to catch, so it is asserted directly instead of only watching for a crash report.
function pg_check_shutdown {
	param([string]$logText, [object[]]$crashes, [int]$exitCode, [bool]$selfExited)

	pg_check $selfExited 'OBS quit by itself (scripted quit, never a kill)'
	pg_check ($exitCode -eq 0) "OBS exit code is 0 (was $exitCode)"
	pg_check ($crashes.Count -eq 0) "no crash report written ($($crashes -join ', '))"
	pg_check ($logText -match 'pregate: ') 'the pregate action hook ran, otherwise the case proves nothing'
	pg_check ($logText -match 'Freeing OBS context data') 'obs_free_data ran, so the shutdown was reached'

	$parts = $logText -split 'Freeing OBS context data'
	if ($parts.Count -gt 1) {
		pg_check ($parts[1] -notmatch "Output '[^']+': (stopping|User stopped)") `
			'no output was left registered when libobs started freeing its data'
	}

	# libobs counts what it had to destroy by itself, and says so. Anything left in those lists was still
	# owned by the frontend when obs_free_data() ran, which is after obs_shutdown() unloaded the plugin
	# modules - the object's type information then points at code that is gone. This is the assertion that
	# catches a leaked reference whether or not it happens to crash.
	$leaked = [regex]::Matches($logText, '\t(\d+) (output|encoder|service)\(s\) were remaining')
	$counts = @($leaked | ForEach-Object { $_.Groups[2].Value + '=' + $_.Groups[1].Value }) -join ', '
	pg_check ($leaked.Count -eq 0) "libobs found no objects still registered at shutdown ($counts)"
}

# Read the growing log while the run goes up, so the case knows whether it is looking at a session that
# uploads or at one that was refused. OBS owns the log file, so the read has to ask for share mode
# read/write; and a refused first attempt is not the end of the story when reconnect is on, so the gate
# waits for the deadline and reports what it saw instead of guessing early.
function pg_wait_live {
	param([string[]]$LogsBefore, [int]$Want, [int]$TimeoutSec = 30)

	$deadline = (Get-Date).AddSeconds($TimeoutSec)
	$seen = 0
	while ((Get-Date) -lt $deadline) {
		$log = pg_newest_log $LogsBefore
		if ($log) {
			$text = ''
			try {
				$stream = [System.IO.File]::Open($log.FullName, [System.IO.FileMode]::Open,
					[System.IO.FileAccess]::Read, [System.IO.FileShare]::ReadWrite)
				$reader = [System.IO.StreamReader]::new($stream)
				$text = $reader.ReadToEnd()
				$reader.Dispose(); $stream.Dispose()
			} catch {}
			$seen = ([regex]::Matches($text, "is live \(\d+ of \d+ enabled destinations up\)")).Count
			if ($seen -ge $Want) { return $true }
		}
		Start-Sleep -Milliseconds 700
	}
	pg_say "  note  after $TimeoutSec s the log showed $seen of $Want destination(s) live"
	return $false
}

function pg_session {
	param([string]$Case, [string]$Actions, [int]$Destinations = 2, [int]$SharedKbps = 3000,
		[string]$OverrideOn = '', [int]$OverrideKbps = 500, [switch]$Stream, [switch]$Record,
		[switch]$Media, [switch]$Ingest, [int]$ExitTimeoutSec = 150, [string[]]$ExpectInLog = @(),
		[int]$ExpectLive = 0, [string]$Server = 'rtmp://127.0.0.1:1935/live', [switch]$Reconnect = $true,
		[int]$LiveTimeoutSec = 30)

	pg_prepare_tree
	if ($Ingest -and -not (pg_ingest_ready)) {
		pg_skip "the ingest is not installed (set OBS_PREGATE_MEDIAMTX, looked for: $($PG.MtxExe))"
		return
	}
	$profile = "pregate-$Case"
	$collection = "pregate-$Case"
	[void](pg_install_profile -Name $profile -Destinations $Destinations -SharedKbps $SharedKbps `
		-OverrideOn $OverrideOn -OverrideKbps $OverrideKbps -Server $Server -Reconnect:$Reconnect)
	if (-not (pg_install_collection -Name $collection -WithMedia:$Media)) {
		pg_skip "a content-carrying scene needs a sample file (set OBS_PREGATE_MEDIA, looked for: $($PG.Media))"
		return
	}

	$crashesBefore = pg_crash_names
	$logsBefore = pg_log_names
	# Not $ingest: PowerShell variable names are case-insensitive, and a local called $ingest overwrites the
	# -Ingest switch - the ingest then never gets started and the case runs against no listener at all.
	$session = $null
	if ($Ingest) {
		pg_say "  ingst starting the ingest ($($PG.MtxExe))"
		$session = pg_ingest_start
		pg_say "  ingst ingest is up (pid $($session.proc.Id)), log $($session.log)"
	}
	pg_say "  run   profile=$profile destinations=$Destinations actions='$Actions'"

	$proc = pg_start_obs -Profile $profile -Collection $collection -Actions $Actions -Stream:$Stream -Record:$Record

	# A case that wants to shut a running session down has to have one. "Uploading" and "connected to
	# nothing" are different teardown paths, so scoring the shutdown of a session that never went live would
	# be testing something else while claiming to test this - say which one it was, and say it before the
	# shutdown checks get to look.
	if ($ExpectLive -gt 0) {
		$live = pg_wait_live -LogsBefore $logsBefore -Want $ExpectLive -TimeoutSec $LiveTimeoutSec
		pg_check $live ("the session was really uploading: $ExpectLive destination(s) reached 'is live' within $LiveTimeoutSec s")
	}

	$exited = pg_wait_self_exit $proc $ExitTimeoutSec
	$exitCode = -1
	if ($exited) {
		try { $exitCode = $proc.ExitCode } catch { $exitCode = -1 }
	} else {
		try { $proc.Kill(); $null } catch {}
		Start-Sleep -Seconds 2
	}
	Start-Sleep -Seconds 2

	$log = pg_newest_log $logsBefore
	if (-not $log) { pg_ingest_stop $session; pg_check $false 'OBS wrote a log file'; return }
	$text = [System.IO.File]::ReadAllText($log.FullName)
	pg_check_shutdown -logText $text -crashes (pg_new_crashes $crashesBefore) -exitCode $exitCode -selfExited $exited

	if ($ExpectLive -gt 0) {
		$live = ([regex]::Matches($text, "is live \(\d+ of \d+ enabled destinations up\)")).Count
		pg_check ($live -ge $ExpectLive) "$ExpectLive destination(s) reached 'is live' (the log said $live)"
	}
	foreach ($pattern in $ExpectInLog) {
		pg_check ($text -match $pattern) "the log contains /$pattern/"
	}
	if ($session) {
		foreach ($line in (pg_ingest_reject_lines $session)) { pg_say "  note  ingest refused: $line" }
		pg_ingest_stop $session
	}
}

# --- ingest: cases that count bytes on the wire, not labels in a dialog ------
function pg_ingest_ready { Test-Path $PG.MtxExe }

# A TCP connect is the only honest check that the RTMP port is really served: the ingest answers on its API
# even when the media listener it needs is the one that died, and a case that streams into nothing would
# report a shutdown problem instead of saying that nothing was ever uploaded.
function pg_tcp_open([string]$host_, [int]$port) {
	try {
		$client = [System.Net.Sockets.TcpClient]::new()
		$client.Connect($host_, $port)
		$client.Close()
		return $true
	} catch {
		return $false
	}
}

function pg_ingest_start {
	New-Item -ItemType Directory -Force -Path $PG.Work | Out-Null

	# An orphaned ingest would pass the API check below while refusing the RTMP port, and the case would then
	# fail on connections nothing attempted. So take such processes down by name and refuse to run at all if
	# a port is still busy.
	Get-Process mediamtx -ErrorAction SilentlyContinue | ForEach-Object { try { $_.Kill(); $null } catch {} }
	Start-Sleep -Seconds 2
	$apiPort = ([System.Uri]$PG.Api).Port
	foreach ($port in @(1935, $apiPort)) {
		$owner = Get-NetTCPConnection -LocalPort $port -State Listen -ErrorAction SilentlyContinue |
			Select-Object -First 1
		if ($owner) {
			throw "port $port is served by pid $($owner.OwningProcess) - an orphaned ingest? clear it first"
		}
	}

	$mtxLog = Join-Path $PG.Work 'mediamtx.log'
	if (Test-Path $mtxLog) { Remove-Item $mtxLog -Force -ErrorAction SilentlyContinue }
	$p = Start-Process -FilePath $PG.MtxExe -ArgumentList "`"$($PG.MtxConf)`"" -WorkingDirectory $PG.Work `
		-RedirectStandardOutput $mtxLog -RedirectStandardError "$mtxLog.err" -PassThru -WindowStyle Hidden

	# The ingest has to answer on its API before OBS is started: a run whose ingest never came up would fail
	# with "no destination reached is live", which blames the code under test for a missing listener.
	for ($i = 0; $i -lt 20; $i++) {
		Start-Sleep -Milliseconds 600
		$api = $false
		try {
			$null = Invoke-RestMethod -Uri "$($PG.Api)/v3/paths/list" -TimeoutSec 3
			$api = $true
		} catch {}
		if ($api -and (pg_tcp_open '127.0.0.1' $apiPort) -and (pg_tcp_open '127.0.0.1' 1935)) {
			return @{ proc = $p; log = $mtxLog }
		}
	}

	$exited = $false
	try { $exited = $p.HasExited } catch {}
	$reason = ''
	if (Test-Path "$mtxLog.err") { $reason = (@(Get-Content "$mtxLog.err" -TotalCount 3) -join ' / ') }
	elseif (Test-Path $mtxLog) { $reason = (@(Get-Content $mtxLog -TotalCount 6) -join ' / ') }
	else { $reason = 'the ingest wrote nothing at all' }
	try { $p.Kill(); $null } catch {}
	throw "the ingest at $($PG.MtxExe) never answered on $($PG.Api) (exited=$exited): $reason"
}

function pg_ingest_stop($ingest) {
	if ($ingest -and $ingest.proc) { try { $ingest.proc.Kill(); $null } catch {} }
}

function pg_ingest_reject_lines($ingest) {
	if (-not (Test-Path $ingest.log)) { return @() }
	@(Select-String -Path $ingest.log -Pattern 'rejected|closed an incoming|invalid' |
		Select-Object -First 6 -ExpandProperty Line)
}

function pg_ingest_path([string]$name) {
	foreach ($attempt in @($name, ($name -replace '/', '~1'), [uri]::EscapeDataString($name))) {
		try { return Invoke-RestMethod -Uri "$($PG.Api)/v3/paths/get/$attempt" -TimeoutSec 5 } catch {}
	}
	try {
		$list = Invoke-RestMethod -Uri "$($PG.Api)/v3/paths/list" -TimeoutSec 5
		foreach ($item in $list.items) { if ($item.name -eq $name) { return $item } }
	} catch {}
	return $null
}

# The rate the ingest counted on its way in, for every path over the same window: the paths are judged
# against each other, so sampling them at different times would compare two different pieces of content
# instead of two encoders. kbps is -1 when the path never appeared.
function pg_ingest_kbps_pair {
	param([string[]]$names, [int]$seconds = 15)

	$first = @{}
	foreach ($n in $names) {
		for ($i = 0; $i -lt 25 -and -not $first.ContainsKey($n); $i++) {
			$row = pg_ingest_path $n
			if ($row) { $first[$n] = $row } else { Start-Sleep -Seconds 1 }
		}
	}
	if ($first.Count -eq 0) { return $null }

	$probe = @($first.Values)[0]
	$fields = @($probe.PSObject.Properties | Where-Object { $_.Name -match 'Byte' } | ForEach-Object { $_.Name })
	$field = if ($fields -contains 'inboundBytes') { 'inboundBytes' } elseif ($fields.Count) { $fields[0] } else { '' }
	if (-not $field) { return $null }

	$t0 = Get-Date
	Start-Sleep -Seconds $seconds
	$out = @{}
	foreach ($n in $names) {
		$later = pg_ingest_path $n
		if ((-not $later) -or (-not $first.ContainsKey($n))) {
			$out[$n] = [pscustomobject]@{ kbps = -1.0; field = $field }
			continue
		}
		$dt = ((Get-Date) - $t0).TotalSeconds
		$out[$n] = [pscustomobject]@{
			kbps  = [math]::Round((([long]$later.$field - [long]$first[$n].$field) * 8.0 / $dt) / 1000.0, 1)
			field = $field
		}
	}
	return $out
}

# A session whose destinations are measured at the ingest rather than read back from the UI. With no
# override both paths must cost the profile bitrate; with one, that path must cost the override and the
# other must stay at the profile value, which is the whole point of encoding a destination of its own.
function pg_bitrate_case {
	param([string]$Case, [int]$Destinations = 2, [int]$SharedKbps = 3000, [string]$OverrideOn = '',
		[int]$OverrideKbps = 800, [int]$AudioKbps = 160, [int]$SampleSeconds = 15, [switch]$NoMedia)

	pg_prepare_tree
	if (-not (pg_ingest_ready)) {
		pg_skip "the ingest is not installed (set OBS_PREGATE_MEDIAMTX, looked for: $($PG.MtxExe))"
		return
	}
	if ($OverrideOn -and -not $NoMedia) {
		if (-not (pg_install_collection -Name "pregate-$Case" -WithMedia)) {
			pg_skip "a content-carrying scene needs a sample file (set OBS_PREGATE_MEDIA, looked for: $($PG.Media))"
			return
		}
	}

	[void](pg_install_profile -Name "pregate-$Case" -Destinations $Destinations -SharedKbps $SharedKbps `
		-OverrideOn $OverrideOn -OverrideKbps $OverrideKbps)
	if (-not (pg_install_collection -Name "pregate-$Case" -WithMedia:(-not $NoMedia))) {
		pg_install_collection -Name "pregate-$Case" | Out-Null
	}

	$crashesBefore = pg_crash_names
	$logsBefore = pg_log_names
	$ingest = pg_ingest_start
	$warmup = $SampleSeconds + 12
	$quitAt = $warmup + ($SampleSeconds * $Destinations) + 12
	pg_say "  run   profile=pregate-$Case shared=${SharedKbps}kb/s override-on='$OverrideOn' override=${OverrideKbps}kb/s"

	$proc = pg_start_obs -Profile "pregate-$Case" -Collection "pregate-$Case" `
		-Actions "quit@$quitAt" -Stream
	$live = pg_wait_live -LogsBefore $logsBefore -Want $Destinations -TimeoutSec 30
	pg_check $live "the session was really uploading before anything was sampled ($Destinations destination(s) live)"
	pg_say "  meas  sampling both paths together for $SampleSeconds s after a ${warmup}s warm-up"

	$names = @('live/pregate-a')
	if ($Destinations -ge 2) { $names += 'live/pregate-b' }
	$pair = pg_ingest_kbps_pair -names $names -seconds $SampleSeconds
	$rows = [ordered]@{}
	if ($pair) {
		$rows['a'] = $pair['live/pregate-a']
		if ($Destinations -ge 2) { $rows['b'] = $pair['live/pregate-b'] }
	}

	$exited = pg_wait_self_exit $proc 150
	$exitCode = -1
	if ($exited) { try { $exitCode = $proc.ExitCode } catch { $exitCode = -1 } } else {
		try { $proc.Kill(); $null } catch {}
		Start-Sleep -Seconds 2
	}
	Start-Sleep -Seconds 2
	$log = pg_newest_log $logsBefore
	if (-not $log) { pg_ingest_stop $ingest; pg_check $false 'OBS wrote a log file'; return }
	$text = [System.IO.File]::ReadAllText($log.FullName)
	pg_check_shutdown -logText $text -crashes (pg_new_crashes $crashesBefore) -exitCode $exitCode -selfExited $exited

	# Audio and the RTMP framing ride along in the ingest counter, so the configured cost of a path is its
	# video plus the audio it shares.
	$expected = @{ a = ($SharedKbps + $AudioKbps); b = ($SharedKbps + $AudioKbps) }
	if ($OverrideOn -eq 'a') { $expected['a'] = $OverrideKbps + $AudioKbps }
	if ($OverrideOn -eq 'b') { $expected['b'] = $OverrideKbps + $AudioKbps }

	if ($rows.Count -eq 0) {
		pg_check $false 'the ingest reported the paths this session was supposed to publish'
		return
	}

	# What E6 changes is who pays for which encoder, so the pair is judged as a ratio. An encoder that is not
	# handed content complex enough to fill its target undershoots that target, and it undershoots on both
	# paths at once - an absolute window around the configured cost would be measuring the clip, not E6. The
	# absolute number here is only a floor proving something was actually moving.
	foreach ($id in @('a', 'b')) {
		if (-not $rows.Contains($id)) { continue }
		$measured = $rows[$id].kbps
		$want = $expected[$id]
		pg_say ("  meas  path {0}: {1} kb/s over {2} (configured cost {3} kb/s)" -f $id, $measured, $rows[$id].field, $want)
		pg_check ($measured -ge 0.35 * $want) ("path '{0}' carried a real stream: {1} kb/s against a configured {2}" -f
			$id, $measured, $want)
	}

	if ($rows.Count -ge 2 -and $rows['a'].kbps -gt 0 -and $rows['b'].kbps -gt 0) {
		$gotRatio = [math]::Round($rows['b'].kbps / $rows['a'].kbps, 3)
		$wantRatio = $expected['b'] / $expected['a']
		pg_check ([math]::Abs($gotRatio - $wantRatio) -le 0.15 * $wantRatio) (
			"path b costs {0} of path a, measured {1} (a={2} kb/s, b={3} kb/s)" -f
			[math]::Round($wantRatio, 3), $gotRatio, $rows['a'].kbps, $rows['b'].kbps)

		if ($OverrideOn) {
			$own = if ($OverrideOn -eq 'a') { $rows['a'].kbps } else { $rows['b'].kbps }
			$shared = if ($OverrideOn -eq 'a') { $rows['b'].kbps } else { $rows['a'].kbps }
			pg_check ($own -lt 0.75 * $shared) (
				"the destination with its own bitrate pays less than the shared encoder: $own against $shared kb/s")
			pg_check ($text -match 'adv_stream_[ab]') 'the destination built a video encoder of its own (named adv_stream_<id>)'
		}
	}
	if ($Destinations -ge 2) {
		pg_check ($text -notmatch 'multitrack-video') 'the plain streaming path was used, so no multitrack guard applies'
	}

	foreach ($line in (pg_ingest_reject_lines $ingest)) { pg_say "  note  ingest refused: $line" }
	pg_ingest_stop $ingest
}
