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

## 2. Working tree hygiene (mandatory)

- **Before trying a different fix or workaround, remove the code of the test
  that did not work.** The working tree must never accumulate experimental
  leftovers from unsuccessful attempts.
- At commit time, `git diff` against the previous commit must contain only what
  the final solution needs: no debug logging added during diagnosis (unless it
  has permanent value), no commented-out code, no unrelated formatting changes.
- Revert cosmetic-only edits to files that are not part of the fix.

## 3. CI gate via test tags (mandatory before pushing program code)

Before pushing program-code changes to this repository, verify that they build
on all supported platforms by running CI on a test tag:

1. Commit the changes locally.
2. Create and push a test tag pointing at that commit:
   - Tag schema: `test-<problem-or-feature>` - the name of the problem being
     fixed or the feature being added, **no whitespace** (use hyphens), e.g.
     `test-qsv-hevc-444-colors`.
   - `git tag test-... <commit> && git push origin test-...`
3. Wait for the "Test Tag" workflow (`.github/workflows/test-tag.yaml`) to
   finish green. It builds Windows (x64/arm64), macOS (arm64/x86_64) and
   Ubuntu 26.04 - no Flatpak. The built artifacts are available on the
   workflow run page (Actions → run → Artifacts) for verification.
4. Only after the build succeeds, push the branch/commit to the repository.
5. After the changes have been pushed, delete the test tag again (mandatory -
   test tags are a temporary CI gate, not part of the release history):
   `git tag -d test-... && git push origin --delete test-...`

Exception: changes that only touch `.github/workflows` (CI configuration) do not
change program code and do **not** require the test-tag build gate.

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
  crash handler land in the same directory as `crash-<timestamp>.txt`.

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

Gotchas (all hit during the QSV HEVC ABR tests):
- **CWD must be `bin/64bit`.** From any other CWD, `BASE_PATH` (`../..`) resolves to a
  different folder - portable config lands somewhere unexpected and/or data files are not
  found (see section 4).
- Advanced output mode is selected with `[Output] Mode=Advanced` in the profile's
  `basic.ini` (not `Service=Advanced`).
- A media source in a scene JSON uses the key `local_file` (not `input`); use forward
  slashes. Point it at a file that actually exists - a missing file encodes to near-black
  at ~177 kbps and invalidates any bitrate measurement.
- To make the no-updater setting stick for every launch from this rundir, also drop an
  empty sentinel at `build_x64/rundir/RelWithDebInfo/disable_updater`
  (`BASE_PATH/disable_updater`). The `--disable-updater` flag covers a single launch; the
  sentinel covers all launches without the flag.

