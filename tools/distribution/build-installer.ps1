[CmdletBinding()]
param([Parameter(Mandatory=$true)][string]$PayloadDirectory,
      [Parameter(Mandatory=$true)][string]$OutputDirectory,
      [Parameter(Mandatory=$true)][string]$Iscc,
      [Parameter(Mandatory=$true)][string]$PrerequisiteDirectory)
. (Join-Path $PSScriptRoot 'ReleasePayload.ps1')
function InnoString([string]$Value) {
    if ($Value -match '["\r\n]' -or $Value.Contains('{') -or $Value.Contains('}')) { throw 'Installer input contains unsupported characters.' }
    return '"'+$Value+'"'
}
function MicrosoftDependency([string]$Name) {
    $path=Join-Path $PrerequisiteDirectory $Name;Assert-ReleasePath $path
    $signature=Get-AuthenticodeSignature -LiteralPath $path
    if ($signature.Status -ne 'Valid' -or $signature.SignerCertificate.Subject -notmatch '(^|, )O=Microsoft Corporation(,|$)') {
        throw ('Use the original Microsoft-signed dependency installer: '+$Name)
    }
    [pscustomobject]@{path=$path;version=(Get-Item -LiteralPath $path).VersionInfo.ProductVersion;sha256=(Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash}
}
$PayloadDirectory=[IO.Path]::GetFullPath($PayloadDirectory)
$OutputDirectory=[IO.Path]::GetFullPath($OutputDirectory)
$PrerequisiteDirectory=[IO.Path]::GetFullPath($PrerequisiteDirectory)
$Iscc=[IO.Path]::GetFullPath($Iscc)
$manifest=Test-ReleasePayload $PayloadDirectory
if ($manifest.version -notmatch '^\d+\.\d+\.\d+$') { throw 'Windows installer builds require a numeric major.minor.patch version.' }
$version=[version]$manifest.version
$binaryVersion=(Get-Item -LiteralPath (Join-Path $PayloadDirectory 'contour.exe')).VersionInfo.ProductVersion
if ($binaryVersion -ne $manifest.version) { throw 'The app version and release payload version differ. Rebuild and stage the app.' }
Assert-ReleasePath $Iscc
$compilerVersion=(& $Iscc --version | Out-String).Trim()
if ([version]$compilerVersion -lt [version]'7.1.0') { throw 'Inno Setup 7.1 or newer is required.' }
$vc=MicrosoftDependency 'vc_redist.x64.exe'
$webview=MicrosoftDependency 'MicrosoftEdgeWebview2Setup.exe'
if ($vc.version -notmatch '^14\.\d+\.\d+\.\d+$') { throw 'A Microsoft Visual C++ v14 x64 redistributable is required.' }
if ((Get-Item -LiteralPath $vc.path).VersionInfo.ProductName -notmatch 'Visual C\+\+.*\(x64\)') { throw 'The Visual C++ redistributable must be the x64 edition.' }
# Verify ancestors before creating output or compiler working files.
$ancestor=$OutputDirectory
while (-not (Test-Path -LiteralPath $ancestor)) { $ancestor=Split-Path $ancestor -Parent;if (-not $ancestor) { throw 'Cannot resolve installer output.' } }
Assert-ReleasePath $ancestor
[void][IO.Directory]::CreateDirectory($OutputDirectory);Assert-ReleasePath $OutputDirectory
$work=Join-Path $OutputDirectory ('compile-'+[guid]::NewGuid().ToString('D'))
[void][IO.Directory]::CreateDirectory($work)
$filesPath=Join-Path $work 'files.iss'
$files=@(foreach ($file in $manifest.files) {
    $relative=$file.path.Replace('/','\');$folder=Split-Path $relative -Parent
    $target=if ($folder) { '{app}\'+$folder } else { '{app}' }
    'Source: '+(InnoString (Join-Path $PayloadDirectory $relative))+'; DestDir: "'+$target+'"; Flags: ignoreversion'
})
$files+='Source: '+(InnoString (Join-Path $PayloadDirectory 'release.json'))+'; DestDir: "{app}"; Flags: ignoreversion'
[IO.File]::WriteAllLines($filesPath,$files,[Text.UTF8Encoding]::new($true))
$config=@(
    '#define AppVersion '+(InnoString $manifest.version)
    '#define OutputDirectory '+(InnoString $OutputDirectory)
    '#define IconFile '+(InnoString ([IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..\src\app\assets\contour.ico'))))
    '#define FilesInclude '+(InnoString $filesPath)
    '#define PrerequisiteDirectory '+(InnoString $PrerequisiteDirectory)
    '#define RuntimeVersion '+(InnoString $vc.version)
    '#define RuntimeHash '+(InnoString $vc.sha256)
    '#define WebViewHash '+(InnoString $webview.sha256))
$configPath=Join-Path $work 'config.iss'
[IO.File]::WriteAllLines($configPath,$config,[Text.UTF8Encoding]::new($true))
& $Iscc /Q ('/DBuildConfigFile='+$configPath) (Join-Path $PSScriptRoot 'contour.iss')
if ($LASTEXITCODE -ne 0) { throw 'Inno Setup compilation failed.' }
$setup=Join-Path $OutputDirectory ('Contour-'+$manifest.version+'-Setup-x64.exe')
$hash=(Get-FileHash -LiteralPath $setup -Algorithm SHA256).Hash
[IO.File]::WriteAllText(($setup+'.sha256'),($hash+'  '+[IO.Path]::GetFileName($setup)+"`r`n"),[Text.UTF8Encoding]::new($false))
$record=[ordered]@{schema=1;version=$manifest.version;architecture='x64';sha256=$hash;
    compiler=$compilerVersion;dependencies=@(
        @{name='Microsoft Visual C++ Runtime x64';version=$vc.version;sha256=$vc.sha256},
        @{name='Microsoft Edge WebView2 bootstrapper';version=$webview.version;sha256=$webview.sha256})}
[IO.File]::WriteAllText(($setup+'.json'),($record | ConvertTo-Json -Depth 5),[Text.UTF8Encoding]::new($false))
Write-Output ('Installer: '+$setup)
