# Protected machine setup helpers shared by device setup and full removal.
. (Join-Path $PSScriptRoot 'ProductRemoval.ps1')

function Assert-NoLink([string]$Path) {
    $item=Get-Item -LiteralPath $Path -Force
    if ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw 'Setup does not follow redirected package paths.' }
}
function Assert-ProtectedSetup([string]$Path) {
    $acl=Get-Acl -LiteralPath $Path
    $trusted=@('S-1-5-18','S-1-5-32-544')
    if ($trusted -notcontains $acl.GetOwner([Security.Principal.SecurityIdentifier]).Value) { throw 'Setup recovery data has an unexpected owner.' }
    $write=[Security.AccessControl.FileSystemRights]::Write -bor [Security.AccessControl.FileSystemRights]::Delete -bor
        [Security.AccessControl.FileSystemRights]::ChangePermissions -bor [Security.AccessControl.FileSystemRights]::TakeOwnership
    foreach ($rule in $acl.GetAccessRules($true,$true,[Security.Principal.SecurityIdentifier])) {
        if ($rule.AccessControlType -eq 'Allow' -and ($rule.FileSystemRights -band $write) -and $trusted -notcontains $rule.IdentityReference.Value) {
            throw 'Setup recovery data is writable outside administrators and SYSTEM.'
        }
    }
}
function Get-SetupPackages([switch]$IncludeRemoved) {
    $base=[Environment]::GetFolderPath('ProgramFiles')
    Assert-NoLink $base
    foreach ($dir in Get-ChildItem -LiteralPath $base -Directory -Filter 'ContourLab-*') {
        $record=Join-Path $dir.FullName 'device-setup.json'
        if (-not (Test-Path -LiteralPath $record)) { continue }
        Assert-NoLink $dir.FullName; Assert-NoLink $record; Assert-ProtectedSetup $dir.FullName
        $state=Read-SetupJson $record; Assert-SetupState $state $dir.FullName
        # Validate all files we may later read/replace, including recovery data.
        Assert-PackageTree $dir.FullName
        if ($state.State -ne 'Removed' -or $state.AudioRestartPending -or ($IncludeRemoved -and $state.Schema -eq 4)) {
            [pscustomobject]@{Path=$dir.FullName;State=$state}
        }
    }
}
function Assert-PackageTree([string]$Path) {
    foreach ($file in Get-ChildItem -LiteralPath $Path -Force) {
        Assert-NoLink $file.FullName;Assert-ProtectedSetup $file.FullName
        if ($file.PSIsContainer) { Assert-PackageTree $file.FullName }
    }
}
function Test-RuntimeFile([string]$Package,$Build) {
    $plan=(Read-SetupJson (Join-Path $Package 'journal.json')).Plan
    Assert-ContourPlan $plan
    if ($plan.Package -cne $Package) { throw 'Engine files belong to another installation.' }
    $dll=Get-RuntimeBuildPath $plan $Build
    Assert-NoLink $dll;Assert-ProtectedSetup $dll
    if ((Get-FileHash -LiteralPath $dll -Algorithm SHA256).Hash -cne $Build.Sha256) { throw 'The installed engine file changed.' }
    return $true
}
function New-RuntimeCallbacks([string]$Package) {
    $plan=(Read-SetupJson (Join-Path $Package 'journal.json')).Plan
    $checker=Join-Path (Split-Path $PSScriptRoot -Parent) 'contour-endpoint-check.exe'
    @{VerifyBuild={param($Build) Test-RuntimeFile $Package $Build}.GetNewClosure();
        VerifyOutput={param($SelectedEndpoint,$Build)
            $arguments=@('present',$SelectedEndpoint)
            if ($Build.Id -cne 'original') { $arguments+=@(Get-RuntimeBuildPath $plan $Build) }
            else { $arguments+=@('--allow-inactive') }
            $output=& $checker @arguments;$code=$LASTEXITCODE
            if ($code -ne 0) { return [pscustomobject]@{Verified=$false;Inactive=$false} }
            $result=$output | ConvertFrom-Json
            [pscustomobject]@{Verified=($result.verified -is [bool] -and $result.verified);
                Inactive=($null -ne $result.PSObject.Properties['inactive'] -and $result.inactive -eq $true)}
        }.GetNewClosure();CaptureServices=${function:Capture-SetupServices};StopAudio=${function:Stop-SetupAudio};StartAudio=${function:Start-SetupAudio}}
}
function New-RuntimePackage([string]$Package) {
    $source=Split-Path $PSScriptRoot -Parent;$dll=Join-Path $source 'contour-apo.dll'
    $build=[pscustomobject]@{Id=[guid]::NewGuid().ToString('D');Sha256=(Get-FileHash -LiteralPath $dll -Algorithm SHA256).Hash}
    $plan=(Read-SetupJson (Join-Path $Package 'journal.json')).Plan;Assert-ContourPlan $plan
    $catalog=Read-DeploymentState (Join-Path $Package 'endpoints.json');Assert-NoDeploymentHandoff $catalog
    if ((Get-RuntimeRecord $catalog $plan).Active.Sha256 -ceq $build.Sha256) { throw 'This engine build is already installed.' }
    $info=& (Join-Path $source 'contour-deploy-info.exe') $dll --registration-only
    if ($LASTEXITCODE -ne 0) { throw 'Cannot inspect the new audio engine.' }
    $info=$info | ConvertFrom-Json
    if ($info.Schema -ne 1 -or $info.EmbeddedManifest -or @($info.Classes).Count -ne 2) { throw 'Unsupported engine registration.' }
    for ($i=0;$i -lt 2;++$i) {
        $cls=$info.Classes[$i];if ($cls.Clsid -cne $script:ClassIds[$i]) { throw 'Engine class identities changed.' }
        $prefix='SOFTWARE\Classes\AudioEngine\AudioProcessingObjects\'+$cls.Clsid
        $values=@($plan.Changes | Where-Object Path -eq $prefix)
        if ($values.Count -ne @($cls.Values).Count) { throw 'The new engine requires a different registration format.' }
        foreach ($value in $values) {
            $candidate=@($cls.Values | Where-Object Name -ceq $value.Name)
            if ($candidate.Count -ne 1 -or -not (Test-RawEqual $candidate[0] $value.After)) { throw 'The new engine requires a different registration format.' }
        }
    }
    $path=Split-Path (Get-RuntimeBuildPath $plan $build) -Parent
    if (Test-Path -LiteralPath $path) { throw 'Engine staging folder already exists.' }
    [void][IO.Directory]::CreateDirectory($path)
    foreach ($name in @('contour-apo.dll','contour-deploy-info.exe','contour-apo-host.exe','contour-endpoint-check.exe')) {
        Copy-Item -LiteralPath (Join-Path $source $name) -Destination (Join-Path $path $name)
    }
    Copy-Item -LiteralPath (Join-Path $Package 'profile.contour') -Destination (Join-Path $path 'profile.contour')
    $scripts=Join-Path $path 'setup';[void][IO.Directory]::CreateDirectory($scripts)
    foreach ($name in @(Get-SetupScriptNames)) { Copy-Item -LiteralPath (Join-Path $PSScriptRoot $name) -Destination (Join-Path $scripts $name) }
    Assert-PackageTree $Package
    [void](Test-RuntimeFile $Package $build)
    $validation=& (Join-Path $path 'contour-apo-host.exe') (Join-Path $path 'contour-apo.dll')
    $validationCode=$LASTEXITCODE
    [IO.File]::WriteAllLines((Join-Path $path 'validation.log'),[string[]]$validation)
    if ($validationCode -ne 0) { throw 'The new engine failed its checks. Your installed engine was not changed. Details are in the staged engine validation.log.' }
    [void](Test-RuntimeFile $Package $build)
    return $build
}
function Get-SetupScriptNames {
    @('contour-setup.ps1','contour-uninstall.ps1','SetupHost.ps1','ProductRemoval.ps1','DeviceSetup.ps1','AudioPolicy.ps1','SharedSetup.ps1','EndpointDeployment.ps1','RuntimeUpdate.ps1','LegacyMigration.ps1','ControlContext.ps1','Deployment.ps1','EffectPlacement.ps1','RegistryStore.cs')
}
function Set-EngineInspection($Store,$Package,$Report) {
    $plan=(Read-SetupJson (Join-Path $Package.Path 'journal.json')).Plan;Assert-ContourPlan $plan
    $catalog=if ($Package.State.Schema -eq 3) { Read-DeploymentState (Join-Path $Package.Path 'endpoints.json') } else { [pscustomobject]@{Schema=2} }
    $runtime=Get-RuntimeRecord $catalog $plan
    $Report.engineDevices=@(if ($Package.State.Schema -eq 3) {
        foreach ($binding in $catalog.Bindings) { (Get-SharedContext $Package.State $binding.EndpointId).Name }
    } else { $Package.State.Name })
    $Report.installedEngine=$runtime.Active.Sha256
    $Report.canUpdate=$false;$Report.canRecoverUpdate=$false
    if ($null -ne $runtime.Update) {
        $Report.engineState='recovery';$Report.canRecoverUpdate=$true;$Report.canInstall=$false;$Report.canRemove=$false
        $Report.detail='An engine update was interrupted. Recover the previous engine before making further changes.'
        return
    }
    [void](Test-RuntimeFile $Package.Path $runtime.Active)
    $available=(Get-FileHash -LiteralPath (Join-Path (Split-Path $PSScriptRoot -Parent) 'contour-apo.dll') -Algorithm SHA256).Hash
    $Report.availableEngine=$available
    $Report.engineState=if ($available -cne $runtime.Active.Sha256) { 'update_available' } else { 'current' }
    $Report.canUpdate=$Report.engineState -eq 'update_available' -and $Report.state -in @('attached','available') -and
        ($Package.State.Schema -eq 3 -or ($Package.State.Schema -eq 1 -and $Package.State.State -eq 'Attached' -and -not $Package.State.AudioRestartPending))
    if ($Package.State.Schema -eq 3 -and ($null -ne $catalog.Operation -or @($Package.State.Contexts | Where-Object ContextState -eq 'Provisioning').Count)) { $Report.canUpdate=$false }
    if ($runtime.LastError) { $Report.engineDetail=$runtime.LastError }
}
function Read-PackageInfo([string]$SelectedEndpoint=$EndpointId) {
    $binary=Join-Path (Split-Path $PSScriptRoot -Parent) 'contour-deploy-info.exe'
    $dll=Join-Path (Split-Path $PSScriptRoot -Parent) 'contour-apo.dll'
    $output=& $binary $dll $SelectedEndpoint
    if ($LASTEXITCODE -ne 0) { throw 'Cannot inspect this output and the packaged APO.' }
    $output | ConvertFrom-Json
}
function Read-SetupAdapter([string]$SelectedEndpoint) {
    $binary=Join-Path (Split-Path $PSScriptRoot -Parent) 'contour-deploy-info.exe'
    $output=& $binary --device-inventory $SelectedEndpoint
    if ($LASTEXITCODE -ne 0) { throw 'Cannot identify the audio device. Reconnect it and check again.' }
    $inventory=$output | ConvertFrom-Json
    if ($inventory.EndpointId -ine $SelectedEndpoint) { throw 'Device inspection returned another endpoint.' }
    Assert-AdapterId $inventory.AdapterId
    return $inventory.AdapterId
}
function New-SharedCallbacks($Store,[string]$Package) {
    $checker=Join-Path (Split-Path $PSScriptRoot -Parent) 'contour-endpoint-check.exe'
    if (-not (Test-Path -LiteralPath $checker -PathType Leaf)) { throw 'Device verification files are missing. Rebuild Contour.' }
    $verify={param($SelectedEndpoint,$Registration,$Binding)
        Assert-SharedRegistration $Store $Package $Registration
        $plan=(Read-SetupJson (Join-Path $Package 'journal.json')).Plan
        $catalog=Read-DeploymentState (Join-Path $Package 'endpoints.json')
        [void](Test-RuntimeFile $Package (Get-RuntimeRecord $catalog $plan).Active)
        $true
    }.GetNewClosure()
    $applied={param($SelectedEndpoint,$Registration,$Binding)
        $mode=if ($null -eq $Binding) { 'absent' } else { 'present' }
        $arguments=@($mode,$SelectedEndpoint)
        $plan=(Read-SetupJson (Join-Path $Package 'journal.json')).Plan
        $catalog=Read-DeploymentState (Join-Path $Package 'endpoints.json');$build=(Get-RuntimeRecord $catalog $plan).Active
        if ($null -ne $Binding -and $build.Id -cne 'original') { $arguments+=@(Get-RuntimeBuildPath $plan $build) }
        $result=& $checker @arguments
        if ($LASTEXITCODE -ne 0) { return $false }
        $report=$result | ConvertFrom-Json
        return ($report.verified -is [bool] -and $report.verified)
    }.GetNewClosure()
    @{VerifyRegistration=$verify;VerifyApplied=$applied;CaptureServices=${function:Capture-SetupServices};
        StopAudio=${function:Stop-SetupAudio};StartAudio=${function:Start-SetupAudio}}
}
function Test-InstalledSetup([string]$SelectedEndpoint) {
    $checker=Join-Path (Split-Path $PSScriptRoot -Parent) 'contour-endpoint-check.exe'
    $result=& $checker present $SelectedEndpoint
    if ($LASTEXITCODE -ne 0) { return $false }
    $report=$result | ConvertFrom-Json
    return ($report.verified -is [bool] -and $report.verified)
}
function Set-SharedInspection($Store,$Package,$Report) {
    $record=$Package.State;$catalog=Read-DeploymentState (Join-Path $Package.Path 'endpoints.json')
    Assert-SharedCatalog $catalog $record -AllowRuntimeUpdate
    $Report.package=$Package.Path
    if ($catalog.Schema -eq 4 -and $null -ne $catalog.Runtime.Update) {
        $Report.state='recovery';$Report.installation=if ($null -ne (Get-Binding $catalog $EndpointId)) { 'incomplete' } else { 'not_installed' }
        $Report.reason='engine_update';return
    }
    if ($null -ne $catalog.Operation -and $catalog.Operation.EndpointId -ieq $EndpointId) {
        $Report.state='recovery';$Report.installation='incomplete';$Report.canRemove=$true
        $Report.detail='An interrupted device operation needs recovery. Uninstall Contour from the affected device before continuing.'
        return
    }
    $binding=Get-Binding $catalog $EndpointId
    $context=Get-SharedContext $record $EndpointId
    if ($null -ne $binding) {
        $Report.canRemove=$true
        try {
            Assert-SharedRegistration $Store $Package.Path (Get-Registration $catalog $record.Id)
            Assert-EndpointAssignments $Store $catalog $EndpointId $binding
            [void](Get-EndpointSettings $Store $EndpointId)
            $Report.state='attached';$Report.installation='installed';$Report.detail='Contour is installed on this device.'
        } catch { $Report.state='recovery';$Report.installation='incomplete';$Report.detail=$_.Exception.Message }
    } elseif ($null -ne $context -and $context.ContextState -eq 'Provisioning') {
        $Report.state='recovery';$Report.installation='incomplete';$Report.canRemove=$true
        $Report.detail='Installation was interrupted. Uninstall it from this device before trying again.'
    } else {
        [void](Read-PackageInfo)
        Assert-SharedRegistration $Store $Package.Path (Get-Registration $catalog $record.Id)
        $placement=Get-AvailableEffectPlacement $Store $EndpointId
        Assert-EndpointAssignments $Store $catalog $EndpointId $null $placement
        [void](New-AudioPolicyJournal $Store)
        if ($null -ne $context -and $context.ContextState -eq 'Ready') { [void](Get-EndpointSettings $Store $EndpointId) }
        else { [void](New-ControlContextPlan $Store $EndpointId) }
        $Report.canInstall=$true
        if ($null -ne $placement) { $Report.detail='Ready to install. Existing device effects will be kept.' }
    }
    if ($null -ne $catalog.Operation) {
        # Another endpoint owns the interrupted transaction. Keep this row's
        # actual installation status, but serialize all setup changes.
        $Report.canInstall=$false;$Report.canRemove=$false
        $Report.detail='Another device has an interrupted installation. Uninstall Contour from that device before making further installation changes.'
    }
}
function New-SetupPlan($Store) {
    $root=Split-Path $PSScriptRoot -Parent
    foreach ($name in @('contour-apo.dll','contour-deploy-info.exe','contour-apo-host.exe','contour-endpoint-check.exe')) {
        if (-not (Test-Path -LiteralPath (Join-Path $root $name))) { throw 'The setup package is incomplete. Rebuild the Contour target.' }
    }
    $dll=Join-Path $root 'contour-apo.dll'
    $hash=(Get-FileHash -LiteralPath $dll -Algorithm SHA256).Hash
    $info=Read-PackageInfo
    if ((Get-FileHash -LiteralPath $dll -Algorithm SHA256).Hash -cne $hash) { throw 'The APO changed during inspection. Check again after the build finishes.' }
    [void](New-AudioPolicyJournal $Store)
    $plan=New-ContourPlan $Store $info $dll $hash ([guid]::NewGuid().ToString('D'))
    [void](New-ControlContextPlan $Store $EndpointId)
    return $plan
}
function New-SetupPackage($Plan) {
    $path=$Plan.Package
    if (Test-Path -LiteralPath $path) { throw 'The setup package already exists.' }
    $acl=[Security.AccessControl.DirectorySecurity]::new()
    $acl.SetAccessRuleProtection($true,$false)
    foreach ($spec in @(@('S-1-5-18','FullControl'),@('S-1-5-32-544','FullControl'),@('S-1-5-32-545','ReadAndExecute'))) {
        $rule=[Security.AccessControl.FileSystemAccessRule]::new([Security.Principal.SecurityIdentifier]::new($spec[0]),$spec[1],'ContainerInherit,ObjectInherit','None','Allow')
        $acl.AddAccessRule($rule)
    }
    $acl.SetOwner([Security.Principal.SecurityIdentifier]::new('S-1-5-32-544'))
    [void][IO.Directory]::CreateDirectory($path,$acl)
    $source=Split-Path $PSScriptRoot -Parent
    foreach ($name in @('contour-apo.dll','contour-deploy-info.exe','contour-apo-host.exe','contour-endpoint-check.exe')) {
        Copy-Item -LiteralPath (Join-Path $source $name) -Destination (Join-Path $path $name)
    }
    # Retain self-contained recovery alongside the DLL. Installed files are
    # immutable; a rebuild in the workspace never overwrites a loaded APO.
    $scripts=Join-Path $path 'setup'; [void][IO.Directory]::CreateDirectory($scripts)
    foreach ($name in @(Get-SetupScriptNames)) {
        Copy-Item -LiteralPath (Join-Path $PSScriptRoot $name) -Destination (Join-Path $scripts $name)
    }
    if ((Get-FileHash -LiteralPath $Plan.TargetDll -Algorithm SHA256).Hash -cne $Plan.DllSha256) { throw 'The APO changed while it was staged. No device settings were changed.' }
    & (Join-Path $path 'contour-apo-host.exe') $Plan.TargetDll | Out-Null
    if ($LASTEXITCODE -ne 0) { throw 'The staged APO failed its contract tests. No device settings were changed.' }
    # Neutral fallback also covers a failed settings-store open: never inherit
    # the original lab's -6 dB fallback on a newly configured output.
    $neutral="CONTOUR 1 $($Plan.Endpoint.SampleRate) $($Plan.Endpoint.Channels)`nBypass 0`nEnd`n"
    [Contour.Deployment.DurableFile]::Save((Join-Path $path 'profile.contour'),$neutral,$true)
    Save-ContourJournal (New-ContourJournal $Plan) (Join-Path $path 'journal.json') -CreateNew
    $state=New-SetupState $Plan
    Save-SetupState $state $path
    return $state
}
function Capture-SetupServices {
    $audio=Get-Service Audiosrv
    if ($audio.Status -ne 'Running') { throw 'Start Windows Audio before setting up this output.' }
    $seen=[Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    function Visit($Service) {
        foreach ($child in $Service.DependentServices) {
            if ($child.Status -eq 'Running' -and $seen.Add($child.Name)) { Visit $child; $child.Name }
        }
    }
    Visit $audio
    'Audiosrv'
}
function Stop-SetupAudio($Names) {
    foreach ($name in $Names) {
        $service=Get-Service -Name $name
        if ($service.Status -ne 'Stopped') { $service.Stop(); $service.WaitForStatus('Stopped',[TimeSpan]::FromSeconds(15)) }
    }
}
function Start-SetupAudio($Names) {
    $errors=@(); $list=@($Names); [array]::Reverse($list)
    foreach ($name in $list) {
        try {
            $service=Get-Service -Name $name
            if ($service.Status -ne 'Running') { $service.Start(); $service.WaitForStatus('Running',[TimeSpan]::FromSeconds(15)) }
        } catch { $errors+=@($_.Exception.Message) }
    }
    if ($errors.Count) { throw ($errors -join '; ') }
}
