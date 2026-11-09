param([string]$BinaryDirectory,[string]$WorkDirectory)
. (Join-Path $PSScriptRoot '..\tools\distribution\ReleasePayload.ps1')
function Assert($Value,[string]$Message) { if (-not $Value) { throw $Message } }
function Fails([scriptblock]$Run) { $failed=$false;try { $null=& $Run } catch { $failed=$true };Assert $failed 'Expected packaging rejection.' }
$work=Join-Path ([IO.Path]::GetFullPath($WorkDirectory)) ([guid]::NewGuid().ToString('D'))
$source=Join-Path $work 'source';$output=Join-Path $work 'payload'
$stage=Join-Path $PSScriptRoot '..\tools\distribution\stage-release.ps1'
[void][IO.Directory]::CreateDirectory($source)
foreach ($file in @(Get-ReleaseFiles)) {
    $target=Join-Path $source $file
    [void][IO.Directory]::CreateDirectory((Split-Path $target -Parent))
    Copy-Item -LiteralPath (Join-Path $BinaryDirectory $file) -Destination $target
}
foreach ($file in @('private-notes.txt','validation.log','contour-dsp-tests.exe','ui/private-design.html')) {
    [IO.File]::WriteAllText((Join-Path $source $file),'not for distribution')
}
& $stage -BinaryDirectory $source -OutputDirectory $output -Version '0.1.0-preview.1'
$manifest=Test-ReleasePayload $output
Assert ($manifest.files.Count -eq @(Get-ReleaseFiles).Count) 'Shipping allowlist changed.'
Assert (-not (Test-Path -LiteralPath (Join-Path $output 'private-notes.txt'))) 'Private files leaked.'
Write-Output 'PASS explicit shipping list excludes extra build files, logs and private UI artifacts'
$before=(Get-FileHash -LiteralPath (Join-Path $output 'release.json')).Hash
$missing=Join-Path $source 'contour-endpoint-check.exe'
Move-Item -LiteralPath $missing -Destination ($missing+'.hidden')
Fails { & $stage -BinaryDirectory $source -OutputDirectory $output -Version '0.1.0-preview.1' }
Assert ((Get-FileHash -LiteralPath (Join-Path $output 'release.json')).Hash -ceq $before) 'Missing helper damaged the previous release.'
Move-Item -LiteralPath ($missing+'.hidden') -Destination $missing
$binary=Join-Path $source 'contour.exe';$original=[IO.File]::ReadAllBytes($binary);$x86=$original.Clone()
$offset=[BitConverter]::ToInt32($x86,0x3C);$x86[$offset+4]=0x4C;$x86[$offset+5]=0x01
[IO.File]::WriteAllBytes($binary,$x86)
Fails { & $stage -BinaryDirectory $source -OutputDirectory $output -Version '0.1.0-preview.1' }
[IO.File]::WriteAllBytes($binary,$original)
Assert ((Get-FileHash -LiteralPath (Join-Path $output 'release.json')).Hash -ceq $before) 'Wrong architecture damaged the previous release.'
Write-Output 'PASS missing helpers and non-x64 binaries leave the previous payload intact'
& $stage -BinaryDirectory $source -OutputDirectory $output -Version '0.1.0-preview.1'
Assert ((Get-FileHash -LiteralPath (Join-Path $output 'release.json')).Hash -ceq $before) 'Identical staging changed the manifest.'
$extra=Join-Path $output 'my-file.txt';[IO.File]::WriteAllText($extra,'keep this')
Fails { & $stage -BinaryDirectory $source -OutputDirectory $output -Version '0.1.0-preview.1' }
Assert ([IO.File]::ReadAllText($extra) -ceq 'keep this') 'Unexpected output data was deleted.'
Remove-Item -LiteralPath $extra
[IO.File]::AppendAllText((Join-Path $output 'ui/styles.css'),'/* changed */')
Fails { Test-ReleasePayload $output }
Fails { & $stage -BinaryDirectory $source -OutputDirectory $output -Version '0.1.0-preview.1' }
Write-Output 'PASS repeatable staging; modified or unowned output files are preserved'
$linked=Join-Path $work 'linked';$target=Join-Path $work 'target'
[void][IO.Directory]::CreateDirectory($target)
[void](New-Item -ItemType Junction -Path $linked -Target $target)
Fails { & $stage -BinaryDirectory $source -OutputDirectory (Join-Path $linked 'missing\payload') -Version '0.1.0-preview.1' }
Assert (-not (Test-Path -LiteralPath (Join-Path $target 'missing'))) 'Packaging followed a redirected output path.'
Fails { & $stage -BinaryDirectory $source -OutputDirectory $source -Version '0.1.0-preview.1' }
Write-Output 'PASS redirected paths and overlapping input/output are rejected'
