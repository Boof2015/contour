Set-StrictMode -Version Latest
$ErrorActionPreference='Stop'

function Assert-ReleasePath([string]$Path) {
    $item=Get-Item -LiteralPath $Path -Force
    while ($null -ne $item) {
        if ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw 'Release packaging does not follow redirected paths.' }
        $item=if ($item -is [IO.DirectoryInfo]) { $item.Parent } else { $item.Directory }
    }
}
function Get-ReleaseFiles {
    @('contour.exe','contour-apo.dll','contour-deploy-info.exe','contour-apo-host.exe','contour-endpoint-check.exe',
      'ui/index.html','ui/tray.html','ui/styles.css','ui/boot.js','ui/editor.js','ui/app.js','ui/settings.js','ui/interactions.js','ui/tray.js','ui/wordmark.svg',
      'ui/fonts/Manrope.ttf','ui/fonts/OFL.txt','licenses/json.txt','licenses/WebView2.txt','licenses/WebView2-NOTICE.txt',
      'setup/contour-setup.ps1','setup/contour-uninstall.ps1','setup/SetupHost.ps1','setup/ProductRemoval.ps1','setup/DeviceSetup.ps1',
      'setup/AudioPolicy.ps1','setup/SharedSetup.ps1','setup/EndpointDeployment.ps1','setup/RuntimeUpdate.ps1','setup/LegacyMigration.ps1',
      'setup/ControlContext.ps1','setup/Deployment.ps1','setup/EffectPlacement.ps1','setup/RegistryStore.cs')
}
function Assert-ReleaseBinary([string]$Path) {
    $stream=[IO.File]::OpenRead($Path);$reader=[IO.BinaryReader]::new($stream)
    try {
        if ($reader.ReadUInt16() -ne 0x5A4D) { throw 'Release binary has no DOS header.' }
        $stream.Position=0x3C;$offset=$reader.ReadUInt32()
        if ($offset -gt $stream.Length-26) { throw 'Release binary header is truncated.' }
        $stream.Position=$offset
        if ($reader.ReadUInt32() -ne 0x4550 -or $reader.ReadUInt16() -ne 0x8664) { throw 'Release packaging requires native x64 binaries.' }
        $stream.Position=$offset+24
        if ($reader.ReadUInt16() -ne 0x20B) { throw 'Release binary is not PE32+.' }
    } finally { $reader.Dispose() }
}
function Test-ReleasePayload([string]$Path) {
    $resolved=[IO.Path]::GetFullPath($Path);Assert-ReleasePath $resolved
    $manifest=Get-Content -LiteralPath (Join-Path $resolved 'release.json') -Raw -Encoding UTF8 | ConvertFrom-Json
    if ($manifest.schema -ne 1 -or $manifest.kind -cne 'ContourReleasePayload' -or $manifest.architecture -cne 'x64') { throw 'This folder is not a Contour release payload.' }
    $expected=@(Get-ReleaseFiles | Sort-Object)
    if (($expected -join '|') -cne (@($manifest.files.path | Sort-Object) -join '|')) { throw 'Release manifest does not match the shipping file list.' }
    $actual=@(Get-ChildItem -LiteralPath $resolved -Recurse -Force | ForEach-Object {
        Assert-ReleasePath $_.FullName
        if (-not $_.PSIsContainer) { $_.FullName.Substring($resolved.TrimEnd('\').Length+1).Replace('\','/') }
    } | Where-Object { $_ -cne 'release.json' } | Sort-Object)
    if (($actual -join '|') -cne ($expected -join '|')) { throw 'Release payload contains missing or unexpected files.' }
    foreach ($file in $manifest.files) {
        $source=Join-Path $resolved $file.path
        if ($file.sha256 -cnotmatch '^[0-9A-F]{64}\z' -or (Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash -cne $file.sha256) { throw ('Release file changed: '+$file.path) }
        if ((Get-Item -LiteralPath $source).Length -ne $file.bytes) { throw 'Release file length changed.' }
        if ([IO.Path]::GetExtension($source) -in @('.exe','.dll')) { Assert-ReleaseBinary $source }
    }
    return $manifest
}
