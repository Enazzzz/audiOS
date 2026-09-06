<#
.SYNOPSIS
	Raw-write audios.flp to a 1.44 MB floppy (A: or a USB floppy).

.DESCRIPTION
	The older BIOS machine boots this disk. Limine stage 1 is in the
	boot sector; the kernel is on the same floppy. Plug the USB stick
	(audios.img) for C:/D: after the kernel is running.

	Requires Administrator. USB floppy drives vary; a real FDC plus
	`floppy format` from audiOS is the reliable path when this write
	is rejected.

.PARAMETER DriveLetter
	Target drive letter without a colon. Defaults to A.

.PARAMETER Image
	Path to audios.flp. Defaults to .\audios.flp, then the latest GitHub release.

.PARAMETER Elevate
	Relaunch elevated if this session is not Administrator.
#>
[CmdletBinding()]
param(
	[ValidatePattern('^[A-Za-z]$')]
	[string]$DriveLetter = 'A',
	[string]$Image = '',
	[switch]$Elevate
)

$ErrorActionPreference = 'Stop'

function Test-IsAdmin {
	$p = [Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
	return $p.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

if ($Elevate -and -not (Test-IsAdmin)) {
	$here = $PSCommandPath
	$args = @("-NoProfile", "-ExecutionPolicy", "Bypass", "-File", $here, "-DriveLetter", $DriveLetter)
	if ($Image) { $args += @("-Image", $Image) }
	Start-Process -FilePath "powershell.exe" -Verb RunAs -ArgumentList $args | Out-Null
	return
}

if (-not (Test-IsAdmin)) {
	throw "Raw floppy write needs Administrator. Re-run with -Elevate."
}

if (-not $Image) {
	$local = Join-Path (Get-Location) "audios.flp"
	if (Test-Path $local) {
		$Image = $local
	} else {
		throw "audios.flp not found. Download it from the GitHub release or pass -Image."
	}
}

$bytes = [IO.File]::ReadAllBytes((Resolve-Path $Image))
if ($bytes.Length -ne 1474560) {
	throw "Image is $($bytes.Length) bytes; a 1.44 MB floppy is 1474560."
}

$vol = "${DriveLetter}:"
$phys = "\\.\${DriveLetter}:"
Write-Host "Writing $($bytes.Length) bytes to $phys  (this erases the floppy)"
$fs = [IO.File]::Open($phys, [IO.FileMode]::Open, [IO.FileAccess]::Write, [IO.FileShare]::ReadWrite)
try {
	$fs.Write($bytes, 0, $bytes.Length)
	$fs.Flush()
} finally {
	$fs.Close()
}
Write-Host "Done. Boot the old machine from $vol with the USB stick plugged in."
