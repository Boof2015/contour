[CmdletBinding()]
param([switch]$Installer)
Set-StrictMode -Version Latest
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot '..\distribution\ReleasePayload.ps1')
$root=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
$directory=Join-Path $root 'build\ci-tools'
$ancestor=$directory
while (-not (Test-Path -LiteralPath $ancestor)) { $ancestor=Split-Path $ancestor -Parent }
Assert-ReleasePath $ancestor
[void][IO.Directory]::CreateDirectory($directory)
Assert-ReleasePath $directory
$dependencies=Get-Content -LiteralPath (Join-Path $PSScriptRoot 'dependencies.json') -Raw | ConvertFrom-Json

function Get-Dependency($Dependency) {
    $path=Join-Path $directory $Dependency.file
    if (-not (Test-Path -LiteralPath $path)) {
        Write-Host ('Downloading '+$Dependency.file)
        [Net.ServicePointManager]::SecurityProtocol=[Net.SecurityProtocolType]::Tls12
        Invoke-WebRequest -Uri $Dependency.url -OutFile $path -UseBasicParsing
    }
    Assert-ReleasePath $path
    if ((Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash -ne $Dependency.sha256) {
        throw ('Checksum mismatch: '+$path+'. Remove this download and retry.')
    }
    $signature=Get-AuthenticodeSignature -LiteralPath $path
    $publisher='(^|, )O='+[regex]::Escape($Dependency.publisher)+'(,|$)'
    if ($signature.Status -ne 'Valid' -or $signature.SignerCertificate.Subject -notmatch $publisher) {
        throw ('Invalid publisher signature: '+$path)
    }
    return $path
}
function Run-Installer([string]$Path,[string]$Arguments) {
    $process=Start-Process -FilePath $Path -ArgumentList $Arguments -WindowStyle Hidden -PassThru -Wait
    if ($process.ExitCode -ne 0) { throw ('Tool preparation failed ('+$process.ExitCode+'): '+$Path) }
}
function Test-WebViewRuntime {
    foreach ($hive in @([Microsoft.Win32.RegistryHive]::LocalMachine,[Microsoft.Win32.RegistryHive]::CurrentUser)) {
        $base=[Microsoft.Win32.RegistryKey]::OpenBaseKey($hive,[Microsoft.Win32.RegistryView]::Registry32)
        try {
            $key=$base.OpenSubKey('SOFTWARE\Microsoft\EdgeUpdate\Clients\{F3017226-FE2A-4295-8BDF-00C3A9A7E4C5}')
            if ($null -ne $key) {
                try { if ([version]$key.GetValue('pv','0.0.0.0') -gt [version]'0.0.0.0') { return $true } }
                finally { $key.Dispose() }
            }
        } finally { $base.Dispose() }
    }
    return $false
}

$inno=Get-Dependency $dependencies.inno
# Extract a fresh portable compiler from the verified installer; no machine install.
$portable=Join-Path $directory ('inno-'+[guid]::NewGuid().ToString('D'))
Run-Installer $inno ('/PORTABLE=1 /CURRENTUSER /VERYSILENT /SUPPRESSMSGBOXES /NORESTART /DIR="'+$portable+'"')
$iscc=Join-Path $portable 'ISCC.exe'
if ((& $iscc --version | Out-String).Trim() -ne $dependencies.inno.version) { throw 'Unexpected Inno compiler version.' }
$hasWebView=Test-WebViewRuntime
if ($Installer -or -not $hasWebView) { $webview=Get-Dependency $dependencies.webview }
if (-not $hasWebView) {
    if ($env:GITHUB_ACTIONS -ne 'true') { throw 'Install Microsoft Edge WebView2 Runtime before running the desktop tests locally.' }
    Run-Installer $webview '/silent /install'
    if (-not (Test-WebViewRuntime)) { throw 'WebView2 runtime is still unavailable.' }
}
if ($Installer) { [void](Get-Dependency $dependencies.vc) }
[pscustomobject]@{iscc=$iscc;prerequisites=$directory}
