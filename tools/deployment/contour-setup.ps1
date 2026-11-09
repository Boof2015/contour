# App-owned development setup entry point. No caller-supplied paths, scripts,
# registry mutations, profile text, or service names cross the elevation boundary.
[CmdletBinding()]
param([Parameter(Mandatory=$true)][ValidateSet('Inspect','Install','Remove','Update','RecoverUpdate')][string]$Action,
      [Parameter(Mandatory=$true)][string]$EndpointId)
Set-StrictMode -Version Latest
$ErrorActionPreference='Stop'
$ProgressPreference='SilentlyContinue'
[Console]::OutputEncoding=[Text.UTF8Encoding]::new($false)
. (Join-Path $PSScriptRoot 'SetupHost.ps1')

$store=$null; $mutex=$null; $owns=$false
try {
    [void](Get-EndpointPath $EndpointId)
    if (-not [Environment]::Is64BitProcess) { throw 'Contour setup requires 64-bit Windows.' }
    if ($Action -ne 'Inspect') {
        $principal=[Security.Principal.WindowsPrincipal]::new([Security.Principal.WindowsIdentity]::GetCurrent())
        if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) { throw 'Windows administrator approval is required.' }
        $mutex=[Threading.Mutex]::new($false,'Global\ContourLabDeployment')
        try { $owns=$mutex.WaitOne(0) } catch [Threading.AbandonedMutexException] { $owns=$true }
        if (-not $owns) { throw 'Another Contour setup or removal is running.' }
    }
    $root=[Microsoft.Win32.RegistryKey]::OpenBaseKey([Microsoft.Win32.RegistryHive]::LocalMachine,[Microsoft.Win32.RegistryView]::Registry64)
    $store=[Contour.Deployment.RegistryStore]::new($root,($Action -ne 'Inspect'))
    $packages=@(Get-SetupPackages)
    if (@($packages | Where-Object { $_.State.Schema -eq 4 }).Count) {
        $removalError=[InvalidOperationException]::new('Contour removal was interrupted. Run the Contour uninstaller again to finish it.')
        $removalError.Data['reason']='product_removal';throw $removalError
    }
    $shared=@($packages | Where-Object { $_.State.Schema -eq 3 })
    if ($shared.Count -gt 1 -or ($shared.Count -and $packages.Count -ne 1)) { throw 'Conflicting Contour installations require recovery.' }
    $selected=@($packages | Where-Object { $_.State.Schema -ne 3 -and $_.State.EndpointId -ceq $EndpointId })
    if ($selected.Count -gt 1) { throw 'Multiple setup records exist for this output. Recover them before continuing.' }
    if ($Action -eq 'Inspect') {
        $report=[ordered]@{endpoint=$EndpointId;state='available';installation='not_installed';reason='';detail='Ready to install';canInstall=$false;canRemove=$false;package='';development=$true}
        $report.enablesUnsignedAudio=-not (Test-RawEqual ($store.Read($script:AudioSettings,'DisableProtectedAudioDG')) (Get-UnsignedAudioValue))
        $report.preservesAudioCompatibility=$true
        if ($shared.Count) { Set-SharedInspection $store $shared[0] $report }
        elseif ($selected.Count) {
            $p=$selected[0]; $report.package=$p.Path
            if ($p.State.Schema -eq 2) {
                $handoffStatus=Get-HandoffSetupStatus $p.State
                foreach ($key in $handoffStatus.Keys) { $report[$key]=$handoffStatus[$key] }
            } else {
                $report.canRemove=$true
                $attached=$p.State.State -eq 'Attached' -and -not $p.State.AudioRestartPending
                $a=Read-SetupJson (Join-Path $p.Path 'journal.json')
                $c=if (Test-Path -LiteralPath (Join-Path $p.Path 'context-journal.json')) { Read-SetupJson (Join-Path $p.Path 'context-journal.json') } else { $null }
                Assert-SetupJournals $p.State $a $c
                foreach ($change in $a.Plan.Changes) { if (-not (Test-RawEqual ($store.Read($change.Path,$change.Name)) $change.After)) { $attached=$false } }
                if ($null -eq $c -or $c.State -ne 'Provisioned') { $attached=$false }
                if ($null -ne $c) { foreach ($key in $c.Plan.Keys) { if (-not $store.Exists($key)) { $attached=$false } } }
                $report.state=if ($attached) { 'attached' } else { 'recovery' }
                $report.installation=if ($attached) { 'installed' } else { 'incomplete' }
                $report.detail=if ($attached) { 'Contour is installed on this device.' } else { 'The installation needs attention. Uninstall Contour from this device before trying again. '+$p.State.Error }
            }
        } elseif ($packages.Count -eq 1 -and $packages[0].State.Schema -eq 1 -and $packages[0].State.State -eq 'Attached' -and -not $packages[0].State.AudioRestartPending) {
            # Read-only preflight for adopting an existing single-device runtime.
            $p=$packages[0];$a=Read-SetupJson (Join-Path $p.Path 'journal.json');$c=Read-SetupJson (Join-Path $p.Path 'context-journal.json')
            [void](Get-LegacyEndpointRecord $store $a $c (Read-SetupAdapter $p.State.EndpointId))
            [void](Read-PackageInfo)
            $empty=[pscustomobject]@{Registrations=@()}
            $placement=Get-AvailableEffectPlacement $store $EndpointId
            Assert-EndpointAssignments $store $empty $EndpointId $null $placement
            [void](New-ControlContextPlan $store $EndpointId);[void](New-AudioPolicyJournal $store)
            $report.canInstall=$true
            if ($null -ne $placement) { $report.detail='Ready to install. Existing device effects will be kept.' }
        } elseif ($packages.Count) {
            $report.state='blocked';$report.reason='installation_recovery';$report.detail='Recover the existing Contour installation before adding another device.'
        } else {
            try { [void](New-SetupPlan $store); $report.canInstall=$true }
            catch { $report.state='blocked'; $report.reason=[string]$_.Exception.Data['reason']; $report.detail=$_.Exception.Message }
        }
        if ($packages.Count -eq 1 -and $packages[0].State.Schema -in @(1,3)) { Set-EngineInspection $store $packages[0] $report }
        $report | ConvertTo-Json -Compress
    } elseif ($Action -in @('Update','RecoverUpdate')) {
        if ($packages.Count -ne 1) { throw 'A single managed Contour installation is required.' }
        $p=$packages[0]
        if ($p.State.Schema -ne 3) { ConvertTo-SharedSetup $store $p.Path (Read-SetupAdapter $p.State.EndpointId) }
        $build=if ($Action -eq 'Update') { New-RuntimePackage $p.Path } else { $null }
        $callbacks=New-RuntimeCallbacks $p.Path
        Invoke-RuntimeUpdate $store $p.Path $build @callbacks
    } elseif ($Action -eq 'Install') {
        if ($selected.Count) { throw 'Contour is already installed on this device, or needs recovery.' }
        [void](Capture-SetupServices)
        if ($packages.Count) {
            if ($packages.Count -ne 1) { throw 'Recover conflicting installations first.' }
            $p=$packages[0];$info=Read-PackageInfo;$adapter=Read-SetupAdapter $EndpointId
            if ($p.State.Schema -ne 3) { ConvertTo-SharedSetup $store $p.Path (Read-SetupAdapter $p.State.EndpointId) }
            $callbacks=New-SharedCallbacks $store $p.Path
            Invoke-SharedDeviceSetup $store $p.Path Install $EndpointId $info.Endpoint.Name $adapter @callbacks
        } else {
            $plan=New-SetupPlan $store
            if (-not $store.CanWrite(((Get-EndpointPath $EndpointId)+'\FxProperties'))) { throw 'This output does not grant effect setup access. Its permissions were not changed.' }
            $state=New-SetupPackage $plan
            Invoke-DeviceSetup $store $state $plan.Package Install ${function:Capture-SetupServices} ${function:Stop-SetupAudio} ${function:Start-SetupAudio} -VerifyInstalled ${function:Test-InstalledSetup}
            if ($plan.Schema -eq 2) { ConvertTo-SharedSetup $store $plan.Package (Read-SetupAdapter $EndpointId) }
        }
    } else {
        if ($shared.Count) {
            $p=$shared[0];$callbacks=New-SharedCallbacks $store $p.Path
            Invoke-SharedDeviceSetup $store $p.Path Remove $EndpointId '' '' @callbacks
        } else {
            if ($selected.Count -ne 1) { throw 'No managed Contour installation was found for this output.' }
            Invoke-DeviceSetup $store $selected[0].State $selected[0].Path Remove ${function:Capture-SetupServices} ${function:Stop-SetupAudio} ${function:Start-SetupAudio}
        }
    }
} catch {
    if ($Action -eq 'Inspect') {
        $reason=[string]$_.Exception.Data['reason'];if (-not $reason) { $reason='inspection_failed' }
        @{endpoint=$EndpointId;state='blocked';installation=$(if ($reason -eq 'existing_effects') { 'not_installed' } else { 'unknown' });reason=$reason;detail=$_.Exception.Message;canInstall=$false;canRemove=$false;development=$true} | ConvertTo-Json -Compress
    }
    else { Write-Error $_ -ErrorAction Continue }
    exit 1
} finally {
    if ($null -ne $store) { $store.Dispose() }
    if ($owns) { $mutex.ReleaseMutex() }; if ($null -ne $mutex) { $mutex.Dispose() }
}
