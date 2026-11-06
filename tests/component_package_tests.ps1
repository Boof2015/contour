param([Parameter(Mandatory=$true)][string] $BinaryDirectory,
      [Parameter(Mandatory=$true)][string] $WorkDirectory,
      [string] $InfVerif = '', [string] $Inf2Cat = '')
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
function Assert($Condition, [string] $Message) { if (-not $Condition) { throw $Message } }

$inspector = Join-Path $BinaryDirectory 'contour-deploy-info.exe'
$dll = Join-Path $BinaryDirectory 'contour-apo.dll'
$stage = Join-Path $PSScriptRoot '..\tools\deployment\stage-component.ps1'
$output = Join-Path ([IO.Path]::GetFullPath($WorkDirectory)) ([guid]::NewGuid().ToString('D'))
& $stage -BinaryDirectory $BinaryDirectory -OutputDirectory $output
$inf = [IO.File]::ReadAllText((Join-Path $output 'contour-apo.inf'))
$raw = & $inspector $dll '--component-registration-only'
Assert ($LASTEXITCODE -eq 0) 'SDK registration capture failed'
$info = $raw | ConvertFrom-Json
Assert (-not $info.EmbeddedManifest -and $null -eq $info.Endpoint -and $info.Classes.Count -eq 2) 'Unexpected component metadata'
$legacyRaw = & $inspector $dll '--registration-only'
Assert ($LASTEXITCODE -eq 0) 'Legacy metadata capture failed'
$legacy = $legacyRaw | ConvertFrom-Json
Assert (@($info.Classes | Where-Object { $_.Clsid -in $legacy.Classes.Clsid }).Count -eq 0) 'HKR must not reuse globally registered CLSIDs'

# Compare every declared registration value with the SDK's actual RegisterAPO
# output in an isolated HKCU capture, not a duplicate metadata expectation.
$entries = @($inf -split '\r?\n' | Where-Object { $_ -match '^HKR,"' } |
    ConvertFrom-Csv -Header Root,Path,Name,Flags,Data)
Assert ($entries.Count -eq 30) 'Unexpected registration entry count'
foreach ($class in $info.Classes) {
    $cls = @($entries | Where-Object { $_.Path -eq "Classes\CLSID\$($class.Clsid)" })
    Assert ($cls.Count -eq 1 -and $cls[0].Data -eq $class.Name) 'COM class name mismatch'
    $server = @($entries | Where-Object { $_.Path -eq "Classes\CLSID\$($class.Clsid)\InprocServer32" })
    Assert ($server.Count -eq 2) 'COM server registration missing'
    Assert (@($server | Where-Object { $_.Name -eq '' -and $_.Flags -eq '0x00020000' -and $_.Data -eq '%13%\contour-apo.dll' }).Count -eq 1) 'DLL must load from Driver Store'
    Assert (@($server | Where-Object { $_.Name -eq 'ThreadingModel' -and $_.Data -eq 'Both' }).Count -eq 1) 'COM threading model mismatch'
    $values = @($entries | Where-Object { $_.Path -eq "AudioEngine\AudioProcessingObjects\$($class.Clsid)" })
    Assert ($values.Count -eq $class.Values.Count) 'APO registration value count mismatch'
    foreach ($value in $class.Values) {
        $actual = @($values | Where-Object { $_.Name -eq $value.Name })
        Assert ($actual.Count -eq 1) "Missing/duplicate metadata: $($value.Name)"
        $bytes = [byte[]]@(for ($i=0; $i -lt $value.Data.Length; $i+=2) { [Convert]::ToByte($value.Data.Substring($i,2),16) })
        if ($value.Type -eq 4) {
            Assert ($actual[0].Flags -eq '0x00010001') 'Incorrect DWORD registration type'
            Assert ($actual[0].Data -match '^0x[0-9a-f]{1,8}$') 'DWORD must use INF-safe hex, including UINT_MAX'
            $expected = [BitConverter]::ToUInt32($bytes,0)
            $actualData = [Convert]::ToUInt32($actual[0].Data.Substring(2),16)
        } elseif ($value.Type -eq 1) {
            Assert ($actual[0].Flags -eq '') 'Incorrect string registration type'
            $expected = [Text.Encoding]::Unicode.GetString($bytes).TrimEnd([char]0)
            $actualData = $actual[0].Data
        } else { throw "Unexpected SDK metadata type: $($value.Type)" }
        Assert ($actualData -ceq $expected) "SDK/INF mismatch: $($value.Name)"
    }
}
Assert ($inf -match '(?m)^Class=AudioProcessingObject\r?$') 'Incorrect Windows 11 driver class'
Assert ($inf.Contains('NTamd64.10.0...22000')) 'Missing Windows 11 x64 floor'
Assert ($inf.Contains('Contour_Files=13') -and $inf.Contains('PETrust=true')) 'Missing isolated file placement or protected environment signature attribute'
Assert ($inf -notmatch '(?im)^HK(CR|LM|CU),|MMDevices|DisableProtectedAudioDG|AddComponent|PKEY_FX|profile\.contour') 'Component INF must not attach endpoints, change protected audio, or ship lab settings'
Assert ((Get-FileHash -LiteralPath (Join-Path $output 'contour-apo.dll')).Hash -eq (Get-FileHash -LiteralPath $dll).Hash) 'Wrong staged DLL'
& $stage -BinaryDirectory $BinaryDirectory -OutputDirectory $output
Assert ([IO.File]::ReadAllText((Join-Path $output 'contour-apo.inf')) -ceq $inf) 'Staging must be reproducible'
if ($InfVerif) {
    foreach ($mode in @('/w', '/h')) {
        & $InfVerif $mode /v (Join-Path $output 'contour-apo.inf')
        Assert ($LASTEXITCODE -eq 0) "InfVerif $mode rejected the component INF"
    }
} else { Write-Output 'NOT RUN: WDK InfVerif (configure CONTOUR_INFVERIF to enable)' }
if ($Inf2Cat) {
    & $Inf2Cat "/driver:$output" /os:10_CO_X64,10_NI_X64,10_GE_X64 /nocat
    Assert ($LASTEXITCODE -eq 0) 'Inf2Cat rejected the component package'
} else { Write-Output 'NOT RUN: WDK Inf2Cat (configure CONTOUR_INF2CAT to enable)' }
[IO.File]::WriteAllText((Join-Path $output 'contour-apo.cat'), 'Existing catalog sentinel')
$rejected = $false
try { & $stage -BinaryDirectory $BinaryDirectory -OutputDirectory $output } catch { $rejected = $true }
Assert $rejected 'Must not overwrite an already cataloged package'
Assert ([IO.File]::ReadAllText((Join-Path $output 'contour-apo.cat')) -eq 'Existing catalog sentinel') 'Existing catalog was changed'
Write-Output 'PASS component identities, SDK metadata equivalence, package scope, reproducible staging, and catalog preservation'
