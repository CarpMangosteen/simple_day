param(
  [string]$Port = "COM4",
  [switch]$Monitor
)

$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $PSScriptRoot
$env:PLATFORMIO_CORE_DIR = Join-Path $Root ".platformio"
$Pio = Join-Path $Root ".venv\Scripts\platformio.exe"

if (-not (Test-Path $Pio)) {
  $Pio = "platformio"
}

& $Pio run -d (Join-Path $Root "firmware") -e sticks3
& $Pio run -d (Join-Path $Root "firmware") -e sticks3 -t upload --upload-port $Port

if ($Monitor) {
  & $Pio device monitor -d (Join-Path $Root "firmware") -p $Port -b 115200
}
