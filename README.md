# solar-control-anemometer

WiFi-connected wind speed sensor on an **ESP32-C3 SuperMini** (Teyleten Robot
or equivalent), reading a **passive DC-generator anemometer** (3-cup, 2-wire,
0–3.8 V output proportional to rotation speed — e.g. the Jeanoko / Bordstract
units on Amazon) and serving a live dashboard.

Companion to [solar-inverter-esp32](../solar-inverter-esp32). It publishes wind
telemetry over MQTT and is battery-aware: in **Active** mode it reports by
exception (only on meaningful change, gusts, or a periodic heartbeat) and lets
the WiFi radio modem-sleep in between; a **Sleep** mode deep-sleeps and wakes
once per interval to report a summary, for when the panels are towed.

## Hardware

| Part                                  | Notes                                          |
|---------------------------------------|------------------------------------------------|
| Teyleten Robot ESP32-C3 SuperMini     | Native USB-C, 4 MB flash, onboard LED on GPIO 8|
| 2-wire DC-generator anemometer        | 0–3.8 V passive output, no excitation needed   |
| 1 µF ceramic cap                      | Smooths the ~100 mV brush ripple on the signal |

### Wiring

```
   Wire +  (signal) ──┬──── GPIO 4   (ADC1_CH4)
                      │
                      ├──── 1µF cap ──┐
                      │               │
   Wire −  (ground) ──┴───────────────┴──── GND  (ESP)
```

Identify which sensor wire is + and which is − with your multimeter: put it
on DC volts, spin the cups, and the wire your **red** probe was on (when
you saw a positive reading) is the **signal +**. The other is **−**.

No voltage divider — the C3 ADC will simply peg at ~3.1 V if the sensor
ever exceeds it (an extreme weather event). The firmware exposes a
`saturated` flag and the dashboard shows a banner so you'll know when the
reading can't be trusted.

Pin assignments live in [src/config.h](src/config.h).

## Build / Flash

Requires [PlatformIO](https://platformio.org/) with the espressif32 platform.

```powershell
pio run                              # build
pio run -t upload                    # flash via USB
pio run -t monitor                   # serial monitor (115200)
```

On first boot with no saved WiFi the device opens an open hotspot named
**`Anemometer-XXXXXX`** (last 3 MAC bytes). Join it and browse to
`http://192.168.4.1/` or `http://anemometer.local/` to configure WiFi.

After it joins your network, the dashboard is at `http://<device-ip>/` or
`http://anemometer-XXXXXX.local/`.

## Dashboard

- **Wind**: live mph + km/h, gauge, average + gust, **live voltage**, plus
  a saturation warning when the ADC pegs.
- **Calibration** (see below) — two-point linear model with a one-click
  "capture zero" button.
- **WiFi**: status, scan, connect, forget.
- **MQTT**: broker config (saved to NVS, optional).
- **System**: firmware version, heap, restart, LED behavior, **power mode**
  (Active / Sleep + wake interval + held backlog count).
- **Update**: upload a `.bin` over the air; writes the inactive OTA slot
  and reboots into it.

Live updates push over `/ws` once per second; falls back to polling
`/api/status` if the WebSocket drops.

## Power / battery

The device runs on battery, so it avoids transmitting on every reading.

**Active mode** (default) keeps the dashboard live but reports to MQTT *by
exception*: 1 Hz readings are batched and only flushed when

- sustained wind moves by ≥ `REPORT_MPH_DEADBAND` (2 mph),
- a gust jumps ≥ `REPORT_GUST_DELTA_MPH` (3 mph) above the last send (flushed
  immediately), or
- the batch fills, or
- the `REPORT_HEARTBEAT_SECONDS` (15 s) heartbeat elapses.

Batches land on `anemometers/<mac>/wind` as a JSON array. Between flushes the
WiFi radio idles in `WIFI_PS_MIN_MODEM`.

**Sleep mode** is a deep-sleep duty cycle for when the panels are towed and
live wind no longer matters. Each wake it samples a `SLEEP_BURST_SECONDS`
(30 s) burst, collapses it to one `{avg,peak,min}` summary, and publishes the
summary plus any backlog accumulated while out of broker range to
`anemometers/<mac>/summary` (QoS 1). The backlog (up to `SLEEP_BACKLOG_MAX`,
192 ≈ 8 days at 1/hour) lives in RTC memory and survives deep sleep. Default
wake interval is `SLEEP_DEFAULT_INTERVAL_S` (3600 s).

In Sleep mode the dashboard is offline, so switch modes via a **retained MQTT
command** on `anemometers/<mac>/cmd`:

```jsonc
{"mode": "active"}              // wake up, restore the dashboard (stays active)
{"mode": "sleep"}              // enter the deep-sleep duty cycle
{"interval": 1800}            // change the wake interval (60–65535 s)
{"ui": true}                  // one-shot: next wake, bring the dashboard up
                              //   for UI_MAINT_WINDOW_S, then resume sleeping
{"ui": 600}                   // same, but for an explicit 600 s
```

**Important:** because deep sleep powers down the radio, a command can only be
*retained* on the broker and is acted on at the **next scheduled wake** — there
is no way to summon the device instantly mid-sleep. Two ways to reach the UI in
Sleep mode:

- `{"mode": "active"}` — reboots into Active mode permanently (UI always on)
  until you send `{"mode": "sleep"}` again.
- `{"ui": true}` — a one-shot **maintenance window**: the device brings the
  dashboard up on its next wake, stays awake long enough to do an OTA update or
  inspect, then automatically goes back to sleep. The retained command is
  cleared once consumed so it doesn't repeat.

A mode change reboots the device into the matching boot path. Persistent
settings (mode, interval) live in NVS (`device_cfg`); the `ui` window is
transient. Tunables live in [src/config.h](src/config.h).

> **Note:** the ESP32-C3 has no ULP coprocessor, so it can't sample the ADC
> while asleep — Sleep mode only captures wind during its brief wake burst.

## Calibration

The firmware models wind speed as:

```
wind_mph = max(0, (voltage_v − zero_offset_v) × mph_per_volt)
```

So two numbers to set:

1. **Zero offset** — the voltage the sensor reads when *truly* stationary.
   Even DC-generator anemometers have a small noise floor (cable pickup,
   brush stiction, ADC offset). On the dashboard, with the cups locked
   still, click **Capture zero (now)** — it snapshots the current 1-second
   average voltage as the new zero. Persisted to NVS.

2. **mph per Volt** — the slope. The Jeanoko spec is 0–32 m/s over
   0–3.8 V, which works out to **18.83 mph/V** (the default). To
   field-calibrate against a hand anemometer:
   - Get a sustained breeze (a leaf blower or stiff fan works).
   - Note the hand anemometer's reading in mph.
   - Note the dashboard's current voltage (in the diagnostics card).
   - Set `mph_per_volt = hand_reading_mph / (voltage_v − zero_offset_v)`.

You can also tweak the numbers freely and watch the live reading change.

## Project layout

```
platformio.ini          # board = esp32-c3-devkitm-1, framework = espidf
partitions.csv          # 4 MB dual-OTA layout (ota_0/ota_1 + otadata)
sdkconfig.defaults      # USB-Serial-JTAG console, WS support
version.py              # SemVer derived from conventional commits
CMakeLists.txt          # project root
src/
  config.h              # pin map, calibration defaults
  main.c                # boot sequence + 1 Hz WS broadcaster
  anemometer.{c,h}      # ADC1_CH4 oneshot, 100 Hz oversample → 1 Hz avg
                        # with curve-fitting calibration scheme
  wifi_manager.{c,h}    # STA + soft-AP fallback, mDNS, SNTP
  wifi_config.{c,h}     # NVS-persisted credentials
  nvs_store.{c,h}       # thin NVS wrapper
  led_status.{c,h}      # onboard LED state machine
  http_server.{c,h}     # /api/* + /ws
  mqtt_bridge.{c,h}     # publishes wind/summary, subscribes to <mac>/cmd
  power_mgr.{c,h}       # Active/Sleep modes, deep-sleep cycle, RTC backlog
  index.html            # embedded dashboard
```

## License

Same as the parent project.
