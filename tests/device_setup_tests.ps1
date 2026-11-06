param([Parameter(Mandatory=$true)][string]$BinaryDirectory,
      [Parameter(Mandatory=$true)][string]$WorkDirectory)
Set-StrictMode -Version Latest
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot '..\tools\deployment\DeviceSetup.ps1')
function Assert($Condition,[string]$Message) { if (-not $Condition) { throw $Message } }
function Fails([scriptblock]$Action) { $failed=$false; try { & $Action } catch { $failed=$true }; Assert $failed 'Expected operation to fail' }
$testId=[guid]::NewGuid().ToString('D')
$key='Software\ContourDeviceSetupTest-'+$testId
$user=[Microsoft.Win32.RegistryKey]::OpenBaseKey([Microsoft.Win32.RegistryHive]::CurrentUser,[Microsoft.Win32.RegistryView]::Registry64)
$root=$null; $stores=@(); $passed=0
$work=Join-Path ([IO.Path]::GetFullPath($WorkDirectory)) $testId
[void][IO.Directory]::CreateDirectory($work)
try {
    $raw=& (Join-Path $BinaryDirectory 'contour-deploy-info.exe') (Join-Path $BinaryDirectory 'contour-apo.dll') '--registration-only'
    if ($LASTEXITCODE -ne 0) { throw 'Registration capture failed.' }
    $info=$raw | ConvertFrom-Json
    $info.Endpoint=[pscustomobject]@{Id='{0.0.0.00000000}.{d673b998-8476-4faa-af56-e3eec4688246}';Name='Test output';State=1;Channels=2;SampleRate=48000}
    $root=$user.CreateSubKey($key)
    function New-Fixture {
        $id=[guid]::NewGuid().ToString('D'); $registry=$root.CreateSubKey($id)
        $fx=(Get-EndpointPath $info.Endpoint.Id)+'\FxProperties'
        foreach ($path in @($fx,((Get-EndpointPath $info.Endpoint.Id)+'\Properties'),'SOFTWARE\Classes\CLSID','SOFTWARE\Classes\AudioEngine\AudioProcessingObjects',$script:AudioSettings)) {
            $k=$registry.CreateSubKey($path); $k.Dispose()
        }
        $store=[Contour.Deployment.RegistryStore]::new($registry,$true); $script:stores+=@($store)
        $store.Write($fx,'DriverData',3,'0123FF')
        $plan=New-ContourPlan $store $info (Join-Path $BinaryDirectory 'contour-apo.dll') ('A'*64) $id
        $package=Join-Path $work $id; [void][IO.Directory]::CreateDirectory($package)
        Save-ContourJournal (New-ContourJournal $plan) (Join-Path $package 'journal.json') -CreateNew
        $state=New-SetupState $plan; Save-SetupState $state $package
        [pscustomobject]@{Store=$store;State=$state;Package=$package;Fx=$fx;Baseline=$store.Snapshot($fx)}
    }
    $capture={ 'Dependent'; 'Audiosrv' }
    $stop={param($Names) Assert (($Names -join ',') -eq 'Dependent,Audiosrv') 'Stop lost the original service list'; $script:stopped=$true }
    $start={param($Names) Assert (($Names -join ',') -eq 'Dependent,Audiosrv') 'Restart lost the original service list'; $script:restarted=$true }
    function Restored($Fixture) {
        Assert ($Fixture.Store.Snapshot($Fixture.Fx) -ceq $Fixture.Baseline) 'Recovery did not preserve the original effect tree'
        foreach ($path in Get-ContourClassKeys) { Assert (-not $Fixture.Store.Exists($path)) 'COM/APO registration remains' }
        Assert $script:restarted 'Audio restart was not attempted'
    }
    $f=New-Fixture
    Invoke-DeviceSetup $f.Store $f.State $f.Package Install $capture $stop $start
    Assert ($f.State.State -eq 'Attached' -and -not $f.State.AudioRestartPending) 'Install did not finish'
    # Real user save data belongs only to our new context, never the profile library.
    $context=Read-SetupJson (Join-Path $f.Package 'context-journal.json')
    $f.Store.Write($context.Plan.Keys[2],($script:ControlContextId+',4'),3,'012345')
    Invoke-DeviceSetup $f.Store $f.State $f.Package Remove $capture $stop $start
    Restored $f
    Invoke-DeviceSetup $f.Store $f.State $f.Package Remove $capture $stop $start
    ++$passed; Write-Output 'PASS complete install, remove, saved context cleanup, repeat remove'
    foreach ($point in @('audio-stopped','attached','context-journaled','context-ready')) {
        $f=New-Fixture; $script:restarted=$false
        $inject={param($At) if ($At -eq $point) { throw 'Injected failure' }}
        Fails { Invoke-DeviceSetup $f.Store $f.State $f.Package Install $capture $stop $start $inject }
        Restored $f; Assert ($f.State.State -eq 'Removed') 'Failed setup did not roll back'
        ++$passed; Write-Output "PASS rollback at $point"
    }
    $f=New-Fixture
    Fails { Invoke-DeviceSetup $f.Store $f.State $f.Package Install $capture $stop {param($Names) throw 'Service start failed'} }
    Assert ($f.State.State -eq 'RecoveryRequired' -and $f.State.AudioRestartPending) 'Service failure was hidden'
    # Simulate reopening after the helper died: all authority comes from disk.
    $reopened=Read-SetupJson (Join-Path $f.Package 'device-setup.json')
    Invoke-DeviceSetup $f.Store $reopened $f.Package Remove { throw 'Must retain original service list' } $stop $start
    Restored $f; Assert (-not $reopened.AudioRestartPending) 'Service recovery remains pending'
    ++$passed; Write-Output 'PASS persistent service restart recovery'
    $f=New-Fixture
    Invoke-DeviceSetup $f.Store $f.State $f.Package Install $capture $stop $start
    $f.Store.Write($f.Fx,($script:EffectPrefix+'2'),1,(ConvertTo-HexString '{11111111-1111-1111-1111-111111111111}'))
    Fails { Invoke-DeviceSetup $f.Store $f.State $f.Package Remove $capture $stop $start }
    Assert ($f.State.State -eq 'RecoveryRequired') 'Foreign assignment did not require recovery'
    Assert ($f.Store.Exists((Get-ControlContextKeys $info.Endpoint.Id)[0])) 'Ambiguous attachment lost its control store'
    ++$passed; Write-Output 'PASS foreign effect conflict preserves context and registrations'
    $f=New-Fixture
    $f.State.EndpointId='{0.0.0.00000000}.{11111111-1111-1111-1111-111111111111}'
    Fails { Invoke-DeviceSetup $f.Store $f.State $f.Package Remove $capture $stop $start }
    Assert ($f.Store.Snapshot($f.Fx) -ceq $f.Baseline) 'Mismatched journal changed the endpoint'
    ++$passed; Write-Output 'PASS mismatched recovery identity rejected'
    Write-Output "$passed setup transaction cases passed; no machine registry or audio services changed."
} finally {
    foreach ($store in $stores) { $store.Dispose() }
    if ($null -ne $root) { $root.Dispose(); $user.DeleteSubKeyTree($key,$false) }
    $user.Dispose()
}
