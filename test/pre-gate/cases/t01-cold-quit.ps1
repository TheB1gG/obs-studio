# t01 - a cold start followed by a real quit: the cheapest run that still proves the shutdown path works,
# and the baseline every later session case is compared against.
. (Join-Path $PSScriptRoot '..\lib\pregate.ps1')
pg_kill_strays
pg_session -Case 't01-cold-quit' -Destinations 2 -Actions 'quit@20'
pg_verdict 't01-cold-quit'
exit $global:PGCODE
