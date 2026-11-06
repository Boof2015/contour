param([string]$BinaryDirectory,[string]$WorkDirectory,[string]$WorkerFixturePath,[string]$CrashAt,
    [ValidateSet('Begin','Recover')][string]$MigrationWorker)
Set-StrictMode -Version Latest
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot '..\tools\deployment\LegacyMigration.ps1')
function Assert($Condition,[string]$Message) { if (-not $Condition) { throw $Message } }
function Fails([scriptblock]$Action) {
    $failed=$false;try { & $Action | Out-Null } catch { $failed=$true }
    Assert $failed 'Expected failure was not reported.'
}
function Clone($Value) { $Value | ConvertTo-Json -Depth 30 | ConvertFrom-Json }
function Callbacks([string]$Directory) {
    $audioFile=Join-Path $Directory 'audio.txt'
    @{
        VerifyRegistration={param($Endpoint,$Registration,$Binding) $true}
        VerifyApplied={param($Endpoint,$Registration,$Binding) $true}
        CaptureServices={ 'FixtureDependent';'Audiosrv' }
        StopAudio={param($Names)
            if (($Names -join ',') -ne 'FixtureDependent,Audiosrv') { throw 'Wrong original service list.' }
            [IO.File]::WriteAllText($audioFile,'stopped')
        }.GetNewClosure()
        StartAudio={param($Names)
            if (($Names -join ',') -ne 'FixtureDependent,Audiosrv') { throw 'Wrong restart service list.' }
            [IO.File]::WriteAllText($audioFile,'running')
        }.GetNewClosure()
    }
}
if ($WorkerFixturePath) {
    $fixture=Get-Content -LiteralPath $WorkerFixturePath -Raw | ConvertFrom-Json
    if ($fixture.RegistryPath -notmatch '^Software\\ContourEndpointDeploymentTest-[0-9a-f-]{36}\\[0-9a-f-]{36}\z') { throw 'Worker requires an isolated HKCU fixture.' }
    $user=[Microsoft.Win32.RegistryKey]::OpenBaseKey([Microsoft.Win32.RegistryHive]::CurrentUser,[Microsoft.Win32.RegistryView]::Registry64)
    $root=$user.OpenSubKey($fixture.RegistryPath,$true)
    if ($null -eq $root) { throw 'Worker registry fixture is absent.' }
    $store=[Contour.Deployment.RegistryStore]::new($root,$true)
    $callbacks=Callbacks (Split-Path $fixture.Path -Parent)
    try {
        $checkpoint={
            param($Point)
            # Actual process termination: no catch/finally recovery runs. The
            # parent must reopen the on-disk journal and recover independently.
            if ($Point -eq $CrashAt) { [Environment]::Exit(91) }
        }
        if ($MigrationWorker -eq 'Begin') {
            Invoke-LegacyMigration $store $fixture.Path (Split-Path $fixture.Path -Parent) $fixture.AdapterA $fixture.RegA @callbacks -Checkpoint $checkpoint
        } elseif ($MigrationWorker -eq 'Recover') {
            Resume-LegacyMigration $store $fixture.Path (Split-Path $fixture.Path -Parent) -StopAudio $callbacks.StopAudio `
                -StartAudio $callbacks.StartAudio -VerifyRegistration $callbacks.VerifyRegistration -VerifyApplied $callbacks.VerifyApplied -Checkpoint $checkpoint
        } else {
            Invoke-EndpointTransition $store $fixture.Path Migrate $fixture.A $fixture.AdapterA $fixture.RegA @callbacks -Checkpoint $checkpoint
        }
        throw 'Crash checkpoint was not reached.'
    } finally { $store.Dispose();$user.Dispose() }
    exit 1
}

$testId=[guid]::NewGuid().ToString('D')
$testPath='Software\ContourEndpointDeploymentTest-'+$testId
$user=[Microsoft.Win32.RegistryKey]::OpenBaseKey([Microsoft.Win32.RegistryHive]::CurrentUser,[Microsoft.Win32.RegistryView]::Registry64)
$root=$null;$stores=[Collections.Generic.List[object]]::new();$passed=0
$work=Join-Path ([IO.Path]::GetFullPath($WorkDirectory)) $testId
[void][IO.Directory]::CreateDirectory($work)
try {
    $raw=& (Join-Path $BinaryDirectory 'contour-deploy-info.exe') (Join-Path $BinaryDirectory 'contour-apo.dll') '--registration-only'
    Assert ($LASTEXITCODE -eq 0) 'Cannot capture SDK registration metadata.'
    $info=$raw | ConvertFrom-Json
    $root=$user.CreateSubKey($testPath)
    function New-Fixture([switch]$Handoff) {
        $id=[guid]::NewGuid().ToString('D');$registry=$root.CreateSubKey($id)
        $directory=Join-Path $work $id;[void][IO.Directory]::CreateDirectory($directory)
        $a='{0.0.0.00000000}.{11111111-1111-4111-8111-111111111111}'
        $b='{0.0.0.00000000}.{22222222-2222-4222-8222-222222222222}'
        foreach ($path in @('SOFTWARE\Classes\CLSID','SOFTWARE\Classes\AudioEngine\AudioProcessingObjects',
            ((Get-EndpointPath $a)+'\FxProperties'),((Get-EndpointPath $a)+'\Properties'),
            ((Get-EndpointPath $b)+'\FxProperties'),((Get-EndpointPath $b)+'\Properties'),$script:AudioSettings)) {
            $key=$registry.CreateSubKey($path);$key.Dispose()
        }
        $store=[Contour.Deployment.RegistryStore]::new($registry,$true);$stores.Add($store)
        $info.Endpoint=[pscustomobject]@{Id=$a;Name='Fixture A';Channels=2;SampleRate=48000;State=1}
        $plan=New-ContourPlan $store $info (Join-Path $BinaryDirectory 'contour-apo.dll') ('A'*64) ([guid]::NewGuid().ToString('D'))
        $attachment=New-ContourJournal $plan
        $attachmentPath=Join-Path $directory 'journal.json'
        Save-ContourJournal $attachment $attachmentPath -CreateNew
        Invoke-ContourApply $store $attachment $attachmentPath
        $contexts=@{}
        foreach ($endpoint in @($a,$b)) {
            $context=New-ControlContextJournal (New-ControlContextPlan $store $endpoint)
            $contextPath=Join-Path $directory (([guid]::NewGuid().ToString('D'))+'.json')
            Save-ContourJournal $context $contextPath -CreateNew
            Invoke-ControlContextApply $store $context $contextPath
            $contexts[$endpoint]=$context
            # Opaque, distinct user data must remain byte-for-byte unchanged.
            $userData=if ($endpoint -eq $a) { '0041FF00A1' } else { '0042FF00B2' }
            $store.Write($context.Plan.Keys[2],($script:ControlContextId+',4'),3,$userData)
            $store.Write($context.Plan.Keys[3],($script:ControlContextId+',2'),3,'1234')
        }
        $path=Join-Path $directory 'deployment.json'
        New-EndpointDeployment $path
        $adapterA='USB\FIXTURE_A\1';$adapterB='USB\FIXTURE_B\2'
        if (-not $Handoff) {
            Import-LegacyEndpoint $store $path $attachment $contexts[$a] $adapterA
            Import-LegacyEndpoint $store $path $attachment $contexts[$a] $adapterA
            Assert ((Read-DeploymentState $path).Bindings.Count -eq 1) 'Repeated import duplicated endpoint ownership.'
        }
        $setup=New-SetupState $plan;$setup.State='Attached'
        Save-SetupState $setup $directory
        $contexts[$a].SavedProfileCleanup=$true
        Save-ContourJournal $contexts[$a] (Join-Path $directory 'context-journal.json') -CreateNew
        $apo=[pscustomobject]@{Id=([guid]::NewGuid().ToString('D'));Kind='Apo';Identity='oem901.inf';Sha256=('B'*64)}
        $extensionA=[pscustomobject]@{Id=([guid]::NewGuid().ToString('D'));Kind='Extension';Identity='oem902.inf';Sha256=('C'*64)}
        $extensionB=[pscustomobject]@{Id=([guid]::NewGuid().ToString('D'));Kind='Extension';Identity='oem903.inf';Sha256=('D'*64)}
        $regA=[pscustomobject]@{Id=([guid]::NewGuid().ToString('D'));Mode='Component';AdapterId=$adapterA;Packages=@($apo,$extensionA)}
        $regB=[pscustomobject]@{Id=([guid]::NewGuid().ToString('D'));Mode='Component';AdapterId=$adapterB;Packages=@($apo,$extensionB)}
        Add-EndpointRegistration $path $regA;Add-EndpointRegistration $path $regB
        $f=[pscustomobject]@{RegistryPath=($testPath+'\'+$id);Path=$path;A=$a;B=$b;AdapterA=$adapterA;AdapterB=$adapterB;
            RegA=$regA.Id;RegB=$regB.Id;Legacy=$plan.Id;Apo=$apo.Id;ExtensionA=$extensionA.Id;ExtensionB=$extensionB.Id;
            Store=$store;Directory=$directory;AttachmentPath=$attachmentPath;LegacyText=[IO.File]::ReadAllText($attachmentPath);OriginalSetup=$setup;
            SettingsA=Get-EndpointSettings $store $a;SettingsB=Get-EndpointSettings $store $b;
            OtherSnapshot=$store.Snapshot((Get-EndpointPath $b)+'\FxProperties');
            ClassSnapshot=$store.Snapshot('SOFTWARE\Classes')}
        [IO.File]::WriteAllText((Join-Path $directory 'worker.json'),([pscustomobject]@{
            RegistryPath=$f.RegistryPath;Path=$path;A=$a;AdapterA=$adapterA;RegA=$regA.Id} | ConvertTo-Json))
        return $f
    }
    function Unchanged($f) {
        foreach ($side in @('A','B')) {
            $actual=Get-EndpointSettings $f.Store $f.$side;$expected=$f.('Settings'+$side)
            foreach ($key in @('Default','User','Security')) { Assert ($actual.$key -ceq $expected.$key) "Endpoint $side $key changed" }
        }
        Assert ($f.Store.Snapshot('SOFTWARE\Classes') -ceq $f.ClassSnapshot) 'Shared registration was changed by an endpoint operation.'
        Assert ([IO.File]::ReadAllText($f.AttachmentPath) -ceq $f.LegacyText) 'Original legacy recovery journal changed.'
    }
    function Retained($f,[string]$Id) {
        $usage=@(Get-DeploymentPackageUsage (Read-DeploymentState $f.Path) | Where-Object { $_.Id -eq $Id })
        Assert ($usage.Count -eq 1 -and $usage[0].RemovalRequiresSystemCheck) 'Package retention must not authorize deletion.'
        return $usage[0].Retain
    }
    function Recover($f,$callbacks) {
        Undo-EndpointTransition $f.Store $f.Path -StopAudio $callbacks.StopAudio -StartAudio $callbacks.StartAudio `
            -VerifyRegistration $callbacks.VerifyRegistration -VerifyApplied $callbacks.VerifyApplied
    }

    foreach ($remove in @('A','B')) {
        $f=New-Fixture;$callbacks=Callbacks $f.Directory
        Invoke-EndpointTransition $f.Store $f.Path Attach $f.B $f.AdapterA $f.RegA @callbacks
        Invoke-EndpointTransition $f.Store $f.Path Migrate $f.A $f.AdapterA $f.RegA @callbacks
        Assert ((Read-DeploymentState $f.Path).Bindings.Count -eq 2) 'Both outputs were not independently bound.'
        Invoke-EndpointTransition $f.Store $f.Path Detach $f.$remove $f.AdapterA '' @callbacks
        Assert (Retained $f $f.Apo) 'Removing one output released a shared APO package.'
        Assert (Retained $f $f.ExtensionA) 'Removing one output released its shared extension.'
        $other=if ($remove -eq 'A') { $f.B } else { $f.A }
        $state=Read-DeploymentState $f.Path
        Assert-EndpointAssignments $f.Store $state $other (Get-Binding $state $other)
        Assert-EndpointAssignments $f.Store $state $f.$remove $null
        Invoke-EndpointTransition $f.Store $f.Path Attach $f.$remove $f.AdapterA $f.RegA @callbacks
        Unchanged $f
        ++$passed;Write-Output "PASS remove/reinstall $remove preserves the other output, shared packages, and saved EQ"
    }
    $f=New-Fixture;$callbacks=Callbacks $f.Directory
    Invoke-EndpointTransition $f.Store $f.Path Attach $f.B $f.AdapterB $f.RegB @callbacks
    Invoke-EndpointTransition $f.Store $f.Path Migrate $f.A $f.AdapterA $f.RegA @callbacks
    Invoke-EndpointTransition $f.Store $f.Path Detach $f.A $f.AdapterA '' @callbacks
    Assert (Retained $f $f.Apo) 'Other adapter lost the shared component package.'
    Assert (Retained $f $f.ExtensionB) 'Other adapter lost its extension.'
    Assert (-not (Retained $f $f.ExtensionA)) 'Unused extension remained a managed dependency.'
    Unchanged $f
    ++$passed;Write-Output 'PASS separate adapters retain their own extensions and share the component package'

    foreach ($point in @('prepared','audio-stopped','intent-0','written-0','intent-1','written-1','verification-pending','audio-restarted','verified','committed')) {
        $f=New-Fixture;$callbacks=Callbacks $f.Directory
        & powershell.exe -NoProfile -ExecutionPolicy RemoteSigned -File $PSCommandPath -WorkerFixturePath (Join-Path $f.Directory 'worker.json') -CrashAt $point
        Assert ($LASTEXITCODE -eq 91) "Worker did not terminate at $point"
        if ($point -ne 'committed') {
            Assert (Retained $f $f.Legacy) 'Pending rollback released the original registration.'
            Assert (Retained $f $f.Apo) 'Pending migration released the destination package.'
        }
        Assert (Recover $f $callbacks) "Recovery failed after process exit at $point"
        Assert (Recover $f $callbacks) 'Repeated recovery was not idempotent.'
        $state=Read-DeploymentState $f.Path
        $expected=if ($point -eq 'committed') { $f.RegA } else { $f.Legacy }
        Assert ((Get-Binding $state $f.A).RegistrationId -eq $expected -and $null -eq $state.Operation) 'Wrong ownership after reopened recovery.'
        Assert-EndpointAssignments $f.Store $state $f.A (Get-Binding $state $f.A)
        Assert ([IO.File]::ReadAllText((Join-Path $f.Directory 'audio.txt')) -eq 'running') 'Recovery left audio stopped.'
        Assert ($f.Store.Snapshot((Get-EndpointPath $f.B)+'\FxProperties') -ceq $f.OtherSnapshot) 'Recovery touched another endpoint.'
        Unchanged $f
        ++$passed;Write-Output "PASS real process exit and durable recovery at $point"
    }
    $f=New-Fixture;$callbacks=Callbacks $f.Directory
    $callbacks.VerifyApplied={param($Endpoint,$Registration,$Binding) ($null -eq $Registration -or $Registration.Mode -eq 'Legacy')}
    Fails { Invoke-EndpointTransition $f.Store $f.Path Migrate $f.A $f.AdapterA $f.RegA @callbacks }
    Assert ((Read-DeploymentState $f.Path).LastResult.Outcome -eq 'RolledBack') 'Failed engine verification did not roll back.'
    Unchanged $f
    ++$passed;Write-Output 'PASS engine verification failure automatically restores the working registration'

    $f=New-Fixture;$callbacks=Callbacks $f.Directory
    $callbacks.StartAudio={param($Names) throw 'Injected restart failure'}
    Fails { Invoke-EndpointTransition $f.Store $f.Path Migrate $f.A $f.AdapterA $f.RegA @callbacks }
    $state=Read-DeploymentState $f.Path
    Assert ($state.Operation.Phase -eq 'RecoveryRequired' -and $state.Operation.AudioRestartPending) 'Restart failure lost durable recovery state.'
    Assert ((Retained $f $f.Legacy) -and (Retained $f $f.Apo)) 'Restart failure released a required package.'
    $callbacks=Callbacks $f.Directory
    Assert (Recover $f $callbacks) 'Restart recovery failed.'
    Unchanged $f
    ++$passed;Write-Output 'PASS persistent restart failure retains both registrations and original service state'

    $f=New-Fixture;$callbacks=Callbacks $f.Directory
    $blocked=[pscustomobject]@{File=$null}
    try {
        Fails { Invoke-EndpointTransition $f.Store $f.Path Migrate $f.A $f.AdapterA $f.RegA @callbacks -Checkpoint {
            param($Point)
            # Reads remain possible, but Windows rejects atomic replacement of
            # the journal. Recovery must restart audio even when its own save
            # fails, and leave the last successfully saved intent recoverable.
            if ($Point -eq 'written-0') {
                $blocked.File=[IO.File]::Open($f.Path,[IO.FileMode]::Open,[IO.FileAccess]::Read,[IO.FileShare]::Read)
            }
        } }
        Assert ([IO.File]::ReadAllText((Join-Path $f.Directory 'audio.txt')) -eq 'running') 'Journal failure left audio stopped.'
        $state=Read-DeploymentState $f.Path
        Assert (($state.Operation.Intents -join ',') -eq '0') 'Journal failure lost the durable write intent.'
        Assert ((Retained $f $f.Legacy) -and (Retained $f $f.Apo)) 'Journal failure released a required package.'
        foreach ($badIndices in @(@('0'),@('0,1'),@(1),@(0,0))) {
            $badState=Clone $state;$badState.Operation.Intents=$badIndices
            Fails { Assert-DeploymentState $badState }
        }
    } finally { if ($null -ne $blocked.File) { $blocked.File.Dispose() } }
    Assert (Recover $f $callbacks) 'Recovery failed after journal access was restored.'
    Unchanged $f
    ++$passed;Write-Output 'PASS journal replacement failure restarts audio and preserves durable recovery; malformed indices rejected'

    $f=New-Fixture;$callbacks=Callbacks $f.Directory
    $foreign=ConvertTo-HexString '{33333333-3333-4333-8333-333333333333}'
    $fx=(Get-EndpointPath $f.A)+'\FxProperties'
    Fails { Invoke-EndpointTransition $f.Store $f.Path Migrate $f.A $f.AdapterA $f.RegA @callbacks -Checkpoint {
        param($Point)
        if ($Point -eq 'written-0') { $f.Store.Write($fx,$script:EffectPrefix+'1',1,$foreign);throw 'Injected external effect edit' }
    } }
    Assert ($f.Store.Read($fx,$script:EffectPrefix+'1').Data -ceq $foreign) 'Rollback overwrote a foreign effect.'
    Assert ((Retained $f $f.Legacy) -and (Retained $f $f.Apo)) 'Conflicted recovery released a required package.'
    $f.Store.Write($fx,$script:EffectPrefix+'1',1,(ConvertTo-HexString $script:ComponentClassIds[0]))
    Assert (Recover $f $callbacks) 'Recovery failed after the external conflict was resolved.'
    Unchanged $f
    ++$passed;Write-Output 'PASS foreign assignment conflict preserves data and both package dependencies'

    $f=New-Fixture;$callbacks=Callbacks $f.Directory
    $userKey=(Get-ControlContextKeys $f.A)[2]
    Fails { Invoke-EndpointTransition $f.Store $f.Path Migrate $f.A $f.AdapterA $f.RegA @callbacks -Checkpoint {
        param($Point)
        if ($Point -eq 'intent-0') { $f.Store.Write($userKey,$script:ControlContextId+',4',3,'CAFE') }
    } }
    Assert ($f.Store.Read($userKey,$script:ControlContextId+',4').Data -ceq 'CAFE') 'Rollback overwrote newer saved EQ.'
    Assert ((Read-DeploymentState $f.Path).LastResult.Outcome -eq 'RolledBack') 'Concurrent EQ edit did not reject migration.'
    Assert ($f.Store.Snapshot((Get-EndpointPath $f.B)+'\FxProperties') -ceq $f.OtherSnapshot) 'Concurrent-edit recovery touched another endpoint.'
    ++$passed;Write-Output 'PASS concurrent saved EQ edit is preserved while assignments roll back'

    $f=New-Fixture;$callbacks=Callbacks $f.Directory
    Fails { Invoke-EndpointTransition $f.Store $f.Path Migrate $f.A $f.AdapterA $f.RegB @callbacks }
    $callbacks.VerifyRegistration={ 'true' }
    Fails { Invoke-EndpointTransition $f.Store $f.Path Migrate $f.A $f.AdapterA $f.RegA @callbacks }
    $state=Read-DeploymentState $f.Path
    Assert ($null -eq $state.Operation) 'Rejected preflight created a transaction.'
    $bad=Clone (Get-Registration $state $f.RegA)
    $bad.Id=[guid]::NewGuid().ToString('D');$bad.Packages[0].Id=[guid]::NewGuid().ToString('D')
    Fails { Add-EndpointRegistration $f.Path $bad }
    $lease=Lock-DeploymentState $f.Path
    try { Fails { Add-EndpointRegistration $f.Path (Get-Registration $state $f.RegA) } }
    finally { $lease.Dispose() }
    Unchanged $f
    ++$passed;Write-Output 'PASS adapter mismatch, non-boolean verification, duplicate package ownership, and concurrent writer rejected'

    function Resume-Migration($f,$callbacks) {
        Resume-LegacyMigration $f.Store $f.Path $f.Directory -StopAudio $callbacks.StopAudio -StartAudio $callbacks.StartAudio `
            -VerifyRegistration $callbacks.VerifyRegistration -VerifyApplied $callbacks.VerifyApplied
    }
    function Crash-Migration($f,[string]$Mode,[string]$Point) {
        & powershell.exe -NoProfile -ExecutionPolicy RemoteSigned -File $PSCommandPath `
            -WorkerFixturePath (Join-Path $f.Directory 'worker.json') -MigrationWorker $Mode -CrashAt $Point
        Assert ($LASTEXITCODE -eq 91) "Migration worker did not terminate at $Mode/$Point"
    }
    function Legacy-Blocked($f) {
        $record=Read-SetupJson (Join-Path $f.Directory 'device-setup.json')
        Assert ($record.Schema -eq 2) 'Old setup hosts would still accept the setup schema.'
        $status=Get-HandoffSetupStatus $record
        Assert (-not $status.canInstall -and -not $status.canRemove) 'Legacy UI offered a competing setup action.'
        $guard=[pscustomobject]@{Called=$false}
        $never={ $guard.Called=$true;throw 'Legacy service callback must not run' }
        $snapshot=$f.Store.Snapshot((Get-EndpointPath $f.A)+'\FxProperties')
        Fails { Invoke-DeviceSetup $f.Store $f.OriginalSetup $f.Directory Remove $never $never $never }
        Fails { Invoke-SetupRecovery $f.Store $f.OriginalSetup $f.Directory }
        Fails { Save-SetupState $f.OriginalSetup $f.Directory }
        Assert (-not $guard.Called) 'Legacy operation reached service mutation after handoff.'
        Assert ($f.Store.Snapshot((Get-EndpointPath $f.A)+'\FxProperties') -ceq $snapshot) 'Legacy removal changed the migrated endpoint.'
    }
    function Returned($f) {
        $state=Read-DeploymentState $f.Path
        Assert ($null -eq (Get-DeploymentHandoff $state) -and $null -eq $state.Operation -and $null -eq (Get-Binding $state $f.A)) 'Handoff did not return endpoint ownership.'
        Assert (@($state.Registrations | Where-Object { $_.Id -eq $f.Legacy }).Count -eq 0) 'Returned legacy package became a cleanup candidate.'
        $record=Read-SetupJson (Join-Path $f.Directory 'device-setup.json')
        Assert (($record | ConvertTo-Json -Depth 10 -Compress) -ceq ($f.OriginalSetup | ConvertTo-Json -Depth 10 -Compress)) 'Original setup authority was not restored.'
        Assert-LegacySetupAuthority $f.OriginalSetup $f.Directory
        Unchanged $f
    }
    $f=New-Fixture -Handoff;$callbacks=Callbacks $f.Directory
    $oldCatalog=Read-DeploymentState $f.Path;$oldCatalog.Schema=1;Save-DeploymentState $oldCatalog $f.Path
    Invoke-LegacyMigration $f.Store $f.Path $f.Directory $f.AdapterA $f.RegA @callbacks
    $state=Read-DeploymentState $f.Path
    Assert ($state.Schema -eq 2) 'Migration did not upgrade the catalog for old-host rejection.'
    Assert ($null -eq (Get-DeploymentHandoff $state) -and (Get-Binding $state $f.A).RegistrationId -eq $f.RegA) 'Migration did not transfer ownership.'
    Assert ((Read-SetupJson (Join-Path $f.Directory 'device-setup.json')).State -eq 'Migrated') 'Legacy setup was not retired.'
    Legacy-Blocked $f
    # Even after the new owner detaches, the old remover cannot delete the saved
    # context just because both original effect assignments are now absent.
    Invoke-EndpointTransition $f.Store $f.Path Detach $f.A $f.AdapterA '' @callbacks
    Legacy-Blocked $f;Unchanged $f
    Assert ((Get-HandoffSetupStatus (Read-SetupJson (Join-Path $f.Directory 'device-setup.json'))).installation -eq 'unknown') 'Retirement record falsely reported an active installation.'
    ++$passed;Write-Output 'PASS committed handoff blocks stale legacy install/remove/recovery even after new-owner detach'

    foreach ($point in @('handoff-prepared','legacy-frozen','migration-ready','endpoint-written-0','endpoint-committed','legacy-retired','handoff-complete')) {
        $f=New-Fixture -Handoff;$callbacks=Callbacks $f.Directory
        Crash-Migration $f Begin $point
        $state=Read-DeploymentState $f.Path
        if ($null -ne (Get-DeploymentHandoff $state)) {
            Assert ((Retained $f $f.Legacy) -and (Retained $f $f.Apo)) 'Interrupted handoff released a required package.'
            Fails { Add-EndpointRegistration $f.Path (Get-Registration $state $f.RegA) }
            Fails { Invoke-EndpointTransition $f.Store $f.Path Attach $f.B $f.AdapterA $f.RegA @callbacks }
            Fails { Recover $f $callbacks }
        }
        Resume-Migration $f $callbacks;Resume-Migration $f $callbacks
        if ($point -in @('endpoint-committed','legacy-retired','handoff-complete')) {
            $state=Read-DeploymentState $f.Path
            Assert ((Get-Binding $state $f.A).RegistrationId -eq $f.RegA -and $null -eq (Get-DeploymentHandoff $state)) 'Recovery undid a committed migration.'
            Legacy-Blocked $f;Unchanged $f
        } else { Returned $f }
        Assert ($f.Store.Snapshot((Get-EndpointPath $f.B)+'\FxProperties') -ceq $f.OtherSnapshot) 'Handoff recovery touched another output.'
        ++$passed;Write-Output "PASS actual process exit at handoff boundary $point"
    }
    foreach ($point in @('returning-recorded','legacy-returned','handoff-returned')) {
        $f=New-Fixture -Handoff;$callbacks=Callbacks $f.Directory
        Crash-Migration $f Begin 'endpoint-written-0'
        Crash-Migration $f Recover $point
        Resume-Migration $f $callbacks;Returned $f
        ++$passed;Write-Output "PASS actual process exit while returning legacy authority at $point"
    }
    foreach ($point in @('handoff-prepared','legacy-returned')) {
        $f=New-Fixture -Handoff;$callbacks=Callbacks $f.Directory
        if ($point -eq 'handoff-prepared') { Crash-Migration $f Begin $point }
        else { Crash-Migration $f Begin 'endpoint-written-0';Crash-Migration $f Recover $point }
        $old=Read-SetupJson (Join-Path $f.Directory 'device-setup.json')
        Invoke-DeviceSetup $f.Store $old $f.Directory Remove $callbacks.CaptureServices $callbacks.StopAudio $callbacks.StartAudio
        $removed=$f.Store.Snapshot((Get-EndpointPath $f.A)+'\FxProperties')
        Resume-Migration $f $callbacks
        Assert ($f.Store.Snapshot((Get-EndpointPath $f.A)+'\FxProperties') -ceq $removed) 'Handoff recovery resurrected a removed installation.'
        Assert ((Read-SetupJson (Join-Path $f.Directory 'device-setup.json')).State -eq 'Removed') 'Handoff recovery overwrote a newer legacy setup record.'
        Assert ($null -eq (Get-DeploymentHandoff (Read-DeploymentState $f.Path))) 'Returned handoff remained pending.'
        ++$passed;Write-Output "PASS newer legacy removal preserved after process exit at $point"
    }
    $f=New-Fixture -Handoff;$callbacks=Callbacks $f.Directory
    $callbacks.VerifyApplied={param($Endpoint,$Registration,$Binding) ($Registration.Mode -eq 'Legacy')}
    Fails { Invoke-LegacyMigration $f.Store $f.Path $f.Directory $f.AdapterA $f.RegA @callbacks }
    Returned $f
    ++$passed;Write-Output 'PASS failed migration verification restores original installer authority and saved EQ'

    $f=New-Fixture -Handoff;$callbacks=Callbacks $f.Directory
    $callbacks.StartAudio={ throw 'Injected migration restart failure' }
    Fails { Invoke-LegacyMigration $f.Store $f.Path $f.Directory $f.AdapterA $f.RegA @callbacks }
    Assert ((Read-DeploymentState $f.Path).Operation.Phase -eq 'RecoveryRequired') 'Migration recovery lost its restart obligation.'
    Legacy-Blocked $f
    Assert ((Retained $f $f.Legacy) -and (Retained $f $f.Apo)) 'Pending migration recovery released packages.'
    $callbacks=Callbacks $f.Directory;Resume-Migration $f $callbacks;Returned $f
    ++$passed;Write-Output 'PASS failed restart keeps one recovery owner until audio recovery succeeds'

    $f=New-Fixture -Handoff;$callbacks=Callbacks $f.Directory
    $blocked=[pscustomobject]@{File=$null}
    try {
        Fails { Invoke-LegacyMigration $f.Store $f.Path $f.Directory $f.AdapterA $f.RegA @callbacks -Checkpoint {
            param($Point)
            if ($Point -eq 'endpoint-committed') {
                $blocked.File=[IO.File]::Open((Join-Path $f.Directory 'device-setup.json'),[IO.FileMode]::Open,[IO.FileAccess]::Read,[IO.FileShare]::Read)
            }
        } }
        $state=Read-DeploymentState $f.Path
        Assert ($state.LastResult.Outcome -eq 'Committed' -and (Get-Binding $state $f.A).RegistrationId -eq $f.RegA) 'Failed retirement write undid a committed migration.'
        Assert ($null -ne (Get-DeploymentHandoff $state)) 'Failed retirement write lost the pending handoff.'
        Legacy-Blocked $f
    } finally { if ($null -ne $blocked.File) { $blocked.File.Dispose() } }
    Resume-Migration $f $callbacks;Unchanged $f
    Assert ((Read-SetupJson (Join-Path $f.Directory 'device-setup.json')).State -eq 'Migrated') 'Retirement did not finish after restoring file access.'
    ++$passed;Write-Output 'PASS retirement file failure preserves the commit and finishes after reopening'

    $f=New-Fixture -Handoff;$callbacks=Callbacks $f.Directory
    Crash-Migration $f Begin 'endpoint-written-0'
    $partial=$f.Store.Snapshot((Get-EndpointPath $f.A)+'\FxProperties')
    [IO.File]::WriteAllText($f.AttachmentPath,($f.LegacyText+' '))
    Fails { Resume-Migration $f $callbacks }
    Assert ($f.Store.Snapshot((Get-EndpointPath $f.A)+'\FxProperties') -ceq $partial) 'Changed journal authorized recovery writes.'
    Assert ([IO.File]::ReadAllText((Join-Path $f.Directory 'audio.txt')) -eq 'running') 'Changed journal prevented original service restart.'
    Assert ((Retained $f $f.Legacy) -and (Retained $f $f.Apo)) 'Changed journal released recovery dependencies.'
    [IO.File]::WriteAllText($f.AttachmentPath,$f.LegacyText)
    $recordPath=Join-Path $f.Directory 'device-setup.json';$record=Read-SetupJson $recordPath
    $wrong=Clone $record;$wrong.HandoffId=[guid]::NewGuid().ToString('D');Save-ContourJournal $wrong $recordPath
    Fails { Resume-Migration $f $callbacks }
    Assert ($f.Store.Snapshot((Get-EndpointPath $f.A)+'\FxProperties') -ceq $partial) 'Another handoff identity authorized recovery writes.'
    Save-ContourJournal $record $recordPath
    Resume-Migration $f $callbacks;Returned $f
    ++$passed;Write-Output 'PASS changed journals and mismatched handoff identity block writes while preserving service recovery'
    Write-Output "$passed endpoint deployment cases passed; no HKLM, real services, or live audio changed."
} finally {
    foreach ($store in $stores) { $store.Dispose() }
    if ($null -ne $root) { $root.Dispose();$user.DeleteSubKeyTree($testPath,$false) }
    $user.Dispose()
}
