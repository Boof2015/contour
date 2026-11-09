param([string]$BinaryDirectory,[string]$WorkDirectory,[string]$Worker,[string]$CrashAt)
Set-StrictMode -Version Latest
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot '..\tools\deployment\ProductRemoval.ps1')
function Assert($Value,[string]$Message) { if (-not $Value) { throw $Message } }
function Fails([scriptblock]$Run) { $failed=$false;try { $null=& $Run } catch { $failed=$true };Assert $failed 'Expected safe rejection.' }
function Callbacks([string]$Package) {
    $audio=Join-Path $Package 'audio.txt'
    @{CaptureServices={ 'Audiosrv' };
        StopAudio={param($Names) Assert (($Names -join ',') -eq 'Audiosrv') 'Lost service recovery set';[IO.File]::WriteAllText($audio,'stopped')}.GetNewClosure();
        StartAudio={param($Names) Assert (($Names -join ',') -eq 'Audiosrv') 'Lost service recovery set';[IO.File]::WriteAllText($audio,'running')}.GetNewClosure()}
}
if ($Worker) {
    Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class ContourRemovalCrash {
    [DllImport("kernel32.dll")] static extern bool TerminateProcess(IntPtr process,uint code);
    public static void Stop() { TerminateProcess(new IntPtr(-1),91); }
}
'@
    $fixture=Read-SetupJson $Worker
    if ($fixture.Registry -notmatch '^Software\\ContourRemovalTest-[0-9a-f-]{36}\\[0-9a-f-]{36}\z') { throw 'Only isolated fixtures can run crash checks.' }
    $user=[Microsoft.Win32.RegistryKey]::OpenBaseKey([Microsoft.Win32.RegistryHive]::CurrentUser,[Microsoft.Win32.RegistryView]::Registry64)
    $store=[Contour.Deployment.RegistryStore]::new($user.OpenSubKey($fixture.Registry,$true),$true)
    $callbacks=Callbacks $fixture.Package
    Invoke-ProductRemoval $store $fixture.Package @callbacks -Checkpoint {param($Point) if ($Point -eq $CrashAt) { [ContourRemovalCrash]::Stop() }}
    throw 'Crash checkpoint was not reached.'
}
$testId=[guid]::NewGuid().ToString('D');$testPath='Software\ContourRemovalTest-'+$testId
$work=Join-Path ([IO.Path]::GetFullPath($WorkDirectory)) $testId;[void][IO.Directory]::CreateDirectory($work)
$user=[Microsoft.Win32.RegistryKey]::OpenBaseKey([Microsoft.Win32.RegistryHive]::CurrentUser,[Microsoft.Win32.RegistryView]::Registry64)
$root=$user.CreateSubKey($testPath);$stores=[Collections.Generic.List[object]]::new();$passed=0
try {
    $startupPath='Software\Microsoft\Windows\CurrentVersion\Run'
    $startupKey=$root.CreateSubKey('startup');$runKey=$startupKey.CreateSubKey($startupPath);$runKey.Dispose()
    $startup=[Contour.Deployment.RegistryStore]::new($startupKey,$true);$stores.Add($startup)
    $ownExe='C:\Program Files\Contour\contour.exe'
    foreach ($case in @(
        @{Type=1;Command=('"'+$ownExe+'" --startup');Remove=$true},
        @{Type=1;Command=('"'+$ownExe.ToLowerInvariant()+'" --startup');Remove=$true},
        @{Type=1;Command='"C:\Other\contour.exe" --startup';Remove=$false},
        @{Type=1;Command=('"'+$ownExe+'" --different');Remove=$false},
        @{Type=2;Command=('"'+$ownExe+'" --startup');Remove=$false})) {
        $startup.Write($startupPath,'Contour',$case.Type,(ConvertTo-HexString $case.Command))
        $before=$startup.Read($startupPath,'Contour')
        Remove-ProductStartupEntry $startup $ownExe
        $after=$startup.Read($startupPath,'Contour')
        if ($case.Remove) { Assert ($null -eq $after) 'Owned startup entry remains.' }
        else { Assert (Test-RawEqual $before $after) 'Another startup entry was changed.' }
    }
    ++$passed;Write-Output 'PASS only the removed application copy loses its startup entry'
    $raw=& (Join-Path $BinaryDirectory 'contour-deploy-info.exe') (Join-Path $BinaryDirectory 'contour-apo.dll') --registration-only
    Assert ($LASTEXITCODE -eq 0) 'Cannot inspect fixture DLL';$info=$raw | ConvertFrom-Json
    function New-Fixture([switch]$Shared,[switch]$Composite) {
        $id=[guid]::NewGuid().ToString('D');$key=$root.CreateSubKey($id)
        $store=[Contour.Deployment.RegistryStore]::new($key,$true);$stores.Add($store)
        $package=Join-Path $work $id;[void][IO.Directory]::CreateDirectory($package)
        $a='{0.0.0.00000000}.{11111111-1111-4111-8111-111111111111}'
        $b='{0.0.0.00000000}.{22222222-2222-4222-8222-222222222222}'
        foreach ($path in @('SOFTWARE\Classes\CLSID','SOFTWARE\Classes\AudioEngine\AudioProcessingObjects',$script:AudioSettings,
            ((Get-EndpointPath $a)+'\FxProperties'),((Get-EndpointPath $a)+'\Properties'),
            ((Get-EndpointPath $b)+'\FxProperties'),((Get-EndpointPath $b)+'\Properties'))) {
            $created=$key.CreateSubKey($path);$created.Dispose()
        }
        if ($Composite) {
            foreach ($slot in @(13,14,15)) {
                $store.Write(((Get-EndpointPath $a)+'\FxProperties'),$script:EffectPrefix+$slot,7,
                    ((ConvertTo-HexString '{aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa}')+'0000'))
            }
        }
        $vendor=$store.Snapshot((Get-EndpointPath $a)+'\FxProperties')
        $security=$store.Security((Get-EndpointPath $a)+'\FxProperties')
        $info.Endpoint=[pscustomobject]@{Id=$a;Name='Fixture A';State=1;Channels=2;SampleRate=48000}
        $plan=New-ContourPlan $store $info (Join-Path $BinaryDirectory 'contour-apo.dll') ('A'*64) $id
        Save-ContourJournal (New-ContourJournal $plan) (Join-Path $package 'journal.json') -CreateNew
        $state=New-SetupState $plan;Save-SetupState $state $package
        $callbacks=Callbacks $package
        Invoke-DeviceSetup $store $state $package Install $callbacks.CaptureServices $callbacks.StopAudio $callbacks.StartAudio
        $store.Write((Get-ControlContextKeys $a)[2],$script:ControlContextId+',4',3,'AABBCC')
        # Current engines publish their identity separately from preview/status.
        # Full removal must clean it while still rejecting unrelated properties.
        $store.Write((Get-ControlContextKeys $a)[3],$script:ControlContextId+',5',3,'4354454E47303031')
        if ($Shared) {
            ConvertTo-SharedSetup $store $package 'USB\A\1'
            Invoke-SharedDeviceSetup $store $package Install $b 'Disconnected fixture B' 'USB\B\2' @callbacks -VerifyRegistration { $true } -VerifyApplied { $true }
            $store.Write((Get-ControlContextKeys $b)[2],$script:ControlContextId+',4',3,'DDEEFF')
            $store.Write((Get-ControlContextKeys $b)[3],$script:ControlContextId+',5',3,'4354454E47303031')
            $store.Write((Get-EndpointPath $b),'DeviceState',4,'08000000')
        }
        $fixture=[pscustomobject]@{Store=$store;Package=$package;A=$a;B=$b;Id=$id;Plan=$plan;Vendor=$vendor;Security=$security;Registry=($testPath+'\'+$id)}
        Save-ContourJournal ([pscustomobject]@{Registry=$fixture.Registry;Package=$package}) (Join-Path $package 'worker.json') -CreateNew
        return $fixture
    }
    function Removed($f) {
        $record=Read-SetupJson (Join-Path $f.Package 'device-setup.json')
        Assert ($record.State -eq 'Removed' -and -not $record.AudioRestartPending) 'Removal did not finish.'
        foreach ($key in @(Get-ContourClassKeys)) { Assert (-not $f.Store.Exists($key)) 'Shared registration remains.' }
        foreach ($endpoint in @($f.A,$f.B)) {
            foreach ($key in @(Get-ControlContextKeys $endpoint)) { Assert (-not $f.Store.Exists($key)) 'Device control context remains.' }
        }
        Assert (Test-RawEqual ($f.Store.Read($script:AudioSettings,'DisableProtectedAudioDG')) (Get-UnsignedAudioValue)) 'Other APO compatibility was disabled.'
        Assert ([IO.File]::ReadAllText((Join-Path $f.Package 'audio.txt')) -eq 'running') 'Audio was left stopped.'
        Assert-ProductRemovalReferences $f.Store @() $f.Plan.Package
    }
    foreach ($mode in @('Legacy','Shared','Composite','Updated','Detached','Gone')) {
        $f=New-Fixture -Shared:($mode -ne 'Legacy') -Composite:($mode -eq 'Composite')
        $callbacks=Callbacks $f.Package
        if ($mode -eq 'Updated') {
            $build=[pscustomobject]@{Id='33333333-3333-4333-8333-333333333333';Sha256=('B'*64)}
            Invoke-RuntimeUpdate $f.Store $f.Package $build @callbacks -VerifyBuild { $true } -VerifyOutput { [pscustomobject]@{Verified=$true;Inactive=$true} }
        }
        if ($mode -eq 'Detached') {
            foreach ($endpoint in @($f.A,$f.B)) {
                Invoke-SharedDeviceSetup $f.Store $f.Package Remove $endpoint '' '' @callbacks -VerifyRegistration { $true } -VerifyApplied { $true }
            }
        }
        if ($mode -eq 'Gone') {
            $fixtureKey=$root.OpenSubKey($f.Id,$true)
            try { $fixtureKey.DeleteSubKeyTree((Get-EndpointPath $f.B)) } finally { $fixtureKey.Dispose() }
        }
        Invoke-ProductRemoval $f.Store $f.Package @callbacks
        Removed $f
        Assert ($f.Store.Snapshot((Get-EndpointPath $f.A)+'\FxProperties') -ceq $f.Vendor) 'Vendor effects changed.'
        Assert ($f.Store.Security((Get-EndpointPath $f.A)+'\FxProperties') -ceq $f.Security) 'Endpoint permissions changed.'
        Invoke-ProductRemoval $f.Store $f.Package @callbacks
        ++$passed;Write-Output "PASS $mode removal is complete, preserves vendor effects/compatibility, and can be retried"
    }
    foreach ($scope in @('Render','Capture','OtherClass','ForeignValue')) {
        $f=New-Fixture -Shared;$callbacks=Callbacks $f.Package
        if ($scope -eq 'ForeignValue') {
            $f.Store.Write(('SOFTWARE\Classes\CLSID\'+$script:ClassIds[0]+'\InprocServer32'),'ThreadingModel',1,(ConvertTo-HexString 'External'))
        } elseif ($scope -eq 'OtherClass') {
            $path='SOFTWARE\Classes\CLSID\{dddddddd-dddd-4ddd-8ddd-dddddddddddd}\InprocServer32'
            $f.Store.Create($path);$f.Store.Write($path,'',1,(ConvertTo-HexString $f.Plan.TargetDll))
        } else {
            $path='SOFTWARE\Microsoft\Windows\CurrentVersion\MMDevices\Audio\'+$scope+'\{dddddddd-dddd-4ddd-8ddd-dddddddddddd}\FxProperties'
            $f.Store.Create($path);$f.Store.Write($path,$script:EffectPrefix+'1',1,(ConvertTo-HexString $script:ClassIds[0].ToLowerInvariant()))
        }
        $before=$f.Store.Snapshot('SOFTWARE')
        Fails { Invoke-ProductRemoval $f.Store $f.Package @callbacks }
        Assert ($f.Store.Snapshot('SOFTWARE') -ceq $before) 'Conflict caused partial deletion before preflight completed.'
        Assert ((Read-SetupJson (Join-Path $f.Package 'device-setup.json')).Schema -eq 3) 'Preflight failure took ownership.'
        ++$passed;Write-Output "PASS $scope untracked reference/foreign change prevents deletion"
    }
    $f=New-Fixture -Shared;$callbacks=Callbacks $f.Package
    $foreign=(Get-ControlContextKeys $f.B)[2]
    $f.Store.Write($foreign,'ForeignData',3,'CAFE')
    Fails { Invoke-ProductRemoval $f.Store $f.Package @callbacks }
    Assert ($f.Store.Read($foreign,'ForeignData').Data -ceq 'CAFE') 'Unrecognized context data was deleted.'
    Assert ((Read-SetupJson (Join-Path $f.Package 'device-setup.json')).Schema -eq 4) 'Interrupted removal lacks authority.'
    Fails { Invoke-SharedDeviceSetup $f.Store $f.Package Install $f.A 'A' 'USB\A\1' @callbacks -VerifyRegistration { $true } -VerifyApplied { $true } }
    $f.Store.DeleteValue($foreign,'ForeignData')
    Invoke-ProductRemoval $f.Store $f.Package @callbacks;Removed $f
    ++$passed;Write-Output 'PASS foreign context data retained; device setup blocked until removal resumes'
    $f=New-Fixture -Shared;$callbacks=Callbacks $f.Package
    $broken=@{}+$callbacks;$broken.StartAudio={ throw 'Simulated service restart failure' }
    Fails { Invoke-ProductRemoval $f.Store $f.Package @broken }
    Assert ((Read-SetupJson (Join-Path $f.Package 'device-setup.json')).AudioRestartPending) 'Lost service restart obligation.'
    Invoke-ProductRemoval $f.Store $f.Package @callbacks;Removed $f
    ++$passed;Write-Output 'PASS service restart failure keeps recovery state'
    foreach ($point in @('removal-owned','removal-audio-stopped','removal-endpoint-0','removal-endpoint-3',
        'removal-context-0','removal-registration-0','removal-registration-29','removal-audio-restarted','removal-complete')) {
        $f=New-Fixture -Shared;$callbacks=Callbacks $f.Package
        & powershell.exe -NoProfile -ExecutionPolicy RemoteSigned -File $PSCommandPath -Worker (Join-Path $f.Package 'worker.json') -CrashAt $point
        Assert ($LASTEXITCODE -eq 91) "Worker missed $point"
        Invoke-ProductRemoval $f.Store $f.Package @callbacks;Removed $f
        ++$passed;Write-Output "PASS process termination and retry at $point"
    }
    $profiles=@((Join-Path $work 'user-a'),(Join-Path $work 'user-b'))
    foreach ($profile in $profiles) {
        $data=Join-Path $profile 'AppData\Local\Contour';[void][IO.Directory]::CreateDirectory($data)
        foreach ($name in @('profiles.contour-library','profiles.contour-library.settings','profiles.contour-library.drafts','profiles.contour-library.lock','profiles.contour-library.{00112233-4455-6677-8899-aabbccddeeff}.tmp','exported.contour','notes.txt')) {
            [IO.File]::WriteAllText((Join-Path $data $name),'keep until explicit cleanup')
        }
    }
    Remove-ProductUserData $profiles
    foreach ($profile in $profiles) {
        $data=Join-Path $profile 'AppData\Local\Contour'
        Assert (@(Get-ChildItem -LiteralPath $data).Count -eq 2) 'Known saved profile data remains.'
        Assert ((Test-Path -LiteralPath (Join-Path $data 'exported.contour')) -and (Test-Path -LiteralPath (Join-Path $data 'notes.txt'))) 'Cleanup removed an exported profile or unrelated file.'
    }
    Remove-ProductUserData $profiles
    ++$passed;Write-Output 'PASS opt-in cleanup removes profiles, preferences, recovery and temporary files for multiple accounts; exports survive; retry is safe'
    $profile=Join-Path $work 'redirected-user';[void][IO.Directory]::CreateDirectory((Join-Path $profile 'AppData\Local'))
    $target=Join-Path $work 'foreign-data';[void][IO.Directory]::CreateDirectory($target)
    [IO.File]::WriteAllText((Join-Path $target 'profiles.contour-library'),'foreign profile')
    $link=Join-Path $profile 'AppData\Local\Contour'
    [void](New-Item -ItemType Junction -Path $link -Target $target)
    Fails { Remove-ProductUserData @($profile) }
    Fails { [Contour.Deployment.UserDataFile]::Delete((Join-Path $link 'profiles.contour-library')) }
    Assert ([IO.File]::ReadAllText((Join-Path $target 'profiles.contour-library')) -eq 'foreign profile') 'Cleanup followed a redirected data folder.'
    # Remove only the known junction itself, never its contents.
    [IO.Directory]::Delete($link,$false)
    ++$passed;Write-Output 'PASS user data cleanup refuses redirected folders'
    Write-Output "PASS $passed product removal checks; no real devices or audio services changed"
} finally {
    foreach ($store in $stores) { $store.Dispose() }
    $root.Dispose();$user.DeleteSubKeyTree($testPath,$false);$user.Dispose()
}
