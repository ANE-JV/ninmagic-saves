[CmdletBinding()]
param([string]$DevkitPro='C:\devkitPro')
$ErrorActionPreference='Stop'
$projectRoot=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$testRoot=[IO.Path]::GetFullPath((Join-Path $env:USERPROFILE '.codex/tmp/ninmagic-saves-tests'))
$null=New-Item -ItemType Directory -Path $testRoot -Force
$cxx=Join-Path $DevkitPro 'msys2/usr/bin/g++.exe'
$cc=Join-Path $DevkitPro 'msys2/usr/bin/gcc.exe'
$nro=Join-Path $projectRoot 'dist/NinMagic-Saves.nro'
foreach ($required in @($cxx,$cc,$nro)) {
    if (-not (Test-Path -LiteralPath $required -PathType Leaf)) { throw "Missing: $required. Build the NRO first with tools/build.ps1." }
}
function Invoke-Checked([string]$ToolPath,[string[]]$ToolArguments) {
    # Cygwin GCC accepts a backslash filename but does not reliably derive its
    # parent for quoted relative includes. Normalize paths so tests include
    # this project's implementation, never a sibling project's same-name file.
    $normalizedArguments=@($ToolArguments | ForEach-Object { $_.Replace('\','/') })
    & $ToolPath @normalizedArguments
    if ($LASTEXITCODE -ne 0) { throw "Verification failed: $ToolPath (exit $LASTEXITCODE)." }
}
$previousMsys=$env:MSYS
try {
    # MSYS's default symlink() may copy the file. This per-process setting
    # represents links as .lnk-backed links understood by the MSYS runtime.
    # It does not change Windows permissions or omit the symlink test cases.
    $env:MSYS='winsymlinks:lnk'
    $include=Join-Path $projectRoot 'include'
    $base=@('-std=c++17','-O2','-Wall','-Wextra','-Werror','-I',$include)
    $updateExe=Join-Path $testRoot 'app-update-test.exe'
    Invoke-Checked $cxx ($base+@((Join-Path $projectRoot 'tests/app_update_test.cpp'),'-o',$updateExe))
    $hash=(Get-FileHash -LiteralPath $nro -Algorithm SHA256).Hash.ToLowerInvariant()
    Invoke-Checked $updateExe @($nro,$hash)
    $networkExe=Join-Path $testRoot 'update-network-test.exe'
    Invoke-Checked $cxx ($base+@((Join-Path $projectRoot 'tests/update_network_test.cpp'),'-o',$networkExe))
    Invoke-Checked $networkExe @()
    $uiExe=Join-Path $testRoot 'ui-policy-test.exe'
    Invoke-Checked $cxx ($base+@((Join-Path $projectRoot 'tests/ui_policy_test.cpp'),'-o',$uiExe))
    Invoke-Checked $uiExe @()
    $transferExe=Join-Path $testRoot 'save-transfer-test.exe'
    Invoke-Checked $cxx ($base+@((Join-Path $projectRoot 'tests/save_transfer_test.cpp'),(Join-Path $projectRoot 'source/save_transfer.cpp'),'-o',$transferExe))
    Invoke-Checked $transferExe @()
    $rebootExe=Join-Path $testRoot 'reboot-workflow-test.exe'
    Invoke-Checked $cxx ($base+@((Join-Path $projectRoot 'tests/reboot_workflow_test.cpp'),'-o',$rebootExe))
    Invoke-Checked $rebootExe @()
    $readerExe=Join-Path $testRoot 'reader-client-test.exe'
    Invoke-Checked $cxx ($base+@((Join-Path $projectRoot 'tests/reader_client_test.cpp'),(Join-Path $projectRoot 'source/reader_client.cpp'),'-o',$readerExe))
    Invoke-Checked $readerExe @()
    $probeExe=Join-Path $testRoot 'reader-probe-test.exe'
    Invoke-Checked $cc @('-std=c11','-O2','-Wall','-Wextra','-Werror','-I',(Join-Path $projectRoot 'reader-prototype/include'),(Join-Path $projectRoot 'reader-prototype/source/probe.c'),(Join-Path $projectRoot 'reader-prototype/experimental/fs_original_accessor_bridge.c'),(Join-Path $projectRoot 'reader-prototype/tests/test_probe.c'),'-o',$probeExe)
    Invoke-Checked $probeExe @()
    Write-Host 'All host suites passed; no checks skipped. Reboot/export/import remain unverified on physical hardware.'
} finally {
    if ($null -eq $previousMsys) { Remove-Item Env:MSYS -ErrorAction SilentlyContinue }
    else { $env:MSYS=$previousMsys }
}
