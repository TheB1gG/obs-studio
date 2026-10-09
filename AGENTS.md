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
