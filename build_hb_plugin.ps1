# 3DS CTRPF Homebrew-Compatible Plugin Build Script
$ErrorActionPreference = "Stop"

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Definition
$CtrpfLibDir = Join-Path $ScriptDir "CTRPluginFramework_Hb\Library"
$PluginDir = Join-Path $ScriptDir "3gx_plugin_hb"
$PluginLibCtrpfDir = Join-Path $PluginDir "libctrpf"
$TargetName = "default.3gx"
$LocalHbDir = Join-Path $ScriptDir "homebrew"
$LocalTarget = Join-Path $LocalHbDir $TargetName
$LocalAlias = Join-Path $LocalHbDir "default_hb.3gx"
$GDriveRoot = if ($env:GDRIVE_DIR) { $env:GDRIVE_DIR } else { $null }
$GDriveHbDir = if ($GDriveRoot) { Join-Path $GDriveRoot "homebrew" } else { $null }

Write-Host "=== Step 1: Building Homebrew-Patched libctrpf.a ==="
docker run --rm -v "${ScriptDir}:/workspace" -w /workspace/CTRPluginFramework_Hb/Library pablomk7/libctrpf:0.8.0 sh -c "make clean && make -j8 lib/libctrpf.a"
if ($LASTEXITCODE -ne 0) {
    Write-Error "libctrpf build failed with exit code $LASTEXITCODE"
    exit $LASTEXITCODE
}

$BuiltLib = Join-Path $CtrpfLibDir "lib\libctrpf.a"
if (-not (Test-Path $BuiltLib)) {
    Write-Error "Expected library not found: $BuiltLib"
    exit 1
}

Write-Host "=== Step 2: Syncing Patched libctrpf Headers & Static Library ==="
Copy-Item -Path (Join-Path $CtrpfLibDir "include\*") -Destination (Join-Path $PluginLibCtrpfDir "include") -Recurse -Force
Copy-Item -Path $BuiltLib -Destination (Join-Path $PluginLibCtrpfDir "lib\libctrpf.a") -Force

Write-Host "=== Step 3: Building Homebrew OCR 3GX Plugin ($TargetName) ==="
docker run --rm -v "${PluginDir}:/build" -w /build pablomk7/libctrpf:0.8.0 sh -c "make clean && make -j8 all"
if ($LASTEXITCODE -ne 0) {
    Write-Error "Plugin build failed with exit code $LASTEXITCODE"
    exit $LASTEXITCODE
}

$BuiltFile = Join-Path $PluginDir $TargetName
if (-not (Test-Path $BuiltFile)) {
    Write-Error "Expected build artifact not found: $BuiltFile"
    exit 1
}

Write-Host "=== Step 4: Patching allowHomebrewLoad Flag (0x200) in 3GX Header ==="
$bytes = [System.IO.File]::ReadAllBytes($BuiltFile)
$magic = [System.Text.Encoding]::ASCII.GetString($bytes, 0, 8)
if ($magic -ne '3GX$0002') {
    Write-Error "Unexpected 3GX magic '$magic' in $BuiltFile"
    exit 1
}

$oldFlags = [System.BitConverter]::ToUInt32($bytes, 0x30)
$newFlags = $oldFlags -bor 0x200
$flagBytes = [System.BitConverter]::GetBytes([uint32]$newFlags)
[Array]::Copy($flagBytes, 0, $bytes, 0x30, 4)
[System.IO.File]::WriteAllBytes($BuiltFile, $bytes)
Write-Host ("Patched _3gx_Infos.flags @ 0x30: 0x{0:X8} -> 0x{1:X8}" -f $oldFlags, $newFlags)

Write-Host "=== Step 5: Deploying to Local homebrew/ Directory ==="
if (-not (Test-Path $LocalHbDir)) {
    New-Item -ItemType Directory -Path $LocalHbDir -Force | Out-Null
}
Copy-Item -Path $BuiltFile -Destination $LocalTarget -Force
Copy-Item -Path $BuiltFile -Destination $LocalAlias -Force
$LocalSize = (Get-Item $LocalTarget).Length
Write-Host "Successfully deployed to: $LocalTarget ($LocalSize bytes)"
Write-Host "Successfully deployed alias: $LocalAlias"

$SlimParam = Join-Path $ScriptDir "slim\user_param.bin"
$LocalParam = Join-Path $LocalHbDir "user_param.bin"
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

if ($GDriveHbDir -and (Test-Path $GDriveRoot)) {
    try {
        if (-not (Test-Path $GDriveHbDir)) {
            New-Item -ItemType Directory -Path $GDriveHbDir -Force | Out-Null
        }
        Copy-Item -Path $BuiltFile -Destination (Join-Path $GDriveHbDir $TargetName) -Force
        Copy-Item -Path $BuiltFile -Destination (Join-Path $GDriveHbDir "default_hb.3gx") -Force
        Write-Host "Synced homebrew plugin to Google Drive: $GDriveHbDir"
    } catch {
        Write-Warning "Could not copy to Google Drive: $_"
    }
}

Write-Host "=== Homebrew Plugin Build Complete! ==="
