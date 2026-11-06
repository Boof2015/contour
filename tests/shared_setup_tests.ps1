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
    if ($Mode -eq 'Convert') { ConvertTo-SharedSetup $store $f.Package 'USB\A\1' $checkpoint }
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
    function New-Fixture($Before=$null) {
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
        $info.Endpoint=[pscustomobject]@{Id=$a;Name='Fixture A';State=1;Channels=2;SampleRate=48000}
        $plan=New-ContourPlan $store $info (Join-Path $BinaryDirectory 'contour-apo.dll') ('A'*64) $id
        Save-ContourJournal (New-ContourJournal $plan) (Join-Path $package 'journal.json') -CreateNew
        $state=New-SetupState $plan;Save-SetupState $state $package
        $callbacks=Callbacks $store $package
        Invoke-DeviceSetup $store $state $package Install $callbacks.CaptureServices $callbacks.StopAudio $callbacks.StartAudio
        $store.Write((Get-ControlContextKeys $a)[2],$script:ControlContextId+',4',3,'AABBCC')
        $f=[pscustomobject]@{Store=$store;Package=$package;A=$a;B=$b;Id=$id;Original=$state;
            SavedA=Get-EndpointSettings $store $a;Globals=$store.Snapshot('SOFTWARE\Classes');Registry=($testPath+'\'+$id)}
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
    Write-Output "$passed shared setup cases passed; no real audio or HKLM changes."
} catch {
    Write-Output $_.ScriptStackTrace
    throw
} finally {
    foreach ($store in $stores) { $store.Dispose() }
    if ($null -ne $root) { $root.Dispose();$user.DeleteSubKeyTree($testPath,$false) }
    $user.Dispose()
}
