param([switch]$SkipTests)
$ErrorActionPreference = 'Stop'
Push-Location $PSScriptRoot
try {
    if (-not $SkipTests) {
        dotnet run --project 'Tests\MediaCenterUpdater.Tests.csproj' -c Release
        if ($LASTEXITCODE -ne 0) { throw 'Verification failed.' }
    }
    dotnet publish 'MediaCenterUpdater.csproj' -c Release -r win-x64 --self-contained true -o (Join-Path $PSScriptRoot 'App')
    if ($LASTEXITCODE -ne 0) { throw 'Publish failed.' }
    Write-Host ('Run: ' + (Join-Path $PSScriptRoot 'App\MediaCenterUpdater.exe'))
}
finally { Pop-Location }
