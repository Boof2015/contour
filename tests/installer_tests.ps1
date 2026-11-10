param([Parameter(Mandatory=$true)][string]$Iscc,[Parameter(Mandatory=$true)][string]$WorkDirectory)
Set-StrictMode -Version Latest
$ErrorActionPreference='Stop'
function Assert($Condition,[string]$Message) { if (-not $Condition) { throw $Message } }
$id=[guid]::NewGuid().ToString('D')
$work=Join-Path ([IO.Path]::GetFullPath($WorkDirectory)) $id
$control=Join-Path $work 'control';[void][IO.Directory]::CreateDirectory($control)
$app=Join-Path $work 'app';$shortcut=Join-Path $work 'shortcuts\Contour.lnk'
$key='Software\Microsoft\Windows\CurrentVersion\Uninstall\ContourInstallerTest-'+$id+'_is1'
$user=[Microsoft.Win32.RegistryKey]::OpenBaseKey([Microsoft.Win32.RegistryHive]::CurrentUser,[Microsoft.Win32.RegistryView]::Registry64)
$repo=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$logNumber=0;$passed=0;$editors=[Collections.Generic.List[object]]::new()
$editorBinary=Join-Path $work 'editor-fixture.exe'
Add-Type -Path (Join-Path $PSScriptRoot 'fixtures\installer-editor.cs') -OutputAssembly $editorBinary -OutputType WindowsApplication -ReferencedAssemblies System.Windows.Forms
function Put([string]$Name,[string]$Value='1') { [IO.File]::WriteAllText((Join-Path $control $Name),$Value) }
function Clear-FixtureFlag([string]$Name) { $path=Join-Path $control $Name;if (Test-Path -LiteralPath $path) { Remove-Item -LiteralPath $path } }
function Read([string]$Name) { $path=Join-Path $control $Name;if (Test-Path -LiteralPath $path) { [IO.File]::ReadAllText($path) } else { '' } }
function Check([bool]$Condition,[string]$Message) { Assert $Condition $Message;++$script:passed;Write-Output ('PASS '+$Message) }
function InstalledVersion {
    $entry=$user.OpenSubKey($key)
    if ($null -eq $entry) { return '' }
    try { return $entry.GetValue('DisplayVersion') } finally { $entry.Dispose() }
}
function Run([string]$Exe,[string]$Extra='') {
    ++$script:logNumber
    $log=Join-Path $work ('run-'+$script:logNumber+'.log')
    $arguments='/VERYSILENT /SUPPRESSMSGBOXES /NORESTART /LOG="'+$log+'" '+$Extra
    # Inno's original uninstaller exits before its temporary child deletes the
    # original EXE. Wait for the whole tree, or the next install can race that
    # cleanup. CTest bounds this isolated lifecycle suite with a 180-second timeout.
    $process=Start-Process -FilePath $Exe -ArgumentList $arguments -WindowStyle Hidden -Wait -PassThru
    try { $process.Refresh();return $process.ExitCode } finally { $process.Dispose() }
}
function Start-Editor([string]$Executable,[string]$Control) {
    [IO.File]::Delete((Join-Path $Control 'editor-ready'))
    $editor=Start-Process -FilePath $Executable -ArgumentList ('"'+$Control+'"') -WindowStyle Hidden -PassThru
    $editors.Add($editor)
    $deadline=[DateTime]::UtcNow.AddSeconds(10)
    while (-not (Test-Path -LiteralPath (Join-Path $Control 'editor-ready'))) {
        if ($editor.HasExited -or [DateTime]::UtcNow -gt $deadline) { throw 'Isolated editor did not start.' }
        Start-Sleep -Milliseconds 25
    }
    return $editor
}
function Build([string]$Version) {
    $source=Join-Path $work ('source-'+$Version);[void][IO.Directory]::CreateDirectory($source)
    Copy-Item -LiteralPath $editorBinary -Destination (Join-Path $source 'contour.exe')
    $include=Join-Path $source 'files.iss'
    $files=@(
        'Source: "'+$source+'\contour.exe"; DestDir: "{app}"; Flags: ignoreversion'
        'Source: "'+$repo+'\tests\fixtures\installer-removal.ps1"; DestDir: "{app}\setup"; DestName: "contour-uninstall.ps1"; Flags: ignoreversion')
    [IO.File]::WriteAllLines($include,$files,[Text.UTF8Encoding]::new($true))
    $config=@(
        '#define FixtureBuild'
        '#define FixtureRoot "'+$work+'"'
        '#define FixtureId "ContourInstallerTest-'+$id+'"'
        '#define AppVersion "'+$Version+'"'
        '#define OutputDirectory "'+$work+'"'
        '#define IconFile "'+$repo+'\src\app\assets\contour.ico"'
        '#define FilesInclude "'+$include+'"'
        '#define RuntimeHash "fixture"'
        '#define WebViewHash "fixture"')
    $configPath=Join-Path $source 'config.iss'
    [IO.File]::WriteAllLines($configPath,$config,[Text.UTF8Encoding]::new($true))
    $output=& $Iscc /Q ('/DBuildConfigFile='+$configPath) (Join-Path $repo 'tools\distribution\contour.iss') 2>&1
    Assert ($LASTEXITCODE -eq 0) ($output -join "`n")
    return Join-Path $work ('Contour-Installer-Test-'+$Version+'.exe')
}
Put fixture;Put profile 'keep my EQ';Put engine 'existing device EQ'
try {
    $v1=Build '0.1.0';$v2=Build '0.1.1'
    Put dependency-fail
    $code=Run $v1 '/TASKS=""'
    Check ($code -ne 0 -and -not (Test-Path -LiteralPath $app) -and (InstalledVersion) -eq '') 'dependency failure stops before installing app files or registration'
    Clear-FixtureFlag dependency-fail;Put dependency-restart
    $code=Run $v1 '/TASKS=""'
    $log=[IO.File]::ReadAllText((Join-Path $work ('run-'+$script:logNumber+'.log')))
    Check ($code -ne 0 -and -not (Test-Path -LiteralPath $app) -and $log.Contains('Visual C++ installer result: 3010')) 'required restart defers app installation without restarting Windows'
    Clear-FixtureFlag dependency-restart
    $code=Run $v1 ('/DIR="'+(Join-Path $work 'redirected-install')+'"')
    Check ($code -ne 0 -and -not (Test-Path -LiteralPath (Join-Path $work 'redirected-install'))) 'custom paths cannot redirect the protected application installation'
    $code=Run $v1 '/TASKS=""'
    Check ($code -eq 0 -and (InstalledVersion) -eq '0.1.0' -and -not (Test-Path -LiteralPath $shortcut)) 'fresh installation supports opting out of the Start menu shortcut'
    Check ((Read vc-ready) -eq 'ready' -and (Read webview-ready) -eq 'ready') 'missing runtime components are prepared before app installation'
    $dependencyCalls=Read dependency-calls
    $code=Run $v2
    Check ($code -eq 0 -and (InstalledVersion) -eq '0.1.1' -and -not (Test-Path -LiteralPath $shortcut)) 'upgrade replaces the app and remembers shortcut opt-out'
    Check ((Read engine) -eq 'existing device EQ' -and (Read profile) -eq 'keep my EQ' -and (Read removal-calls) -eq '' -and (Read dependency-calls) -ceq $dependencyCalls) 'upgrade preserves EQ and skips existing dependencies and engine removal'
    $code=Run $v1
    Check ($code -ne 0 -and (InstalledVersion) -eq '0.1.1' -and (Get-FileHash -LiteralPath (Join-Path $app 'contour.exe')).Hash -eq (Get-FileHash -LiteralPath $editorBinary).Hash) 'downgrade leaves the newer installation untouched'
    $code=Run $v2 '/TASKS="startmenu"'
    $shell=New-Object -ComObject WScript.Shell
    Check ($code -eq 0 -and (Test-Path -LiteralPath $shortcut) -and $shell.CreateShortcut($shortcut).TargetPath -ieq (Join-Path $app 'contour.exe')) 'shortcut opt-in creates a link to the installed app'
    $code=Run $v2
    Check ($code -eq 0 -and (Test-Path -LiteralPath $shortcut)) 'upgrade remembers shortcut opt-in'
    $code=Run $v2 '/TASKS=""'
    Check ($code -eq 0 -and -not (Test-Path -LiteralPath $shortcut)) 'later opt-out removes the prior owned shortcut'
    $foreign=$shell.CreateShortcut($shortcut);$foreign.TargetPath=Join-Path $work 'other.exe';$foreign.Save()
    $code=Run $v2 '/TASKS=""'
    Check ($code -eq 0 -and (Test-Path -LiteralPath $shortcut) -and $shell.CreateShortcut($shortcut).TargetPath -ieq (Join-Path $work 'other.exe')) 'opting out does not delete another application shortcut'
    # Restore an owned shortcut for the removal lifecycle check.
    Assert ((Run $v2 '/TASKS="startmenu"') -eq 0) 'Cannot prepare owned shortcut'
    $foreignDirectory=Join-Path $work 'other-copy';[void][IO.Directory]::CreateDirectory($foreignDirectory)
    $foreignExe=Join-Path $foreignDirectory 'contour.exe';Copy-Item -LiteralPath $editorBinary -Destination $foreignExe
    $foreignEditor=Start-Editor $foreignExe $foreignDirectory
    $editor=Start-Editor (Join-Path $app 'contour.exe') $control
    $deploymentLock=[Threading.Mutex]::new($false,('Local\ContourInstallerTest-'+$id+'.Deployment'))
    Assert ($deploymentLock.WaitOne(0)) 'Cannot prepare isolated active deployment'
    try {
        $code=Run $v2
        Check ($code -ne 0 -and -not $editor.HasExited) 'an active audio deployment prevents automatic termination'
    } finally { $deploymentLock.ReleaseMutex();$deploymentLock.Dispose() }
    $code=Run $v2
    Check ($code -eq 0 -and $editor.HasExited -and (Read editor-closed) -eq 'recovery checkpoint' -and -not $foreignEditor.HasExited) 'upgrade closes the installed editor with recovery while leaving another copy running'
    $entry=$user.OpenSubKey($key);$command=$entry.GetValue('UninstallString');$entry.Dispose()
    Check ($command -match '/SILENT /CONTOURUI$') 'Windows Apps opens one uninstall options screen'
    $uninstaller=Join-Path $app 'unins000.exe'
    Put editor-busy
    $editor=Start-Editor (Join-Path $app 'contour.exe') $control
    $code=Run $uninstaller
    Check ($code -ne 0 -and -not $editor.HasExited -and (Read removal-calls) -eq '' -and (Test-Path -LiteralPath $uninstaller)) 'an active operation defers removal without force-killing the editor'
    Clear-FixtureFlag editor-busy;Put removal-fail
    $code=Run $uninstaller '/DELETEUSERDATA'
    Check ($code -ne 0 -and $editor.HasExited -and (Read profile) -eq 'keep my EQ' -and (InstalledVersion) -eq '0.1.1' -and (Test-Path -LiteralPath (Join-Path $app 'setup\contour-uninstall.ps1')) -and (Test-Path -LiteralPath $uninstaller)) 'failed engine removal retains profiles even when cleanup was requested'
    [IO.File]::WriteAllText((Join-Path $app 'foreign-file.txt'),'not installer owned')
    Clear-FixtureFlag removal-fail
    $code=Run $uninstaller
    Check ($code -eq 0 -and (Read removed) -eq 'done' -and (InstalledVersion) -eq '' -and -not (Test-Path -LiteralPath (Join-Path $app 'contour.exe')) -and -not (Test-Path -LiteralPath $shortcut)) 'retry removes the app and shortcut after successful engine removal'
    Check (-not (Test-Path -LiteralPath $uninstaller)) 'uninstaller child finishes deleting itself before the next installation'
    Check ((Read profile) -eq 'keep my EQ' -and (Test-Path -LiteralPath (Join-Path $app 'foreign-file.txt'))) 'uninstall preserves profiles and unrecognized files'
    Assert ((Run $v2) -eq 0) 'Cannot reinstall cleanup fixture'
    Put editor-hung
    $editor=Start-Editor (Join-Path $app 'contour.exe') $control
    $code=Run (Join-Path $app 'unins000.exe') '/DELETEUSERDATA'
    Check ($code -eq 0 -and $editor.HasExited -and (Read data-removed) -eq 'done' -and (Read profile) -eq '' -and -not $foreignEditor.HasExited) 'uninstall force-closes an unresponsive installed editor and performs explicit data cleanup'
    Assert ((Run $v2) -eq 0) 'Cannot reinstall after cleanup'
    Check ((Read profile) -eq '') 'reinstall after cleanup does not resurrect saved profiles'
    Assert ((Run (Join-Path $app 'unins000.exe')) -eq 0) 'Cannot remove final fixture'
    Write-Output "PASS $passed native installer lifecycle checks; no actual audio, startup, Start menu, or profile settings changed"
} catch {
    Write-Output ('Installer fixture directory: '+$work)
    $lastLog=Join-Path $work ('run-'+$script:logNumber+'.log')
    if (Test-Path -LiteralPath $lastLog) { Get-Content -LiteralPath $lastLog -Tail 60 | Write-Output }
    throw
} finally {
    foreach ($editor in $editors) { if (-not $editor.HasExited) { $editor.Kill();[void]$editor.WaitForExit(5000) };$editor.Dispose() }
    # The exact randomly named fixture key is the only registry cleanup target.
    $user.DeleteSubKeyTree($key,$false);$user.Dispose()
}
