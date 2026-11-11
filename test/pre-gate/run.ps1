<#
	The pre-gate suite. Run it after a local build and before any test-* tag exists (AGENTS.md section 3):
	one green tag run costs 20+ minutes, and none of the CI legs ever quits OBS while a session is live -
	which is exactly how the multi-destination shutdown crash survived two slices.

	  pwsh -NoProfile -File test/pre-gate/run.ps1
	  pwsh -NoProfile -File test/pre-gate/run.ps1 -Cases t03-quit-while-streaming-two
	  pwsh -NoProfile -File test/pre-gate/run.ps1 -NoMeasure        # lifecycle only, the fast half
	  pwsh -NoProfile -File test/pre-gate/run.ps1 -Strict          # a SKIP is a failure (for gating)
	  pwsh -NoProfile -File test/pre-gate/run.ps1 -ForceBusy       # accept a run measured on a loaded box

 Before the first case the runner runs a preflight: active compilers, the WSL VM, CPU load, free memory and
 free disk. The delivery check counts frames delivered over a run, so a busy machine reads exactly like a
 starved consumer, and the suite refuses to measure on one (AGENTS.md section 3). -ForceBusy accepts it, and
 the transcript then prints "ACCEPTED ON A LOADED BOX" so the record says which kind of run it was.

	Exit code 0 means every case passed; 1 means at least one failed or errored. Each case runs in its own
	powerhouse so a crashed OBS cannot take the rest of the suite with it.
#>
param(
	[string]$Cases = '',
	[string]$Base = 'HEAD',
	[switch]$NoMeasure,
	[switch]$Strict,
	[int]$CaseTimeoutSec = 420,
	[switch]$Background,
	[switch]$ForceBusy
)

$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
Set-Location $repo
$env:PREGATE_BASE = $Base

# The suite outlasts most shells that wait for it. -Background starts another runner and returns at once;
# follow it in scratch/tmp/pregate/run-<time>.txt, which every case appends to as it finishes. The relay is
# written as a -Command line because Start-Process mangles quoted argument lists, and the child's own output
# is captured to a file: a launcher that dies quietly is worse than one that does not start.
if ($Background) {
	$quoted = @()
	if ($Cases) { $quoted += "-Cases '$Cases'" }
	if ($Base -ne 'HEAD') { $quoted += "-Base '$Base'" }
	if ($PSBoundParameters.ContainsKey('CaseTimeoutSec')) { $quoted += "-CaseTimeoutSec $CaseTimeoutSec" }
	if ($NoMeasure) { $quoted += '-NoMeasure' }
	if ($Strict) { $quoted += '-Strict' }
	$cmd = "& '$PSCommandPath' $($quoted -join ' ')"
	$relay = Join-Path $repo 'scratch/tmp/pregate/background-out.txt'
	New-Item -ItemType Directory -Force -Path (Split-Path $relay) | Out-Null
	$p = Start-Process -FilePath 'pwsh' -ArgumentList @('-NoProfile', '-Command', $cmd) `
		-WorkingDirectory $repo -PassThru -RedirectStandardOutput $relay -RedirectStandardError "$relay.err"
	Write-Output ("detached runner pid " + $p.Id + " - follow scratch/tmp/pregate/run-*.txt")
	Write-Output ("launcher output: " + $relay + " (and .err)")
	exit 0
}

# Anything still running from an earlier attempt poisons the ports and the log/crash comparisons.
Get-Process obs64, mediamtx -ErrorAction SilentlyContinue | ForEach-Object {
	try { $_.Kill(); $null } catch {}
}
Start-Sleep -Seconds 2

# Preflight: is this machine quiet enough to measure on? The delivery check counts frames delivered over a
# run, and CPU starvation reads **exactly** like a starved consumer: two gates on 2026-11-10/11 were scored
# while local builds ran, and one of them cost a 40-minute hunt for a deadlock the code did not contain.
# A loaded box is a hard stop rather than a note - a gate that passes on a loaded machine proves less than
# one that refuses to run - and only -ForceBusy accepts it, on the record.
$busy = @()
$probe = 'cl', 'link', 'MSBuild', 'VBCSCompiler', 'gcc', 'g++', 'cc1plus', 'ninja', 'cmake', 'ccache', 'vmmem', 'wslservice'
# Win32_Processor.LoadPercentage reads 0 on this box even under 12 spinners (measured 2026-11-11), so the
# utilization is sampled directly: the sum of the per-process CPU-time deltas over one wall-clock second,
# divided by the core count. It costs two Get-Process passes and it names the processes that moved.
$cores = [Environment]::ProcessorCount
$before = @{}
foreach ($pr in Get-Process) { try { $before[$pr.Id] = $pr.CPU } catch {} }
Start-Sleep -Milliseconds 1000
$used = 0.0
$movers = @()
foreach ($pr in Get-Process) {
	if (-not $before.ContainsKey($pr.Id)) { continue }
	try { $delta = $pr.CPU - $before[$pr.Id] } catch { continue }
	if ($delta -le 0) { continue }
	$used += $delta
	if ($delta -gt 0.25) { $movers += [pscustomobject]@{ Name = $pr.ProcessName; Sec = $delta } }
}
$util = [math]::Round(100 * $used / $cores)
if ($util -gt 25) {
	$top = ($movers | Sort-Object Sec -Descending | Select-Object -First 4 |
		ForEach-Object { "$($_.Name) $([math]::Round($_.Sec, 1))s" }) -join ', '
	$busy += "CPU at $util% of $cores cores over the last second" + $(if ($top) { " (top: $top)" } else { '' })
}
$hot = @($movers | Where-Object { $probe -contains $_.Name } | ForEach-Object { $_.Name } | Select-Object -First 4)
if ($hot.Count) { $busy += ("compiler/VM active: " + ($hot -join ', ')) }
$freeGB = [math]::Round((Get-CimInstance Win32_OperatingSystem).FreePhysicalMemory / 1MB, 1)
if ($freeGB -lt 4) { $busy += "only $freeGB GB physical memory free" }
$drive = ($repo -split ':')[0]
$freeDiskGB = [math]::Round((Get-PSDrive $drive).Free / 1GB, 1)
if ($freeDiskGB -lt 5) { $busy += "only $freeDiskGB GB free on ${drive}: (the cases write recordings)" }
if ($busy.Count -and -not $ForceBusy) {
	Write-Output 'PREFLIGHT FAIL - the machine is not idle, and the delivery metric cannot tell CPU starvation from a code defect'
	foreach ($b in $busy) { Write-Output "  - $b" }
	Write-Output '  let the job finish first, or pass -ForceBusy to accept a run measured on a loaded box (and say so in scratch/plan/CI-PENDING.md)'
	exit 2
}
$note = if ($ForceBusy -and $busy.Count) { ", ACCEPTED ON A LOADED BOX: $($busy -join '; ')" } else { '' }
Write-Output ("preflight: machine usable (CPU " + $util + "% of $cores cores, $freeGB GB RAM free, ${drive}: $freeDiskGB GB free$note)")

$pgLib = Join-Path $PSScriptRoot 'lib\pregate.ps1'
if (-not (Test-Path $pgLib)) { throw "missing the suite library: $pgLib" }

# The redirect files of a case stay open after the case's own process is gone: OBS and the ingest
# are its grandchildren and inherit the stdout handle, so they keep the file alive as long as they
# run. Reading it the plain way threw "the process cannot access the file" 5 s into a case and, with
# $ErrorActionPreference = 'Stop', took the whole suite down with it. Open it for shared reading
# instead, and give a lingering grandchild a moment to go.
function Read-SharedText {
	param([string]$Path)
	if (-not (Test-Path $Path)) { return '' }
	for ($try = 0; $try -lt 40; $try++) {
		try {
			$fs = [System.IO.File]::Open($Path, [System.IO.FileMode]::Open, [System.IO.FileAccess]::Read,
				[System.IO.FileShare]::ReadWrite)
			$sr = New-Object System.IO.StreamReader($fs)
			$text = $sr.ReadToEnd()
			$sr.Dispose()
			$fs.Dispose()
			return $text
		} catch [System.IO.IOException] {
			Start-Sleep -Milliseconds 500
		}
	}
	return ''
}

$caseDir = Join-Path $PSScriptRoot 'cases'
$all = @(Get-ChildItem $caseDir -Filter '*.ps1' | Sort-Object Name | ForEach-Object { $_.BaseName })

$wanted = if ($Cases) { @($Cases -split '[,\s]+' | Where-Object { $_ }) } else { @($all) }

# Name a case by its full name or by an unambiguous prefix: -Cases t15 means
# t15-mixed-stream-and-recording-cycles. Typing the long names by hand is how a gating run ends up
# launching nothing at all (the runner exits, the transcript file never appears).
$expanded = @()
foreach ($name in $wanted) {
	$hits = @($all | Where-Object { $_ -eq $name })
	if ($hits.Count -eq 0) {
		$hits = @($all | Where-Object { $_.StartsWith($name) })
	}
	if ($hits.Count -eq 1) {
		$expanded += $hits[0]
	} elseif ($hits.Count -gt 1) {
		throw "case '$name' is ambiguous (matches: $($hits -join ', '))"
	} else {
		throw "no such case '$name' (available: $($all -join ', '))"
	}
}
$wanted = $expanded
if ($NoMeasure) { $wanted = @($wanted | Where-Object { $_ -notmatch '^t0[789]' }) }

$logDir = Join-Path $repo 'scratch\tmp\pregate'
New-Item -ItemType Directory -Force -Path $logDir | Out-Null
$transcript = Join-Path $logDir ("run-{0}.txt" -f (Get-Date -Format 'yyyy-MM-dd HH-mm-ss'))

$results = @()
$startAll = Get-Date
foreach ($name in $wanted) {
	$line = "--- {0} ---" -f $name
	Write-Output $line
	Add-Content -Path $transcript -Value $line

	$start = Get-Date
	$outFile = Join-Path $logDir "$name.out.txt"
	$errFile = Join-Path $logDir "$name.err.txt"

	# A case runs as its own process with a hard ceiling: a hung OBS or a stuck harness must not sit there
	# holding the ingest ports while the rest of the suite queues up behind it.
	$child = Start-Process -FilePath 'pwsh' `
		-ArgumentList @('-NoProfile', '-File', (Join-Path $caseDir "$name.ps1")) `
		-WorkingDirectory $repo -PassThru -RedirectStandardOutput $outFile -RedirectStandardError $errFile
	$timedOut = -not $child.WaitForExit($CaseTimeoutSec * 1000)
	if ($timedOut) {
		try { $child.Kill(); $null } catch {}
		Get-Process obs64, mediamtx -ErrorAction SilentlyContinue | ForEach-Object {
			try { $_.Kill(); $null } catch {}
		}
		Start-Sleep -Seconds 2
	}

	$code = if ($timedOut) { -1 } else { try { $child.ExitCode } catch { -1 } }
	$text = Read-SharedText $outFile
	$err = Read-SharedText $errFile
	if ($err.Trim()) { $text += "`nSTDERR:`n" + $err }
	Add-Content -Path $transcript -Value $text

	$escaped = [regex]::Escape($name)
	$verdict = if ($timedOut) { 'TIMEOUT' }
	elseif ($text -match "RESULT\s+$escaped\s+PASS") { 'PASS' }
	elseif ($text -match "RESULT\s+$escaped\s+SKIP") { 'SKIP' }
	elseif ($text -match "RESULT\s+$escaped\s+FAIL") { 'FAIL' }
	else { 'ERROR' }

	$secs = [math]::Round(((Get-Date) - $start).TotalSeconds)
	$results += [pscustomobject]@{ Case = $name; Verdict = $verdict; Code = $code; Seconds = $secs }
	@($text -split "`r?`n" | Select-Object -Last 24) | ForEach-Object { Write-Output "  $_" }
	Write-Output ("  -> {0} in {1} s (exit {2})" -f $verdict, $secs, $code)
}

$failed = @($results | Where-Object { $_.Verdict -in 'FAIL', 'ERROR', 'TIMEOUT' })
$skipped = @($results | Where-Object { $_.Verdict -eq 'SKIP' })
$passed = @($results | Where-Object { $_.Verdict -eq 'PASS' })

Write-Output ''
Write-Output '===== pre-gate summary ====='
foreach ($r in $results) { Write-Output ("  {0,-6} {1,-40} {2,5} s" -f $r.Verdict, $r.Case, $r.Seconds) }
Write-Output ("  {0} passed, {1} failed/errored, {2} skipped in {3} min" -f
	$passed.Count, $failed.Count, $skipped.Count,
	[math]::Round(((Get-Date) - $startAll).TotalMinutes, 1))
Write-Output "  full transcript: $transcript"

if ($skipped.Count -and -not $Strict) {
	Write-Output '  NOTE: skipped cases did not run - a gate may not be tagged over a skip without saying so'
}

$exit = if ($failed.Count) { 1 } elseif ($Strict -and $skipped.Count) { 1 } else { 0 }
exit $exit
