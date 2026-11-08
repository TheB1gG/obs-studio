# Pre-gate suite

Run this **after a local build and before any `test-*` tag exists** (AGENTS.md section 3).

```
pwsh -NoProfile -File test/pre-gate/run.ps1                  # everything (~15 min)
pwsh -NoProfile -File test/pre-gate/run.ps1 -NoMeasure       # lifecycle only, the fast half
pwsh -NoProfile -File test/pre-gate/run.ps1 -Cases t03-quit-while-streaming-two
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
| `t01-cold-quit` | idle | the baseline: nothing running, still shuts down clean |
| `t02-quit-while-streaming-one` | one destination uploading | the handler's own output and encoders are released in time |
| `t03-quit-while-streaming-two` | two destinations uploading | the fan-out's extra outputs: the crash this suite was written for |
| `t04-quit-while-recording` | recording and streaming | the recording encoder set and muxer in the same teardown |
| `t05-stream-stop-then-start` | second session uploading | the fan-out rebuilt after a stop (four `is live` lines) |
| `t06-restart-one-destination` | one restarted, one never dropped | single-destination kill/restart inside a live session |
| `t10-quit-after-refused-session` | session ended (refused, no reconnect) | the objects of a finished session must not linger either |
| `t11-quit-while-reconnecting` | retry timers armed | shutdown in the middle of a reconnect |
| `t07-two-destinations-one-encoder` | two uploading | both keys really reach the ingest, both cost the profile bitrate |
| `t08-backup-own-bitrate` | two uploading | the destination with an override pays for its own encoder, the other keeps the shared one |
| `t09-primary-own-bitrate` | two uploading | the shared encoder is not retuned when the primary overrides its bitrate |

## Environment

Nothing outside the rundir and `scratch/` is touched: the runs use `--portable`, fresh
`pregate-<case>` profiles and collections written from `fixtures/`.

| variable | meaning |
|---|---|
| `OBS_PREGATE_ACTIONS` (read by OBS) | `<action>@<seconds>` list, fired by the frontend hook in `OBSBasic::OBSInit`: `start`, `stop`, `quit` (a real window close), `restart-dest:<id>`, `stop-dest:<id>`, `kill-dest:<id>` |
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
