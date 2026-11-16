# Builds Media Center Updater.
#   .\Publish.ps1              run the tests, then build the app into the App folder
#   .\Publish.ps1 -Installer   also build the setup program (needs Inno Setup 6)
#   .\Publish.ps1 -Dev         build a development copy that runs without administrator rights (see README.md)
param([switch]$SkipTests, [switch]$Installer, [switch]$Dev)
$ErrorActionPreference = 'Stop'
Push-Location $PSScriptRoot
try {
    # Load the Microsoft C compiler (x64) into this session unless a developer prompt already did.
    if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) {
        $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
        $vs = if (Test-Path $vswhere) { & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath }
        if (-not $vs) { throw 'The Microsoft C compiler was not found. Install Visual Studio or its Build Tools with "Desktop development with C++".' }
        $vcvars = Join-Path $vs 'VC\Auxiliary\Build\vcvars64.bat'
        cmd /c "`"$vcvars`" >nul 2>&1 && set" | ForEach-Object {
            if ($_ -match '^([^=]+)=(.*)$') { Set-Item -Path "env:$($Matches[1])" -Value $Matches[2] }
        }
        if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) { throw 'The Microsoft C compiler could not be loaded.' }
    }
    if (-not (Get-Command cmake.exe -ErrorAction SilentlyContinue)) { throw 'CMake was not found. Install it from https://cmake.org/download/ or with the Visual Studio installer.' }

    $buildDir = if ($Dev) { 'build\dev' } else { 'build\release' }
    $generator = if (Get-Command ninja.exe -ErrorAction SilentlyContinue) { 'Ninja' } else { 'NMake Makefiles' }
    $devOption = if ($Dev) { 'ON' } else { 'OFF' }
    cmake -S . -B $buildDir -G $generator -DCMAKE_BUILD_TYPE=Release "-DMCU_DEV=$devOption" | Out-Null
    if ($LASTEXITCODE -ne 0) { throw 'CMake could not prepare the build.' }
    cmake --build $buildDir
    if ($LASTEXITCODE -ne 0) { throw 'Build failed.' }

    if (-not $SkipTests) {
        & (Join-Path $buildDir 'tests.exe')
        if ($LASTEXITCODE -ne 0) { throw 'Verification failed.' }
    }
    if ($Dev) {
        Write-Host ('Run: ' + (Join-Path $PSScriptRoot "$buildDir\MediaCenterUpdater.exe"))
        return
    }

    $appDir = Join-Path $PSScriptRoot 'App'
    if (Test-Path $appDir) { Remove-Item -Recurse -Force $appDir }
    New-Item -ItemType Directory -Path $appDir | Out-Null
    Copy-Item (Join-Path $buildDir 'MediaCenterUpdater.exe') $appDir
    Write-Host ('Run: ' + (Join-Path $appDir 'MediaCenterUpdater.exe'))

    if ($Installer) {
        # Needs Inno Setup 6: https://jrsoftware.org/isinfo.php
        $iscc = @("${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe", "$env:ProgramFiles\Inno Setup 6\ISCC.exe") |
            Where-Object { Test-Path $_ } | Select-Object -First 1
        if (-not $iscc) { throw 'Inno Setup 6 was not found. Install it, or run without -Installer.' }
        if ((Get-Content 'src\version.h' -Raw) -notmatch '#define APP_VERSION "([^"]+)"') { throw 'The version was not found in src\version.h.' }
        $version = $Matches[1]
        & $iscc "/DMyAppVersion=$version" 'Installer\MediaCenterUpdater.iss'
        if ($LASTEXITCODE -ne 0) { throw 'Installer build failed.' }
        Write-Host ('Installer: ' + (Join-Path $PSScriptRoot "Installer\Output\MediaCenterUpdater-$version-Setup.exe"))
    }
}
finally { Pop-Location }
