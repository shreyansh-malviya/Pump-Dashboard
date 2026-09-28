# Pump Dashboard — Design & Architecture

Engineering design distilled from the Phase-0 research. This is **guidance, not a certified
schematic** — a licensed electrician must design, execute, and sign off all mains/control work.

- [1. Motor-starter integration](#interposing)
- [2. Reading button & run state safely](#sensing)
- [3. ESP32 board & I/O plan](#board)
- [4. Web server on the ESP32](#webserver)
- [5. Remote access — "from anywhere"](#remote-access)
- [6. Safety & reliability](#safety)
- [7. Architecture diagram & BOM](#arch)

---

<a name="interposing"></a>
## 1. Motor-starter integration

### Starting method
- **5 H.P. (~3.7 kW):** DOL (Direct-On-Line) is standard. Inrush 5–8× full-load current (FLC).
- **7.5 H.P. (~5.5 kW):** boundary case. Often DOL, but **star-delta** is common at this rating.
  Either way the ESP interposes only at the **control-circuit start/stop level** — do **not** touch
  the star/delta timing logic; only gate the "run command."

### Keep the native protective circuit intact
A starter = **contactor** (switches the 3 phases) + **overload relay (OL)** in series with the coil
(NC contact 95–96). The OL de-energizes the coil on overcurrent. **Leave the OL, coil, and hold-in
auxiliary untouched.**

Standard latched DOL control circuit:

```
L ──[OL 95–96 NC]──[STOP red, NC mom.]──┬──[START green, NO mom.]──┬── A1 (coil) A2 ── N
                                        └──[Aux NO 13–14 hold-in]──┘
```

Green energizes the coil; the contactor's aux NO (13–14) latches ("seals in"); red or an OL trip
drops the latch.

### Recommended interposing — two relays per pump  {#two-relay}

Keep the entire native circuit and add **two ESP-driven interposing contacts**:

1. **ESP START relay — NO contact in PARALLEL with the physical green button.**
   ESP pulses it ~0.3–0.5 s to start; the contactor's native aux latch seals it in.
   Physical green still works independently.
2. **ESP STOP relay — NC contact in SERIES in the coil rung, beside the physical red.**
   ESP energizes it to *open* the rung and drop the latch (stops and stays stopped).
   Default de-energized = closed = current passes.

**Why this is the safest scheme:**
- The **physical red STOP stays hardwired in series and never routes through the ESP** → it always
  stops the motor, even with the ESP frozen or unpowered.
- **ESP power/firmware failure de-energizes both relays** → START opens (no spurious start), STOP
  closes (passes through) → the panel reverts to a **normal manual starter**. Graceful degradation.
- Overload protection is untouched.

> **Feedback input:** wire the contactor's aux contact into an ESP opto input so firmware knows the
> *actual* run state (not just what it commanded) — for the dashboard and anti-chatter logic.

A single-relay alternative (ESP relay as the sole latch) exists but needs a watchdog, must sense the
momentary-red stop to avoid re-start, and loses local start when the ESP is down. **Prefer two relays.**

### Switching the AC coil
ESP relay **module** (not a bare relay): contacts **≥10 A / 250 V AC**, built-in optocoupler,
flyback diode, plus an **RC snubber (~0.1 µF/275 V X2 + 100 Ω)** across the contacts to absorb the
inductive kick from the AC contactor coil and prevent contact welding. **Confirm each coil's voltage
(230 V vs 415 V)** before choosing ratings.

---

<a name="sensing"></a>
## 2. Reading button & run state safely (mains → 3.3 V)

- **Never** use a resistor voltage divider off 230 V — no isolation; a fault puts mains on the ESP.
- **Use AC-input opto-isolator modules** (bridge rectifier + limiting R/C → PC817/LTV816 LED →
  transistor to 3.3 V). Ready-made "1-bit AC 220 V optocoupler isolation" boards do exactly this.
- **Gotcha:** on raw AC the opto output is a **100 Hz square wave** (pulses at 2× line frequency).
  Treat *"any pulses within the last ~40–60 ms"* as "live" — do not read the instantaneous level.
- **Debounce** 20–50 ms stable window. Buttons are **momentary** → read as *events*.

Read three signals per pump: **green button**, **red button**, **contactor aux (run state)**.

---

<a name="board"></a>
## 3. ESP32 board & I/O plan

- **4 MB flash is plenty** (firmware ~1–1.5 MB; a LittleFS partition of 1–1.5 MB holds a rich
  dashboard). Choose a partition scheme with an OTA slot + LittleFS.
- For **on-device HTTPS** headroom prefer an **ESP32-WROVER (8 MB PSRAM)**; a plain WROOM-32 works
  if TLS is terminated at the gateway (recommended — see §5).
- **LittleFS** (not SPIFFS) for web assets; gzip them and serve `Content-Encoding: gzip`.
- **Relays:** opto-isolated **low-level-trigger** module, 3.3 V logic compatible, ≥10 A/250 V.
  For 3 pumps × (START+STOP) = 6 relays → use an **8-channel** board.
- **Power:** relay coils from a **separate 5 V/≥2 A** supply (remove the VCC–JD-VCC jumper, feed
  JD-VCC), ESP on 3.3 V, **common ground shared**. Use a regulated DIN-rail PSU, not laptop USB.

### Pin plan (WROOM/WROVER)

| Use | Suggested GPIO | Notes |
|-----|----------------|-------|
| Relay outputs (6) | 13, 14, 25, 26, 27, 32 | Safe output pins; default de-energized at boot |
| Spare relay outputs | 33, 23, 22, 19, 18 | if needed |
| Opto inputs (buttons/aux) | 34, 35, 36 (VP), 39 (VN) + 16, 17, 21 | 34–39 input-only (external pull-up) |

**Avoid / handle with care:**
- **GPIO6–11** — SPI flash, never use.
- **Strapping pins** GPIO0 (HIGH to boot), GPIO2 (LOW/float, no pull-up), **GPIO12/MTDI** (HIGH →
  boot-loop; most dangerous), GPIO15 — avoid for I/O or use series resistors.
- **ADC2 pins** are unusable for analog while Wi-Fi is on.
- Use **low-level-trigger** relays + external pull-downs so the boot glitch can't pulse a coil.

---

<a name="webserver"></a>
## 4. Web server on the ESP32

- **Library:** `esp32async/ESPAsyncWebServer` (maintained fork) — non-blocking, WebSocket/SSE,
  LittleFS static serving, Basic/Digest auth. For production HTTPS consider **PsychicHttp** (WSS).
  Avoid the stock synchronous `WebServer` (blocks under concurrency).
- **Live state:** push pump/button/contactor state over a **WebSocket**; receive start/stop commands.
- **Auth:** prefer a **login form → session cookie** (random token in RAM with expiry) over HTTP Basic.
  Store only a **salted hash** of the password (SHA-256+salt via mbedTLS) in NVS — never plaintext.
  Add login rate-limiting / lockout.
- **HTTPS on-device is costly** (~35–50 KB heap per TLS session; WROOM handles only 3–6 concurrent)
  and self-signed certs trigger browser warnings. **Recommendation: don't publicly expose ESP32
  HTTPS — terminate TLS at the tunnel/gateway** (real browser-trusted cert), ESP serves plain HTTP
  on the LAN or inside the tunnel.

---

<a name="remote-access"></a>
## 5. Remote access — "from anywhere"

The ESP32 sits behind NAT, so nothing on the internet reaches it inbound by default.

| Option | Verdict |
|--------|---------|
| (a) Port-forward + Dynamic DNS | ❌ Fails under **CGNAT** (common on rural/mobile ISPs); directly exposes an industrial device to scanners. **Do not use.** |
| (b) Outbound MQTT/WebSocket → cloud broker + thin frontend | ✅ ESP dials out (TLS 8883); CGNAT irrelevant, no inbound ports. Mosquitto/EMQX/HiveMQ. |
| (c) IoT platforms (Blynk, Arduino IoT Cloud, Firebase) | ✅ Outbound-only, ready UI + auth; vendor lock-in. |
| (d) Tailscale / Cloudflare Tunnel | ✅ No open ports, through CGNAT. **No native ESP32 client** — the agent runs on a small always-on Linux box (Pi) reverse-proxying to the ESP32. |

### Recommendation
Add a small **always-on site gateway** (Raspberry Pi Zero 2 W / Pi 4) on the LAN:

- **Best security:** **Tailscale** (WireGuard mesh) — no inbound ports, through CGNAT, E2E encrypted,
  device ACLs + MFA. Strongest posture for an industrial device.
- **Best shareable URL:** **Cloudflare Tunnel** on the Pi → `https://pumps.example.com` with a real
  trusted cert + **Cloudflare Access** (SSO / one-time-PIN / MFA).
- The ESP32 then serves plain HTTP on the LAN (or MQTT to the Pi); TLS + edge auth live at the tunnel.
- No Pi? Use hosted MQTT (b/c) with the ESP32 connecting outbound over TLS to a hosted frontend.

**Defense-in-depth (mandatory, since this switches real motors):** outbound-only connections, TLS
everywhere, MFA at the edge, a hashed app password as a second factor, rate-limiting/lockout — and the
**hardware fail-safes below must hold even if the remote channel is compromised.** Never port-forward
the raw ESP32.

---

<a name="safety"></a>
## 6. Safety & reliability

- **Stagger starts:** never start 2–3 pumps simultaneously (combined inrush trips the incomer).
  Software interlock with a **5–10 s stagger**; optional cap on how many run at once.
- **Keep the overload relay** in every starter (set 100–115% FLC) — primary burnout protection.
- **Dry-run protection:** electronic pump-protection relay (e.g. Fanox PF-R) sensing cos φ / current,
  or 3× CTs. **Hardwired trip into the coil circuit**, not ESP-only; ESP reads status.
- **Phase-failure / single-phasing:** phase-failure/sequence/under-over-voltage relay per starter,
  **hardwired trip** (a thermal OL alone reacts too slowly to single-phasing).
- **ESP32 watchdog:** enable Task WDT + RTC WDT; on reset, GPIO defaults leave **relays de-energized**.
- **Fail-safe on Wi-Fi/power loss:** safe state = **motor OFF / no spurious start**. Never auto-start
  on boot — require an explicit command. Auto-reconnect Wi-Fi without reboot loops; a lost cloud link
  must trigger **no** motor action.
- **Anti-chatter:** enforce **minimum ON / minimum OFF** times + a command rate-limit and a
  restart-inhibit timer to protect the contactor and motor.
- **E-stop:** the physical red button is the local e-stop; consider a maintained mushroom E-stop in series.
- **Electrician sign-off** for all mains work; the ESP add-on must not bypass any protective device.

---

<a name="arch"></a>
## 7. Architecture diagram & BOM

```
                          INTERNET (accessed from anywhere)
                                     │
                     ┌───────────────┴────────────────┐
                     │  Tailscale tailnet  OR          │  (browser-trusted TLS,
                     │  Cloudflare Tunnel + Access(MFA) │   MFA/SSO, NO open ports)
                     └───────────────┬────────────────┘
                                     │  (outbound-only from site)
                        ┌────────────┴─────────────┐
                        │  Site gateway: Raspberry  │  runs cloudflared/tailscale
                        │  Pi (always-on, on LAN)   │  + optional Mosquitto (MQTT)
                        └────────────┬─────────────┘
                                     │  LAN Wi-Fi (HTTP/WS or MQTT, local)
                        ┌────────────┴─────────────┐
                        │        ESP32 (WROVER)      │
                        │  Async web server + WS     │
                        │  LittleFS assets, WDT,     │
                        │  auth (hashed pw+session)  │
                        └───┬───────────────┬────────┘
        3.3V logic (common GND)             │ opto INPUTS (isolated)
          relay drives │                    │  read: Green, Red, Contactor AUX
                       ▼                    ▲   (AC opto modules per pump)
   ┌──── 5V relay board (JD-VCC sep. supply, opto-isolated, low-level trigger) ────┐
   │  Pump1: [START NO]‖green   [STOP NC]series                                    │
   │  Pump2: [START NO]‖green   [STOP NC]series                                    │
   │  Pump3: [START NO]‖green   [STOP NC]series                                    │
   └──────────────────────────────┬──────────────────────────────────────────────┘
                                   │ switches CONTROL circuit only (230/415V coil)
        ┌───────────────────────────┴───────────────────────────┐
        │  EXISTING STARTER PANEL (per pump) — UNTOUCHED CORE:     │
        │  Contactor coil ─ Aux hold-in ─ Physical RED (NC,        │
        │  hardwired) ─ Physical GREEN (NO) ─ OVERLOAD RELAY ─     │
        │  + Dry-run / Phase-failure relay (hardwired trip)        │
        └───────────────────────────┬───────────────────────────┘
                                     │ 3-phase power (DOL / star-delta)
                        ┌────────────┴────────────┐
                        │  2× 5 HP + 1× 7.5 HP     │
                        │  3-phase induction motors│
                        └──────────────────────────┘
```

### Bill of materials (rough)

| # | Item | Notes / spec | Qty |
|---|------|--------------|-----|
| 1 | ESP32 module | WROVER (8 MB PSRAM) preferred for TLS headroom; WROOM-32 ok if TLS at gateway | 1 |
| 2 | Opto-isolated relay board | Low-level trigger, 3.3 V compatible, ≥10 A/250 V, separable JD-VCC; 8-ch (6 used) | 1 |
| 3 | AC-input opto modules | PC817-based; one per read: green, red, aux × 3 pumps | ≈9 |
| 4 | RC snubbers 275 V AC (X2 + R) | across each interposing relay contact | 6 |
| 5 | 5 V regulated PSU (DIN-rail) | ≥2 A for relays; common GND with ESP 3.3 V | 1 |
| 6 | Pump-protection relay (dry-run + phase-failure) | e.g. Fanox PF-R — hardwired trip, per pump | 3 |
| 7 | Thermal/electronic overload relay | keep/verify existing per starter | 3 (existing) |
| 8 | Site gateway | Raspberry Pi Zero 2 W / Pi 4 (tunnel + optional Mosquitto) | 1 |
| 9 | Enclosure, terminals, control-ckt MCB/fuses, ferrules, cable | panel-build consumables | — |
| 10 | (Optional) 3× current transformers | current-based dry-run / load telemetry | per pump |

**Firmware stack:** Arduino-ESP32 or ESP-IDF · `esp32async/ESPAsyncWebServer` (or PsychicHttp for
HTTPS) · LittleFS (gzipped assets) · WebSocket for live state · NVS for hashed password + config ·
Task/RTC watchdog enabled · MQTT (PubSubClient/async) if using the broker pattern.

---

_Sources for every claim above are listed in the Phase-0 research dump kept locally in
`rough/research/` (not committed)._
