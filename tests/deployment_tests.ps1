param([Parameter(Mandatory=$true)][string] $BinaryDirectory,
      [Parameter(Mandatory=$true)][string] $WorkDirectory)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot '..\tools\deployment\Deployment.ps1')

function Assert($Condition, [string] $Message) { if (-not $Condition) { throw $Message } }
function Expect-Failure([scriptblock] $Action, [string] $Message) {
    $failed = $false
    try { & $Action } catch { $failed = $true }
    Assert $failed $Message
}
function Case([string] $Name, [scriptblock] $Body) {
    try { & $Body; ++$script:Passed; Write-Output "PASS $Name" }
    catch { ++$script:Failed; Write-Output "FAIL $Name : $($_.Exception.Message)"; Write-Output $_.ScriptStackTrace }
}

$script:Passed = 0; $script:Failed = 0
$testId = [guid]::NewGuid().ToString('D')
$testPath = 'Software\ContourDeploymentTest-' + $testId
$userRoot = [Microsoft.Win32.RegistryKey]::OpenBaseKey([Microsoft.Win32.RegistryHive]::CurrentUser,
    [Microsoft.Win32.RegistryView]::Registry64)
$testRoot = $null
$fixtures = [Collections.Generic.List[object]]::new()
$work = Join-Path ([IO.Path]::GetFullPath($WorkDirectory)) $testId
[void][IO.Directory]::CreateDirectory($work)
try {
    $rawInfo = & (Join-Path $BinaryDirectory 'contour-deploy-info.exe') (Join-Path $BinaryDirectory 'contour-apo.dll') '--registration-only'
    if ($LASTEXITCODE -ne 0) { throw 'SDK registration capture failed.' }
    $info = $rawInfo | ConvertFrom-Json
    $info.Endpoint = [pscustomobject]@{Id='{0.0.0.00000000}.{d673b998-8476-4faa-af56-e3eec4688246}';
        Name='Isolated test endpoint';State=1;Channels=2;SampleRate=48000}
    $testRoot = $userRoot.CreateSubKey($testPath)

    function New-Fixture {
        $caseId = [guid]::NewGuid().ToString('D')
        $root = $testRoot.CreateSubKey($caseId)
        $endpoint = Get-EndpointPath $info.Endpoint.Id
        foreach ($path in @('SOFTWARE\Classes\CLSID', 'SOFTWARE\Classes\AudioEngine\AudioProcessingObjects',
            ($endpoint+'\FxProperties\context\Default'), ($endpoint+'\Properties'), $script:AudioSettings)) {
            $key = $root.CreateSubKey($path); $key.Dispose()
        }
        $store = [Contour.Deployment.RegistryStore]::new($root, $true)
        $fx = $endpoint+'\FxProperties'
        # Deliberately include bytes that a string-based registry backup could normalize.
        $store.Write($fx, 'UnterminatedString', 1, '4100')
        $store.Write($fx, 'MultiString', 7, '41000000420000000000')
        $store.Write($fx, 'OpaqueData', 3, '0001FF7F80')
        $store.Write($fx, 'EmptyBinary', 3, '')
        $store.Write($script:AudioSettings, 'DisableProtectedAudioDG', 4, '01000000')
        $plan = New-ContourPlan $store $info (Join-Path $BinaryDirectory 'contour-apo.dll') ('A'*64) $caseId
        $journal = New-ContourJournal $plan
        $path = Join-Path $work ($caseId+'.json')
        Save-ContourJournal $journal $path -CreateNew
        $fixture = [pscustomobject]@{Store=$store;Plan=$plan;Journal=$journal;Path=$path;Fx=$fx;
            Baseline=$store.Snapshot($fx);Root=$root}
        $fixtures.Add($fixture)
        return $fixture
    }
    function Assert-Restored($Fixture) {
        Assert ($Fixture.Store.Snapshot($Fixture.Fx) -ceq $Fixture.Baseline) 'Endpoint baseline was not restored byte-for-byte.'
        foreach ($key in $Fixture.Plan.CreateKeys) { Assert (-not $Fixture.Store.Exists($key)) "Created key remains: $key" }
    }

    Case 'SDK registration capture, own identities, no embedded manifest' {
        Assert (-not $info.EmbeddedManifest) 'APO still contains an embedded manifest.'
        Assert ($info.Classes.Count -eq 2) 'Wrong number of classes.'
        for ($i=0; $i -lt 2; ++$i) {
            Assert ($info.Classes[$i].Clsid -ceq $script:ClassIds[$i]) 'Wrong class ID.'
            Assert ($info.Classes[$i].Values.Count -eq 12) 'Unexpected SDK metadata shape.'
            $flags = @($info.Classes[$i].Values | Where-Object Name -eq 'Flags')[0]
            Assert ($flags.Type -eq 4 -and $flags.Data -ceq '0F000000') 'Registration flags differ from DLL.'
        }
    }
    Case 'planning preserves typed bytes and touches no target keys' {
        $f = New-Fixture
        Assert ($f.Plan.Changes.Count -eq 32) 'Unexpected write count.'
        Assert ($f.Plan.CreateKeys.Count -eq 6) 'Unexpected key count.'
        Assert-Restored $f
        $roundTrip = Get-Content -LiteralPath $f.Path -Raw | ConvertFrom-Json
        Assert-ContourBaseline $f.Store $roundTrip.Plan
    }
    Case 'complete install, persisted recovery, and repeat restoration' {
        $f = New-Fixture
        Invoke-ContourApply $f.Store $f.Journal $f.Path
        foreach ($change in $f.Plan.Changes) {
            Assert (Test-RawEqual ($f.Store.Read($change.Path,$change.Name)) $change.After) 'Installed value differs.'
        }
        $journal = Get-Content -LiteralPath $f.Path -Raw | ConvertFrom-Json
        Assert (Invoke-ContourRestore $f.Store $journal $f.Path) 'Restoration failed.'
        Assert-Restored $f
        Assert (Invoke-ContourRestore $f.Store $journal $f.Path) 'Repeat restoration failed.'
        Assert-Restored $f
    }
    Case 'recovery after every key/value mutation (38 interruption points)' {
        for ($point=1; $point -le 38; ++$point) {
            $f = New-Fixture
            Expect-Failure { Invoke-ContourApply $f.Store $f.Journal $f.Path $point } "No interruption at $point"
            $journal = Get-Content -LiteralPath $f.Path -Raw | ConvertFrom-Json
            Assert (Invoke-ContourRestore $f.Store $journal $f.Path) "Recovery failed at $point"
            Assert-Restored $f
        }
    }
    Case 'crash after durable intent but before registry write' {
        $f = New-Fixture
        Expect-Failure { Invoke-ContourApply $f.Store $f.Journal $f.Path 6 } 'Expected stop after keys.'
        $f.Journal.ValueIntents = @(0)
        Save-ContourJournal $f.Journal $f.Path
        $journal = Get-Content -LiteralPath $f.Path -Raw | ConvertFrom-Json
        Assert (Invoke-ContourRestore $f.Store $journal $f.Path) 'Unwritten intent cannot recover.'
        Assert-Restored $f
    }
    Case 'stale baseline aborts before any registration write' {
        $f = New-Fixture
        $f.Store.Write($f.Fx, 'DriverUpdate', 4, '01000000')
        Expect-Failure { Invoke-ContourApply $f.Store $f.Journal $f.Path } 'Stale plan was accepted.'
        foreach ($key in $f.Plan.CreateKeys) { Assert (-not $f.Store.Exists($key)) 'Wrote a key before stale-plan rejection.' }
        Assert ($f.Journal.State -eq 'Prepared') 'Stale plan advanced journal.'
    }
    Case 'rollback preserves conflicting values and unrelated keys' {
        $f = New-Fixture
        Invoke-ContourApply $f.Store $f.Journal $f.Path
        $changed = $f.Plan.Changes[0]
        $f.Store.Write($changed.Path, $changed.Name, 3, 'AABBCC')
        $foreign = $changed.Path+'\ForeignChild'
        $f.Store.Create($foreign)
        $f.Store.Write($f.Fx, 'DriverUpdate', 3, 'FF')
        Assert (-not (Invoke-ContourRestore $f.Store $f.Journal $f.Path)) 'Conflict was not reported.'
        Assert ($f.Journal.State -eq 'RecoveryRequired') 'Wrong conflict state.'
        Assert ($f.Store.Read($changed.Path,$changed.Name).Data -ceq 'AABBCC') 'Foreign value overwritten.'
        Assert ($f.Store.Exists($foreign)) 'Foreign key deleted.'
        Assert ($f.Store.Read($f.Fx,'DriverUpdate').Data -ceq 'FF') 'Unrelated endpoint value overwritten.'
        Assert ($null -eq $f.Store.Read($f.Fx,($script:EffectPrefix+'1'))) 'Owned endpoint assignment was not detached.'
        $f.Store.DeleteValue($changed.Path,$changed.Name)
        [void]$f.Store.DeleteEmpty($foreign)
        $f.Store.DeleteValue($f.Fx,'DriverUpdate')
        Assert (Invoke-ContourRestore $f.Store $f.Journal $f.Path) 'Recovery cannot resume after conflict resolution.'
        Assert-Restored $f
    }
    Case 'refuse existing effects and disabled enhancements' {
        $f = New-Fixture
        $f.Store.Write($f.Fx, ($script:EffectPrefix+'13'), 7, '00000000')
        Expect-Failure { New-ContourPlan $f.Store $info $f.Plan.SourceDll ('A'*64) $f.Plan.Id } 'Composite effect was ignored.'
        $f.Store.DeleteValue($f.Fx,($script:EffectPrefix+'13'))
        $f.Store.Write($f.Fx,$script:DisableEffects,4,'01000000')
        Expect-Failure { New-ContourPlan $f.Store $info $f.Plan.SourceDll ('A'*64) $f.Plan.Id } 'Enhancements were silently enabled.'
    }
    Case 'endpoint conflicts retain class registrations for recovery' {
        $f = New-Fixture
        Invoke-ContourApply $f.Store $f.Journal $f.Path
        $name = $script:EffectPrefix+'2'
        # Semantically the same GUID but different bytes; do not guess ownership.
        $foreignBytes = ConvertTo-HexString $script:ClassIds[1].ToLowerInvariant()
        $f.Store.Write($f.Fx,$name,1,$foreignBytes)
        Assert (-not (Invoke-ContourRestore $f.Store $f.Journal $f.Path)) 'Endpoint conflict was ignored.'
        Assert ($f.Store.Read($f.Fx,$name).Data -ceq $foreignBytes) 'Conflicting endpoint value changed.'
        $registration = $f.Plan.Changes[1]
        Assert (Test-RawEqual ($f.Store.Read($registration.Path,$registration.Name)) $registration.After) 'Class removed while endpoint reference was ambiguous.'
        $f.Store.DeleteValue($f.Fx,$name)
        Assert (Invoke-ContourRestore $f.Store $f.Journal $f.Path) 'Cannot finish endpoint-conflict recovery.'
        Assert-Restored $f
    }
    Case 'reject changed protection setting and unsafe plan/journal scope' {
        $f = New-Fixture
        $f.Store.Write($script:AudioSettings,'DisableProtectedAudioDG',4,'00000000')
        Expect-Failure { Invoke-ContourApply $f.Store $f.Journal $f.Path } 'Protection change was ignored.'
        $f.Journal.Plan.Changes[0].Path = 'SOFTWARE\UnrelatedProduct'
        Expect-Failure { Invoke-ContourRestore $f.Store $f.Journal $f.Path } 'Unexpected write target was accepted.'
        $g = New-Fixture
        $g.Journal.KeyIntents = @('SOFTWARE')
        Expect-Failure { Invoke-ContourRestore $g.Store $g.Journal $g.Path } 'Unexpected key deletion was accepted.'
    }
    Case 'atomic journal replacement and no accidental overwrite' {
        $path = Join-Path $work 'atomic.json'
        [Contour.Deployment.DurableFile]::Save($path,'{"generation":1}',$true)
        Expect-Failure { [Contour.Deployment.DurableFile]::Save($path,'{"generation":2}',$true) } 'CreateNew overwrote a journal.'
        Assert ((Get-Content -LiteralPath $path -Raw | ConvertFrom-Json).generation -eq 1) 'Old journal was damaged.'
        [Contour.Deployment.DurableFile]::Save($path,'{"generation":2}',$false)
        Assert ((Get-Content -LiteralPath $path -Raw | ConvertFrom-Json).generation -eq 2) 'Journal replacement failed.'
    }
    Case 'existing endpoint permits value edits but denies subkey creation' {
        $f = New-Fixture
        $key = $f.Root.OpenSubKey($f.Fx, [Microsoft.Win32.RegistryKeyPermissionCheck]::ReadWriteSubTree,
            [Security.AccessControl.RegistryRights]::ChangePermissions)
        try {
            $acl = $key.GetAccessControl()
            $sid = [Security.Principal.WindowsIdentity]::GetCurrent().User
            $deny = [Security.AccessControl.RegistryAccessRule]::new($sid,
                [Security.AccessControl.RegistryRights]::CreateSubKey,[Security.AccessControl.AccessControlType]::Deny)
            $acl.AddAccessRule($deny)
            $key.SetAccessControl($acl)
        } finally { $key.Dispose() }
        try {
            Assert ($f.Store.CanWrite($f.Fx)) 'Value-edit preflight was rejected.'
            Expect-Failure { $unexpected = $f.Root.OpenSubKey($f.Fx,$true); $unexpected.Dispose() } 'Fixture did not reject broad write access.'
            Invoke-ContourApply $f.Store $f.Journal $f.Path
            Assert (Invoke-ContourRestore $f.Store $f.Journal $f.Path) 'Value-only restoration failed.'
            Assert-Restored $f
        } finally {
            $key = $f.Root.OpenSubKey($f.Fx, [Microsoft.Win32.RegistryKeyPermissionCheck]::ReadWriteSubTree,
                ([Security.AccessControl.RegistryRights]::ChangePermissions -bor [Security.AccessControl.RegistryRights]::ReadPermissions))
            try {
                $restoreAcl = $key.GetAccessControl()
                $restoreAcl.RemoveAccessRuleSpecific($deny)
                $key.SetAccessControl($restoreAcl)
            } finally { $key.Dispose() }
        }
    }
    Write-Output "$script:Passed passed, $script:Failed failed"
    if ($script:Failed -ne 0) { exit 1 }
} catch {
    Write-Error $_ -ErrorAction Continue
    exit 1
} finally {
    foreach ($fixture in $fixtures) { $fixture.Store.Dispose() }
    if ($null -ne $testRoot) {
        $testRoot.Dispose()
        # The deletion is restricted to this run's freshly created test GUID.
        if ($testPath -cne ('Software\ContourDeploymentTest-'+$testId)) { throw 'Invalid cleanup scope.' }
        $userRoot.DeleteSubKeyTree($testPath, $false)
    }
    $userRoot.Dispose()
}
