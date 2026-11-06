# Shared, journaled setup transaction. Registry/service adapters are supplied by
# the caller so interruption/recovery tests never touch Windows Audio or HKLM.
Set-StrictMode -Version Latest
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot 'ControlContext.ps1')

function Get-SetupJournalPath([string] $Package, [string] $Name) { Join-Path $Package $Name }
function Read-SetupJson([string] $Path) { Get-Content -LiteralPath $Path -Raw -Encoding UTF8 | ConvertFrom-Json }
function Save-SetupState($State, [string] $Package) { Save-ContourJournal $State (Get-SetupJournalPath $Package 'device-setup.json') }
function Assert-SetupState($State, [string] $Package) {
    if ($State.Schema -ne 1 -or $State.Kind -cne 'ContourDeviceSetup') { throw 'Unrecognized device setup record.' }
    [void](Get-EndpointPath $State.EndpointId)
    $expected=Join-Path ([Environment]::GetFolderPath('ProgramFiles')) ('ContourLab-'+([guid]::Parse($State.Id)).ToString('D'))
    if ($Package -cne $expected) { throw 'Setup record is outside its protected package.' }
    if ($State.State -notin @('Prepared','Installing','Attached','Removing','Removed','RecoveryRequired')) { throw 'Invalid setup state.' }
    if ($State.AudioRestartPending -isnot [bool]) { throw 'Invalid audio recovery state.' }
    foreach ($service in $State.Services) {
        if ($service -notmatch '^[A-Za-z0-9_.-]{1,256}$') { throw 'Invalid service recovery record.' }
    }
}
function Assert-SetupJournals($State, $Attachment, $Context) {
    Assert-ContourPlan $Attachment.Plan
    if ($Attachment.Plan.Id -cne $State.Id -or $Attachment.Plan.Endpoint.Id -cne $State.EndpointId) { throw 'Attachment journal belongs to another setup.' }
    if ($null -ne $Context) {
        Assert-ControlContextPlan $Context.Plan
        if ($Context.Plan.EndpointId -cne $State.EndpointId) { throw 'Control journal belongs to another output.' }
    }
}
function New-SetupState($Plan) {
    [pscustomobject]@{Schema=1;Kind='ContourDeviceSetup';Id=$Plan.Id;EndpointId=$Plan.Endpoint.Id;
        Name=$Plan.Endpoint.Name;State='Prepared';Services=@();AudioRestartPending=$false;Error=''}
}
function Invoke-SetupRecovery($Store, $State, [string] $Package) {
    $attachmentPath=Get-SetupJournalPath $Package 'journal.json'
    $contextPath=Get-SetupJournalPath $Package 'context-journal.json'
    $attachment=Read-SetupJson $attachmentPath
    $context=if (Test-Path -LiteralPath $contextPath) { Read-SetupJson $contextPath } else { $null }
    Assert-SetupJournals $State $attachment $context
    # Detach before deleting the control context. Never delete that context if
    # an ambiguous/foreign endpoint assignment might still load our APO.
    if (-not (Invoke-ContourRestore $Store $attachment $attachmentPath)) {
        throw ('Attachment recovery needs attention: '+($attachment.Conflicts -join '; '))
    }
    if ($null -ne $context -and -not (Invoke-ControlContextRestore $Store $context $contextPath)) {
        throw ('Control recovery needs attention: '+($context.Conflicts -join '; '))
    }
}
function Invoke-DeviceSetup($Store, $State, [string] $Package, [string] $Action,
    [scriptblock] $CaptureServices, [scriptblock] $StopAudio, [scriptblock] $StartAudio,
    [scriptblock] $Checkpoint={param($Point)}) {
    if ($Action -notin @('Install','Remove')) { throw 'Unknown device setup operation.' }
    $attachment=Read-SetupJson (Get-SetupJournalPath $Package 'journal.json')
    Assert-SetupJournals $State $attachment $null
    if ($Action -eq 'Install' -and $State.State -ne 'Prepared') { throw 'Setup is already in progress or installed.' }
    if ($Action -eq 'Remove' -and $State.State -eq 'Removed' -and -not $State.AudioRestartPending) { return }
    $failure=$null; $recovered=$false
    try {
        # Persist the originally running services before stopping any of them.
        # Recovery keeps this list even if a previous helper died during stop.
        if (-not $State.AudioRestartPending) {
            $State.Services=@(& $CaptureServices)
            $State.AudioRestartPending=$true
        }
        $State.State=if ($Action -eq 'Install') { 'Installing' } else { 'Removing' }
        $State.Error=''; Save-SetupState $State $Package
        & $StopAudio $State.Services
        & $Checkpoint 'audio-stopped'
        if ($Action -eq 'Install') {
            Invoke-ContourApply $Store $attachment (Get-SetupJournalPath $Package 'journal.json')
            & $Checkpoint 'attached'
            $context=New-ControlContextJournal (New-ControlContextPlan $Store $State.EndpointId)
            Save-ContourJournal $context (Get-SetupJournalPath $Package 'context-journal.json') -CreateNew
            & $Checkpoint 'context-journaled'
            Invoke-ControlContextApply $Store $context (Get-SetupJournalPath $Package 'context-journal.json') -AllowRestorePrivilege
            # Only our newly created User store is removed on detach. The
            # independent profile library in LocalAppData is never touched.
            $context.SavedProfileCleanup=$true
            Save-ContourJournal $context (Get-SetupJournalPath $Package 'context-journal.json')
            & $Checkpoint 'context-ready'
            $State.State='Attached'
        } else {
            Invoke-SetupRecovery $Store $State $Package
            $State.State='Removed'
        }
        Save-SetupState $State $Package
    } catch {
        $failure=$_.Exception.Message
        if ($Action -eq 'Install') {
            try {
                # Also quiesce audio on failure: stop itself may have failed.
                & $StopAudio $State.Services
                Invoke-SetupRecovery $Store $State $Package
                $recovered=$true
            } catch { $failure+=' Recovery: '+$_.Exception.Message }
        }
        $State.State=if ($recovered) { 'Removed' } else { 'RecoveryRequired' }
        $State.Error=$failure; Save-SetupState $State $Package
    } finally {
        if ($State.AudioRestartPending) {
            try { & $StartAudio $State.Services; $State.AudioRestartPending=$false }
            catch {
                $failure=(@($failure,$_.Exception.Message) | Where-Object { $_ }) -join ' '
                $State.Error=$failure; $State.State='RecoveryRequired'
            }
            Save-SetupState $State $Package
        }
    }
    if ($failure) { throw $failure }
}
