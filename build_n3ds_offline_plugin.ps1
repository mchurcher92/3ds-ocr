# 3DS CTRPF New 3DS Offline OCR Plugin Build Script
$ErrorActionPreference = "Stop"

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Definition
$CtrpfLibDir = Join-Path $ScriptDir "CTRPluginFramework_Hb\Library"
$PluginDir = Join-Path $ScriptDir "3gx_plugin_n3ds_offline"
$PluginLibCtrpfDir = Join-Path $PluginDir "libctrpf"
$TargetName = "default.3gx"
$OutDir = Join-Path $ScriptDir "n3ds_offline"
$LocalTarget = Join-Path $OutDir $TargetName
$LocalAlias = Join-Path $OutDir "default_n3ds_offline.3gx"

Write-Host "=== Step 1: Syncing Homebrew-Patched libctrpf Headers & Static Library ==="
$BuiltLib = Join-Path $CtrpfLibDir "lib\libctrpf.a"
if (-not (Test-Path $BuiltLib)) {
    Write-Host "Building CTRPluginFramework_Hb/Library/lib/libctrpf.a..."
    docker run --rm -v "${ScriptDir}:/workspace" -w /workspace/CTRPluginFramework_Hb/Library pablomk7/libctrpf:0.8.0 sh -c "make clean && make -j8 lib/libctrpf.a"
    if ($LASTEXITCODE -ne 0) {
        Write-Error "libctrpf build failed with exit code $LASTEXITCODE"
        exit $LASTEXITCODE
    }
}
Copy-Item -Path (Join-Path $CtrpfLibDir "include\*") -Destination (Join-Path $PluginLibCtrpfDir "include") -Recurse -Force
Copy-Item -Path $BuiltLib -Destination (Join-Path $PluginLibCtrpfDir "lib\libctrpf.a") -Force

Write-Host "=== Step 2: Building N3DS Offline OCR 3GX Plugin ($TargetName) ==="
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

Write-Host "=== Step 3: Patching allowHomebrewLoad Flag (0x200) in 3GX Header ==="
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

Write-Host "=== Step 4: Deploying to n3ds_offline/ Directory ==="
if (-not (Test-Path $OutDir)) {
    New-Item -ItemType Directory -Path $OutDir -Force | Out-Null
}
Copy-Item -Path $BuiltFile -Destination $LocalTarget -Force
Copy-Item -Path $BuiltFile -Destination $LocalAlias -Force
$LocalSize = (Get-Item $LocalTarget).Length
Write-Host "Successfully deployed to: $LocalTarget ($LocalSize bytes)"
Write-Host "Successfully deployed alias: $LocalAlias"

$SlimParam = Join-Path $ScriptDir "slim\user_param.bin"
$LocalParam = Join-Path $OutDir "user_param.bin"
if (Test-Path $SlimParam) {
    Copy-Item -Path $SlimParam -Destination $LocalParam -Force
}

Write-Host "=== N3DS Offline Plugin Build Complete! ==="
