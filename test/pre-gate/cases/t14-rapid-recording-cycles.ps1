# t14 - cycle the recording the same way: four stop/start pairs on the recorder, two seconds apart, no stream
# involved. The muxer and the recording encoder set are torn down and rebuilt four times, and the file output
# has to be finished before the next one starts.
. (Join-Path $PSScriptRoot '..\lib\pregate.ps1')
pg_kill_strays
pg_session -Case 't14-rapid-recording-cycles' -Destinations 1 -Record `
	-Actions 'rec-stop@10,rec-start@12,rec-stop@14,rec-start@16,rec-stop@18,rec-start@20,rec-stop@22,rec-start@24,quit@32' `
	-ExpectRecStarts 5 -ExpectRecStops 4
pg_verdict 't14-rapid-recording-cycles'
exit $global:PGCODE
