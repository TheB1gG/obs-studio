# t03 - the regression case: quit while a two-destination session is live. The extra destination output
# belongs to the streaming engine, not to the output handler that libobs tears down, and quitting OBS
# with it still connected crashed in the encoder / core video-mix teardown.
. (Join-Path $PSScriptRoot '..\lib\pregate.ps1')
pg_kill_strays
pg_session -Case 't03-quit-while-streaming-two' -Destinations 2 -Stream -Ingest `
	-Actions 'quit@26' -ExpectLive 2 `
	-ExpectInLog @('Starting stream due to command line parameter',
		'streaming session: \d+ additional destination output')
pg_verdict 't03-quit-while-streaming-two'
exit $global:PGCODE
