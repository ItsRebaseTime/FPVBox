# FPVBox development

## Architecture

```
5.8 GHz FM video ─► ESP32-C5: C5VRX (radio = receiver, BitScrambler FM demodulator)
                        │ 6-bit composite samples, 40 MHz clock (on-module SDIO traces)
                        ▼
                    ESP32-P4: PARLIO capture ─► software PAL/NTSC decoder (2 cores)
                        ├─► UVC: YUYV raw | MJPEG (HW JPEG) | H.264 (HW encoder)
SX1280 + PA ◄─SPI──►    ├─► CDC "ELRS": CRSF, ExpressLRS 4.1 TX
L76K GNSS ◄─UART───►    ├─► CDC "GNSS": NMEA as-is
                        └─► CDC "Control": box commands + C5VRX console
```

## Repository

| Path | Content |
|---|---|
| `c5/` | ESP32-C5 firmware: a board patch for [Twotoz/C5VRX](https://github.com/Twotoz/C5VRX) (pinned commit) |
| `p4/` | ESP32-P4 firmware (ESP-IDF project) |
| `p4/components/cvbs_decode` | Composite decoder: sync, line PLL, PAL/NTSC chroma. Plain C, host-testable |
| `p4/components/vrx_capture` | PARLIO RX capture of the C5 bus |
| `p4/components/video_pipe` | Decoder tasks, field buffers, weave/bob, JPEG/H.264, NO SIGNAL screen |
| `p4/components/usb_composite` | TinyUSB descriptors and runtime: UVC + 3× CDC-ACM |
| `p4/components/elrs_tx` | ExpressLRS TX port: `setup.sh` fetches 4.1.0; `port/` holds the Arduino layer, SPI, timer and USB CRSF handset |
| `p4/components/gnss_bridge`, `c5_link` | L76K bridge, C5 reset and console link |
| `p4/host/` | Host tests and tools: decoder test with synthetic NTSC/PAL, CRSF test client |
| `docs/` | Wiring (with the WireViz source), usage, udev rules |

## Build

Both projects need **ESP-IDF 6.0.2**. C5VRX's filter and DC correction use PHY internals of exactly that release; other versions build with those features disabled.

```sh
# ESP32-C5 (C5VRX for the module)
c5/setup.sh            # clone C5VRX at the pinned commit, apply the board patch
c5/build.sh            # -> c5/C5VRX/build-p4c5/

# ESP32-P4
p4/components/elrs_tx/setup.sh   # ExpressLRS 4.1.0 (ELRS_TAG=3.x.y for 3.x receivers, untested)
cd p4 && idf.py build
```

Options are under `idf.py menuconfig` → "FPVBox …":
- video standard and decoder workers,
- JPEG quality and H.264 bitrate/GOP,
- USB VID/PID,
- ELRS power cap, binding phrase and EU LBT.

## Flash

The carrier's J7 (CH343) flashes either chip. Slide switch S1 selects the target: up = P4, down = C5.

- **P4:** S1 up, then `cd p4 && idf.py -p /dev/ttyACM0 flash monitor` (the CH343 may enumerate as ttyUSB0).
- **C5:** S1 down, then:
  ```sh
  python -m esptool --chip esp32c5 -p /dev/ttyACM0 write-flash @c5/C5VRX/build-p4c5/flash_args
  ```
  If the auto-reset does not work: send `` ` `` on the box's Control port. C5VRX reboots into ROM download mode. Then flash with `--before no-reset`.

## Tests

```sh
make -C p4/host test                 # decoder against synthetic NTSC/PAL colour bars
p4/host/crsf_test.py --selftest      # CRSF framing
wireviz -f sp docs/wiring/fpvbox.yml # regenerate the wiring diagram
```

**Verified so far, without hardware:**
- Both firmwares build. The C5 patch also keeps the upstream XIAO build compiling.
- The decoder reproduces BT.601 colour bars within 3 levels at 6-bit input, with noise σ 0.4 and 1.5 codes and a +35 ppm clock offset. That run covers sync lock, field parity, PAL V-switch, AFC and the NTSC/PAL decision.

**Open hardware risks** are tracked in the checklist in the [README](../README.md#to-do-and-verification-checklist).
