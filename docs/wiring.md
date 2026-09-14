# Voxie — Hardware Wiring

Matches `firmware/main/system/config.h`. ESP32-S3-DevKitC-1.

## Pin map (firmware)

| Peripheral | Signal | ESP32-S3 GPIO |
|---|---|---|
| ICS43434 / INMP441 mic (I2S0, RX) | WS / LRCLK | GPIO4 |
| | SCK / BCLK | GPIO5 |
| | SD / DATA | GPIO6 |
| | L/R (select) | GND (left channel) |
| MAX98357A amp (I2S1, TX) | LRC / LRCLK | GPIO7 |
| | BCLK / SCK | GPIO15 |
| | DIN / DATA | GPIO16 |
| | SD / SD_MODE | 3V3 (enable — do NOT float) |
| SSD1306 OLED (I2C) | SDA | GPIO41 |
| | SCL | GPIO42 |
| WS2812B LED strip (optional) | DIN | GPIO48 |
| Relay / lamp (optional) | IN | GPIO47 |
| DHT22 sensor (optional) | DATA | GPIO46 |
| BOOT button (onboard) | — | GPIO0 |

Do **not** use GPIO0, 19, 20, 43, 44 (boot / USB / console UART).

## Diagram

```
                          ESP32-S3-DevKitC-1
                     ┌────────────────────────────┐
                     │                            │
  ICS43434 / INMP441 │                            │
  (I2S MEMS mic)     │                            │
    VDD  ────────────┤ 3V3                        │
    GND  ────────────┤ GND                        │
    WS   ────────────┤ GPIO4                      │
    SCK  ────────────┤ GPIO5                      │
    SD   ────────────┤ GPIO6                      │
    L/R  ────────────┤ GND      (select left)     │
                     │                            │
  MAX98357A          │                            │
  (I2S class-D amp)  │                            │
    VIN  ────────────┤ 5V / VBUS                  │
    GND  ────────────┤ GND                        │
    LRC  ────────────┤ GPIO7                      │
    BCLK ────────────┤ GPIO15                     │
    DIN  ────────────┤ GPIO16                     │
    SD   ────────────┤ 3V3      (enable!)         │
    GAIN ────────────┤ (leave open = 9 dB)        │
                     │                            │
  3W speaker          │                            │
    (+)  ────────────┤ OUT+  ┐                    │
    (−)  ────────────┤ OUT−  ┘ bridge-tied (BTL)  │
                     │       never connect to GND │
                     │                            │
  SSD1306 OLED       │                            │
    VCC  ────────────┤ 3V3                        │
    GND  ────────────┤ GND                        │
    SDA  ────────────┤ GPIO41                     │
    SCL  ────────────┤ GPIO42                     │
                     │                            │
  (optional)         │                            │
    WS2812B DIN ─────┤ GPIO48                     │
    Relay IN    ─────┤ GPIO47                     │
    DHT22 DATA  ─────┤ GPIO46                     │
    BOOT button ─────┤ GPIO0 (onboard)            │
                     └────────────────────────────┘
```

## Critical checks (speaker noise)

1. **MAX98357A `SD` (SD_MODE) must be tied to 3V3.** If it floats, the amp
   sits in an undefined state and emits a constant high-frequency noise.
2. **Speaker is bridge-tied:** connect across `OUT+`/`OUT−` only. Wiring a
   speaker terminal to GND causes noise/distortion.
3. **Do not swap `BCLK` and `DIN`.** `BCLK→GPIO15`, `DIN→GPIO16`.
4. **Common ground** between ESP32, mic, amp and OLED.
5. Amp `VIN` on **5V** for full volume (3V3 also works, quieter).
6. OLED I2C modules usually include pull-ups; if not, add 4.7 kΩ SDA/SCL→3V3.

## Power

- Mic + OLED: **3V3**
- MAX98357A: **5V (VBUS)**; speaker on `OUT+/OUT−`
- Keep all grounds common.
