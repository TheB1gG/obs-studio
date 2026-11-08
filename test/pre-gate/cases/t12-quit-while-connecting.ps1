# t12 - quit while a connect attempt is still in flight. The server is a black hole: it never answers, so the
# connect neither succeeds nor fails quickly and the output is still in the connect phase when OBS is asked
# to close. That is the state where the RTMP output's teardown has to decide about the send thread.
. (Join-Path $PSScriptRoot '..\lib\pregate.ps1')
pg_kill_strays
pg_session -Case 't12-quit-while-connecting' -Destinations 2 -Stream `
	-Server 'rtmp://10.255.255.1:1935/live' `
	-Actions 'quit@22' -ExitTimeoutSec 90 `
	-ExpectInLog @('Connecting to RTMP URL rtmp://10\.255\.255\.1')
pg_verdict 't12-quit-while-connecting'
exit $global:PGCODE
