param(
  [int]$Port = 18080
)

$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $PSScriptRoot
$ServerDir = Join-Path $Root "server"
$EnvFile = Join-Path $ServerDir ".env.local"

if (-not (Test-Path $EnvFile)) {
  $EnvFile = Join-Path $ServerDir ".env"
}

if (Test-Path $EnvFile) {
  Get-Content $EnvFile | ForEach-Object {
    if ($_ -match "^\s*([^#][^=]+)=(.*)$") {
      [Environment]::SetEnvironmentVariable($matches[1].Trim(), $matches[2].Trim(), "Process")
    }
  }
} else {
  $env:SIMPLE_DAY_ADMIN_TOKEN = "admin-test"
  $env:SIMPLE_DAY_DEVICE_TOKEN = "device-test"
  $env:SIMPLE_DAY_DB = "data/simple_day.sqlite"
}

$Python = Join-Path $Root ".venv\Scripts\python.exe"
if (-not (Test-Path $Python)) {
  $Python = "python"
}

Set-Location $ServerDir
& $Python -m uvicorn app.main:app --host 0.0.0.0 --port $Port
