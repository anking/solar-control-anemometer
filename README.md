# solar-control-anemometer

WiFi-connected wind speed sensor on an **ESP32-C3 SuperMini** (Teyleten Robot
or equivalent), reading a **passive DC-generator anemometer** (3-cup, 2-wire,
0–3.8 V output proportional to rotation speed — e.g. the Jeanoko / Bordstract
units on Amazon) and serving a live dashboard.

Companion to [solar-inverter-esp32](../solar-inverter-esp32). MQTT bridge is
scaffolded (configurable from the dashboard) but not yet wired to publish on
every reading — that's a follow-up.

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
- **System**: firmware version, heap, restart.
- **Update**: upload a `.bin` over the air; writes the inactive OTA slot
  and reboots into it.

Live updates push over `/ws` once per second; falls back to polling
`/api/status` if the WebSocket drops.

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
  mqtt_bridge.{c,h}     # scaffold; publishes to anemometers/<mac>/wind
  index.html            # embedded dashboard
```

## License

Same as the parent project.
