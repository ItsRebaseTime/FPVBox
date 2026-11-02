/* Control CDC port (see control.h).
 *
 * Host -> box: a line starting with '!' is a box command, everything else is
 * passed to the C5VRX console byte by byte (its single-key commands, e.g.
 * 'c' next channel, 'b' next band, 's' scan, 'T' status; see C5VRX README).
 * Box -> host: C5VRX console output as-is; box replies are prefixed "BOX ".
 */
#include "control.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "c5_link.h"
#include "elrs_tx.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "gnss_bridge.h"
#include "usb_composite.h"
#include "video_pipe.h"

static char s_cmd[96];
static size_t s_cmd_len;
static bool s_in_cmd, s_line_start = true;

static void ctrl_out(const char *line, void *ctx)
{
    (void)ctx;
    usb_cdc_write(USB_CDC_CTRL, line, strlen(line));
}

static void reply(const char *fmt, ...)
{
    char buf[192];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf) - 2, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (n > (int)sizeof(buf) - 3) n = (int)sizeof(buf) - 3;
    buf[n++] = '\r';
    buf[n++] = '\n';
    usb_cdc_write(USB_CDC_CTRL, "BOX ", 4);
    usb_cdc_write(USB_CDC_CTRL, buf, (size_t)n);
    usb_cdc_flush(USB_CDC_CTRL);
}

static const char *std_name(cvbs_std_t s) { return s == CVBS_STD_PAL ? "PAL" : s == CVBS_STD_NTSC ? "NTSC" : "none"; }

void control_print_status(void (*out)(const char *line, void *ctx), void *ctx)
{
    char l[224];
    video_pipe_status_t v;
    video_pipe_get_status(&v);
    snprintf(l, sizeof(l), "video locked=%d std=%s color=%d line_us=%.3f burst=%.2f afc_ppm=%.1f fields=%lu "
             "fields_dropped=%lu lock_losses=%lu lines_dropped=%lu cap_overruns=%lu cap_timeouts=%lu\r\n",
             v.locked, std_name(v.std), v.color, (double)v.line_period_us, (double)v.burst_ratio, (double)v.afc_ppm,
             (unsigned long)v.fields_decoded, (unsigned long)v.fields_dropped, (unsigned long)v.lock_losses,
             (unsigned long)v.lines_dropped, (unsigned long)v.capture_overruns, (unsigned long)v.capture_timeouts);
    out(l, ctx);
    snprintf(l, sizeof(l), "uvc streaming=%d fmt=%s %ux%u fps=%.1f sent=%lu dropped=%lu enc_bytes=%lu decode_ms=%.1f\r\n",
             v.streaming, v.stream.format == USB_VIDEO_YUYV ? "YUYV" : v.stream.format == USB_VIDEO_MJPEG ? "MJPEG" : "H264",
             v.stream.width, v.stream.height, (double)v.out_fps, (unsigned long)v.frames_sent,
             (unsigned long)v.frames_dropped, (unsigned long)v.last_encoded_bytes, (double)v.decode_ms);
    out(l, ctx);
    c5_status_t c;
    c5_link_get_status(&c);
    snprintf(l, sizeof(l), "c5 alive=%d channel=%s mhz=%u lines=%lu resets=%lu\r\n", c.alive, c.channel, c.mhz,
             (unsigned long)c.lines, (unsigned long)c.resets);
    out(l, ctx);
    gnss_status_t g;
    gnss_bridge_get_status(&g);
    snprintf(l, sizeof(l), "gnss seen=%d baud=%lu fix=%u sats=%u pps=%lu rx=%llu tx=%llu\r\n", g.seen,
             (unsigned long)g.baud, g.fix_quality, g.satellites, (unsigned long)g.pps_count,
             (unsigned long long)g.bytes_from_gnss, (unsigned long long)g.bytes_to_gnss);
    out(l, ctx);
    elrs_tx_status_t e;
    elrs_tx_get_status(&e);
    snprintf(l, sizeof(l), "elrs state=%s rate=%uHz power=%ddBm lq=%u rssi=%d snr=%d host_frames=%lu "
             "tx_packets=%lu telem=%lu\r\n", elrs_tx_state_name(e.state), e.rate_hz, e.power_dbm,
             e.uplink_lq, e.downlink_rssi, e.downlink_snr, (unsigned long)e.host_frames,
             (unsigned long)e.tx_packets, (unsigned long)e.telemetry_frames);
    out(l, ctx);
}

static void box_status_line(const char *line, void *ctx)
{
    (void)ctx;
    usb_cdc_write(USB_CDC_CTRL, "BOX ", 4);
    usb_cdc_write(USB_CDC_CTRL, line, strlen(line));
}

static void run_command(char *cmd)
{
    char *arg = strchr(cmd, ' ');
    if (arg) *arg++ = 0;
    if (!strcmp(cmd, "status")) {
        control_print_status(box_status_line, NULL);
        usb_cdc_flush(USB_CDC_CTRL);
    } else if (!strcmp(cmd, "c5reset")) {
        c5_link_reset();
        reply("ok");
    } else if (!strcmp(cmd, "color")) {
        const bool on = !arg || strcmp(arg, "off") != 0;
        video_pipe_set_color(on, 0.0f);
        reply("color %s", on ? "on" : "off");
    } else if (!strcmp(cmd, "sat")) {
        const float sat = arg ? strtof(arg, NULL) : 1.0f;
        video_pipe_set_color(true, sat > 0.05f ? sat : 1.0f);
        reply("saturation %.2f", (double)sat);
    } else if (!strcmp(cmd, "elrs")) {
        char out[160];
        elrs_tx_command(arg ? arg : "", out, sizeof(out));
        reply("%s", out);
    } else if (!strcmp(cmd, "reboot")) {
        reply("rebooting");
        esp_restart();
    } else {
        reply("commands: !status  !c5reset  !color on|off  !sat <x>  !elrs <cmd>  !reboot");
        reply("other input goes to the C5VRX console (c/b/s/T ... see C5VRX README)");
    }
}

static void on_ctrl_rx(const uint8_t *data, size_t len, void *ctx)
{
    (void)ctx;
    size_t fwd_from = 0;
    for (size_t i = 0; i < len; ++i) {
        const char c = (char)data[i];
        if (s_in_cmd) {
            if (c == '\r' || c == '\n') {
                s_cmd[s_cmd_len] = 0;
                s_in_cmd = false;
                s_line_start = true;
                run_command(s_cmd);
            } else if (s_cmd_len < sizeof(s_cmd) - 1) {
                s_cmd[s_cmd_len++] = c;
            }
            fwd_from = i + 1;
            continue;
        }
        if (s_line_start && c == '!') {
            if (i > fwd_from) c5_link_write(data + fwd_from, i - fwd_from);
            s_in_cmd = true;
            s_cmd_len = 0;
            fwd_from = i + 1;
            continue;
        }
        s_line_start = c == '\r' || c == '\n';
    }
    if (!s_in_cmd && len > fwd_from) c5_link_write(data + fwd_from, len - fwd_from);
}

void control_c5_output(const uint8_t *data, size_t len, void *ctx)
{
    (void)ctx;
    if (!usb_cdc_connected(USB_CDC_CTRL)) return;
    usb_cdc_write(USB_CDC_CTRL, data, len);
    usb_cdc_flush(USB_CDC_CTRL);
}

void control_start(void)
{
    usb_cdc_set_rx_cb(USB_CDC_CTRL, on_ctrl_rx, NULL);
    (void)ctrl_out;
}
