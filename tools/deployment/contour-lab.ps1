# This is an explicit, single-endpoint LAB installer. It does not restart audio,
# change security descriptors on registry keys, or disable protected audio.
[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][ValidateSet('Plan','Install','Restore','Status')][string] $Action,
    [Parameter(Mandatory=$true)][string] $File,
    [string] $EndpointId,
    [string] $BuildDirectory = '',
    [switch] $UseExistingUnprotectedAudio
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'Deployment.ps1')
if ([string]::IsNullOrWhiteSpace($BuildDirectory)) {
    $BuildDirectory = Join-Path $PSScriptRoot '..\..\build\windows-x64\bin\Release'
}

function Assert-Administrator {
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = [Security.Principal.WindowsPrincipal]::new($identity)
    if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
        throw 'Install/Restore must run from an elevated Windows PowerShell. Plan/Status do not require elevation.'
    }
}
function Read-ContourInfo([string] $Directory, [string] $Dll, [string] $Endpoint) {
    $output = & (Join-Path $Directory 'contour-deploy-info.exe') $Dll $Endpoint
    if ($LASTEXITCODE -ne 0) { throw 'Deployment inspection failed; no plan can be used.' }
    $output | ConvertFrom-Json
}
function Assert-LabEnvironment($Store) {
    if (-not $UseExistingUnprotectedAudio) {
        throw 'This unsigned lab experiment requires -UseExistingUnprotectedAudio and an already-enabled override. It never enables the override itself.'
    }
    $expected = [pscustomobject]@{Type=4;Data='01000000'}
    if (-not (Test-RawEqual ($Store.Read($script:AudioSettings,'DisableProtectedAudioDG')) $expected)) {
        throw 'DisableProtectedAudioDG is not already 1. Resolve signed/protected deployment before installation.'
    }
}
function New-ProtectedPackage([string] $Path) {
    if (Test-Path -LiteralPath $Path) { throw 'Package destination already exists; inspect its journal first.' }
    $acl = [Security.AccessControl.DirectorySecurity]::new()
    $acl.SetAccessRuleProtection($true, $false)
    foreach ($spec in @(@('S-1-5-18','FullControl'), @('S-1-5-32-544','FullControl'), @('S-1-5-32-545','ReadAndExecute'))) {
        $sid = [Security.Principal.SecurityIdentifier]::new($spec[0])
        $rule = [Security.AccessControl.FileSystemAccessRule]::new($sid, $spec[1],
            'ContainerInherit, ObjectInherit', 'None', 'Allow')
        $acl.AddAccessRule($rule)
    }
    $acl.SetOwner([Security.Principal.SecurityIdentifier]::new('S-1-5-32-544'))
    [void][IO.Directory]::CreateDirectory($Path, $acl)
}

$store = $null
$mutex = $null
$ownsMutex = $false
try {
    if (-not [Environment]::Is64BitProcess) { throw 'Use 64-bit Windows PowerShell.' }
    $write = $Action -in @('Install','Restore')
    if ($write) {
        Assert-Administrator
        $mutex = [Threading.Mutex]::new($false, 'Global\ContourLabDeployment')
        try { $ownsMutex = $mutex.WaitOne(0) } catch [Threading.AbandonedMutexException] { $ownsMutex = $true }
        if (-not $ownsMutex) { throw 'Another Contour deployment is active.' }
    }
    $root = [Microsoft.Win32.RegistryKey]::OpenBaseKey([Microsoft.Win32.RegistryHive]::LocalMachine,
        [Microsoft.Win32.RegistryView]::Registry64)
    $store = [Contour.Deployment.RegistryStore]::new($root, $write)
    $File = [IO.Path]::GetFullPath($File)

    if ($Action -eq 'Plan') {
        if ([string]::IsNullOrWhiteSpace($EndpointId)) { throw 'Plan requires -EndpointId.' }
        $directory = [IO.Path]::GetFullPath($BuildDirectory)
        $dll = Join-Path $directory 'contour-apo.dll'
        $hash = (Get-FileHash -LiteralPath $dll -Algorithm SHA256).Hash
        $info = Read-ContourInfo $directory $dll $EndpointId
        $plan = New-ContourPlan $store $info $dll $hash ([guid]::NewGuid().ToString('D'))
        if ((Get-FileHash -LiteralPath $dll -Algorithm SHA256).Hash -cne $hash) { throw 'DLL changed during inspection.' }
        [Contour.Deployment.DurableFile]::Save($File, ($plan | ConvertTo-Json -Depth 30), $true)
        Write-Output "Plan saved: $File"
        Write-Output "Endpoint: $($info.Endpoint.Name) [$EndpointId]"
        Write-Output "DLL SHA-256: $hash"
        Write-Output "Target: $($plan.TargetDll)"
        Write-Output "Changes: $($plan.Changes.Count) new registry values; $($plan.CreateKeys.Count) new Contour keys. Existing endpoint values are preserved."
        Write-Output 'Placement: LFX pass-through + GFX -6 dB. No live attachment was made.'
        Write-Output "Current process can write endpoint effects: $($store.CanWrite(((Get-EndpointPath $EndpointId)+'\FxProperties')))"
        exit 0
    }

    $document = Get-Content -LiteralPath $File -Raw -Encoding UTF8 | ConvertFrom-Json
    if ($Action -eq 'Status') {
        if ($document.PSObject.Properties.Name -contains 'Plan') {
            Assert-ContourPlan $document.Plan
            Write-Output "Journal state: $($document.State)"
            foreach ($conflict in $document.Conflicts) { Write-Output "Conflict: $conflict" }
            $plan = $document.Plan
        } else { $plan = $document; Assert-ContourPlan $plan }
        foreach ($change in $plan.Changes) {
            $current = $store.Read($change.Path,$change.Name)
            $state = if (Test-RawEqual $current $change.After) { 'Contour value present' }
                elseif (Test-RawEqual $current $change.Before) { 'Baseline' } else { 'CONFLICT' }
            Write-Output "$state : $($change.Path) [$($change.Name)]"
        }
        exit 0
    }

    if ($Action -eq 'Restore') {
        $journal = $document
        Assert-ContourPlan $journal.Plan
        if ($File -ine (Join-Path $journal.Plan.Package 'journal.json')) { throw 'Use the protected package journal for restoration.' }
        $restored = Invoke-ContourRestore $store $journal $File
        Write-Output "Journal state: $($journal.State)"
        if (-not $restored) { $journal.Conflicts | Write-Output; exit 1 }
        Write-Output 'Contour registry changes were removed. Rebuild the audio graph before judging live output.'
        Write-Output 'The package and recovery journal are retained; no loaded DLL is deleted.'
        exit 0
    }

    $plan = $document
    Assert-ContourBaseline $store $plan
    Assert-LabEnvironment $store
    $fx = (Get-EndpointPath $plan.Endpoint.Id)+'\FxProperties'
    if (-not $store.CanWrite($fx)) { throw 'Endpoint effects are not writable even in this process. No ownership or ACL change will be attempted.' }
    if ((Get-FileHash -LiteralPath $plan.SourceDll -Algorithm SHA256).Hash -cne $plan.DllSha256) { throw 'DLL changed after planning; create a new plan.' }
    $sourceDirectory = Split-Path -Parent $plan.SourceDll
    $freshInfo = Read-ContourInfo $sourceDirectory $plan.SourceDll $plan.Endpoint.Id
    $freshPlan = New-ContourPlan $store $freshInfo $plan.SourceDll $plan.DllSha256 $plan.Id
    for ($i=0; $i -lt $plan.Changes.Count; ++$i) {
        $old = $plan.Changes[$i]; $fresh = $freshPlan.Changes[$i]
        if ($old.Path -cne $fresh.Path -or $old.Name -cne $fresh.Name -or -not (Test-RawEqual $old.After $fresh.After)) {
            throw 'Planned registration does not match the inspected DLL.'
        }
    }
    New-ProtectedPackage $plan.Package
    Copy-Item -LiteralPath $plan.SourceDll -Destination $plan.TargetDll
    foreach ($name in @('contour-apo-host.exe','contour-deploy-info.exe')) {
        Copy-Item -LiteralPath (Join-Path $sourceDirectory $name) -Destination (Join-Path $plan.Package $name)
    }
    foreach ($name in @('contour-lab.ps1','Deployment.ps1','RegistryStore.cs')) {
        Copy-Item -LiteralPath (Join-Path $PSScriptRoot $name) -Destination (Join-Path $plan.Package $name)
    }
    if ((Get-FileHash -LiteralPath $plan.TargetDll -Algorithm SHA256).Hash -cne $plan.DllSha256) { throw 'Staged DLL hash mismatch.' }
    & (Join-Path $plan.Package 'contour-apo-host.exe') $plan.TargetDll
    if ($LASTEXITCODE -ne 0) { throw 'Staged APO failed its standalone tests; registration was not changed.' }
    $journal = New-ContourJournal $plan
    $journalPath = Join-Path $plan.Package 'journal.json'
    Save-ContourJournal $journal $journalPath -CreateNew
    try {
        Invoke-ContourApply $store $journal $journalPath
    } catch {
        $journal.LastError = $_.Exception.Message
        [void](Invoke-ContourRestore $store $journal $journalPath)
        throw "Installation failed: $($journal.LastError). Recovery state: $($journal.State). Journal: $journalPath"
    }
    Write-Output "Attached; live processing has NOT been verified. Journal: $journalPath"
    Write-Output 'Rebuild the audio graph and measure output. This tool did not restart Windows Audio.'
} catch {
    Write-Error $_ -ErrorAction Continue
    exit 1
} finally {
    if ($null -ne $store) { $store.Dispose() }
    if ($ownsMutex) { $mutex.ReleaseMutex() }
    if ($null -ne $mutex) { $mutex.Dispose() }
}
