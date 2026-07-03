# Guition ESP32-S3-4848S040 Sundial

This project turns a Guition ESP32-S3-4848S040 display board into a Wi-Fi connected sundial-style clock.

The firmware renders a full-screen 480x480 analog-style clock with:

- a day/night sky background
- sunrise and sunset-aware visuals
- a sun position that changes through the day
- local time synchronized over Wi-Fi using SNTP

It is built with **ESP-IDF** for the **ESP32-S3**.

## Media

Add a product photo or screenshot here:

![Sundial photo or screenshot](docs/images/sundial.jpg)
- [Demo video 1](docs/videos/demo-1.mp4)
- [Demo video 2](docs/videos/demo-2.mp4)

## Hardware target

This firmware is written for the **Guition ESP32-S3-4848S040** board with a 480x480 RGB display.

If you are using different hardware, you will likely need to adjust display and pin definitions in:

- `main/esp32s3_4848s040_sundial.c`

## Requirements

- ESP-IDF **6.0.1**
- Python, CMake, Ninja, and the ESP-IDF toolchain
- A supported ESP32-S3 board connected over USB for flashing

## Setting up the build environment

Follow the normal ESP-IDF installation process for your platform, but make sure to use **ESP-IDF 6.0.1**.

A typical Linux setup looks like this:

1. Install the host tools required by ESP-IDF.
2. Clone `esp-idf` at version `v6.0.1`.
3. Run the ESP-IDF installer for the `esp32s3` target.
4. Source the ESP-IDF environment before building.

Example:

```bash
git clone --branch v6.0.1 https://github.com/espressif/esp-idf.git ~/esp-idf
~/esp-idf/install.sh esp32s3
. ~/esp-idf/export.sh
```

For full platform-specific instructions, see the official ESP-IDF setup guide:

- https://docs.espressif.com/projects/esp-idf/en/v6.0.1/esp32s3/get-started/

## Project configuration

Before flashing, you should update the user-specific settings in:

- `main/esp32s3_4848s040_sundial.c`

### Required changes

#### Wi-Fi credentials

Update these two macros so the device can join your network and fetch time from SNTP:

```c
#define WIFI_SSID "ssid"
#define WIFI_PASSWORD "hemmelig kode"
```

#### Your location

Update these values so sunrise and sunset are calculated for your location:

```c
#define LATITUDE 57.0488
#define LONGITUDE 9.9217
```

These should be your coordinates in decimal degrees.

#### Your timezone

The timezone is currently set in `init_time_zone()`:

```c
setenv("TZ", "CET-1CEST,M3.5.0/2,M10.5.0/3", 1);
```

Change this to match your local timezone rules. If this is wrong, the clock may synchronize correctly from the network but still display the wrong local time.

### Optional changes

#### Fallback sunrise and sunset values

These defaults are used before a proper daily calculation is available:

```c
#define DEFAULT_SUNRISE_MINUTES (6 * 60)
#define DEFAULT_SUNSET_MINUTES (18 * 60)
```

You usually do not need to change them.

#### Board-specific display settings

If you are adapting the firmware to another board, review constants such as:

- display size
- RGB/LCD pin assignments
- backlight pin
- panel initialization sequence

These are also defined in:

- `main/esp32s3_4848s040_sundial.c`

## Building

With the ESP-IDF environment loaded, build the firmware from the project root:

```bash
idf.py build
```

The project target is `esp32s3`, and the repository already includes defaults in:

- `sdkconfig.defaults`

## Flashing

Flash to the connected board with:

```bash
idf.py -p /dev/ttyUSB0 flash
```

If you also want serial logs:

```bash
idf.py -p /dev/ttyUSB0 flash monitor
```

Replace `/dev/ttyUSB0` with the correct serial device for your system.

## How the clock gets its time

On boot, the firmware:

1. initializes storage
2. configures the display and backlight
3. connects to Wi-Fi
4. synchronizes time using SNTP
5. renders the watchface continuously

Because the clock depends on network time synchronization, a correct Wi-Fi configuration is required for normal use.

## Font pipeline

The text renderer uses a vendored open-source font:

- `assets/fonts/DejaVuSansMono-Bold.ttf`
- license text: `assets/fonts/DejaVu-LICENSE.txt`

Generated glyph data lives in:

- `main/generated_font.h`

To regenerate the font header after changing the font or glyph generation settings:

```bash
python tools/generate_font_header.py
```

## License

This project is released under the **Unlicense**.

See:

- `UNLICENSE`

Note that bundled font assets may have their own license terms. See:

- `assets/fonts/DejaVu-LICENSE.txt`
