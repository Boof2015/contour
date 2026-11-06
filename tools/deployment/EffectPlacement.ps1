# Typed endpoint placement. Composite support adds one endpoint EQ instance;
# existing vendor stream/mode/endpoint effects are preserved byte for byte.
function Throw-EffectCompatibility([string]$Detail='') {
    $error=[InvalidOperationException]::new('Contour does not yet support this device''s existing audio effects. '+$Detail)
    $error.Data['reason']='existing_effects'
    throw $error
}
function Get-CompositeGuardNames {
    foreach ($id in @(13,14,15)) { $script:EffectPrefix+$id }
    foreach ($id in @(5,6,7)) { '{d3993a3f-99c2-4402-b5ec-a92a0367664b},'+$id }
}
function Read-EffectClassList($Raw) {
    if ($null -eq $Raw -or $Raw.Type -ne 7 -or $Raw.Data -cnotmatch '^([0-9A-F]{2}){4,4096}$') { throw 'Invalid composite effect list.' }
    $bytes=New-Object byte[] ($Raw.Data.Length/2)
    for ($i=0;$i -lt $bytes.Length;++$i) { $bytes[$i]=[Convert]::ToByte($Raw.Data.Substring($i*2,2),16) }
    $text=[Text.UnicodeEncoding]::new($false,$false,$true).GetString($bytes)
    if (-not $text.EndsWith(([string][char]0+[char]0))) { throw 'Composite effect list is not terminated.' }
    $items=@($text.Substring(0,$text.Length-2).Split([char]0))
    if ($items.Count -gt 32) { throw 'Composite effect list is too long.' }
    $seen=[Collections.Generic.HashSet[guid]]::new()
    foreach ($item in $items) {
        $guid=[guid]::Empty
        if (-not [guid]::TryParseExact($item,'B',[ref]$guid) -or $guid -eq [guid]::Empty -or -not $seen.Add($guid)) {
            throw 'Composite effect list has an invalid or duplicate class.'
        }
    }
    $items
}
function Assert-CompositePlacement($Placement) {
    if ($Placement.Kind -cne 'CompositeEndpoint' -or
        (($Placement.Guards.Name -join '|') -cne ((@(Get-CompositeGuardNames)) -join '|'))) { throw 'Invalid composite placement.' }
    $own=@($script:ClassIds)+@('{C96CE6C1-7F62-46A5-BDFF-46FCA19567AB}','{C2DB4B2B-8B4D-43C2-BCC3-A2B0D3E8FBA1}')
    for ($i=0;$i -lt 6;++$i) {
        $raw=$Placement.Guards[$i].Value
        if ($null -ne $raw) {
            $ids=@(Read-EffectClassList $raw)
            if ($i -lt 3 -and @($ids | Where-Object { $own -icontains $_ }).Count) { throw 'Composite baseline already contains Contour.' }
            if ($i -eq 2 -and $ids.Count -ge 32) { throw 'Composite endpoint list has no room for Contour.' }
        } elseif ($i -eq 2) { throw 'A composite endpoint list is required.' }
    }
}
function Add-CompositeClass($Original,[string]$ClassId) {
    [void]@(Read-EffectClassList $Original)
    [pscustomobject]@{Type=7;Data=($Original.Data.Substring(0,$Original.Data.Length-4)+(ConvertTo-HexString $ClassId)+'0000')}
}
function Get-EffectAssignments($Store,[string]$EndpointId) {
    $fx=(Get-EndpointPath $EndpointId)+'\FxProperties'
    if (-not $Store.Exists($fx)) { throw 'Endpoint effect store is absent.' }
    @($Store.Entries($fx) | Where-Object { $_.Name -match '^\{d04e05a6-594b-4fb6-a80d-01af5eed7d1d\},(1|2|[5-9]|1[0-9]|20)$' })
}
function Get-AvailableEffectPlacement($Store,[string]$EndpointId) {
    $fx=(Get-EndpointPath $EndpointId)+'\FxProperties'
    $entries=@(Get-EffectAssignments $Store $EndpointId)
    if (-not $entries.Count) { return $null }
    foreach ($entry in $entries) {
        if ($entry.Path -ine $fx -or $entry.Name -notin @(($script:EffectPrefix+'13'),($script:EffectPrefix+'14'),($script:EffectPrefix+'15'))) {
            Throw-EffectCompatibility 'This effect arrangement is not supported.'
        }
    }
    $placement=[pscustomobject]@{Kind='CompositeEndpoint';Guards=@(Get-CompositeGuardNames | ForEach-Object {
        [pscustomobject]@{Name=$_;Value=$Store.Read($fx,$_)}
    })}
    try { Assert-CompositePlacement $placement } catch { Throw-EffectCompatibility $_.Exception.Message }
    return $placement
}
function Get-BindingPlacement($Binding) {
    if ($null -ne $Binding -and $null -ne $Binding.PSObject.Properties['Placement']) { return $Binding.Placement }
    return $null
}
function Assert-CompositeAssignments($Store,[string]$EndpointId,$Placement,[string]$AttachedClass='') {
    Assert-CompositePlacement $Placement
    $fx=(Get-EndpointPath $EndpointId)+'\FxProperties'
    foreach ($entry in @(Get-EffectAssignments $Store $EndpointId)) {
        if ($entry.Path -ine $fx -or $entry.Name -notin @(($script:EffectPrefix+'13'),($script:EffectPrefix+'14'),($script:EffectPrefix+'15'))) {
            throw 'Device effects changed since Contour was installed. Existing effects were preserved.'
        }
    }
    foreach ($guard in $Placement.Guards) {
        $expected=$guard.Value
        if ($AttachedClass -and $guard.Name -eq ($script:EffectPrefix+'15')) { $expected=Add-CompositeClass $expected $AttachedClass }
        if (-not (Test-RawEqual ($Store.Read($fx,$guard.Name)) $expected)) {
            throw 'Device effects changed since Contour was installed. Existing effects were preserved.'
        }
    }
}
