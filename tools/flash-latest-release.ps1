<#
.SYNOPSIS
	Downloads the latest audiOS GitHub release image and flashes it to a USB drive with balena CLI.

.DESCRIPTION
	Pulls the newest published release (not the newest commit) from the GitHub
	Releases API, verifies the asset against the SHA256 digest GitHub reports,
	caches it so unchanged releases are not re-downloaded, raw-writes it to the
	physical disk backing the given drive letter, then reads the disk back and
	compares hashes so a failed write can never be reported as success.

	Raw disk access requires an elevated (Administrator) PowerShell session.

	Uses the standalone balena CLI when present. The npm build of balena-cli runs
	on the system Node, and Node below v22.20.0 fails raw physical-drive opens on
	Windows with "EIO: i/o error" (nodejs/node#55623). The standalone package
	bundles its own patched Node and avoids that entirely.

.PARAMETER DriveLetter
	Drive letter of the target USB device, without a colon. Defaults to D.
	Resolved to the underlying \\.\PhysicalDriveN.

.PARAMETER DiskNumber
	Target a physical disk directly instead of resolving a drive letter. Needed
	when Windows has not assigned a letter, which is normal after a flash.

.PARAMETER Force
	Re-download even when the cached copy already matches the release digest.

.PARAMETER DownloadOnly
	Fetch and verify the image but skip flashing.

.PARAMETER VerifyOnly
	Skip flashing and only check whether the target disk already holds the image.

.PARAMETER SkipVerify
	Flash without the read-back comparison. Not recommended.

.PARAMETER Yes
	Skip the interactive confirmation before writing to the disk.

.PARAMETER Elevate
	Relaunch in an elevated window (single UAC prompt) rather than failing when
	the current session is not Administrator.

.EXAMPLE
	.\tools\flash-latest-release.ps1 -Elevate
	Flashes the latest release to whatever disk currently holds D:.

.EXAMPLE
	.\tools\flash-latest-release.ps1 -DiskNumber 1 -VerifyOnly -Elevate
	Checks whether disk 1 already matches the latest release.
#>
[CmdletBinding(DefaultParameterSetName = 'ByLetter')]
param(
	[Parameter(ParameterSetName = 'ByLetter')]
	[ValidatePattern('^[A-Za-z]$')]
	[string]$DriveLetter = 'D',

	[Parameter(ParameterSetName = 'ByDisk', Mandatory = $true)]
	[int]$DiskNumber,

	[switch]$Force,
	[switch]$DownloadOnly,
	[switch]$VerifyOnly,
	[switch]$SkipVerify,
	[switch]$Yes,
	[switch]$Elevate
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$Repo = 'Enazzzz/audiOS'
$AssetName = 'audios.img.gz'   # ~600 KB versus 64 MB for the raw image
$CacheDir = Join-Path $env:LOCALAPPDATA 'audiOS-releases'
$StandaloneBalena = Join-Path $env:LOCALAPPDATA 'balena-cli\balena\bin\balena.cmd'

# GitHub's API rejects requests without a User-Agent.
$ApiHeaders = @{
	'User-Agent'           = 'audiOS-flash-script'
	'Accept'               = 'application/vnd.github+json'
	'X-GitHub-Api-Version' = '2022-11-28'
}

<#
	Fetches the latest published release. The /releases/latest endpoint excludes
	drafts and prereleases, so this is always a real shipped build.
#>
function Get-LatestRelease {
	Write-Host "Checking latest release of $Repo..."
	return Invoke-RestMethod -Uri "https://api.github.com/repos/$Repo/releases/latest" -Headers $ApiHeaders
}

<#
	Returns the cached .gz asset, downloading only when the local copy is missing
	or its SHA256 differs from the digest GitHub published alongside the asset.
#>
function Resolve-ReleaseAsset {
	param([Parameter(Mandatory)]$Release)

	$asset = $Release.assets | Where-Object { $_.name -eq $AssetName }
	if (-not $asset) {
		$available = ($Release.assets | ForEach-Object { $_.name }) -join ', '
		throw "Release '$($Release.tag_name)' has no asset named '$AssetName'. Available: $available"
	}

	# GitHub reports the digest as "sha256:<hex>"; older assets may omit it.
	$expected = $null
	if ($asset.PSObject.Properties.Name -contains 'digest' -and $asset.digest) {
		$expected = ($asset.digest -replace '^sha256:', '').ToLowerInvariant()
	}

	New-Item -ItemType Directory -Force -Path $CacheDir | Out-Null
	$path = Join-Path $CacheDir $AssetName

	if (-not $Force -and (Test-Path $path) -and $expected -and
		(Get-FileHash $path -Algorithm SHA256).Hash.ToLowerInvariant() -eq $expected) {
		Write-Host "Cached image is current ($AssetName), skipping download."
		return $path
	}

	Write-Host "Downloading $AssetName ($([math]::Round($asset.size / 1MB, 2)) MB)..."
	$progress = $ProgressPreference
	$ProgressPreference = 'SilentlyContinue'  # Invoke-WebRequest is far slower with the progress bar on.
	try {
		Invoke-WebRequest -Uri $asset.browser_download_url -OutFile $path -Headers @{ 'User-Agent' = 'audiOS-flash-script' }
	} finally {
		$ProgressPreference = $progress
	}

	if ($expected) {
		$actual = (Get-FileHash $path -Algorithm SHA256).Hash.ToLowerInvariant()
		if ($actual -ne $expected) {
			Remove-Item $path -Force
			throw "SHA256 mismatch for ${AssetName}: expected $expected, got $actual"
		}
		Write-Host "SHA256 verified."
	} else {
		Write-Warning "Release published no digest for $AssetName; skipping checksum verification."
	}

	return $path
}

<#
	Expands the .gz into the cache. The raw image is needed regardless of what we
	hand to balena, because read-back verification compares against raw bytes.
#>
function Expand-Image {
	param([Parameter(Mandatory)][string]$GzPath)

	$imgPath = $GzPath -replace '\.gz$', ''
	$stamp = "$imgPath.source"
	$gzHash = (Get-FileHash $GzPath -Algorithm SHA256).Hash.ToLowerInvariant()

	# Re-expand only when the source archive changed.
	if ((Test-Path $imgPath) -and (Test-Path $stamp) -and (Get-Content $stamp -Raw).Trim() -eq $gzHash) {
		return $imgPath
	}

	Write-Host "Expanding $([IO.Path]::GetFileName($GzPath))..."
	$in = [IO.File]::OpenRead($GzPath)
	try {
		$gz = New-Object IO.Compression.GZipStream($in, [IO.Compression.CompressionMode]::Decompress)
		try {
			$out = [IO.File]::Create($imgPath)
			try { $gz.CopyTo($out) } finally { $out.Dispose() }
		} finally { $gz.Dispose() }
	} finally { $in.Dispose() }

	Set-Content -Path $stamp -Value $gzHash -NoNewline
	Write-Host "Expanded to $([math]::Round((Get-Item $imgPath).Length / 1MB, 1)) MB raw image."
	return $imgPath
}

<#
	Resolves the target physical disk and refuses anything that looks like an
	internal, system, or boot disk. A raw write to the wrong disk is unrecoverable.
#>
function Resolve-TargetDisk {
	if ($PSCmdlet.ParameterSetName -eq 'ByDisk') {
		$number = $DiskNumber
	} else {
		$partition = Get-Partition -DriveLetter $DriveLetter -ErrorAction SilentlyContinue
		if (-not $partition) {
			$usb = Get-Disk | Where-Object { $_.BusType -eq 'USB' }
			$hint = if ($usb) {
				'Removable disks present: ' + (($usb | ForEach-Object { "disk $($_.Number) ($($_.FriendlyName))" }) -join ', ')
			} else {
				'No USB disks are currently attached.'
			}
			throw "Drive ${DriveLetter}: was not found. $hint  Re-run with -DiskNumber <n> to target a disk directly."
		}
		$number = $partition.DiskNumber
	}

	$disk = Get-Disk -Number $number -ErrorAction Stop

	if ($disk.IsSystem -or $disk.IsBoot) {
		throw "Refusing to flash disk $number - it is the system/boot disk."
	}
	if ($disk.BusType -ne 'USB') {
		throw "Refusing to flash disk $number - bus type is '$($disk.BusType)', expected USB."
	}

	return $disk
}

<# Confirms the session is elevated; raw access to \\.\PhysicalDrive fails otherwise. #>
function Test-Elevated {
	$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
	return ([Security.Principal.WindowsPrincipal]$identity).IsInRole(
		[Security.Principal.WindowsBuiltInRole]::Administrator)
}

<#
	Prefers the standalone balena CLI. The npm build inherits the system Node, and
	Node < 22.20.0 cannot open physical drives on Windows.
#>
function Resolve-BalenaCommand {
	if (Test-Path $StandaloneBalena) {
		return $StandaloneBalena
	}

	$onPath = Get-Command balena -ErrorAction SilentlyContinue
	if (-not $onPath) {
		throw @"
balena CLI not found. Install the standalone build (bundles a patched Node):
  Invoke-WebRequest https://github.com/balena-io/balena-cli/releases/latest -OutFile ...
Or extract balena-cli-vX-windows-x64-standalone.tar.gz to:
  $env:LOCALAPPDATA\balena-cli
"@
	}

	$nodeVersion = (& node --version 2>$null) -replace '^v', ''
	if ($nodeVersion -and [version]$nodeVersion -lt [version]'22.20.0') {
		Write-Warning "Node $nodeVersion cannot open physical drives on Windows (nodejs/node#55623); the flash will likely fail with EIO. Install the standalone balena CLI to $env:LOCALAPPDATA\balena-cli."
	}
	return $onPath.Source
}

<#
	Reads the written bytes back off the raw device and compares them to the image.
	This is the only trustworthy success signal: balena can print an error and
	still exit zero, which previously let a failed flash report success.
#>
function Test-DiskMatchesImage {
	param(
		[Parameter(Mandatory)][int]$Number,
		[Parameter(Mandatory)][string]$ImagePath
	)

	$device = "\\.\PhysicalDrive$Number"
	$length = (Get-Item $ImagePath).Length

	Write-Host "Verifying $([math]::Round($length / 1MB, 1)) MB read back from $device..."

	$sha = [Security.Cryptography.SHA256]::Create()
	$disk = $null
	try {
		# FileShare ReadWrite: Windows keeps its own handle on the device open.
		$disk = New-Object IO.FileStream($device, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::ReadWrite)

		$chunk = 1MB
		$buffer = New-Object byte[] $chunk
		$remaining = $length

		while ($remaining -gt 0) {
			# Raw device reads must stay sector aligned, so always request full chunks.
			$read = $disk.Read($buffer, 0, $chunk)
			if ($read -le 0) { throw "Unexpected end of device after $($length - $remaining) bytes." }

			$use = [Math]::Min([long]$read, $remaining)
			$sha.TransformBlock($buffer, 0, [int]$use, $null, 0) | Out-Null
			$remaining -= $use
		}

		$sha.TransformFinalBlock((New-Object byte[] 0), 0, 0) | Out-Null
		$diskHash = ([BitConverter]::ToString($sha.Hash) -replace '-', '').ToLowerInvariant()
	} finally {
		if ($disk) { $disk.Dispose() }
		$sha.Dispose()
	}

	$imageHash = (Get-FileHash $ImagePath -Algorithm SHA256).Hash.ToLowerInvariant()
	return [pscustomobject]@{
		Match     = ($diskHash -eq $imageHash)
		DiskHash  = $diskHash
		ImageHash = $imageHash
	}
}

# --- main ---

$release = Get-LatestRelease
$commit = if ($release.target_commitish) {
	$release.target_commitish.Substring(0, [Math]::Min(7, $release.target_commitish.Length))
} else { 'unknown' }
Write-Host "Latest release: $($release.name) [$($release.tag_name)] from commit $commit, published $($release.published_at)"

$image = Expand-Image (Resolve-ReleaseAsset -Release $release)
Write-Host "Image ready: $image"

if ($DownloadOnly) {
	Write-Host 'Download-only mode; not flashing.'
	return
}

$balena = if ($VerifyOnly) { $null } else { Resolve-BalenaCommand }
$disk = Resolve-TargetDisk
$target = "\\.\PhysicalDrive$($disk.Number)"

Write-Host ''
Write-Host "Target: $target  disk $($disk.Number)  $($disk.FriendlyName)  $([math]::Round($disk.Size / 1GB, 2)) GB  ($($disk.BusType))"

# Checked before prompting so an unelevated run fails fast instead of after confirmation.
if (-not (Test-Elevated)) {
	if (-not $Elevate) {
		throw 'Raw disk access requires an elevated shell. Re-run from an Administrator PowerShell window, or add -Elevate to relaunch automatically.'
	}

	# Pass the resolved disk number so the elevated run cannot pick a different
	# disk if drive letters shift in between.
	$argv = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-NoExit', '-File', $PSCommandPath, '-DiskNumber', $disk.Number)
	if ($Yes) { $argv += '-Yes' }
	if ($VerifyOnly) { $argv += '-VerifyOnly' }
	if ($SkipVerify) { $argv += '-SkipVerify' }

	Write-Host 'Relaunching elevated (accept the UAC prompt)...'
	Start-Process -FilePath 'powershell.exe' -ArgumentList $argv -Verb RunAs
	return
}

if ($VerifyOnly) {
	$result = Test-DiskMatchesImage -Number $disk.Number -ImagePath $image
	if ($result.Match) {
		Write-Host "Disk $($disk.Number) already matches audiOS $commit." -ForegroundColor Green
		return
	}
	Write-Host "Disk $($disk.Number) does NOT match the latest image." -ForegroundColor Yellow
	Write-Host "  disk:  $($result.DiskHash)"
	Write-Host "  image: $($result.ImageHash)"
	exit 1
}

Write-Host 'This ERASES the entire disk.' -ForegroundColor Yellow

if (-not $Yes) {
	if ((Read-Host "Type the disk number ($($disk.Number)) to confirm") -ne "$($disk.Number)") {
		Write-Host 'Aborted.'
		return
	}
}

Write-Host 'Flashing...'
$global:LASTEXITCODE = 0
& $balena local flash $image --drive $target --yes 2>&1 | Tee-Object -Variable flashOutput | Write-Host
$flashExit = $LASTEXITCODE

# balena has been observed printing a fatal error while still exiting zero, so
# treat obvious error text as failure too. The read-back below is the real gate.
$flashText = ($flashOutput | Out-String)
if ($flashExit -ne 0 -or $flashText -match 'EIO:|EACCES|EPERM|Error:|error, open') {
	if ($SkipVerify) {
		throw "balena local flash failed (exit $flashExit). See output above."
	}
	Write-Warning "balena reported a problem (exit $flashExit). Checking the disk to confirm..."
}

if ($SkipVerify) {
	Write-Host ''
	Write-Host "Flash command completed (unverified). audiOS $commit -> disk $($disk.Number)."
	return
}

$result = Test-DiskMatchesImage -Number $disk.Number -ImagePath $image
if (-not $result.Match) {
	Write-Host "  disk:  $($result.DiskHash)"
	Write-Host "  image: $($result.ImageHash)"
	throw "FLASH FAILED - disk $($disk.Number) does not match the image. The drive is not bootable. Do not remove it; re-run the script."
}

Write-Host ''
Write-Host "Verified. audiOS $commit written to disk $($disk.Number)." -ForegroundColor Green
Write-Host 'Boot the ASRock from rear USB 2.0 (F11). PS/2 keyboard.'
