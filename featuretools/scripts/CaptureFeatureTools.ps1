param(
  [Parameter(Mandatory = $true)]
  [string]$Executable,

  [Parameter(Mandatory = $true)]
  [string]$Output,

  [int]$WarmupSeconds = 3
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

Add-Type -AssemblyName System.Drawing
Add-Type @'
using System;
using System.Runtime.InteropServices;

public static class FeatureToolsCaptureNative
{
    [StructLayout(LayoutKind.Sequential)]
    public struct RECT
    {
        public int Left;
        public int Top;
        public int Right;
        public int Bottom;
    }

    [DllImport("user32.dll")]
    public static extern bool GetWindowRect(IntPtr hWnd, out RECT rect);

    [DllImport("user32.dll")]
    public static extern bool PrintWindow(IntPtr hWnd, IntPtr hdc, uint flags);

    [DllImport("user32.dll")]
    public static extern bool SetForegroundWindow(IntPtr hWnd);
}
'@

$resolvedExecutable = (Resolve-Path -LiteralPath $Executable).Path
$resolvedOutput = [System.IO.Path]::GetFullPath($Output)
$workingDirectory = Split-Path -Parent $resolvedExecutable
$process = Start-Process -FilePath $resolvedExecutable -WorkingDirectory $workingDirectory -PassThru

try {
  $handle = [IntPtr]::Zero
  for ($attempt = 0; $attempt -lt 50; $attempt++) {
    Start-Sleep -Milliseconds 100
    $process.Refresh()
    if ($process.HasExited) {
      throw "FeatureTools exited before visual capture with code $($process.ExitCode)."
    }
    if ($process.MainWindowHandle -ne [IntPtr]::Zero) {
      $handle = $process.MainWindowHandle
      break
    }
  }

  if ($handle -eq [IntPtr]::Zero) {
    throw 'FeatureTools did not create a main window within five seconds.'
  }

  Start-Sleep -Seconds $WarmupSeconds
  $process.Refresh()
  if ($process.HasExited) {
    throw "FeatureTools exited during warmup with code $($process.ExitCode)."
  }
  if ($process.MainWindowHandle -ne [IntPtr]::Zero) {
    $handle = $process.MainWindowHandle
  }

  [FeatureToolsCaptureNative+RECT]$windowRect = New-Object FeatureToolsCaptureNative+RECT
  if (-not [FeatureToolsCaptureNative]::GetWindowRect($handle, [ref]$windowRect)) {
    throw 'GetWindowRect failed.'
  }

  $width = $windowRect.Right - $windowRect.Left
  $height = $windowRect.Bottom - $windowRect.Top
  if ($width -le 0 -or $height -le 0) {
    throw "Invalid window bounds: ${width}x${height}."
  }

  $bitmap = New-Object System.Drawing.Bitmap($width, $height)
  $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
  try {
    $hdc = $graphics.GetHdc()
    try {
      $captured = [FeatureToolsCaptureNative]::PrintWindow($handle, $hdc, 2)
    }
    finally {
      $graphics.ReleaseHdc($hdc)
    }

    if (-not $captured) {
      [FeatureToolsCaptureNative]::SetForegroundWindow($handle) | Out-Null
      Start-Sleep -Milliseconds 300
      $graphics.CopyFromScreen($windowRect.Left, $windowRect.Top, 0, 0,
        (New-Object System.Drawing.Size($width, $height)))
    }

    $outputDirectory = Split-Path -Parent $resolvedOutput
    if ($outputDirectory -and -not (Test-Path -LiteralPath $outputDirectory)) {
      New-Item -ItemType Directory -Path $outputDirectory | Out-Null
    }
    $bitmap.Save($resolvedOutput, [System.Drawing.Imaging.ImageFormat]::Png)
  }
  finally {
    $graphics.Dispose()
    $bitmap.Dispose()
  }

  Write-Output "Captured FeatureTools window to $resolvedOutput"
}
finally {
  if (-not $process.HasExited) {
    Stop-Process -Id $process.Id
  }
}
