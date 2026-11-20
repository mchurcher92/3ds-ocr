# 3DS CTRPF Full Plugin Build and Google Drive Sync Script
$ErrorActionPreference = "Stop"

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Definition
$PluginDir = Join-Path $ScriptDir "3gx_plugin"
$TargetName = "default.3gx"
$LocalFullDir = Join-Path $ScriptDir "full"
$LocalTarget = Join-Path $LocalFullDir $TargetName
$GDriveRoot = if ($env:GDRIVE_DIR) { $env:GDRIVE_DIR } else { $null }
$GDriveFullDir = if ($GDriveRoot) { Join-Path $GDriveRoot "full" } else { $null }
$GDriveTarget = if ($GDriveFullDir) { Join-Path $GDriveFullDir $TargetName } else { $null }

Write-Host "=== Building 3DS CTRPF Full Plugin ($TargetName) ==="

# Run libctrpf docker builder
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

# 1. Deploy to local full directory
if (-not (Test-Path $LocalFullDir)) {
    New-Item -ItemType Directory -Path $LocalFullDir -Force | Out-Null
}
Copy-Item -Path $BuiltFile -Destination $LocalTarget -Force
$LocalSize = (Get-Item $LocalTarget).Length
Write-Host "Successfully deployed to local workspace: $LocalTarget ($LocalSize bytes)"

# Copy or generate user_param.bin in local full dir
$SlimParam = Join-Path $ScriptDir "slim\user_param.bin"
$LocalParam = Join-Path $LocalFullDir "user_param.bin"
if (Test-Path $SlimParam) {
    Copy-Item -Path $SlimParam -Destination $LocalParam -Force
} else {
    $UserParamBytes = New-Object byte[] 396
    [System.Text.Encoding]::ASCII.GetBytes("PERS").CopyTo($UserParamBytes, 0)
    $UserParamBytes[5] = 1  # PLG_STRATEGY_MODE3
    $UserParamBytes[6] = 1  # persistent
    [System.Text.Encoding]::ASCII.GetBytes("/luma/plugins/default.3gx").CopyTo($UserParamBytes, 12)
    [System.IO.File]::WriteAllBytes($LocalParam, $UserParamBytes)
}

# 2. Deploy to Google Drive if mounted or configured
if ($GDriveRoot -and (Test-Path $GDriveRoot)) {
    try {
        if (-not (Test-Path $GDriveFullDir)) {
            New-Item -ItemType Directory -Path $GDriveFullDir -Force | Out-Null
        }
        Copy-Item -Path $BuiltFile -Destination $GDriveTarget -Force
        Copy-Item -Path $BuiltFile -Destination (Join-Path $GDriveRoot $TargetName) -Force
        $GDriveParam = Join-Path $GDriveFullDir "user_param.bin"
        if (Test-Path $LocalParam) {
            Copy-Item -Path $LocalParam -Destination $GDriveParam -Force
            Copy-Item -Path $LocalParam -Destination (Join-Path $GDriveRoot "user_param.bin") -Force
        }
        $GDriveSize = (Get-Item $GDriveTarget).Length
        Write-Host "Successfully synced to Google Drive: $GDriveTarget ($GDriveSize bytes)"
        Write-Host "Successfully synced to Google Drive root: $(Join-Path $GDriveRoot $TargetName)"
    } catch {
        Write-Warning "Could not copy to Google Drive: $_"
    }
} elseif ($env:GDRIVE_DIR) {
    Write-Warning "Google Drive directory ($env:GDRIVE_DIR) is not accessible."
}

Write-Host "=== Build and Deployment Complete! ==="
