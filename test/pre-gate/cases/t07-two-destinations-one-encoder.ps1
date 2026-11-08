# t07 - two destinations, two different stream keys, one ingest: both paths must really be published, at
# the profile rate, because that is what the shared encoder guarantees.
. (Join-Path $PSScriptRoot '..\lib\pregate.ps1')
pg_kill_strays
pg_bitrate_case -Case 't07-two-destinations-one-encoder' -Destinations 2 -SharedKbps 1500 -SampleSeconds 15
pg_verdict 't07-two-destinations-one-encoder'
exit $global:PGCODE
