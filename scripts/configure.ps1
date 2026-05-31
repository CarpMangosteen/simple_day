param(
  [string]$WifiSsid,
  [string]$WifiPassword,
  [string]$ServerBaseUrl,
  [string]$AdminToken,
  [string]$DeviceToken
)

$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $PSScriptRoot

function New-Token {
  $bytes = New-Object byte[] 24
  $rng = [System.Security.Cryptography.RandomNumberGenerator]::Create()
  try {
    $rng.GetBytes($bytes)
  } finally {
    $rng.Dispose()
  }
  return [Convert]::ToBase64String($bytes).TrimEnd("=").Replace("+", "-").Replace("/", "_")
}

function Read-PlainSecret($Prompt) {
  $secure = Read-Host $Prompt -AsSecureString
  $ptr = [Runtime.InteropServices.Marshal]::SecureStringToBSTR($secure)
  try {
    return [Runtime.InteropServices.Marshal]::PtrToStringBSTR($ptr)
  } finally {
    [Runtime.InteropServices.Marshal]::ZeroFreeBSTR($ptr)
  }
}

function Escape-CString([string]$Value) {
  return $Value.Replace("\", "\\").Replace('"', '\"')
}

if (-not $WifiSsid) {
  $WifiSsid = Read-Host "Wi-Fi SSID"
}
if (-not $WifiPassword) {
  $WifiPassword = Read-PlainSecret "Wi-Fi password"
}
if (-not $ServerBaseUrl) {
  $ServerBaseUrl = Read-Host "Server base URL, for example http://123.45.67.89:18080"
}
if (-not $AdminToken) {
  $AdminToken = New-Token
}
if (-not $DeviceToken) {
  $DeviceToken = New-Token
}

$ServerBaseUrl = $ServerBaseUrl.TrimEnd("/")
$FeedUrl = "$ServerBaseUrl/api/device/feed"

$serverEnv = @"
SIMPLE_DAY_ADMIN_TOKEN=$AdminToken
SIMPLE_DAY_DEVICE_TOKEN=$DeviceToken
SIMPLE_DAY_DB=/data/simple_day.sqlite
"@

$serverLocalEnv = @"
SIMPLE_DAY_ADMIN_TOKEN=$AdminToken
SIMPLE_DAY_DEVICE_TOKEN=$DeviceToken
SIMPLE_DAY_DB=data/simple_day.sqlite
"@

$secrets = @"
#pragma once

static const char *WIFI_SSID = "$(Escape-CString $WifiSsid)";
static const char *WIFI_PASSWORD = "$(Escape-CString $WifiPassword)";
static const char *FEED_URL = "$(Escape-CString $FeedUrl)";
static const char *DEVICE_TOKEN = "$(Escape-CString $DeviceToken)";
"@

Set-Content -Path (Join-Path $Root "server\.env") -Value $serverEnv -Encoding UTF8
Set-Content -Path (Join-Path $Root "server\.env.local") -Value $serverLocalEnv -Encoding UTF8
Set-Content -Path (Join-Path $Root "firmware\src\secrets.h") -Value $secrets -Encoding UTF8

Write-Host "Wrote server\.env, server\.env.local, and firmware\src\secrets.h"
Write-Host "Admin password: $AdminToken"
Write-Host "Device feed: $FeedUrl"
