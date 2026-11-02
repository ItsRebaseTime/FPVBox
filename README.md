# FPVBox

**One USB-C cable between your FPV drone and any computer, headset or phone.**

FPVBox is a small ground station that turns analog FPV and ExpressLRS into standard USB devices:
- It **receives 5.8 GHz analog FPV video** and shows up as an ordinary USB webcam. VR headsets such as the Steam Frame, laptops and Android phones can display it without drivers or capture dongles.
- It **is an ExpressLRS 2.4 GHz transmitter.** A program on the host flies the drone and gets its telemetry back over a plain USB serial port, using the same CRSF protocol a radio handset uses.
- It **reports the ground station's position** from a built-in GNSS receiver, as a standard NMEA serial port that `gpsd` and Android GPS apps understand.

> **Status: work in progress.** Both firmwares build, and the video decoder is tested on synthetic signals. Nothing has run on the real hardware yet; see the [checklist](#to-do-and-verification-checklist).

## Goals

- **No drivers on the host:**
  - video is UVC,
  - the other ports are CDC-ACM serial,
  - it works on Linux, Android, Windows and macOS.
- **Lowest glass-to-glass latency:**
  - the video uses most of the USB bandwidth,
  - raw video goes out field by field,
  - compressed video uses the chip's hardware encoders.
- **Video formats a headset can decode in hardware:** raw YUYV (nothing to decode), MJPEG, and H.264 for SoC video decoders.
- **Open, cheap hardware:** two off-the-shelf boards and jumper wires, and no extra RF parts for the video receiver.

## What the host sees

| USB function | Purpose |
|---|---|
| Video (UVC) | The live FPV picture, PAL or NTSC, as YUYV, MJPEG or H.264 |
| Serial "ELRS" | CRSF to and from the ExpressLRS transmitter: sticks out; link statistics and drone telemetry in |
| Serial "GNSS" | NMEA from the ground station's GNSS receiver |
| Serial "Control" | Box status and settings, video receiver channel |

How to use each one: [docs/USAGE.md](docs/USAGE.md).

## Hardware

| Part | Role | Manufacturer documentation |
|---|---|---|
| OSPTEK ESP32-P4C5 module on its dev board (V1.3 carrier) | The ESP32-C5 receives the video with its own radio; the ESP32-P4 decodes it and provides USB high-speed | [Core board](https://github.com/osptek/esp32-p4c5-core-board) (user guide, pinout, internal C5↔P4 wiring) · [Dev board](https://github.com/osptek/esp32-p4c5-module-dev-board) (schematic V1.3, user guide) |
| LilyGo T-ETH-Elite LoRa shield with the SX1280 + PA ("17AP") module | ExpressLRS 2.4 GHz radio and Quectel L76K GNSS | [Wiki](https://wiki.lilygo.cc/products/t-eth-series/t-eth-lora-shield/) · [Schematic](https://github.com/Xinyuan-LilyGO/LilyGO-T-ETH-Series/blob/master/schematic/T-ETH-ELite-LoRa-Shield.pdf) |
| Antennas | 5.8 GHz (video), 2.4 GHz (ELRS), GPS | — |

Chip documentation:
- [ESP32-P4 datasheet](https://documentation.espressif.com/esp32-p4_datasheet_en.html) · [ESP32-P4 TRM](https://documentation.espressif.com/esp32-p4_technical_reference_manual_en.pdf)
- [ESP32-C5 datasheet](https://documentation.espressif.com/esp32-c5_datasheet_en.html)
- [Semtech SX1280](https://www.semtech.com/products/wireless-rf/lora-connect/sx1280)

Wiring: [docs/WIRING.md](docs/WIRING.md), with a [WireViz diagram](docs/wiring/fpvbox.svg).

Local copies of manufacturer documents go in `docs/vendor/`. That folder is not committed; download the files from the links above.

## Built on

- [C5VRX](https://github.com/Twotoz/C5VRX) by Twotoz: an analog 5.8 GHz FPV receiver on the ESP32-C5's own radio.
- [ExpressLRS](https://github.com/ExpressLRS/ExpressLRS): the long-range RC link.
- ESP-IDF and TinyUSB.

Build, flash and implementation notes: [docs/DEVELOPMENT.md](docs/DEVELOPMENT.md).

## To-do and verification checklist

### Hardware
- [ ] Check the shield's "TBD" resistors with a meter: SX1280 RX/TX enable → header pins 40/18, and DIO1, BUSY and SPI → 27/29/13/19/21/23.
- [ ] Wire the shield and the C5 console jumpers ([WIRING.md](docs/WIRING.md)), set SW6 ON and bridge JP1.
- [ ] Confirm that C5 flashing through the carrier's S1 switch and the CH343 (J7) works with the P4C5 module.
- [ ] Measure the SX1280 PA output and calibrate the ELRS power table (capped at 100 mW until then).
- [ ] Enclosure and antenna placement.

### Video receiver (ESP32-C5)
- [ ] C5VRX boots on the module; its console reaches the P4 (`!status` shows `c5 alive=1`).
- [ ] 40 MHz bus clock and data are present on the internal lines.
- [ ] Reception range and picture quality match C5VRX on the XIAO board (moved IQ lane, carrier pull-up on GPIO25).

### Video to USB (ESP32-P4)
- [ ] The P4 captures the 40 MHz bus clock (hardware ÷2 decimation, or the software fallback).
- [ ] The decoder locks on a real PAL and NTSC VTX with correct colours and field order.
- [ ] CPU budget: no dropped lines with colour decoding; optimise with the P4's SIMD (PIE) if needed.
- [ ] The UVC device enumerates and plays YUYV, MJPEG and H.264 on Linux, Windows and Android.
- [ ] H.264 plays with hardware decoding on the Steam Frame.
- [ ] USB throughput holds for raw 720×288 at 50 fps.
- [ ] MJPEG colour order is correct (red and blue not swapped).
- [ ] Glass-to-glass latency is measured for each format.
- [ ] Optimisation: send raw video line by line while it is being decoded.

### ExpressLRS
- [ ] SX1280 starts; the PA/LNA switches with TX/RX.
- [ ] Binds and links to an ExpressLRS 4.x receiver; telemetry reaches the host.
- [ ] Packet timing is stable at 250 Hz, 500 Hz and F1000.
- [ ] Settings work through the CRSF parameter protocol from host tools.

### GNSS
- [ ] NMEA reaches the host; `gpsd` gets a fix; host baud changes and PPS work.

### System
- [ ] All four USB functions enumerate together (endpoint and FIFO limits).
- [ ] 30-minute combined load test (video + ELRS + GNSS), including temperature.
- [ ] Own USB VID/PID.
- [ ] Optional: OLED status display and shield buttons for channel selection.
- [ ] Optional: DVR recording to the TF card.

## Licence

FPVBox is licensed under GPL-3.0, see [LICENSE](LICENSE). C5VRX and ExpressLRS are GPL-3.0 as well, so the firmware images built from this project are GPL-3.0.
