param([Parameter(Mandatory=$true)][string] $WorkDirectory, [switch] $ExerciseRestorePrivilege)
Set-StrictMode -Version Latest
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot '..\tools\deployment\ControlContext.ps1')
function Assert($Condition,[string]$Message) { if (-not $Condition) { throw $Message } }
function Expect-Failure([scriptblock]$Action) { $failed=$false; try { & $Action } catch { $failed=$true }; Assert $failed 'Expected operation to be rejected.' }
$script:Passed=0; $script:Failed=0
function Case([string]$Name,[scriptblock]$Body) {
    try { & $Body; ++$script:Passed; Write-Output "PASS $Name" }
    catch { ++$script:Failed; Write-Output "FAIL $Name : $($_.Exception.Message)"; Write-Output $_.ScriptStackTrace }
}
$testId=[guid]::NewGuid().ToString('D')
$testPath='Software\ContourContextTest-'+$testId
$work=Join-Path ([IO.Path]::GetFullPath($WorkDirectory)) $testId
[void][IO.Directory]::CreateDirectory($work)
$userRoot=[Microsoft.Win32.RegistryKey]::OpenBaseKey([Microsoft.Win32.RegistryHive]::CurrentUser,[Microsoft.Win32.RegistryView]::Registry64)
$testRoot=$null
$fixtures=[Collections.Generic.List[object]]::new()
try {
    $testRoot=$userRoot.CreateSubKey($testPath)
    function New-Fixture {
        $caseId=[guid]::NewGuid().ToString('D'); $root=$testRoot.CreateSubKey($caseId)
        $endpoint='{0.0.0.00000000}.{d673b998-8476-4faa-af56-e3eec4688246}'
        $fx=(Get-EndpointPath $endpoint)+'\FxProperties'
        $key=$root.CreateSubKey($fx+'\ExistingContext\Default'); $key.SetValue('Keep','original'); $key.Dispose()
        $store=[Contour.Deployment.RegistryStore]::new($root,$true)
        $plan=New-ControlContextPlan $store $endpoint
        $journal=New-ControlContextJournal $plan; $path=Join-Path $work ($caseId+'.json')
        Save-ContourJournal $journal $path -CreateNew
        $f=[pscustomobject]@{Root=$root;Store=$store;Plan=$plan;Journal=$journal;Path=$path;Fx=$fx}
        $fixtures.Add($f); return $f
    }
    function Restored($f, [switch] $AfterAclFixture) {
        Assert ($f.Store.Snapshot($f.Fx) -ceq $f.Plan.Baseline) 'Endpoint contents differ from baseline.'
        $actualSecurity=$f.Store.Security($f.Fx)
        $expectedSecurity=$f.Plan.ParentSecurity
        if ($AfterAclFixture) {
            # Windows marks the DACL auto-inherited after the fixture itself
            # writes/restores its deny rule. ACEs, owner, and group must match.
            # Production apply/restore checks above remain exact SDDL checks.
            $actualSecurity=$actualSecurity.Replace('D:AI(','D:(')
            $expectedSecurity=$expectedSecurity.Replace('D:AI(','D:(')
        }
        Assert ($actualSecurity -ceq $expectedSecurity) 'Existing endpoint owner/DACL changed.'
    }
    Case 'provision, runtime data cleanup, persisted recovery, and idempotent restoration' {
        $f=New-Fixture; Invoke-ControlContextApply $f.Store $f.Journal $f.Path
        foreach ($name in @(($script:ControlContextId+',1'),($script:ControlContextId+',2'),($script:ControlContextId+',5'))) { $f.Store.Write($f.Plan.Keys[3],$name,3,'0001FF7F') }
        $f.Journal.SavedProfileCleanup=$true; Save-ContourJournal $f.Journal $f.Path
        $f.Store.Write($f.Plan.Keys[2],($script:ControlContextId+',4'),3,'010203')
        $journal=Get-Content -LiteralPath $f.Path -Raw | ConvertFrom-Json
        Assert (Invoke-ControlContextRestore $f.Store $journal $f.Path) 'Restoration failed.'
        Restored $f
        Assert (Invoke-ControlContextRestore $f.Store $journal $f.Path) 'Repeat restoration failed.'
        Restored $f
    }
    Case 'recovery after every provisioning mutation' {
        for ($point=1; $point -le 6; ++$point) {
            $f=New-Fixture
            Expect-Failure { Invoke-ControlContextApply $f.Store $f.Journal $f.Path -FailAfter $point }
            $journal=Get-Content -LiteralPath $f.Path -Raw | ConvertFrom-Json
            Assert (Invoke-ControlContextRestore $f.Store $journal $f.Path) "Recovery failed after mutation $point."
            Restored $f
        }
    }
    Case 'unknown values and changed provisioning metadata are preserved' {
        $f=New-Fixture; Invoke-ControlContextApply $f.Store $f.Journal $f.Path
        $f.Store.Write($f.Plan.Keys[3],'ForeignData',3,'AABB')
        $f.Store.Write($f.Plan.Keys[2],($script:ControlContextId+',4'),3,'010203')
        $value=$f.Plan.Values[1]; $f.Store.Write($value.Path,$value.Name,4,'02000000')
        Assert (-not (Invoke-ControlContextRestore $f.Store $f.Journal $f.Path)) 'Conflicting data was silently removed.'
        Assert ($f.Store.Read($f.Plan.Keys[3],'ForeignData').Data -ceq 'AABB') 'Unknown value was lost.'
        Assert ($f.Store.Read($value.Path,$value.Name).Data -ceq '02000000') 'Changed metadata was lost.'
        Assert ($f.Store.Read($f.Plan.Keys[2],($script:ControlContextId+',4')).Data -ceq '010203') 'Saved data without explicit test ownership was lost.'
    }
    Case 'stale baseline, existing context, and malformed recovery scope are rejected' {
        $f=New-Fixture; $f.Store.Write($f.Fx,'NewDriverSetting',4,'01000000')
        Expect-Failure { Invoke-ControlContextApply $f.Store $f.Journal $f.Path }
        Assert (-not $f.Store.Exists($f.Plan.Keys[0])) 'Stale plan created a key.'
        $f=New-Fixture; Invoke-ControlContextApply $f.Store $f.Journal $f.Path
        Expect-Failure { New-ControlContextPlan $f.Store $f.Plan.EndpointId }
        $f.Journal.RuntimeIntents=@('ForeignData')
        Expect-Failure { Invoke-ControlContextRestore $f.Store $f.Journal $f.Path }
        Assert ($f.Store.Exists($f.Plan.Keys[3])) 'Invalid journal removed context.'
        $f=New-Fixture; $f.Journal.SavedProfileCleanup=$true
        Expect-Failure { Invoke-ControlContextRestore $f.Store $f.Journal $f.Path }
        $f.Journal.SavedProfileCleanup='true'
        Expect-Failure { Invoke-ControlContextRestore $f.Store $f.Journal $f.Path }
    }
    Case 'native privileged-operation allowlist rejects unrelated paths and missing parents' {
        $f=New-Fixture
        Expect-Failure { $f.Store.CreateControlContextKey($f.Fx+'\ForeignContext',$true) }
        Expect-Failure { $f.Store.CreateControlContextKey($f.Plan.Keys[3],$true) }
        Restored $f
    }
    if ($ExerciseRestorePrivilege) {
        Case 'scoped restore privilege creates context below a denied parent without changing that ACL' {
            $f=New-Fixture
            $key=$f.Root.OpenSubKey($f.Fx,[Microsoft.Win32.RegistryKeyPermissionCheck]::ReadWriteSubTree,
                ([Security.AccessControl.RegistryRights]::ChangePermissions -bor [Security.AccessControl.RegistryRights]::ReadPermissions))
            $changed=$key.GetAccessControl()
            $deny=[Security.AccessControl.RegistryAccessRule]::new([Security.Principal.WindowsIdentity]::GetCurrent().User,
                [Security.AccessControl.RegistryRights]::CreateSubKey,[Security.AccessControl.AccessControlType]::Deny)
            try {
                $changed.AddAccessRule($deny); $key.SetAccessControl($changed)
                $plan=New-ControlContextPlan $f.Store $f.Plan.EndpointId
                $journal=New-ControlContextJournal $plan
                Expect-Failure { $f.Store.CreateControlContextKey($plan.Keys[0],$false) }
                Invoke-ControlContextApply $f.Store $journal $f.Path -AllowRestorePrivilege
                Assert ($f.Store.Security($f.Fx) -ceq $plan.ParentSecurity) 'Protected parent permissions were rewritten.'
                Assert (Invoke-ControlContextRestore $f.Store $journal $f.Path) 'Privileged context cleanup failed.'
                Assert ($f.Store.Security($f.Fx) -ceq $plan.ParentSecurity) 'Recovery changed parent permissions.'
            } finally {
                $restore=$key.GetAccessControl()
                $restore.SetSecurityDescriptorSddlForm($f.Plan.ParentSecurity,[Security.AccessControl.AccessControlSections]::Access)
                $key.SetAccessControl($restore); $key.Dispose()
            }
            Restored $f -AfterAclFixture
        }
    }
    Write-Output "$script:Passed passed, $script:Failed failed"
    if ($script:Failed -ne 0) { exit 1 }
} finally {
    foreach ($f in $fixtures) { $f.Store.Dispose() }
    if ($null -ne $testRoot) {
        $testRoot.Dispose()
        if ($testPath -cne ('Software\ContourContextTest-'+$testId)) { throw 'Invalid fixture cleanup scope.' }
        $userRoot.DeleteSubKeyTree($testPath,$false)
    }
    $userRoot.Dispose()
}
