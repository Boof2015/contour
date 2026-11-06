Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Assert-ExtensionDefinition($Definition) {
    if ($Definition.Schema -ne 1 -or $Definition.Kind -cne 'ContourRegistrationExtension') { throw 'Unknown extension definition.' }
    if ($Definition.Name -cnotmatch '^[a-z][a-z0-9-]{0,47}\z') { throw 'Invalid extension name.' }
    if ($Definition.Description -cnotmatch '^[A-Za-z0-9 ()-]{1,100}\z') { throw 'Invalid extension description.' }
    $id = [guid]::Empty
    if (-not [guid]::TryParseExact($Definition.ExtensionId,'B',[ref]$id) -or $id -eq [guid]::Empty) { throw 'Invalid extension identity.' }
    $ids = @($Definition.HardwareIds)
    if ($ids.Count -lt 1 -or $ids.Count -gt 16) { throw 'Extension requires a bounded hardware-ID list.' }
    foreach ($hardware in $ids) {
        # Exact PnP IDs, never wildcards, instance IDs, INF substitutions or lines.
        if ($hardware -isnot [string] -or $hardware -cnotmatch '^[A-Za-z0-9_&.+-]+\\[A-Za-z0-9_&.+-]{1,180}\z') {
            throw 'Invalid extension hardware ID.'
        }
    }
    if (@($ids | Sort-Object -Unique).Count -ne $ids.Count) { throw 'Duplicate extension hardware ID.' }
    foreach ($name in @('Provider','Service','Version')) {
        if ($Definition.ObservedDriver.$name -isnot [string] -or [string]::IsNullOrWhiteSpace($Definition.ObservedDriver.$name)) {
            throw 'Extension definition needs the observed driver baseline.'
        }
    }
}

function Get-ComponentPackageIdentity([string]$Inf) {
    # Obtain identity and version from the paired generated component package;
    # don't duplicate its private ComponentID in a second source file.
    $match = [regex]::Matches($Inf, '(?m)^%Description%=Contour_Install,SWC\\([A-Z0-9_&]+)\r?$')
    $version = [regex]::Matches($Inf, '(?m)^DriverVer=([0-9/,\.]+)\r?$')
    if ($match.Count -ne 1 -or $version.Count -ne 1 -or
        $Inf -notmatch '(?m)^Class=AudioProcessingObject\r?$' -or
        $Inf -notmatch '(?m)^\[Contour.NTamd64.10.0...22000\]\r?$') {
        throw 'Unsupported component INF; generate it with contour-deploy-info.'
    }
    [pscustomobject]@{ComponentId=$match[0].Groups[1].Value;DriverVer=$version[0].Groups[1].Value}
}

function New-RegistrationExtensionInf($Definition, [string]$ComponentInf) {
    Assert-ExtensionDefinition $Definition
    $component = Get-ComponentPackageIdentity $ComponentInf
    $models = ($Definition.HardwareIds | ForEach-Object { '%Description%=Contour_Extension,'+$_ }) -join "`r`n"
    # This child makes the APO discoverable under the adapter. It does not
    # activate processing, install a function driver, or alter OEM FX settings.
    @"
; Contour registration-only extension. Unsigned development staging artifact.
; PnP matching is driver-wide, including future instances with these IDs.
; Endpoint activation and saved EQ remain separately owned by Contour setup.
[Version]
Signature="`$WINDOWS NT`$"
Class=Extension
ClassGuid={e2f84ce7-8efa-411c-aa69-97454ca4cb57}
Provider=%Provider%
ExtensionId=$($Definition.ExtensionId)
DriverVer=$($component.DriverVer)
CatalogFile=contour-$($Definition.Name).cat
PnpLockDown=1

[Manufacturer]
%Provider%=Contour,NTamd64.10.0...22000

[Contour.NTamd64.10.0...22000]
$models

[Contour_Extension.NT]

[Contour_Extension.NT.Components]
AddComponent=ContourApo,,Contour_Component

[Contour_Component]
ComponentIDs=$($component.ComponentId)
Description=%ComponentDescription%

[Strings]
Provider="Contour"
Description="$($Definition.Description)"
ComponentDescription="Contour Audio Processing"
"@
}

function New-ExtensionScopeReport($Definition, $Inventory) {
    Assert-ExtensionDefinition $Definition
    if ($Inventory.Schema -ne 1 -or $Inventory.Kind -cne 'ContourAdapterInventory') { throw 'Unknown adapter inventory.' }
    if ([string]::IsNullOrWhiteSpace($Inventory.EndpointId) -or
        [string]::IsNullOrWhiteSpace($Inventory.TopologyDeviceId) -or [string]::IsNullOrWhiteSpace($Inventory.AdapterId)) {
        throw 'Invalid endpoint inventory identity.'
    }
    foreach ($id in $Definition.HardwareIds) {
        if (@($Inventory.SearchIds) -notcontains $id) { throw 'Inventory does not cover every extension match ID.' }
    }
    $matches = @()
    $seen = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    foreach ($device in $Inventory.Matches) {
        if ([string]::IsNullOrWhiteSpace($device.InstanceId) -or -not $seen.Add($device.InstanceId) -or $device.Present -isnot [bool]) {
            throw 'Invalid or duplicate adapter inventory entry.'
        }
        $hardware = @($Definition.HardwareIds | Where-Object { @($device.HardwareIds) -contains $_ })
        $compatible = @($Definition.HardwareIds | Where-Object { @($device.CompatibleIds) -contains $_ })
        if ($hardware.Count -eq 0 -and $compatible.Count -eq 0) { continue }
        $review = @()
        if ($device.ClassId -ine '{4d36e96c-e325-11ce-bfc1-08002be10318}') { $review += 'Matched device is not a MEDIA adapter.' }
        if ($hardware.Count -eq 0) { $review += 'Match is through a compatible ID only.' }
        if ($device.Provider -ine $Definition.ObservedDriver.Provider -or $device.Service -ine $Definition.ObservedDriver.Service) {
            $review += 'Driver provider or service differs from the observed driver.'
        }
        if ($device.DriverVersion -ine $Definition.ObservedDriver.Version) { $review += 'Driver version differs from the observed driver.' }
        $matches += [pscustomobject]@{
            InstanceId=$device.InstanceId;Name=$device.Name;Present=$device.Present
            Selected=($device.InstanceId -ieq $Inventory.AdapterId)
            HardwareMatches=$hardware;CompatibleMatches=$compatible
            DriverInf=$device.DriverInf;DriverVersion=$device.DriverVersion
            ClassId=$device.ClassId;Provider=$device.Provider;Service=$device.Service
            Children=@($device.Children);ReviewReasons=$review
        }
    }
    $selected = @($matches | Where-Object { $_.Selected })
    if ($selected.Count -ne 1) { throw 'Selected endpoint adapter is missing from the extension match set.' }
    [pscustomobject]@{
        Schema=1;Kind='ContourExtensionScope';ExtensionId=$Definition.ExtensionId
        EndpointId=$Inventory.EndpointId;AdapterId=$Inventory.AdapterId;HardwareIds=@($Definition.HardwareIds)
        Matches=$matches
        RequiresDriverReview=(@($matches | Where-Object { $_.ReviewReasons.Count -gt 0 }).Count -gt 0)
        ActivatesEndpoints=$false
        FutureMatchingDevicesIncluded=$true
        ReadyForLiveInstall=$false
        Pending=@('Package signing','Endpoint migration and recovery','Live component discovery and removal verification')
    }
}
