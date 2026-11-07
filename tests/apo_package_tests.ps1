param([string]$BinaryDirectory,[string]$WorkDirectory)
Set-StrictMode -Version Latest
$ErrorActionPreference='Stop'
# Installed packages have a neutral fallback profile. The verification host must
# neither assume the development fallback gain nor modify the installed profile.
$directory=Join-Path $WorkDirectory ([guid]::NewGuid().ToString('D'))
[void][IO.Directory]::CreateDirectory($directory)
$dll=Join-Path $directory 'contour-apo.dll';$profile=Join-Path $directory 'profile.contour'
try {
    Copy-Item -LiteralPath (Join-Path $BinaryDirectory 'contour-apo.dll') -Destination $dll
    [IO.File]::WriteAllText($profile,"CONTOUR 1 48000 2`nBypass 0`nEnd`n")
    $before=(Get-FileHash -LiteralPath $profile -Algorithm SHA256).Hash
    & (Join-Path $BinaryDirectory 'contour-apo-host.exe') $dll
    if ($LASTEXITCODE -ne 0) { throw 'Installed-package APO verification failed.' }
    if ((Get-FileHash -LiteralPath $profile -Algorithm SHA256).Hash -cne $before) { throw 'Verification changed the package profile.' }
} finally {
    [IO.File]::Delete($profile);[IO.File]::Delete($dll)
    [IO.Directory]::Delete($directory)
}
