# ExpressLRS 2.4 GHz TX on the ESP32-P4 (FPVBox). Upstream sources come from
# ./ExpressLRS (run ./setup.sh); the port layer in ./port provides the
# Arduino surface, SPI, timer, NVS EEPROM, hardware layout and the USB CRSF
# handset. Without ./ExpressLRS the component builds a stub.
set(ELRS "${CMAKE_CURRENT_LIST_DIR}/ExpressLRS/src")

if(NOT EXISTS "${ELRS}/src/tx_main.cpp")
    message(WARNING "ExpressLRS sources missing: run components/elrs_tx/setup.sh (building the ELRS stub)")
    idf_component_register(SRCS "elrs_stub.c" INCLUDE_DIRS "include")
    return()
endif()

set(elrs_libs
    CONFIG CrsfProtocol DEVICE FHSS Handset MSP OPTIONS OTA POWERMGNT SX1280Driver SX12xxDriverCommon
    elrs_eeprom logging tx-crsf StubbornSender StubbornReceiver CRC LQCALC FIFO HWTIMER LBT
    MeanAccumulator LED ADC BUTTON VTX WIFI BLE Backpack SCREEN GSENSOR THERMAL POWER_DETECT RFAMP
    MSPVTX CRSF2MSP PFD)
set(elrs_inc "${CMAKE_CURRENT_LIST_DIR}/port/include" "${ELRS}/include" "${ELRS}/src")
foreach(l ${elrs_libs})
    list(APPEND elrs_inc "${ELRS}/lib/${l}")
endforeach()

set(elrs_srcs
    "${ELRS}/src/tx_main.cpp"
    "${ELRS}/src/common.cpp"
    "${ELRS}/src/dynpower.cpp"
    "${ELRS}/src/rxtx_common.cpp"
    "${ELRS}/lib/CONFIG/config.cpp"
    "${ELRS}/lib/CrsfProtocol/CRSFConnector.cpp"
    "${ELRS}/lib/CrsfProtocol/CRSFEndpoint.cpp"
    "${ELRS}/lib/CrsfProtocol/CRSFParser.cpp"
    "${ELRS}/lib/CrsfProtocol/CRSFRouter.cpp"
    "${ELRS}/lib/CrsfProtocol/RxTxEndpoint.cpp"
    "${ELRS}/lib/DEVICE/device.cpp"
    "${ELRS}/lib/FHSS/FHSS.cpp"
    "${ELRS}/lib/FHSS/random.cpp"
    "${ELRS}/lib/MSP/msp.cpp"
    "${ELRS}/lib/OTA/OTA.cpp"
    "${ELRS}/lib/POWERMGNT/POWERMGNT.cpp"
    "${ELRS}/lib/SX1280Driver/SX1280.cpp"
    "${ELRS}/lib/SX1280Driver/SX1280_hal.cpp"
    "${ELRS}/lib/elrs_eeprom/elrs_eeprom.cpp"
    "${ELRS}/lib/logging/logging.cpp"
    "${ELRS}/lib/tx-crsf/TXModuleEndpoint.cpp"
    "${ELRS}/lib/tx-crsf/TXModuleParameters.cpp"
    "${ELRS}/lib/tx-crsf/TXOTAConnector.cpp"
    "${ELRS}/lib/tx-crsf/TXUSBConnector.cpp"
    "${ELRS}/lib/tx-crsf/devTXLUA.cpp"
    "${ELRS}/lib/StubbornSender/stubborn_sender.cpp"
    "${ELRS}/lib/StubbornReceiver/stubborn_receiver.cpp"
    "${ELRS}/lib/RFAMP/RFAMP_hal.cpp"
)
file(GLOB elrs_lbt "${ELRS}/lib/LBT/*.cpp")
file(GLOB elrs_crc "${ELRS}/lib/CRC/*.cpp")
list(APPEND elrs_srcs ${elrs_lbt} ${elrs_crc})

set(port_srcs
    "port/arduino_core.cpp"
    "port/spiex.cpp"
    "port/hwtimer.cpp"
    "port/eeprom.cpp"
    "port/options.cpp"
    "port/stubs.cpp"
    "port/usb_handset.cpp"
    "port/elrs_glue.cpp"
)

idf_component_register(SRCS ${elrs_srcs} ${port_srcs}
                       INCLUDE_DIRS "include"
                       PRIV_INCLUDE_DIRS ${elrs_inc} "port"
                       PRIV_REQUIRES fpv_board usb_composite esp_driver_gpio esp_driver_spi esp_driver_gptimer esp_driver_uart
                                     esp_timer nvs_flash mbedtls esp_partition app_update esp_hw_support)

target_compile_definitions(${COMPONENT_LIB} PRIVATE
    TARGET_TX=1 RADIO_SX128X=1 PLATFORM_ESP32=1 PLATFORM_ESP32_P4=1
    MAVLINK_NO_SIGN_PACKET MAVLINK_NO_SIGNATURE_CHECK CONFIG_DISABLE_HAL_LOCKS=1)
if(CONFIG_FPV_ELRS_EU_LBT)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE Regulatory_Domain_EU_CE_2400=1)
endif()
target_compile_options(${COMPONENT_LIB} PRIVATE
    "SHELL:-include ${CMAKE_CURRENT_LIST_DIR}/port/include/elrs_port_pre.h"
    "SHELL:-include target/Unified_ESP32_TX.h"
    -O2 -Wno-error -Wno-unused-variable -Wno-unused-function -Wno-missing-field-initializers
    $<$<COMPILE_LANGUAGE:CXX>:-Wno-volatile>)
