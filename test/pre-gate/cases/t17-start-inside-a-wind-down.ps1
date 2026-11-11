# t17 - a start requested one second after a stop, so it lands inside the wind-down of the session it replaces.
#
# t16 cycles at two seconds, which is what the UI allows; the measured stop of a fan-out session is 1.26-1.33 s
# (2.6-6.2 s when a connection is held), so at that spacing every start finds the previous session already
# gone. One second is the window itself, and this case drives it on purpose.
#
# This is the positive test of the deferral: a start that lands in the window is refused by libobs (the output
# is still in use) and the frontend used to drop the request, which is how a user lost a session with no
# feedback. It now defers the request and serves it when the session is over, so the case asserts:
#  - the log shows the deferral: a case that never entered the window is a hole in coverage, so a count of zero
#    fails here, through -ExpectDeferredStarts;
#  - the deferral is served: the pair "start streaming deferred" / "serving the streaming start" is checked for
#    every case by pg_check_cycles, and here the first line has to be there at all;
#  - the session it asked for comes up - 'is live' for both destinations of both sessions - and its frames
#    arrive over its own window;
#  - the stopped-into session ends cleanly and OBS quits with nothing left registered.
#
# Two failure modes this case found, in order, both in the shape "serve the request when the session's stop
# signal arrives":
#  1. the start invoked from the stop signal re-entered StartStreaming inside the ~0.35 s for which the handler
#     still answers StreamingActive() (Q13 in StreamingStop() records the same lag from the other side) and
#     refused its own request: "start streaming refused: a streaming session is already running", logged in the
#     same millisecond as the serve;
#  2. letting the flag go and starting on the record alone starts the session too early - libobs has emitted
#     the stop signal but is still taking the output and its encoders down, so SetupStreaming re-attaches an
#     active encoder: "Cannot apply a new video_t object while the encoder is active", "tried to set audio
#     encoder 0 on output \"adv_stream\" while the output is still active!", and then "Stream output type
#     'rtmp_output' failed to start!". The full-gate run at 21:12:26 is that one: deferred, served, dead.
# What the code does now: StartStreaming defers, and ServeDeferredStreamStart() runs the request only when the
# handler's flag and the session's runtime record are both free - retried on a QTimer 100 ms apart, bounded at
# 8 s, dropped with a warning past that. Measured with it: the stop of the 2-destination session takes 1.25 s,
# the served start is re-armed a few times and the new session is live inside ~0.5 s of the stop signal; the
# local ingest answers instantly once the encoders are free. STATUS 2026-11-10: GREEN - 4 runs of t17 with t16
# (scratch/tmp/pregate/loop-t17timer.txt); the deferral count is asserted, so a run that never entered the
# window fails this case as a coverage hole, and an unserved deferral fails every case.
. (Join-Path $PSScriptRoot '..\lib\pregate.ps1')
pg_kill_strays
pg_session -Case 't17-start-inside-a-wind-down' -Destinations 2 -Stream -Ingest `
	-Actions 'stop@14,start@15,stop@25,quit@30' `
	-ExpectLive 4 -ExpectStreamStarts 2 -ExpectStreamStops 2 `
	-MaxStopLatencySec 3.0 -ExpectDeferredStarts 1
pg_verdict 't17-start-inside-a-wind-down'
exit $global:PGCODE
