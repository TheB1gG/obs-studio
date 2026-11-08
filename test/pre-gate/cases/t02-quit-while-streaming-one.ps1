# t02 - quit while a one-destination session is still live. The first case in which the shutdown of an
# active output is exercised at all: a force-killed process never runs obs_shutdown.
. (Join-Path $PSScriptRoot '..\lib\pregate.ps1')
pg_kill_strays
pg_session -Case 't02-quit-while-streaming-one' -Destinations 1 -Stream -Ingest `
	-Actions 'quit@26' -ExpectLive 1 `
	-ExpectInLog @('Starting stream due to command line parameter')
pg_verdict 't02-quit-while-streaming-one'
exit $global:PGCODE
