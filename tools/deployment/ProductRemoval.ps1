# Full product removal is a monotonic, resumable operation. Its schema-4
# authority record prevents older device setup helpers from reattaching a
# partially removed runtime. Host callbacks own services; tests use HKCU only.
. (Join-Path $PSScriptRoot 'SharedSetup.ps1')

function Remove-ProductStartupEntry($Store,[string]$Executable) {
    $path='Software\Microsoft\Windows\CurrentVersion\Run'
    $current=$Store.Read($path,'Contour')
    if ($null -eq $current -or $current.Type -ne 1) { return }
    $expected='"'+$Executable+'" --startup'
    $command=[Text.Encoding]::Unicode.GetString([Contour.Deployment.RegistryStore]::Decode($current.Data)).TrimEnd([char]0)
    if (-not $command.Equals($expected,[StringComparison]::OrdinalIgnoreCase)) { return }
    if (-not (Test-RawEqual ($Store.Read($path,'Contour')) $current)) { throw 'Windows startup changed during removal. Retry uninstallation.' }
    $Store.DeleteValue($path,'Contour')
}

function Assert-ProductRemovalRecord($Record) {
    if ($Record.Schema -ne 4 -or $Record.Kind -cne 'ContourProductRemoval' -or
        $Record.State -notin @('Removing','Removed') -or $Record.AudioRestartPending -isnot [bool]) {
        throw 'Invalid application removal record.'
    }
    if ($Record.Original.Schema -eq 1) { Assert-LegacySetupRecord $Record.Original }
    elseif ($Record.Original.Schema -eq 3) {
        Assert-SharedSetupRecord $Record.Original
        Assert-SharedCatalog $Record.Catalog $Record.Original
        if ($null -ne $Record.Catalog.Operation) { throw 'Recover the interrupted device operation before removing Contour.' }
    } else { throw 'This installation needs recovery before application removal.' }
    if ($Record.Id -cne $Record.Original.Id -or $Record.EndpointId -cne $Record.Original.EndpointId) {
        throw 'Removal ownership changed.'
    }
    foreach ($service in $Record.Services) {
        if ($service -cnotmatch '^[A-Za-z0-9_.-]{1,256}\z') { throw 'Invalid audio service recovery record.' }
    }
    if ($Record.State -eq 'Removed' -and $Record.AudioRestartPending) { throw 'Audio service recovery is incomplete.' }
}
function Get-ProductRemovalChanges($Record,[string]$Package) {
    Assert-ProductRemovalRecord $Record
    $attachment=Read-SetupJson (Join-Path $Package 'journal.json');$plan=$attachment.Plan
    Assert-ContourPlan $plan
    if ($plan.Id -cne $Record.Id -or $plan.Endpoint.Id -cne $Record.EndpointId) { throw 'Removal journal belongs to another installation.' }
    foreach ($i in $attachment.ValueIntents) {
        if (($i -isnot [int] -and $i -isnot [long]) -or $i -lt 0 -or $i -ge $plan.Changes.Count) { throw 'Invalid removal value intent.' }
    }
    foreach ($key in $attachment.KeyIntents) { if ($plan.CreateKeys -cnotcontains $key) { throw 'Invalid removal key intent.' } }
    $shared=$Record.Original.Schema -eq 3
    $runtime=if ($shared) { (Get-RuntimeRecord $Record.Catalog $plan).Active } else { [pscustomobject]@{Id='original';Sha256=$plan.DllSha256} }
    $dll=Get-RuntimeBuildPath $plan $runtime
    # Detach every journaled endpoint, even if it is disconnected. A composite
    # binding restores the original vendor list byte-for-byte.
    if ($shared) {
        foreach ($binding in $Record.Catalog.Bindings) {
            foreach ($change in @(Get-EndpointChanges $Record.Catalog $binding $null)) {
                [pscustomobject]@{Path=((Get-EndpointPath $binding.EndpointId)+'\FxProperties');Name=$change.Name;
                    Before=$change.Before;After=$change.After;EndpointId=$binding.EndpointId}
            }
        }
    }
    foreach ($i in @($attachment.ValueIntents | Sort-Object -Unique)) {
        $change=$plan.Changes[$i];$endpoint=$change.Path -ieq ((Get-EndpointPath $plan.Endpoint.Id)+'\FxProperties')
        if ($shared -and $endpoint) { continue }
        $before=$change.After
        if ($change.Path.EndsWith('\InprocServer32') -and $change.Name -ceq '') {
            $before=[pscustomobject]@{Type=1;Data=(ConvertTo-HexString $dll)}
        }
        [pscustomobject]@{Path=$change.Path;Name=$change.Name;Before=$before;After=$change.Before;
            EndpointId=$(if ($endpoint) { $plan.Endpoint.Id } else { '' })}
    }
}
function Test-RemovalEndpointGone($Store,$Change) {
    $Change.EndpointId -and -not $Store.Exists((Get-EndpointPath $Change.EndpointId))
}
function Assert-ProductRemovalValues($Store,$Changes) {
    foreach ($change in $Changes) {
        if (Test-RemovalEndpointGone $Store $change) { continue }
        $current=$Store.Read($change.Path,$change.Name)
        if (-not (Test-RawEqual $current $change.Before) -and -not (Test-RawEqual $current $change.After)) {
            throw ('A setting changed outside Contour and was kept: '+$change.Path+' ['+$change.Name+'].')
        }
    }
}
function Invoke-ProductRemovalChange($Store,$Change) {
    if (Test-RemovalEndpointGone $Store $Change) { return }
    $current=$Store.Read($Change.Path,$Change.Name)
    if (Test-RawEqual $current $Change.After) { return }
    if (-not (Test-RawEqual $current $Change.Before)) { throw 'A setting changed during removal and was kept.' }
    if ($null -eq $Change.After) { $Store.DeleteValue($Change.Path,$Change.Name) }
    else { $Store.Write($Change.Path,$Change.Name,$Change.After.Type,$Change.After.Data) }
    if (-not (Test-RawEqual ($Store.Read($Change.Path,$Change.Name)) $Change.After)) { throw 'A removal change did not persist.' }
}
function ConvertFrom-RemovalValue($Value) {
    [Text.Encoding]::Unicode.GetString([Contour.Deployment.RegistryStore]::Decode($Value.Data)).TrimEnd([char]0)
}
function Assert-ProductRemovalReferences($Store,$Changes,[string]$RegisteredPackage) {
    $audio='SOFTWARE\Microsoft\Windows\CurrentVersion\MMDevices\Audio'
    $references=@()
    if ($Store.Exists($audio)) {
        foreach ($entry in $Store.Entries($audio)) {
            if ($entry.Name -notlike ($script:EffectPrefix+'*')) { continue }
            $text=ConvertFrom-RemovalValue $entry.Value
            foreach ($id in $script:ClassIds) {
                if ($text.IndexOf($id,[StringComparison]::OrdinalIgnoreCase) -ge 0) { $references+=@($entry);break }
            }
        }
    }
    $classes='SOFTWARE\Classes\CLSID'
    if ($Store.Exists($classes)) {
        foreach ($entry in $Store.Entries($classes)) {
            if (-not $entry.Path.EndsWith('\InprocServer32') -or $entry.Name -ne '') { continue }
            $text=ConvertFrom-RemovalValue $entry.Value
            if ($text.StartsWith($RegisteredPackage+'\',[StringComparison]::OrdinalIgnoreCase)) { $references+=@($entry) }
        }
    }
    foreach ($entry in $references) {
        $known=@($Changes | Where-Object { $_.Path -ieq $entry.Path -and $_.Name -ieq $entry.Name -and (Test-RawEqual $_.Before $entry.Value) })
        if ($known.Count -ne 1) { throw ('An untracked audio attachment still uses Contour. Its engine files were kept: '+$entry.Path) }
    }
}
function Get-ProductRemovalContexts($Record,[string]$Package) {
    $entries=if ($Record.Original.Schema -eq 3) { @($Record.Original.Contexts) }
        else { @([pscustomobject]@{EndpointId=$Record.EndpointId;ContextState='Provisioning'}) }
    foreach ($entry in $entries) {
        $path=if ($Record.Original.Schema -eq 3) { Get-SharedContextPath $Package $entry.EndpointId }
            else { Join-Path $Package 'context-journal.json' }
        if (-not (Test-Path -LiteralPath $path)) {
            if ($entry.ContextState -eq 'Ready') { throw 'A saved context recovery record is missing.' }
            continue
        }
        $journal=Read-SetupJson $path;Assert-ControlContextPlan $journal.Plan
        if ($journal.Plan.EndpointId -cne $entry.EndpointId) { throw 'Context journal belongs to another endpoint.' }
        [pscustomobject]@{Path=$path;Journal=$journal}
    }
}
function New-ProductRemovalRecord($Store,[string]$Package,[scriptblock]$CaptureServices) {
    $original=Read-SetupJson (Join-Path $Package 'device-setup.json')
    if ($original.Schema -eq 4) { Assert-ProductRemovalRecord $original;return $original }
    $catalog=if ($original.Schema -eq 3) { Read-DeploymentState (Join-Path $Package 'endpoints.json') } else { $null }
    $record=[pscustomobject]@{Schema=4;Kind='ContourProductRemoval';Id=$original.Id;EndpointId=$original.EndpointId;
        Name=$original.Name;State='Removing';AudioRestartPending=$true;
        Services=@();Original=$original;Catalog=$catalog}
    Assert-ProductRemovalRecord $record
    $changes=@(Get-ProductRemovalChanges $record $Package)
    Assert-ProductRemovalValues $Store $changes
    $plan=(Read-SetupJson (Join-Path $Package 'journal.json')).Plan
    Assert-ProductRemovalReferences $Store $changes $plan.Package
    $null=@(Get-ProductRemovalContexts $record $Package)
    $record.Services=@(if ($original.AudioRestartPending) { $original.Services } else { & $CaptureServices })
    Assert-ProductRemovalRecord $record
    return $record
}
function Invoke-ProductRemoval($Store,[string]$Package,
    [Parameter(Mandatory=$true)][scriptblock]$CaptureServices,
    [Parameter(Mandatory=$true)][scriptblock]$StopAudio,
    [Parameter(Mandatory=$true)][scriptblock]$StartAudio,
    [scriptblock]$Checkpoint={param($Point)}) {
    $path=Join-Path $Package 'device-setup.json'
    $record=New-ProductRemovalRecord $Store $Package $CaptureServices
    $changes=@(Get-ProductRemovalChanges $record $Package)
    $attachment=Read-SetupJson (Join-Path $Package 'journal.json')
    if ($record.State -eq 'Removed') {
        Assert-ProductRemovalValues $Store @($changes | ForEach-Object { [pscustomobject]@{Path=$_.Path;Name=$_.Name;Before=$_.After;After=$_.After;EndpointId=$_.EndpointId} })
        Assert-ProductRemovalReferences $Store @() $attachment.Plan.Package
        return
    }
    # Persist authority and the original running services before stopping audio.
    # A process crash at any later instruction can resume this same operation.
    $record.AudioRestartPending=$true;Save-ContourJournal $record $path
    $null=& $Checkpoint 'removal-owned'
    $complete=$false
    try {
        $null=& $StopAudio $record.Services
        $null=& $Checkpoint 'removal-audio-stopped'
        Assert-ProductRemovalValues $Store $changes
        Assert-ProductRemovalReferences $Store $changes $attachment.Plan.Package
        $endpoints=@($changes | Where-Object EndpointId)
        $globals=@($changes | Where-Object { -not $_.EndpointId })
        $index=0
        foreach ($change in $endpoints) {
            Invoke-ProductRemovalChange $Store $change
            $null=& $Checkpoint ('removal-endpoint-'+$index++)
        }
        # No endpoint, including an untracked/disconnected one, may still load
        # this runtime when its contexts and COM registration are removed.
        Assert-ProductRemovalReferences $Store $globals $attachment.Plan.Package
        $index=0
        foreach ($context in @(Get-ProductRemovalContexts $record $Package)) {
            $journal=$context.Journal
            if ($journal.KeyIntents -ccontains $journal.Plan.Keys[2]) {
                $journal | Add-Member NoteProperty SavedProfileCleanup $true -Force
                Save-ContourJournal $journal $context.Path
            }
            if (-not (Invoke-ControlContextRestore $Store $journal $context.Path)) {
                throw ('Device settings could not be fully removed. '+($journal.Conflicts -join '; '))
            }
            $null=& $Checkpoint ('removal-context-'+$index++)
        }
        $index=0
        foreach ($change in $globals) {
            Invoke-ProductRemovalChange $Store $change
            $null=& $Checkpoint ('removal-registration-'+$index++)
        }
        foreach ($key in @($attachment.KeyIntents | Sort-Object @{Expression={$_.Length};Descending=$true}, @{Expression={$_}} -Unique)) {
            if (-not $Store.DeleteEmpty($key)) { throw ('Unrecognized registration data was kept: '+$key) }
        }
        Assert-ProductRemovalReferences $Store @() $attachment.Plan.Package
        Preserve-UnsignedAudio (Join-Path $Package 'audio-policy.json')
        $complete=$true
    } finally {
        # Keep the durable restart flag if service recovery itself fails.
        $null=& $StartAudio $record.Services
        $record.AudioRestartPending=$false;Save-ContourJournal $record $path
    }
    $null=& $Checkpoint 'removal-audio-restarted'
    if ($complete) {
        $record.State='Removed';Save-ContourJournal $record $path
        $null=& $Checkpoint 'removal-complete'
    }
}
