# One shared runtime switch, with immutable DLLs and a single catalog commit.
# All callbacks are supplied by the protected host or isolated test fixtures.
function Assert-RuntimeBuild($Build) {
    if ($Build.Sha256 -cnotmatch '^[0-9A-F]{64}\z') { throw 'Invalid engine fingerprint.' }
    if ($Build.Id -cne 'original') { Assert-DeploymentId $Build.Id }
}
function Get-RuntimeBuildPath($Plan,$Build) {
    Assert-RuntimeBuild $Build
    if ($Build.Id -ceq 'original') {
        if ($Build.Sha256 -cne $Plan.DllSha256) { throw 'Original engine fingerprint changed.' }
        return $Plan.TargetDll
    }
    Join-Path $Plan.Package ('runtimes\'+$Build.Id+'\contour-apo.dll')
}
function Get-RuntimeRecord($Catalog,$Plan) {
    if ($Catalog.Schema -eq 4) { return $Catalog.Runtime }
    [pscustomobject]@{Active=[pscustomobject]@{Id='original';Sha256=$Plan.DllSha256};Update=$null;Deferred=@();LastError=''}
}
function Assert-RuntimeCatalog($Catalog) {
    if ($Catalog.Schema -ne 4) { return }
    if ($Catalog.Registrations.Count -ne 1 -or $Catalog.Registrations[0].Mode -ne 'Legacy' -or
        $null -ne (Get-DeploymentHandoff $Catalog)) { throw 'Runtime updates require one shared global registration.' }
    $runtime=$Catalog.Runtime;Assert-RuntimeBuild $runtime.Active
    if ($runtime.LastError -isnot [string]) { throw 'Invalid engine update diagnostic.' }
    foreach ($id in $runtime.Deferred) { [void](Get-EndpointPath $id) }
    $op=$runtime.Update;if ($null -eq $op) { return }
    Assert-DeploymentId $op.Id;Assert-RuntimeBuild $op.Before;Assert-RuntimeBuild $op.After
    if ($op.After.Id -ceq 'original' -or $op.After.Id -ieq $op.Before.Id -or
        $op.Before.Sha256 -ceq $op.After.Sha256 -or
        $op.Phase -notin @('Prepared','Writing','Verifying','Restoring','RecoveryRequired') -or
        $op.AudioRestartPending -isnot [bool] -or $null -ne $Catalog.Operation -or
        ($op.Intents -join ',') -notin @('','0','0,1') -or
        ($runtime.Active | ConvertTo-Json -Compress) -cne ($op.Before | ConvertTo-Json -Compress) -or
        ($Catalog.Bindings | ConvertTo-Json -Depth 20 -Compress) -cne ($op.Bindings | ConvertTo-Json -Depth 20 -Compress)) {
        throw 'Invalid engine update transaction.'
    }
    foreach ($i in $op.Intents) { if ($i -isnot [int] -and $i -isnot [long]) { throw 'Invalid engine write intent.' } }
    if (-not @($op.Services).Count) { throw 'Missing audio service recovery state.' }
    foreach ($service in $op.Services) { if ($service -cnotmatch '^[A-Za-z0-9_.-]{1,256}\z') { throw 'Invalid audio service.' } }
    if ((@($op.Settings | ForEach-Object EndpointId) -join '|') -cne (@($op.Bindings | ForEach-Object EndpointId) -join '|')) { throw 'Missing engine update settings guards.' }
    foreach ($guard in $op.Settings) {
        foreach ($part in @('Default','User','Security')) { if ($guard.Value.$part -isnot [string]) { throw 'Invalid settings guard.' } }
    }
}
function Assert-NoRuntimeUpdate($Catalog) {
    if ($Catalog.Schema -eq 4 -and $null -ne $Catalog.Runtime.Update) { throw 'Recover the interrupted engine update before changing devices.' }
}
function Get-RuntimeChanges($Plan,$Before,$After) {
    foreach ($id in $script:ClassIds) {
        [pscustomobject]@{Path=('SOFTWARE\Classes\CLSID\'+$id+'\InprocServer32');Name='';
            Before=[pscustomobject]@{Type=1;Data=(ConvertTo-HexString (Get-RuntimeBuildPath $Plan $Before))};
            After=[pscustomobject]@{Type=1;Data=(ConvertTo-HexString (Get-RuntimeBuildPath $Plan $After))}}
    }
}
function Assert-RuntimeRegistration($Store,$Plan,$Build) {
    $fx=(Get-EndpointPath $Plan.Endpoint.Id)+'\FxProperties'
    $paths=@($script:ClassIds | ForEach-Object { 'SOFTWARE\Classes\CLSID\'+$_+'\InprocServer32' })
    $dll=[pscustomobject]@{Type=1;Data=(ConvertTo-HexString (Get-RuntimeBuildPath $Plan $Build))}
    foreach ($change in $Plan.Changes) {
        if ($change.Path -ieq $fx) { continue }
        $expected=if ($change.Name -ceq '' -and $paths -icontains $change.Path) { $dll } else { $change.After }
        if (-not (Test-RawEqual ($Store.Read($change.Path,$change.Name)) $expected)) { throw 'Shared engine registration changed externally.' }
    }
}
function Assert-RuntimeTargets($Store,$Catalog,$Operation,[switch]$Settings) {
    foreach ($binding in $Operation.Bindings) { Assert-EndpointAssignments $Store $Catalog $binding.EndpointId $binding }
    if ($Settings) {
        foreach ($guard in $Operation.Settings) {
            $now=Get-EndpointSettings $Store $guard.EndpointId
            foreach ($part in @('Default','User','Security')) {
                if ($now.$part -cne $guard.Value.$part) { throw 'Device EQ changed during the engine update. Your newer settings were preserved.' }
            }
        }
    }
}
function Confirm-RuntimeBuild([scriptblock]$VerifyBuild,$Build) {
    $result=& $VerifyBuild $Build
    if ($result -isnot [bool] -or -not $result) { throw 'The engine files could not be verified.' }
}
function Confirm-RuntimeOutputs([scriptblock]$VerifyOutput,$Bindings,$Build) {
    foreach ($binding in $Bindings) {
        $result=& $VerifyOutput $binding.EndpointId $Build
        if ($null -eq $result -or $result.Verified -isnot [bool] -or -not $result.Verified -or $result.Inactive -isnot [bool]) {
            throw ('The engine did not respond correctly on '+$binding.EndpointId+'.')
        }
        if ($result.Inactive) { $binding.EndpointId }
    }
}
function Restore-RuntimeUpdate($Store,$Catalog,[string]$Path,$Plan,[scriptblock]$VerifyBuild,[scriptblock]$VerifyOutput,
    [scriptblock]$StopAudio,[scriptblock]$StartAudio,[scriptblock]$Checkpoint={param($Point)}) {
    $op=$Catalog.Runtime.Update;if ($null -eq $op) { return $true }
    $op.Phase='Restoring';$op.AudioRestartPending=$true
    $errors=@();$restored=$false
    try {
        Save-DeploymentState $Catalog $Path
        Confirm-RuntimeBuild $VerifyBuild $op.Before
        $null=& $StopAudio $op.Services
        $changes=@(Get-RuntimeChanges $Plan $op.Before $op.After)
        foreach ($i in @($op.Intents | Sort-Object -Descending)) {
            $change=$changes[$i];$current=$Store.Read($change.Path,$change.Name)
            if (Test-RawEqual $current $change.Before) { continue }
            if (-not (Test-RawEqual $current $change.After)) { $errors+=@('A newer engine registration was preserved.');continue }
            $Store.Write($change.Path,$change.Name,$change.Before.Type,$change.Before.Data)
            $null=& $Checkpoint ('restored-'+$i)
        }
        Assert-RuntimeRegistration $Store $Plan $op.Before
        Assert-RuntimeTargets $Store $Catalog $op
        $restored=$errors.Count -eq 0
    } catch { $errors+=@($_.Exception.Message) }
    try {
        $null=& $StartAudio $op.Services;$op.AudioRestartPending=$false
        $null=& $Checkpoint 'rollback-audio-restarted'
        if ($restored) { $Catalog.Runtime.Deferred=@(Confirm-RuntimeOutputs $VerifyOutput $op.Bindings $op.Before) }
    } catch { $errors+=@($_.Exception.Message) }
    if ($errors.Count) { $op.Phase='RecoveryRequired';$Catalog.Runtime.LastError=$errors -join ' ' }
    else { $Catalog.Runtime.Update=$null }
    Save-DeploymentState $Catalog $Path
    return $errors.Count -eq 0
}
function Invoke-RuntimeUpdate($Store,[string]$Package,$Build,
    [Parameter(Mandatory=$true)][scriptblock]$VerifyBuild,[Parameter(Mandatory=$true)][scriptblock]$VerifyOutput,
    [Parameter(Mandatory=$true)][scriptblock]$CaptureServices,[Parameter(Mandatory=$true)][scriptblock]$StopAudio,
    [Parameter(Mandatory=$true)][scriptblock]$StartAudio,[scriptblock]$Checkpoint={param($Point)}) {
    $path=Join-Path $Package 'endpoints.json';$lease=Lock-DeploymentState $path;$operation=$null
    try {
        $catalog=Read-DeploymentState $path;$record=Read-SharedSetup $Package
        Assert-SharedCatalog $catalog $record -AllowRuntimeUpdate
        if ($null -ne $catalog.Operation -or @($record.Contexts | Where-Object ContextState -eq 'Provisioning').Count) { throw 'Recover interrupted device setup first.' }
        $plan=(Read-SetupJson (Join-Path $Package 'journal.json')).Plan;Assert-ContourPlan $plan
        if ($plan.Id -cne $record.Id -or $catalog.Registrations[0].Packages[0].Sha256 -cne $plan.DllSha256) { throw 'Engine update ownership does not match the original installation.' }
        $runtime=Get-RuntimeRecord $catalog $plan
        if ($null -ne $runtime.Update) {
            $operation=$runtime.Update
            if (-not (Restore-RuntimeUpdate $Store $catalog $path $plan $VerifyBuild $VerifyOutput $StopAudio $StartAudio $Checkpoint)) {
                throw ('Engine recovery needs attention. '+$catalog.Runtime.LastError)
            }
            return
        }
        if ($null -eq $Build) { throw 'No interrupted engine update needs recovery.' }
        Assert-RuntimeBuild $Build
        if ($Build.Id -ceq 'original' -or $Build.Sha256 -ceq $runtime.Active.Sha256) { throw 'This engine build is already installed.' }
        Confirm-RuntimeBuild $VerifyBuild $runtime.Active;Confirm-RuntimeBuild $VerifyBuild $Build
        Assert-RuntimeRegistration $Store $plan $runtime.Active
        $operation=[pscustomobject]@{Id=[guid]::NewGuid().ToString('D');Before=$runtime.Active;After=$Build;Phase='Prepared';Intents=@();
            AudioRestartPending=$true;Services=@(& $CaptureServices);Bindings=@($catalog.Bindings);
            Settings=@($catalog.Bindings | ForEach-Object { [pscustomobject]@{EndpointId=$_.EndpointId;Value=(Get-EndpointSettings $Store $_.EndpointId)} })}
        Assert-RuntimeTargets $Store $catalog $operation -Settings
        $runtime.Update=$operation;$runtime.LastError='';$catalog.Schema=4
        if ($null -eq $catalog.PSObject.Properties['Runtime']) { $catalog | Add-Member NoteProperty Runtime $runtime }
        else { $catalog.Runtime=$runtime }
        Save-DeploymentState $catalog $path
        $committed=$false
        try {
            $null=& $Checkpoint 'update-prepared'
            $null=& $StopAudio $operation.Services;$null=& $Checkpoint 'update-audio-stopped'
            Confirm-RuntimeBuild $VerifyBuild $operation.Before;Confirm-RuntimeBuild $VerifyBuild $operation.After
            Assert-RuntimeRegistration $Store $plan $operation.Before;Assert-RuntimeTargets $Store $catalog $operation -Settings
            $operation.Phase='Writing';Save-DeploymentState $catalog $path
            $changes=@(Get-RuntimeChanges $plan $operation.Before $operation.After)
            for ($i=0;$i -lt $changes.Count;++$i) {
                $change=$changes[$i];$operation.Intents+=@($i);Save-DeploymentState $catalog $path
                $null=& $Checkpoint ('update-intent-'+$i)
                if (-not (Test-RawEqual ($Store.Read($change.Path,$change.Name)) $change.Before)) { throw 'Engine registration changed before update.' }
                $Store.Write($change.Path,$change.Name,$change.After.Type,$change.After.Data)
                $null=& $Checkpoint ('update-written-'+$i)
                if (-not (Test-RawEqual ($Store.Read($change.Path,$change.Name)) $change.After)) { throw 'Engine update did not persist.' }
            }
            $operation.Phase='Verifying';Save-DeploymentState $catalog $path
            $null=& $StartAudio $operation.Services;$operation.AudioRestartPending=$false
            $null=& $Checkpoint 'update-audio-restarted'
            $deferred=@(Confirm-RuntimeOutputs $VerifyOutput $operation.Bindings $operation.After)
            Assert-RuntimeRegistration $Store $plan $operation.After;Assert-RuntimeTargets $Store $catalog $operation -Settings
            Confirm-RuntimeBuild $VerifyBuild $operation.After
            $null=& $Checkpoint 'update-verified'
            $runtime.Active=$Build;$runtime.Deferred=$deferred;$runtime.Update=$null
            Save-DeploymentState $catalog $path;$committed=$true
            $null=& $Checkpoint 'update-committed'
        } catch {
            if ($committed) { throw }
            $failure=$_.Exception.Message
            # Reload the last durable authority if the final commit failed.
            $catalog=Read-DeploymentState $path
            $catalog.Runtime.LastError=$failure
            if (Restore-RuntimeUpdate $Store $catalog $path $plan $VerifyBuild $VerifyOutput $StopAudio $StartAudio $Checkpoint) {
                throw ('The engine update failed. The previous engine was restored. '+$failure)
            }
            throw ('The engine update needs recovery. '+$failure+' '+$catalog.Runtime.LastError)
        }
    } finally {
        try { if ($null -ne $operation -and $operation.AudioRestartPending) { $null=& $StartAudio $operation.Services } }
        finally { $lease.Dispose() }
    }
}
