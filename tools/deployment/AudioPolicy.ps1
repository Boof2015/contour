# This Windows audio setting is machine-wide and has no cross-application
# ownership count. Record its origin, but never reset it during normal removal:
# another APO may have started depending on it after Contour was installed.
Set-StrictMode -Version Latest
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot 'Deployment.ps1')

function Get-UnsignedAudioValue { [pscustomobject]@{Type=4;Data='01000000'} }
function Assert-UnsignedAudioValue($Value) {
    if ($null -ne $Value -and -not (Test-RawEqual $Value (Get-UnsignedAudioValue)) -and
        -not (Test-RawEqual $Value ([pscustomobject]@{Type=4;Data='00000000'}))) {
        throw 'The Windows audio compatibility setting has an unsupported value. It was not changed.'
    }
}
function New-AudioPolicyJournal($Store) {
    $before=$Store.Read($script:AudioSettings,'DisableProtectedAudioDG')
    Assert-UnsignedAudioValue $before
    [pscustomobject]@{Schema=1;Kind='ContourUnsignedAudio';Before=$before;
        AlreadyEnabled=(Test-RawEqual $before (Get-UnsignedAudioValue));WriteIntent=$false;WriteConfirmed=$false;
        LastBefore=$before;State='Prepared';RemovalPolicy='Preserve'}
}
function Assert-AudioPolicyJournal($Journal) {
    if ($Journal.Schema -ne 1 -or $Journal.Kind -cne 'ContourUnsignedAudio' -or $Journal.RemovalPolicy -cne 'Preserve' -or
        $Journal.State -notin @('Prepared','Enabling','Enabled','Retained') -or
        $Journal.AlreadyEnabled -isnot [bool] -or $Journal.WriteIntent -isnot [bool] -or $Journal.WriteConfirmed -isnot [bool]) {
        throw 'Invalid Windows audio compatibility record.'
    }
    Assert-UnsignedAudioValue $Journal.Before
    Assert-UnsignedAudioValue $Journal.LastBefore
    if ($Journal.AlreadyEnabled -ne (Test-RawEqual $Journal.Before (Get-UnsignedAudioValue)) -or
        ($Journal.WriteConfirmed -and -not $Journal.WriteIntent)) { throw 'Inconsistent Windows audio compatibility record.' }
}
function Enable-UnsignedAudio($Store,[string]$Path,[scriptblock]$Checkpoint={param($Point)}) {
    if (Test-Path -LiteralPath $Path) {
        $journal=Get-Content -LiteralPath $Path -Raw -Encoding UTF8 | ConvertFrom-Json
        Assert-AudioPolicyJournal $journal
    } else {
        $journal=New-AudioPolicyJournal $Store
        Save-ContourJournal $journal $Path -CreateNew
    }
    $null=& $Checkpoint 'policy-recorded'
    $current=$Store.Read($script:AudioSettings,'DisableProtectedAudioDG')
    Assert-UnsignedAudioValue $current
    if (-not (Test-RawEqual $current (Get-UnsignedAudioValue))) {
        $journal.LastBefore=$current;$journal.WriteIntent=$true;$journal.State='Enabling'
        Save-ContourJournal $journal $Path
        $null=& $Checkpoint 'policy-intent'
        if (-not (Test-RawEqual ($Store.Read($script:AudioSettings,'DisableProtectedAudioDG')) $current)) {
            throw 'The Windows audio compatibility setting changed during installation. Check the device again.'
        }
        $Store.Write($script:AudioSettings,'DisableProtectedAudioDG',4,'01000000')
        $null=& $Checkpoint 'policy-written'
        if (-not (Test-RawEqual ($Store.Read($script:AudioSettings,'DisableProtectedAudioDG')) (Get-UnsignedAudioValue))) {
            throw 'Windows did not retain the audio compatibility setting.'
        }
        $journal.WriteConfirmed=$true
    }
    $journal.State='Enabled';Save-ContourJournal $journal $Path
}
function Preserve-UnsignedAudio([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path)) { return }
    $journal=Get-Content -LiteralPath $Path -Raw -Encoding UTF8 | ConvertFrom-Json
    Assert-AudioPolicyJournal $journal
    $journal.State='Retained';Save-ContourJournal $journal $Path
    # Deliberately no registry API: preserve both pre-existing dependencies and
    # any third-party/user change made since installation, including a new 0.
}
