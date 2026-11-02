#!/usr/bin/env python3
"""Drive the FPVBox ELRS TX over its USB CDC port with CRSF.

Sends RC channel frames (centre sticks, throttle low unless --throttle) at
--rate Hz and prints what comes back: link statistics, telemetry from the
receiver and the timing frames the TX uses to align the host's packets.

    pip install pyserial
    ./crsf_test.py /dev/fpvbox-elrs            # or /dev/ttyACM0
    ./crsf_test.py /dev/fpvbox-elrs --rate 500 --seconds 30
    ./crsf_test.py --selftest                   # CRC/packing check, no device
"""
import argparse
import struct
import sys
import time

CRSF_SYNC = 0xC8
TYPE_RC = 0x16
NAMES = {
    0x02: "GPS", 0x07: "VARIO", 0x08: "BATTERY", 0x09: "BARO_ALT", 0x14: "LINK_STATS",
    0x1E: "ATTITUDE", 0x21: "FLIGHT_MODE", 0x29: "DEVICE_INFO", 0x2B: "PARAM_ENTRY",
    0x32: "COMMAND", 0x3A: "RADIO_ID/TIMING",
}


def crc8_dvb_s2(data: bytes) -> int:
    crc = 0
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = ((crc << 1) ^ 0xD5) & 0xFF if crc & 0x80 else (crc << 1) & 0xFF
    return crc


def us_to_crsf(us: int) -> int:
    """1000..2000 us -> CRSF 172..1811 (OpenTX/EdgeTX mapping)."""
    return max(172, min(1811, round((us - 1500) * 8 / 5 + 992)))


def rc_frame(channels_us) -> bytes:
    bits = 0
    for i, us in enumerate(channels_us[:16]):
        bits |= us_to_crsf(us) << (11 * i)
    payload = bits.to_bytes(22, "little")
    body = bytes([TYPE_RC]) + payload
    return bytes([CRSF_SYNC, len(body) + 1]) + body + bytes([crc8_dvb_s2(body)])


def describe(ftype: int, p: bytes) -> str:
    if ftype == 0x14 and len(p) >= 10:
        r1, r2, lq, snr, ant, mode, pwr, drssi, dlq, dsnr = struct.unpack("<BBBbBBBBBb", p[:10])
        return (f"uplink rssi -{r1}/-{r2} dBm lq {lq}% snr {snr} ant {ant} mode {mode} power {pwr} | "
                f"downlink rssi -{drssi} dBm lq {dlq}% snr {dsnr}")
    if ftype == 0x08 and len(p) >= 8:
        v, a = struct.unpack(">HH", p[:4])
        cap = int.from_bytes(p[4:7], "big")
        return f"{v / 10:.1f} V {a / 10:.1f} A {cap} mAh {p[7]}%"
    if ftype == 0x02 and len(p) >= 15:
        lat, lon, gs, hdg, alt, sats = struct.unpack(">iiHHHB", p[:15])
        return f"lat {lat / 1e7:.6f} lon {lon / 1e7:.6f} {gs / 36:.1f} m/s alt {alt - 1000} m sats {sats}"
    if ftype == 0x21:
        return p.split(b"\0")[0].decode(errors="replace")
    if ftype == 0x1E and len(p) >= 6:
        pitch, roll, yaw = struct.unpack(">hhh", p[:6])
        return f"pitch {pitch / 10000:.2f} roll {roll / 10000:.2f} yaw {yaw / 10000:.2f} rad"
    if ftype == 0x3A and len(p) >= 11 and p[2] == 0x10:
        rate, offset = struct.unpack(">ii", p[3:11])
        return f"timing: packet interval {rate / 10:.1f} us, offset {offset / 10:.1f} us"
    return p.hex()


class Deframer:
    def __init__(self):
        self.buf = bytearray()

    def feed(self, data: bytes):
        self.buf += data
        while len(self.buf) >= 2:
            if self.buf[0] not in (0xC8, 0xEA, 0xEE, 0xEC):
                del self.buf[0]
                continue
            n = self.buf[1]
            if n < 2 or n > 62:
                del self.buf[0]
                continue
            if len(self.buf) < n + 2:
                return
            frame = bytes(self.buf[:n + 2])
            del self.buf[:n + 2]
            body = frame[2:-1]
            if crc8_dvb_s2(body) != frame[-1]:
                print("bad crc", frame.hex())
                continue
            ftype, payload = body[0], body[1:]
            if ftype >= 0x28:   # extended header: destination, origin
                payload = payload[2:]
            yield ftype, payload


def selftest() -> int:
    f = rc_frame([1500] * 16)
    assert f[0] == CRSF_SYNC and f[1] == 24 and f[2] == TYPE_RC and len(f) == 26
    assert crc8_dvb_s2(f[2:-1]) == f[-1]
    # 992 in every channel: the bit pattern repeats every 11 bits.
    assert int.from_bytes(f[3:25], "little") & 0x7FF == 992
    d = Deframer()
    out = list(d.feed(b"\x00\x01" + f))
    assert out and out[0][0] == TYPE_RC
    print("selftest ok")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("port", nargs="?")
    ap.add_argument("--rate", type=int, default=250, help="RC frames per second (match the ELRS packet rate)")
    ap.add_argument("--seconds", type=float, default=0, help="stop after this time (0: run until Ctrl-C)")
    ap.add_argument("--throttle", type=int, default=1000, help="channel 3 in us")
    ap.add_argument("--arm", action="store_true", help="channel 5 high (ELRS uses CH5 as the arm switch)")
    ap.add_argument("--selftest", action="store_true")
    a = ap.parse_args()
    if a.selftest:
        return selftest()
    if not a.port:
        ap.error("port required")
    import serial  # pyserial
    ser = serial.Serial(a.port, 115200, timeout=0)   # baud is ignored by USB CDC
    ser.dtr = True
    channels = [1500] * 16
    channels[2] = a.throttle
    channels[4] = 2000 if a.arm else 1000
    frame = rc_frame(channels)
    period = 1.0 / a.rate
    deframer = Deframer()
    t0 = time.monotonic()
    next_tx = t0
    last_print = {}
    try:
        while not a.seconds or time.monotonic() - t0 < a.seconds:
            now = time.monotonic()
            if now >= next_tx:
                ser.write(frame)
                next_tx += period
                if next_tx < now:
                    next_tx = now + period
            for ftype, payload in deframer.feed(ser.read(512)):
                # Print each frame type at most twice a second.
                if now - last_print.get(ftype, 0) >= 0.5:
                    last_print[ftype] = now
                    print(f"{now - t0:7.2f}s {NAMES.get(ftype, hex(ftype)):>16}: {describe(ftype, payload)}")
            time.sleep(min(0.0005, max(0.0, next_tx - time.monotonic())))
    except KeyboardInterrupt:
        pass
    return 0


if __name__ == "__main__":
    sys.exit(main())
