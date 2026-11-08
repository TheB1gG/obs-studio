# t06 - restart one destination while the other keeps streaming. Three "is live" lines means the session
# came up with two destinations, the killed one came back, and the quit afterwards still shut down clean.
. (Join-Path $PSScriptRoot '..\lib\pregate.ps1')
pg_kill_strays
pg_session -Case 't06-restart-one-destination' -Destinations 2 -Stream -Ingest `
	-Actions 'kill-dest:b@20,restart-dest:b@26,quit@38' -ExpectLive 3
pg_verdict 't06-restart-one-destination'
exit $global:PGCODE
