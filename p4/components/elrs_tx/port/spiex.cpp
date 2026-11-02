/* ExpressLRS SPIEx on the ESP-IDF SPI master (SPI2, polling, no DMA).
 * The SX1280 transfers are short (command + at most an OTA packet), well
 * inside the 64-byte FIFO of a non-DMA transaction. */
#include "SPIEx.h"
#include <string.h>
#include "driver/spi_master.h"
#include "esp_log.h"
#include "fpv_board.h"

static const char *TAG = "elrs_spi";

SPIExClass SPIEx;

static spi_device_handle_t s_dev;
static bool s_bus;
static int8_t s_cs = -1;

void SPIClass::begin(int8_t sck, int8_t miso, int8_t mosi, int8_t ss)
{
    if (s_bus) return;
    spi_bus_config_t bus = {};
    bus.sclk_io_num = sck >= 0 ? sck : FPV_SX_SCK_GPIO;
    bus.miso_io_num = miso >= 0 ? miso : FPV_SX_MISO_GPIO;
    bus.mosi_io_num = mosi >= 0 ? mosi : FPV_SX_MOSI_GPIO;
    bus.quadwp_io_num = -1;
    bus.quadhd_io_num = -1;
    bus.max_transfer_sz = 64;
    if (spi_bus_initialize(FPV_SX_SPI_HOST, &bus, SPI_DMA_DISABLED) != ESP_OK) {
        ESP_LOGE(TAG, "spi bus init failed");
        return;
    }
    s_bus = true;
    s_cs = ss >= 0 ? ss : FPV_SX_NSS_GPIO;
}

void SPIClass::end()
{
    if (s_dev) { spi_bus_remove_device(s_dev); s_dev = nullptr; }
    if (s_bus) { spi_bus_free(FPV_SX_SPI_HOST); s_bus = false; }
}

void SPIExClass::_transfer(uint8_t cs_mask, uint8_t *data, uint32_t size, bool reading)
{
    (void)cs_mask;      /* one radio */
    if (!s_bus || !size) return;
    if (!s_dev) {
        /* Added on first use: the frequency is set after begin(). */
        spi_device_interface_config_t dev = {};
        dev.mode = 0;
        dev.clock_speed_hz = (int)freq_;
        dev.spics_io_num = s_cs;
        dev.queue_size = 1;
        if (spi_bus_add_device(FPV_SX_SPI_HOST, &dev, &s_dev) != ESP_OK) {
            ESP_LOGE(TAG, "spi device add failed");
            return;
        }
    }
    if (size > 64) {
        ESP_LOGE(TAG, "transfer of %u bytes exceeds the FIFO", (unsigned)size);
        return;
    }
    uint8_t rx[64];
    spi_transaction_t t = {};
    t.length = size * 8u;
    t.tx_buffer = data;
    t.rx_buffer = reading ? rx : nullptr;
    if (spi_device_polling_transmit(s_dev, &t) == ESP_OK && reading) memcpy(data, rx, size);
}
