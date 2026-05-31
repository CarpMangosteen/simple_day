# Flashing StickS3

The connected board was detected on `COM4`.

## Install PlatformIO

```powershell
python -m pip install -U platformio
pio --version
```

If the default `python` is too old, use a newer Python and run the same command.
In this workspace, PlatformIO was installed into `.venv`, so commands can also be run as `.venv\Scripts\platformio.exe`.

To keep PlatformIO packages inside the project directory on this machine:

```powershell
$env:PLATFORMIO_CORE_DIR="D:\Docu\simple_day\.platformio"
```

## Configure secrets

The easiest path is the setup helper:

```powershell
.\scripts\configure.ps1
```

It writes `server\.env`, `server\.env.local`, and `firmware\src\secrets.h`.
You can also copy `firmware\src\secrets.example.h` manually and edit:

```cpp
static const char *WIFI_SSID = "your wifi";
static const char *WIFI_PASSWORD = "your password";
static const char *FEED_URL = "http://SERVER_PUBLIC_IP:18080/api/device/feed";
static const char *DEVICE_TOKEN = "same-as-server-device-token";
```

## Build and upload

```powershell
.\scripts\flash.ps1 -Port COM4
.\scripts\flash.ps1 -Port COM4 -Monitor
```

After upload, the device should boot into `Simple Day`, connect to Wi-Fi, fetch the feed, and show the four pages.
