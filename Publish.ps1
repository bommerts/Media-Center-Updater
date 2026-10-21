param([switch]$SkipTests, [switch]$Installer)
$ErrorActionPreference = 'Stop'
Push-Location $PSScriptRoot
try {
    if (-not $SkipTests) {
        dotnet run --project 'Tests\MediaCenterUpdater.Tests.csproj' -c Release
        if ($LASTEXITCODE -ne 0) { throw 'Verification failed.' }
    }
    $appDir = Join-Path $PSScriptRoot 'App'
    if (Test-Path $appDir) { Remove-Item -Recurse -Force $appDir }
    dotnet publish 'MediaCenterUpdater.csproj' -c Release -r win-x64 --self-contained true -o $appDir
    if ($LASTEXITCODE -ne 0) { throw 'Publish failed.' }
    Write-Host ('Run: ' + (Join-Path $appDir 'MediaCenterUpdater.exe'))

    if ($Installer) {
        # Needs Inno Setup 6: https://jrsoftware.org/isinfo.php
        $iscc = @("${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe", "$env:ProgramFiles\Inno Setup 6\ISCC.exe") |
            Where-Object { Test-Path $_ } | Select-Object -First 1
        if (-not $iscc) { throw 'Inno Setup 6 was not found. Install it, or run without -Installer.' }
        $version = ([xml](Get-Content 'MediaCenterUpdater.csproj')).Project.PropertyGroup.Version | Where-Object { $_ } | Select-Object -First 1
        & $iscc "/DMyAppVersion=$version" 'Installer\MediaCenterUpdater.iss'
        if ($LASTEXITCODE -ne 0) { throw 'Installer build failed.' }
        Write-Host ('Installer: ' + (Join-Path $PSScriptRoot "Installer\Output\MediaCenterUpdater-$version-Setup.exe"))
    }
}
finally { Pop-Location }
