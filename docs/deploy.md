# Server deployment

The first version can run without a domain:

```bash
cd server
cp .env.example .env
vi .env
docker compose up -d --build
```

Open the web UI from your phone or computer:

```text
http://SERVER_PUBLIC_IP:18080
```

The StickS3 feed URL is:

```text
http://SERVER_PUBLIC_IP:18080/api/device/feed
```

The firmware appends `?token=DEVICE_TOKEN` automatically.

For local Windows testing, run:

```powershell
.\scripts\configure.ps1
.\scripts\start_server.ps1
```

Then open `http://127.0.0.1:18080` on this PC or `http://PC_LAN_IP:18080` from your phone.

## Firewall

Open TCP port `18080` on the cloud server security group and local firewall.

## Tokens

- `SIMPLE_DAY_ADMIN_TOKEN`: password for the web UI.
- `SIMPLE_DAY_DEVICE_TOKEN`: read-only token for the StickS3.

Use long random values for both tokens. HTTP is acceptable for early testing, but the values travel in clear text. When you add a domain, put Caddy or another reverse proxy in front and switch to HTTPS.
