# t08 - the extra destination asks for a bitrate of its own: the profile encoder stays at its own rate and
# the backup connection is measured at the override, which is only possible if it got its own encoder.
. (Join-Path $PSScriptRoot '..\lib\pregate.ps1')
pg_kill_strays
pg_bitrate_case -Case 't08-backup-own-bitrate' -Destinations 2 -SharedKbps 3000 -OverrideOn 'b' `
	-OverrideKbps 800 -SampleSeconds 15
pg_verdict 't08-backup-own-bitrate'
exit $global:PGCODE
