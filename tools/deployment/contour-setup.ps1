# App-owned development setup entry point. No caller-supplied paths, scripts,
# registry mutations, profile text, or service names cross the elevation boundary.
[CmdletBinding()]
param([Parameter(Mandatory=$true)][ValidateSet('Inspect','Install','Remove')][string]$Action,
      [Parameter(Mandatory=$true)][string]$EndpointId)
Set-StrictMode -Version Latest
$ErrorActionPreference='Stop'
$ProgressPreference='SilentlyContinue'
[Console]::OutputEncoding=[Text.UTF8Encoding]::new($false)
. (Join-Path $PSScriptRoot 'SharedSetup.ps1')

function Assert-NoLink([string]$Path) {
    $item=Get-Item -LiteralPath $Path -Force
    if ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw 'Setup does not follow redirected package paths.' }
}
function Assert-ProtectedSetup([string]$Path) {
    $acl=Get-Acl -LiteralPath $Path
    $trusted=@('S-1-5-18','S-1-5-32-544')
    if ($trusted -notcontains $acl.GetOwner([Security.Principal.SecurityIdentifier]).Value) { throw 'Setup recovery data has an unexpected owner.' }
    $write=[Security.AccessControl.FileSystemRights]::Write -bor [Security.AccessControl.FileSystemRights]::Delete -bor
        [Security.AccessControl.FileSystemRights]::ChangePermissions -bor [Security.AccessControl.FileSystemRights]::TakeOwnership
    foreach ($rule in $acl.GetAccessRules($true,$true,[Security.Principal.SecurityIdentifier])) {
        if ($rule.AccessControlType -eq 'Allow' -and ($rule.FileSystemRights -band $write) -and $trusted -notcontains $rule.IdentityReference.Value) {
            throw 'Setup recovery data is writable outside administrators and SYSTEM.'
        }
    }
}
function Get-SetupPackages {
    $base=[Environment]::GetFolderPath('ProgramFiles')
    Assert-NoLink $base
    foreach ($dir in Get-ChildItem -LiteralPath $base -Directory -Filter 'ContourLab-*') {
        $record=Join-Path $dir.FullName 'device-setup.json'
        if (-not (Test-Path -LiteralPath $record)) { continue }
        Assert-NoLink $dir.FullName; Assert-NoLink $record; Assert-ProtectedSetup $dir.FullName
        $state=Read-SetupJson $record; Assert-SetupState $state $dir.FullName
        # Validate all files we may later read/replace, including recovery data.
        foreach ($file in Get-ChildItem -LiteralPath $dir.FullName -Force) { Assert-NoLink $file.FullName; Assert-ProtectedSetup $file.FullName }
        if ($state.State -ne 'Removed' -or $state.AudioRestartPending) {
            [pscustomobject]@{Path=$dir.FullName;State=$state}
        }
    }
}
function Read-PackageInfo([string]$SelectedEndpoint=$EndpointId) {
    $binary=Join-Path (Split-Path $PSScriptRoot -Parent) 'contour-deploy-info.exe'
    $dll=Join-Path (Split-Path $PSScriptRoot -Parent) 'contour-apo.dll'
    $output=& $binary $dll $SelectedEndpoint
    if ($LASTEXITCODE -ne 0) { throw 'Cannot inspect this output and the packaged APO.' }
    $output | ConvertFrom-Json
}
function Read-SetupAdapter([string]$SelectedEndpoint) {
    $binary=Join-Path (Split-Path $PSScriptRoot -Parent) 'contour-deploy-info.exe'
    $output=& $binary --device-inventory $SelectedEndpoint
    if ($LASTEXITCODE -ne 0) { throw 'Cannot identify the audio device. Reconnect it and check again.' }
    $inventory=$output | ConvertFrom-Json
    if ($inventory.EndpointId -ine $SelectedEndpoint) { throw 'Device inspection returned another endpoint.' }
    Assert-AdapterId $inventory.AdapterId
    return $inventory.AdapterId
}
function New-SharedCallbacks($Store,[string]$Package) {
    $checker=Join-Path (Split-Path $PSScriptRoot -Parent) 'contour-endpoint-check.exe'
    if (-not (Test-Path -LiteralPath $checker -PathType Leaf)) { throw 'Device verification files are missing. Rebuild Contour.' }
    $verify={param($SelectedEndpoint,$Registration,$Binding)
        Assert-SharedRegistration $Store $Package $Registration
        $plan=(Read-SetupJson (Join-Path $Package 'journal.json')).Plan
        if ((Get-FileHash -LiteralPath $plan.TargetDll -Algorithm SHA256).Hash -cne $plan.DllSha256) { throw 'The installed APO file changed.' }
        $true
    }.GetNewClosure()
    $applied={param($SelectedEndpoint,$Registration,$Binding)
        $mode=if ($null -eq $Binding) { 'absent' } else { 'present' }
        $result=& $checker $mode $SelectedEndpoint
        if ($LASTEXITCODE -ne 0) { return $false }
        $report=$result | ConvertFrom-Json
        return ($report.verified -is [bool] -and $report.verified)
    }.GetNewClosure()
    @{VerifyRegistration=$verify;VerifyApplied=$applied;CaptureServices=${function:Capture-SetupServices};
        StopAudio=${function:Stop-SetupAudio};StartAudio=${function:Start-SetupAudio}}
}
function Test-InstalledSetup([string]$SelectedEndpoint) {
    $checker=Join-Path (Split-Path $PSScriptRoot -Parent) 'contour-endpoint-check.exe'
    $result=& $checker present $SelectedEndpoint
    if ($LASTEXITCODE -ne 0) { return $false }
    $report=$result | ConvertFrom-Json
    return ($report.verified -is [bool] -and $report.verified)
}
function Set-SharedInspection($Store,$Package,$Report) {
    $record=$Package.State;$catalog=Read-DeploymentState (Join-Path $Package.Path 'endpoints.json')
    Assert-SharedCatalog $catalog $record
    $Report.package=$Package.Path
    if ($null -ne $catalog.Operation -and $catalog.Operation.EndpointId -ieq $EndpointId) {
        $Report.state='recovery';$Report.installation='incomplete';$Report.canRemove=$true
        $Report.detail='An interrupted device operation needs recovery. Uninstall Contour from the affected device before continuing.'
        return
    }
    $binding=Get-Binding $catalog $EndpointId
    $context=Get-SharedContext $record $EndpointId
    if ($null -ne $binding) {
        $Report.canRemove=$true
        try {
            Assert-SharedRegistration $Store $Package.Path (Get-Registration $catalog $record.Id)
            Assert-EndpointAssignments $Store $catalog $EndpointId $binding
            [void](Get-EndpointSettings $Store $EndpointId)
            $Report.state='attached';$Report.installation='installed';$Report.detail='Contour is installed on this device.'
        } catch { $Report.state='recovery';$Report.installation='incomplete';$Report.detail=$_.Exception.Message }
    } elseif ($null -ne $context -and $context.ContextState -eq 'Provisioning') {
        $Report.state='recovery';$Report.installation='incomplete';$Report.canRemove=$true
        $Report.detail='Installation was interrupted. Uninstall it from this device before trying again.'
    } else {
        [void](Read-PackageInfo)
        Assert-SharedRegistration $Store $Package.Path (Get-Registration $catalog $record.Id)
        $placement=Get-AvailableEffectPlacement $Store $EndpointId
        Assert-EndpointAssignments $Store $catalog $EndpointId $null $placement
        [void](New-AudioPolicyJournal $Store)
        if ($null -ne $context -and $context.ContextState -eq 'Ready') { [void](Get-EndpointSettings $Store $EndpointId) }
        else { [void](New-ControlContextPlan $Store $EndpointId) }
        $Report.canInstall=$true
        if ($null -ne $placement) { $Report.detail='Ready to install. Existing device effects will be kept.' }
    }
    if ($null -ne $catalog.Operation) {
        # Another endpoint owns the interrupted transaction. Keep this row's
        # actual installation status, but serialize all setup changes.
        $Report.canInstall=$false;$Report.canRemove=$false
        $Report.detail='Another device has an interrupted installation. Uninstall Contour from that device before making further installation changes.'
    }
}
function New-SetupPlan($Store) {
    $root=Split-Path $PSScriptRoot -Parent
    foreach ($name in @('contour-apo.dll','contour-deploy-info.exe','contour-apo-host.exe','contour-endpoint-check.exe')) {
        if (-not (Test-Path -LiteralPath (Join-Path $root $name))) { throw 'The setup package is incomplete. Rebuild the Contour target.' }
    }
    $dll=Join-Path $root 'contour-apo.dll'
    $hash=(Get-FileHash -LiteralPath $dll -Algorithm SHA256).Hash
    $info=Read-PackageInfo
    if ((Get-FileHash -LiteralPath $dll -Algorithm SHA256).Hash -cne $hash) { throw 'The APO changed during inspection. Check again after the build finishes.' }
    [void](New-AudioPolicyJournal $Store)
    $plan=New-ContourPlan $Store $info $dll $hash ([guid]::NewGuid().ToString('D'))
    [void](New-ControlContextPlan $Store $EndpointId)
    return $plan
}
function New-SetupPackage($Plan) {
    $path=$Plan.Package
    if (Test-Path -LiteralPath $path) { throw 'The setup package already exists.' }
    $acl=[Security.AccessControl.DirectorySecurity]::new()
    $acl.SetAccessRuleProtection($true,$false)
    foreach ($spec in @(@('S-1-5-18','FullControl'),@('S-1-5-32-544','FullControl'),@('S-1-5-32-545','ReadAndExecute'))) {
        $rule=[Security.AccessControl.FileSystemAccessRule]::new([Security.Principal.SecurityIdentifier]::new($spec[0]),$spec[1],'ContainerInherit,ObjectInherit','None','Allow')
        $acl.AddAccessRule($rule)
    }
    $acl.SetOwner([Security.Principal.SecurityIdentifier]::new('S-1-5-32-544'))
    [void][IO.Directory]::CreateDirectory($path,$acl)
    $source=Split-Path $PSScriptRoot -Parent
    foreach ($name in @('contour-apo.dll','contour-deploy-info.exe','contour-apo-host.exe','contour-endpoint-check.exe')) {
        Copy-Item -LiteralPath (Join-Path $source $name) -Destination (Join-Path $path $name)
    }
    # Retain self-contained recovery alongside the DLL. Installed files are
    # immutable; a rebuild in the workspace never overwrites a loaded APO.
    $scripts=Join-Path $path 'setup'; [void][IO.Directory]::CreateDirectory($scripts)
    foreach ($name in @('contour-setup.ps1','DeviceSetup.ps1','AudioPolicy.ps1','SharedSetup.ps1','EndpointDeployment.ps1','LegacyMigration.ps1','ControlContext.ps1','Deployment.ps1','EffectPlacement.ps1','RegistryStore.cs')) {
        Copy-Item -LiteralPath (Join-Path $PSScriptRoot $name) -Destination (Join-Path $scripts $name)
    }
    if ((Get-FileHash -LiteralPath $Plan.TargetDll -Algorithm SHA256).Hash -cne $Plan.DllSha256) { throw 'The APO changed while it was staged. No device settings were changed.' }
    & (Join-Path $path 'contour-apo-host.exe') $Plan.TargetDll | Out-Null
    if ($LASTEXITCODE -ne 0) { throw 'The staged APO failed its contract tests. No device settings were changed.' }
    # Neutral fallback also covers a failed settings-store open: never inherit
    # the original lab's -6 dB fallback on a newly configured output.
    $neutral="CONTOUR 1 $($Plan.Endpoint.SampleRate) $($Plan.Endpoint.Channels)`nBypass 0`nEnd`n"
    [Contour.Deployment.DurableFile]::Save((Join-Path $path 'profile.contour'),$neutral,$true)
    Save-ContourJournal (New-ContourJournal $Plan) (Join-Path $path 'journal.json') -CreateNew
    $state=New-SetupState $Plan
    Save-SetupState $state $path
    return $state
}
function Capture-SetupServices {
    $audio=Get-Service Audiosrv
    if ($audio.Status -ne 'Running') { throw 'Start Windows Audio before setting up this output.' }
    $seen=[Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    function Visit($Service) {
        foreach ($child in $Service.DependentServices) {
            if ($child.Status -eq 'Running' -and $seen.Add($child.Name)) { Visit $child; $child.Name }
        }
    }
    Visit $audio
    'Audiosrv'
}
function Stop-SetupAudio($Names) {
    foreach ($name in $Names) {
        $service=Get-Service -Name $name
        if ($service.Status -ne 'Stopped') { $service.Stop(); $service.WaitForStatus('Stopped',[TimeSpan]::FromSeconds(15)) }
    }
}
function Start-SetupAudio($Names) {
    $errors=@(); $list=@($Names); [array]::Reverse($list)
    foreach ($name in $list) {
        try {
            $service=Get-Service -Name $name
            if ($service.Status -ne 'Running') { $service.Start(); $service.WaitForStatus('Running',[TimeSpan]::FromSeconds(15)) }
        } catch { $errors+=@($_.Exception.Message) }
    }
    if ($errors.Count) { throw ($errors -join '; ') }
}
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
        $report | ConvertTo-Json -Compress
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
