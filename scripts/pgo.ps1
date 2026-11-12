#requires -Version 5.1
<#
.SYNOPSIS
    Builds, trains, and rebuilds Kessoku with profile-guided optimization.

.DESCRIPTION
    The workflow has four phases:

      1. Configure and build the Release app and its vcpkg dependencies with
         /LTCG /GENPROFILE. The app link writes build-pgo\pgo\Kessoku.pgd and
         each dependency DLL link writes its own .pgd next to the DLL in the
         vcpkg build tree.
      2. Generate a small training library (FLAC and WAV) under
         build-pgo\training, drive the real UI through scan, play, pause,
         resume, seek, natural end-of-stream, and a second track, then close
         the app so it flushes the .pgc files of the app and every loaded DLL.
      3. Merge the .pgc files into their matching .pgd databases with pgomgr,
         then relink each dependency DLL with /LTCG /USEPROFILE in its vcpkg
         build tree and deploy the optimized DLL.
      4. Reconfigure the same build tree with /LTCG /USEPROFILE, relink the
         app, verify that no deployed binary imports the PGO runtime, and run
         the Release tests plus a smoke test of the optimized binary.

    PGO is opt-in: the normal presets never pass /GENPROFILE or /USEPROFILE.
    Profiles and training media live under build-pgo, which is gitignored.
    Each run rebuilds the x64-windows-native-pgo dependency packages from
    source and clears them from the binary cache, so stale counters cannot
    leak into a new profile.

.PARAMETER SkipTests
    Skip the CTest run after the optimized build.

.PARAMETER SkipSmokeTest
    Skip the scripted UI smoke test of the optimized binary.

.NOTES
    This script controls the desktop UI while it runs. Do not use the mouse or
    keyboard until it finishes, and make sure no Kessoku instance is already
    running. Training opens the default output device in WASAPI Exclusive
    Mode, so stop other audio test suites before invoking it. It requires
    VCPKG_ROOT and deletes the x64-windows-native-pgo build trees under the
    vcpkg root so the instrumented dependencies are rebuilt from source.
#>
[CmdletBinding()]
param(
    [switch]$SkipTests,
    [switch]$SkipSmokeTest
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$RepoRoot = Split-Path -Parent $PSScriptRoot
$BuildDir = Join-Path $RepoRoot 'build-pgo'
$PgoDir = Join-Path $BuildDir 'pgo'
$TrainingRoot = Join-Path $BuildDir 'training'
$FlacTrainingRoot = Join-Path $TrainingRoot 'flac'
$WavTrainingRoot = Join-Path $TrainingRoot 'wav'
$ReleaseDir = Join-Path $BuildDir 'Release'
$AppPath = Join-Path $ReleaseDir 'Kessoku.exe'
$PgdPath = Join-Path $PgoDir 'Kessoku.pgd'

# Dependency PGO targets the vcpkg triplet that adds /GENPROFILE to the same
# Zen 3 profile as x64-windows-native.
$PgoTriplet = 'x64-windows-native-pgo'
$TripletDir = Join-Path $RepoRoot 'triplets'
$BuildToolsDir = Join-Path $BuildDir 'tools'
$DependencyBin = Join-Path $BuildDir "vcpkg_installed\$PgoTriplet\bin"

$VcpkgRoot = $env:VCPKG_ROOT
if (-not $VcpkgRoot) {
    $cachePath = Join-Path $RepoRoot 'build\CMakeCache.txt'
    if (Test-Path $cachePath) {
        $match = Select-String -Path $cachePath -Pattern '^Z_VCPKG_ROOT_DIR:INTERNAL=(.+)$' |
            Select-Object -First 1
        if ($null -ne $match) {
            $VcpkgRoot = $match.Matches[0].Groups[1].Value
        }
    }
}
if (-not $VcpkgRoot) {
    throw 'VCPKG_ROOT is not set and the default build cache has no vcpkg root.'
}
$VcpkgExe = Join-Path $VcpkgRoot 'vcpkg.exe'
$VcpkgBuildtrees = Join-Path $VcpkgRoot 'buildtrees'
if (-not (Test-Path $VcpkgExe)) {
    throw ("vcpkg.exe not found at {0}." -f $VcpkgExe)
}

# Windows messages used by the automation.
$WM_CLOSE = 0x0010
$WM_HSCROLL = 0x0114
$BM_CLICK = 0x00F5
$TBM_SETPOS = 0x0405
$TB_ENDTRACK = 8

$IDC_PLAY_BUTTON = 101
$IDC_SEEK_BAR = 102
$IDC_STATUS_LABEL = 103
$IDC_POSITION_LABEL = 104

function Invoke-Native {
    param(
        [Parameter(Mandatory = $true)][string]$FilePath,
        [Parameter()][string[]]$Arguments = @()
    )

    Write-Host ("> {0} {1}" -f $FilePath, ($Arguments -join ' '))
    & $FilePath @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw ("{0} exited with code {1}" -f $FilePath, $LASTEXITCODE)
    }
}

function Wait-For {
    param(
        [Parameter(Mandatory = $true)][scriptblock]$Probe,
        [Parameter(Mandatory = $true)][string]$Description,
        [int]$TimeoutSeconds = 30
    )

    $deadline = (Get-Date).AddSeconds($TimeoutSeconds)
    while ((Get-Date) -lt $deadline) {
        $value = & $Probe
        if ($null -ne $value -and $value -ne $false) {
            return $value
        }
        Start-Sleep -Milliseconds 100
    }
    throw ("Timed out after {0}s waiting for {1}." -f $TimeoutSeconds, $Description)
}

if (-not ('KessokuPgo.Ui' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;

namespace KessokuPgo
{
    public static class Ui
    {
        public delegate bool EnumWindowsProc(IntPtr hWnd, IntPtr lParam);

        [DllImport("user32.dll")]
        private static extern bool EnumWindows(EnumWindowsProc callback, IntPtr lParam);

        [DllImport("user32.dll")]
        private static extern uint GetWindowThreadProcessId(IntPtr hWnd, out uint processId);

        [DllImport("user32.dll")]
        public static extern bool IsWindow(IntPtr hWnd);

        [DllImport("user32.dll")]
        public static extern bool IsWindowVisible(IntPtr hWnd);

        [DllImport("user32.dll", CharSet = CharSet.Unicode)]
        public static extern IntPtr FindWindowEx(IntPtr parent, IntPtr after, string className, string title);

        [DllImport("user32.dll", CharSet = CharSet.Unicode)]
        private static extern int GetWindowTextLength(IntPtr hWnd);

        [DllImport("user32.dll", CharSet = CharSet.Unicode)]
        private static extern int GetWindowText(IntPtr hWnd, StringBuilder text, int maxCount);

        [DllImport("user32.dll", CharSet = CharSet.Unicode, EntryPoint = "SendMessageW")]
        private static extern IntPtr SendMessageText(IntPtr hWnd, uint msg, IntPtr wParam, StringBuilder lParam);

        [DllImport("user32.dll", EntryPoint = "SendMessageW")]
        public static extern IntPtr SendMessage(IntPtr hWnd, uint msg, IntPtr wParam, IntPtr lParam);

        [DllImport("user32.dll")]
        public static extern bool PostMessage(IntPtr hWnd, uint msg, IntPtr wParam, IntPtr lParam);

        [DllImport("user32.dll")]
        public static extern IntPtr GetDlgItem(IntPtr dialog, int id);

        [DllImport("user32.dll")]
        public static extern bool SetForegroundWindow(IntPtr hWnd);

        public static IntPtr[] GetVisibleTopLevelWindows(uint processId)
        {
            var windows = new List<IntPtr>();
            EnumWindows(delegate(IntPtr hWnd, IntPtr lParam)
            {
                uint pid;
                GetWindowThreadProcessId(hWnd, out pid);
                if (pid == processId && IsWindowVisible(hWnd))
                {
                    windows.Add(hWnd);
                }
                return true;
            }, IntPtr.Zero);
            return windows.ToArray();
        }

        public static string GetWindowTitle(IntPtr hWnd)
        {
            int length = GetWindowTextLength(hWnd);
            if (length <= 0)
            {
                return string.Empty;
            }
            var text = new StringBuilder(length + 1);
            GetWindowText(hWnd, text, text.Capacity);
            return text.ToString();
        }

        public static string GetControlText(IntPtr hWnd)
        {
            var text = new StringBuilder(512);
            SendMessageText(hWnd, 0x000D, (IntPtr)text.Capacity, text);
            return text.ToString();
        }

        public static IntPtr FindTopLevelWindow(uint processId, string title)
        {
            foreach (IntPtr hWnd in GetVisibleTopLevelWindows(processId))
            {
                if (GetWindowTitle(hWnd) == title)
                {
                    return hWnd;
                }
            }
            return IntPtr.Zero;
        }

        public static int GetListItemCount(IntPtr listView)
        {
            return SendMessage(listView, 0x1004, IntPtr.Zero, IntPtr.Zero).ToInt32();
        }

        public static int GetSelectedIndex(IntPtr listView)
        {
            return SendMessage(listView, 0x100C, (IntPtr)(-1), (IntPtr)0x0002).ToInt32();
        }

        public static IntPtr MakeLParam(int low, int high)
        {
            return (IntPtr)((high << 16) | (low & 0xFFFF));
        }
    }
}
'@
}

Add-Type -AssemblyName System.Windows.Forms

function ConvertTo-SendKeysLiteral {
    param([Parameter(Mandatory = $true)][string]$Text)

    return [regex]::Replace($Text, '([+^%~()\[\]{}])', '{$1}')
}

function Find-KessokuMainWindow {
    param([Parameter(Mandatory = $true)][int]$ProcessId)

    return [KessokuPgo.Ui]::FindTopLevelWindow([uint32]$ProcessId, 'Kessoku')
}

function Find-FolderPickerWindow {
    param([Parameter(Mandatory = $true)][int]$ProcessId)

    return [KessokuPgo.Ui]::FindTopLevelWindow(
        [uint32]$ProcessId, 'Select music library folder')
}

function Select-LibraryFolder {
    param(
        [Parameter(Mandatory = $true)][int]$ProcessId,
        [Parameter(Mandatory = $true)][string]$Folder
    )

    $picker = Wait-For {
        $hWnd = Find-FolderPickerWindow -ProcessId $ProcessId
        if ($hWnd -ne [IntPtr]::Zero) { return $hWnd }
        return $null
    } 'the library folder picker' 20

    [void][KessokuPgo.Ui]::SetForegroundWindow($picker)
    Start-Sleep -Milliseconds 400

    # Alt+D focuses the file dialog's address bar. Ctrl+A replaces whatever
    # path is already there, Enter navigates into the training folder.
    [System.Windows.Forms.SendKeys]::SendWait('%d')
    Start-Sleep -Milliseconds 250
    [System.Windows.Forms.SendKeys]::SendWait('^a')
    Start-Sleep -Milliseconds 100
    [System.Windows.Forms.SendKeys]::SendWait((ConvertTo-SendKeysLiteral -Text $Folder))
    Start-Sleep -Milliseconds 100
    [System.Windows.Forms.SendKeys]::SendWait('{ENTER}')
    Start-Sleep -Milliseconds 800

    # The modern file dialog's "Select Folder" button is control ID 1.
    $selectButton = [KessokuPgo.Ui]::GetDlgItem($picker, 1)
    if ($selectButton -eq [IntPtr]::Zero) {
        throw 'Could not find the folder picker''s Select Folder button (ID 1).'
    }
    [void][KessokuPgo.Ui]::PostMessage($selectButton, $BM_CLICK, [IntPtr]::Zero, [IntPtr]::Zero)

    [void](Wait-For {
        if (-not [KessokuPgo.Ui]::IsWindow($picker)) { return $true }
        return $null
    } 'the folder picker to close' 20)
}

function Start-KessokuSession {
    param(
        [Parameter(Mandatory = $true)][string]$Folder,
        [Parameter(Mandatory = $true)][string]$WorkingDirectory
    )

    $process = Start-Process -FilePath $AppPath -WorkingDirectory $WorkingDirectory -PassThru

    $main = Wait-For {
        $hWnd = Find-KessokuMainWindow -ProcessId $process.Id
        if ($hWnd -ne [IntPtr]::Zero) { return $hWnd }
        return $null
    } 'the Kessoku main window' 20

    Select-LibraryFolder -ProcessId $process.Id -Folder $Folder

    $list = Wait-For {
        $hWnd = [KessokuPgo.Ui]::FindWindowEx($main, [IntPtr]::Zero, 'SysListView32', $null)
        if ($hWnd -ne [IntPtr]::Zero) { return $hWnd }
        return $null
    } 'the track list view' 10

    return [pscustomobject]@{
        Process = $process
        Main    = $main
        List    = $list
    }
}

function Wait-TrackList {
    param(
        [Parameter(Mandatory = $true)][IntPtr]$List,
        [Parameter(Mandatory = $true)][int]$ExpectedCount
    )

    [void](Wait-For {
        $count = [KessokuPgo.Ui]::GetListItemCount($List)
        if ($count -eq $ExpectedCount) { return $count }
        return $null
    } ("the scan to produce {0} tracks (currently {1})" -f $ExpectedCount,
        [KessokuPgo.Ui]::GetListItemCount($List)) 30)
}

function Select-Track {
    param(
        [Parameter(Mandatory = $true)][IntPtr]$List,
        [Parameter(Mandatory = $true)][int]$Index
    )

    # Pointers in WM_USER control messages are not marshaled across process
    # boundaries, so LVM_SETITEMSTATE with an LVITEM structure cannot work from
    # this script. A posted left-click is pointer-free, and LVM_GETNEXTITEM can
    # verify which row actually became selected.
    $rowHeight = 21
    $headerHeight = 27
    $lParam = [KessokuPgo.Ui]::MakeLParam(30, $headerHeight + ($rowHeight * $Index))
    [void][KessokuPgo.Ui]::PostMessage($List, 0x0201, [IntPtr]1, $lParam)
    [void][KessokuPgo.Ui]::PostMessage($List, 0x0202, [IntPtr]0, $lParam)

    [void](Wait-For {
        if ([KessokuPgo.Ui]::GetSelectedIndex($List) -eq $Index) { return $true }
        return $null
    } ("row {0} to be selected" -f $Index) 5)
}

function Get-StatusText {
    param([Parameter(Mandatory = $true)][IntPtr]$Main)

    $label = [KessokuPgo.Ui]::GetDlgItem($Main, $IDC_STATUS_LABEL)
    return [KessokuPgo.Ui]::GetControlText($label)
}

function Get-PositionText {
    param([Parameter(Mandatory = $true)][IntPtr]$Main)

    $label = [KessokuPgo.Ui]::GetDlgItem($Main, $IDC_POSITION_LABEL)
    return [KessokuPgo.Ui]::GetControlText($label)
}

function Wait-StatusText {
    param(
        [Parameter(Mandatory = $true)][IntPtr]$Main,
        [Parameter(Mandatory = $true)][string]$Pattern,
        [Parameter(Mandatory = $true)][string]$Description,
        [int]$TimeoutSeconds = 15
    )

    return Wait-For {
        $text = Get-StatusText -Main $Main
        if ($text -like $Pattern) { return $text }
        return $null
    } $Description $TimeoutSeconds
}

function Click-PlayPause {
    param([Parameter(Mandatory = $true)][IntPtr]$Main)

    $button = [KessokuPgo.Ui]::GetDlgItem($Main, $IDC_PLAY_BUTTON)
    if ($button -eq [IntPtr]::Zero) {
        throw 'Could not find the Play button.'
    }
    [void][KessokuPgo.Ui]::PostMessage($button, $BM_CLICK, [IntPtr]::Zero, [IntPtr]::Zero)
}

function Seek-To {
    param(
        [Parameter(Mandatory = $true)][IntPtr]$Main,
        [Parameter(Mandatory = $true)][int]$Position
    )

    $seekBar = [KessokuPgo.Ui]::GetDlgItem($Main, $IDC_SEEK_BAR)
    if ($seekBar -eq [IntPtr]::Zero) {
        throw 'Could not find the seek bar.'
    }

    # TBM_SETPOS is sent synchronously so the following TB_ENDTRACK observer
    # always reads the new position. TB_ENDTRACK makes the app call
    # SeekFromBar(GetPos()), the same path a mouse drag release takes.
    [void][KessokuPgo.Ui]::SendMessage(
        $seekBar, $TBM_SETPOS, [IntPtr]1, [IntPtr]$Position)
    [void][KessokuPgo.Ui]::PostMessage(
        $Main, $WM_HSCROLL, [IntPtr]$TB_ENDTRACK, $seekBar)
}

function Close-KessokuSession {
    param(
        [Parameter(Mandatory = $true)][pscustomobject]$Session
    )

    [void][KessokuPgo.Ui]::PostMessage(
        $Session.Main, $WM_CLOSE, [IntPtr]::Zero, [IntPtr]::Zero)

    if (-not $Session.Process.WaitForExit(15000)) {
        throw 'Kessoku did not exit within 15s of WM_CLOSE.'
    }
    if ($Session.Process.ExitCode -ne 0) {
        throw ("Kessoku exited with code {0}." -f $Session.Process.ExitCode)
    }
}

function New-WavFile {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [double]$Seconds = 5.0,
        [int]$SampleRate = 44100,
        [double]$Frequency = 220.0
    )

    $channels = 2
    $bitsPerSample = 16
    $bytesPerSample = 2
    $frameCount = [int]($SampleRate * $Seconds)
    $blockAlign = $channels * $bytesPerSample
    $dataSize = $frameCount * $blockAlign

    $stream = [System.IO.File]::Create($Path)
    $writer = [System.IO.BinaryWriter]::new($stream)
    try {
        $ascii = [System.Text.Encoding]::ASCII
        $writer.Write($ascii.GetBytes('RIFF'))
        $writer.Write([uint32](36 + $dataSize))
        $writer.Write($ascii.GetBytes('WAVE'))
        $writer.Write($ascii.GetBytes('fmt '))
        $writer.Write([uint32]16)
        $writer.Write([uint16]1) # PCM
        $writer.Write([uint16]$channels)
        $writer.Write([uint32]$SampleRate)
        $writer.Write([uint32]($SampleRate * $blockAlign))
        $writer.Write([uint16]$blockAlign)
        $writer.Write([uint16]$bitsPerSample)
        $writer.Write($ascii.GetBytes('data'))
        $writer.Write([uint32]$dataSize)

        $amplitude = 0.2 * 32767.0
        $twoPi = 2.0 * [math]::PI
        for ($i = 0; $i -lt $frameCount; $i++) {
            $sample = [int16][math]::Round(
                $amplitude * [math]::Sin($twoPi * $Frequency * $i / $SampleRate))
            $writer.Write($sample)
            $writer.Write($sample)
        }
    }
    finally {
        $writer.Dispose()
        $stream.Dispose()
    }
}

function Resolve-Ffmpeg {
    $command = Get-Command ffmpeg.exe -ErrorAction SilentlyContinue
    if ($null -ne $command) {
        return $command.Source
    }

    $wingetPackages = Join-Path $env:LOCALAPPDATA 'Microsoft\WinGet\Packages'
    if (Test-Path $wingetPackages) {
        $candidate = Get-ChildItem -Path $wingetPackages -Recurse -Filter ffmpeg.exe `
            -ErrorAction SilentlyContinue |
            Where-Object { $_.FullName -match '\\bin\\ffmpeg\.exe$' } |
            Select-Object -First 1
        if ($null -ne $candidate) {
            return $candidate.FullName
        }
    }

    return $null
}

function New-TrainingLibrary {
    if (Test-Path $TrainingRoot) {
        Remove-Item $TrainingRoot -Recurse -Force
    }
    New-Item -ItemType Directory -Path $TrainingRoot | Out-Null

    $albumA = Join-Path $FlacTrainingRoot 'album-a'
    $albumB = Join-Path $FlacTrainingRoot 'album-b'
    New-Item -ItemType Directory -Path $albumA -Force | Out-Null
    New-Item -ItemType Directory -Path $albumB -Force | Out-Null
    New-Item -ItemType Directory -Path $WavTrainingRoot -Force | Out-Null

    # A non-audio file exercises the scanner's skip branch.
    Set-Content -Path (Join-Path $WavTrainingRoot 'notes.txt') -Value 'not audio' -Encoding ASCII

    $ffmpeg = Resolve-Ffmpeg
    if ($null -ne $ffmpeg) {
        Write-Host ("Using ffmpeg: {0}" -f $ffmpeg)
        $tracks = @(
            @{ Path = (Join-Path $albumA '01-alpha.flac'); Frequency = 220; Title = 'Alpha'; Track = '1' },
            @{ Path = (Join-Path $albumA '02-beta.flac'); Frequency = 330; Title = 'Beta'; Track = '2' },
            @{ Path = (Join-Path $albumA '03-gamma.flac'); Frequency = 440; Title = 'Gamma'; Track = '3' },
            @{ Path = (Join-Path $albumB '01-delta.flac'); Frequency = 550; Title = 'Delta'; Track = '1' }
        )
        foreach ($track in $tracks) {
            Invoke-Native -FilePath $ffmpeg -Arguments @(
                '-hide_banner', '-loglevel', 'error', '-y',
                '-f', 'lavfi',
                '-i', ('sine=frequency={0}:sample_rate=44100:duration=6' -f $track.Frequency),
                '-ac', '2',
                '-c:a', 'flac',
                '-sample_fmt', 's16',
                '-metadata', ('title={0}' -f $track.Title),
                '-metadata', 'artist=Kessoku PGO',
                '-metadata', ('album={0}' -f (Split-Path -Leaf (Split-Path -Parent $track.Path))),
                '-metadata', ('track={0}' -f $track.Track),
                $track.Path
            )
        }

        Invoke-Native -FilePath $ffmpeg -Arguments @(
            '-hide_banner', '-loglevel', 'error', '-y',
            '-f', 'lavfi',
            '-i', 'sine=frequency=660:sample_rate=44100:duration=5',
            '-ac', '2',
            '-c:a', 'pcm_s16le',
            (Join-Path $WavTrainingRoot 'waveform.wav')
        )
    }
    else {
        Write-Warning 'ffmpeg not found; FLAC decode will not be trained.'
        New-WavFile -Path (Join-Path $WavTrainingRoot 'waveform.wav')
    }
}

function Invoke-FlacTrainingSession {
    param([Parameter(Mandatory = $true)][string]$Folder)

    $session = Start-KessokuSession -Folder $Folder -WorkingDirectory $PgoDir
    try {
        Wait-TrackList -List $session.List -ExpectedCount 4

        # FLAC decode, pause, resume, mid-track seek, then natural
        # end-of-stream (which releases the exclusive device and resets state).
        Select-Track -List $session.List -Index 0
        Click-PlayPause -Main $session.Main
        [void](Wait-StatusText -Main $session.Main -Pattern 'Playing: 01-alpha.flac' `
            -Description 'FLAC playback to start')

        Start-Sleep -Milliseconds 1200
        Click-PlayPause -Main $session.Main
        [void](Wait-StatusText -Main $session.Main -Pattern 'Paused' `
            -Description 'FLAC playback to pause')
        Start-Sleep -Milliseconds 800

        Click-PlayPause -Main $session.Main
        [void](Wait-StatusText -Main $session.Main -Pattern 'Playing' `
            -Description 'FLAC playback to resume')

        Seek-To -Main $session.Main -Position 400
        Start-Sleep -Milliseconds 500
        $position = Get-PositionText -Main $session.Main
        if ($position -notmatch '^0:0[1-5] / 0:06$') {
            throw ("Unexpected position after FLAC seek: '{0}'" -f $position)
        }
        if ((Get-StatusText -Main $session.Main) -eq 'Seek failed.') {
            throw 'FLAC seek reported failure.'
        }

        Seek-To -Main $session.Main -Position 995
        [void](Wait-StatusText -Main $session.Main -Pattern 'Finished' `
            -Description 'the FLAC track to finish naturally' 15)
    }
    finally {
        Close-KessokuSession -Session $session
    }
}

function Invoke-WavTrainingSession {
    param([Parameter(Mandatory = $true)][string]$Folder)

    $session = Start-KessokuSession -Folder $Folder -WorkingDirectory $PgoDir
    try {
        Wait-TrackList -List $session.List -ExpectedCount 1

        Select-Track -List $session.List -Index 0
        Click-PlayPause -Main $session.Main
        [void](Wait-StatusText -Main $session.Main -Pattern 'Playing: waveform.wav' `
            -Description 'WAV playback to start')

        Start-Sleep -Milliseconds 1000
        Click-PlayPause -Main $session.Main
        [void](Wait-StatusText -Main $session.Main -Pattern 'Paused' `
            -Description 'WAV playback to pause')
        Start-Sleep -Milliseconds 500

        Click-PlayPause -Main $session.Main
        [void](Wait-StatusText -Main $session.Main -Pattern 'Playing' `
            -Description 'WAV playback to resume')

        Seek-To -Main $session.Main -Position 500
        Start-Sleep -Milliseconds 500
        $position = Get-PositionText -Main $session.Main
        if ($position -notmatch '^0:0[1-4] / 0:05$') {
            throw ("Unexpected position after WAV seek: '{0}'" -f $position)
        }
        if ((Get-StatusText -Main $session.Main) -eq 'Seek failed.') {
            throw 'WAV seek reported failure.'
        }
    }
    finally {
        Close-KessokuSession -Session $session
    }
}

function Invoke-SmokeSession {
    param([Parameter(Mandatory = $true)][string]$Folder)

    $session = Start-KessokuSession -Folder $Folder -WorkingDirectory $ReleaseDir
    try {
        Wait-TrackList -List $session.List -ExpectedCount 4
        Select-Track -List $session.List -Index 0
        Click-PlayPause -Main $session.Main
        [void](Wait-StatusText -Main $session.Main -Pattern 'Playing: 01-alpha.flac' `
            -Description 'optimized FLAC playback to start')
        Start-Sleep -Milliseconds 1000
    }
    finally {
        Close-KessokuSession -Session $session
    }
}

function Get-PgomgrPath {
    $command = Get-Command pgomgr.exe -ErrorAction SilentlyContinue
    if ($null -ne $command) {
        return $command.Source
    }

    $cachePath = Join-Path $BuildDir 'CMakeCache.txt'
    if (-not (Test-Path $cachePath)) {
        throw "CMakeCache.txt not found at $cachePath; configure pgo-generate first."
    }
    $match = Select-String -Path $cachePath -Pattern '^CMAKE_LINKER:FILEPATH=(.+)$' |
        Select-Object -First 1
    if ($null -eq $match) {
        throw 'CMAKE_LINKER not found in CMakeCache.txt; cannot locate pgomgr.exe.'
    }
    $linker = $match.Matches[0].Groups[1].Value
    $candidate = Join-Path (Split-Path -Parent $linker) 'pgomgr.exe'
    if (Test-Path $candidate) {
        return $candidate
    }

    throw "pgomgr.exe not found next to $linker."
}

function Assert-ProjectUsesPgoFlag {
    param([Parameter(Mandatory = $true)][string]$Flag)

    $projectPath = Join-Path $BuildDir 'Kessoku.vcxproj'
    if (-not (Test-Path $projectPath)) {
        throw "Generated project not found at $projectPath."
    }
    $project = Get-Content -Path $projectPath -Raw
    if ($project -notmatch [regex]::Escape($Flag)) {
        throw ("{0} is missing from {1}; the link would not use PGO." -f $Flag, $projectPath)
    }
    Write-Host ("Verified {0} in Kessoku.vcxproj" -f $Flag)
}

function Reset-PgoState {
    $running = @(Get-Process -Name Kessoku -ErrorAction SilentlyContinue)
    if ($running.Count -gt 0) {
        throw 'Kessoku is running. Close it before starting the PGO workflow.'
    }

    if (Test-Path $BuildDir) {
        Remove-Item $BuildDir -Recurse -Force
    }

    # The dependency packages must be recompiled and relinked for this run:
    # a restored or reused package would carry the previous run's counters.
    if (Test-Path $VcpkgBuildtrees) {
        Get-ChildItem $VcpkgBuildtrees -Directory | ForEach-Object {
            $pgoTree = Join-Path $_.FullName ($PgoTriplet + '-rel')
            if (Test-Path $pgoTree) {
                Remove-Item $pgoTree -Recurse -Force
            }
        }
    }
    $env:VCPKG_BINARY_SOURCES = 'clear'

    New-Item -ItemType Directory -Path $BuildDir | Out-Null
}

function Get-DependencyBuildTrees {
    if (-not (Test-Path $VcpkgBuildtrees)) {
        return @()
    }
    return @(Get-ChildItem $VcpkgBuildtrees -Directory | ForEach-Object {
        $pgoTree = Join-Path $_.FullName ($PgoTriplet + '-rel')
        if (Test-Path $pgoTree) { Get-Item $pgoTree }
    })
}

function Find-DependencyPgd {
    param([Parameter(Mandatory = $true)][string]$Module)

    foreach ($tree in Get-DependencyBuildTrees) {
        $pgd = Get-ChildItem $tree.FullName -Recurse -Filter ($Module + '.pgd') `
            -File -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($null -ne $pgd) {
            return $pgd
        }
    }
    return $null
}

function Get-BuildTreeRoot {
    param([Parameter(Mandatory = $true)][System.IO.FileInfo]$Pgd)

    $directory = $Pgd.Directory
    while ($null -ne $directory) {
        if (Test-Path (Join-Path $directory.FullName 'build.ninja')) {
            return $directory.FullName
        }
        $directory = $directory.Parent
    }
    throw ("No build.ninja found above {0}." -f $Pgd.FullName)
}

function Get-DumpbinPath {
    $cachePath = Join-Path $BuildDir 'CMakeCache.txt'
    $match = Select-String -Path $cachePath -Pattern '^CMAKE_LINKER:FILEPATH=(.+)$' |
        Select-Object -First 1
    if ($null -eq $match) {
        throw 'CMAKE_LINKER not found in CMakeCache.txt; cannot locate dumpbin.exe.'
    }
    $linker = $match.Matches[0].Groups[1].Value
    $candidate = Join-Path (Split-Path -Parent $linker) 'dumpbin.exe'
    if (-not (Test-Path $candidate)) {
        throw ("dumpbin.exe not found next to {0}." -f $linker)
    }
    return $candidate
}

function Ensure-RelinkNinja {
    $target = Join-Path $BuildToolsDir 'ninja.exe'
    if (Test-Path $target) {
        return $target
    }

    $tree = Get-DependencyBuildTrees | Select-Object -First 1
    if ($null -eq $tree) {
        throw 'No dependency build tree found; cannot locate ninja.exe.'
    }
    $cachePath = Join-Path $tree.FullName 'CMakeCache.txt'
    $match = Select-String -Path $cachePath -Pattern '^CMAKE_MAKE_PROGRAM:FILEPATH=(.+)$' |
        Select-Object -First 1
    if ($null -eq $match) {
        throw ("CMAKE_MAKE_PROGRAM not found in {0}." -f $cachePath)
    }
    New-Item -ItemType Directory -Path $BuildToolsDir -Force | Out-Null
    Copy-Item $match.Matches[0].Groups[1].Value $target -Force
    return $target
}

function Invoke-DependencyRelink {
    param(
        [Parameter(Mandatory = $true)][System.IO.FileInfo]$Pgd,
        [Parameter(Mandatory = $true)][string]$Target
    )

    $buildTree = Get-BuildTreeRoot -Pgd $Pgd
    $buildFile = Join-Path $buildTree 'build.ninja'
    $content = Get-Content $buildFile -Raw
    if ($content -notmatch [regex]::Escape('/USEPROFILE')) {
        if ($content -notmatch [regex]::Escape('/GENPROFILE')) {
            throw ("/GENPROFILE not found in {0}; the dependency was not instrumented." -f $buildFile)
        }
        $content = $content.Replace('/GENPROFILE', '/USEPROFILE')
        Set-Content -Path $buildFile -Value $content -NoNewline -Encoding ASCII
    }

    # vcpkg env provides the MSVC INCLUDE/LIB environment; its command parser
    # does not honor quoted paths, so keep the batch and build paths space-free.
    $ninja = Ensure-RelinkNinja
    $batch = Join-Path $BuildToolsDir 'relink.bat'
    Set-Content -Path $batch -Encoding ASCII -Value @(
        '@echo off',
        ('"{0}" -C "{1}" "{2}"' -f $ninja, $buildTree, $Target)
    )

    Write-Host ("Relinking {0} with /USEPROFILE" -f $Target)
    & $VcpkgExe env ('cmd /d /c ' + $batch) --triplet $PgoTriplet --overlay-triplets $TripletDir
    if ($LASTEXITCODE -ne 0) {
        throw ("Dependency relink failed for {0} (exit {1})." -f $Target, $LASTEXITCODE)
    }
}

function Copy-OptimizedDependency {
    param(
        [Parameter(Mandatory = $true)][string]$Source,
        [Parameter(Mandatory = $true)][string]$Name
    )

    $destinations = @(
        (Join-Path $DependencyBin $Name),
        (Join-Path $ReleaseDir $Name),
        (Join-Path (Join-Path $BuildDir 'tests') (Join-Path 'Release' $Name))
    )
    foreach ($destination in $destinations) {
        $directory = Split-Path -Parent $destination
        if (Test-Path $directory) {
            Copy-Item $Source $destination -Force
        }
    }
}

function Assert-NoPgoRuntime {
    $dumpbin = Get-DumpbinPath
    $files = @()
    foreach ($root in @($DependencyBin, $ReleaseDir,
            (Join-Path $BuildDir 'tests\Release'))) {
        if (Test-Path $root) {
            $files += @(Get-ChildItem $root -File |
                Where-Object { $_.Extension -in '.exe', '.dll' })
        }
    }
    foreach ($file in $files) {
        $output = & $dumpbin /dependents $file.FullName
        if ($LASTEXITCODE -ne 0) {
            throw ("dumpbin failed for {0} (exit {1})." -f $file.FullName, $LASTEXITCODE)
        }
        # Only the indented dependency lines matter; the "Dump of file" header
        # contains the filename and makes a whole-output match useless.
        $imports = @($output | Where-Object { $_ -match '^\s+\S+\.dll\s*$' })
        if ($imports -match 'pgort') {
            throw ("{0} still imports the PGO runtime." -f $file.FullName)
        }
    }
    Write-Host ("Verified {0} deployed binaries without a PGO runtime import." -f $files.Count)
}

# ---------------------------------------------------------------------------
# Phase 0: clean instrumentation state
# ---------------------------------------------------------------------------
Write-Host ''
Write-Host '=== Phase 0/4: reset PGO build and dependency caches ===' -ForegroundColor Cyan
Reset-PgoState
Write-Host ("Reset {0} and the {1} vcpkg build trees." -f $BuildDir, $PgoTriplet)

# ---------------------------------------------------------------------------
# Phase 1: instrumented build
# ---------------------------------------------------------------------------
Write-Host ''
Write-Host '=== Phase 1/4: build instrumented app and dependencies (/GENPROFILE) ===' -ForegroundColor Cyan
Invoke-Native -FilePath 'cmake' -Arguments @('--preset', 'pgo-generate')

# /GENPROFILE writes an empty Kessoku.pgd as part of the link. Delete the
# binary and database together, then force a relink so this run starts from a
# genuinely fresh database instead of inheriting earlier counters.
Remove-Item -Path $PgdPath -Force -ErrorAction SilentlyContinue
Remove-Item -Path $AppPath -Force -ErrorAction SilentlyContinue
Invoke-Native -FilePath 'cmake' -Arguments @('--build', '--preset', 'pgo-generate')
Assert-ProjectUsesPgoFlag -Flag '/GENPROFILE:PGD='

if (-not (Test-Path $AppPath)) {
    throw "Instrumented app not found at $AppPath."
}
if (-not (Test-Path $PgdPath)) {
    throw "The /GENPROFILE link did not create $PgdPath."
}

# ---------------------------------------------------------------------------
# Phase 2: training library and UI session
# ---------------------------------------------------------------------------
Write-Host ''
Write-Host '=== Phase 2/4: generate training media and drive the UI ===' -ForegroundColor Cyan

Get-Process -Name Kessoku -ErrorAction SilentlyContinue | ForEach-Object {
    try {
        if ($_.Path -eq $AppPath) {
            Stop-Process -Id $_.Id -Force
        }
    }
    catch {
        # Process exited between enumeration and inspection.
    }
}
Start-Sleep -Milliseconds 300
if (@(Get-Process -Name Kessoku -ErrorAction SilentlyContinue).Count -gt 0) {
    throw 'Another Kessoku instance is running. Close it before training; ' +
        'it would contend for the WASAPI exclusive endpoint.'
}

New-Item -ItemType Directory -Path $PgoDir -Force | Out-Null
foreach ($root in @($PgoDir, $ReleaseDir, $DependencyBin,
        (Join-Path $BuildDir 'tests\Release'))) {
    if (Test-Path $root) {
        Get-ChildItem -Path $root -Filter '*!*.pgc' -File -ErrorAction SilentlyContinue |
            Remove-Item -Force
    }
}

New-TrainingLibrary
Invoke-FlacTrainingSession -Folder $FlacTrainingRoot
Invoke-WavTrainingSession -Folder $WavTrainingRoot

# ---------------------------------------------------------------------------
# Phase 3: merge profile data and relink dependencies
# ---------------------------------------------------------------------------
Write-Host ''
Write-Host '=== Phase 3/4: merge app and dependency profiles ===' -ForegroundColor Cyan

$pgcRoots = @($PgoDir, $ReleaseDir, $DependencyBin,
    (Join-Path $BuildDir 'tests\Release')) | Where-Object { Test-Path $_ }
$pgcFiles = @($pgcRoots | ForEach-Object {
        Get-ChildItem -Path $_ -Filter '*!*.pgc' -File -ErrorAction SilentlyContinue
    } | Sort-Object FullName -Unique)
if ($pgcFiles.Count -eq 0) {
    throw "Training produced no profile files under $BuildDir."
}
Write-Host ("Collected {0} profile file(s): {1}" -f $pgcFiles.Count,
    (($pgcFiles | ForEach-Object { $_.Name }) -join ', '))

$pgomgr = Get-PgomgrPath
$pgcByModule = $pgcFiles | Group-Object { ($_.BaseName -replace '!\d+$', '') }

$appProfile = $pgcByModule | Where-Object Name -eq 'Kessoku' | Select-Object -First 1
if ($null -eq $appProfile) {
    throw 'Training produced no Kessoku profile files.'
}
$mergeArguments = @('/merge') +
    @($appProfile.Group | ForEach-Object { $_.FullName }) +
    @($PgdPath)
Invoke-Native -FilePath $pgomgr -Arguments $mergeArguments

# Keep the per-function report in the build tree instead of flooding the
# console; the /USEPROFILE links print the aggregate coverage.
$summaryPath = Join-Path $PgoDir 'summary.txt'
& $pgomgr /summary $PgdPath | Set-Content -Path $summaryPath -Encoding UTF8
if ($LASTEXITCODE -ne 0) {
    throw ("pgomgr /summary exited with code {0}" -f $LASTEXITCODE)
}
Write-Host ("App profile summary: {0}" -f $summaryPath)

if (-not (Test-Path $PgdPath)) {
    throw "Profile database was not created at $PgdPath."
}
$pgdSize = (Get-Item $PgdPath).Length
if ($pgdSize -le 0) {
    throw "Profile database at $PgdPath is empty."
}
Write-Host ("App profile database: {0} ({1:N0} bytes)" -f $PgdPath, $pgdSize)

$dependencyProfiles = @($pgcByModule | Where-Object Name -ne 'Kessoku')
if ($dependencyProfiles.Count -eq 0) {
    Write-Warning 'Training produced no dependency profile files; the DLLs will be relinked without training data.'
}

$installedDependencies = @(Get-ChildItem -Path $DependencyBin -Filter '*.dll' -File)
if ($installedDependencies.Count -eq 0) {
    throw "No dependency DLLs found at $DependencyBin."
}

foreach ($dependency in $installedDependencies) {
    $module = $dependency.BaseName
    $pgd = Find-DependencyPgd -Module $module
    if ($null -eq $pgd) {
        # This DLL was not built by an instrumented port.
        continue
    }

    $profile = $dependencyProfiles | Where-Object Name -eq $module | Select-Object -First 1
    if ($null -ne $profile) {
        Write-Host ("Merging {0} profile file(s) into {1}" -f $profile.Count, $pgd.FullName)
        $dependencyMergeArguments = @('/merge') +
            @($profile.Group | ForEach-Object { $_.FullName }) +
            @($pgd.FullName)
        Invoke-Native -FilePath $pgomgr -Arguments $dependencyMergeArguments

        $dependencySummary = Join-Path $PgoDir ("summary-{0}.txt" -f $module)
        & $pgomgr /summary $pgd.FullName | Set-Content -Path $dependencySummary -Encoding UTF8
        if ($LASTEXITCODE -ne 0) {
            throw ("pgomgr /summary exited with code {0}" -f $LASTEXITCODE)
        }
        Write-Host ("{0} profile summary: {1}" -f $module, $dependencySummary)
    }

    $optimizedDll = [System.IO.Path]::ChangeExtension($pgd.FullName, '.dll')
    if (-not (Test-Path $optimizedDll)) {
        throw ("Profile database {0} has no matching DLL." -f $pgd.FullName)
    }
    $buildTree = Get-BuildTreeRoot -Pgd $pgd
    $target = $optimizedDll.Substring($buildTree.Length + 1).Replace('/', '\')
    Invoke-DependencyRelink -Pgd $pgd -Target $target
    Copy-OptimizedDependency -Source $optimizedDll -Name $dependency.Name
    Write-Host ("Deployed optimized {0}" -f $dependency.Name)
}

# The .pgc inputs are now in their databases. Remove them so a later
# /USEPROFILE link cannot rediscover and double-count them.
$pgcFiles | Remove-Item -Force

Write-Host 'App and dependency DLLs merged and relinked with /USEPROFILE.'

# ---------------------------------------------------------------------------
# Phase 4: optimized build, tests, smoke test
# ---------------------------------------------------------------------------
Write-Host ''
Write-Host '=== Phase 4/4: build optimized app (/USEPROFILE) ===' -ForegroundColor Cyan
Invoke-Native -FilePath 'cmake' -Arguments @('--preset', 'pgo-use')
Assert-ProjectUsesPgoFlag -Flag '/USEPROFILE:PGD='

# Force a relink: the /USEPROFILE flag only takes effect at link time, and the
# instrumented binary must not survive as the final artifact.
Remove-Item -Path $AppPath -Force
Invoke-Native -FilePath 'cmake' -Arguments @('--build', '--preset', 'pgo-use')

# Test targets may not have relinked after the dependency relink, so their
# app-local DLL copies can still be the instrumented ones from phase 1.
foreach ($dependency in $installedDependencies) {
    $pgd = Find-DependencyPgd -Module $dependency.BaseName
    if ($null -eq $pgd) {
        continue
    }
    Copy-OptimizedDependency -Source (
        [System.IO.Path]::ChangeExtension($pgd.FullName, '.dll')) -Name $dependency.Name
}

Assert-NoPgoRuntime

# The profiling runtime is only needed while the instrumented binaries run.
# No deployed binary imports it now, so remove it from the build tree.
Get-ChildItem $BuildDir -Recurse -Filter 'pgort140.dll' -File -ErrorAction SilentlyContinue |
    Remove-Item -Force
Write-Host 'Removed pgort140.dll from the build tree.'

if (-not $SkipTests) {
    Write-Host ''
    Write-Host '=== Tests: optimized Release build ===' -ForegroundColor Cyan
    Invoke-Native -FilePath 'ctest' -Arguments @('--preset', 'pgo-use')
}

if (-not $SkipSmokeTest) {
    Write-Host ''
    Write-Host '=== Smoke test: optimized app playback ===' -ForegroundColor Cyan
    Invoke-SmokeSession -Folder $FlacTrainingRoot
    Write-Host 'Optimized app played a FLAC track and exited 0.'
}

Write-Host ''
Write-Host 'PGO workflow complete.' -ForegroundColor Green
Write-Host ("Optimized app: {0}" -f $AppPath)
Write-Host ("App profile database: {0}" -f $PgdPath)
Write-Host ("Optimized dependencies: {0}" -f (($installedDependencies |
    ForEach-Object { $_.Name }) -join ', '))
