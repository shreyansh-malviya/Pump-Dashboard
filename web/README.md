# Public control website

[index.html](index.html) is a single-file, static, dependency-free control panel for the pump
(its only external load is the MQTT-over-WebSockets browser library from a CDN). It talks to
your MQTT broker (HiveMQ Cloud) **over secure WebSockets straight from the browser** — no
backend, no server code, no build step. Host it anywhere static and it works from anywhere in
the world, because both the ESP32 and this page connect *outbound* to the broker (no
port-forwarding).

Deliberately minimal: plain HTML, no fonts, no gradients. It stays usable even with the small
inline CSS removed.

## How the pieces connect

```
  Browser (this page) --wss-->  HiveMQ Cloud  <--TLS--  ESP32 (pump_control.ino)
        publish pump/cmd  ------------------------------>  subscribes
        subscribe pump/state <--------------------------  publishes retained state
```

- **Command topic** `pump/cmd` — payload `"<id>:<cmd>"`, e.g. `0:start`.
  The ESP32 trusts every command on this topic, so you **must** use your broker's user
  permissions (ACLs) to control who may publish here (see Security below).
- **State topic** `pump/state` — retained JSON the ESP32 publishes; drives the live table.
- **Status topic** `pump/status` — retained `online`/`offline` (MQTT last-will).

## Using it

Open the page, then enter:
- **Broker host** — e.g. `xxxxxxxx.s1.eu.hivemq.cloud` (your HiveMQ Cloud cluster URL).
- **WebSocket port** — `8884` for HiveMQ Cloud (secure WebSocket). The firmware uses `8883`
  for native MQTT; the browser must use the WebSocket port.
- **MQTT username / password** — the broker credentials for the control user.

Tick **Remember on this device** to keep these in this browser's `localStorage` (convenience
only; skip it on shared computers). No credentials are stored in the file itself.

## Deploy to Vercel (what this repo is set up for)

The site is static, so there is no build step.

1. Push this repo to GitHub (done — it's private).
2. In Vercel: **Add New… → Project**, import this repository.
3. Set **Root Directory** to `web`.
4. **Framework Preset:** Other. Leave **Build Command** and **Output Directory** empty.
5. Deploy. Your URL will be `https://<project>.vercel.app/`, serving `web/index.html`.

Alternatives (all static, all free): **GitHub Pages** (Settings → Pages; serve `/web`),
**Netlify** or **Cloudflare Pages** (drag-and-drop the `web/` folder). Local test:
`python -m http.server 8099` in this folder, then open `http://127.0.0.1:8099/`.

## Security

- The browser authenticates to the broker with the credentials the viewer types; treat those
  as the access control for remote control. Create a **dedicated control user** on the broker
  (e.g. `PumpAdmin`) whose ACL only allows **publish to `pump/cmd`** and **subscribe to
  `pump/state` / `pump/status`**. Give the ESP32's own user an ACL that **subscribes** to
  `pump/cmd` and **publishes** state — it should not need anything more.
- Because control is gated at the broker, if the web credentials ever leak you just
  rotate/revoke them on the broker — no re-flashing the ESP32.
- Keep everything on TLS (native MQTT 8883, WebSocket 8884); never expose the broker without it.
- The firmware's hardware safety (min-off-time, max-run lockout, boot-safe-off, and the
  physical stop button) still applies to every command from this page.
