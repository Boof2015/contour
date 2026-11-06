# Run elevated only after reviewing the endpoint plan. Always attempts journal
# restoration, even if installation, measurement, or a graph rebuild fails.
[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string] $PlanFile,
    [Parameter(Mandatory=$true)][string] $OutputDirectory,
    [switch] $PeqTest,
    [string] $ControlPlanFile = '',
    [switch] $SaveDuringPreview
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'Deployment.ps1')
. (Join-Path $PSScriptRoot 'ControlContext.ps1')
$controlTest = -not [string]::IsNullOrWhiteSpace($ControlPlanFile)
if ($controlTest) { $PeqTest = $true }
if ($SaveDuringPreview -and -not $controlTest) { throw 'SaveDuringPreview requires a context plan.' }

function Invoke-ExperimentChild([string] $Executable, [string[]] $Arguments, [string] $Label, [int] $TimeoutSeconds = 30) {
    # Arguments here are paths/IDs produced by this experiment, never shell code.
    foreach ($argument in $Arguments) {
        if ($argument.Contains('"') -or $argument.Contains("`n")) { throw 'Invalid child argument.' }
    }
    $quoted = @($Arguments | ForEach-Object { '"'+$_+'"' })
    $stdout = Join-Path $OutputDirectory ($Label+'.stdout.log')
    $stderr = Join-Path $OutputDirectory ($Label+'.stderr.log')
    $process = Start-Process -FilePath $Executable -ArgumentList $quoted -WindowStyle Hidden -PassThru -RedirectStandardOutput $stdout -RedirectStandardError $stderr
    # Cache the process handle before it exits, for reliable ExitCode retrieval.
    $null = $process.Handle
    if (-not $process.WaitForExit($TimeoutSeconds*1000)) {
        $process.Kill()
        $process.WaitForExit()
        throw "$Label timed out after $TimeoutSeconds seconds."
    }
    $process.Refresh()
    Get-Content -LiteralPath $stdout | Write-Output
    $errors = Get-Content -LiteralPath $stderr -Raw
    if ($errors) { Write-Output $errors }
    if ($process.ExitCode -ne 0) { throw "$Label failed with exit code $($process.ExitCode)." }
}

function Invoke-WithStoppedAudio([scriptblock] $Action) {
    $audio = Get-Service -Name Audiosrv
    if ($audio.Status -ne 'Running') { throw 'Windows Audio was not running before the requested rebuild.' }
    $dependents = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    function Remember-RunningDependents($Service) {
        foreach ($child in $Service.DependentServices) {
            if ($child.Status -eq 'Running' -and $dependents.Add($child.Name)) { Remember-RunningDependents $child }
        }
    }
    Remember-RunningDependents $audio
    Write-Output ('Rebuilding Windows Audio; active dependents: '+($dependents -join ', '))
    try {
        Stop-Service -Name Audiosrv -Force -ErrorAction Stop
        (Get-Service Audiosrv).WaitForStatus('Stopped',[TimeSpan]::FromSeconds(15))
        & $Action
    }
    finally {
        Start-Service -Name Audiosrv -ErrorAction Stop
        (Get-Service Audiosrv).WaitForStatus('Running',[TimeSpan]::FromSeconds(15))
        foreach ($name in $dependents) {
            Start-Service -Name $name -ErrorAction Stop
            (Get-Service $name).WaitForStatus('Running',[TimeSpan]::FromSeconds(15))
        }
    }
}
function Restart-ExperimentAudio { Invoke-WithStoppedAudio {} }

function Invoke-WithDeploymentLock([scriptblock] $Action) {
    $mutex = [Threading.Mutex]::new($false,'Global\ContourLabDeployment')
    $owns = $false
    try {
        try { $owns = $mutex.WaitOne(0) } catch [Threading.AbandonedMutexException] { $owns = $true }
        if (-not $owns) { throw 'Another Contour deployment is active.' }
        & $Action
    } finally { if ($owns) { $mutex.ReleaseMutex() }; $mutex.Dispose() }
}

function Measure-Experiment([string] $Label) {
    $report = Join-Path $OutputDirectory ($Label+'.json')
    Invoke-ExperimentChild $verifier @('measure', $plan.Endpoint.Id, $report) $Label 15
    $measurement = Get-Content -LiteralPath $report -Raw | ConvertFrom-Json
    if (-not $measurement.Valid) { throw "$Label measurement was invalid." }
}

$transcript = $false
$traceStarted = $false
$contextStore = $null
$contextJournal = $null
$outcome = [ordered]@{Schema=1;Passed=$false;Restored=$false;Errors=@();Measurements=@();LoadedModules=@()}
try {
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = [Security.Principal.WindowsPrincipal]::new($identity)
    if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) { throw 'The live experiment requires Windows elevation.' }
    $PlanFile = [IO.Path]::GetFullPath($PlanFile)
    $OutputDirectory = [IO.Path]::GetFullPath($OutputDirectory)
    if (Test-Path -LiteralPath $OutputDirectory) { throw 'Use a new output directory for each experiment.' }
    [void][IO.Directory]::CreateDirectory($OutputDirectory)
    Start-Transcript -Path (Join-Path $OutputDirectory 'experiment.log') | Out-Null
    $transcript = $true
    $plan = Get-Content -LiteralPath $PlanFile -Raw -Encoding UTF8 | ConvertFrom-Json
    Assert-ContourPlan $plan
    $outcome.PlanId = $plan.Id
    $outcome.Endpoint = $plan.Endpoint.Id
    $outcome.DllSha256 = $plan.DllSha256
    $outcome.ProtectedAudioOverride = $plan.ProtectedAudioOverride
    $verifier = Join-Path (Split-Path -Parent $plan.SourceDll) 'contour-audio-test.exe'
    $inspector = Join-Path (Split-Path -Parent $plan.SourceDll) 'contour-eq-inspect.exe'
    $powershell = Join-Path $PSHOME 'powershell.exe'
    $installer = Join-Path $PSScriptRoot 'contour-lab.ps1'
    $journalPath = Join-Path $plan.Package 'journal.json'
    if ($controlTest) {
        $contextPlan = Get-Content -LiteralPath $ControlPlanFile -Raw | ConvertFrom-Json
        Assert-ControlContextPlan $contextPlan
        if ($contextPlan.EndpointId -cne $plan.Endpoint.Id) { throw 'Context and APO plans target different endpoints.' }
        $root = [Microsoft.Win32.RegistryKey]::OpenBaseKey([Microsoft.Win32.RegistryHive]::LocalMachine,[Microsoft.Win32.RegistryView]::Registry64)
        $contextStore = [Contour.Deployment.RegistryStore]::new($root,$true)
        Assert-ContourBaseline $contextStore $plan
        $contextJournalPath = Join-Path $OutputDirectory 'context-journal.json'
        $outcome.ControlTest = $true
    }
    $initialExpected = @(-6.0,-6.0)
    if ($PeqTest) {
        # These repository fixtures place each channel's peak at its test-tone
        # frequency. The All layer adds gain, giving exact center responses.
        $fixtures = Join-Path (Split-Path -Parent (Split-Path -Parent $PSScriptRoot)) 'tests\fixtures\peq'
        $initialProfile = Join-Path $OutputDirectory 'initial.contour'
        $updatedProfile = Join-Path $OutputDirectory 'updated.contour'
        $bypassProfile = Join-Path $OutputDirectory 'bypass.contour'
        Invoke-ExperimentChild $inspector @('--apply',(Join-Path $fixtures 'live-initial.contour'),$initialProfile) 'prepare-initial'
        Invoke-ExperimentChild $inspector @('--apply',(Join-Path $fixtures 'live-updated.contour'),$updatedProfile) 'prepare-updated'
        Invoke-ExperimentChild $inspector @('--bypass','on',$updatedProfile,$bypassProfile) 'prepare-bypass'
        $initialExpected = @(-8.0,-5.0)
        $outcome.PeqTest = $true
    }
    Measure-Experiment 'baseline'
    $traceName = 'ContourApo-'+$plan.Id
    $traceFile = Join-Path $OutputDirectory 'apo.etl'
    $logman = Join-Path $env:SystemRoot 'System32\logman.exe'
    Invoke-ExperimentChild $logman @('create','trace',$traceName,'-p',
        '{A4E1577A-45ED-4E67-B1C2-6848125BA8C9}','0xffffffffffffffff','5',
        '-o',$traceFile,'-ets') 'start-trace'
    $traceStarted = $true
    try {
        if ($controlTest) {
            $contextJournal = New-ControlContextJournal $contextPlan
            Save-ContourJournal $contextJournal $contextJournalPath -CreateNew
            Invoke-WithDeploymentLock { Invoke-ControlContextApply $contextStore $contextJournal $contextJournalPath -AllowRestorePrivilege }
            $contextJournal.SavedProfileCleanup=$true
            Save-ContourJournal $contextJournal $contextJournalPath
            [Contour.Deployment.DurableFile]::Save((Join-Path $OutputDirectory 'save-ready.json'),'{"Ready":true}',$true)
            $deadline=[DateTime]::UtcNow.AddSeconds(180)
            while (-not (Test-Path -LiteralPath (Join-Path $OutputDirectory 'save-finish'))) {
                if ([DateTime]::UtcNow -ge $deadline) { throw 'Ordinary-user initial Save window expired; restoring.' }
                Start-Sleep -Milliseconds 250
            }
            $seed=Get-Content -LiteralPath (Join-Path $OutputDirectory 'initial-save.json') -Raw | ConvertFrom-Json
            if (-not $seed.Saved -or -not $seed.Verified -or $seed.Elevated -or $seed.Restricted) { throw 'Expected a verified ordinary-user initial Save.' }
            # Reinspect after provisioning so the APO install guards include the
            # context. Keep the reviewed ID, binary and exact registration writes.
            $info = & (Join-Path (Split-Path -Parent $plan.SourceDll) 'contour-deploy-info.exe') $plan.SourceDll $plan.Endpoint.Id | ConvertFrom-Json
            if ($LASTEXITCODE -ne 0) { throw 'Deployment reinspection failed.' }
            $refreshed = New-ContourPlan $contextStore $info $plan.SourceDll $plan.DllSha256 $plan.Id
            if (($refreshed.Changes | ConvertTo-Json -Depth 12 -Compress) -cne ($plan.Changes | ConvertTo-Json -Depth 12 -Compress)) { throw 'APO registration changed after context provisioning.' }
            $plan = $refreshed
            $PlanFile = Join-Path $OutputDirectory 'attachment-plan.json'
            [Contour.Deployment.DurableFile]::Save($PlanFile,($plan | ConvertTo-Json -Depth 30),$true)
        }
        $installArguments = @('-NoProfile','-ExecutionPolicy','RemoteSigned','-File',$installer,
            '-Action','Install','-File',$PlanFile,'-UseExistingUnprotectedAudio')
        if ($PeqTest -and -not $controlTest) { $installArguments += @('-InitialProfile',$initialProfile) }
        Invoke-ExperimentChild $powershell $installArguments 'install'
        if ($PeqTest) {
            $installedProfile = Join-Path $plan.Package 'profile.contour'
            if (-not $controlTest) { $initialHash = (Get-FileHash -LiteralPath $installedProfile -Algorithm SHA256).Hash }
        }
        Restart-ExperimentAudio
        Measure-Experiment 'attached'
        if ($controlTest) {
            [Contour.Deployment.DurableFile]::Save((Join-Path $OutputDirectory 'ready.json'),'{"Ready":true}',$true)
            $deadline = [DateTime]::UtcNow.AddSeconds(180)
            while (-not (Test-Path -LiteralPath (Join-Path $OutputDirectory 'finish'))) {
                if ([DateTime]::UtcNow -ge $deadline) { throw 'Ordinary-user preview test window expired; restoring.' }
                Start-Sleep -Milliseconds 250
            }
            $live = Get-Content -LiteralPath (Join-Path $OutputDirectory 'live-updates.json') -Raw | ConvertFrom-Json
            if (-not $live.Valid -or $live.Control.Elevated -or $live.Control.Restricted -or
                -not $live.Control.SourceProfileUnchanged -or -not $live.Control.SavedSettingsVerified -or
                $live.Control.SavedUpdate -ne $SaveDuringPreview.IsPresent) { throw 'Expected a successful ordinary-user preview/save test.' }
            $outcome.SaveDuringPreview=$SaveDuringPreview.IsPresent
            $outcome.SavedSettingsVerified=$live.Control.SavedSettingsVerified
            $outcome.NoLegacyProfileFile=-not (Test-Path -LiteralPath $installedProfile)
            if (-not $outcome.NoLegacyProfileFile) { throw 'Settings experiment unexpectedly has a legacy profile file.' }
        } elseif ($PeqTest) {
            Invoke-ExperimentChild $verifier @('measure-updates',$plan.Endpoint.Id,
                (Join-Path $OutputDirectory 'live-updates.json'),$installedProfile,$updatedProfile,$bypassProfile) 'live-updates' 25
            Invoke-ExperimentChild $inspector @('--apply',$initialProfile,$installedProfile) 'restore-initial-profile'
        }
        foreach ($audioProcess in @(Get-Process -Name audiodg -ErrorAction SilentlyContinue)) {
            try {
                foreach ($module in $audioProcess.Modules) {
                    if ($module.FileName -ieq $plan.TargetDll) {
                        $outcome.LoadedModules += [pscustomobject]@{ProcessId=$audioProcess.Id;Path=$module.FileName}
                    }
                }
            } catch { Write-Output "Module inspection unavailable for process $($audioProcess.Id): $($_.Exception.Message)" }
        }
        Restart-ExperimentAudio
        Measure-Experiment 'attached-after-rebuild'
        if ($controlTest) {
            $expectedSaved=if ($SaveDuringPreview) { $updatedProfile } else { $initialProfile }
            Invoke-ExperimentChild (Join-Path (Split-Path -Parent $plan.SourceDll) 'contour-control-probe.exe') @(
                'verify-saved',$plan.Endpoint.Id,$expectedSaved,(Join-Path $OutputDirectory 'saved-after-rebuild.json')) 'verify-saved-after-rebuild'
        }
    } finally {
        if ((Test-Path -LiteralPath $journalPath) -or $null -ne $contextJournal) {
            # Stop workers before removing the volatile request/status values.
            Invoke-WithStoppedAudio {
                if (Test-Path -LiteralPath $journalPath) {
                    Invoke-ExperimentChild $powershell @('-NoProfile','-ExecutionPolicy','RemoteSigned','-File',
                        (Join-Path $plan.Package 'contour-lab.ps1'),'-Action','Restore','-File',$journalPath) 'restore'
                    $journal = Get-Content -LiteralPath $journalPath -Raw | ConvertFrom-Json
                    $outcome.Restored = $journal.State -eq 'RestoredPendingGraphRebuild'
                }
                if ($null -ne $contextJournal) {
                    Invoke-WithDeploymentLock {
                        $outcome.ContextRestored = Invoke-ControlContextRestore $contextStore $contextJournal $contextJournalPath
                        if (-not $outcome.ContextRestored) { throw ('Context restoration conflict: '+($contextJournal.Conflicts -join '; ')) }
                        $fx = (Get-EndpointPath $contextPlan.EndpointId)+'\FxProperties'
                        $outcome.BaselineMatches = $contextStore.Snapshot($fx) -ceq $contextPlan.Baseline
                        $outcome.ParentSecurityMatches = $contextStore.Security($fx) -ceq $contextPlan.ParentSecurity
                        $outcome.ProtectedAudioOverrideUnchanged = Test-RawEqual ($contextStore.Read($script:AudioSettings,'DisableProtectedAudioDG')) $plan.ProtectedAudioOverride
                        if (-not $outcome.BaselineMatches -or -not $outcome.ParentSecurityMatches -or -not $outcome.ProtectedAudioOverrideUnchanged) { throw 'Endpoint baseline, permissions, or protected-audio setting changed.' }
                    }
                }
            }
            Measure-Experiment 'restored'
        }
    }
    $baseline = Get-Content -LiteralPath (Join-Path $OutputDirectory 'baseline.json') -Raw | ConvertFrom-Json
    $passed = $outcome.Restored
    foreach ($label in @('attached','attached-after-rebuild','restored')) {
        $measurement = Get-Content -LiteralPath (Join-Path $OutputDirectory ($label+'.json')) -Raw | ConvertFrom-Json
        if ($measurement.Endpoint -cne $baseline.Endpoint -or $measurement.SampleRate -ne $baseline.SampleRate -or
            $measurement.EndpointVolumeBefore -ne $baseline.EndpointVolumeBefore -or $measurement.SessionVolume -ne $baseline.SessionVolume) {
            throw 'Endpoint, format, or volume changed between measurements.'
        }
        $expected = if ($label -eq 'restored') { @(0.0,0.0) }
            elseif ($label -eq 'attached-after-rebuild' -and $SaveDuringPreview) { @(-4.0,-10.0) }
            else { $initialExpected }
        $deltas = @()
        for ($channel=0; $channel -lt 2; ++$channel) {
            $delta = 20 * [Math]::Log10($measurement.Channels[$channel].Amplitude / $baseline.Channels[$channel].Amplitude)
            $deltas += $delta
            if ([Math]::Abs($delta-$expected[$channel]) -gt 0.05) { $passed = $false }
            Write-Output ('{0}, channel {1}: {2:F6} dB relative to baseline (expected {3})' -f $label,$channel,$delta,$expected[$channel])
        }
        $outcome.Measurements += [pscustomobject]@{Phase=$label;GainChangeDb=$deltas;ExpectedDb=$expected}
    }
    if ($PeqTest) {
        $live = Get-Content -LiteralPath (Join-Path $OutputDirectory 'live-updates.json') -Raw | ConvertFrom-Json
        if (-not $live.Valid -or -not $live.ContinuousStream -or $live.Phases.Count -ne 3 -or
            $live.Endpoint -cne $baseline.Endpoint -or $live.SampleRate -ne $baseline.SampleRate -or
            $live.EndpointVolumeBefore -ne $baseline.EndpointVolumeBefore -or $live.SessionVolume -ne $baseline.SessionVolume) {
            throw 'Continuous-stream measurement, format, or volume was invalid.'
        }
        $targets = @($initialExpected,@(-4.0,-10.0),@(0.0,0.0))
        for ($phase=0; $phase -lt 3; ++$phase) {
            $deltas = @()
            for ($channel=0; $channel -lt 2; ++$channel) {
                $delta = 20 * [Math]::Log10($live.Phases[$phase].Channels[$channel].Amplitude / $baseline.Channels[$channel].Amplitude)
                $deltas += $delta
                if ([Math]::Abs($delta-$targets[$phase][$channel]) -gt 0.05) { $passed = $false }
                Write-Output ('Live phase {0}, channel {1}: {2:F6} dB (expected {3})' -f $phase,$channel,$delta,$targets[$phase][$channel])
            }
            $outcome.Measurements += [pscustomobject]@{Phase=('live-'+$phase);GainChangeDb=$deltas;ExpectedDb=$targets[$phase]}
        }
    }
    $outcome.Passed = $passed
    if (-not $passed) { $outcome.Errors += 'Measured gain or restoration did not meet the acceptance criteria.' }
} catch {
    $outcome.Errors += $_.Exception.Message
    Write-Output $_.Exception.Message
    Write-Output $_.ScriptStackTrace
} finally {
    if ($null -ne $contextStore) { $contextStore.Dispose() }
    if ($traceStarted) {
        try {
            Invoke-ExperimentChild $logman @('stop',$traceName,'-ets') 'stop-trace'
            Invoke-ExperimentChild (Join-Path $env:SystemRoot 'System32\tracerpt.exe') @(
                $traceFile,'-of','XML','-o',(Join-Path $OutputDirectory 'apo-events.xml'),'-y') 'decode-trace'
        } catch {
            $outcome.Errors += 'Diagnostic trace cleanup: '+$_.Exception.Message
            $outcome.Passed = $false
            Write-Output $_.Exception.Message
        }
    }
    if ($transcript) {
        $outcome | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath (Join-Path $OutputDirectory 'outcome.json') -Encoding UTF8
        Stop-Transcript | Out-Null
    }
}
if (-not $outcome.Passed) { exit 1 }
