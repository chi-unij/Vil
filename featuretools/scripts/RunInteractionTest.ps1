param(
  [Parameter(Mandatory = $true)]
  [string]$Executable,

  [int]$TimeoutSeconds = 30
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$resolvedExecutable = (Resolve-Path -LiteralPath $Executable).Path
$workingDirectory = Split-Path -Parent $resolvedExecutable
$reportPath = Join-Path $workingDirectory 'featuretools-self-test.log'
$errorPath = Join-Path $workingDirectory 'featuretools-error.log'

foreach ($path in @($reportPath, $errorPath)) {
  if (Test-Path -LiteralPath $path) {
    Remove-Item -LiteralPath $path
  }
}

$process = Start-Process -FilePath $resolvedExecutable -ArgumentList '--self-test' `
  -WorkingDirectory $workingDirectory -WindowStyle Hidden -PassThru

try {
  $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
  while (-not $process.HasExited -and [DateTime]::UtcNow -lt $deadline) {
    Start-Sleep -Milliseconds 100
    $process.Refresh()
  }
  if (-not $process.HasExited) {
    Stop-Process -Id $process.Id
    throw "FeatureTools self-test exceeded ${TimeoutSeconds} seconds."
  }
  if ($process.ExitCode -ne 0) {
    $details = if (Test-Path -LiteralPath $errorPath) {
      Get-Content -LiteralPath $errorPath -Raw
    } else {
      'No runtime error log was produced.'
    }
    throw "FeatureTools self-test failed with code $($process.ExitCode).`n$details"
  }
  if (-not (Test-Path -LiteralPath $reportPath)) {
    throw 'FeatureTools did not write its self-test report.'
  }
  $report = Get-Content -LiteralPath $reportPath -Raw
  if ($report -notmatch '^PASS') {
    throw "Unexpected FeatureTools self-test report:`n$report"
  }
  Write-Output "[PASS] DX12 render loop, TreeView toggle, buttons, pass changes, all-off, restore, and debug-layer validation."
  Write-Output $report.Trim()
}
finally {
  if (-not $process.HasExited) {
    Stop-Process -Id $process.Id
  }
}
