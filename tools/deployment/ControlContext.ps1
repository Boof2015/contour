Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'Deployment.ps1')
$script:ControlContextId = '{8b5699ab-dbc3-4ff7-82ed-8fbcbf7c8cf4}'

function Get-ControlContextKeys([string] $EndpointId) {
    $context = (Get-EndpointPath $EndpointId) + '\FxProperties\' + $script:ControlContextId
    @($context, ($context+'\Default'), ($context+'\User'), ($context+'\Volatile'))
}
function New-ControlContextPlan($Store, [string] $EndpointId) {
    $keys = @(Get-ControlContextKeys $EndpointId)
    $fx = (Get-EndpointPath $EndpointId) + '\FxProperties'
    if (-not $Store.Exists($fx)) { throw 'Endpoint effects store is absent.' }
    foreach ($key in $keys) { if ($Store.Exists($key)) { throw 'Contour context already exists; inspect/recover its journal.' } }
    $plan = [pscustomobject]@{
        Schema=1;Kind='ContourControlContext';EndpointId=$EndpointId;Keys=$keys
        Baseline=$Store.Snapshot($fx);ParentSecurity=$Store.Security($fx)
        Values=@(
            [pscustomobject]@{Path=$keys[0];Name=($script:EffectPrefix+'0');Type=1;Data=(ConvertTo-HexString '{00000000-0000-0000-0000-000000000000}')},
            [pscustomobject]@{Path=$keys[1];Name=($script:ControlContextId+',3');Type=4;Data='01000000'}
        )
    }
    Assert-ControlContextPlan $plan
    return $plan
}
function Assert-ControlContextPlan($Plan) {
    if ($Plan.Schema -ne 1 -or $Plan.Kind -cne 'ContourControlContext') { throw 'Invalid context plan schema.' }
    $keys = @(Get-ControlContextKeys $Plan.EndpointId)
    if (($Plan.Keys -join '|') -cne ($keys -join '|') -or @($Plan.Values).Count -ne 2) { throw 'Invalid context key/value scope.' }
    $a=$Plan.Values[0]; $b=$Plan.Values[1]
    if ($a.Path -cne $keys[0] -or $a.Name -cne ($script:EffectPrefix+'0') -or $a.Type -ne 1 -or
        $a.Data -cne (ConvertTo-HexString '{00000000-0000-0000-0000-000000000000}') -or
        $b.Path -cne $keys[1] -or $b.Name -cne ($script:ControlContextId+',3') -or $b.Type -ne 4 -or $b.Data -cne '01000000') {
        throw 'Invalid context provisioning values.'
    }
}
function New-ControlContextJournal($Plan) {
    Assert-ControlContextPlan $Plan
    [pscustomobject]@{Schema=1;Plan=$Plan;State='Prepared';KeyIntents=@();ValueIntents=@();RuntimeIntents=@();SavedProfileCleanup=$false;Conflicts=@()}
}
function Invoke-ControlContextApply($Store, $Journal, [string] $JournalPath, [switch] $AllowRestorePrivilege, [int] $FailAfter=-1) {
    Assert-ControlContextPlan $Journal.Plan
    if ($Journal.State -ne 'Prepared') { throw 'Context journal has already been applied.' }
    $fx=(Get-EndpointPath $Journal.Plan.EndpointId)+'\FxProperties'
    if ($Store.Snapshot($fx) -cne $Journal.Plan.Baseline -or $Store.Security($fx) -cne $Journal.Plan.ParentSecurity) { throw 'Endpoint changed after context planning.' }
    $Journal.State='Applying'; Save-ContourJournal $Journal $JournalPath
    $operations=0
    foreach ($key in $Journal.Plan.Keys) {
        if ($Store.Exists($key)) { throw 'Context key appeared after planning.' }
        $Journal.KeyIntents+=@($key); Save-ContourJournal $Journal $JournalPath
        $Store.CreateControlContextKey($key,$AllowRestorePrivilege.IsPresent)
        if (++$operations -eq $FailAfter) { throw 'Injected context creation interruption.' }
    }
    for ($i=0; $i -lt $Journal.Plan.Values.Count; ++$i) {
        $value=$Journal.Plan.Values[$i]
        if ($null -ne $Store.Read($value.Path,$value.Name)) { throw 'Context value appeared after planning.' }
        $Journal.ValueIntents+=@($i); Save-ContourJournal $Journal $JournalPath
        $Store.Write($value.Path,$value.Name,$value.Type,$value.Data)
        if (-not (Test-RawEqual ($Store.Read($value.Path,$value.Name)) $value)) { throw 'Context value readback failed.' }
        if (++$operations -eq $FailAfter) { throw 'Injected context value interruption.' }
    }
    if ($Store.Security($fx) -cne $Journal.Plan.ParentSecurity) { throw 'Existing endpoint security changed.' }
    $Journal.State='Provisioned'; Save-ContourJournal $Journal $JournalPath
}
function Invoke-ControlContextRestore($Store, $Journal, [string] $JournalPath) {
    # Caller must quiesce any APO worker using the context before restoration.
    Assert-ControlContextPlan $Journal.Plan
    if ($Journal.Schema -ne 1) { throw 'Unknown context journal schema.' }
    foreach ($key in $Journal.KeyIntents) { if ($Journal.Plan.Keys -cnotcontains $key) { throw 'Invalid context key intent.' } }
    foreach ($index in $Journal.ValueIntents) { if ([int]$index -notin @(0,1)) { throw 'Invalid context value intent.' } }
    $volatile=$Journal.Plan.Keys[3]
    # Preview request, status, and the engine identity published by current APOs.
    # Keep this explicit: unrelated values must still stop context deletion.
    $names=@(($script:ControlContextId+',1'),($script:ControlContextId+',2'),($script:ControlContextId+',5'))
    foreach ($name in $Journal.RuntimeIntents) { if ($names -cnotcontains $name) { throw 'Invalid runtime cleanup intent.' } }
    # Persistent user data is preserved unless this fresh-context experiment
    # explicitly journaled ownership before its first test Save. Older journals
    # have no such intent and must not remove saved settings.
    $savedCleanup=$false
    if ($Journal.PSObject.Properties.Name -contains 'SavedProfileCleanup') {
        if ($Journal.SavedProfileCleanup -isnot [bool]) { throw 'Invalid saved-profile cleanup intent.' }
        $savedCleanup=$Journal.SavedProfileCleanup
        if ($savedCleanup -and $Journal.KeyIntents -cnotcontains $Journal.Plan.Keys[2]) { throw 'Saved-profile cleanup requires a journaled new User store.' }
    }
    $Journal.State='Restoring'; $Journal.Conflicts=@(); Save-ContourJournal $Journal $JournalPath
    if ($Journal.KeyIntents -ccontains $volatile) {
        foreach ($name in $names) {
            try {
                $current=$Store.Read($volatile,$name)
                if ($null -ne $current) {
                    if ($Journal.RuntimeIntents -cnotcontains $name) { $Journal.RuntimeIntents+=@($name); Save-ContourJournal $Journal $JournalPath }
                    if (-not (Test-RawEqual ($Store.Read($volatile,$name)) $current)) { throw 'Preview property changed during recovery; stop the audio worker first.' }
                    $Store.DeleteValue($volatile,$name)
                }
            } catch { $Journal.Conflicts+=@($_.Exception.Message) }
        }
    }
    if ($savedCleanup) {
        $user=$Journal.Plan.Keys[2]; $name=$script:ControlContextId+',4'
        try {
            $current=$Store.Read($user,$name)
            if ($null -ne $current) {
                if (-not (Test-RawEqual ($Store.Read($user,$name)) $current)) { throw 'Saved settings changed during recovery; stop the test writer first.' }
                $Store.DeleteValue($user,$name)
            }
        } catch { $Journal.Conflicts+=@($_.Exception.Message) }
    }
    foreach ($i in @($Journal.ValueIntents | Sort-Object -Descending -Unique)) {
        $value=$Journal.Plan.Values[[int]$i]
        try {
            $current=$Store.Read($value.Path,$value.Name)
            if (Test-RawEqual $current $value) { $Store.DeleteValue($value.Path,$value.Name) }
            elseif ($null -ne $current) { $Journal.Conflicts+=@('Context provisioning value changed externally: '+$value.Name) }
        } catch { $Journal.Conflicts+=@($_.Exception.Message) }
    }
    foreach ($key in @($Journal.KeyIntents | Sort-Object Length -Descending -Unique)) {
        try { if (-not $Store.DeleteEmpty($key)) { $Journal.Conflicts+=@('Unrecognized context data retained: '+$key) } }
        catch { $Journal.Conflicts+=@($_.Exception.Message) }
    }
    $fx=(Get-EndpointPath $Journal.Plan.EndpointId)+'\FxProperties'
    # A device that Windows has removed entirely has no remaining context or
    # parent ACL to restore. Never recreate it just to finish uninstallation.
    if ($Store.Exists((Get-EndpointPath $Journal.Plan.EndpointId)) -and
        (-not $Store.Exists($fx) -or $Store.Security($fx) -cne $Journal.Plan.ParentSecurity)) {
        $Journal.Conflicts+=@('Existing endpoint security differs from baseline.')
    }
    $Journal.State=if ($Journal.Conflicts.Count -eq 0) { 'Restored' } else { 'RecoveryRequired' }
    Save-ContourJournal $Journal $JournalPath
    return $Journal.Conflicts.Count -eq 0
}
