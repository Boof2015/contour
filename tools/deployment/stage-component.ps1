param(
    [Parameter(Mandatory=$true)][string] $BinaryDirectory,
    [Parameter(Mandatory=$true)][string] $OutputDirectory
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

# Staging only: no registry, PnP, certificates, endpoint, or service changes.
# Keep this separate from contour-setup until extension matching, signing,
# coexistence, and migration/removal have been validated on real hardware.
$binary = [IO.Path]::GetFullPath($BinaryDirectory)
$output = [IO.Path]::GetFullPath($OutputDirectory)
$dll = Join-Path $binary 'contour-apo.dll'
$inspector = Join-Path $binary 'contour-deploy-info.exe'
foreach ($file in @($dll, $inspector)) {
    if (-not (Test-Path -LiteralPath $file -PathType Leaf)) { throw "Missing build artifact: $file" }
}
if ([string]::Equals($dll, (Join-Path $output 'contour-apo.dll'), [StringComparison]::OrdinalIgnoreCase)) {
    throw 'Stage the component package separately from the build binaries.'
}
if (Test-Path -LiteralPath (Join-Path $output 'contour-apo.cat')) {
    throw 'A catalog already exists in the destination. Stage in a fresh directory so signed contents are not overwritten.'
}
$before = (Get-FileHash -LiteralPath $dll -Algorithm SHA256).Hash
$lines = @(& $inspector $dll '--component-inf')
if ($LASTEXITCODE -ne 0) { throw 'Could not generate component registration from this DLL.' }
$inf = ($lines -join "`r`n") + "`r`n"
if (-not $inf.Contains('Class=AudioProcessingObject') -or $inf -match '(?m)^HK(CR|LM|CU),') {
    throw 'Unexpected component registration output.'
}
[void][IO.Directory]::CreateDirectory($output)
Copy-Item -LiteralPath $dll -Destination (Join-Path $output 'contour-apo.dll') -Force
if ((Get-FileHash -LiteralPath (Join-Path $output 'contour-apo.dll') -Algorithm SHA256).Hash -ne $before) {
    throw 'APO changed during staging. Rebuild and stage again.'
}
# INF supports UTF-16 LE with BOM; avoid dependence on PowerShell's code page.
[IO.File]::WriteAllText((Join-Path $output 'contour-apo.inf'), $inf, [Text.Encoding]::Unicode)
Write-Output "Unsigned component package staged in $output"
Write-Output 'Pending: WDK INF validation, catalog/signing, driver-specific extension, and live migration/removal testing.'
