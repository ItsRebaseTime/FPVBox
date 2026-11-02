# Using FPVBox

Connect the carrier's **J8 ("USB") Type-C** to the host. Optional: `sudo cp docs/99-fpvbox.rules /etc/udev/rules.d/` gives stable Linux device names (used below).

## Video (UVC, `/dev/fpvbox-video`)

| Format | Sizes | Rate | Notes |
|---|---|---|---|
| YUYV | 720×240 / 720×288 | 59.94 / 50 | Single fields, sent as soon as decoded: lowest latency, no decoding on the host |
| YUYV | 720×480 / 720×576 | 29.97 / 25 | Woven frames |
| MJPEG | 720×480 / 720×576 | 59.94 / 50 | Line-interpolated fields, P4 hardware JPEG |
| H.264 | 720×480 / 720×576 | 59.94 / 50 | P4 hardware encoder; hardware-decoded by Snapdragon (Steam Frame) and most SoCs |

```sh
v4l2-ctl -d /dev/fpvbox-video --list-formats-ext
ffplay -f v4l2 -input_format yuyv422 -video_size 720x240 -fflags nobuffer /dev/fpvbox-video
ffplay -f v4l2 -input_format mjpeg   -video_size 720x480 -fflags nobuffer /dev/fpvbox-video
ffplay -f v4l2 -input_format h264    -video_size 720x480 -fflags nobuffer /dev/fpvbox-video
```

- **Standard:** PAL or NTSC is detected automatically. If you pick the other standard's size, lines are rescaled.
- **NO SIGNAL:** without a lock the box sends a NO SIGNAL screen with the receiver channel.

## GNSS (`/dev/fpvbox-gnss`)

- Raw L76K NMEA, both directions, behaving like a USB-UART adapter. The host's baud setting is applied to the GNSS UART, so `$PCAS01` baud changes work.
- `gpsd -N /dev/fpvbox-gnss`, then `cgps`.
- Android: any USB-GPS app that uses usb-serial-for-android (CDC-ACM).

## ELRS (`/dev/fpvbox-elrs`)

- **The host plays the radio.** It sends CRSF RC channel frames (type 0x16, sync byte 0xC8) at the packet rate.
- **The box answers with:**
  - link statistics (0x14),
  - all telemetry from the receiver (GPS, battery, attitude, flight mode, MSP),
  - EdgeTX-style timing frames (0x3A).
- **One radio:** control and telemetry use the same SX1280.
- **Settings** (packet rate, power, telemetry ratio, bind) work through the standard CRSF parameter protocol, as with the ELRS Lua script, or via `!elrs ...` on the Control port.
- **Failsafe:** the TX transmits only while the host sends channel frames. Without host frames for one second, the receiver goes to failsafe.
- **Receivers** must run the same ELRS **major version** (4.x by default) and binding phrase (`CONFIG_FPV_ELRS_BIND_PHRASE`), or use `!elrs bind` with the receiver in bind mode.

```sh
p4/host/crsf_test.py /dev/fpvbox-elrs --rate 250
```

## Control (`/dev/fpvbox-ctrl`)

- Lines starting with `!` are box commands:
  - `!status`
  - `!c5reset`
  - `!color on|off`
  - `!sat 1.2`
  - `!elrs bind|power N|rate N|tlm N`
  - `!reboot`
- Everything else goes to the C5VRX console, e.g. `c` next channel, `b` next band, `s` scan, `T` status (see the [C5VRX README](https://github.com/Twotoz/C5VRX)).
