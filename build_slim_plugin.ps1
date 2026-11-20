# 3DS CTRPF Slim Plugin Build and Google Drive Sync Script
$ErrorActionPreference = "Stop"

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Definition
$PluginDir = Join-Path $ScriptDir "3gx_plugin_slim"
$TargetName = "default.3gx"
$LocalSlimDir = Join-Path $ScriptDir "slim"
$LocalTarget = Join-Path $LocalSlimDir $TargetName
$GDriveRoot = if ($env:GDRIVE_DIR) { $env:GDRIVE_DIR } else { $null }
$GDriveSlimDir = if ($GDriveRoot) { Join-Path $GDriveRoot "slim" } else { $null }
$GDriveTarget = if ($GDriveSlimDir) { Join-Path $GDriveSlimDir $TargetName } else { $null }

Write-Host "=== Building 3DS CTRPF Slim Plugin ($TargetName) ==="

docker run --rm -v "${PluginDir}:/build" -w /build pablomk7/libctrpf:0.8.0 make clean all
if ($LASTEXITCODE -ne 0) {
    Write-Error "Docker build failed with exit code $LASTEXITCODE"
    exit $LASTEXITCODE
}

$BuiltFile = Join-Path $PluginDir $TargetName
if (-not (Test-Path $BuiltFile)) {
    Write-Error "Expected build artifact not found: $BuiltFile"
    exit 1
}

# 1. Deploy to local .\slim\default.3gx and .\slim\default_slim.3gx
if (-not (Test-Path $LocalSlimDir)) {
    New-Item -ItemType Directory -Path $LocalSlimDir -Force | Out-Null
}

# Generate user_param.bin for Old 3DS Mode 3 / Extended Memory Mode (Smash Bros, Monster Hunter 4U)
$UserParamBytes = New-Object byte[] 396
[System.Text.Encoding]::ASCII.GetBytes("PERS").CopyTo($UserParamBytes, 0)
$UserParamBytes[5] = 1  # PLG_STRATEGY_MODE3
$UserParamBytes[6] = 1  # persistent
[System.Text.Encoding]::ASCII.GetBytes("/luma/plugins/default.3gx").CopyTo($UserParamBytes, 12)
[System.IO.File]::WriteAllBytes((Join-Path $LocalSlimDir "user_param.bin"), $UserParamBytes)

Copy-Item -Path $BuiltFile -Destination $LocalTarget -Force
Copy-Item -Path $BuiltFile -Destination (Join-Path $LocalSlimDir "default_slim.3gx") -Force
$LocalSize = (Get-Item $LocalTarget).Length
Write-Host "Successfully deployed to: $LocalTarget ($LocalSize bytes)"

# 2. Deploy to Google Drive (slim\ and root) if mounted or configured
if ($GDriveRoot -and (Test-Path $GDriveRoot)) {
    try {
        if (-not (Test-Path $GDriveSlimDir)) {
            New-Item -ItemType Directory -Path $GDriveSlimDir -Force | Out-Null
        }
        Copy-Item -Path $BuiltFile -Destination $GDriveTarget -Force
        Copy-Item -Path $BuiltFile -Destination "$GDriveSlimDir\default_slim.3gx" -Force
        Copy-Item -Path $BuiltFile -Destination (Join-Path $GDriveRoot "default_slim.3gx") -Force
        $LocalParam = Join-Path $LocalSlimDir "user_param.bin"
        $GDriveParam = Join-Path $GDriveSlimDir "user_param.bin"
        if (Test-Path $LocalParam) {
            Copy-Item -Path $LocalParam -Destination $GDriveParam -Force
        }
        $GDriveSize = (Get-Item $GDriveTarget).Length
        Write-Host "Successfully synced to Google Drive: $GDriveTarget ($GDriveSize bytes)"
        Write-Host "Successfully synced to Google Drive root: $(Join-Path $GDriveRoot "default_slim.3gx")"
        Write-Host "Successfully synced user_param.bin to Google Drive: $GDriveParam"
    } catch {
        Write-Warning "Could not copy to Google Drive: $_"
    }
} elseif ($env:GDRIVE_DIR) {
    Write-Warning "Google Drive directory ($env:GDRIVE_DIR) is not accessible."
}

Write-Host "=== Build and Deployment Complete! ==="
