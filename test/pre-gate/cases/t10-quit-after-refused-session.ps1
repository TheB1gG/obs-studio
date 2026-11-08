# t10 - quit after a session that was refused by the ingest (nothing listening, reconnect off). This is the
# state every earlier harness left OBS in while believing it had tested a session: the connections are gone,
# the objects of the fan-out are not necessarily. A destination output holding one reference too many
# survives its session here and gets destroyed by obs_free_data() after the encoder plugins were unloaded.
. (Join-Path $PSScriptRoot '..\lib\pregate.ps1')
pg_kill_strays
pg_session -Case 't10-quit-after-refused-session' -Destinations 2 -Stream `
	-Server 'rtmp://127.0.0.1:1936/live' -Reconnect:$false `
	-Actions 'quit@22' `
	-ExpectInLog @('streaming session: \d+ additional destination output',
		'Streaming Stop')
pg_verdict 't10-quit-after-refused-session'
exit $global:PGCODE
