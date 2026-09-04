# Builds audios.iso via WSL (Ubuntu).
$ErrorActionPreference = "Stop"
$RepoRoot = (Resolve-Path "$PSScriptRoot\..").Path
$WslPath = "/mnt/c/Robotics/newer-non-robotics/audiOS"

Write-Host "Building audios.iso in WSL ($WslPath)..."
wsl -e bash -lc "sed -i 's/\r$//' '$WslPath/tools/build-iso.sh' && bash '$WslPath/tools/build-iso.sh'"
if ($LASTEXITCODE -ne 0) { throw "WSL build failed with exit code $LASTEXITCODE." }

$iso = Join-Path $RepoRoot "audios.iso"
if (-not (Test-Path $iso)) {
	throw "Build finished but audios.iso was not found."
}

$sizeMb = [math]::Round((Get-Item $iso).Length / 1MB, 2)
Write-Host "Done: $iso ($sizeMb MB)"
