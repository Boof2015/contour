# Transaction core for shared registrations and independently owned endpoints.
# Not exposed by contour-setup yet. The elevated host must validate protected
# state/package paths and supply fresh registration/engine checks. Tests supply
# an isolated registry root and fake service callbacks; no callback defaults to
# successful verification. Package removal is deliberately a separate operation.
Set-StrictMode -Version Latest
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot 'ControlContext.ps1')
$script:ComponentClassIds=@('{C96CE6C1-7F62-46A5-BDFF-46FCA19567AB}','{C2DB4B2B-8B4D-43C2-BCC3-A2B0D3E8FBA1}')

function Assert-DeploymentId([string]$Id) {
    $guid=[guid]::Empty
    if (-not [guid]::TryParseExact($Id,'D',[ref]$guid) -or $guid -eq [guid]::Empty) { throw 'Invalid deployment identity.' }
}
function Assert-AdapterId($Id) {
    if ($Id -isnot [string] -or $Id -notmatch '^[^\x00-\x1f]{1,512}\z') { throw 'Invalid adapter identity.' }
}
function Assert-Registration($Registration) {
    Assert-DeploymentId $Registration.Id
    if ($Registration.Mode -notin @('Legacy','Component')) { throw 'Unknown registration mode.' }
    if ($Registration.Mode -eq 'Component') { Assert-AdapterId $Registration.AdapterId }
    elseif ($Registration.AdapterId -cne '') { throw 'Global registration must not claim a single adapter.' }
    $expected=if ($Registration.Mode -eq 'Legacy') { @('Legacy') } else { @('Apo','Extension') }
    if ((@($Registration.Packages.Kind) -join ',') -cne ($expected -join ',')) { throw 'Invalid registration package set.' }
    foreach ($package in $Registration.Packages) {
        Assert-DeploymentId $package.Id
        if ($package.Sha256 -cnotmatch '^[0-9A-F]{64}\z') { throw 'Invalid package fingerprint.' }
        if ($package.Kind -eq 'Legacy') { Assert-DeploymentId $package.Identity }
        elseif ($package.Identity -cnotmatch '^oem[0-9]+\.inf\z') { throw 'Expected a published package INF name.' }
    }
}
function Get-Registration($State,[string]$Id) {
    $found=@($State.Registrations | Where-Object { $_.Id -ieq $Id })
    if ($found.Count -ne 1) { throw 'Registration is missing or ambiguous.' }
    return $found[0]
}
function Assert-Binding($State,$Binding) {
    [void](Get-EndpointPath $Binding.EndpointId)
    Assert-AdapterId $Binding.AdapterId
    $registration=Get-Registration $State $Binding.RegistrationId
    if ($registration.Mode -eq 'Component' -and $registration.AdapterId -ine $Binding.AdapterId) {
        throw 'Device-scoped registration belongs to another adapter.'
    }
}
function Get-Binding($State,[string]$EndpointId) {
    $found=@($State.Bindings | Where-Object { $_.EndpointId -ieq $EndpointId })
    if ($found.Count -gt 1) { throw 'Endpoint has multiple owners.' }
    if ($found.Count) { return $found[0] }
    return $null
}
function Assert-DeploymentState($State) {
    if ($State.Schema -ne 1 -or $State.Kind -cne 'ContourEndpointDeployment' -or
        ($State.Revision -isnot [long] -and $State.Revision -isnot [int]) -or $State.Revision -lt 0) { throw 'Invalid deployment state.' }
    $ids=[Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    $packages=@{};$identities=@{}
    foreach ($registration in $State.Registrations) {
        Assert-Registration $registration
        if (-not $ids.Add($registration.Id)) { throw 'Duplicate registration identity.' }
        foreach ($package in $registration.Packages) {
            $identity=$package.Kind+'|'+$package.Identity+'|'+$package.Sha256
            if ($packages.ContainsKey($package.Id) -and $packages[$package.Id] -cne $identity) { throw 'Shared package identity changed.' }
            $packages[$package.Id]=$identity
            $physical=if ($package.Kind -eq 'Legacy') { 'Legacy|'+$package.Identity } else { 'Published|'+$package.Identity }
            if ($identities.ContainsKey($physical) -and $identities[$physical] -ine $package.Id) { throw 'A package cannot have two ownership identities.' }
            $identities[$physical]=$package.Id
        }
    }
    $ids.Clear()
    foreach ($binding in $State.Bindings) {
        Assert-Binding $State $binding
        if (-not $ids.Add($binding.EndpointId)) { throw 'Duplicate endpoint ownership.' }
    }
    $op=$State.Operation
    if ($null -eq $op) { return }
    Assert-DeploymentId $op.Id
    [void](Get-EndpointPath $op.EndpointId)
    if ($op.Action -notin @('Attach','Migrate','Detach') -or $op.Phase -notin @('Prepared','Writing','Verifying','Restoring','RecoveryRequired')) {
        throw 'Invalid endpoint transaction.'
    }
    foreach ($binding in @($op.Before,$op.After)) {
        if ($null -ne $binding) {
            Assert-Binding $State $binding
            if ($binding.EndpointId -ine $op.EndpointId) { throw 'Transaction targets another endpoint.' }
        }
    }
    if (($op.Action -eq 'Attach' -and ($null -ne $op.Before -or $null -eq $op.After)) -or
        ($op.Action -eq 'Detach' -and ($null -eq $op.Before -or $null -ne $op.After)) -or
        ($op.Action -eq 'Migrate' -and ($null -eq $op.Before -or $null -eq $op.After -or
            $op.Before.AdapterId -ine $op.After.AdapterId -or $op.Before.RegistrationId -ieq $op.After.RegistrationId))) {
        throw 'Inconsistent endpoint transition.'
    }
    $current=Get-Binding $State $op.EndpointId
    if (($null -eq $current) -ne ($null -eq $op.Before) -or
        ($null -ne $current -and ($current.RegistrationId -ine $op.Before.RegistrationId -or $current.AdapterId -ine $op.Before.AdapterId))) {
        throw 'Transaction no longer matches endpoint ownership.'
    }
    if ((@($op.Intents) -join ',') -notin @('','0','0,1') -or $op.AudioRestartPending -isnot [bool]) { throw 'Invalid recovery intent.' }
    foreach ($intent in $op.Intents) {
        if ($intent -isnot [int] -and $intent -isnot [long]) { throw 'Recovery indices must be integers.' }
    }
    if (@($op.Services).Count -eq 0) { throw 'Missing original service state.' }
    foreach ($service in $op.Services) { if ($service -notmatch '^[A-Za-z0-9_.-]{1,256}\z') { throw 'Invalid service state.' } }
    foreach ($name in @('Default','User','Security')) { if ($op.Settings.$name -isnot [string]) { throw 'Invalid settings guard.' } }
}
function Read-DeploymentState([string]$Path) {
    $state=Get-Content -LiteralPath $Path -Raw -Encoding UTF8 | ConvertFrom-Json
    Assert-DeploymentState $state
    return $state
}
function Save-DeploymentState($State,[string]$Path,[switch]$CreateNew) {
    Assert-DeploymentState $State
    ++$State.Revision
    Save-ContourJournal $State $Path -CreateNew:$CreateNew
}
function Lock-DeploymentState([string]$Path) {
    # One writer per catalog, including across processes and recovery. The
    # containing directory must be protected by the eventual elevated host.
    [IO.File]::Open($Path+'.lock',[IO.FileMode]::OpenOrCreate,[IO.FileAccess]::ReadWrite,[IO.FileShare]::None)
}
function New-EndpointDeployment([string]$Path) {
    $lease=Lock-DeploymentState $Path
    try {
        $state=[pscustomobject]@{Schema=1;Kind='ContourEndpointDeployment';Revision=0;Registrations=@();Bindings=@();Operation=$null;LastResult=$null}
        Save-DeploymentState $state $Path -CreateNew
    } finally { $lease.Dispose() }
}
function Add-RegistrationRecord($State,$Registration) {
    Assert-Registration $Registration
    $existing=@($State.Registrations | Where-Object { $_.Id -ieq $Registration.Id })
    if ($existing.Count) {
        if (($existing[0] | ConvertTo-Json -Depth 10 -Compress) -cne ($Registration | ConvertTo-Json -Depth 10 -Compress)) {
            throw 'Registration identity already has different contents.'
        }
    } else { $State.Registrations+=@($Registration) }
}
function Add-EndpointRegistration([string]$Path,$Registration) {
    $lease=Lock-DeploymentState $Path
    try {
        $state=Read-DeploymentState $Path
        if ($null -ne $state.Operation) { throw 'Finish endpoint recovery before changing registrations.' }
        Add-RegistrationRecord $state $Registration
        Save-DeploymentState $state $Path
    } finally { $lease.Dispose() }
}
function Get-EndpointSettings($Store,[string]$EndpointId) {
    $keys=@(Get-ControlContextKeys $EndpointId)
    foreach ($key in $keys) { if (-not $Store.Exists($key)) { throw 'Provision the endpoint control context before attachment.' } }
    if (-not (Test-RawEqual ($Store.Read($keys[0],$script:EffectPrefix+'0')) ([pscustomobject]@{Type=1;Data=(ConvertTo-HexString '{00000000-0000-0000-0000-000000000000}')})) -or
        -not (Test-RawEqual ($Store.Read($keys[1],$script:ControlContextId+',3')) ([pscustomobject]@{Type=4;Data='01000000'}))) { throw 'Unsupported endpoint context.' }
    [pscustomobject]@{Default=$Store.Snapshot($keys[1]);User=$Store.Snapshot($keys[2]);Security=$Store.Security((Get-EndpointPath $EndpointId)+'\FxProperties')}
}
function Get-BindingValue($State,$Binding,[int]$Index) {
    if ($null -eq $Binding) { return $null }
    $registration=Get-Registration $State $Binding.RegistrationId
    $classes=if ($registration.Mode -eq 'Legacy') { $script:ClassIds } else { $script:ComponentClassIds }
    [pscustomobject]@{Type=1;Data=(ConvertTo-HexString $classes[$Index])}
}
function Assert-EndpointAssignments($Store,$State,[string]$EndpointId,$Binding) {
    $fx=(Get-EndpointPath $EndpointId)+'\FxProperties'
    for ($i=0;$i -lt 2;++$i) {
        if (-not (Test-RawEqual ($Store.Read($fx,$script:EffectPrefix+($i+1))) (Get-BindingValue $State $Binding $i))) {
            throw 'Endpoint effect ownership changed.'
        }
    }
    foreach ($entry in $Store.Entries($fx)) {
        if ($entry.Name -match '^\{d04e05a6-594b-4fb6-a80d-01af5eed7d1d\},(1|2|[5-9]|1[0-9]|20)$' -and
            ($entry.Path -ine $fx -or $entry.Name -notin @(($script:EffectPrefix+'1'),($script:EffectPrefix+'2')))) {
            throw 'Other effect assignments require a coexistence strategy.'
        }
    }
    foreach ($path in @($fx,((Get-EndpointPath $EndpointId)+'\Properties'))) {
        if (-not $Store.Exists($path)) { throw 'Endpoint property store is absent.' }
        $disabled=$Store.Read($path,$script:DisableEffects)
        if ($null -ne $disabled -and -not (Test-RawEqual $disabled ([pscustomobject]@{Type=4;Data='00000000'}))) { throw 'Audio enhancements are disabled or unknown.' }
    }
}
function Import-LegacyEndpoint($Store,[string]$Path,$Attachment,$Context,[string]$AdapterId) {
    $lease=Lock-DeploymentState $Path
    try {
        $state=Read-DeploymentState $Path
        if ($null -ne $state.Operation) { throw 'Finish recovery before importing an installation.' }
        Assert-ContourPlan $Attachment.Plan
        Assert-ControlContextPlan $Context.Plan
        if ($Attachment.Schema -ne 1 -or $Attachment.State -ne 'AttachedPendingGraphRebuild' -or
            $Context.Schema -ne 1 -or $Context.State -ne 'Provisioned' -or
            $Context.Plan.EndpointId -ine $Attachment.Plan.Endpoint.Id) { throw 'Legacy journals do not describe a provisioned endpoint.' }
        foreach ($change in $Attachment.Plan.Changes) {
            if (-not (Test-RawEqual ($Store.Read($change.Path,$change.Name)) $change.After)) { throw 'Legacy installation differs from its journal.' }
        }
        $registration=[pscustomobject]@{Id=$Attachment.Plan.Id;Mode='Legacy';AdapterId='';Packages=@(
            [pscustomobject]@{Id=$Attachment.Plan.Id;Kind='Legacy';Identity=$Attachment.Plan.Id;Sha256=$Attachment.Plan.DllSha256})}
        Add-RegistrationRecord $state $registration
        $binding=[pscustomobject]@{EndpointId=$Attachment.Plan.Endpoint.Id;AdapterId=$AdapterId;RegistrationId=$registration.Id}
        Assert-Binding $state $binding
        Assert-EndpointAssignments $Store $state $binding.EndpointId $binding
        [void](Get-EndpointSettings $Store $binding.EndpointId)
        $current=Get-Binding $state $binding.EndpointId
        if ($null -ne $current) {
            if ($current.AdapterId -ine $AdapterId -or $current.RegistrationId -ine $registration.Id) { throw 'Endpoint already has a different owner.' }
            return
        }
        $state.Bindings+=@($binding)
        Save-DeploymentState $state $Path
        # Original protected journals remain intact. The new host must retire
        # their old removal path only after a verified migration has committed.
    } finally { $lease.Dispose() }
}
function Confirm-EndpointCheck([scriptblock]$Check,$State,$Binding,[string]$EndpointId) {
    $registration=if ($null -ne $Binding) { Get-Registration $State $Binding.RegistrationId } else { $null }
    $result=& $Check $EndpointId $registration $Binding
    if ($result -isnot [bool] -or -not $result) { throw 'Endpoint or registration verification did not succeed.' }
}
function Assert-SettingsUnchanged($Store,$Operation) {
    $current=Get-EndpointSettings $Store $Operation.EndpointId
    foreach ($name in @('Default','User','Security')) {
        if ($current.$name -cne $Operation.Settings.$name) { throw 'Endpoint settings changed during the transaction; preserving the newer data.' }
    }
}
function Restore-EndpointTransition($Store,$State,[string]$Path,[scriptblock]$StopAudio,[scriptblock]$StartAudio,
    [scriptblock]$VerifyRegistration,[scriptblock]$VerifyApplied) {
    $op=$State.Operation
    if ($null -eq $op) { return $true }
    $op.Phase='Restoring';$op.AudioRestartPending=$true;$op.Conflicts=@()
    try {
        Save-DeploymentState $State $Path
        $null=& $StopAudio $op.Services
        if ($null -ne $op.Before) { Confirm-EndpointCheck $VerifyRegistration $State $op.Before $op.EndpointId }
        $fx=(Get-EndpointPath $op.EndpointId)+'\FxProperties'
        foreach ($i in @($op.Intents | Sort-Object -Descending)) {
            $name=$script:EffectPrefix+($i+1)
            $before=Get-BindingValue $State $op.Before $i
            $after=Get-BindingValue $State $op.After $i
            $current=$Store.Read($fx,$name)
            if (Test-RawEqual $current $before) { continue }
            if (-not (Test-RawEqual $current $after)) { $op.Conflicts+=@('Foreign effect assignment retained: '+$name);continue }
            # A second comparison immediately before the write prevents a stale
            # read from authorizing recovery. This is not a registry-wide CAS.
            if (-not (Test-RawEqual ($Store.Read($fx,$name)) $after)) { throw 'Effect assignment changed during recovery.' }
            if ($null -eq $before) { $Store.DeleteValue($fx,$name) }
            else { $Store.Write($fx,$name,$before.Type,$before.Data) }
            if (-not (Test-RawEqual ($Store.Read($fx,$name)) $before)) { throw 'Recovery write did not persist.' }
        }
        # Check even slots with no write intent: a foreign change may have
        # caused failure before our first write. Keep both registrations pinned.
        Assert-EndpointAssignments $Store $State $op.EndpointId $op.Before
    } catch { $op.Conflicts+=@($_.Exception.Message) }
    try {
        $null=& $StartAudio $op.Services; $op.AudioRestartPending=$false
        if ($op.Conflicts.Count -eq 0) { Confirm-EndpointCheck $VerifyApplied $State $op.Before $op.EndpointId }
    }
    catch { $op.Conflicts+=@($_.Exception.Message) }
    if ($op.Conflicts.Count) {
        $op.Phase='RecoveryRequired';Save-DeploymentState $State $Path;return $false
    }
    $State.LastResult=[pscustomobject]@{Id=$op.Id;EndpointId=$op.EndpointId;Action=$op.Action;Outcome='RolledBack';Error=$op.Error}
    $State.Operation=$null
    Save-DeploymentState $State $Path
    return $true
}
function Undo-EndpointTransition($Store,[string]$Path,
    [Parameter(Mandatory=$true)][scriptblock]$StopAudio,
    [Parameter(Mandatory=$true)][scriptblock]$StartAudio,
    [Parameter(Mandatory=$true)][scriptblock]$VerifyRegistration,
    [Parameter(Mandatory=$true)][scriptblock]$VerifyApplied) {
    $lease=Lock-DeploymentState $Path
    try { Restore-EndpointTransition $Store (Read-DeploymentState $Path) $Path $StopAudio $StartAudio $VerifyRegistration $VerifyApplied }
    finally { $lease.Dispose() }
}
function Invoke-EndpointTransition($Store,[string]$Path,[ValidateSet('Attach','Migrate','Detach')][string]$Action,
    [string]$EndpointId,[string]$AdapterId,[string]$RegistrationId,
    [Parameter(Mandatory=$true)][scriptblock]$VerifyRegistration,
    [Parameter(Mandatory=$true)][scriptblock]$VerifyApplied,
    [Parameter(Mandatory=$true)][scriptblock]$CaptureServices,
    [Parameter(Mandatory=$true)][scriptblock]$StopAudio,
    [Parameter(Mandatory=$true)][scriptblock]$StartAudio,
    [scriptblock]$Checkpoint={param($Point)}) {
    $lease=Lock-DeploymentState $Path
    try {
        $state=Read-DeploymentState $Path
        if ($null -ne $state.Operation) { throw 'Finish the pending endpoint recovery first.' }
        $before=Get-Binding $state $EndpointId
        $after=if ($Action -eq 'Detach') { $null } else { [pscustomobject]@{EndpointId=$EndpointId;AdapterId=$AdapterId;RegistrationId=$RegistrationId} }
        if (($Action -eq 'Attach' -and $null -ne $before) -or ($Action -ne 'Attach' -and $null -eq $before)) { throw 'Endpoint ownership does not permit this action.' }
        if ($null -ne $after) { Assert-Binding $state $after }
        if ($null -ne $before -and $before.AdapterId -ine $AdapterId) { throw 'Endpoint adapter changed; inspect it again.' }
        Assert-EndpointAssignments $Store $state $EndpointId $before
        $settings=Get-EndpointSettings $Store $EndpointId
        foreach ($binding in @($before,$after)) {
            if ($null -ne $binding) { Confirm-EndpointCheck $VerifyRegistration $state $binding $EndpointId }
        }
        $services=@(& $CaptureServices)
        $operation=[pscustomobject]@{Id=([guid]::NewGuid().ToString('D'));EndpointId=$EndpointId;Action=$Action;
            Before=$before;After=$after;Phase='Prepared';Intents=@();Settings=$settings;
            Services=$services;AudioRestartPending=$true;Error='';Conflicts=@()}
        $state.Operation=$operation
        Save-DeploymentState $state $Path
        $audioNeedsRestart=$false
        try {
            $null=& $Checkpoint 'prepared'
            $audioNeedsRestart=$true
            $null=& $StopAudio $services
            $null=& $Checkpoint 'audio-stopped'
            Assert-EndpointAssignments $Store $state $EndpointId $before
            Assert-SettingsUnchanged $Store $operation
            foreach ($binding in @($before,$after)) {
                if ($null -ne $binding) { Confirm-EndpointCheck $VerifyRegistration $state $binding $EndpointId }
            }
            $operation.Phase='Writing';Save-DeploymentState $state $Path
            $fx=(Get-EndpointPath $EndpointId)+'\FxProperties'
            for ($i=0;$i -lt 2;++$i) {
                $name=$script:EffectPrefix+($i+1)
                $old=Get-BindingValue $state $before $i;$new=Get-BindingValue $state $after $i
                $operation.Intents+=@($i);Save-DeploymentState $state $Path
                $null=& $Checkpoint ('intent-'+$i)
                if (-not (Test-RawEqual ($Store.Read($fx,$name)) $old)) { throw 'Endpoint changed before write.' }
                if ($null -eq $new) { $Store.DeleteValue($fx,$name) } else { $Store.Write($fx,$name,$new.Type,$new.Data) }
                if (-not (Test-RawEqual ($Store.Read($fx,$name)) $new)) { throw 'Endpoint write did not persist.' }
                $null=& $Checkpoint ('written-'+$i)
            }
            $operation.Phase='Verifying';Save-DeploymentState $state $Path
            $null=& $Checkpoint 'verification-pending'
            $null=& $StartAudio $services
            $audioNeedsRestart=$false
            $operation.AudioRestartPending=$false;Save-DeploymentState $state $Path
            $null=& $Checkpoint 'audio-restarted'
            Confirm-EndpointCheck $VerifyApplied $state $after $EndpointId
            $null=& $Checkpoint 'verified'
            Assert-EndpointAssignments $Store $state $EndpointId $after
            Assert-SettingsUnchanged $Store $operation
            $state.Bindings=@($state.Bindings | Where-Object { $_.EndpointId -ine $EndpointId })
            if ($null -ne $after) { $state.Bindings+=@($after) }
            $state.LastResult=[pscustomobject]@{Id=$operation.Id;EndpointId=$EndpointId;Action=$Action;Outcome='Committed';Error=''}
            $state.Operation=$null;Save-DeploymentState $state $Path
            $null=& $Checkpoint 'committed'
        } catch {
            $failure=$_
            # Reload the durable record, including after ambiguous file-write
            # failures. A committed transaction must never be undone as if it
            # were still pending, and no in-memory mutation grants ownership.
            $current=Read-DeploymentState $Path
            if ($null -ne $current.Operation) {
                $current.Operation.Error=$failure.Exception.Message
                Save-DeploymentState $current $Path
                $audioNeedsRestart=$true
                if (Restore-EndpointTransition $Store $current $Path $StopAudio $StartAudio $VerifyRegistration $VerifyApplied) {
                    $audioNeedsRestart=$false
                }
            }
            throw $failure
        } finally {
            # Journal I/O failure must not skip service recovery. Keep the last
            # durable intent for the next run; never guess ownership to repair
            # assignments when the journal cannot be read or saved.
            if ($audioNeedsRestart) {
                try { $null=& $StartAudio $services }
                catch { Write-Warning ('Audio restart retry failed: '+$_.Exception.Message) }
            }
        }
    } finally { $lease.Dispose() }
}
function Get-DeploymentPackageUsage($State) {
    Assert-DeploymentState $State
    $packages=@{}
    foreach ($registration in $State.Registrations) {
        foreach ($package in $registration.Packages) {
            if (-not $packages.ContainsKey($package.Id)) {
                $packages[$package.Id]=[pscustomobject]@{Id=$package.Id;Kind=$package.Kind;Identity=$package.Identity;
                    Owners=@();Retain=$false;RemovalRequiresSystemCheck=$true}
            }
        }
    }
    $pins=@($State.Bindings | ForEach-Object { [pscustomobject]@{Binding=$_;Reason='Bound'} })
    if ($null -ne $State.Operation) {
        foreach ($side in @('Before','After')) {
            if ($null -ne $State.Operation.$side) { $pins+=@([pscustomobject]@{Binding=$State.Operation.$side;Reason=('Pending'+$side)}) }
        }
    }
    foreach ($pin in $pins) {
        foreach ($package in (Get-Registration $State $pin.Binding.RegistrationId).Packages) {
            $packages[$package.Id].Retain=$true
            $packages[$package.Id].Owners+=@([pscustomobject]@{EndpointId=$pin.Binding.EndpointId;RegistrationId=$pin.Binding.RegistrationId;Reason=$pin.Reason})
        }
    }
    # Zero managed references is only a cleanup candidate. Live PnP users,
    # untracked endpoint references, signatures and published INF identity must
    # also be checked before a later host can delete any package.
    @($packages.Values | Sort-Object Id)
}
