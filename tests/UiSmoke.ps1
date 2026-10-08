[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$Executable,
    [string]$OutputDirectory = 'build/ui-smoke'
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$sourceExecutable = (Resolve-Path -LiteralPath $Executable).Path
$outputPath = [IO.Path]::GetFullPath($OutputDirectory)
$null = New-Item -ItemType Directory -Path $outputPath -Force
$runDirectory = Join-Path $outputPath ('run-' + [Guid]::NewGuid().ToString('N'))
$null = New-Item -ItemType Directory -Path $runDirectory
$testExecutable = Join-Path $runDirectory 'viewer.exe'
$settingsPath = Join-Path $runDirectory 'settings.json'
$report = [Collections.Generic.List[string]]::new()
$viewerProcess = $null
$failure = $null

function Write-Report([string]$Message) {
    $line = '{0:O} {1}' -f [DateTime]::UtcNow, $Message
    $report.Add($line)
    Write-Host $line
}

function Assert-ViewerAlive {
    $viewerProcess.Refresh()
    if ($viewerProcess.HasExited) {
        throw "Viewer exited unexpectedly with code $($viewerProcess.ExitCode)."
    }
}

function Wait-Setting([string]$Name, [bool]$Expected) {
    $timer = [Diagnostics.Stopwatch]::StartNew()
    do {
        Assert-ViewerAlive
        # A settings save truncates and rewrites the file; retry incomplete JSON.
        $settings = $null
        try { $settings = Get-Content -LiteralPath $settingsPath -Raw | ConvertFrom-Json }
        catch { }
        if ($null -ne $settings) {
            $property = $settings.PSObject.Properties[$Name]
            if ($null -ne $property -and $property.Value -is [bool] -and $property.Value -eq $Expected) {
                Write-Report "PASS: $Name = $Expected"
                return
            }
        }
        Start-Sleep -Milliseconds 100
    } while ($timer.Elapsed.TotalSeconds -lt 5)
    throw "Timed out waiting for persisted $Name = $Expected."
}

function Send-Shortcut([int]$VirtualKey, [int]$ScanCode) {
    Assert-ViewerAlive
    $windowHandle = $viewerProcess.MainWindowHandle
    if ($windowHandle -eq [IntPtr]::Zero) { throw 'Viewer has no main window.' }
    $down = [long](($ScanCode -shl 16) -bor 1)
    $up = $down -bor ([long]3 -shl 30)
    if (-not [CaptureViewerSmoke.Native]::PostMessage($windowHandle, 0x0100, [UIntPtr]::new([uint32]$VirtualKey), [IntPtr]::new($down))) {
        throw 'Failed to post key-down message.'
    }
    if (-not [CaptureViewerSmoke.Native]::PostMessage($windowHandle, 0x0101, [UIntPtr]::new([uint32]$VirtualKey), [IntPtr]::new($up))) {
        throw 'Failed to post key-up message.'
    }
}

function Save-WindowScreenshot {
    $bitmap = $null
    $graphics = $null
    try {
        Add-Type -AssemblyName System.Drawing
        Add-Type -AssemblyName System.Windows.Forms
        $rect = [CaptureViewerSmoke.Native+RECT]::new()
        if (-not [CaptureViewerSmoke.Native]::GetWindowRect($viewerProcess.MainWindowHandle, [ref]$rect)) {
            throw 'Cannot read the viewer window bounds.'
        }
        $windowBounds = [Drawing.Rectangle]::FromLTRB($rect.Left, $rect.Top, $rect.Right, $rect.Bottom)
        $bounds = [Drawing.Rectangle]::Intersect($windowBounds, [Windows.Forms.SystemInformation]::VirtualScreen)
        if ($bounds.Width -le 0 -or $bounds.Height -le 0) { throw 'No visible desktop area for the viewer.' }
        $bitmap = [Drawing.Bitmap]::new($bounds.Width, $bounds.Height)
        $graphics = [Drawing.Graphics]::FromImage($bitmap)
        $graphics.CopyFromScreen($bounds.Location, [Drawing.Point]::Empty, $bounds.Size)
        $bitmap.Save((Join-Path $outputPath 'window.png'), [Drawing.Imaging.ImageFormat]::Png)
        Write-Report "Screenshot saved: window.png ($($bounds.Width) x $($bounds.Height)); desktop capture is best-effort."
    }
    catch {
        Write-Report "WARNING: Screenshot unavailable on this runner desktop: $($_.Exception.Message)"
    }
    finally {
        if ($null -ne $graphics) { $graphics.Dispose() }
        if ($null -ne $bitmap) { $bitmap.Dispose() }
    }
}

try {
    Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
namespace CaptureViewerSmoke {
    public static class Native {
        [StructLayout(LayoutKind.Sequential)]
        public struct RECT { public int Left, Top, Right, Bottom; }
        [DllImport("user32.dll", SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        public static extern bool PostMessage(IntPtr hwnd, uint message, UIntPtr wParam, IntPtr lParam);
        [DllImport("user32.dll", SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        public static extern bool GetWindowRect(IntPtr hwnd, out RECT rect);
    }
}
'@
    Copy-Item -LiteralPath $sourceExecutable -Destination $testExecutable
    @{
        audioPlaybackEnabled = $false
        videoBorderlessWindowed = $false
        videoFullscreen = $false
        videoAllowResizing = $true
        windowClientWidth = 1000
        windowClientHeight = 700
        windowPosX = 0
        windowPosY = 0
        hasWindowPlacement = $true
        windowWasMaximized = $false
        showLatencyOverlay = $true
        vsyncEnabled = $false
    } | ConvertTo-Json | Set-Content -LiteralPath $settingsPath -Encoding utf8

    Write-Report "Starting isolated viewer: $testExecutable"
    $viewerProcess = Start-Process -FilePath $testExecutable -ArgumentList '--no-audio' `
        -WorkingDirectory $runDirectory -PassThru `
        -RedirectStandardOutput (Join-Path $runDirectory 'stdout.log') `
        -RedirectStandardError (Join-Path $runDirectory 'stderr.log')
    $timer = [Diagnostics.Stopwatch]::StartNew()
    $windowReady = $false
    do {
        Assert-ViewerAlive
        if ($viewerProcess.MainWindowHandle -ne [IntPtr]::Zero -and $viewerProcess.MainWindowTitle -like 'CaptureViewer*') {
            $windowReady = $true
            break
        }
        Start-Sleep -Milliseconds 100
    } while ($timer.Elapsed.TotalSeconds -lt 10)
    if (-not $windowReady) { throw 'No CaptureViewer window appeared within 10 seconds.' }
    Write-Report "PASS: main window title '$($viewerProcess.MainWindowTitle)', PID $($viewerProcess.Id)."

    Send-Shortcut 0x7A 0x57 # F11
    Wait-Setting 'videoFullscreen' $true
    Send-Shortcut 0x7A 0x57
    Wait-Setting 'videoFullscreen' $false
    Send-Shortcut 0x79 0x44 # F10
    Wait-Setting 'showLatencyOverlay' $false
    Send-Shortcut 0x79 0x44
    Wait-Setting 'showLatencyOverlay' $true
    Start-Sleep -Milliseconds 500
    Assert-ViewerAlive
    Save-WindowScreenshot
    Assert-ViewerAlive
    Write-Report 'PASS: no-device UI startup, fullscreen round trip, and overlay round trip.'
    Write-Report 'Capture hardware, audio, input FPS, and end-to-end latency are not tested by this smoke check.'
}
catch {
    $failure = $_
    Write-Report "FAIL: $($_.Exception.Message)"
}
finally {
    if ($null -ne $viewerProcess) {
        try {
            $viewerProcess.Refresh()
            if (-not $viewerProcess.HasExited) {
                # Terminate only the exact process created by this script.
                $viewerProcess.Kill()
                $null = $viewerProcess.WaitForExit(5000)
            }
        }
        catch { Write-Report "WARNING: Process cleanup: $($_.Exception.Message)" }
        $viewerProcess.Dispose()
    }
    foreach ($name in @('settings.json', 'viewer.log', 'stdout.log', 'stderr.log')) {
        $source = Join-Path $runDirectory $name
        if (Test-Path -LiteralPath $source) {
            try { Copy-Item -LiteralPath $source -Destination (Join-Path $outputPath $name) -Force }
            catch { Write-Report "WARNING: Could not preserve ${name}: $($_.Exception.Message)" }
        }
    }
    $report | Set-Content -LiteralPath (Join-Path $outputPath 'report.txt') -Encoding utf8
}

if ($null -ne $failure) { throw $failure }
