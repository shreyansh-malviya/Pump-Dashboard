# Public control website

[index.html](index.html) is a single-file, static control panel for the pump. It talks to
your MQTT broker (HiveMQ Cloud) **over secure WebSockets straight from the browser** — no
backend, no server code. Host it anywhere static and it works from anywhere in the world,
because both the ESP32 and this page connect *outbound* to the broker (no port-forwarding).

## How the pieces connect

```
  Browser (this page) ──wss──►  HiveMQ Cloud  ◄──TLS──  ESP32 (pump_control.ino)
        publish pump/cmd  ─────────────────────────►  subscribes
        subscribe pump/state ◄───────────────────────  publishes retained state
```

- **Command topic** `pump/cmd` — payload `"<id>:<cmd>"`, e.g. `0:start`.
  The ESP32 trusts all commands arriving here, so you **must** configure your MQTT broker to only allow authorized admin users to publish to this topic.
- **State topic** `pump/state` — retained JSON the ESP32 publishes; drives the live cards.
- **Status topic** `pump/status` — retained `online`/`offline` (MQTT last-will).

## Using it

Open the page, then enter:
- **Broker host** — e.g. `xxxxxxxx.s1.eu.hivemq.cloud` (your HiveMQ Cloud cluster URL).
- **WebSocket port** — `8884` for HiveMQ Cloud (secure WebSocket). Firmware uses `8883` for
  native MQTT; the browser must use the WebSocket port.
- **MQTT username / password** — your broker credentials for the dashboard user.

Tick **Remember on this device** to keep these in this browser's `localStorage` (convenience
only; skip it on shared computers).

## Publishing it (pick one — all free)

No credentials are baked into the file, so it is safe to host publicly.

- **GitHub Pages:** push this repo, then in the repo's *Settings → Pages* serve from the
  `main` branch. To publish only this folder, either move `index.html` to `/docs` and select
  `/docs`, or use a Pages action. URL: `https://<user>.github.io/<repo>/web/`.
- **Netlify / Vercel / Cloudflare Pages:** drag-and-drop the `web/` folder, or point the
  project at this repo. Instant HTTPS URL.
- **Local test:** `python -m http.server 8099` in this folder, then open
  `http://127.0.0.1:8099/`. (Opening the file directly with `file://` also works in most
  browsers.)

## Security notes

- The browser authenticates to the broker with credentials the viewer types; these credentials are
  the **only** access control for remote control. You **must** create a separate MQTT user for the web dashboard (e.g. `PumpAdmin`) with an ACL that only allows publish to `pump/cmd` and subscribe to `pump/state`/`pump/status`. Ensure the ESP32's own MQTT user is not allowed to publish commands.
- If these web credentials leak, you simply rotate/revoke them on your MQTT broker without having to re-flash the ESP32.
- The firmware's hardware safety (min-off-time, max-run lockout, boot-safe-off, and the
  physical stop button) still applies to every command from this page.
