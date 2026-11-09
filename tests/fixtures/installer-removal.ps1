# Only compiled into the isolated per-user installer fixture. This helper has
# no registry, service, device, or user-profile operations.
param([ValidateSet('Remove')][string]$Action,[switch]$PlainText,[switch]$DeleteUserData)
$ErrorActionPreference='Stop'
$control=Join-Path (Split-Path (Split-Path $PSScriptRoot -Parent) -Parent) 'control'
if (-not (Test-Path -LiteralPath (Join-Path $control 'fixture'))) { throw 'Missing installer fixture marker.' }
[IO.File]::AppendAllText((Join-Path $control 'removal-calls'),"attempt`r`n")
if (Test-Path -LiteralPath (Join-Path $control 'removal-fail')) {
    Write-Output 'Simulated engine removal failure. Retry after recovery.'
    exit 1
}
[IO.File]::WriteAllText((Join-Path $control 'removed'),'done')
if ($DeleteUserData) {
    [IO.File]::Delete((Join-Path $control 'profile'))
    [IO.File]::WriteAllText((Join-Path $control 'data-removed'),'done')
}
Write-Output 'Test engine removed.'
