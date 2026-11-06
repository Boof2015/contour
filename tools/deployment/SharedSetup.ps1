# Registry-based installation: one application runtime, independent endpoint
# attachments. The elevated host holds Global\ContourLabDeployment and validates
# protected package files before invoking this code. Device removal retains the
# runtime and saved contexts, ready for another device or later reattachment.
Set-StrictMode -Version Latest
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot 'DeviceSetup.ps1')
. (Join-Path $PSScriptRoot 'EndpointDeployment.ps1')

function Get-SharedContextPath([string]$Package,[string]$EndpointId) {
    $key=Get-EndpointPath $EndpointId
    Join-Path $Package ('context-'+([guid]::Parse(($key -split '\\')[-1])).ToString('D')+'.json')
}
function Read-SharedSetup([string]$Package) {
    $record=Read-SetupJson (Join-Path $Package 'device-setup.json')
    Assert-SharedSetupRecord $record
    return $record
}
function Save-SharedSetup($Record,[string]$Package) {
    Assert-SharedSetupRecord $Record
    $current=Read-SharedSetup $Package
    if ($current.Id -ine $Record.Id -or $current.EndpointId -ine $Record.EndpointId) { throw 'Shared installation identity changed.' }
    Save-ContourJournal $Record (Join-Path $Package 'device-setup.json')
}
function Get-SharedContext($Record,[string]$EndpointId) {
    @($Record.Contexts | Where-Object { $_.EndpointId -ieq $EndpointId }) | Select-Object -First 1
}
function Assert-SharedCatalog($Catalog,$Record) {
    Assert-DeploymentState $Catalog
    Assert-NoDeploymentHandoff $Catalog
    if (@($Catalog.Registrations).Count -ne 1 -or $Catalog.Registrations[0].Id -ine $Record.Id -or
        $Catalog.Registrations[0].Mode -ne 'Legacy') { throw 'Shared registration does not match the installation.' }
    foreach ($binding in $Catalog.Bindings) {
        $context=Get-SharedContext $Record $binding.EndpointId
        if ($null -eq $context -or $context.ContextState -ne 'Ready' -or $context.AdapterId -ine $binding.AdapterId) {
            throw 'Installed endpoint has no matching saved context.'
        }
    }
}
function Assert-SharedRegistration($Store,[string]$Package,$Registration) {
    $attachment=Read-SetupJson (Join-Path $Package 'journal.json')
    Assert-ContourPlan $attachment.Plan
    if ($Registration.Mode -ne 'Legacy' -or $Registration.Id -ine $attachment.Plan.Id -or
        $Registration.Packages[0].Sha256 -cne $attachment.Plan.DllSha256) { throw 'Unexpected shared registration.' }
    # Tests use an isolated registry and fixture DLL; the production host also
    # verifies the protected DLL hash before mutation and each engine check.
    $fx=(Get-EndpointPath $attachment.Plan.Endpoint.Id)+'\FxProperties'
    foreach ($change in $attachment.Plan.Changes) {
        if ($change.Path -ine $fx -and -not (Test-RawEqual ($Store.Read($change.Path,$change.Name)) $change.After)) {
            throw 'Shared APO registration changed. Other devices were left untouched.'
        }
    }
}
function ConvertTo-SharedSetup($Store,[string]$Package,[string]$AdapterId,[scriptblock]$Checkpoint={param($Point)}) {
    $original=Read-SetupJson (Join-Path $Package 'device-setup.json')
    if ($original.Schema -eq 3) { Assert-SharedSetupRecord $original;return }
    Assert-LegacySetupRecord $original
    if ($original.State -ne 'Attached' -or $original.AudioRestartPending) { throw 'Recover the existing installation before adding another device.' }
    $attachment=Read-SetupJson (Join-Path $Package 'journal.json')
    $context=Read-SetupJson (Join-Path $Package 'context-journal.json')
    Assert-SetupJournals $original $attachment $context
    $legacy=Get-LegacyEndpointRecord $Store $attachment $context $AdapterId
    $path=Join-Path $Package 'endpoints.json'
    if (-not (Test-Path -LiteralPath $path)) { New-EndpointDeployment $path }
    $lease=Lock-DeploymentState $path
    try {
        $catalog=Read-DeploymentState $path
        Assert-NoDeploymentHandoff $catalog
        if ($null -ne $catalog.Operation -or @($catalog.Bindings).Count -gt 1 -or @($catalog.Registrations).Count -gt 1) {
            throw 'Unexpected pending shared installation.'
        }
        if ($catalog.Registrations.Count -and $catalog.Registrations[0].Id -ine $original.Id) { throw 'Another installation owns this catalog.' }
        if ($catalog.Bindings.Count -and ($catalog.Bindings[0].EndpointId -ine $original.EndpointId -or
            $catalog.Bindings[0].AdapterId -ine $AdapterId -or $catalog.Bindings[0].RegistrationId -ine $original.Id)) {
            throw 'Another endpoint owns this prepared catalog.'
        }
        Add-RegistrationRecord $catalog $legacy.Registration
        if ($null -ne (Get-BindingPlacement $legacy.Binding)) { $catalog.Schema=3 }
        $catalog.Bindings=@($legacy.Binding)
        Save-DeploymentState $catalog $path
        $null=& $Checkpoint 'shared-catalog-ready'
        $contextPath=Get-SharedContextPath $Package $original.EndpointId
        if (Test-Path -LiteralPath $contextPath) {
            if ((Read-SetupJson $contextPath | ConvertTo-Json -Depth 30 -Compress) -cne ($context | ConvertTo-Json -Depth 30 -Compress)) {
                throw 'A different saved context already exists.'
            }
        } else { Save-ContourJournal $context $contextPath -CreateNew }
        $null=& $Checkpoint 'shared-context-ready'
        Assert-LegacySetupAuthority $original $Package
        $current=Read-SetupJson (Join-Path $Package 'device-setup.json')
        if (($current | ConvertTo-Json -Depth 10 -Compress) -cne ($original | ConvertTo-Json -Depth 10 -Compress)) { throw 'Original setup changed during conversion.' }
        [void](Get-LegacyEndpointRecord $Store $attachment $context $AdapterId)
        $record=[pscustomobject]@{Schema=3;Kind='ContourSharedSetup';Id=$original.Id;EndpointId=$original.EndpointId;
            Name=$original.Name;State='Shared';AudioRestartPending=$false;
            Contexts=@([pscustomobject]@{EndpointId=$original.EndpointId;AdapterId=$AdapterId;Name=$original.Name;ContextState='Ready'})}
        Assert-SharedSetupRecord $record
        # Single durable authority transfer. Older setup hosts reject schema 3;
        # before this write they remain the owner. No registry changes occur.
        Save-ContourJournal $record (Join-Path $Package 'device-setup.json')
        $null=& $Checkpoint 'shared-committed'
    } finally { $lease.Dispose() }
}
function Prepare-SharedContext($Store,$Record,[string]$Package,[string]$EndpointId,[string]$Name,[string]$AdapterId,
    [scriptblock]$Checkpoint={param($Point)}) {
    $entry=Get-SharedContext $Record $EndpointId
    if ($null -ne $entry -and $entry.ContextState -eq 'Ready') {
        if ($entry.AdapterId -ine $AdapterId) { throw 'The device identity changed. Inspect it again.' }
        [void](Get-EndpointSettings $Store $EndpointId)
        return
    }
    if ($null -ne $entry -and $entry.ContextState -eq 'Provisioning') { throw 'Recover the interrupted device installation first.' }
    $context=New-ControlContextJournal (New-ControlContextPlan $Store $EndpointId)
    if ($null -eq $entry) {
        $entry=[pscustomobject]@{EndpointId=$EndpointId;AdapterId=$AdapterId;Name=$Name;ContextState='Provisioning'}
        $Record.Contexts+=@($entry)
    } else { $entry.AdapterId=$AdapterId;$entry.Name=$Name;$entry.ContextState='Provisioning' }
    Save-SharedSetup $Record $Package
    $null=& $Checkpoint 'context-recorded'
    $path=Get-SharedContextPath $Package $EndpointId
    if (Test-Path -LiteralPath $path) {
        $old=Read-SetupJson $path
        Assert-ControlContextPlan $old.Plan
        if ($old.Plan.EndpointId -ine $EndpointId -or $old.State -ne 'Restored') { throw 'Previous context recovery is unfinished.' }
    }
    Save-ContourJournal $context $path
    $null=& $Checkpoint 'context-journaled'
    Invoke-ControlContextApply $Store $context $path -AllowRestorePrivilege
    $null=& $Checkpoint 'context-provisioned'
    $entry.ContextState='Ready';Save-SharedSetup $Record $Package
}
function Recover-SharedContext($Store,$Record,[string]$Package,[string]$EndpointId) {
    $entry=Get-SharedContext $Record $EndpointId
    if ($null -eq $entry -or $entry.ContextState -ne 'Provisioning') { return }
    $path=Get-SharedContextPath $Package $EndpointId
    if (Test-Path -LiteralPath $path) {
        $context=Read-SetupJson $path
        Assert-ControlContextPlan $context.Plan
        if ($context.Plan.EndpointId -ine $EndpointId) { throw 'Context recovery targets another device.' }
        if ($context.State -eq 'Provisioned') {
            [void](Get-EndpointSettings $Store $EndpointId)
            $entry.ContextState='Ready'
        } else {
            if (-not (Invoke-ControlContextRestore $Store $context $path)) { throw 'Device context recovery needs attention.' }
            $entry.ContextState='Absent'
        }
    } else { $entry.ContextState='Absent' }
    Save-SharedSetup $Record $Package
}
function Invoke-SharedDeviceSetup($Store,[string]$Package,[ValidateSet('Install','Remove')][string]$Action,
    [string]$EndpointId,[string]$Name,[string]$AdapterId,
    [Parameter(Mandatory=$true)][scriptblock]$VerifyRegistration,[Parameter(Mandatory=$true)][scriptblock]$VerifyApplied,
    [Parameter(Mandatory=$true)][scriptblock]$CaptureServices,[Parameter(Mandatory=$true)][scriptblock]$StopAudio,
    [Parameter(Mandatory=$true)][scriptblock]$StartAudio,[scriptblock]$Checkpoint={param($Point)}) {
    $path=Join-Path $Package 'endpoints.json';$lease=Lock-DeploymentState $path
    try {
        $record=Read-SharedSetup $Package;$catalog=Read-DeploymentState $path
        Assert-SharedCatalog $catalog $record
        if ($null -ne $catalog.Operation) {
            if ($Action -ne 'Remove' -or $catalog.Operation.EndpointId -ine $EndpointId) { throw 'Finish the interrupted device operation first.' }
            if (-not (Restore-EndpointTransition $Store $catalog $path $StopAudio $StartAudio $VerifyRegistration $VerifyApplied)) {
                throw 'Device recovery needs attention.'
            }
            $catalog=Read-DeploymentState $path
        }
        $binding=Get-Binding $catalog $EndpointId
        if ($Action -eq 'Install') {
            if ($null -ne $binding) { throw 'Contour is already installed on this device.' }
            $placement=Get-AvailableEffectPlacement $Store $EndpointId
            Assert-EndpointAssignments $Store $catalog $EndpointId $null $placement
            Prepare-SharedContext $Store $record $Package $EndpointId $Name $AdapterId $Checkpoint
            Enable-UnsignedAudio $Store (Join-Path $Package 'audio-policy.json') $Checkpoint
            Invoke-EndpointTransitionCore $Store $path Attach $EndpointId $AdapterId $record.Id `
                -VerifyRegistration $VerifyRegistration -VerifyApplied $VerifyApplied -CaptureServices $CaptureServices `
                -StopAudio $StopAudio -StartAudio $StartAudio -Checkpoint $Checkpoint
        } else {
            if ($null -ne $binding) {
                Invoke-EndpointTransitionCore $Store $path Detach $EndpointId $binding.AdapterId '' `
                    -VerifyRegistration $VerifyRegistration -VerifyApplied $VerifyApplied -CaptureServices $CaptureServices `
                    -StopAudio $StopAudio -StartAudio $StartAudio -Checkpoint $Checkpoint
            } else {
                $placement=Get-AvailableEffectPlacement $Store $EndpointId
                Assert-EndpointAssignments $Store $catalog $EndpointId $null $placement
            }
            Recover-SharedContext $Store $record $Package $EndpointId
            Preserve-UnsignedAudio (Join-Path $Package 'audio-policy.json')
        }
    } finally { $lease.Dispose() }
}
