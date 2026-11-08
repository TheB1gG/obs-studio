# t16 - the restart that lands inside a stop that is still winding down, at two-second spacing.
#
# A user cannot reach this rate through the UI: the Start/Stop button stays on "Stopping" until the session is
# really down. The case drives it from script on purpose, because the window is where the lifetime questions
# live (a destination output whose stop is still pending while the next session is being built is the shape
# the audio-header shutdown race needs too).
#
# What it pins down is the contract that has to hold even for a request that gets refused:
#  - every stop asked for finishes by itself and quickly: 1.2 s on a healthy cycle, and the window a held
#    connection opens was measured at 2.6 s, 2.8 s, 3.0 s and 6.2 s - inside it, a start is refused and lost;
#  - a start issued inside the window is refused *in the log*, not silently: the request is lost either way,
#    which is why "start streaming refused: ..." exists in StartStreaming();
#  - the cycles that were performed are all accounted for, nothing is left attached to a dead output, no
#    output is starved, and OBS still closes itself cleanly with no object still registered.
# The counts below are therefore the sessions that actually ran, and the refusal is allowed here only - in
# t13-t15, which cycle at rates the UI does allow, a refused start stays a failure.
#
# STATUS 2026-11-09: RED, on starvation and on stop latency. Reproduced on every run at this spacing: one
# packet delivered of 115 drawn to *all three* outputs of a cycle (encoder producing 30 fps, outputs receiving
# nothing), and one run where OBS hung completely - the log stops at a refusal line and nine later scripted
# actions including the quit never fire, with no crash report and the process alive until the harness killed
# it. Root cause of the stall, evidence and the fix plan: scratch/plan/CI-PENDING.md.
. (Join-Path $PSScriptRoot '..\lib\pregate.ps1')
pg_kill_strays
pg_session -Case 't16-restart-inside-a-pending-stop' -Destinations 2 -Stream -Record -Ingest `
	-Actions 'stop@14,rec-stop@15,start@16,rec-start@17,stop@18,rec-stop@19,start@20,rec-start@21,stop@22,rec-stop@23,start@24,rec-start@25,quit@34' `
	-ExpectLive 2 -ExpectStreamStarts 3 -ExpectStreamStops 2 -ExpectRecStarts 4 -ExpectRecStops 3 `
	-MaxStopLatencySec 3.0 -AllowRefusedStarts
pg_verdict 't16-restart-inside-a-pending-stop'
exit $global:PGCODE
