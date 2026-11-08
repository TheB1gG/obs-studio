# t09 - the override sits on the destination that owns the profile session: the primary path has to drop to
# the override rate while the second destination keeps costing the profile bitrate, because the shared
# encoder is deliberately not retuned for one destination.
. (Join-Path $PSScriptRoot '..\lib\pregate.ps1')
pg_kill_strays
pg_bitrate_case -Case 't09-primary-own-bitrate' -Destinations 2 -SharedKbps 3000 -OverrideOn 'a' `
	-OverrideKbps 800 -SampleSeconds 15
pg_verdict 't09-primary-own-bitrate'
exit $global:PGCODE
