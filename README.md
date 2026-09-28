# Pump Dashboard

An **ESP32-based controller** to switch three industrial pump motors on/off — from the
**existing physical red/green buttons** *and* from a **password-protected web dashboard**
reachable over the internet.

> ⚠️ **This project switches 3-phase mains-powered motors.** All mains/control-circuit
> wiring must be designed, executed, and signed off by a **licensed electrician** per local
> code. The ESP32 add-on must **never** defeat the overload relay or the physical stop.
> See [docs/DESIGN.md](docs/DESIGN.md) for the full safety rationale.

## Hardware in scope

| Pump | Rating | Typical starter |
|------|--------|-----------------|
| Pump 1 | 5 H.P. 3-phase | DOL |
| Pump 2 | 5 H.P. 3-phase | DOL |
| Pump 3 | 7.5 H.P. 3-phase | DOL or star-delta |

Each pump's starter keeps its existing **red (stop)** and **green (start)** buttons. The
ESP32 is **interposed** into the control circuit so control works from **both** the buttons
and the web UI, and so that an ESP failure degrades gracefully to a normal manual starter.

## How it works (summary)

- **Two-relay interposing per pump** — an ESP START relay in *parallel* with the green button,
  and an ESP STOP relay in *series* in the coil rung. The physical red STOP stays hardwired and
  always works, even if the ESP is dead. See [docs/DESIGN.md](docs/DESIGN.md#interposing).
- **Isolated button/state sensing** — physical buttons and the contactor's run state are read
  through AC opto-isolators, never a divider off mains.
- **Web dashboard on the ESP32** — ESPAsyncWebServer + LittleFS assets, live state over
  WebSocket, password-protected (hashed).
- **Reach from anywhere** — *not* by port-forwarding. Via an outbound tunnel (Tailscale /
  Cloudflare Tunnel) or MQTT through a small always-on site gateway. See
  [docs/DESIGN.md](docs/DESIGN.md#remote-access).

## Project status

| Phase | What | Status |
|-------|------|--------|
| 0 | Research, architecture, repo setup | ✅ done |
| 1 | Local control firmware (relays + button sensing + safety) | ⬜ next |
| 2 | Web dashboard on ESP32 (async server + WebSocket + auth) | ⬜ |
| 3 | Secure remote access | ⬜ |

## Repository layout

```
Pump-Dashboard/
├── README.md            # this file
├── docs/
│   └── DESIGN.md        # architecture, wiring, safety, BOM (from research)
├── .gitignore           # ESP32 / PlatformIO
└── rough/               # local-only scratch (NOT committed): research, notes, tasks
```

> `rough/` is intentionally excluded from git via `.git/info/exclude` — it holds research
> dumps, scratch notes, and secrets that must never be shared.

## Open questions

Before Phase 1, confirm:
1. Each contactor's **coil voltage** (230 V vs 415 V) — sets relay/snubber ratings.
2. Whether the **7.5 H.P.** starter is **DOL or star-delta**.
3. Whether the site ISP uses **CGNAT** (affects remote-access approach).
4. Toolchain: **PlatformIO** (VS Code) vs Arduino IDE.

## Documentation

- **[docs/DESIGN.md](docs/DESIGN.md)** — full engineering design: starter integration,
  isolated I/O, ESP32 pin plan, web server, remote-access options, safety interlocks, and BOM.
