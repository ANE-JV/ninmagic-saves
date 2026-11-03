[CmdletBinding()]
param(
    [switch]$PrepareAssets,
    [switch]$Preview,
    [string]$DevkitPro = 'C:\devkitPro',
    [ValidatePattern('^[0-9]+\.[0-9]+\.[0-9]+$')][string]$ExpectedVersion='0.2.4'
)

$ErrorActionPreference = 'Stop'
$projectRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$stageRoot = [IO.Path]::GetFullPath((Join-Path $env:USERPROFILE '.codex\tmp\ninmagic-saves-build'))
$safeParent = [IO.Path]::GetFullPath((Join-Path $env:USERPROFILE '.codex\tmp'))
if (-not $stageRoot.StartsWith($safeParent + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
    throw 'The build staging folder must be inside the Codex temporary directory.'
}
if ($stageRoot.Contains(' ')) {
    throw 'devkitPro make requires a staging path without spaces. Choose a user profile without spaces or change stageRoot.'
}
$bash = Join-Path $DevkitPro 'msys2\usr\bin\bash.exe'
if (-not (Test-Path -LiteralPath $bash -PathType Leaf)) {
    throw "devkitPro MSYS2 bash not found at $bash. Set -DevkitPro to the installed devkitPro folder."
}
if ($PrepareAssets) { throw 'Use the prepared Saves branding assets; no automatic regeneration is supported.' }

# Isolate make from project paths containing spaces. Only this validated, dedicated
# temporary folder is cleared; the connected console is never a build destination.
if (Test-Path -LiteralPath $stageRoot) {
    Remove-Item -LiteralPath $stageRoot -Recurse -Force
}
$null = New-Item -ItemType Directory -Path $stageRoot -Force
foreach ($name in @('source', 'include', 'assets', 'romfs', 'reader-prototype', 'Makefile')) {
    Copy-Item -LiteralPath (Join-Path $projectRoot $name) -Destination $stageRoot -Recurse
}
$posixStage = '/' + $stageRoot.Substring(0, 1).ToLowerInvariant() + $stageRoot.Substring(2).Replace('\', '/')
$posixDevkit = '/' + $DevkitPro.Substring(0, 1).ToLowerInvariant() + $DevkitPro.Substring(2).Replace('\', '/')
$jobs = [Math]::Min(8, [Math]::Max(1, [Environment]::ProcessorCount))
$buildCommand = "export DEVKITPRO='$posixDevkit'; export DEVKITA64=`"`$DEVKITPRO/devkitA64`"; export PATH=`"`$DEVKITPRO/tools/bin:`$PATH`"; cd '$posixStage' && make -j$jobs"
& $bash --login -c $buildCommand
if ($LASTEXITCODE -ne 0) { throw "NRO build failed (exit $LASTEXITCODE)." }
$outputFolder = Join-Path $projectRoot 'dist'
$null = New-Item -ItemType Directory -Path $outputFolder -Force
foreach ($extension in @('nro', 'elf', 'nacp')) {
    Copy-Item -LiteralPath (Join-Path $stageRoot "NinMagic-Saves.$extension") -Destination $outputFolder -Force
}
& python (Join-Path $PSScriptRoot 'verify_nro.py') (Join-Path $outputFolder 'NinMagic-Saves.nro') --expected-version $ExpectedVersion --output (Join-Path $outputFolder 'validation.json')
if ($LASTEXITCODE -ne 0) { throw 'NRO structural and embedded asset verification failed.' }
Write-Host "NRO ready: $(Join-Path $outputFolder 'NinMagic-Saves.nro')"

if ($Preview) {
    $previewFolder = Join-Path $projectRoot ('docs\preview-' + $ExpectedVersion)
    $null = New-Item -ItemType Directory -Path $previewFolder -Force
    $previewExe = Join-Path $stageRoot 'preview.exe'
    & (Join-Path $DevkitPro 'msys2\usr\bin\g++.exe') -std=c++17 -O2 -Wall -Wextra -Werror '-I' (Join-Path $projectRoot 'include') (Join-Path $PSScriptRoot 'preview.cpp') (Join-Path $projectRoot 'source\ui.cpp') -o $previewExe
    if ($LASTEXITCODE -ne 0) { throw 'Host UI preview compilation failed.' }
    & $previewExe (Join-Path $projectRoot 'romfs') $previewFolder
    if ($LASTEXITCODE -ne 0) { throw 'Host UI preview generation failed.' }
    & python -c 'from PIL import Image; from pathlib import Path; import sys; [Image.open(p).save(p.with_suffix(".png")) for p in Path(sys.argv[1]).glob("*.ppm")]' $previewFolder
    if ($LASTEXITCODE -ne 0) { throw 'Preview PNG conversion failed.' }
    foreach ($image in Get-ChildItem -LiteralPath $previewFolder -Filter '*.ppm' -File) { Remove-Item -LiteralPath $image.FullName -Force }
    Write-Host "UI preview ready: $previewFolder"
}
