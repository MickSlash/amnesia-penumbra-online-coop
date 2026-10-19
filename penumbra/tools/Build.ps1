# SPDX-License-Identifier: GPL-3.0-or-later
param([string]$Generator='Visual Studio 18 2026',[string]$Toolset='v145',[ValidateSet('Release','Debug')][string]$Configuration='Release')
$ErrorActionPreference='Stop'
$root=(Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$build=Join-Path $root 'build'
$sdk=Join-Path $build 'upstream/penumbra_vr'
$revision='7d7071c74455409c2b6a58dce935b1efc0d5d51d'
New-Item -ItemType Directory -Force (Join-Path $build 'upstream') | Out-Null
if(!(Test-Path -LiteralPath (Join-Path $sdk '.git'))) {
    & git clone --filter=blob:none --no-checkout https://github.com/veryjos/penumbra_vr.git $sdk
    if($LASTEXITCODE-ne0) {throw 'Dependency clone failed'}
    & git -C $sdk sparse-checkout init --cone
    if($LASTEXITCODE-ne0) {throw 'Sparse checkout setup failed'}
    & git -C $sdk sparse-checkout set dependencies/include dependencies/lib/win32
    if($LASTEXITCODE-ne0) {throw 'Dependency selection failed'}
    & git -C $sdk checkout --detach $revision
    if($LASTEXITCODE-ne0) {throw 'Dependency checkout failed'}
}
$actual=(& git -c "safe.directory=$($sdk.Replace('\','/'))" -C $sdk rev-parse HEAD).Trim()
if($actual-ne $revision) {throw 'Unexpected dependency revision'}
$deps=Join-Path $build 'dependencies'
New-Item -ItemType Directory -Force $deps,(Join-Path $deps 'lib') | Out-Null
Copy-Item -LiteralPath (Join-Path $sdk 'dependencies/include') -Destination $deps -Recurse -Force
Copy-Item -LiteralPath (Join-Path $sdk 'dependencies/lib/win32') -Destination (Join-Path $deps 'lib') -Recurse -Force
$cmake=$null
$vswhere=Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
if(Test-Path -LiteralPath $vswhere) {
    $cmake=(& $vswhere -latest -products '*' -find Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe | Select-Object -First 1)
}
if(!$cmake) { $cmake=(Get-Command cmake -ErrorAction SilentlyContinue).Source }
if(!$cmake) {throw 'CMake not found'}
& $cmake -S $root -B (Join-Path $build 'windows') -G $Generator -A Win32 -T $Toolset
if($LASTEXITCODE-ne0) {throw 'CMake configuration failed'}
& $cmake --build (Join-Path $build 'windows') --config $Configuration --parallel 8
if($LASTEXITCODE-ne0) {throw 'Game build failed'}
$compiler=Join-Path $env:WINDIR 'Microsoft.NET/Framework/v4.0.30319/csc.exe'
$launcher=Join-Path $build 'PenumbraCoopLauncher.exe'
& $compiler /nologo /target:winexe /optimize+ "/out:$launcher" /r:System.Windows.Forms.dll /r:System.Drawing.dll (Join-Path $PSScriptRoot 'CoopLauncher.cs')
if($LASTEXITCODE-ne0) {throw 'Launcher build failed'}
Write-Output $launcher
