# s01 - the things the Windows build cannot see: Apple clang's -Wnewline-eof (both macOS legs fail on a
# missing final newline while MSVC stays silent), clang-format, and ad-hoc probes left behind.
# Scope: the C/C++ files this batch touched, never the whole repository (AGENTS.md section 3a).
. (Join-Path $PSScriptRoot '..\lib\pregate.ps1')
Set-Location $PG.Root

$base = if ($env:PREGATE_BASE) { $env:PREGATE_BASE } else { 'HEAD' }
$sources = @(git -c core.quotepath=false diff --name-only $base -- |
	Where-Object { $_ -match '\.(c|cc|cpp|h|hpp)$' })
pg_say "  scan  $($sources.Count) changed C/C++ file(s) since $base"
$haveSources = $sources.Count -gt 0
if (-not $haveSources) {
	pg_say "  note  no C/C++ file changed since $base, the file-scoped checks would be vacuous"
}

# Apple clang warns on a missing final newline and CMAKE_COMPILE_WARNING_AS_ERROR turns it into an error.
$missingNewline = @()
foreach ($file in $sources) {
	if (-not (Test-Path $file)) { continue }
	$bytes = [System.IO.File]::ReadAllBytes($file)
	if ($bytes.Length -eq 0) { continue }
	if ($bytes[-1] -ne 0x0A) { $missingNewline += $file }
	if ($bytes -contains 0x0D) { pg_say "  note  $file carries CRLF line endings" }
}
pg_check ($missingNewline.Count -eq 0) "every changed source ends in a newline (missing: $($missingNewline -join ', '))"

# A probe is a diagnostic tool: none of it may ride along into a gated batch. The marker is the point - any
# 'TEMP-' tag or OBS_TEST_ name means code written to look, not code written to stay.
$probes = @(git --no-pager grep -n -e 'TEMP-' -e 'OBS_TEST_' -- frontend libobs plugins shared)
pg_check ($probes.Count -eq 0) "no ad-hoc probe survives in program code ($($probes -join ' ; '))"

# clang-format: the format job in CI runs version 22 (Homebrew clang-format@22), so the local one has to be
# the same major for this check to mean anything. The Visual Studio LLVM toolset ships it off PATH.
$clangFormat = @(Get-Command clang-format -ErrorAction SilentlyContinue | ForEach-Object { $_.Source })[0]
if (-not $clangFormat) {
	$candidates = @(@($env:OBS_PREGATE_CLANGFORMAT,
		'C:\Program Files\Microsoft Visual Studio\18\Community\VC\Tools\Llvm\x64\bin\clang-format.exe',
		'C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\Llvm\x64\bin\clang-format.exe') |
		Where-Object { $_ -and (Test-Path $_) })
	if ($candidates.Count) { $clangFormat = $candidates[0] }
}

if (-not $clangFormat) {
	pg_skip 'clang-format is not installed, the format half of this check did not run'
} else {
	$version = & $clangFormat --version
	pg_say "  scan  $version over the changed sources"
	if ($version -notmatch '\b22\.') {
		pg_say '  note  this is not clang-format 22, the version the CI format job uses'
	}

	# A file can already be off-format before this batch touches it (the repository has that debt, see
	# AGENTS.md section 3a). Comparing against the same file at the base says who added the drift: a file
	# that was clean and is not any more is this batch's problem, one that was already dirty is not - and a
	# file the batch introduced has to be clean.
	$madeWorse = @()
	$inherited = @()
	foreach ($file in $sources) {
		if (-not (Test-Path $file)) { continue }

		& $clangFormat --dry-run -Werror -- $file 2>&1 | Out-Null
		if ($LASTEXITCODE -eq 0) { continue }

		$baseFile = Join-Path $PG.Work ('base-' + ($file -replace '[\\/]', '_'))
		New-Item -ItemType Directory -Force -Path $PG.Work | Out-Null
		$baseLines = @(& git show "${base}:$file" 2>$null)
		if ($LASTEXITCODE -eq 0 -and $baseLines.Count -gt 0) {
			[System.IO.File]::WriteAllText($baseFile, ($baseLines -join "`n"))
			& $clangFormat --dry-run -Werror -- $baseFile 2>&1 | Out-Null
			if ($LASTEXITCODE -eq 0) { $madeWorse += $file } else { $inherited += $file }
		} else {
			pg_say ("  note  {0} is new in this batch, so it has to be format-clean" -f $file)
			$madeWorse += $file
		}
		Remove-Item $baseFile -Force -ErrorAction SilentlyContinue
	}

	if ($inherited.Count) {
		pg_say ("  note  already off-format at the base (debt from earlier commits): " + ($inherited -join ', '))
	}
	pg_check ($madeWorse.Count -eq 0) "this batch introduced no clang-format offences ($($madeWorse -join ', '))"
}

# A green source scan over an empty file list proves nothing, and the probe grep above is not the whole case:
# say the checks did not read anything, and let -Strict refuse to gate on it. Point the runner at the batch
# with -Base <the commit the batch starts from> when the batch is already committed.
if (-not $haveSources) {
	pg_skip "no C/C++ file changed since $base - name the batch: run.ps1 -Base <commit before the batch>"
}

pg_verdict 's01-static'
exit $global:PGCODE
