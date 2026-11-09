# Installer-owned entry point. No caller-supplied paths or registry addresses.
# Inspect is read-only; Remove requires elevation and a closed editor.
[CmdletBinding()]
param([ValidateSet('Inspect','Remove')][string]$Action='Inspect',[switch]$PlainText)
Set-StrictMode -Version Latest
$ErrorActionPreference='Stop'
$ProgressPreference='SilentlyContinue'
[Console]::OutputEncoding=[Text.UTF8Encoding]::new($false)
. (Join-Path $PSScriptRoot 'SetupHost.ps1')

function Remove-ProductPackageFiles($Store,$Package) {
    $current=Read-SetupJson (Join-Path $Package.Path 'device-setup.json')
    Assert-SetupState $current $Package.Path
    if ($current.Schema -ne 4 -or $current.State -ne 'Removed' -or $current.AudioRestartPending) {
        throw 'Engine removal is incomplete. Its recovery files were kept.'
    }
    Assert-ProductRemovalReferences $Store @() $Package.Path
    foreach ($key in @(Get-ContourClassKeys)) {
        if ($Store.Exists($key)) { throw 'Contour registration still exists. Its engine files were kept.' }
    }
    # Resolve and verify the exact protected GUID directory before recursive
    # deletion. Reject links anywhere in the tree, including on retry.
    $expected=Join-Path ([Environment]::GetFolderPath('ProgramFiles')) ('ContourLab-'+([guid]::Parse($current.Id)).ToString('D'))
    $resolved=(Get-Item -LiteralPath $Package.Path -Force).FullName
    if ($resolved -cne $expected) { throw 'Engine cleanup path is outside its protected package.' }
    Assert-NoLink ([Environment]::GetFolderPath('ProgramFiles'))
    Assert-NoLink $resolved;Assert-ProtectedSetup $resolved;Assert-PackageTree $resolved
    # Keep the authority record until all other files are gone, so an interrupted
    # file cleanup is still discoverable and safe to retry.
    foreach ($item in Get-ChildItem -LiteralPath $resolved -Force) {
        if ($item.Name -ceq 'device-setup.json') { continue }
        Remove-Item -LiteralPath $item.FullName -Recurse -Force
    }
    Remove-Item -LiteralPath (Join-Path $resolved 'device-setup.json') -Force
    [IO.Directory]::Delete($resolved,$false)
}
$store=$null;$mutex=$null;$owns=$false
try {
    if (-not [Environment]::Is64BitProcess) { throw 'Contour removal requires 64-bit Windows.' }
    if ($Action -eq 'Remove') {
        $principal=[Security.Principal.WindowsPrincipal]::new([Security.Principal.WindowsIdentity]::GetCurrent())
        if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) { throw 'Windows administrator approval is required.' }
        $mutex=[Threading.Mutex]::new($false,'Global\ContourLabDeployment')
        try { $owns=$mutex.WaitOne(0) } catch [Threading.AbandonedMutexException] { $owns=$true }
        if (-not $owns) { throw 'Another Contour installation or removal is running.' }
        if (@(Get-Process -Name contour -ErrorAction SilentlyContinue).Count) { throw 'Quit Contour from its tray menu, then retry uninstallation.' }
    }
    $root=[Microsoft.Win32.RegistryKey]::OpenBaseKey([Microsoft.Win32.RegistryHive]::LocalMachine,[Microsoft.Win32.RegistryView]::Registry64)
    $store=[Contour.Deployment.RegistryStore]::new($root,($Action -eq 'Remove'))
    $packages=@(Get-SetupPackages -IncludeRemoved)
    if ($packages.Count -gt 1) { throw 'Conflicting Contour installations require recovery before uninstalling.' }
    if (-not $packages.Count) {
        foreach ($key in @(Get-ContourClassKeys)) {
            if ($store.Exists($key)) { throw 'Contour engine registration has no matching recovery record. Its files and settings were kept.' }
        }
        Assert-ProductRemovalReferences $store @() (Join-Path ([Environment]::GetFolderPath('ProgramFiles')) 'ContourLab-unowned')
    }
    foreach ($package in $packages) {
        if ($package.State.Schema -eq 2) { throw 'Finish the installation migration before uninstalling Contour.' }
        if ($package.State.Schema -notin @(1,3,4)) { throw 'This installation needs a compatible Contour uninstaller.' }
    }
    if ($Action -eq 'Inspect') {
        $names=@(foreach ($package in $packages) {
            $owner=if ($package.State.Schema -eq 4) { $package.State.Original } else { $package.State }
            if ($owner.Schema -eq 3) { $owner.Contexts | ForEach-Object Name } else { $owner.Name }
        })
        @{ready=$true;hasEngine=($packages.Count -gt 0);devices=$names;keepsProfiles=$true;preservesAudioCompatibility=$true} | ConvertTo-Json -Compress
    } else {
        foreach ($package in $packages) {
            if ($package.State.Schema -eq 3) {
                $catalog=Read-DeploymentState (Join-Path $package.Path 'endpoints.json')
                if ($catalog.Schema -eq 4 -and $null -ne $catalog.Runtime.Update) {
                    $callbacks=New-RuntimeCallbacks $package.Path
                    Invoke-RuntimeUpdate $store $package.Path $null @callbacks
                }
                $catalog=Read-DeploymentState (Join-Path $package.Path 'endpoints.json')
                if ($null -ne $catalog.Operation) {
                    $callbacks=New-SharedCallbacks $store $package.Path
                    Invoke-SharedDeviceSetup $store $package.Path Remove $catalog.Operation.EndpointId '' '' @callbacks
                }
            }
            if ($package.State.Schema -ne 4 -or $package.State.State -ne 'Removed') {
                $capture={ if ((Get-Service Audiosrv).Status -eq 'Running') { Capture-SetupServices } }
                Invoke-ProductRemoval $store $package.Path -CaptureServices $capture -StopAudio ${function:Stop-SetupAudio} -StartAudio ${function:Start-SetupAudio}
            }
            Remove-ProductPackageFiles $store $package
        }
        # Clean matching startup entries in loaded user profiles. Never load or
        # rewrite an offline user's hive, or remove a different Contour copy.
        $executable=Join-Path (Split-Path $PSScriptRoot -Parent) 'contour.exe'
        $users=[Microsoft.Win32.RegistryKey]::OpenBaseKey([Microsoft.Win32.RegistryHive]::Users,[Microsoft.Win32.RegistryView]::Registry64)
        try {
            foreach ($sid in $users.GetSubKeyNames()) {
                if ($sid -notmatch '^S-1-5-21-(\d+-){3}\d+$' -and $sid -notmatch '^S-1-12-1-(\d+-){3}\d+$') { continue }
                $startupStore=[Contour.Deployment.RegistryStore]::new($users.OpenSubKey($sid,$false),$true)
                try { Remove-ProductStartupEntry $startupStore $executable } finally { $startupStore.Dispose() }
            }
        } finally { $users.Dispose() }
        if ($PlainText) { Write-Output 'Contour audio processing was removed. Your profiles were kept.' }
        else { @{removed=$true;keptProfiles=$true;preservedAudioCompatibility=$true} | ConvertTo-Json -Compress }
    }
} catch {
    if ($PlainText) { Write-Output $_.Exception.Message }
    else { @{ready=$false;removed=$false;error=$_.Exception.Message} | ConvertTo-Json -Compress }
    exit 1
} finally {
    if ($null -ne $store) { $store.Dispose() }
    if ($owns) { $mutex.ReleaseMutex() };if ($null -ne $mutex) { $mutex.Dispose() }
}
