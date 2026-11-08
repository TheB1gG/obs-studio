# t04 - quit while recording and streaming at the same time: recording has its own encoder set, and the
# muxer is yet another object that has to be closed before libobs pulls the context out from under it.
. (Join-Path $PSScriptRoot '..\lib\pregate.ps1')
pg_kill_strays
pg_session -Case 't04-quit-while-recording' -Destinations 2 -Stream -Record -Ingest `
	-Actions 'quit@26' -ExpectLive 2 `
	-ExpectInLog @('Starting stream due to command line parameter',
		'Starting recording due to command line parameter')
pg_verdict 't04-quit-while-recording'
exit $global:PGCODE
