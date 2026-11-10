# Pre-gate suite

Run this **after a local build and before any `test-*` tag exists** (AGENTS.md section 3).

```
pwsh -NoProfile -File test/pre-gate/run.ps1                  # everything (~15 min)
pwsh -NoProfile -File test/pre-gate/run.ps1 -NoMeasure       # lifecycle only, the fast half
pwsh -NoProfile -File test/pre-gate/run.ps1 -Cases t03-quit-while-streaming-two
pwsh -NoProfile -File test/pre-gate/run.ps1 -Cases t15,t16   # an unambiguous prefix names a case
pwsh -NoProfile -File test/pre-gate/run.ps1 -Base 727a470f2  # gate a batch that is already committed
pwsh -NoProfile -File test/pre-gate/run.ps1 -Strict          # a SKIP counts as a failure
pwsh -NoProfile -File test/pre-gate/run.ps1 -Background      # return at once, follow the transcript
```

`s01-static` reads the C/C++ files **the batch changed**, so a committed batch has to be named: with the
working tree clean and the default `-Base HEAD` it reports SKIP, and `-Strict` turns that into a failure
rather than letting a vacuous green pass for the gate.

Exit code 0 = green. A transcript of every run is written to `scratch/tmp/pregate/run-<time>.txt`,
and each case's own stdout to `scratch/tmp/pregate/<case>.out.txt`. The suite is quiet when it is
happy, noisy when it is not: read the failing case's block, not just the summary.

The suite takes several minutes; start it detached (`Start-Process pwsh -ArgumentList ...`) and poll
the transcript rather than blocking a shell on it.

## Why this exists

Every harness used while building the streaming-destination feature ended its run by terminating the
OBS process. A killed process never runs `obs_shutdown()`, so the teardown of a session was never
executed by any "verification" step - and quitting OBS with a multi-destination session connected
crashed in libobs. A CI tag run cannot see this either: no CI job ever starts a stream.

## What a case asserts

`pg_check_shutdown` (in `lib/pregate.ps1`) runs on every OBS case:

| check | why it exists |
|---|---|
| OBS quit **by itself** | the shutdown path is what is under test; a case that has to kill OBS did not run it |
| exit code 0 | a crash handler exit or a hang is not a clean shutdown |
| no new crash report | `config/obs-studio/crashes/` is compared before/after |
| the pregate hook ran | otherwise the environment never armed the script and nothing happened |
| `Freeing OBS context data` | proof that `obs_free_data()` was reached |
| nothing stopped after that line | an output that was still registered when libobs began freeing its data |
| no `N output(s)/encoder(s)/service(s) were remaining` | libobs' own count of objects the frontend still held; catches a leaked reference whether or not it crashes |

A case that means to shut a **running** session down first proves it is running: `pg_wait_live` reads
the growing log and demands `is live` for the expected number of destinations. "Connected and
uploading", "connection refused", and "still reconnecting" are different teardown paths, so they are
separate cases and only the first one has a live gate.

## Cases

| case | state at quit | what it pins down |
|---|---|---|
| `s01-static` | - | final newline (Apple clang `-Wnewline-eof`, errors in CI), clang-format 22 attributed to this batch rather than to repo debt, no `TEMP-`/`OBS_TEST_` probe left in program code |
| `s02-accounting-selftest` | - | the frame accounting is proven to bite: the defect-1 shape is rejected, a healthy stop tail and a 1.2 s reconnect run are not, counters without session separators are reported |
| `t01-cold-quit` | idle | the baseline: nothing running, still shuts down clean |
| `t02-quit-while-streaming-one` | one destination uploading | the handler's own output and encoders are released in time |
| `t03-quit-while-streaming-two` | two destinations uploading | the fan-out's extra outputs: the crash this suite was written for |
| `t04-quit-while-recording` | recording and streaming | the recording encoder set and muxer in the same teardown |
| `t05-stream-stop-then-start` | second session uploading | the fan-out rebuilt after a stop (four `is live` lines) |
| `t06-restart-one-destination` | one restarted, one never dropped | single-destination kill/restart inside a live session |
| `t10-quit-after-refused-session` | session ended (refused, no reconnect) | the objects of a finished session must not linger either |
| `t11-quit-while-reconnecting` | retry timers armed | shutdown in the middle of a reconnect |
| `t12-quit-while-connecting` | connect never answered | shutdown while an output is still in the connect phase, before any header was sent |
| `t13-rapid-stream-cycles` | last session stopped, then quit | four stop/start pairs two seconds apart: every cycle really starts and really stops, nothing is refused |
| `t14-rapid-recording-cycles` | recorder restarted four times | the muxer and the recording encoders rebuilt under the same rapid cycling |
| `t15-mixed-stream-and-recording-cycles` | both cycled, three seconds apart | stream and recorder going down and up close to each other on top of a live fan-out |
| `t16-restart-inside-a-pending-stop` | the same at two seconds, a start landing inside a stop | the wind-down window: stops still come home, a refused start is logged rather than lost, nothing stays attached to a dead output |
| `t07-two-destinations-one-encoder` | two uploading | both keys really reach the ingest, both cost the profile bitrate |
| `t08-backup-own-bitrate` | two uploading | the destination with an override pays for its own encoder, the other keeps the shared one |
| `t09-primary-own-bitrate` | two uploading | the shared encoder is not retuned when the primary overrides its bitrate |

The cycle cases (`t13`-`t16`) add three checks on top of the shutdown ones, because a clean exit proves
nothing about the cycling itself: every scripted action has to be seen running and within three seconds of
its timer (the actions are timers on the main window, so a late or missing one is the log record of an
unresponsive UI thread); every start and stop has to appear in the log as a real output transition, which is
what a silently no-opping restart would cost; and no output may end up starved - frames drawn for it but
almost none delivered, the shape an encoder or send path that is being held leaves behind.

The frame accounting (`pg_check_frame_windows`) is the check that pins defect 1, and it scores each
output **run** by comparing the two counters of the stop that closed it: `Total frames output` (what the
output sent) against `Total drawn frames` (what its encoder produced for it). A run that drew frames and
delivered hardly any is the signature of a consumer attached to a running encoder, which is what the
mid-GOP keyframe gate used to do.

Three measurements make the comparison honest, all of them from this suite's own logs:
- The two counters are written in **separate passes and interleave between outputs**, so pairing them by
  line adjacency matches nothing as soon as a session has two outputs - which is how a 1-of-144 delivery
  hid in plain sight for a day. They are paired by output name and by index instead.
- The encoder's "drawn" counter spans the *encoder*, which starts with the first consumer that opened it.
  A recorder that shares that encoder, or a destination attached later, is charged for frames drawn before
  it existed, so a healthy 2 s recording read as 63 of 359 lost. This is why the loss is scored per run of
  one output, and the run's window (the `Streaming Start`/`Stop` separators, `Recording Start`/`Stop` for
  the recorder) only decides whether the run is long enough to judge.
- The gap between the two counters is a **start window**, not a loss at the stop. `Total drawn frames`
  starts counting at the start *request* (libobs snapshots the global frame counter in `obs_output_start`),
  while an output only starts sending when its session is up: ~1.2 s to reach the ingest, ~1.2 s to spawn the
  muxer process, and at launch the time until the graphics thread exists. The stop tail is intact - the ffmpeg
  muxer gates the close on the packet's *capture* timestamp (`sys_dts_usec`, which libobs sets from the
  packet's own clock, not its delivery time), so the file ends at the instant Stop was pressed. The containers
  this suite recorded hold exactly what the log claims: a 2.0 s run holds 60 frames, and the run whose log
  reads "delivered 181 of 219" holds 181 video packets. The deficit is a constant 36-39 frames in runs of
  3.2 s and of 24.3 s alike, which is what a fixed window looks like and not what a proportional loss does.
- The allowance is therefore 80 frames (~2.7 s of start window at 30 fps) and the shape the check exists to
  catch stays far outside it: defect 1 delivered 12 frames of a 10 s run. A run shorter than two seconds is
  not judged at all - its whole frame count is inside the allowance, so scoring a 1.2 s reconnect run against
  a 2.7 s demand is not a finding. The one measured case that sits near the allowance, 66 frames (32 of 98
  over a 3.2 s run), is `t16`: a restart issued inside a stop that has not finished. That overlap, not the
  stop tail, is the open half of defect 2.

`s02-accounting-selftest` runs on synthetic logs and asserts both directions of that check: the defect-1
shape is rejected, a healthy stop tail and a short reconnect run are not, and a log whose counters have no
session separator is reported instead of scored as nothing. It exists because a check that measures nothing
passes - which is exactly what the first version of this accounting did, and OBS keeps only the ten newest
logs, so the pre-fix evidence that would have caught it rotates out of the rundir within a suite run.

## Environment

Nothing outside the rundir and `scratch/` is touched: the runs use `--portable`, fresh
`pregate-<case>` profiles and collections written from `fixtures/`.

| variable | meaning |
|---|---|
| `OBS_PREGATE_ACTIONS` (read by OBS) | `<action>@<seconds>` list, fired by the frontend hook in `OBSBasic::OBSInit`: `start`, `stop`, `rec-start`, `rec-stop`, `quit` (a real window close), `restart-dest:<id>`, `stop-dest:<id>`, `kill-dest:<id>` |
| `OBS_PREGATE_MEDIAMTX` | path of the ingest binary (default `E:\Downloads\mediamtx_v1.21.0_windows_amd64\mediamtx.exe`) |
| `OBS_PREGATE_MEDIA` | a sample video for the cases that need moving content |
| `OBS_PREGATE_CLANGFORMAT` | clang-format to use, when it is not on PATH |
| `PREGATE_BASE` | what `s01-static` compares against (the runner passes `-Base`) |

Cases that need a missing tool **SKIP** and say so; a gate may not be tagged over a SKIP without the
CI-PENDING entry naming it, and `-Strict` turns skips into failures for exactly that check.

## Adding a case

Write `cases/tNN-<name>.ps1`: dot-source `../lib/pregate.ps1`, call `pg_kill_strays`, then
`pg_session` (a run) or `pg_bitrate_case` (a measured run), then `pg_verdict '<name>'` and
`exit $global:PGCODE`. Give a streaming case `-ExpectLive <n>` so it fails loudly when the session
never went live - a case that streamed into nothing tests nothing. Name the case for the state OBS
is in when it is asked to quit.
