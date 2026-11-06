param(
    [Parameter(Mandatory=$true)][string]$DefinitionFile,
    [Parameter(Mandatory=$true)][string]$ComponentDirectory,
    [Parameter(Mandatory=$true)][string]$OutputDirectory,
    [string]$InventoryFile = ''
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'ExtensionPackage.ps1')
$definition = Get-Content -LiteralPath $DefinitionFile -Raw | ConvertFrom-Json
Assert-ExtensionDefinition $definition
$component = [IO.File]::ReadAllText((Join-Path ([IO.Path]::GetFullPath($ComponentDirectory)) 'contour-apo.inf'))
$inf = New-RegistrationExtensionInf $definition $component
$output = [IO.Path]::GetFullPath($OutputDirectory)
# Separate driver packages have separate catalogs. Require a clean directory
# so an earlier INF/catalog or scope report cannot leak into a new staging run.
if ((Test-Path -LiteralPath $output) -and @(Get-ChildItem -LiteralPath $output -Force).Count -gt 0) {
    throw 'Stage the extension into an empty directory.'
}
$report = $null
if ($InventoryFile) {
    $inventory = Get-Content -LiteralPath $InventoryFile -Raw | ConvertFrom-Json
    $report = New-ExtensionScopeReport $definition $inventory
}
[void][IO.Directory]::CreateDirectory($output)
[IO.File]::WriteAllText((Join-Path $output ('contour-'+$definition.Name+'.inf')), ($inf.TrimEnd()+"`r`n"), [Text.Encoding]::Unicode)
if ($null -ne $report) {
    [IO.File]::WriteAllText((Join-Path $output 'scope.json'), ($report | ConvertTo-Json -Depth 12), [Text.UTF8Encoding]::new($false))
}
Write-Output "Unsigned registration-only extension staged in $output"
Write-Output 'No endpoints were activated. Live installation still requires signing, migration/recovery, and hardware verification.'
