# t16 - the restart that lands inside a stop that is still winding down, at two-second spacing.
#
# A user cannot reach this rate through the UI: the Start/Stop button stays on "Stopping" until the session is
# really down. The case drives it from script on purpose, because the window is where the lifetime questions
# live (a destination output whose stop is still pending while the next session is being built is the shape
# the audio-header shutdown race needs too).
#
# What it pins down:
#  - every stop asked for finishes by itself and quickly: 1.26-1.33 s measured here, 1.2-1.9 s on a healthy
#    cycle. The window a held connection used to open was 2.6 s, 2.8 s, 3.0 s and 6.2 s;
#  - a start that lands inside that window is deferred by the frontend and served when the session is over,
#    never dropped: pg_check_cycles asserts the pair "start streaming deferred" / "serving the streaming
#    start" for every case, so a request that vanishes fails a run anywhere, not only here;
#  - the cycles that were performed are all accounted for, nothing is left attached to a dead output, no
#    output is starved, and OBS still closes itself cleanly with no object still registered.
#
# STATUS 2026-11-10: GREEN, 6 of 6 runs, with no refused start at this spacing - so the case no longer allows
# one, and the expectations are the exact counts of the script: 4 starts for 4 requests, 3 stops for 3
# requests. The frame deficit is the start window, not a loss inside a run: the worst is a constant 66-67
# frames (~2.2 s at 30 fps - the connect plus the muxer spawn after a request) in runs of 3.2 s, where that
# leaves 31-32 of 98 drawn, and in runs of 9.1 s, where it leaves 236-237 of 302-303. Same constant, so the
# 80-frame allowance is the right one and a short session is not evidence of starvation.
# What this case used to reproduce - one packet of 115 drawn delivered to all three outputs of a cycle, and a
# run where OBS hung with nine later scripted actions lost - was the UI-thread join and the keyframe gate:
# fixed by c94fad910 (stop/abort without joining) and 7d92d092b (a keyframe for a consumer that attaches
# mid-session). Reproduce the old shape with scratch/tmp/pg-t16-repeat.ps1 -Runs 6 -Tag <name>.
. (Join-Path $PSScriptRoot '..\lib\pregate.ps1')
pg_kill_strays
pg_session -Case 't16-restart-inside-a-pending-stop' -Destinations 2 -Stream -Record -Ingest `
	-Actions 'stop@14,rec-stop@15,start@16,rec-start@17,stop@18,rec-stop@19,start@20,rec-start@21,stop@22,rec-stop@23,start@24,rec-start@25,quit@34' `
	-ExpectLive 2 -ExpectStreamStarts 4 -ExpectStreamStops 3 -ExpectRecStarts 4 -ExpectRecStops 3 `
	-MaxStopLatencySec 3.0
pg_verdict 't16-restart-inside-a-pending-stop'
exit $global:PGCODE
