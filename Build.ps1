param([ValidateSet('Debug','Release')][string]$Configuration='Release')
$ErrorActionPreference='Stop'
$root=$PSScriptRoot
& cmake -S $root -B (Join-Path $root 'obj/build') -G 'Visual Studio 17 2022' -A x64
if($LASTEXITCODE){throw 'CMake configuration failed'}
& cmake --build (Join-Path $root 'obj/build') --config $Configuration --parallel
if($LASTEXITCODE){throw 'C++ build failed'}
& ctest --test-dir (Join-Path $root 'obj/build') -C $Configuration --output-on-failure
if($LASTEXITCODE){throw 'Offline model tests failed'}
