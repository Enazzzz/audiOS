<#
.SYNOPSIS
	Updates C: (the 64 MiB system volume) on an audiOS USB without touching D:.

.DESCRIPTION
	Downloads the latest GitHub release image (or uses -Image) and copies only
	partition 1 onto the stick. The MBR and partition 2 (D:, leftover data)
	are left alone.

	This is the script to run after the first install. A full raw flash
	(flash-latest-release.ps1 -Full, Etcher, Rufus DD) rewrites the MBR and
	drops D:.

	Raw disk access requires an elevated (Administrator) PowerShell session.

.PARAMETER DriveLetter
	Windows letter of the USB (usually the data volume). Defaults to D.
	Resolved to \\.\PhysicalDriveN.

.PARAMETER DiskNumber
	Target a physical disk directly when Windows has not assigned a letter.

.PARAMETER Image
	Path to a local audios.img. Skips the GitHub download.

.PARAMETER ReleaseTag
	GitHub release tag to fetch instead of /releases/latest (e.g. build-42).

.PARAMETER Force
	Re-download even when the cached copy already matches the release digest.

.PARAMETER DownloadOnly
	Fetch and verify the image but skip writing.

.PARAMETER VerifyOnly
	Skip writing; check whether partition 1 already matches the image.

.PARAMETER SkipVerify
	Write without the read-back comparison. Not recommended.

.PARAMETER DryRun
	Parse MBR layout and print what would be written.

.PARAMETER Yes
	Skip the interactive confirmation.

.PARAMETER Elevate
	Relaunch in an elevated window (single UAC prompt).

.EXAMPLE
	.\tools\update-system.ps1 -Elevate
	Updates C: on whatever USB currently holds D:, from the latest release.

.EXAMPLE
	.\tools\update-system.ps1 -Image .\audios.img -DiskNumber 1 -Elevate
	Writes a local image's system partition onto disk 1.
#>
[CmdletBinding(DefaultParameterSetName = 'ByLetter')]
param(
	[Parameter(ParameterSetName = 'ByLetter')]
	[ValidatePattern('^[A-Za-z]$')]
	[string]$DriveLetter = 'D',

	[Parameter(ParameterSetName = 'ByDisk', Mandatory = $true)]
	[int]$DiskNumber,

	[string]$Image,
	[string]$ReleaseTag,
	[switch]$Force,
	[switch]$DownloadOnly,
	[switch]$VerifyOnly,
	[switch]$SkipVerify,
	[switch]$DryRun,
	[switch]$Yes,
	[switch]$Elevate
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$Repo = 'Enazzzz/audiOS'
$AssetName = 'audios.img.gz'
$CacheDir = Join-Path $env:LOCALAPPDATA 'audiOS-releases'
$Sector = 512
$MbrOff = 446

$ApiHeaders = @{
	'User-Agent'           = 'audiOS-update-script'
	'Accept'               = 'application/vnd.github+json'
	'X-GitHub-Api-Version' = '2022-11-28'
}

<# Fetch a published release (latest, or a specific tag). #>
function Get-Release {
	if ($ReleaseTag) {
		Write-Host "Checking release '$ReleaseTag' of $Repo..."
		return Invoke-RestMethod -Uri "https://api.github.com/repos/$Repo/releases/tags/$ReleaseTag" -Headers $ApiHeaders
	}
	Write-Host "Checking latest release of $Repo..."
	return Invoke-RestMethod -Uri "https://api.github.com/repos/$Repo/releases/latest" -Headers $ApiHeaders
}

<# Return the cached .gz, downloading when missing or the digest changed. #>
function Resolve-ReleaseAsset {
	param([Parameter(Mandatory)]$Release)

	$asset = $Release.assets | Where-Object { $_.name -eq $AssetName }
	if (-not $asset) {
		$available = ($Release.assets | ForEach-Object { $_.name }) -join ', '
		throw "Release '$($Release.tag_name)' has no asset named '$AssetName'. Available: $available"
	}

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
	$ProgressPreference = 'SilentlyContinue'
	try {
		Invoke-WebRequest -Uri $asset.browser_download_url -OutFile $path -Headers @{ 'User-Agent' = 'audiOS-update-script' }
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

<# Expand the .gz into the cache when the source archive changed. #>
function Expand-Image {
	param([Parameter(Mandatory)][string]$GzPath)

	$imgPath = $GzPath -replace '\.gz$', ''
	$stamp = "$imgPath.source"
	$gzHash = (Get-FileHash $GzPath -Algorithm SHA256).Hash.ToLowerInvariant()

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

<# Read one MBR slot (type, LBA, sector count). #>
function Get-MbrPart {
	param(
		[Parameter(Mandatory)][byte[]]$Mbr,
		[Parameter(Mandatory)][int]$Slot
	)
	$off = $MbrOff + ($Slot * 16)
	[pscustomobject]@{
		Type    = [int]$Mbr[$off + 4]
		Lba     = [BitConverter]::ToUInt32($Mbr, $off + 8)
		Sectors = [BitConverter]::ToUInt32($Mbr, $off + 12)
	}
}

<# SHA256 of a byte array as lowercase hex. #>
function Get-BytesSha256 {
	param([Parameter(Mandatory)][byte[]]$Bytes)
	$sha = [Security.Cryptography.SHA256]::Create()
	try {
		$hash = $sha.ComputeHash($Bytes)
		return ([BitConverter]::ToString($hash) -replace '-', '').ToLowerInvariant()
	} finally {
		$sha.Dispose()
	}
}

<# Open \\.\PhysicalDriveN with share-readwrite (Windows keeps its own handle). #>
function Open-PhysicalDrive {
	param(
		[Parameter(Mandatory)][int]$Number,
		[Parameter(Mandatory)][IO.FileAccess]$Access
	)
	$device = "\\.\PhysicalDrive$Number"
	return New-Object IO.FileStream($device, [IO.FileMode]::Open, $Access, [IO.FileShare]::ReadWrite)
}

<# Read exactly $Count bytes at $Offset from the raw disk. #>
function Read-DiskBytes {
	param(
		[Parameter(Mandatory)][int]$Number,
		[Parameter(Mandatory)][long]$Offset,
		[Parameter(Mandatory)][int]$Count
	)
	$fs = Open-PhysicalDrive -Number $Number -Access Read
	try {
		[void]$fs.Seek($Offset, [IO.SeekOrigin]::Begin)
		$buf = New-Object byte[] $Count
		$got = 0
		while ($got -lt $Count) {
			$n = $fs.Read($buf, $got, $Count - $got)
			if ($n -le 0) {
				throw "Unexpected end of disk $($Number) after $got bytes (wanted $Count at offset $Offset)."
			}
			$got += $n
		}
		return $buf
	} finally {
		$fs.Dispose()
	}
}

<# Resolve the USB disk and refuse system/boot/internal drives. #>
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
			throw "Drive ${DriveLetter}: was not found. $hint  Re-run with -DiskNumber <n>."
		}
		$number = $partition.DiskNumber
	}

	$disk = Get-Disk -Number $number -ErrorAction Stop
	if ($disk.IsSystem -or $disk.IsBoot) {
		throw "Refusing disk $number - it is the system/boot disk."
	}
	if ($disk.BusType -ne 'USB') {
		throw "Refusing disk $number - bus type is '$($disk.BusType)', expected USB."
	}
	return $disk
}

<# True when this PowerShell session is Administrator. #>
function Test-Elevated {
	$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
	return ([Security.Principal.WindowsPrincipal]$identity).IsInRole(
		[Security.Principal.WindowsBuiltInRole]::Administrator)
}

<# Plan the C:-only copy. Never writes past the existing system partition. #>
function Get-SystemUpdatePlan {
	param(
		[Parameter(Mandatory)][string]$ImagePath,
		[Parameter(Mandatory)][byte[]]$DestMbr
	)

	$imgLen = (Get-Item $ImagePath).Length
	if ($imgLen -lt 4096) {
		throw "Image is too small to be an MBR disk."
	}

	$srcMbr = New-Object byte[] $Sector
	$img = [IO.File]::OpenRead($ImagePath)
	try {
		[void]$img.Read($srcMbr, 0, $Sector)
	} finally {
		$img.Dispose()
	}
	if ($srcMbr[510] -ne 0x55 -or $srcMbr[511] -ne 0xAA) {
		throw "Image is not an MBR disk (missing 0x55AA)."
	}
	if ($DestMbr.Length -lt $Sector -or $DestMbr[510] -ne 0x55 -or $DestMbr[511] -ne 0xAA) {
		throw "Destination has no MBR. First install needs a full flash: .\tools\flash-latest-release.ps1 -Full -Elevate"
	}

	$src = Get-MbrPart $srcMbr 0
	$dst = Get-MbrPart $DestMbr 0
	$data = Get-MbrPart $DestMbr 1
	if ($src.Lba -eq 0 -or $src.Sectors -eq 0) {
		throw "Image has no partition 1."
	}
	if ($dst.Lba -eq 0 -or $dst.Sectors -eq 0) {
		throw "Destination has no partition 1."
	}
	if ($data.Lba -eq 0 -or $data.Sectors -eq 0) {
		throw "No data partition on the stick. First install is a full flash (flash-latest-release.ps1 -Full). This script is for updating C: without wiping D:."
	}

	$n = [Math]::Min([uint32]$src.Sectors, [uint32]$dst.Sectors)
	$srcOff = [long]$src.Lba * $Sector
	$dstOff = [long]$dst.Lba * $Sector
	$bytes = [long]$n * $Sector
	if (($srcOff + $bytes) -gt $imgLen) {
		throw "Image is shorter than partition 1."
	}

	return [pscustomobject]@{
		SrcType   = $src.Type
		DstType   = $dst.Type
		SrcOff    = $srcOff
		DstOff    = $dstOff
		Sectors   = $n
		Bytes     = $bytes
		DataLba   = $data.Lba
		DataSecs  = $data.Sectors
		ImagePath = $ImagePath
	}
}

<# Load the image's system-partition bytes. #>
function Read-ImageSlice {
	param(
		[Parameter(Mandatory)][string]$Path,
		[Parameter(Mandatory)][long]$Offset,
		[Parameter(Mandatory)][int]$Count
	)
	$fs = [IO.File]::OpenRead($Path)
	try {
		[void]$fs.Seek($Offset, [IO.SeekOrigin]::Begin)
		$buf = New-Object byte[] $Count
		$got = 0
		while ($got -lt $Count) {
			$n = $fs.Read($buf, $got, $Count - $got)
			if ($n -le 0) { throw "Image ended early at offset $Offset." }
			$got += $n
		}
		return $buf
	} finally {
		$fs.Dispose()
	}
}

<# Drop Windows volume handles so the raw write is not cached against a mounted FAT. #>
function Dismount-DiskVolumes {
	param([Parameter(Mandatory)][int]$Number)
	Get-Partition -DiskNumber $Number -ErrorAction SilentlyContinue | ForEach-Object {
		if ($_.DriveLetter) {
			try {
				Dismount-Volume -DriveLetter $_.DriveLetter -Force -Confirm:$false -ErrorAction Stop
				Write-Host "Dismounted $($_.DriveLetter):"
			} catch {
				Write-Warning "Could not dismount $($_.DriveLetter):: $_"
			}
		}
	}
}

# --- main ---

$label = $null
if ($Image) {
	if (-not (Test-Path -LiteralPath $Image)) {
		throw "Missing image $Image"
	}
	$imagePath = (Resolve-Path -LiteralPath $Image).Path
	$label = [IO.Path]::GetFileName($imagePath)
} else {
	$release = Get-Release
	$commit = if ($release.target_commitish) {
		$release.target_commitish.Substring(0, [Math]::Min(7, $release.target_commitish.Length))
	} else { 'unknown' }
	$label = "$($release.tag_name) / $commit"
	Write-Host "Release: $($release.name) [$($release.tag_name)] from commit $commit, published $($release.published_at)"
	$imagePath = Expand-Image (Resolve-ReleaseAsset -Release $release)
}

Write-Host "Image ready: $imagePath"

if ($DownloadOnly) {
	Write-Host 'Download-only mode; not writing.'
	return
}

$disk = Resolve-TargetDisk
Write-Host ''
Write-Host "Target: \\.\PhysicalDrive$($disk.Number)  disk $($disk.Number)  $($disk.FriendlyName)  $([math]::Round($disk.Size / 1GB, 2)) GB  ($($disk.BusType))"
Write-Host 'This writes C: only. D: (partition 2) is not touched.' -ForegroundColor Green

if (-not (Test-Elevated)) {
	if (-not $Elevate) {
		throw 'Raw disk access requires an elevated shell. Re-run from Administrator PowerShell, or add -Elevate.'
	}
	$argv = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-NoExit', '-File', $PSCommandPath, '-DiskNumber', $disk.Number)
	if ($Image) { $argv += @('-Image', $imagePath) }
	if ($ReleaseTag) { $argv += @('-ReleaseTag', $ReleaseTag) }
	if ($Yes) { $argv += '-Yes' }
	if ($VerifyOnly) { $argv += '-VerifyOnly' }
	if ($SkipVerify) { $argv += '-SkipVerify' }
	if ($DryRun) { $argv += '-DryRun' }
	Write-Host 'Relaunching elevated (accept the UAC prompt)...'
	Start-Process -FilePath 'powershell.exe' -ArgumentList $argv -Verb RunAs
	return
}

$destMbr = Read-DiskBytes -Number $disk.Number -Offset 0 -Count $Sector
$plan = Get-SystemUpdatePlan -ImagePath $imagePath -DestMbr $destMbr
Write-Host ("system type 0x{0:x2}->0x{1:x2}  dest LBA {2}  {3} sectors  ({4} KiB)" -f `
	$plan.SrcType, $plan.DstType, ($plan.DstOff / $Sector), $plan.Sectors, ($plan.Bytes / 1024))
Write-Host "leaving partition 2 at LBA $($plan.DataLba) ($($plan.DataSecs) sectors) untouched"

$payload = Read-ImageSlice -Path $plan.ImagePath -Offset $plan.SrcOff -Count $plan.Bytes
$wantHash = Get-BytesSha256 $payload

if ($DryRun) {
	Write-Host 'dry-run: no write'
	return
}

if ($VerifyOnly) {
	$have = Read-DiskBytes -Number $disk.Number -Offset $plan.DstOff -Count $plan.Bytes
	$haveHash = Get-BytesSha256 $have
	if ($haveHash -eq $wantHash) {
		Write-Host "Disk $($disk.Number) C: already matches $label." -ForegroundColor Green
		return
	}
	Write-Host "Disk $($disk.Number) C: does NOT match the image." -ForegroundColor Yellow
	Write-Host "  disk:  $haveHash"
	Write-Host "  image: $wantHash"
	exit 1
}

if (-not $Yes) {
	$prompt = "Type the disk number ($($disk.Number)) to update C: only"
	if ((Read-Host $prompt) -ne "$($disk.Number)") {
		Write-Host 'Aborted.'
		return
	}
}

Dismount-DiskVolumes -Number $disk.Number

Write-Host "Writing C: ($([math]::Round($plan.Bytes / 1MB, 1)) MB)..."
$out = Open-PhysicalDrive -Number $disk.Number -Access ReadWrite
try {
	[void]$out.Seek($plan.DstOff, [IO.SeekOrigin]::Begin)
	$out.Write($payload, 0, $payload.Length)
	$out.Flush()
} finally {
	$out.Dispose()
}

if ($SkipVerify) {
	Write-Host "Wrote C: (unverified). $label -> disk $($disk.Number)."
	Write-Host 'Reboot the FX board into the new kernel. D: was not touched.'
	return
}

$have = Read-DiskBytes -Number $disk.Number -Offset $plan.DstOff -Count $plan.Bytes
$haveHash = Get-BytesSha256 $have
if ($haveHash -ne $wantHash) {
	Write-Host "  disk:  $haveHash"
	Write-Host "  image: $wantHash"
	throw "UPDATE FAILED - C: on disk $($disk.Number) does not match the image. D: was not rewritten. Re-run the script."
}

Write-Host ''
Write-Host "Verified. C: updated to $label on disk $($disk.Number). D: untouched." -ForegroundColor Green
Write-Host 'Reboot the ASRock from rear USB 2.0 (F11). PS/2 keyboard.'
