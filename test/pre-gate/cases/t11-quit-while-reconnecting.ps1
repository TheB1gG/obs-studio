# t11 - quit while the session is still trying to reconnect: the outputs are registered, their retry timers
# are armed, and the app is going down in the middle of that. Different teardown path from a live session
# (t03) and from a finished one (t10), so it is its own case.
. (Join-Path $PSScriptRoot '..\lib\pregate.ps1')
pg_kill_strays
pg_session -Case 't11-quit-while-reconnecting' -Destinations 2 -Stream `
	-Server 'rtmp://127.0.0.1:1936/live' `
	-Actions 'quit@20' `
	-ExpectInLog @('streaming session: \d+ additional destination output')
pg_verdict 't11-quit-while-reconnecting'
exit $global:PGCODE
