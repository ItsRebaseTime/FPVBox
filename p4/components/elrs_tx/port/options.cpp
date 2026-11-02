/* ExpressLRS firmware options and hardware layout, compiled in for the
 * FPVBox (LilyGo "17AP" SX1280 + PA on the T-ETH-Elite LoRa shield, wired
 * to the ESP32-P4 as in docs/WIRING.md). Replaces lib/OPTIONS, which reads
 * JSON appended to the firmware image. */
#include "targets.h"
#include "options.h"
#include "esp_mac.h"
#include "esp_rom_md5.h"
#include "fpv_board.h"
#include "sdkconfig.h"

const unsigned char target_name[] = "\xBE\xEF\xCA\xFE" "FPVBOX P4 2400 TX";
const uint8_t target_name_size = sizeof(target_name);
const char commit[] = "fpvbox";
const char version[] = "fpvbox";
const char *wifi_hostname = "elrs_tx";
const char *wifi_ap_ssid = "ExpressLRS TX";
const char *wifi_ap_password = "expresslrs";
const char *wifi_ap_address = "10.0.0.1";
char product_name[ELRSOPTS_PRODUCTNAME_SIZE + 1] = "FPVBox 2.4GHz TX";
char device_name[ELRSOPTS_DEVICENAME_SIZE + 1] = "FPVBox TX";
uint32_t logo_image = 0;
firmware_options_t firmwareOptions;

static String s_empty;
String &getOptions() { return s_empty; }
String &getHardware() { return s_empty; }
void saveOptions() {}
void setOptions(String &) {}

/* LilyGo's PA gain is not published: power_values are SX1280 output
 * settings (dBm) for 10/25/50/100/250 mW assuming about +22 dB. Measure
 * before raising CONFIG_FPV_ELRS_MAX_POWER_IDX. */
static const int16_t POWER_VALUES[] = {-12, -8, -5, -2, 1};

static int s_int[HARDWARE_LAST];

bool hardware_init()
{
    for (int i = 0; i < HARDWARE_LAST; ++i) s_int[i] = UNDEF_PIN;
    /* Non-pin fields default to 0. */
    static const nameType zero_fields[] = {
        HARDWARE_customised, HARDWARE_radio_dcdc, HARDWARE_radio_rfo_hf, HARDWARE_radio_rfsw_ctrl_count,
        HARDWARE_power_lna_gain, HARDWARE_power_min, HARDWARE_power_max, HARDWARE_power_default,
        HARDWARE_power_control, HARDWARE_power_values_count, HARDWARE_power_values_dual_count,
        HARDWARE_led_blue_invert, HARDWARE_led_green_invert, HARDWARE_led_red_invert, HARDWARE_led_rgb_isgrb,
        HARDWARE_ledidx_rgb_status_count, HARDWARE_ledidx_rgb_vtx_count, HARDWARE_ledidx_rgb_boot_count,
        HARDWARE_screen_type, HARDWARE_screen_reversed, HARDWARE_screen_mirror, HARDWARE_use_backpack,
        HARDWARE_debug_backpack_baud, HARDWARE_passthrough_baud, HARDWARE_misc_fan_speeds_count,
        HARDWARE_gsensor_stk8xxx, HARDWARE_thermal_lm75a, HARDWARE_pwm_outputs_count, HARDWARE_pwm_out_only,
        HARDWARE_ant_group,
    };
    for (nameType f : zero_fields) s_int[f] = 0;

    s_int[HARDWARE_radio_busy] = FPV_SX_BUSY_GPIO;
    s_int[HARDWARE_radio_dio1] = FPV_SX_DIO1_GPIO;
    s_int[HARDWARE_radio_miso] = FPV_SX_MISO_GPIO;
    s_int[HARDWARE_radio_mosi] = FPV_SX_MOSI_GPIO;
    s_int[HARDWARE_radio_nss] = FPV_SX_NSS_GPIO;
    s_int[HARDWARE_radio_rst] = FPV_SX_RST_GPIO;
    s_int[HARDWARE_radio_sck] = FPV_SX_SCK_GPIO;
    s_int[HARDWARE_power_rxen] = FPV_SX_RXEN_GPIO;
    s_int[HARDWARE_power_txen] = FPV_SX_TXEN_GPIO;
    s_int[HARDWARE_power_lna_gain] = 12;
    const int max_idx = CONFIG_FPV_ELRS_MAX_POWER_IDX;
    s_int[HARDWARE_power_min] = 0;
    s_int[HARDWARE_power_max] = max_idx;
    s_int[HARDWARE_power_default] = max_idx < 2 ? max_idx : 2;
    s_int[HARDWARE_power_control] = 0;     /* SX1280 output power, no DAC */
    s_int[HARDWARE_power_values_count] = max_idx + 1;
    return true;
}

int hardware_pin(nameType name) { return s_int[name]; }
bool hardware_flag(nameType name) { return s_int[name] > 0; }
int hardware_int(nameType name) { return s_int[name]; }
float hardware_float(nameType name) { (void)name; return 0.0f; }
const int16_t *hardware_i16_array(nameType name) { return name == HARDWARE_power_values ? POWER_VALUES : nullptr; }
const uint16_t *hardware_u16_array(nameType name) { (void)name; return nullptr; }

/* UID: from the binding phrase like the ELRS build scripts
 * (MD5 of '-DMY_BINDING_PHRASE="<phrase>"'), otherwise the chip MAC. */
static void set_uid(void)
{
    const char *phrase = CONFIG_FPV_ELRS_BIND_PHRASE;
    if (phrase[0]) {
        char buf[160];
        const int n = snprintf(buf, sizeof(buf), "-DMY_BINDING_PHRASE=\"%s\"", phrase);
        md5_context_t c;
        uint8_t digest[16];
        esp_rom_md5_init(&c);
        esp_rom_md5_update(&c, buf, (uint32_t)n);
        esp_rom_md5_final(digest, &c);
        memcpy(firmwareOptions.uid, digest, 6);
    } else {
        uint8_t mac[8] = {0};
        esp_read_mac(mac, ESP_MAC_BASE);
        memcpy(firmwareOptions.uid, mac, 6);
    }
    firmwareOptions.hasUID = 1;
}

bool options_init()
{
    memset(&firmwareOptions, 0, sizeof(firmwareOptions));
    firmwareOptions._version_ = 1;
    firmwareOptions.domain = 0;
    firmwareOptions.flash_discriminator = 0x46505642;   /* "FPVB" */
    firmwareOptions.fan_min_runtime = 30;
    firmwareOptions.wifi_auto_on_interval = -1;
    firmwareOptions.tlm_report_interval = 240;
    firmwareOptions.unlock_higher_power = false;
    firmwareOptions.is_airport = false;
    firmwareOptions.uart_baud = 460800;
    set_uid();
    return hardware_init();
}

void elrs_options_prepare(void) { options_init(); }
