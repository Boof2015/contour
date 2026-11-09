[CmdletBinding()]
param([switch]$Installer)
Set-StrictMode -Version Latest
$ErrorActionPreference='Stop'
$root=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
Set-Location -LiteralPath $root
function Check-Exit([string]$Operation) {
    if ($LASTEXITCODE -ne 0) { throw ($Operation+' failed with exit code '+$LASTEXITCODE) }
}

# Use the same toolset as the bundled VC runtime, even when the runner's default
# compiler changes. Both VS 2022 and newer installations can supply this toolset.
$vswhere=Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$installations=@(& $vswhere -all -products '*' -property installationPath)
Check-Exit 'Visual Studio discovery'
$visualStudio=$installations | Where-Object {
    Test-Path (Join-Path $_ 'VC\Tools\MSVC\14.44.*')
} | Select-Object -First 1
if (-not $visualStudio) { throw 'MSVC 14.44 x64 tools are required.' }
Import-Module (Join-Path $visualStudio 'Common7\Tools\Microsoft.VisualStudio.DevShell.dll')
Enter-VsDevShell -VsInstallPath $visualStudio -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64 -vcvars_ver=14.44 -winsdk=10.0.26100.0'
if ($env:VCToolsVersion -notlike '14.44.*' -or $env:WindowsSDKVersion.TrimEnd('\') -ne '10.0.26100.0') {
    throw 'The requested compiler / Windows SDK environment could not be selected.'
}
Get-Command cl.exe,ninja.exe,cmake.exe,ctest.exe,node.exe -ErrorAction Stop | Format-Table Name,Source
$node=(Get-Command node.exe).Source
$edge=@(
    (Join-Path ${env:ProgramFiles(x86)} 'Microsoft\Edge\Application\msedge.exe'),
    (Join-Path $env:ProgramFiles 'Microsoft\Edge\Application\msedge.exe')
) | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
if (-not $edge) { throw 'Microsoft Edge is required for the UI interaction tests.' }
$env:CONTOUR_TEST_BROWSER=$edge
$tools=& (Join-Path $PSScriptRoot 'prepare-tools.ps1') -Installer:$Installer
& cmake --preset ci-windows-x64 "-DCONTOUR_NODE_EXECUTABLE=$node" "-DCONTOUR_ISCC=$($tools.iscc)" "-DCONTOUR_PREREQUISITE_DIRECTORY=$($tools.prerequisites)"
Check-Exit 'Configure'
& cmake --build --preset ci-windows-x64-release --parallel 4
Check-Exit 'Build'
& ctest --preset ci-windows-x64-release --output-junit (Join-Path $root 'build\ci\test-results.xml')
Check-Exit 'Tests'
if (-not $Installer) { return }

& cmake --build --preset ci-windows-x64-release --target contour-installer --parallel 4
Check-Exit 'Installer build'
$manifest=Get-Content 'build/ci/distribution/Release/Contour-*/release.json' -Raw | ConvertFrom-Json
if (@($manifest).Count -ne 1) { throw 'Use a clean CI build directory when changing the project version.' }
$commit=(& git rev-parse HEAD | Out-String).Trim()
Check-Exit 'Source revision'
if ($env:GITHUB_ACTIONS -eq 'true' -and $commit -ne $env:GITHUB_SHA) { throw 'Checkout and workflow revisions differ.' }
$artifact=Join-Path $root ('build\ci\artifacts\'+[guid]::NewGuid().ToString('D'))
[void][IO.Directory]::CreateDirectory($artifact)
$filename='Contour-'+$manifest.version+'-Setup-x64.exe'
foreach ($suffix in @('','.sha256','.json')) {
    Copy-Item -LiteralPath (Join-Path $root ('build\ci\distribution\installer\'+$filename+$suffix)) -Destination $artifact
}
$record=[ordered]@{
    schema=1;version=$manifest.version;commit=$commit;configuration='Release';architecture='x64'
    toolset=$env:VCToolsVersion;sdk=$env:WindowsSDKVersion.TrimEnd('\')
}
[IO.File]::WriteAllText((Join-Path $artifact 'build-info.json'),($record | ConvertTo-Json),[Text.UTF8Encoding]::new($false))
if ($env:GITHUB_OUTPUT) {
    [IO.File]::AppendAllText($env:GITHUB_OUTPUT,('version='+$manifest.version+"`nartifact-directory="+$artifact+"`n"),[Text.UTF8Encoding]::new($false))
}
Write-Host ('Installer files: '+$artifact)
