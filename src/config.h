#ifndef CONFIG_H
#define CONFIG_H

// =============================================================================
// HARDWARE: Teyleten Robot ESP32-C3 SuperMini
// =============================================================================
// Pinout note: GPIO 8 has an onboard LED, active-LOW on most SuperMini boards
// (LED lights when the pin is driven LOW). GPIO 8 is also a strapping pin, so
// drive it AFTER boot completes (which we do).

// Onboard LED (active LOW)
#define LED_GPIO              8
#define LED_ACTIVE_LOW        1

// Anemometer pulse input. The reed/hall switch closes against GND; we enable
// the internal pull-up so the line idles HIGH and dips LOW on each closure.
#define ANEMOMETER_GPIO       4

// =============================================================================
// ANEMOMETER CALIBRATION
// =============================================================================
// Davis-style cup anemometer convention: 1 closure per revolution, and
// 2.4 km/h (=1.492 mph) per Hz of rotation.
//   wind_mph = pulses_per_sec * MPH_PER_HZ
// Override at runtime via the dashboard if your sensor is different.
#define ANEMOMETER_DEFAULT_MPH_PER_HZ   1.492f

// Debounce window for the GPIO ISR. Reed switches bounce ~1 ms typically.
// At 1.492 mph/Hz, even a 100 mph wind is only ~67 Hz (≈15 ms period), so
// a 2 ms guard rejects bounce without clipping real pulses.
#define ANEMOMETER_DEBOUNCE_US          2000

// Sampling window — pulses are counted over this interval and converted to Hz.
#define ANEMOMETER_SAMPLE_INTERVAL_MS   1000

// Rolling average length for the dashboard reading (smooths gusts).
#define ANEMOMETER_AVG_SAMPLES          5

// =============================================================================
// FIRMWARE VERSION (injected by version.py at build time)
// =============================================================================

#ifndef FIRMWARE_VERSION
#define FIRMWARE_VERSION "0.0.0"
#endif

#ifndef GIT_HASH
#define GIT_HASH "unknown"
#endif

#endif // CONFIG_H
