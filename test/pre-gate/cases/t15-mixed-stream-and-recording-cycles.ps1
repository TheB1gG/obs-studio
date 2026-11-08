# t15 - both at once, cycling: the stream and the recorder stopped and restarted in overlapping pairs, with a
# two-destination fan-out uploading. This is the harshest shape the start/stop path sees - encoder sets, the
# muxer and the fan-out all going down and coming back close to each other - and it has to end with OBS still
# responsive enough to close itself.
#
# The pairs are three seconds apart, not two: at two seconds the stop of one cycle is still pending when the
# next start comes, that start is refused, and t16 is the case that drives that window on purpose.
#
# STATUS 2026-11-09: RED at either spacing, and that is the finding, not a flake. With the starvation check
# repaired (it had never matched a two-output session) the recorder delivers 20 of 218 frames here while the
# two RTMP outputs on the SAME shared encoder take 181 of 217: the muxer output attaches to an encoder that is
# already running, and libobs discards every packet until the next keyframe - keyint 250 at 30 fps, so up to
# 8.3 s of nothing. Root cause, evidence and the fix plan are in scratch/plan/CI-PENDING.md.
. (Join-Path $PSScriptRoot '..\lib\pregate.ps1')
pg_kill_strays
pg_session -Case 't15-mixed-stream-and-recording-cycles' -Destinations 2 -Stream -Record -Ingest `
	-Actions 'stop@15,rec-stop@18,start@21,rec-start@24,stop@27,rec-stop@30,start@33,rec-start@36,stop@39,rec-stop@42,start@45,rec-start@48,quit@58' `
	-ExitTimeoutSec 90 -ExpectLive 2 -ExpectStreamStarts 4 -ExpectStreamStops 3 -ExpectRecStarts 4 -ExpectRecStops 3
pg_verdict 't15-mixed-stream-and-recording-cycles'
exit $global:PGCODE
