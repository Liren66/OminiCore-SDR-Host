param(
    [ValidateSet('rx','tx-zero','duplex-zero')][string]$Mode='rx',
    [string]$Serial='',
    [uint32]$SampleRate=30720000,
    [ValidateRange(1,50)][int]$Rounds=2,
    [switch]$RfControls,
    [switch]$BuildOnly
)
$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot -Parent
$build=Join-Path $root 'build'
$compiler='C:\msys64\mingw64\bin\g++.exe'
$env:PATH='C:\msys64\mingw64\bin;'+$env:PATH
if(-not (Test-Path -LiteralPath (Join-Path $build 'omnicore.dll'))){throw 'Build the updated OCH first with tools/build.ps1'}
$program=Join-Path $build 'hardware_reconfigure.exe'
# Explicit MinGW import library avoids selecting a stale MSVC omnicore.lib.
& $compiler -std=c++17 -Wall -Wextra -Wpedantic -O2 -static '-I' (Join-Path $root 'include') (Join-Path $PSScriptRoot 'hardware_reconfigure.cpp') (Join-Path $build 'libomnicore.dll.a') '-o' $program
if($LASTEXITCODE){throw 'Hardware reconfiguration harness compilation failed'}
if($BuildOnly){Write-Host "Built $program (no hardware operation executed)"; return}
$runArgs=@("--$Mode",'--sample-rate',"$SampleRate",'--rounds',"$Rounds")
if($Serial){$runArgs+=@('--serial',$Serial)}
if($RfControls){$runArgs+='--rf-controls'}
# RX is the default. TX is only selected through an explicit -Mode value.
& $program @runArgs
exit $LASTEXITCODE
