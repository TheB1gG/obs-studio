# t13 - cycle the stream itself: four stop/start pairs two seconds apart on top of the session the command
# line started. Rapid cycling is where an encoder or an output that never came free shows up: the next start
# is then refused, or silently no-ops, and the log shows fewer transitions than there were actions. The case
# also asserts every timer fired near its time, which is the proof the UI thread was not blocked.
. (Join-Path $PSScriptRoot '..\lib\pregate.ps1')
pg_kill_strays
pg_session -Case 't13-rapid-stream-cycles' -Destinations 2 -Stream -Ingest `
	-Actions 'stop@12,start@14,stop@16,start@18,stop@20,start@22,stop@24,start@26,quit@34' `
	-ExpectLive 1 -ExpectStreamStarts 5 -ExpectStreamStops 4
pg_verdict 't13-rapid-stream-cycles'
exit $global:PGCODE
