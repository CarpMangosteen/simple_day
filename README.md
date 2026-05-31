# Simple Day StickS3

Simple Day turns an M5Stack StickS3 into a four-page dynamic note:

- `NEXT`: the next upcoming event
- `TODAY`: today's events
- `TODO`: open tasks
- `DEADLINE`: upcoming deadlines

The v1 system is intentionally small: a FastAPI + SQLite server provides a phone-friendly web UI and a device JSON feed, while the StickS3 firmware connects to Wi-Fi and refreshes the display on boot and every 15 minutes.

## Layout

- `server/`: FastAPI app, static web UI, SQLite storage, Docker deployment
- `firmware/`: PlatformIO/Arduino firmware for StickS3
- `docs/`: deployment and flashing notes

Start with [server deployment](docs/deploy.md) and [firmware flashing](docs/flashing.md).

## Quick local flow

```powershell
.\scripts\configure.ps1
.\scripts\start_server.ps1
.\scripts\flash.ps1 -Port COM4
```

`configure.ps1` writes ignored local secret files for the server and firmware. It prints the admin password once; keep it somewhere safe.
