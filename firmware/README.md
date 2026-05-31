# Firmware

This firmware targets M5Stack StickS3 with PlatformIO.

1. Copy `src/secrets.example.h` to `src/secrets.h`.
2. Fill in Wi-Fi, `FEED_URL`, and `DEVICE_TOKEN`.
3. Build and upload:

```powershell
$env:PLATFORMIO_CORE_DIR="D:\Docu\simple_day\.platformio"
.venv\Scripts\platformio.exe run -d firmware -e sticks3
.venv\Scripts\platformio.exe run -d firmware -e sticks3 -t upload --upload-port COM4
.venv\Scripts\platformio.exe device monitor -d firmware -p COM4 -b 115200
```

If PlatformIO does not recognize the StickS3-specific board yet, this project builds against the generic `esp32-s3-devkitc-1` target and lets M5Unified initialize the device at runtime.
