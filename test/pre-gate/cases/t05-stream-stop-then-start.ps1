# t05 - stop the session, start it again, then quit: the fan-out has to be rebuilt from scratch, which is
# where encoder attachment and per-destination state used to go wrong. Four "is live" lines means both
# destinations came up in both sessions and the second session is the one that gets shut down.
. (Join-Path $PSScriptRoot '..\lib\pregate.ps1')
pg_kill_strays
pg_session -Case 't05-stream-stop-then-start' -Destinations 2 -Stream -Ingest `
	-Actions 'stop@24,start@32,quit@44' -ExpectLive 4 -LiveTimeoutSec 40
pg_verdict 't05-stream-stop-then-start'
exit $global:PGCODE
