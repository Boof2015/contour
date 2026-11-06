Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
if (-not ('Contour.Deployment.RegistryStore' -as [type])) {
    Add-Type -Path (Join-Path $PSScriptRoot 'RegistryStore.cs')
}

$script:ClassIds = @('{D1F3F732-FD5B-48AB-A989-5086B6FBEA37}', '{E841587D-E457-4F68-A425-C989A1DD8CFF}')
$script:EffectPrefix = '{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},'
$script:DisableEffects = '{1da5d803-d492-4edd-8c23-e0c0ffee7f0e},5'
$script:AudioSettings = 'SOFTWARE\Microsoft\Windows\CurrentVersion\Audio'
. (Join-Path $PSScriptRoot 'EffectPlacement.ps1')

function ConvertTo-HexString([string] $Text) {
    [BitConverter]::ToString([Text.Encoding]::Unicode.GetBytes($Text + [char]0)).Replace('-', '')
}
function Test-RawEqual($A, $B) {
    if ($null -eq $A -or $null -eq $B) { return ($null -eq $A -and $null -eq $B) }
    return ([uint32]$A.Type -eq [uint32]$B.Type -and [string]$A.Data -ceq [string]$B.Data)
}
function Save-ContourJournal($Journal, [string] $Path, [switch] $CreateNew) {
    [Contour.Deployment.DurableFile]::Save($Path, ($Journal | ConvertTo-Json -Depth 30), $CreateNew.IsPresent)
}
function Get-EndpointPath([string] $EndpointId) {
    if ($EndpointId -notmatch '^\{0\.0\.0\.00000000\}\.(\{[0-9a-fA-F-]{36}\})$') {
        throw 'A full, explicit playback endpoint ID is required.'
    }
    $guid = [guid]::Parse($Matches[1])
    'SOFTWARE\Microsoft\Windows\CurrentVersion\MMDevices\Audio\Render\' + $guid.ToString('B')
}
function Get-ContourClassKeys {
    foreach ($id in $script:ClassIds) {
        'SOFTWARE\Classes\CLSID\' + $id
        'SOFTWARE\Classes\CLSID\' + $id + '\InprocServer32'
        'SOFTWARE\Classes\AudioEngine\AudioProcessingObjects\' + $id
    }
}
function New-ContourPlan($Store, $Info, [string] $Dll, [string] $Hash, [string] $Id) {
    if ($Info.Schema -ne 1 -or $Info.EmbeddedManifest -or $null -eq $Info.Endpoint) {
        throw 'Deployment inspection is missing an endpoint or found an embedded DLL manifest.'
    }
    $endpointPath = Get-EndpointPath $Info.Endpoint.Id
    $fx = $endpointPath + '\FxProperties'
    $properties = $endpointPath + '\Properties'
    if (-not $Store.Exists($fx) -or -not $Store.Exists($properties)) {
        throw 'This lab installer requires an existing endpoint effect store.'
    }
    $placement=Get-AvailableEffectPlacement $Store $Info.Endpoint.Id
    foreach ($path in @($fx, $properties)) {
        $disabled = $Store.Read($path, $script:DisableEffects)
        if ($null -ne $disabled -and -not (Test-RawEqual $disabled ([pscustomobject]@{Type=4;Data='00000000'}))) {
            throw 'Audio enhancements are disabled or their state is unsupported; the lab installer will not override it.'
        }
    }
    foreach ($parent in @('SOFTWARE\Classes\CLSID', 'SOFTWARE\Classes\AudioEngine\AudioProcessingObjects')) {
        if (-not $Store.Exists($parent)) { throw "Required registry parent is absent: $parent" }
    }
    $createdKeys = @(Get-ContourClassKeys)
    foreach ($path in $createdKeys) {
        if ($Store.Exists($path)) { throw "Contour registration already exists; recover/remove it before planning: $path" }
    }
    $installId = ([guid]::Parse($Id)).ToString('D')
    $package = Join-Path ([Environment]::GetFolderPath('ProgramFiles')) ('ContourLab-' + $installId)
    $target = Join-Path $package 'contour-apo.dll'
    $changes = [Collections.Generic.List[object]]::new()
    if (@($Info.Classes).Count -ne 2) { throw 'Expected two Contour APO classes.' }
    for ($index = 0; $index -lt 2; ++$index) {
        $cls = $Info.Classes[$index]
        if ($cls.Clsid -cne $script:ClassIds[$index]) { throw 'Unexpected Contour class identity.' }
        $com = 'SOFTWARE\Classes\CLSID\' + $cls.Clsid
        $apo = 'SOFTWARE\Classes\AudioEngine\AudioProcessingObjects\' + $cls.Clsid
        foreach ($item in @(
            @{Path=$com;Name='';Text=$cls.Name},
            @{Path=($com+'\InprocServer32');Name='';Text=$target},
            @{Path=($com+'\InprocServer32');Name='ThreadingModel';Text='Both'})) {
            $changes.Add([pscustomobject]@{Path=$item.Path;Name=$item.Name;Before=$null;
                After=[pscustomobject]@{Type=1;Data=(ConvertTo-HexString $item.Text)}})
        }
        foreach ($value in $cls.Values) {
            $changes.Add([pscustomobject]@{Path=$apo;Name=$value.Name;Before=$null;
                After=[pscustomobject]@{Type=$value.Type;Data=$value.Data}})
        }
    }
    if ($null -ne $placement) {
        $original=$placement.Guards[2].Value
        $changes.Add([pscustomobject]@{Path=$fx;Name=($script:EffectPrefix+'15');Before=$original;After=(Add-CompositeClass $original $script:ClassIds[1])})
    } else {
        for ($index = 0; $index -lt 2; ++$index) {
            $changes.Add([pscustomobject]@{Path=$fx;Name=($script:EffectPrefix+($index+1));Before=$null;
                After=[pscustomobject]@{Type=1;Data=(ConvertTo-HexString $script:ClassIds[$index])}})
        }
    }
    $guards = @($fx, $properties) + $createdKeys
    $plan = [pscustomobject]@{
        Schema=1;Id=$installId;CreatedUtc=[DateTime]::UtcNow.ToString('o');Endpoint=$Info.Endpoint;
        SourceDll=[IO.Path]::GetFullPath($Dll);DllSha256=$Hash;Package=$package;TargetDll=$target;
        ProtectedAudioOverride=$Store.Read($script:AudioSettings, 'DisableProtectedAudioDG');
        Guards=@($guards | ForEach-Object { [pscustomobject]@{Path=$_;Snapshot=$Store.Snapshot($_)} });
        CreateKeys=$createdKeys;Changes=$changes.ToArray()
    }
    if ($null -ne $placement) { $plan.Schema=2;$plan | Add-Member NoteProperty Placement $placement }
    Assert-ContourPlan $plan
    return $plan
}

function Assert-ContourPlan($Plan) {
    if ($Plan.Schema -notin @(1,2) -or $Plan.DllSha256 -notmatch '^[0-9A-F]{64}$') { throw 'Invalid plan schema or hash.' }
    $composite=$Plan.Schema -eq 2
    if ($composite) { Assert-CompositePlacement $Plan.Placement }
    $expectedPackage = Join-Path ([Environment]::GetFolderPath('ProgramFiles')) ('ContourLab-'+([guid]::Parse($Plan.Id)).ToString('D'))
    if ($Plan.Package -cne $expectedPackage -or $Plan.TargetDll -cne (Join-Path $expectedPackage 'contour-apo.dll')) {
        throw 'Invalid package destination.'
    }
    $fx = (Get-EndpointPath $Plan.Endpoint.Id) + '\FxProperties'
    $keys = @(Get-ContourClassKeys)
    if (($Plan.CreateKeys -join '|') -cne ($keys -join '|')) { throw 'Unexpected registry key scope.' }
    $expectedGuards = @($fx, ((Get-EndpointPath $Plan.Endpoint.Id)+'\Properties')) + $keys
    if (($Plan.Guards.Path -join '|') -cne ($expectedGuards -join '|')) { throw 'Unexpected baseline scope.' }
    $allowedMetadata = @('FriendlyName','Copyright','MajorVersion','MinorVersion','Flags',
        'MinInputConnections','MaxInputConnections','MinOutputConnections','MaxOutputConnections',
        'MaxInstances','NumAPOInterfaces','APOInterface0')
    $seen = @{}
    foreach ($change in $Plan.Changes) {
        if ($null -ne $change.Before -and -not ($composite -and $change.Path -eq $fx)) { throw 'This installer only adds previously absent values.' }
        $identity = $change.Path + '|' + $change.Name
        if ($seen.ContainsKey($identity)) { throw 'Duplicate registry change.' }
        $seen[$identity] = $true
        if ($change.After.Data -cnotmatch '^([0-9A-F]{2}){0,4096}$') { throw 'Invalid value bytes.' }
        if ($change.Path -eq $fx) {
            if ($composite) {
                if ($change.Name -cne ($script:EffectPrefix+'15') -or
                    -not (Test-RawEqual $change.Before $Plan.Placement.Guards[2].Value) -or
                    -not (Test-RawEqual $change.After (Add-CompositeClass $change.Before $script:ClassIds[1]))) { throw 'Invalid composite endpoint change.' }
                continue
            }
            $stage = [array]::IndexOf(@(($script:EffectPrefix+'1'),($script:EffectPrefix+'2')), $change.Name)
            if ($stage -lt 0 -or $change.After.Type -ne 1 -or
                $change.After.Data -cne (ConvertTo-HexString $script:ClassIds[$stage])) { throw 'Invalid endpoint change.' }
        } elseif ($keys -contains $change.Path) {
            if ($change.Path -like '*\AudioProcessingObjects\*') {
                if ($allowedMetadata -cnotcontains $change.Name -or $change.After.Type -notin @(1,4)) { throw 'Invalid APO metadata.' }
            } elseif ($change.Path.EndsWith('\InprocServer32')) {
                $expected = if ($change.Name -eq '') { $Plan.TargetDll } elseif ($change.Name -ceq 'ThreadingModel') { 'Both' } else { throw 'Invalid COM value.' }
                if ($change.After.Type -ne 1 -or $change.After.Data -cne (ConvertTo-HexString $expected)) { throw 'Invalid COM DLL registration.' }
            } elseif ($change.Name -ne '' -or $change.After.Type -ne 1) { throw 'Invalid COM class name.' }
        } else { throw 'Change is outside the endpoint/Contour registration allowlist.' }
    }
    if ($composite) {
        if (@($Plan.Changes).Count -ne 31 -or -not $seen.ContainsKey($fx+'|'+$script:EffectPrefix+'15')) { throw 'Incomplete composite registration plan.' }
    } elseif (@($Plan.Changes).Count -ne 32 -or -not $seen.ContainsKey($fx+'|'+$script:EffectPrefix+'1') -or
        -not $seen.ContainsKey($fx+'|'+$script:EffectPrefix+'2')) { throw 'Incomplete registration plan.' }
}

function Assert-ContourBaseline($Store, $Plan) {
    Assert-ContourPlan $Plan
    foreach ($guard in $Plan.Guards) {
        if ($Store.Snapshot($guard.Path) -cne $guard.Snapshot) { throw "Stale plan: $($guard.Path) changed." }
    }
    if (-not (Test-RawEqual ($Store.Read($script:AudioSettings,'DisableProtectedAudioDG')) $Plan.ProtectedAudioOverride)) {
        throw 'The protected-audio environment changed after planning.'
    }
}

function New-ContourJournal($Plan) {
    [pscustomobject]@{Schema=1;Plan=$Plan;State='Prepared';KeyIntents=@();ValueIntents=@();Conflicts=@();LastError=''}
}

function Invoke-ContourApply($Store, $Journal, [string] $JournalPath, [int] $FailAfter = -1) {
    Assert-ContourBaseline $Store $Journal.Plan
    if ($Journal.State -ne 'Prepared') { throw 'A journal may only be applied once.' }
    $Journal.State = 'Applying'
    Save-ContourJournal $Journal $JournalPath
    $operations = 0
    foreach ($path in $Journal.Plan.CreateKeys) {
        $Journal.KeyIntents += $path
        Save-ContourJournal $Journal $JournalPath
        $Store.Create($path)
        if (++$operations -eq $FailAfter) { throw 'Injected interruption after key creation.' }
    }
    for ($index = 0; $index -lt $Journal.Plan.Changes.Count; ++$index) {
        $change = $Journal.Plan.Changes[$index]
        if (-not (Test-RawEqual ($Store.Read($change.Path,$change.Name)) $change.Before)) { throw 'Value changed before write.' }
        $Journal.ValueIntents += $index
        Save-ContourJournal $Journal $JournalPath
        # Recheck after making the intent durable. External writers must be quiescent;
        # registry compare/write is not a system-wide compare-and-swap primitive.
        if (-not (Test-RawEqual ($Store.Read($change.Path,$change.Name)) $change.Before)) { throw 'Value changed during write preparation.' }
        $Store.Write($change.Path,$change.Name,$change.After.Type,$change.After.Data)
        if (-not (Test-RawEqual ($Store.Read($change.Path,$change.Name)) $change.After)) { throw 'Registry write verification failed.' }
        if (++$operations -eq $FailAfter) { throw 'Injected interruption after value write.' }
    }
    $Journal.State = 'AttachedPendingGraphRebuild'
    Save-ContourJournal $Journal $JournalPath
}

function Invoke-ContourRestore($Store, $Journal, [string] $JournalPath) {
    Assert-ContourPlan $Journal.Plan
    if ($Journal.Schema -ne 1) { throw 'Unknown journal schema.' }
    foreach ($path in $Journal.KeyIntents) {
        if ($Journal.Plan.CreateKeys -notcontains $path) { throw 'Journal contains an unexpected key intent.' }
    }
    foreach ($index in $Journal.ValueIntents) {
        if ([int]$index -lt 0 -or [int]$index -ge $Journal.Plan.Changes.Count) { throw 'Invalid value intent.' }
    }
    $Journal.State = 'Restoring'
    $Journal.Conflicts = @()
    Save-ContourJournal $Journal $JournalPath
    $fx = (Get-EndpointPath $Journal.Plan.Endpoint.Id)+'\FxProperties'
    $endpointConflict = $false
    # Endpoint assignments are last on install, first on restoration.
    foreach ($index in @($Journal.ValueIntents | Sort-Object { [int]$_ } -Descending -Unique)) {
        $change = $Journal.Plan.Changes[[int]$index]
        # An ambiguous endpoint assignment might still refer to our classes.
        # Keep those classes usable until the conflict is explicitly resolved.
        if ($endpointConflict -and $change.Path -ne $fx) { continue }
        try {
            $current = $Store.Read($change.Path,$change.Name)
            if (Test-RawEqual $current $change.After) {
                if (-not (Test-RawEqual ($Store.Read($change.Path,$change.Name)) $change.After)) { throw 'Value changed during restoration.' }
                if ($null -eq $change.Before) { $Store.DeleteValue($change.Path,$change.Name) }
                else { $Store.Write($change.Path,$change.Name,$change.Before.Type,$change.Before.Data) }
                if (-not (Test-RawEqual ($Store.Read($change.Path,$change.Name)) $change.Before)) { throw 'Restoration did not persist.' }
            } elseif (-not (Test-RawEqual $current $change.Before)) {
                $Journal.Conflicts += "$($change.Path) [$($change.Name)] changed externally"
                if ($change.Path -eq $fx) { $endpointConflict = $true }
            }
        } catch {
            $Journal.Conflicts += $_.Exception.Message
            if ($change.Path -eq $fx) { $endpointConflict = $true }
        }
    }
    foreach ($path in @($Journal.KeyIntents | Sort-Object @{Expression={$_.Length};Descending=$true}, @{Expression={$_}} -Unique)) {
        if ($endpointConflict) { continue }
        try {
            if (-not $Store.DeleteEmpty($path)) { $Journal.Conflicts += "$path contains unowned data; retained" }
        } catch { $Journal.Conflicts += $_.Exception.Message }
    }
    $Journal.State = if ($Journal.Conflicts.Count -eq 0) { 'RestoredPendingGraphRebuild' } else { 'RecoveryRequired' }
    Save-ContourJournal $Journal $JournalPath
    return ($Journal.Conflicts.Count -eq 0)
}
