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

# Cycles and liveness, for the cases that start and stop sessions repeatedly.
#
# Scripted actions run as timers on the main window, so an action that never fires, or fires seconds late,
# means the UI thread was blocked: that is OBS being unresponsive, and a clean exit afterwards would hide it.
# Every start and stop also has to show up as a real output transition. A start that silently no-ops because
# the previous cycle's encoder never came free is exactly the stuck-encoder failure these cases look for, and
# it reads as a missing separator line in the log.
function pg_log_seconds {
	param($m)
	([int] $m.Groups[1].Value * 3600) + ([int] $m.Groups[2].Value * 60) + [int] $m.Groups[3].Value +
		([int] $m.Groups[4].Value / 1000.0)
}

function pg_check_cycles {
	param([string]$logText, [int]$streamStarts = 0, [int]$streamStops = 0, [int]$recStarts = 0,
		[int]$recStops = 0, [double]$latenessTolSec = 3.0, [double]$maxGraphicsStallMs = 2000,
		[double]$maxStopLatencySec = 0, [double]$fps = 0, [int]$frameAllowFrames = 80,
		[switch]$AllowRefusedStarts)

	$scheduled = @{}
	foreach ($m in [regex]::Matches($logText, '(\d{2}):(\d{2}):(\d{2})\.(\d{3}):\s*pregate: ''([^'']+)'' scheduled for t\+(\d+) s')) {
		$armedAt = pg_log_seconds $m
		$scheduled[$m.Groups[5].Value + '@' + $m.Groups[6].Value] = @($armedAt, [double]$m.Groups[6].Value)
	}

	$worst = 0.0
	$worstKey = ''
	$fired = @{}
	foreach ($m in [regex]::Matches($logText, '(\d{2}):(\d{2}):(\d{2})\.(\d{3}):\s*pregate: running ''([^'']+)'' \(t\+(\d+) s\)')) {
		$key = $m.Groups[5].Value + '@' + $m.Groups[6].Value
		$fired[$key] = $true
		if (-not $scheduled.ContainsKey($key)) { continue }
		$late = (pg_log_seconds $m) - $scheduled[$key][0] - $scheduled[$key][1]
		if ([math]::Abs($late) -gt [math]::Abs($worst)) {
			$worst = $late
			$worstKey = $key
		}
	}

	$missing = @($scheduled.Keys | Where-Object { -not $fired.ContainsKey($_) })
	pg_check ($missing.Count -eq 0) ("every scripted action ran, so the main loop stayed alive ($($scheduled.Count) scheduled, missing: $($missing -join ', '))")
	if ($scheduled.Count -gt 0) {
		pg_check ([math]::Abs($worst) -le $latenessTolSec) (
			"no action ran more than $latenessTolSec s after its timer (worst: $worstKey by $([math]::Round($worst, 2)) s)")
	}

	$seen = [ordered]@{
		'stream start' = ([regex]::Matches($logText, '==== Streaming Start ')).Count
		'stream stop'  = ([regex]::Matches($logText, '==== Streaming Stop ')).Count
		'rec start'    = ([regex]::Matches($logText, '==== Recording Start ')).Count
		'rec stop'     = ([regex]::Matches($logText, '==== Recording Stop ')).Count
	}
	$want = [ordered]@{'stream start' = $streamStarts; 'stream stop' = $streamStops; 'rec start' = $recStarts;
		'rec stop' = $recStops}
	foreach ($k in $want.Keys) {
		if ($want[$k] -le 0) { continue }
		pg_check ($seen[$k] -ge $want[$k]) ("$k happened at least $($want[$k]) time(s): the log shows $($seen[$k])")
	}

	# Refusals and forced teardowns: the fingerprints of an output or encoder that did not come free.
	$alarm = @('Failed to start output', 'is already active', 'Tried to start output', 'Tried to stop output',
		'Invalid call to obs_', 'Forcing shutdown', 'Failed to allocate memory', 'Failed to create',
		'streaming refused: the previous session', 'recording refused: the previous recording')
	$hits = @()
	foreach ($line in ($logText -split "`r`n")) {
		foreach ($p in $alarm) {
			if ($line -match $p) {
				$hits += $line.Trim()
				break
			}
		}
	}
	foreach ($h in ($hits | Select-Object -First 6)) { pg_say "  note  refused or forced: $h" }
	if (-not $AllowRefusedStarts) {
		pg_check ($hits.Count -eq 0) "no start was refused and no output was forced down through the cycles ($($hits.Count) such lines)"
	} else {
		pg_check ($true) "a refused start is allowed by this case (the cycles it counts are the ones performed)"
	}

	# How long a stop takes to come home. A fan-out session has to walk every one of its connections down;
	# measured on a healthy cycle the separator follows the request by 1.2-1.9 s. Far above that means a
	# connection is being held - and while it is held StreamingActive() still answers "running", so every
	# start issued in that window is refused and lost. That window is what t16 drives on purpose.
	if ($maxStopLatencySec -gt 0) {
		$sepAt = @{}
		foreach ($kind in @('Streaming', 'Recording')) {
			$startTimes = @()
			$stopTimes = @()
			foreach ($m in [regex]::Matches($logText, "(\d{2}):(\d{2}):(\d{2})\.(\d{3}):\s*==== $kind Start =+")) {
				$startTimes += pg_log_seconds $m
			}
			foreach ($m in [regex]::Matches($logText, "(\d{2}):(\d{2}):(\d{2})\.(\d{3}):\s*==== $kind Stop =+")) {
				$stopTimes += pg_log_seconds $m
			}
			$sepAt[$kind] = @($startTimes, $stopTimes)
		}
		$lateStops = @()
		$worstStop = 0.0
		foreach ($m in [regex]::Matches($logText, '(\d{2}):(\d{2}):(\d{2})\.(\d{3}):\s*pregate: running ''(stop|rec-stop)'' \(t\+(\d+) s\)')) {
			$kind = if ($m.Groups[5].Value -eq 'stop') { 'Streaming' } else { 'Recording' }
			$asked = pg_log_seconds $m
			$startTimes = @($sepAt[$kind][0] | Where-Object { $_ -lt $asked })
			$stopTimes = @($sepAt[$kind][1] | Where-Object { $_ -lt $asked })
			# Was anything of this kind actually running when the request came? A start that was refused
			# leaves nothing to stop, and then no separator may be demanded of the stop action that follows.
			$running = ($startTimes.Count -gt 0) -and
				(($stopTimes.Count -eq 0) -or ($startTimes[-1] -gt $stopTimes[-1]))
			if (-not $running) {
				pg_say ("  note  {0} at {1} s asked while nothing was running: nothing to bring down" -f
					$m.Groups[5].Value, $m.Groups[6].Value)
				continue
			}
			$finished = @($sepAt[$kind][1] | Where-Object { $_ -ge $asked } | Select-Object -First 1)
			if ($finished.Count -eq 0) {
				$lateStops += "$($m.Groups[5].Value) at $($m.Groups[6].Value) s never reported a finished stop"
				continue
			}
			$took = $finished[0] - $asked
			if ($took -gt $worstStop) { $worstStop = $took }
			if ($took -gt $maxStopLatencySec) {
				$lateStops += "$($m.Groups[5].Value) at $($m.Groups[6].Value) s took $([math]::Round($took, 2)) s"
			}
		}
		foreach ($s in ($lateStops | Select-Object -First 4)) { pg_say "  note  slow stop: $s" }
		pg_check ($lateStops.Count -eq 0) (
			"every stop of a session that was actually up finished within $maxStopLatencySec s of being asked (worst observed: $([math]::Round($worstStop, 2)) s)")
	}

	# Frame accounting over each output's own session window.
	#
	# The counters libobs prints at a stop belong to different lifetimes: "Total frames output: N" is
	# what one output sent, while its encoder's "Total frames drawn" spans the encoder's whole run,
	# which begins with the first consumer that started it. A recorder sharing that encoder, or a
	# destination attached later, is charged for frames drawn before it existed - comparing the two
	# counters called a healthy 2 s recording a loss of 296 of 359. The window of a stream output is
	# its session's "Streaming Start" to "Streaming Stop" separator, of the recorder the recording's,
	# and what an output should have delivered is fps times that window.
	#
	# The allowance is two measured costs summed with margin: a start reaches the ingest ~1.2 s after
	# its request (the connect), and a stop discards the ~1.2 s of video the encoder still holds -
	# 36-38 frames, whatever the session length (scratch/plan/CI-PENDING.md defects 2 and 3). Both
	# are open defects, so the allowance is a fixed frame count and the check fails on a stall, not
	# on the pipeline's normal latency: 33 delivered in a 3.2 s window is the healthy shape, 12 of
	# 10 s (the mid-GOP attach of defect 1) is not.
	pg_check_frame_windows -logText $logText -fps $fps -allowFrames $frameAllowFrames

	$g = [regex]::Match($logText, 'obs_graphics_thread\([^)]*\): min=([\d.]+) ms, median=([\d.]+) ms, max=([\d.]+) ms')
	if ($g.Success) {
		pg_say ("  note  graphics thread over the run: median {0} ms, max {1} ms (a frame period is 33.3 ms)" -f
			$g.Groups[2].Value, $g.Groups[3].Value)
		pg_check ([double]$g.Groups[3].Value -le $maxGraphicsStallMs) (
			"the graphics thread never stalled (max {0} ms over {1} ms)" -f $g.Groups[3].Value, $maxGraphicsStallMs)
	}
}

# What each output delivered across its own session window, against what that window calls for.
#
# An output's "Total frames output" counter is its own; the encoder's "Total frames drawn" spans the
# encoder's whole run, which starts with the first consumer that opened it - so a recorder sharing the
# encoder, or a destination attached later, is charged for frames drawn before it existed. The window
# of a stream output is its session's "Streaming Start"/"Streaming Stop" separators, the recorder's the
# recording's, and the number it should reach is fps times that window minus the measured latency
# allowance (a start needs ~1.2 s to reach the ingest, a stop discards ~1.2 s of held video).
function pg_check_frame_windows {
	param([string]$logText, [double]$fps = 30, [int]$allowFrames = 80)

	if ($fps -le 0) { $fps = 30 }

	# The case knows its own profile frame rate; without it a 25 fps profile would be measured against 30.
	$fm = [regex]::Match($logText, 'Output Size: \d+x\d+ (\d+(?:\.\d+)?) fps')
	if ($fm.Success) { $fps = [double]$fm.Groups[1].Value }

	$windows = New-Object System.Collections.Generic.List[object]
	$openKind = @{}
	foreach ($m in [regex]::Matches($logText,
			'(\d{2}):(\d{2}):(\d{2})\.(\d{3}):\s*==== (Streaming|Recording) (Start|Stop) =+')) {
		$t = pg_log_seconds $m
		$kind = $m.Groups[5].Value
		if ($m.Groups[6].Value -eq 'Start') {
			$w = [pscustomobject]@{Kind = $kind; Start = $t; End = $null}
			$windows.Add($w)
			$openKind[$kind] = $w
			continue
		}
		if ($openKind.ContainsKey($kind)) {
			$openKind[$kind].End = $t
			$openKind.Remove($kind)
		}
	}

	# A session ended by quitting OBS has no "Streaming Stop" separator - the counters print during the
	# teardown, so the window is still open there. Its end is the last counter line of the run.
	$lastCounter = 0.0
	foreach ($m in [regex]::Matches($logText,
			"(\d{2}):(\d{2}):(\d{2})\.(\d{3}):\s*Output '[^']+': Total frames output: \d+")) {
		$t = pg_log_seconds $m
		if ($t -gt $lastCounter) { $lastCounter = $t }
	}
	foreach ($w in @($openKind.Values)) {
		if ($lastCounter -gt $w.Start) { $w.End = $lastCounter }
	}

	if ($windows.Count -eq 0) {
		if ($lastCounter -gt 0) {
			pg_check $false 'the log carries output counters but no session separator, so the frames cannot be scored'
		} else {
			pg_say '  note  frame accounting: this log has no streaming or recording session to measure'
		}
		return
	}

	$bad = @()
	$measured = 0
	$worstLoss = 0
	$worstText = 'every run was too short to judge'

	# The two counters of a stop block belong to the run that just ended: "Total frames output" is what
	# this output sent, "Total drawn frames" what its encoder produced for it. They are written in
	# separate passes and interleave between outputs, so they are paired by output name and by index -
	# the i-th "frames output" of an output with the i-th "drawn frames" of the same output.
	#
	# A run shorter than two seconds is not judged: at 30 fps its entire frame count is inside the
	# latency allowance, and demanding 2.7 s of delivery from a 1.2 s reconnect is not a finding.
	$sent = @{}
	$drawn = @{}
	foreach ($m in [regex]::Matches($logText,
			"(\d{2}):(\d{2}):(\d{2})\.(\d{3}):\s*Output '([^']+)': Total (frames output|drawn frames): (\d+)")) {
		$t = pg_log_seconds $m
		$e = [pscustomobject]@{T = $t; N = [int]$m.Groups[7].Value}
		if ($m.Groups[6].Value -eq 'frames output') {
			if (-not $sent.ContainsKey($m.Groups[5].Value)) { $sent[$m.Groups[5].Value] = @() }
			$sent[$m.Groups[5].Value] += $e
		} else {
			if (-not $drawn.ContainsKey($m.Groups[5].Value)) { $drawn[$m.Groups[5].Value] = @() }
			$drawn[$m.Groups[5].Value] += $e
		}
	}

	foreach ($name in $drawn.Keys) {
		$dl = $drawn[$name]
		$sl = @()
		if ($sent.ContainsKey($name)) { $sl = $sent[$name] }
		for ($i = 0; $i -lt $dl.Count; $i++) {
			$t = $dl[$i].T
			$got = if ($i -lt $sl.Count) { $sl[$i].N } else { 0 }

			# The run this stop closes: the innermost window of its own kind containing the line, so a
			# recorder stopped inside a live stream is measured over its recording, not the stream.
			$w = $null
			foreach ($cand in $windows) {
				if ($null -eq $cand.End -or $cand.Start -ge $t -or $cand.End -lt $t) { continue }
				if ($null -eq $w -or ($cand.End - $cand.Start) -lt ($w.End - $w.Start)) { $w = $cand }
			}
			if ($null -eq $w) { continue }
			$span = $t - $w.Start
			if ($span -lt 2) { continue }

			$measured++
			$lost = $dl[$i].N - $got
			if ($lost -gt $worstLoss) {
				$worstLoss = $lost
				$worstText = "$name delivered $got of $($dl[$i].N) drawn over a $([math]::Round($span, 1)) s $($w.Kind) run"
			}
			if ($lost -gt $allowFrames) {
				$bad += "$name delivered $got of $($dl[$i].N) drawn over a $([math]::Round($span, 1)) s " +
					"$($w.Kind) run (lost $lost frames, allowance $allowFrames)"
			}
		}
	}

	if ($measured -eq 0) {
		pg_check $false ("the frame accounting judged none of the $($windows.Count) session windows " +
			'(every run shorter than two seconds, or no drawn-frame counter to read)')
		return
	}

	foreach ($b in ($bad | Select-Object -First 4)) { pg_say "  note  starved run: $b" }
	pg_check ($bad.Count -eq 0) (
		"$measured output runs delivered their encoder's frames (worst loss $worstLoss frames: $worstText)")
}

function pg_session {
	param([string]$Case, [string]$Actions, [int]$Destinations = 2, [int]$SharedKbps = 3000,
		[string]$OverrideOn = '', [int]$OverrideKbps = 500, [switch]$Stream, [switch]$Record,
		[switch]$Media, [switch]$Ingest, [int]$ExitTimeoutSec = 150, [string[]]$ExpectInLog = @(),
		[int]$ExpectLive = 0, [string]$Server = 'rtmp://127.0.0.1:1935/live', [switch]$Reconnect = $true,
		[int]$LiveTimeoutSec = 30, [int]$ExpectStreamStarts = 0, [int]$ExpectStreamStops = 0,
		[int]$ExpectRecStarts = 0, [int]$ExpectRecStops = 0, [double]$MaxStopLatencySec = 0,
		[switch]$AllowRefusedStarts)

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

	# Every session case, not just the cycle cases: the actions were scheduled on the main window, so seeing
	# them all run near their time is the liveness proof; the cycle counts only mean something when given.
	pg_check_cycles -logText $text -streamStarts $ExpectStreamStarts -streamStops $ExpectStreamStops `
		-recStarts $ExpectRecStarts -recStops $ExpectRecStops -maxStopLatencySec $MaxStopLatencySec `
		-AllowRefusedStarts:$AllowRefusedStarts

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
