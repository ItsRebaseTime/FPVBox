/* ExpressLRS TX devices and services the FPVBox does not use: the UI
 * devices are inert, there is no WiFi, BLE, backpack, screen or VTX. */
#include "targets.h"
#include "common.h"
#include "device.h"

device_t LED_device = {};
device_t RGB_device = {};
device_t ADC_device = {};
device_t WIFI_device = {};
device_t Button_device = {};
device_t BLE_device = {};
device_t Backpack_device = {};
device_t Screen_device = {};
device_t Gsensor_device = {};
device_t Thermal_device = {};
device_t PDET_device = {};
device_t VTX_device = {};

/* WiFi / backpack / button / VTX services referenced by tx_main and the
 * TX parameters. */
#include "msp.h"
#include "devButton.h"

bool webserverPreventAutoStart = true;
bool BackpackTelemReadyToSend = false;
bool TxBackpackWiFiReadyToSend = false;
bool VRxBackpackWiFiReadyToSend = false;

void setWifiUpdateMode() {}
void checkBackpackUpdate() {}
void sendCRSFTelemetryToBackpack(const uint8_t *data) { (void)data; }
void sendMAVLinkTelemetryToBackpack(const uint8_t *data) { (void)data; }
void processPanTiltRollPacket(uint32_t now, const mspPacket_t *packet) { (void)now; (void)packet; }
void registerButtonFunction(action_e action, ButtonAction_fn function) { (void)action; (void)function; }
void VtxTriggerSend() {}
void VtxPitmodeSwitchUpdate() {}
