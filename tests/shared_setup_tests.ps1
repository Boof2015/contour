param([string]$BinaryDirectory,[string]$WorkDirectory,[string]$Worker,[string]$Mode,[string]$CrashAt)
Set-StrictMode -Version Latest
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot '..\tools\deployment\SharedSetup.ps1')
function Assert($Condition,[string]$Message) { if (-not $Condition) { throw $Message } }
function Fails([scriptblock]$Run) { $failed=$false;try { $null=& $Run } catch { $failed=$true };Assert $failed 'Expected rejection.' }
function Callbacks($Store,[string]$Package) {
    $audio=Join-Path $Package 'audio.txt'
    @{CaptureServices={ 'Audiosrv' };
        StopAudio={param($Names) Assert (($Names -join ',') -eq 'Audiosrv') 'Lost original service set';[IO.File]::WriteAllText($audio,'stopped')}.GetNewClosure();
        StartAudio={param($Names) Assert (($Names -join ',') -eq 'Audiosrv') 'Lost original service set';[IO.File]::WriteAllText($audio,'running')}.GetNewClosure();
        VerifyRegistration={param($Endpoint,$Registration,$Binding) Assert-SharedRegistration $Store $Package $Registration;$true}.GetNewClosure();
        VerifyApplied={param($Endpoint,$Registration,$Binding) $true}}
}
function UpdateBuild { [pscustomobject]@{Id='33333333-3333-4333-8333-333333333333';Sha256=('B'*64)} }
function UpdateCallbacks($Store,[string]$Package) {
    $base=Callbacks $Store $Package
    $plan=(Read-SetupJson (Join-Path $Package 'journal.json')).Plan
    @{CaptureServices=$base.CaptureServices;StopAudio=$base.StopAudio;StartAudio=$base.StartAudio;
        VerifyBuild={param($Build) $true};
        VerifyOutput={param($Endpoint,$Build)
            Assert-RuntimeRegistration $Store $plan $Build
            [pscustomobject]@{Verified=$true;Inactive=$false}
        }.GetNewClosure()}
}
if ($Worker) {
    # TerminateProcess models a crash without running CLR shutdown/finalizers.
    # Environment.Exit can time out while registry handles are still live.
    Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class ContourSharedSetupCrash {
    [DllImport("kernel32.dll")] static extern bool TerminateProcess(IntPtr process, uint code);
    public static void Stop() { TerminateProcess(new IntPtr(-1), 91); }
}
'@
    $f=Read-SetupJson $Worker
    if ($f.Registry -notmatch '^Software\\ContourSharedSetupTest-[0-9a-f-]{36}\\[0-9a-f-]{36}\z') { throw 'Worker requires an isolated HKCU fixture.' }
    $user=[Microsoft.Win32.RegistryKey]::OpenBaseKey([Microsoft.Win32.RegistryHive]::CurrentUser,[Microsoft.Win32.RegistryView]::Registry64)
    $key=$user.OpenSubKey($f.Registry,$true);$store=[Contour.Deployment.RegistryStore]::new($key,$true)
    $callbacks=Callbacks $store $f.Package
    $checkpoint={param($Point) if ($Point -eq $CrashAt) { [ContourSharedSetupCrash]::Stop() }}
    if ($Mode -in @('Runtime','RuntimeRollback')) {
        $updates=UpdateCallbacks $store $f.Package
        if ($Mode -eq 'RuntimeRollback') { $updates.VerifyOutput={param($Endpoint,$Build) [pscustomobject]@{Verified=($Build.Id -eq 'original');Inactive=$false}} }
        Invoke-RuntimeUpdate $store $f.Package (UpdateBuild) @updates -Checkpoint $checkpoint
    }
    elseif ($Mode -eq 'Convert') { ConvertTo-SharedSetup $store $f.Package 'USB\A\1' $checkpoint }
    elseif ($Mode -eq 'Policy') { Enable-UnsignedAudio $store (Join-Path $f.Package 'policy-crash.json') $checkpoint }
    else { Invoke-SharedDeviceSetup $store $f.Package Install $f.B 'Fixture B' 'USB\B\2' @callbacks -Checkpoint $checkpoint }
    throw 'Crash checkpoint was not reached.'
}
$testId=[guid]::NewGuid().ToString('D');$testPath='Software\ContourSharedSetupTest-'+$testId
$user=[Microsoft.Win32.RegistryKey]::OpenBaseKey([Microsoft.Win32.RegistryHive]::CurrentUser,[Microsoft.Win32.RegistryView]::Registry64)
$root=$null;$stores=[Collections.Generic.List[object]]::new();$passed=0
$work=Join-Path ([IO.Path]::GetFullPath($WorkDirectory)) $testId;[void][IO.Directory]::CreateDirectory($work)
try {
    Add-Type -TypeDefinition @'
using System;
using System.IO;
using System.Threading;
public static class ContourJournalReader {
    public static Thread ReadBriefly(string path) {
        var stream = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.Read);
        var thread = new Thread(() => { Thread.Sleep(150); stream.Dispose(); });
        thread.Start(); return thread;
    }
}
'@
    $journalPath=Join-Path $work 'locked-journal.json'
    [Contour.Deployment.DurableFile]::Save($journalPath,'{"version":1}',$true)
    $reader=[ContourJournalReader]::ReadBriefly($journalPath)
    try { [Contour.Deployment.DurableFile]::Save($journalPath,'{"version":2}',$false) } finally { $reader.Join() }
    Assert ([IO.File]::ReadAllText($journalPath) -ceq '{"version":2}') 'Transient reader prevented durable replacement.'
    $locked=[IO.File]::Open($journalPath,[IO.FileMode]::Open,[IO.FileAccess]::Read,[IO.FileShare]::Read)
    try { Fails { [Contour.Deployment.DurableFile]::Save($journalPath,'{"version":3}',$false) } } finally { $locked.Dispose() }
    Assert ([IO.File]::ReadAllText($journalPath) -ceq '{"version":2}') 'A persistent lock destroyed the last durable record.'
    Fails { [Contour.Deployment.DurableFile]::Save($journalPath,'{"version":4}',$true) }
    Assert ([IO.File]::ReadAllText($journalPath) -ceq '{"version":2}') 'CreateNew overwrote another owner.'
    ++$passed;Write-Output 'PASS journal replacement waits for a brief reader; persistent locks and existing owners preserved'
    $raw=& (Join-Path $BinaryDirectory 'contour-deploy-info.exe') (Join-Path $BinaryDirectory 'contour-apo.dll') --registration-only
    Assert ($LASTEXITCODE -eq 0) 'Cannot inspect fixture DLL';$info=$raw | ConvertFrom-Json
    $root=$user.CreateSubKey($testPath)
    function Set-VendorEffects($Store,[string]$Endpoint) {
        $fx=(Get-EndpointPath $Endpoint)+'\FxProperties'
        foreach ($id in @(13,14,15)) {
            $list=if ($id -eq 15) { '{aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa}'+[char]0+'{bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb}' }
                else { '{cccccccc-cccc-4ccc-8ccc-cccccccccccc}' }
            $Store.Write($fx,$script:EffectPrefix+$id,7,((ConvertTo-HexString $list)+'0000'))
        }
        foreach ($id in @(5,6,7)) {
            $Store.Write($fx,'{d3993a3f-99c2-4402-b5ec-a92a0367664b},'+$id,7,((ConvertTo-HexString '{C18E2F7E-933D-4965-B7D1-1EEF228D2AF3}')+'0000'))
        }
        $Store.Create($fx+'\VendorContext');$Store.Write($fx+'\VendorContext','Keep',3,'CAFE')
    }
    function VendorState($Store,[string]$Endpoint) {
        $fx=(Get-EndpointPath $Endpoint)+'\FxProperties'
        [pscustomobject]@{Values=@(Get-CompositeGuardNames | ForEach-Object { $Store.Read($fx,$_) });Context=$Store.Snapshot($fx+'\VendorContext');Security=$Store.Security($fx)} | ConvertTo-Json -Depth 8 -Compress
    }
    function New-Fixture($Before=$null,[switch]$Composite) {
        $id=[guid]::NewGuid().ToString('D');$registry=$root.CreateSubKey($id)
        $package=Join-Path $work $id;[void][IO.Directory]::CreateDirectory($package)
        $a='{0.0.0.00000000}.{11111111-1111-4111-8111-111111111111}'
        $b='{0.0.0.00000000}.{22222222-2222-4222-8222-222222222222}'
        foreach ($path in @('SOFTWARE\Classes\CLSID','SOFTWARE\Classes\AudioEngine\AudioProcessingObjects',$script:AudioSettings,
            ((Get-EndpointPath $a)+'\FxProperties'),((Get-EndpointPath $a)+'\Properties'),
            ((Get-EndpointPath $b)+'\FxProperties'),((Get-EndpointPath $b)+'\Properties'))) {
            $key=$registry.CreateSubKey($path);$key.Dispose()
        }
        $store=[Contour.Deployment.RegistryStore]::new($registry,$true);$stores.Add($store)
        if ($null -ne $Before) { $store.Write($script:AudioSettings,'DisableProtectedAudioDG',4,$Before) }
        $vendorBefore=$null
        if ($Composite) { Set-VendorEffects $store $a;$vendorBefore=VendorState $store $a }
        $info.Endpoint=[pscustomobject]@{Id=$a;Name='Fixture A';State=1;Channels=2;SampleRate=48000}
        $plan=New-ContourPlan $store $info (Join-Path $BinaryDirectory 'contour-apo.dll') ('A'*64) $id
        Save-ContourJournal (New-ContourJournal $plan) (Join-Path $package 'journal.json') -CreateNew
        $state=New-SetupState $plan;Save-SetupState $state $package
        $callbacks=Callbacks $store $package
        Invoke-DeviceSetup $store $state $package Install $callbacks.CaptureServices $callbacks.StopAudio $callbacks.StartAudio
        $store.Write((Get-ControlContextKeys $a)[2],$script:ControlContextId+',4',3,'AABBCC')
        $f=[pscustomobject]@{Store=$store;Package=$package;A=$a;B=$b;Id=$id;Original=$state;
            SavedA=Get-EndpointSettings $store $a;Globals=$store.Snapshot('SOFTWARE\Classes');Registry=($testPath+'\'+$id);VendorBefore=$vendorBefore}
        Save-ContourJournal ([pscustomobject]@{Registry=$f.Registry;Package=$package;A=$a;B=$b}) (Join-Path $package 'worker.json') -CreateNew
        return $f
    }
    function Unchanged($f) {
        Assert ($f.Store.Snapshot('SOFTWARE\Classes') -ceq $f.Globals) 'Shared registration was changed.'
        $now=Get-EndpointSettings $f.Store $f.A
        foreach ($part in @('Default','User','Security')) { Assert ($now.$part -ceq $f.SavedA.$part) "Original EQ or access changed: $part" }
    }
    function Crash($f,[string]$Mode,[string]$Point) {
        & powershell.exe -NoProfile -ExecutionPolicy RemoteSigned -File $PSCommandPath -Worker (Join-Path $f.Package 'worker.json') -Mode $Mode -CrashAt $Point
        Assert ($LASTEXITCODE -eq 91) "Worker failed to reach $Mode/$Point (exit $LASTEXITCODE)"
    }
    foreach ($before in @($null,'00000000','01000000')) {
        $f=New-Fixture $before;$callbacks=Callbacks $f.Store $f.Package
        $policy=Read-SetupJson (Join-Path $f.Package 'audio-policy.json')
        Assert ($policy.AlreadyEnabled -eq ($before -eq '01000000')) 'Lost original enabled state.'
        if ($null -eq $before) { Assert ($null -eq $policy.Before) 'Absent value was recorded as an existing zero.' }
        else { Assert ($policy.Before.Data -ceq $before) 'Original registry value was overwritten in the journal.' }
        Invoke-DeviceSetup $f.Store $f.Original $f.Package Remove $callbacks.CaptureServices $callbacks.StopAudio $callbacks.StartAudio
        Assert (Test-RawEqual ($f.Store.Read($script:AudioSettings,'DisableProtectedAudioDG')) (Get-UnsignedAudioValue)) 'Uninstall disabled other APOs.'
        Assert ((Read-SetupJson (Join-Path $f.Package 'audio-policy.json')).State -eq 'Retained') 'Removal policy was not recorded.'
        ++$passed;Write-Output "PASS standalone setup/removal preserves compatibility (original: $before)"
    }
    $f=New-Fixture
    $f.Store.Write($script:AudioSettings,'DisableProtectedAudioDG',4,'00000000')
    Preserve-UnsignedAudio (Join-Path $f.Package 'audio-policy.json')
    Assert ($f.Store.Read($script:AudioSettings,'DisableProtectedAudioDG').Data -eq '00000000') 'Removal overwrote a later external change.'
    $f.Store.Write($script:AudioSettings,'DisableProtectedAudioDG',1,(ConvertTo-HexString '1'))
    Fails { Enable-UnsignedAudio $f.Store (Join-Path $f.Package 'audio-policy.json') }
    Assert ($f.Store.Read($script:AudioSettings,'DisableProtectedAudioDG').Type -eq 1) 'Unsupported policy was overwritten.'
    ++$passed;Write-Output 'PASS external changes and unknown policy types preserved'

    foreach ($point in @('policy-recorded','policy-intent','policy-written')) {
        $f=New-Fixture;$f.Store.DeleteValue($script:AudioSettings,'DisableProtectedAudioDG')
        Crash $f Policy $point
        Enable-UnsignedAudio $f.Store (Join-Path $f.Package 'policy-crash.json')
        $policy=Read-SetupJson (Join-Path $f.Package 'policy-crash.json')
        Assert ($null -eq $policy.Before -and -not $policy.AlreadyEnabled) 'Policy recovery invented a pre-existing owner.'
        Preserve-UnsignedAudio (Join-Path $f.Package 'policy-crash.json')
        Assert (Test-RawEqual ($f.Store.Read($script:AudioSettings,'DisableProtectedAudioDG')) (Get-UnsignedAudioValue)) 'Policy recovery disabled compatibility.'
        ++$passed;Write-Output "PASS real process exit during compatibility setup at $point"
    }
    foreach ($point in @('shared-catalog-ready','shared-context-ready','shared-committed')) {
        $f=New-Fixture
        Crash $f Convert $point
        ConvertTo-SharedSetup $f.Store $f.Package 'USB\A\1'
        Assert ((Read-SharedSetup $f.Package).Schema -eq 3) 'Shared owner was not established.'
        Fails { Invoke-SetupRecovery $f.Store $f.Original $f.Package }
        Unchanged $f
        ++$passed;Write-Output "PASS real process exit during shared ownership transfer at $point"
    }
    foreach ($remove in @('A','B')) {
        $f=New-Fixture;$callbacks=Callbacks $f.Store $f.Package
        ConvertTo-SharedSetup $f.Store $f.Package 'USB\A\1'
        Invoke-SharedDeviceSetup $f.Store $f.Package Install $f.B 'Fixture B' 'USB\B\2' @callbacks
        $f.Store.Write((Get-ControlContextKeys $f.B)[2],$script:ControlContextId+',4',3,'DDEEFF')
        Invoke-SharedDeviceSetup $f.Store $f.Package Remove $f.$remove '' '' @callbacks
        $catalog=Read-DeploymentState (Join-Path $f.Package 'endpoints.json')
        $other=if ($remove -eq 'A') { $f.B } else { $f.A }
        Assert-EndpointAssignments $f.Store $catalog $other (Get-Binding $catalog $other)
        Assert-EndpointAssignments $f.Store $catalog $f.$remove $null
        Invoke-SharedDeviceSetup $f.Store $f.Package Remove $other '' '' @callbacks
        Assert ((Read-DeploymentState (Join-Path $f.Package 'endpoints.json')).Bindings.Count -eq 0) 'Last device remained attached.'
        Invoke-SharedDeviceSetup $f.Store $f.Package Install $f.B 'Fixture B' 'USB\B\2' @callbacks
        Assert ($f.Store.Read((Get-ControlContextKeys $f.B)[2],$script:ControlContextId+',4').Data -eq 'DDEEFF') 'Reinstallation lost saved EQ.'
        Unchanged $f
        ++$passed;Write-Output "PASS remove $remove first, remove last device, and reinstall with independent saved EQ"
    }
    foreach ($point in @('context-recorded','context-journaled','context-provisioned','written-0','committed')) {
        $f=New-Fixture;$callbacks=Callbacks $f.Store $f.Package
        ConvertTo-SharedSetup $f.Store $f.Package 'USB\A\1'
        Crash $f Install $point
        Invoke-SharedDeviceSetup $f.Store $f.Package Remove $f.B '' '' @callbacks
        Invoke-SharedDeviceSetup $f.Store $f.Package Install $f.B 'Fixture B' 'USB\B\2' @callbacks
        $catalog=Read-DeploymentState (Join-Path $f.Package 'endpoints.json')
        Assert ($catalog.Bindings.Count -eq 2 -and $null -eq $catalog.Operation) 'Recovery did not leave two independent devices.'
        Assert ([IO.File]::ReadAllText((Join-Path $f.Package 'audio.txt')) -eq 'running') 'Recovery left audio stopped.'
        Unchanged $f
        ++$passed;Write-Output "PASS interrupted second-device setup at $point recovers without altering the first device"
    }
    $f=New-Fixture;$callbacks=Callbacks $f.Store $f.Package
    ConvertTo-SharedSetup $f.Store $f.Package 'USB\A\1'
    $callbacks.VerifyApplied={param($Endpoint,$Registration,$Binding) $null -eq $Binding}
    Fails { Invoke-SharedDeviceSetup $f.Store $f.Package Install $f.B 'Fixture B' 'USB\B\2' @callbacks }
    Assert ((Read-DeploymentState (Join-Path $f.Package 'endpoints.json')).Bindings.Count -eq 1) 'Failed verification retained a new binding.'
    Unchanged $f
    ++$passed;Write-Output 'PASS failed second-device engine check rolls back only that attachment'

    foreach ($remove in @('A','B')) {
        $f=New-Fixture;$callbacks=Callbacks $f.Store $f.Package
        Set-VendorEffects $f.Store $f.B;$original=VendorState $f.Store $f.B
        ConvertTo-SharedSetup $f.Store $f.Package 'USB\A\1'
        Invoke-SharedDeviceSetup $f.Store $f.Package Install $f.B 'Composite B' 'USB\B\2' @callbacks
        $path=Join-Path $f.Package 'endpoints.json';$catalog=Read-DeploymentState $path
        Assert ($catalog.Schema -eq 3 -and (Get-Binding $catalog $f.B).Placement.Kind -eq 'CompositeEndpoint') 'Composite ownership was not versioned.'
        $old=$catalog.Schema;$catalog.Schema=2;Fails { Assert-DeploymentState $catalog };$catalog.Schema=$old
        $fx=(Get-EndpointPath $f.B)+'\FxProperties'
        Assert (@(Read-EffectClassList ($f.Store.Read($fx,$script:EffectPrefix+'15'))).Count -eq 3) 'Expected two vendor effects plus one Contour instance.'
        Assert ($null -eq $f.Store.Read($fx,$script:EffectPrefix+'1') -and $null -eq $f.Store.Read($fx,$script:EffectPrefix+'2')) 'Composite installation also wrote legacy slots.'
        $f.Store.Write((Get-ControlContextKeys $f.B)[2],$script:ControlContextId+',4',3,'DDEEFF')
        Invoke-SharedDeviceSetup $f.Store $f.Package Remove $f.$remove '' '' @callbacks
        $other=if ($remove -eq 'A') { $f.B } else { $f.A }
        Invoke-SharedDeviceSetup $f.Store $f.Package Remove $other '' '' @callbacks
        Assert ((VendorState $f.Store $f.B) -ceq $original) 'Removal changed vendor effects, settings, order, or access.'
        Invoke-SharedDeviceSetup $f.Store $f.Package Install $f.B 'Composite B' 'USB\B\2' @callbacks
        Assert ($f.Store.Read((Get-ControlContextKeys $f.B)[2],$script:ControlContextId+',4').Data -eq 'DDEEFF') 'Composite reinstallation lost saved EQ.'
        Unchanged $f
        ++$passed;Write-Output "PASS composite and legacy coexist; remove $remove first; preserve vendor effects and saved EQ"
    }
    foreach ($point in @('prepared','intent-0','written-0','audio-restarted','committed')) {
        $f=New-Fixture;$callbacks=Callbacks $f.Store $f.Package
        Set-VendorEffects $f.Store $f.B;$original=VendorState $f.Store $f.B
        ConvertTo-SharedSetup $f.Store $f.Package 'USB\A\1'
        Crash $f Install $point
        Invoke-SharedDeviceSetup $f.Store $f.Package Remove $f.B '' '' @callbacks
        Assert ((VendorState $f.Store $f.B) -ceq $original) 'Interrupted composite operation damaged vendor state.'
        Assert ([IO.File]::ReadAllText((Join-Path $f.Package 'audio.txt')) -eq 'running') 'Composite recovery left audio stopped.'
        Unchanged $f
        ++$passed;Write-Output "PASS composite worker termination at $point preserves vendor state"
    }
    $f=New-Fixture;$callbacks=Callbacks $f.Store $f.Package
    Set-VendorEffects $f.Store $f.B;$original=VendorState $f.Store $f.B
    ConvertTo-SharedSetup $f.Store $f.Package 'USB\A\1'
    $callbacks.VerifyApplied={param($Endpoint,$Registration,$Binding) $null -eq $Binding}
    Fails { Invoke-SharedDeviceSetup $f.Store $f.Package Install $f.B 'Composite B' 'USB\B\2' @callbacks }
    Assert ((VendorState $f.Store $f.B) -ceq $original) 'Failed composite engine check damaged vendor state.'
    Assert ($null -eq (Read-DeploymentState (Join-Path $f.Package 'endpoints.json')).Operation) 'Failed composite check did not finish recovery.'
    Unchanged $f
    ++$passed;Write-Output 'PASS composite engine failure restores original vendor chain'

    $f=New-Fixture;$callbacks=Callbacks $f.Store $f.Package
    Set-VendorEffects $f.Store $f.B
    ConvertTo-SharedSetup $f.Store $f.Package 'USB\A\1'
    Invoke-SharedDeviceSetup $f.Store $f.Package Install $f.B 'Composite B' 'USB\B\2' @callbacks
    $fx=(Get-EndpointPath $f.B)+'\FxProperties'
    $foreign=Add-CompositeClass ($f.Store.Read($fx,$script:EffectPrefix+'15')) '{dddddddd-dddd-4ddd-8ddd-dddddddddddd}'
    $f.Store.Write($fx,$script:EffectPrefix+'15',$foreign.Type,$foreign.Data)
    Fails { Invoke-SharedDeviceSetup $f.Store $f.Package Remove $f.B '' '' @callbacks }
    Assert (Test-RawEqual ($f.Store.Read($fx,$script:EffectPrefix+'15')) $foreign) 'Removal overwrote a later vendor change.'
    Unchanged $f
    ++$passed;Write-Output 'PASS later vendor changes block stale composite removal without overwriting them'

    $f=New-Fixture;$callbacks=Callbacks $f.Store $f.Package
    Set-VendorEffects $f.Store $f.B
    ConvertTo-SharedSetup $f.Store $f.Package 'USB\A\1'
    $interrupt={param($Point)
        if ($Point -eq 'written-0') {
            $fx=(Get-EndpointPath $f.B)+'\FxProperties'
            $new=Add-CompositeClass ($f.Store.Read($fx,$script:EffectPrefix+'15')) '{dddddddd-dddd-4ddd-8ddd-dddddddddddd}'
            $f.Store.Write($fx,$script:EffectPrefix+'15',$new.Type,$new.Data)
            throw 'Concurrent vendor writer'
        }
    }
    Fails { Invoke-SharedDeviceSetup $f.Store $f.Package Install $f.B 'Composite B' 'USB\B\2' @callbacks -Checkpoint $interrupt }
    $fx=(Get-EndpointPath $f.B)+'\FxProperties'
    Assert (@(Read-EffectClassList ($f.Store.Read($fx,$script:EffectPrefix+'15'))).Count -eq 4) 'Rollback removed a foreign list entry.'
    $catalog=Read-DeploymentState (Join-Path $f.Package 'endpoints.json')
    Assert ($catalog.Operation.Phase -eq 'RecoveryRequired') 'A conflicted rollback claimed success.'
    Assert ([IO.File]::ReadAllText((Join-Path $f.Package 'audio.txt')) -eq 'running') 'Conflicted rollback left audio stopped.'
    Unchanged $f
    ++$passed;Write-Output 'PASS foreign change during composite write is retained with recovery intent'

    $f=New-Fixture -Composite;$callbacks=Callbacks $f.Store $f.Package
    $plan=(Read-SetupJson (Join-Path $f.Package 'journal.json')).Plan
    Assert ($plan.Schema -eq 2 -and $plan.Changes.Count -eq 31) 'First-device composite plan is not versioned.'
    Invoke-DeviceSetup $f.Store $f.Original $f.Package Remove $callbacks.CaptureServices $callbacks.StopAudio $callbacks.StartAudio
    Assert ((VendorState $f.Store $f.A) -ceq $f.VendorBefore) 'Standalone composite removal lost vendor effects.'
    ++$passed;Write-Output 'PASS first-device composite installation/removal restores vendor bytes'

    # With no registered runtime left, exercise actual first-install verification
    # failure and restoration of a vendor chain (not only the shared path).
    Set-VendorEffects $f.Store $f.B;$vendorBefore=VendorState $f.Store $f.B
    $id=[guid]::NewGuid().ToString('D');$package=Join-Path $work $id;[void][IO.Directory]::CreateDirectory($package)
    $info.Endpoint=[pscustomobject]@{Id=$f.B;Name='First composite';State=1;Channels=2;SampleRate=48000}
    $plan=New-ContourPlan $f.Store $info (Join-Path $BinaryDirectory 'contour-apo.dll') ('A'*64) $id
    Save-ContourJournal (New-ContourJournal $plan) (Join-Path $package 'journal.json') -CreateNew
    $state=New-SetupState $plan;Save-SetupState $state $package;$callbacks=Callbacks $f.Store $package
    Fails { Invoke-DeviceSetup $f.Store $state $package Install $callbacks.CaptureServices $callbacks.StopAudio $callbacks.StartAudio -VerifyInstalled {param($Endpoint) $false} }
    Assert ((VendorState $f.Store $f.B) -ceq $vendorBefore -and $state.State -eq 'Removed') 'First-install verification failure did not restore vendor state.'
    foreach ($key in @(Get-ContourClassKeys)) { Assert (-not $f.Store.Exists($key)) 'Failed first install retained registration.' }
    Assert ([IO.File]::ReadAllText((Join-Path $package 'audio.txt')) -eq 'running') 'Failed first install left audio stopped.'
    ++$passed;Write-Output 'PASS first-device failed engine verification restores vendor effects and audio'

    $f=New-Fixture -Composite;$callbacks=Callbacks $f.Store $f.Package
    ConvertTo-SharedSetup $f.Store $f.Package 'USB\A\1'
    Invoke-SharedDeviceSetup $f.Store $f.Package Remove $f.A '' '' @callbacks
    Assert ((VendorState $f.Store $f.A) -ceq $f.VendorBefore) 'Adopted composite installation lost vendor effects.'
    Unchanged $f
    ++$passed;Write-Output 'PASS first-device composite installation transfers to shared ownership and preserves saved EQ'

    foreach ($bad in @('scalar','unterminated','duplicate','own','mixed','nested')) {
        $f=New-Fixture;Set-VendorEffects $f.Store $f.B;$fx=(Get-EndpointPath $f.B)+'\FxProperties'
        switch ($bad) {
            scalar { $f.Store.Write($fx,$script:EffectPrefix+'15',1,(ConvertTo-HexString '{aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa}')) }
            unterminated { $f.Store.Write($fx,$script:EffectPrefix+'15',7,(ConvertTo-HexString '{aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa}')) }
            duplicate { $f.Store.Write($fx,$script:EffectPrefix+'15',7,((ConvertTo-HexString ('{aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa}'+[char]0+'{AAAAAAAA-AAAA-4AAA-8AAA-AAAAAAAAAAAA}'))+'0000')) }
            own { $f.Store.Write($fx,$script:EffectPrefix+'15',7,((ConvertTo-HexString $script:ClassIds[1])+'0000')) }
            mixed { $f.Store.Write($fx,$script:EffectPrefix+'7',1,(ConvertTo-HexString '{aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa}')) }
            nested { $f.Store.Write($fx+'\VendorContext',$script:EffectPrefix+'15',7,((ConvertTo-HexString '{aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa}')+'0000')) }
        }
        $before=$f.Store.Snapshot($fx)
        Fails { Get-AvailableEffectPlacement $f.Store $f.B }
        Assert ($f.Store.Snapshot($fx) -ceq $before) 'Unsupported effects were mutated.'
        ++$passed;Write-Output "PASS reject unsupported composite arrangement: $bad"
    }
    function New-UpdateFixture {
        $f=New-Fixture;ConvertTo-SharedSetup $f.Store $f.Package 'USB\A\1'
        Set-VendorEffects $f.Store $f.B;$callbacks=Callbacks $f.Store $f.Package
        Invoke-SharedDeviceSetup $f.Store $f.Package Install $f.B 'Fixture B' 'USB\B\2' @callbacks
        $f.Store.Write((Get-ControlContextKeys $f.B)[2],$script:ControlContextId+',4',3,'DDEEFF')
        $f | Add-Member NoteProperty EndpointsBefore (@($f.A,$f.B | ForEach-Object { $f.Store.Snapshot((Get-EndpointPath $_)) }) | ConvertTo-Json -Compress)
        return $f
    }
    function UpdateUnchanged($f) {
        Assert ((@($f.A,$f.B | ForEach-Object { $f.Store.Snapshot((Get-EndpointPath $_)) }) | ConvertTo-Json -Compress) -ceq $f.EndpointsBefore) 'Update changed endpoint assignments or saved EQ.'
        Assert ([IO.File]::ReadAllText((Join-Path $f.Package 'audio.txt')) -eq 'running') 'Update left audio stopped.'
        Assert (Test-RawEqual ($f.Store.Read($script:AudioSettings,'DisableProtectedAudioDG')) (Get-UnsignedAudioValue)) 'Update altered the compatibility setting.'
    }
    $f=New-UpdateFixture;$updates=UpdateCallbacks $f.Store $f.Package
    Invoke-RuntimeUpdate $f.Store $f.Package (UpdateBuild) @updates
    $catalog=Read-DeploymentState (Join-Path $f.Package 'endpoints.json')
    Assert ($catalog.Schema -eq 4 -and $catalog.Runtime.Active.Sha256 -ceq ('B'*64) -and $null -eq $catalog.Runtime.Update) 'Engine update did not commit.'
    UpdateUnchanged $f
    Fails { Invoke-RuntimeUpdate $f.Store $f.Package (UpdateBuild) @updates }
    $callbacks=Callbacks $f.Store $f.Package
    Invoke-SharedDeviceSetup $f.Store $f.Package Remove $f.B '' '' @callbacks
    Invoke-SharedDeviceSetup $f.Store $f.Package Install $f.B 'Fixture B' 'USB\B\2' @callbacks
    Assert ((Read-DeploymentState (Join-Path $f.Package 'endpoints.json')).Schema -eq 4) 'Composite reinstall downgraded engine ownership.'
    UpdateUnchanged $f
    $next=[pscustomobject]@{Id='44444444-4444-4444-8444-444444444444';Sha256=('C'*64)}
    Invoke-RuntimeUpdate $f.Store $f.Package $next @updates
    Assert ((Read-DeploymentState (Join-Path $f.Package 'endpoints.json')).Runtime.Active.Sha256 -ceq ('C'*64)) 'Second engine update failed.'
    $updates.VerifyOutput={param($Endpoint,$Build) [pscustomobject]@{Verified=($Build.Sha256 -ceq ('C'*64));Inactive=$false}}
    Fails { Invoke-RuntimeUpdate $f.Store $f.Package (UpdateBuild) @updates }
    Assert ((Read-DeploymentState (Join-Path $f.Package 'endpoints.json')).Runtime.Active.Sha256 -ceq ('C'*64)) 'Rollback lost the previous versioned engine.'
    $downgrade=Read-DeploymentState (Join-Path $f.Package 'endpoints.json');$downgrade.Schema=3;Fails { Assert-DeploymentState $downgrade }
    ++$passed;Write-Output 'PASS versioned engine updates preserve legacy/composite attachments, EQ, policy and subsequent device management'
    foreach ($point in @('update-prepared','update-audio-stopped','update-intent-0','update-written-0','update-intent-1','update-written-1','update-audio-restarted','update-verified','update-committed')) {
        $f=New-UpdateFixture;Crash $f Runtime $point;$updates=UpdateCallbacks $f.Store $f.Package
        $catalog=Read-DeploymentState (Join-Path $f.Package 'endpoints.json')
        if ($point -ne 'update-committed') {
            $devices=Callbacks $f.Store $f.Package
            Fails { Invoke-SharedDeviceSetup $f.Store $f.Package Remove $f.A '' '' @devices }
            Invoke-RuntimeUpdate $f.Store $f.Package $null @updates
        }
        $catalog=Read-DeploymentState (Join-Path $f.Package 'endpoints.json')
        Assert ($null -eq $catalog.Runtime.Update) 'Interrupted engine update retained recovery state.'
        Assert ($catalog.Runtime.Active.Sha256 -ceq $(if ($point -eq 'update-committed') { 'B'*64 } else { 'A'*64 })) 'Crash recovered the wrong engine.'
        Assert-RuntimeRegistration $f.Store (Read-SetupJson (Join-Path $f.Package 'journal.json')).Plan $catalog.Runtime.Active
        UpdateUnchanged $f;++$passed;Write-Output "PASS engine worker termination at $point"
    }
    $f=New-UpdateFixture;$updates=UpdateCallbacks $f.Store $f.Package
    $updates.VerifyOutput={param($Endpoint,$Build) [pscustomobject]@{Verified=($Build.Id -eq 'original');Inactive=$false}}
    Fails { Invoke-RuntimeUpdate $f.Store $f.Package (UpdateBuild) @updates }
    $catalog=Read-DeploymentState (Join-Path $f.Package 'endpoints.json')
    Assert ($catalog.Runtime.Active.Id -eq 'original' -and $null -eq $catalog.Runtime.Update) 'Failed engine did not roll back.'
    UpdateUnchanged $f;++$passed;Write-Output 'PASS failed engine loading rolls back all shared class paths and preserves every output'
    $f=New-UpdateFixture;$updates=UpdateCallbacks $f.Store $f.Package
    $updates.VerifyBuild={param($Build) $Build.Id -eq 'original'}
    Fails { Invoke-RuntimeUpdate $f.Store $f.Package (UpdateBuild) @updates }
    Unchanged $f;UpdateUnchanged $f;++$passed;Write-Output 'PASS unverifiable candidate rejected before switching or stopping audio'
    $f=New-UpdateFixture;$updates=UpdateCallbacks $f.Store $f.Package
    $updates.VerifyOutput={param($Endpoint,$Build) [pscustomobject]@{Verified=$true;Inactive=$true}}
    Invoke-RuntimeUpdate $f.Store $f.Package (UpdateBuild) @updates
    Assert ((Read-DeploymentState (Join-Path $f.Package 'endpoints.json')).Runtime.Deferred.Count -eq 2) 'Disconnected devices falsely claimed live verification.'
    UpdateUnchanged $f;++$passed;Write-Output 'PASS disconnected attachments retained with deferred live verification'
    $f=New-UpdateFixture;$updates=UpdateCallbacks $f.Store $f.Package
    $changedPath='SOFTWARE\Classes\CLSID\'+$script:ClassIds[0]+'\InprocServer32'
    $foreign=[pscustomobject]@{Type=1;Data=(ConvertTo-HexString 'C:\Foreign\engine.dll')}
    $checkpoint={param($Point) if ($Point -eq 'update-written-0') { $f.Store.Write($changedPath,'',$foreign.Type,$foreign.Data);throw 'Injected external writer' }}
    Fails { Invoke-RuntimeUpdate $f.Store $f.Package (UpdateBuild) @updates -Checkpoint $checkpoint }
    Assert (Test-RawEqual ($f.Store.Read($changedPath,'')) $foreign) 'Rollback overwrote a foreign registration.'
    Assert ((Read-DeploymentState (Join-Path $f.Package 'endpoints.json')).Runtime.Update.Phase -eq 'RecoveryRequired') 'Foreign registration lost recovery intent.'
    UpdateUnchanged $f;++$passed;Write-Output 'PASS conflicting registration preserved and audio restarted with explicit recovery state'
    foreach ($point in @('restored-1','restored-0','rollback-audio-restarted')) {
        $f=New-UpdateFixture;Crash $f RuntimeRollback $point;$updates=UpdateCallbacks $f.Store $f.Package
        Invoke-RuntimeUpdate $f.Store $f.Package $null @updates
        Assert ((Read-DeploymentState (Join-Path $f.Package 'endpoints.json')).Runtime.Active.Id -eq 'original') 'Interrupted rollback failed to restore the original engine.'
        UpdateUnchanged $f;++$passed;Write-Output "PASS worker termination during engine rollback at $point"
    }
    $f=New-UpdateFixture;$callbacks=Callbacks $f.Store $f.Package
    foreach ($id in @($f.A,$f.B)) { Invoke-SharedDeviceSetup $f.Store $f.Package Remove $id '' '' @callbacks }
    $updates=UpdateCallbacks $f.Store $f.Package;Invoke-RuntimeUpdate $f.Store $f.Package (UpdateBuild) @updates
    $catalog=Read-DeploymentState (Join-Path $f.Package 'endpoints.json')
    Assert ($catalog.Runtime.Active.Sha256 -ceq ('B'*64) -and $catalog.Bindings.Count -eq 0) 'Retained runtime without attachments could not update.'
    ++$passed;Write-Output 'PASS retained runtime updates with no attached devices'
    Write-Output "$passed shared setup cases passed; no real audio or HKLM changes."
} catch {
    Write-Output $_.ScriptStackTrace
    throw
} finally {
    foreach ($store in $stores) { $store.Dispose() }
    if ($null -ne $root) { $root.Dispose();$user.DeleteSubKeyTree($testPath,$false) }
    $user.Dispose()
}
