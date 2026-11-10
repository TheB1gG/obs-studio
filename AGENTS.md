# AGENTS.md

Operating rules for AI agents (and humans) working in this repository.

## Repository context

- This is a fork of [obsproject/obs-studio](https://github.com/obsproject/obs-studio).
- Active development branch: `31.1.2-features`; version tags follow `31.1.2-betaN`.
- Local builds use CMake in `build_x64/` (RelWithDebInfo) and deploy to `build_x64/rundir/`.

## 1. Commit message format (mandatory)

Every commit must let a future reader (human or AI) understand what was broken,
why, and how it was fixed. Use this structure:

```
<area>: <imperative summary of the change>

PROBLEM
-------
What was broken or missing, with the observable symptoms (error messages,
wrong output, failing behavior). List each distinct symptom.

ROOT CAUSE
----------
Why it happened. Include concrete evidence where available (log lines,
decoded values, error codes, spec references) so the diagnosis can be
verified without reproducing the bug.

FIX
---
Describe every change grouped by file or subsystem ((a), (b), (c), ...) and
explain why each part is needed. Only describe changes that are in this commit.

KNOWN WORKAROUND (optional section)
-----------------------------------
Flag any platform-specific, temporary, or driver-level workarounds so they can
be revisited later. State the environment where the problem was observed.

VERIFICATION
------------
How the fix was verified: which test cases were run, what was compared against
a reference, and how metadata (headers/ffprobe) was checked.
```

Rules:

- Title: `<area>:` prefix (e.g. `qsv/hevc:`, `libobs:`, `nvenc:`), imperative
  mood, keep it under ~100 characters.
- Keep the sections in this order; omit a section only if it genuinely does not
  apply (e.g. no workaround exists).
- Never omit VERIFICATION - an unverified fix is not done.

### What the PROBLEM section is for - and what it must not record

Judge every change that came out of a red leg or a rework with these four questions, in order:

1. **Is the code, as it now stands, explicable without this history?** -> *level 0*: fold the change into the
   commit that contained the mistake and keep nothing. A signed/unsigned comparison gcc rejects and MSVC does
   not, a missing include, a comment worded wrong, a source file without a final newline.
2. **Does the fact change how the next change must be written?** -> *level 1*: it belongs in AGENTS.md
   (section 3a), not in the commit. The `-Wnewline-eof` + `CMAKE_COMPILE_WARNING_AS_ERROR` mechanism is this
   level: MSVC is silent, both macOS legs fail.
3. **Does the fact explain why the code looks wrong but is not?** -> *level 2*: fold it into the owning commit
   but keep the ROOT CAUSE, because the receiving commit's message grows to cover what it absorbs. The density
   control ids are this level: `QButtonGroup` auto-ids start at -2, so Classic is `-2` and a stored `-1`
   resolves to no button at all - an unexplained `-2` invites someone to "fix" it back.
4. **Is it OBS's own behaviour - pre-existing, or reachable at runtime by a user or a harness?** -> *level 3*:
   it is content, not a fixup. Its own commit (the P010 query answering the opposite of its name, the
   unbound-canvas null dereference because encoder create runs before the mix exists), or a full
   PROBLEM/ROOT CAUSE inside the feature commit that needed it.

Tie-breaker: if nobody writes it down, does someone later "clean up" the code and break it?

One home per fact: the commit says why the code is the way it is, AGENTS.md says what to do differently next
time, `scratch/plan/*.md` keeps the narrative and the harness commands. Never the same sentence twice.

## 2. Working tree hygiene (mandatory)

- **Before trying a different fix or workaround, remove the code of the test
  that did not work.** The working tree must never accumulate experimental
  leftovers from unsuccessful attempts.
- At commit time, `git diff` against the previous commit must contain only what
  the final solution needs: no debug logging added during diagnosis (unless it
  has permanent value), no commented-out code, no unrelated formatting changes.
- Revert cosmetic-only edits to files that are not part of the fix.
- **Write source with the file-edit tool, and read the diff of a touched file before
  building it.** A shell write channel (`Get-Content`/`Set-Content`, `>` redirection)
  rewrites the payload on the way in: a C block spliced into `libobs/obs-encoder.c` that
  way arrived with every `"` as the two characters `34` - `obs_encoder_valid(encoder,
  34obs_encoder_keyframe_requested34)` - and a `%s` that had gained a space, so the file
  did not compile. `git diff --stat` reported "36 insertions", which says nothing; the
  diff of the file shows it at a glance.

## 3. One gate per finished feature: finish, verify locally, shape the commits, tag once

The **feature**, not the commit, is what CI sees and what gets pushed. Tag runs cost ~20 min (Windows x64),
longer on arm64, 30-45 min on Ubuntu, so letting a runner discover what a local check would have caught costs a
whole cycle. Waiting for a run while doing nothing is still forbidden - start the next slice right after
tagging.

1. Finish the whole feature locally, working as if it were one commit.
2. Pre-tag checklist - all of it, before any tag exists:
   - `cmake --build build_x64 --config RelWithDebInfo --target obs-studio`
   - `pwsh -NoProfile -File test/pre-gate/run.ps1` green, or every SKIP named in CI-PENDING
     (`-NoMeasure` is the fast lifecycle half; `test/pre-gate/README.md` says what each case pins down).
     Once the batch is committed, name it - `-Base <the commit the batch starts from>` - and gate with
     `-Strict`, so a SKIP has to be explained instead of passing quietly. `-Background` returns at once and
     the run is followed in `scratch/tmp/pregate/run-<time>.txt`.
     This one command covers what used to be the newline scan, the clang-format probe and the probe-leak
     grep, and it is the only place those rules get re-checked from now on.
   - the gcc-strictness read of the new code: signed/unsigned comparisons, unused members and functions, and
     STL includes Apple libc++ does not provide transitively - a read, not a script: the suite cannot do it
   - one portable smoke launch (section 6) with no crash report written
   - never finish a verification run by killing OBS: `obs_shutdown()` only runs on a real close, and that is
     where the fan-out's outputs, encoders and services die. Ask the app to quit - `test/pre-gate` does that
     with `OBS_PREGATE_ACTIONS=quit@<s>` - and a run that had to be killed tested no shutdown at all.
   - after an operation that rewrites source files (a `git checkout`, a restore from a copy, a commit split),
     prove the binary carries that code before believing a test result: `Copy-Item` and `git checkout` give
     the file the mtime of its source, which can be older than the `.obj`, and MSBuild then relinks the stale
     objects - the build is clean and the reverted behaviour is quietly back in the executable. Check the
     `.obj` timestamp, or look for the new identifier in the built binary (the frontend is compiled into
     `obs64.exe`, there is no frontend DLL to inspect).
3. Shape the history to its final form **before** the gate (section 7): squash the batch and correct a build
   slip by amending the commit that contains it, so a mistake never becomes a commit of its own.
4. Tag the tip once: schema `test-<problem-or-feature>` - the name of the problem being fixed or the feature
   being added, **no whitespace** (use hyphens), e.g. `test-qsv-hevc-444-colors` - then
   `git tag test-... <commit> && git push origin test-...`
5. Keep working on the next slice immediately; poll the run between other steps
   (`scratch/tmp/gate-check.ps1`, `scratch/tmp/gate-wait.ps1`; token comes from the credential manager,
   see `scratch/plan/CI-NOTES.md`).
6. When every required job is green: push the branch - that push is then a fast-forward, because green work
   reaches origin exactly once and in final form - delete the tag (mandatory: test tags are a temporary CI
   gate, not release history), and remove the covered rows from `scratch/plan/CI-PENDING.md`:
   `git push origin <branch> && git tag -d test-... && git push origin --delete test-...`
7. A red leg means the feature was **not finished**: fix it by correcting the commit that was wrong (amend, or
   a fixup folded into it), never a new fixup commit on top. Delete the old tag locally and on origin, tag the
   new tip, and the branch still never saw the broken state.
8. Batching: several finished slices may share one tag at the newest tip - that build contains all earlier
   commits, so a single run gates the whole batch, which is then pushed together. The batch must be final
   **before** it is tagged: correcting it afterwards means re-tagging, not adding a commit.
9. Read the individual jobs of the "Test Tag" workflow (`.github/workflows/test-tag.yaml`), not the run
   conclusion: Windows x64, Windows arm64 **and both macOS targets** must be green. The macOS runners are
   online again (they built on 2026-11-06), so a macOS failure is a real build failure - never write it off
   as an offline-runner artefact. Ubuntu is slow; skipping it needs the user's go-ahead and the CI-PENDING
   entry must say so. A red Ubuntu job whose **Build** step passed but whose **Package** step died on a
   network error is an infrastructure flake, not a code failure (see scratch/plan/CI-NOTES.md).
   A macOS leg may be skipped **only on the evidence that no runner was ever assigned to it**:
   `scratch/tmp/gate-jobs.ps1 <run id>` shows the cancelled job with an empty `runner_name` (it stayed queued
   until the cancel), and `scratch/tmp/gate-run-compare.ps1 <red run> <last green run>` proves the same pool
   built recently. `cancelled` **with** a runner assigned, and `failure`, are build results and must be fixed,
   not skipped. When macOS is skipped, the CI-PENDING row has to state the evidence and what the batch
   touched, so the unverified surface is written down (2026-11-10, run 42837843752: both macOS legs cancelled
   after a 15 min queue with no runner, Ubuntu and both Windows legs green, batch = one plugin file and the
   harness, so the branch moved).

## 3a. What the macOS job catches and the Windows jobs do not (check before tagging)

The frontend is compiled with Apple clang on both macOS targets and the build turns warnings into errors:
`cmake/common/compiler_common.cmake` enables `-Wnewline-eof` (line 50) and sets
`CMAKE_COMPILE_WARNING_AS_ERROR ON` unless the caller decided otherwise (lines 85-86) - which the CI presets
force on anyway (`CMAKE_COMPILE_WARNING_AS_ERROR: true` in `CMakePresets.json`). MSVC never reports a missing
final newline, so a source file written without one passes every Windows job and kills both macOS jobs - that
is exactly what failed run 42284720040. Case `s01-static` checks every C/C++ file the batch changed;
`scratch/tmp/fix-newline.ps1` appends what is missing.

The same file turns on `-Wshorten-64-to-32` (line 48), which MSVC has no equivalent of: a Qt 6 `qsizetype`
(the return type of `QString::indexOf`/`lastIndexOf`, container `size()`) must never be assigned to `int`.
Write `const qsizetype at = name.indexOf(':');` - `int` compiles here and breaks both macOS legs.

A branch push also runs `.github/workflows/check-format.yaml`, which pipes the changed C/C++ files through
clang-format 22 (Homebrew `clang-format@22`). The local VS LLVM clang-format is the same major (22.1.3) and
`s01-static` runs it over the changed files, comparing each against its version at the base so only drift the
batch itself added can fail the gate (the repository carries older debt; `scratch/tmp/format-scan.ps1 -Base
31.1.2` counted 75 of 174 sources, and a repo-wide pass is the user's call, not an agent's).
`scratch/tmp/same-tokens.ps1 -A <file> -B <file>` (it takes two files) then proves such a pass moved nothing but
whitespace, by hashing both with all spaces, tabs, CR and LF removed.

`scratch/plan/CI-PENDING.md` lists the commits that are committed but not on the branch yet because their
gate is still running (or went red). Never leave a commit out of it.

Exception: changes that only touch `.github/workflows` (CI configuration) or documentation (`AGENTS.md`) do
not change program code and do **not** require the test-tag build gate.

## 3b. libobs object ownership: hand a create result over, never assign it

Every `obs_*_create()` returns a reference the caller owns, and the wrapper types take a **second** reference
when they are assigned or constructed from it. So the two shapes below both leak the create reference:

```c
stream->output = obs_output_create(...);      /* OBSSafeRef member: assignment addrefs, nobody drops this one */
OBSService svc(obs_service_create(...));      /* constructor owns one, the create reference is extra */
```

Write it the other way round instead: create into an auto-release wrapper and assign that
(`OBSOutputAutoRelease created = obs_output_create(...); member = created;`), or release the create reference
explicitly right after constructing the wrapper. The entry then owns exactly one reference.

A leaked reference does not merely leak memory: the object stays registered until `obs_free_data()` frees it,
which runs **after** `obs_shutdown()` unloaded the plugin modules, so the encoder type's `destroy` pointer is
read out of unloaded code and libobs calls through it. That is the OBS-quit-with-a-live-session crash, and it
cost ~350 MB of encoder state per destination per session. `test/pre-gate` fails a case on the leak even when
nothing crashes: libobs logs `N output(s)/encoder(s)/service(s) were remaining`, and any such line is a FAIL.

## 3c. libobs locks: what a send thread may never take

`struct obs_encoder`'s `init_mutex` (recursive) is the lock `obs_encoder_start_internal()`,
`obs_encoder_stop()` and `obs_encoder_shutdown()` hold while they create/destroy the plugin context and move
the raw-video connection. **A thread that consumes the encoder's packet queue must never take it**, because
the holder is waiting on exactly that pipeline:

```
obs_encoder_start / _stop / _shutdown   [holds init_mutex]
   └─ waits for the video/encode pipeline to settle (the connection, the in-flight frame)
        └─ the encode thread delivers into the output's packet callback
             └─ the RTMP add_packet path is full and waits for the send thread to drain
                  └─ the send thread is inside a libobs entry point that takes init_mutex   ✗ cycle
```

This is not theoretical. Adding `obs_encoder_copy_extra_data()` - which took `init_mutex`, on the theory
that it is the lock guarding the plugin context the RTMP send thread reads - turned every RTMP case into a
hang: `t02`, `t12`, `t13`, `t16` ended with OBS frozen, exit -1, no `obs_free_data` line, and `t16` lost all
13 scripted actions, i.e. it deadlocked during startup. The recording-only and cold-quit cases stayed green,
because they have no send thread. The change was reverted.

So the fix for the `send_audio_header` use-after-free must not put a libobs lock on the send thread. The
shape that works is **moving the read to the thread the encoder already orders against**: the plugin snapshots
the audio header bytes (its own `bmemdup`) from the audio packet callback - that callback runs on the
encoder's own thread, where the borrowed pointer is safe by contract - and the send thread sends from the
snapshot, the way `video_headers[]`/`update_video_header_snapshot` already does for video. Rule of thumb:
snapshot the header where the lifecycle is ordered, consume it where it is not.

## 4. Launching the local rundir build (Windows, mandatory)

`obs64.exe` resolves its data files **relative to the current working
directory**, not relative to the executable: `GetDataFilePath()`
(`frontend/utility/platform-windows.cpp`) first tries `data/obs-studio/<file>`
and falls back to `OBS_DATA_PATH "/obs-studio/"`, where `OBS_DATA_PATH` is
`"../../data"` on Windows (`cmake/windows/defaults.cmake`) - also relative.
Launched from any other cwd (e.g. the repository root), OBS dies at startup
with **"Failed to find locale/en-US.ini"**.

Always launch with the working directory set to the executable's folder
(paths below are relative to the repository root):

```powershell
Start-Process -FilePath 'build_x64/rundir/RelWithDebInfo/bin/64bit/obs64.exe' `
  -WorkingDirectory 'build_x64/rundir/RelWithDebInfo/bin/64bit'
```

(Equivalently, the rundir root `build_x64/rundir/RelWithDebInfo` works as cwd,
since the first, cwd-relative attempt is `data/obs-studio/...`.)

Related facts:

- A separate NSIS install of this fork may exist at the default Windows
  install location (e.g. `C:\Program Files\obs-studio`, same layout:
  `bin\64bit` + `data\obs-studio`). Never confuse the two - freshly
  built/instrumented DLLs only exist in `build_x64/rundir/`.
- Logs of this fork are written to `%APPDATA%\obs-studio\logs` and named
  `<YYYY-MM-DD HH-MM-SS>.txt` (no `obs` prefix). Crash reports from the OBS
  crash handler do **not** land there: they go to a sibling `crashes` folder as
  `Crash <YYYY-MM-DD HH-MM-SS>.txt` (portable mode:
  `build_x64/rundir/RelWithDebInfo/config/obs-studio/crashes/`, see section 6).
- That report is the fastest way to a Windows crash here: it names the exception
  code, the faulting module and the **symbolised stack of the crashed thread**,
  so it beats both the Event Viewer (which often blames a module that was merely
  loaded last) and reproducing by hand. Read it before guessing.
- One exception: `0xc0000409` (`__fastfail`, e.g. a CEF/obs-browser abort or a
  MSVC stack-buffer-overrun check) never reaches the OBS handler, so no report
  file appears and only Windows Error Reporting knows about it. A genuine
  access violation (`0xc0000005`) does get a report written.

## 5. Temporary / scratch files (mandatory)

Never create temporary or scratch files **outside** the working directory
(e.g. `%TEMP%`, `%APPDATA%`, other drives): analysis scripts, ffprobe/ffmpeg
dumps, decoded frames, intermediate data - everything goes into a scratch
folder inside the repository (use `scratch/` at the repo root). Scratch files
are temporary: remove them when they are no longer needed and never commit them.

## 6. Portable mode for profile/config tests (mandatory when a test modifies profiles or configs)

When a test needs to create or modify **profiles, scene collections, or the global
config** (encoder settings, output paths, ...), run OBS in **portable mode**. All state
is then written into the rundir instead of the user's real `%APPDATA%\obs-studio`, keeping
tests isolated, repeatable, and free of cross-contamination with a manual install. Never
hand-edit profiles/configs under `%APPDATA%` for a test.

Path rule: in portable mode `CONFIG_PATH = BASE_PATH "/config"`
(`frontend/OBSApp.cpp:1450`) and the updater sentinel is read from
`BASE_PATH "/disable_updater"` (`frontend/obs-main.cpp:1084`). On Windows `BASE_PATH` is
`"../.."` (`frontend/obs-main.cpp:903`) - **relative to the current working directory**.
With CWD = `bin/64bit` that resolves two levels up to the rundir root, so the whole
portable tree lives under `build_x64/rundir/RelWithDebInfo/`.

Launch (paths relative to the repository root; CWD **must** be `bin/64bit`):

```powershell
$wd = 'build_x64/rundir/RelWithDebInfo/bin/64bit'
Start-Process -FilePath "$wd/obs64.exe" `
  -ArgumentList '--portable', `
                '--profile', '<profile>', `
                '--collection', '<collection>', `
                '--startrecording', `
                '--disable-shutdown-check', `
                '--disable-updater' `
  -WorkingDirectory $wd
```

Parameters:
- `--portable` - write all config/state under the rundir (path rule above). Always.
- `--profile <P>` / `--collection <C>` - select the test profile and scene collection.
- `--startrecording` - start recording on launch (headless/automated runs only).
- `--disable-shutdown-check` - skip the shutdown confirmation prompt (automated runs).
- `--disable-updater` - no update check/popup; required for unattended runs.

Portable layout (relative to the repository root, CWD = `bin/64bit`):

| What | Path |
|---|---|
| Portable root (`BASE_PATH`) | `build_x64/rundir/RelWithDebInfo/` |
| Config root | `build_x64/rundir/RelWithDebInfo/config/obs-studio/` |
| Global config | `build_x64/rundir/RelWithDebInfo/config/obs-studio/global.ini` |
| Profiles | `build_x64/rundir/RelWithDebInfo/config/obs-studio/basic/profiles/<profile>/basic.ini` |
| Scene collections | `build_x64/rundir/RelWithDebInfo/config/obs-studio/basic/scenes/<collection>.json` |
| Logs | `build_x64/rundir/RelWithDebInfo/config/obs-studio/logs/<YYYY-MM-DD HH-MM-SS>.txt` |
| Crash reports | `build_x64/rundir/RelWithDebInfo/config/obs-studio/crashes/Crash <YYYY-MM-DD HH-MM-SS>.txt` |

Gotchas (all hit during the QSV HEVC ABR tests):
- **CWD must be `bin/64bit`.** From any other CWD, `BASE_PATH` (`../..`) resolves to a
  different folder - portable config lands somewhere unexpected and/or data files are not
  found (see section 4).
- There is no simple/advanced output-mode split any more: the advanced handler is
  always built, `[Output] Mode` is not read and is removed from the profile on
  load (the `[SimpleOutput]` section is swept at the same time). Older
  documents/harnesses that set `Mode=Advanced` are harmless but pointless.
- A media source in a scene JSON uses the key `local_file` (not `input`); use forward
  slashes. Point it at a file that actually exists - a missing file encodes to near-black
  at ~177 kbps and invalidates any bitrate measurement.
- To make the no-updater setting stick for every launch from this rundir, also drop an
  empty sentinel at `build_x64/rundir/RelWithDebInfo/disable_updater`
  (`BASE_PATH/disable_updater`). The `--disable-updater` flag covers a single launch; the
  sentinel covers all launches without the flag.

## 7. History is written once: shape the commits at feature completion, before the gate

Squashing happens when a feature is finished and **before** it is tagged (section 3 step 3), so the runners
build exactly the history that will be published and a mistake never reaches origin as a commit of its own.
Rewriting history that is already published is a separate, deliberate act: only with the user's explicit
go-ahead, and only with the proof chain at the end of this section.

### Shaping a finished feature

- One commit per change that could be shipped on its own, described by one
  PROBLEM/ROOT CAUSE/FIX/VERIFICATION block (section 1). The default boundary is the slice/stage the plan
  documents already use.
- Build slips are corrected inside the commit that contained them (level 0/1 of section 1) - never as a fixup
  commit, and not mentioned in the message.
- When a folded change is **above level 1**, the receiving commit's message grows to cover its reasoning: a
  squash that drops a level-2 ROOT CAUSE has lost information, not just commits.
- A genuine OBS/libobs defect found along the way stays content of its own (level 3), not a fixup: its
  PROBLEM section describes behaviour that exists without the feature.
- Docs/CI-only commits stay separate from program code: the gate exception in section 3 depends on telling
  them apart, and a squash that mixes them forces a build for a comment.
- `SUPERSEDED COMMITS` is listed only when a **published** commit is re-shaped and its SHA is quoted
  somewhere (`scratch/plan/CI-PENDING.md`, crash reports, older plan notes). Folded mistakes are not listed
  at all.

### Before rewriting published history

- Name the anchor and check whether it is public: `git tag --contains <anchor>` plus
  `git merge-base --is-ancestor <anchor> origin/31.1.2-features`. If the anchor is a **release tag**
  (`31.1.2-betaN`) then everything after it is already published, and publishing the result is a
  `--force-with-lease` push that replaces public SHAs.
- `git log --reverse --oneline <anchor>..HEAD` into `scratch/plan/SQUASH-PLAN.md`, together with the proposed
  grouping, **before** any rewrite. Ask the user to approve the grouping.
- Create both a backup branch and a backup tag at HEAD (`backup/pre-squash-<date>`), and record
  `git rev-parse HEAD^{tree}`. A rewrite with no backup ref is unrecoverable.
- Working tree must be clean (section 2): a rewrite may not smuggle working-tree changes in.
- Check that nothing is already in flight before starting: `Test-Path .git/rebase-merge`, `.git/rebase-apply`,
  `.git/MERGE_HEAD`, `.git/CHERRY_PICK_HEAD`. Starting a rewrite on top of a stale sequencer state leaves that
  state's todo list queued behind the new history, and the next `--continue` replays it.

### Proof, in this order (a squash that changes content is not a squash)

1. `git rev-parse HEAD^{tree}` after the rewrite **equals** the recorded baseline.
2. `git range-diff <anchor>..backup/pre-squash-<date> <anchor>..HEAD` read in full - the only allowed
   differences are reshaping and message text.
3. A local build of the affected target; cheap, and it catches what a tree comparison cannot (a lost file in a
   changed set the tree does not notice).
4. Then re-attach the tags and re-gate: the gate belongs to a commit that no longer exists, so delete the old
   `test-*` tag locally and on origin, create a fresh one at the new tip, and treat the rewritten tip as
   **unverified until that run is green** (section 3) - the `--force-with-lease` branch push happens only
   then. Release tags are never moved; scratch tags whose commits were folded away are deleted.
5. Fix every SHA quoted in `scratch/plan/*.md` - those files are untracked, cite commits by SHA, and silently
   rot after a rewrite (`scratch/tmp/sub-text.ps1`).
6. Close the rewrite by checking the sequencer state is gone: `Test-Path .git/rebase-merge` must be false. A
   rebase that was given up on is cleared with `git rebase --quit`, which moves no ref and touches no file.
   Never `--abort` - it resets the branch to the rebase's `orig-head`, i.e. the pre-rewrite history you have
   already published over - and never `--continue` onto a tip that already contains those commits. This happened
   on 2026-11-07: the squash was rebuilt with reset + re-commit, the abandoned interactive rebase stayed at
   `msgnum 15/48` for hours, and the editor kept showing a rebase that did not exist (scratch/plan/CI-PENDING.md).

