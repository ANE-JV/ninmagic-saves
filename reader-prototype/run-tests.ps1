$ErrorActionPreference = 'Stop'
$prototypeRoot = $PSScriptRoot
$outputDir = Join-Path $prototypeRoot 'build'
New-Item -ItemType Directory -Path $outputDir -Force | Out-Null
$testExe = Join-Path $outputDir 'test_probe.exe'
& gcc -std=c11 -Wall -Wextra -Werror -pedantic -O2 `
    -I (Join-Path $prototypeRoot 'include') `
    (Join-Path $prototypeRoot 'source\probe.c') `
    (Join-Path $prototypeRoot 'experimental\fs_original_accessor_bridge.c') `
    (Join-Path $prototypeRoot 'tests\test_probe.c') -o $testExe
if ($LASTEXITCODE -ne 0) { throw 'Offline prototype compilation failed.' }
& $testExe
if ($LASTEXITCODE -ne 0) { throw 'Offline prototype tests failed.' }
