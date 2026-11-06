param([Parameter(Mandatory=$true)][string]$BinaryDirectory,
      [Parameter(Mandatory=$true)][string]$WorkDirectory,
      [string]$InfVerif = '', [string]$Inf2Cat = '')
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot '..\tools\deployment\ExtensionPackage.ps1')
function Assert($Condition, [string]$Message) { if (-not $Condition) { throw $Message } }
function Reject([scriptblock]$Action, [string]$Message) {
    $failed=$false
    try { & $Action | Out-Null } catch { $failed=$true }
    Assert $failed $Message
}
function Clone($Value) { $Value | ConvertTo-Json -Depth 16 | ConvertFrom-Json }
$root = Join-Path ([IO.Path]::GetFullPath($WorkDirectory)) ([guid]::NewGuid().ToString('D'))
$componentDirectory = Join-Path $root 'component'
$extensionDirectory = Join-Path $root 'extension'
$definitionFile = Join-Path $PSScriptRoot '..\tools\deployment\drivers\focusrite-usb.json'
$definition = Get-Content -LiteralPath $definitionFile -Raw | ConvertFrom-Json
$stage = Join-Path $PSScriptRoot '..\tools\deployment\stage-extension.ps1'
& (Join-Path $PSScriptRoot '..\tools\deployment\stage-component.ps1') -BinaryDirectory $BinaryDirectory -OutputDirectory $componentDirectory
$component = [IO.File]::ReadAllText((Join-Path $componentDirectory 'contour-apo.inf'))
$identity = Get-ComponentPackageIdentity $component

$adapter = [pscustomobject]@{
    InstanceId='FOCUSRITEUSB\AUDIO&ADAPTER\FIXTURE-A';Name='Fixture output adapter'
    HardwareIds=@('focusriteusb\audio&adapter');CompatibleIds=@('FixtureCompatible')
    ClassId='{4d36e96c-e325-11ce-bfc1-08002be10318}';Present=$true
    DriverInf='oem999.inf';DriverVersion=$definition.ObservedDriver.Version
    Provider=$definition.ObservedDriver.Provider;Service=$definition.ObservedDriver.Service
    Children=@('SWD\MMDEVAPI\fixture-render','SWD\MMDEVAPI\fixture-capture')
}
$old = Clone $adapter
$old.InstanceId='FOCUSRITEUSB\AUDIO&ADAPTER\FIXTURE-B';$old.Present=$false;$old.DriverVersion='4.144.0.362';$old.Children=@()
$inventory = [pscustomobject]@{
    Schema=1;Kind='ContourAdapterInventory'
    EndpointId='{0.0.0.00000000}.{11111111-1111-4111-8111-111111111111}'
    TopologyDeviceId='opaque topology device identity';AdapterId=$adapter.InstanceId
    SearchIds=@('FocusriteUSB\Audio&Adapter');Matches=@($adapter,$old)
}
$report = New-ExtensionScopeReport $definition $inventory
Assert ($report.Matches.Count -eq 2 -and -not $report.Matches[1].Present) 'Disconnected matching adapters must be retained'
Assert ($report.Matches[0].Selected -and -not $report.Matches[1].Selected) 'Selection must identify exactly one adapter'
Assert ($report.Matches[0].ReviewReasons.Count -eq 0 -and $report.Matches[1].ReviewReasons.Count -gt 0) 'Old driver must be flagged, not excluded from PnP scope'
Assert ($report.RequiresDriverReview -and $report.FutureMatchingDevicesIncluded) 'Driver-wide scope must be explicit'
Assert (-not $report.ActivatesEndpoints -and -not $report.ReadyForLiveInstall) 'An offline scope report is not install readiness'

$compatible = Clone $adapter
$compatible.InstanceId='OTHER\DEVICE\FIXTURE-C';$compatible.HardwareIds=@('OTHER\DEVICE');$compatible.CompatibleIds=@('FocusriteUSB\Audio&Adapter')
$compatible.ClassId='{00000000-0000-0000-0000-000000000000}'
$expanded = Clone $inventory
$expanded.Matches+=@($compatible)
$expandedReport=New-ExtensionScopeReport $definition $expanded
Assert ($expandedReport.Matches.Count -eq 3 -and $expandedReport.Matches[2].ReviewReasons.Count -eq 2) 'Compatible-only and non-audio matches must be visible for review'
$other = Clone $adapter
$other.InstanceId='UNRELATED\AUDIO\FIXTURE-D';$other.HardwareIds=@('Unrelated\Audio');$other.CompatibleIds=@()
$expanded.Matches+=@($other)
Assert ((New-ExtensionScopeReport $definition $expanded).Matches.Count -eq 3) 'Unrelated adapters must not enter the extension scope'

$bad=Clone $inventory;$bad.SearchIds=@('USB\VID_1235&PID_8218')
Reject { New-ExtensionScopeReport $definition $bad } 'Physical USB ID must not substitute for the endpoint adapter'
$bad=Clone $inventory;$bad.Matches=@($old)
Reject { New-ExtensionScopeReport $definition $bad } 'Missing selected adapter must not be accepted'
$bad=Clone $inventory;$bad.Matches+=@($adapter)
Reject { New-ExtensionScopeReport $definition $bad } 'Duplicate inventory identities must not be accepted'
foreach ($invalid in @('FocusriteUSB\*','FocusriteUSB\Audio&Adapter\Instance','USB\VID_1235%Bad%',"USB\ID`r`n[Injected]","USB\ID`n")) {
    $badDefinition=Clone $definition;$badDefinition.HardwareIds=@($invalid)
    Reject { New-RegistrationExtensionInf $badDefinition $component } 'INF metacharacters or instance IDs accepted'
}
$badDefinition=Clone $definition;$badDefinition.HardwareIds+=@('focusriteusb\audio&adapter')
Reject { New-RegistrationExtensionInf $badDefinition $component } 'Case-insensitive duplicate hardware IDs accepted'
$badDefinition=Clone $definition;$badDefinition.ExtensionId=[guid]::Empty.ToString('B')
Reject { New-RegistrationExtensionInf $badDefinition $component } 'Empty extension identity accepted'
Reject { New-RegistrationExtensionInf $definition ($component.Replace('Class=AudioProcessingObject','Class=SoftwareComponent')) } 'Mismatched component family accepted'

$inventoryFile=Join-Path $root 'inventory.json'
[IO.File]::WriteAllText($inventoryFile,($inventory | ConvertTo-Json -Depth 12))
& $stage -DefinitionFile $definitionFile -ComponentDirectory $componentDirectory -OutputDirectory $extensionDirectory -InventoryFile $inventoryFile
$infPath=Join-Path $extensionDirectory 'contour-focusrite-usb.inf'
$inf=[IO.File]::ReadAllText($infPath)
Assert ($inf -match '(?m)^Class=Extension\r?$') 'Incorrect extension class'
Assert ($inf.Contains('ComponentIDs='+$identity.ComponentId)) 'Extension child does not match the component package'
Assert ($inf.Contains('DriverVer='+$identity.DriverVer)) 'Extension and component versions differ'
Assert ($inf.Contains('ExtensionId='+$definition.ExtensionId)) 'Extension identity was not stable'
$models=@($inf -split '\r?\n' | Where-Object { $_ -match '^%Description%=Contour_Extension,' })
Assert ($models.Count -eq 1 -and $models[0] -ceq '%Description%=Contour_Extension,FocusriteUSB\Audio&Adapter') 'Extension must match the reviewed adapter hardware ID exactly'
Assert ($inf -notmatch '(?im)^\s*(HKR|HKLM|HKCR|HKCU|AddReg|DelReg|AddInterface|AddService|CopyInf|CopyFiles)\b|MMDevices|DisableProtectedAudioDG') 'Registration-only extension must not rewrite endpoint/OEM settings or replace a driver'
$savedReport=Get-Content -LiteralPath (Join-Path $extensionDirectory 'scope.json') -Raw | ConvertFrom-Json
Assert ($savedReport.Matches.Count -eq 2) 'Staged scope report lost an adapter'
Reject { & $stage -DefinitionFile $definitionFile -ComponentDirectory $componentDirectory -OutputDirectory $extensionDirectory } 'Existing staging directory must not be overwritten'
Assert ([IO.File]::ReadAllText($infPath) -ceq $inf) 'Rejected staging changed the existing INF'

if ($InfVerif) {
    foreach ($mode in @('/w','/h')) {
        & $InfVerif $mode /v $infPath
        Assert ($LASTEXITCODE -eq 0) "InfVerif $mode rejected the extension"
    }
} else { Write-Output 'NOT RUN: WDK InfVerif' }
if ($Inf2Cat) {
    & $Inf2Cat "/driver:$extensionDirectory" /os:10_CO_X64,10_NI_X64,10_GE_X64 /nocat
    Assert ($LASTEXITCODE -eq 0) 'Inf2Cat rejected the extension'
} else { Write-Output 'NOT RUN: WDK Inf2Cat' }
Write-Output 'PASS extension package, identity pairing, exact matching, phantom/compatible matches, review boundaries, and staging preservation'
