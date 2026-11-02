/* CRSF "handset" on the FPVBox USB CDC port "ELRS".
 *
 * Replaces ExpressLRS' CRSFHandset (a half-duplex UART with autobaud and
 * duplex switching): the host program plays the radio and exchanges
 * standard CRSF frames over USB. RC channel frames reach the TX endpoint
 * through the CRSF router exactly as from a UART handset; everything the
 * router sends to the radio (link statistics, telemetry, parameters) goes
 * back over USB. The OpenTX/EdgeTX timing frames are sent as well, so a host
 * can align its channel frames with the packet rate. */
#include "targets.h"
#include "common.h"
#include "logging.h"
#include "handset.h"
#include "devHandset.h"
#include "CRSFHandset.h"
#include "CRSFRouter.h"
#include "CRSFConnector.h"
#include "crsf_protocol.h"
#include "elrs_port.h"
#include "freertos/stream_buffer.h"
#include "usb_composite.h"

/* CRSFHandset statistics read by the TX parameters (Lua "bad/good"). */
uint32_t CRSFHandset::GoodPktsCountResult = 0;
uint32_t CRSFHandset::BadPktsCountResult = 0;

static constexpr uint32_t SYNC_INTERVAL_MS = 200;
static constexpr int32_t SYNC_SAFE_MARGIN = 1000;   /* 100 us in 0.1 us units */
static constexpr uint32_t HOST_TIMEOUT_MS = 1000;

static StreamBufferHandle_t s_rx;
static TaskHandle_t s_waiter;
static volatile uint32_t s_host_frames, s_to_host;

class UsbCrsfHandset final : public Handset, public CRSFConnector
{
public:
    void Begin() override
    {
        addDevice(CRSF_ADDRESS_ELRS_LUA);
        addDevice(CRSF_ADDRESS_RADIO_TRANSMITTER);
        crsfRouter.addConnector(this);
        statsLast = millis();
    }

    void End() override {}

    void forwardMessage(const crsf_header_t *message) override
    {
        if (!controllerConnected) return;
        const uint8_t size = CRSF_FRAME_SIZE(message->frame_size);
        if (size > CRSF_MAX_PACKET_LEN) return;
        uint8_t out[CRSF_MAX_PACKET_LEN];
        memcpy(out, message, size);
        /* CRSF on a serial link always starts with the sync byte. */
        out[0] = CRSF_SYNC_BYTE;
        if (usb_cdc_write(USB_CDC_ELRS, out, size) == size) s_to_host = s_to_host + 1;
        usb_cdc_flush(USB_CDC_ELRS);
    }

    void handleInput() override
    {
        uint8_t buf[128];
        size_t n;
        while ((n = xStreamBufferReceive(s_rx, buf, sizeof(buf), 0)) > 0) {
            for (size_t i = 0; i < n; ++i) feed(buf[i]);
        }
        const uint32_t now = millis();
        if (controllerConnected) {
            if (now - lastFrameMs > HOST_TIMEOUT_MS) {
                controllerConnected = false;
                DBGLN("USB CRSF host lost");
                if (disconnected) disconnected();
            } else {
                sendSyncPacket(now);
            }
        }
        if (now - statsLast >= 1000) {
            CRSFHandset::GoodPktsCountResult = goodPkts;
            CRSFHandset::BadPktsCountResult = badPkts;
            goodPkts = badPkts = 0;
            statsLast = now;
        }
    }

    void setPacketInterval(int32_t interval) override
    {
        RequestedRCpacketInterval = interval;
        syncOffset = 0;
        syncWindow = 0;
        syncWindowSize = std::max((int32_t)1, (int32_t)(20000 / RequestedRCpacketInterval));
        syncLastSent -= SYNC_INTERVAL_MS;
    }

    void JustSentRFpacket() override
    {
        const uint32_t last = dataLastRecv;
        const int32_t delta = (int32_t)(micros() - last);
        if (delta >= (int32_t)RequestedRCpacketInterval) {
            syncOffset = -(delta % RequestedRCpacketInterval) * 10;
            syncWindow = 0;
            syncLastSent -= SYNC_INTERVAL_MS;
        } else {
            syncWindow = std::min(syncWindow + 1, syncWindowSize);
            syncOffset = ((syncOffset * (syncWindow - 1)) + delta * 10) / syncWindow;
        }
    }

    bool connectedToHost() const { return controllerConnected; }

private:
    uint8_t frame[CRSF_MAX_PACKET_LEN] = {};
    uint8_t pos = 0;
    uint32_t lastFrameMs = 0;
    volatile uint32_t dataLastRecv = 0;
    int32_t syncOffset = 0, syncWindow = 0, syncWindowSize = 1;
    uint32_t syncLastSent = 0;
    uint32_t goodPkts = 0, badPkts = 0, statsLast = 0;

    /* Byte-wise CRSF deframer: [addr/sync][len][type ... payload][crc]. */
    void feed(uint8_t b)
    {
        if (pos == 0) {
            if (b != CRSF_SYNC_BYTE && b != CRSF_ADDRESS_CRSF_TRANSMITTER) return;
        } else if (pos == 1) {
            if (b < 2 || b > CRSF_MAX_PACKET_LEN - 2) { pos = 0; return; }
        }
        frame[pos++] = b;
        if (pos < 2 || pos < frame[1] + 2) return;
        const uint8_t len = frame[1];
        const uint8_t crc = crsfRouter.crsf_crc.calc(&frame[2], len - 1);
        if (crc == frame[len + 1]) {
            goodPkts++;
            process();
        } else {
            badPkts++;
        }
        pos = 0;
    }

    void process()
    {
        dataLastRecv = micros();
        lastFrameMs = millis();
        s_host_frames = s_host_frames + 1;
        if (!controllerConnected) {
            controllerConnected = true;
            DBGLN("USB CRSF host connected");
            if (connected) connected();
        }
        crsfRouter.processMessage(this, (crsf_header_t *)frame);
    }

    void sendSyncPacket(uint32_t now)
    {
        if (now - syncLastSent < SYNC_INTERVAL_MS) return;
        CRSF_MK_EXT_FRAME_T(crsf_sync_packet_t) pkt = {
            .h = {
                CRSF_ADDRESS_RADIO_TRANSMITTER,
                CRSF_EXT_FRAME_SIZE(sizeof(crsf_sync_packet_t)),
                CRSF_FRAMETYPE_HANDSET,
                CRSF_ADDRESS_RADIO_TRANSMITTER,
                CRSF_ADDRESS_CRSF_TRANSMITTER,
            },
            .p = {
                .subType = CRSF_HANDSET_SUBCMD_TIMING,
                .rate = htobe32(RequestedRCpacketInterval * 10),
                .offset = htobe32(syncOffset - SYNC_SAFE_MARGIN),
            },
            .crc = 0,
        };
        pkt.crc = crsfRouter.crsf_crc.calc((uint8_t *)&pkt + CRSF_TELEMETRY_TYPE_INDEX, sizeof(pkt) - 3);
        crsfRouter.deliverMessageTo(CRSF_ADDRESS_RADIO_TRANSMITTER, (crsf_header_t *)&pkt);
        syncLastSent = now;
    }
};

static UsbCrsfHandset s_handset;
Handset *handset;

static void on_cdc_rx(const uint8_t *data, size_t len, void *ctx)
{
    (void)ctx;
    xStreamBufferSend(s_rx, data, len, 0);
    if (s_waiter) xTaskNotifyGive(s_waiter);
}

void usb_handset_attach(void)
{
    if (!s_rx) s_rx = xStreamBufferCreate(4096, 1);
    s_waiter = xTaskGetCurrentTaskHandle();
    usb_cdc_set_rx_cb(USB_CDC_ELRS, on_cdc_rx, nullptr);
}

uint32_t usb_handset_host_frames(void) { return s_host_frames; }
uint32_t usb_handset_frames_to_host(void) { return s_to_host; }
bool usb_handset_connected(void) { return s_handset.connectedToHost(); }

static bool initialize()
{
    handset = &s_handset;
    return true;
}

static int start()
{
    handset->Begin();
    return DURATION_IMMEDIATELY;
}

static int timeout()
{
    handset->handleInput();
    return DURATION_IMMEDIATELY;
}

device_t Handset_device = {
    .initialize = initialize,
    .start = start,
    .event = nullptr,
    .timeout = timeout,
    .subscribe = EVENT_NONE,
};
