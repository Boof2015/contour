# Provision only Contour's context, inspect administrator access, allow a
# separate ordinary-user probe, then restore it. No APO attachment or playback.
param([Parameter(Mandatory=$true)][string]$PlanFile,
      [Parameter(Mandatory=$true)][string]$OutputDirectory,
      [Parameter(Mandatory=$true)][string]$Probe)
Set-StrictMode -Version Latest
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot 'ControlContext.ps1')
$store=$null; $journal=$null; $mutex=$null; $ownsMutex=$false; $transcript=$false
$outcome=[ordered]@{Schema=1;Provisioned=$false;Restored=$true;BaselineMatches=$false;ParentSecurityMatches=$false;Error=$null}
try {
    $principal=[Security.Principal.WindowsPrincipal]::new([Security.Principal.WindowsIdentity]::GetCurrent())
    if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) { throw 'Context provisioning requires elevation.' }
    $OutputDirectory=[IO.Path]::GetFullPath($OutputDirectory)
    if (Test-Path -LiteralPath $OutputDirectory) { throw 'Use a new experiment directory.' }
    [void][IO.Directory]::CreateDirectory($OutputDirectory)
    Start-Transcript -Path (Join-Path $OutputDirectory 'experiment.log') | Out-Null; $transcript=$true
    $plan=Get-Content -LiteralPath $PlanFile -Raw | ConvertFrom-Json; Assert-ControlContextPlan $plan
    $mutex=[Threading.Mutex]::new($false,'Global\ContourLabDeployment')
    try { $ownsMutex=$mutex.WaitOne(0) } catch [Threading.AbandonedMutexException] { $ownsMutex=$true }
    if (-not $ownsMutex) { throw 'Another Contour deployment is active.' }
    $root=[Microsoft.Win32.RegistryKey]::OpenBaseKey([Microsoft.Win32.RegistryHive]::LocalMachine,[Microsoft.Win32.RegistryView]::Registry64)
    $store=[Contour.Deployment.RegistryStore]::new($root,$true)
    $fx=(Get-EndpointPath $plan.EndpointId)+'\FxProperties'
    foreach ($entry in $store.Entries($fx)) {
        foreach ($id in $script:ClassIds) {
            if ($entry.Value.Type -eq 1 -and $entry.Value.Data -ceq (ConvertTo-HexString $id)) { throw 'Remove the attached lab APO before the context-only experiment.' }
        }
    }
    $journal=New-ControlContextJournal $plan; $journalPath=Join-Path $OutputDirectory 'context-journal.json'
    Save-ContourJournal $journal $journalPath -CreateNew
    $outcome.Restored=$false
    Invoke-ControlContextApply $store $journal $journalPath -AllowRestorePrivilege
    $outcome.Provisioned=$true
    & $Probe inspect $plan.EndpointId (Join-Path $OutputDirectory 'administrator.json')
    if ($LASTEXITCODE -ne 0) { throw 'Administrator settings probe failed.' }
    [Contour.Deployment.DurableFile]::Save((Join-Path $OutputDirectory 'ready.json'),'{"Ready":true}',$true)
    $deadline=[DateTime]::UtcNow.AddSeconds(180)
    while (-not (Test-Path -LiteralPath (Join-Path $OutputDirectory 'finish'))) {
        if ([DateTime]::UtcNow -ge $deadline) { throw 'Ordinary-user probe window expired; restoring context.' }
        Start-Sleep -Milliseconds 250
    }
} catch { $outcome.Error=$_.Exception.Message; Write-Output $_ }
finally {
    if ($null -ne $journal) {
        try {
            $outcome.Restored=Invoke-ControlContextRestore $store $journal $journalPath
            $outcome.BaselineMatches=$store.Snapshot($fx) -ceq $plan.Baseline
            $outcome.ParentSecurityMatches=$store.Security($fx) -ceq $plan.ParentSecurity
            if (-not $outcome.Restored) { Write-Output $journal.Conflicts }
        } catch { $outcome.Restored=$false; $outcome.Error=$_.Exception.Message; Write-Output $_ }
    }
    if ($null -ne $store) { $store.Dispose() }
    if ($ownsMutex) { $mutex.ReleaseMutex() }; if ($null -ne $mutex) { $mutex.Dispose() }
    if ($transcript) {
        [Contour.Deployment.DurableFile]::Save((Join-Path $OutputDirectory 'outcome.json'),($outcome | ConvertTo-Json -Depth 8),$true)
        Stop-Transcript | Out-Null
    }
}
if ($outcome.Error -or -not $outcome.Restored -or -not $outcome.BaselineMatches -or -not $outcome.ParentSecurityMatches) { exit 1 }
