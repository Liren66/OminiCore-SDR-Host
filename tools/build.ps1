param([string]$Configuration='Release')
$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot -Parent
$env:PATH='C:\msys64\mingw64\bin;'+$env:PATH
& cmake -S $root -B "$root/build" -G Ninja "-DCMAKE_BUILD_TYPE=$Configuration" -DCMAKE_C_COMPILER=gcc -DCMAKE_CXX_COMPILER=g++
if($LASTEXITCODE){throw 'CMake configure failed'}
& cmake --build "$root/build" -j 6
if($LASTEXITCODE){throw 'Build failed'}
& cmake --install "$root/build" --prefix "$root/build/stage"
if($LASTEXITCODE){throw 'Install staging failed'}
Write-Host "Built OCH and staged runtime/development files in $root/build/stage"
