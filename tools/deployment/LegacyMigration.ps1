# Durable handoff from a single-device setup to shared endpoint ownership.
# The elevated caller must validate protected paths and hold the existing global
# deployment mutex. This coordinator also holds the catalog lease throughout.
# Package installation and trusted registration/engine checks remain host duties.
Set-StrictMode -Version Latest
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot 'DeviceSetup.ps1')
. (Join-Path $PSScriptRoot 'EndpointDeployment.ps1')

function Assert-HandoffJournals($Handoff,[string]$Package) {
    foreach ($pair in @(@('journal.json','AttachmentSha256'),@('context-journal.json','ContextSha256'))) {
        if ((Get-FileHash -LiteralPath (Join-Path $Package $pair[0]) -Algorithm SHA256).Hash -cne $Handoff.($pair[1])) {
            throw 'An original installation journal changed during migration.'
        }
    }
}
function New-HandoffSetupRecord($Handoff,[string]$Status) {
    [pscustomobject]@{Schema=2;Kind='ContourDeviceSetupHandoff';Id=$Handoff.OriginalSetup.Id;
        EndpointId=$Handoff.Before.EndpointId;Name=$Handoff.OriginalSetup.Name;State=$Status;
        AudioRestartPending=$false;HandoffId=$Handoff.Id;TargetRegistrationId=$Handoff.After.RegistrationId;
        AdapterId=$Handoff.Before.AdapterId}
}
function Assert-HandoffEnvelope($Record,$Handoff) {
    Assert-HandoffSetupRecord $Record
    if ($Record.Id -ine $Handoff.OriginalSetup.Id -or $Record.EndpointId -ine $Handoff.Before.EndpointId -or
        $Record.HandoffId -ine $Handoff.Id -or $Record.TargetRegistrationId -ine $Handoff.After.RegistrationId -or
        $Record.AdapterId -ine $Handoff.Before.AdapterId) { throw 'The old installation belongs to another migration.' }
}
function Clear-ReturnedHandoff($State,[string]$Path) {
    $handoff=Get-DeploymentHandoff $State
    $binding=Get-Binding $State $handoff.Before.EndpointId
    if ($null -ne $State.Operation -or $binding.RegistrationId -ine $handoff.Before.RegistrationId) {
        throw 'Cannot return ownership before endpoint recovery finishes.'
    }
    $State.Bindings=@($State.Bindings | Where-Object { $_.EndpointId -ine $handoff.Before.EndpointId })
    # This legacy registration was introduced solely for the handoff. Do not
    # leave an unreferenced cleanup candidate for a package now owned by the
    # original installer again.
    if (@($State.Bindings | Where-Object { $_.RegistrationId -ieq $handoff.Before.RegistrationId }).Count) {
        throw 'Another binding unexpectedly depends on the legacy registration.'
    }
    $State.Registrations=@($State.Registrations | Where-Object { $_.Id -ine $handoff.Before.RegistrationId })
    $State.Handoff=$null
    Save-DeploymentState $State $Path
}
# Internal: the catalog lease and elevated host's global mutex must be held.
function Complete-LegacyHandoff($Store,[string]$Path,[string]$Package,
    [scriptblock]$StopAudio,[scriptblock]$StartAudio,[scriptblock]$VerifyRegistration,[scriptblock]$VerifyApplied,
    [scriptblock]$Checkpoint={param($Point)}) {
    $state=Read-DeploymentState $Path
    $handoff=Get-DeploymentHandoff $state
    if ($null -eq $handoff) { return }
    Assert-LegacySetupRecord $handoff.OriginalSetup
    $recordPath=Join-Path $Package 'device-setup.json'
    $record=Read-SetupJson $recordPath
    if ($record.Schema -eq 1) {
        # A crash may occur before freezing the legacy installer, or after
        # returning it authority. It may since have completed an uninstall.
        # Retire our provisional ownership without changing its newer state.
        Assert-LegacySetupRecord $record
        if ($record.Id -ine $handoff.OriginalSetup.Id -or $record.EndpointId -ine $handoff.Before.EndpointId -or
            $handoff.Phase -notin @('Prepared','Returning')) { throw 'Unexpected legacy ownership during migration.' }
        Clear-ReturnedHandoff $state $Path
        return
    }
    Assert-HandoffEnvelope $record $handoff
    Assert-HandoffJournals $handoff $Package
    if ($null -ne $state.Operation) {
        if (-not (Restore-EndpointTransition $Store $state $Path $StopAudio $StartAudio $VerifyRegistration $VerifyApplied)) {
            throw 'Finish endpoint recovery before completing this installation update.'
        }
        $state=Read-DeploymentState $Path;$handoff=Get-DeploymentHandoff $state
    }
    $binding=Get-Binding $state $handoff.Before.EndpointId
    if ($binding.RegistrationId -ieq $handoff.After.RegistrationId) {
        if ($null -eq $state.LastResult -or $state.LastResult.Id -ine $handoff.Id -or $state.LastResult.Outcome -ne 'Committed') {
            throw 'No verified migration commit exists for this installation.'
        }
        # A durable commit transfers authority. Finishing that bookkeeping must
        # not undo a successful migration, even if the device is now unplugged.
        Save-ContourJournal (New-HandoffSetupRecord $handoff 'Migrated') $recordPath
        $null=& $Checkpoint 'legacy-retired'
        $state.Handoff=$null;Save-DeploymentState $state $Path
        $null=& $Checkpoint 'handoff-complete'
        return
    }
    if ($record.State -ne 'Migrating') { throw 'A retired installation cannot regain ownership implicitly.' }
    Assert-EndpointAssignments $Store $state $handoff.Before.EndpointId $handoff.Before
    [void](Get-EndpointSettings $Store $handoff.Before.EndpointId)
    Confirm-EndpointCheck $VerifyRegistration $state $handoff.Before $handoff.Before.EndpointId
    Confirm-EndpointCheck $VerifyApplied $state $handoff.Before $handoff.Before.EndpointId
    $handoff.Phase='Returning';Save-DeploymentState $state $Path
    $null=& $Checkpoint 'returning-recorded'
    Assert-HandoffEnvelope (Read-SetupJson $recordPath) $handoff
    Save-ContourJournal $handoff.OriginalSetup $recordPath
    $null=& $Checkpoint 'legacy-returned'
    Clear-ReturnedHandoff $state $Path
    $null=& $Checkpoint 'handoff-returned'
}
function Resume-LegacyMigration($Store,[string]$Path,[string]$Package,
    [Parameter(Mandatory=$true)][scriptblock]$StopAudio,
    [Parameter(Mandatory=$true)][scriptblock]$StartAudio,
    [Parameter(Mandatory=$true)][scriptblock]$VerifyRegistration,
    [Parameter(Mandatory=$true)][scriptblock]$VerifyApplied,
    [scriptblock]$Checkpoint={param($Point)}) {
    $lease=Lock-DeploymentState $Path
    try {
        $pending=Read-DeploymentState $Path
        try { Complete-LegacyHandoff @PSBoundParameters }
        catch {
            # Even a mismatched old record must not strand services stopped by
            # a prior process. Restart only the validated original service set;
            # keep registry recovery pending until the conflict is resolved.
            if ($null -ne $pending.Operation -and $pending.Operation.AudioRestartPending) {
                try { $null=& $StartAudio $pending.Operation.Services }
                catch { Write-Warning ('Audio restart retry failed: '+$_.Exception.Message) }
            }
            throw
        }
    }
    finally { $lease.Dispose() }
}
function Invoke-LegacyMigration($Store,[string]$Path,[string]$Package,[string]$AdapterId,[string]$RegistrationId,
    [Parameter(Mandatory=$true)][scriptblock]$VerifyRegistration,
    [Parameter(Mandatory=$true)][scriptblock]$VerifyApplied,
    [Parameter(Mandatory=$true)][scriptblock]$CaptureServices,
    [Parameter(Mandatory=$true)][scriptblock]$StopAudio,
    [Parameter(Mandatory=$true)][scriptblock]$StartAudio,
    [scriptblock]$Checkpoint={param($Point)}) {
    $lease=Lock-DeploymentState $Path
    try {
        $state=Read-DeploymentState $Path
        Assert-NoDeploymentHandoff $state
        if ($null -ne $state.Operation) { throw 'Finish endpoint recovery before migrating an installation.' }
        $recordPath=Join-Path $Package 'device-setup.json'
        $original=Read-SetupJson $recordPath
        Assert-LegacySetupRecord $original
        if ($original.State -ne 'Attached' -or $original.AudioRestartPending) { throw 'Recover the existing installation before migrating it.' }
        if ($null -ne (Get-Binding $state $original.EndpointId) -or
            @($state.Registrations | Where-Object { $_.Id -ieq $original.Id }).Count) {
            throw 'The existing installation already has a catalog owner.'
        }
        $attachmentPath=Join-Path $Package 'journal.json';$contextPath=Join-Path $Package 'context-journal.json'
        $attachmentHash=(Get-FileHash -LiteralPath $attachmentPath -Algorithm SHA256).Hash
        $contextHash=(Get-FileHash -LiteralPath $contextPath -Algorithm SHA256).Hash
        $attachment=Read-SetupJson $attachmentPath;$context=Read-SetupJson $contextPath
        Assert-SetupJournals $original $attachment $context
        $legacy=Get-LegacyEndpointRecord $Store $attachment $context $AdapterId
        Add-RegistrationRecord $state $legacy.Registration
        $after=[pscustomobject]@{EndpointId=$original.EndpointId;AdapterId=$AdapterId;RegistrationId=$RegistrationId}
        Assert-Binding $state $after
        if ((Get-Registration $state $RegistrationId).Mode -ne 'Component') { throw 'Migration requires a device-scoped registration.' }
        foreach ($binding in @($legacy.Binding,$after)) {
            Confirm-EndpointCheck $VerifyRegistration $state $binding $original.EndpointId
        }
        $handoff=[pscustomobject]@{Id=([guid]::NewGuid().ToString('D'));Phase='Prepared';Before=$legacy.Binding;After=$after;
            OriginalSetup=$original;AttachmentSha256=$attachmentHash;ContextSha256=$contextHash}
        Assert-HandoffJournals $handoff $Package
        $state.Bindings+=@($legacy.Binding)
        $state.Schema=2
        $state | Add-Member -NotePropertyName Handoff -NotePropertyValue $handoff -Force
        Save-DeploymentState $state $Path
        try {
            $null=& $Checkpoint 'handoff-prepared'
            $current=Read-SetupJson $recordPath
            if (($current | ConvertTo-Json -Depth 10 -Compress) -cne ($original | ConvertTo-Json -Depth 10 -Compress)) {
                throw 'The original setup record changed before migration.'
            }
            Save-ContourJournal (New-HandoffSetupRecord $handoff 'Migrating') $recordPath
            $null=& $Checkpoint 'legacy-frozen'
            $handoff.Phase='Migrating';Save-DeploymentState $state $Path
            $null=& $Checkpoint 'migration-ready'
            Assert-HandoffEnvelope (Read-SetupJson $recordPath) $handoff
            Assert-HandoffJournals $handoff $Package
            $endpointCheckpoint={param($Point) $null=& $Checkpoint ('endpoint-'+$Point)}.GetNewClosure()
            Invoke-EndpointTransitionCore $Store $Path Migrate $original.EndpointId $AdapterId $RegistrationId `
                -VerifyRegistration $VerifyRegistration -VerifyApplied $VerifyApplied -CaptureServices $CaptureServices `
                -StopAudio $StopAudio -StartAudio $StartAudio -Checkpoint $endpointCheckpoint
            Complete-LegacyHandoff $Store $Path $Package $StopAudio $StartAudio $VerifyRegistration $VerifyApplied $Checkpoint
        } catch {
            $failure=$_
            # Reopen durable state; a commit may have succeeded even when the
            # caller observed an I/O error or died before retiring the old record.
            try { Complete-LegacyHandoff $Store $Path $Package $StopAudio $StartAudio $VerifyRegistration $VerifyApplied }
            catch { throw ($failure.Exception.Message+' Handoff recovery: '+$_.Exception.Message) }
            throw $failure
        }
    } finally { $lease.Dispose() }
}
