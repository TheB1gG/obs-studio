# The frame accounting is the check that pins defect 1, so the suite has to prove it bites: a check that
# measures nothing passes vacuously, and this one did for a day (it paired libobs' two counters by line
# adjacency, which matches nothing in a fan-out, and scored a 1-frame reconnect print against the whole
# session window).  OBS keeps only the ten newest logs, so the pre-fix evidence rotates out - synthetic
# logs are the only way to pin the red path.  No OBS here, so it runs in under a second.
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot '..\lib\pregate.ps1')

pg_kill_strays

$hdr = '00:00:01.000: Core Video: Output Size: 1280x720 30 fps'

$healthy = @"
$hdr
00:00:02.000: ==== Streaming Start ===============================================
00:00:12.000: Output 'adv_stream': Total frames output: 265
00:00:12.000: Output 'b': Total frames output: 265
00:00:12.000: Output 'adv_stream': Total drawn frames: 300
00:00:12.000: Output 'b': Total drawn frames: 300
00:00:12.100: ==== Streaming Stop ================================================
"@

$starved = @"
$hdr
00:00:02.000: ==== Streaming Start ===============================================
00:00:12.000: Output 'adv_stream': Total frames output: 265
00:00:12.000: Output 'b': Total frames output: 12
00:00:12.000: Output 'adv_stream': Total drawn frames: 300
00:00:12.000: Output 'b': Total drawn frames: 300
00:00:12.100: ==== Streaming Stop ================================================
"@

$reconnect = @"
$hdr
00:00:02.000: ==== Streaming Start ===============================================
00:00:03.200: Output 'b': Total frames output: 1
00:00:03.200: Output 'b': Total drawn frames: 40
00:00:12.000: Output 'adv_stream': Total frames output: 265
00:00:12.000: Output 'b': Total frames output: 265
00:00:12.000: Output 'adv_stream': Total drawn frames: 300
00:00:12.000: Output 'b': Total drawn frames: 300
00:00:12.100: ==== Streaming Stop ================================================
"@

$nested = @"
$hdr
00:00:02.000: ==== Streaming Start ===============================================
00:00:06.000: ==== Recording Start ===============================================
00:00:10.000: Output 'adv_file_output': Total frames output: 110
00:00:10.000: Output 'adv_file_output': Total drawn frames: 120
00:00:10.100: ==== Recording Stop ================================================
00:00:12.000: Output 'adv_stream': Total frames output: 265
00:00:12.000: Output 'adv_stream': Total drawn frames: 300
00:00:12.100: ==== Streaming Stop ================================================
"@

$nosep = @"
$hdr
00:00:12.000: Output 'adv_stream': Total frames output: 290
00:00:12.000: Output 'adv_stream': Total drawn frames: 300
"@

function pg_accounting_fails([string]$text) {
	$script:pgFails = @()
	# The check writes its own lines; collecting them keeps the fixture's report out of the result and
	# stops the function from returning the whole output as an array (PowerShell's -eq on an array is a
	# filter, which is how this line ended up handing an Object[] to a [bool] parameter).
	$null = pg_check_frame_windows -logText $text -fps 30
	return $script:pgFails.Count
}

# Each fixture's expected failures must not land in the case's own verdict: the case reports the
# assertions below, not the failures the fixtures are designed to produce.
$caseFails = @()
foreach ($fixture in @(
		@('a healthy stop tail is accepted', $healthy, 0),
		@('the defect-1 shape - frames drawn and almost none delivered - is rejected', $starved, 1),
		@('a 1.2 s reconnect run is not judged, the session beside it is', $reconnect, 0),
		@('a recorder stopped inside a live stream is scored over its own recording', $nested, 0),
		@('counters with no session separator are reported, not scored as nothing', $nosep, 1))) {
	$got = pg_accounting_fails $fixture[1]
	$want = $fixture[2]
	if ($got -eq $want) {
		pg_say "  ok    $($fixture[0]) (rejected with $got check failure(s), as intended)"
	} else {
		$msg = "{0} (got $got check failure(s), want $want)" -f $fixture[0]
		pg_say "  FAIL  $msg"
		$caseFails += $msg
	}
}
$script:pgFails = $caseFails

pg_verdict 's02-accounting-selftest'
exit $global:PGCODE
