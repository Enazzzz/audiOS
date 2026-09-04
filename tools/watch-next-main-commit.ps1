# Polls origin/main until it advances past a persisted baseline SHA, then
# pulls, builds audios.iso, and emits a wake sentinel for the Cursor agent.
param(
	[int]$IntervalSeconds = 30
)

$ErrorActionPreference = "Stop"
Set-Location $PSScriptRoot\..

$baselineFile = Join-Path (Get-Location) ".cursor\main-watch-baseline"

function Sync-Main {
	# git writes fetch progress to stderr; don't treat that as a fatal error.
	$prev = $ErrorActionPreference
	$ErrorActionPreference = "Continue"
	git fetch origin main 2>&1 | Out-Null
	$ErrorActionPreference = $prev
}

function Get-Baseline {
	if (Test-Path $baselineFile) {
		return (Get-Content $baselineFile -Raw).Trim()
	}

	Sync-Main
	$initial = (git rev-parse origin/main).Trim()
	New-Item -ItemType Directory -Force -Path (Split-Path $baselineFile) | Out-Null
	Set-Content -Path $baselineFile -Value $initial -NoNewline
	return $initial
}

function Set-Baseline([string]$Sha) {
	New-Item -ItemType Directory -Force -Path (Split-Path $baselineFile) | Out-Null
	Set-Content -Path $baselineFile -Value $Sha -NoNewline
}

function Invoke-UpdateAndBuild([string]$FromShort, [string]$ToShort, [string]$FromSha, [string]$ToSha) {
	Write-Host "New commit on main: $FromShort -> $ToShort"
	Write-Host "Pulling latest..."
	git pull origin main

	Write-Host "Building audios.iso..."
	try {
		& "$PSScriptRoot\build-iso.ps1"
	} catch {
		Write-Warning "ISO build failed: $_"
	}
}

Sync-Main
$baseline = Get-Baseline
$short = $baseline.Substring(0, 7)

Write-Host "Watching origin/main (baseline: $short). Poll every ${IntervalSeconds}s."

while ($true) {
	$remote = (git rev-parse origin/main).Trim()

	if ($remote -ne $baseline) {
		$remoteShort = $remote.Substring(0, 7)
		Invoke-UpdateAndBuild $short $remoteShort $baseline $remote
		Set-Baseline $remote

		$payload = @{
			prompt = "audiOS main updated ($short -> $remoteShort). Pull and ISO build completed."
			baseline = $baseline
			remote = $remote
		} | ConvertTo-Json -Compress

		Write-Output "AGENT_LOOP_WAKE_audios_main $payload"
		break
	}

	Start-Sleep -Seconds $IntervalSeconds
	Sync-Main
}
