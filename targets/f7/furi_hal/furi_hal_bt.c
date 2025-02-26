#include <furi_hal_bt.h>

#include <ble/ble.h>
#include <interface/patterns/ble_thread/shci/shci.h>

#include <stm32wbxx.h>
#include <stm32wbxx_ll_hsem.h>

#include <hsem_map.h>

#include <furi_hal_version.h>
#include <furi_hal_power.h>
#include <furi_hal_bt_hid.h>
#include <furi_hal_bt_serial.h>
#include <furi_hal_bus.c>
#include <services/battery_service.h>
#include <furi.h>

#include <xtreme/xtreme.h>

#define TAG "FuriHalBt"

/* Time, in ms, to wait for mode transition before crashing */
#define C2_MODE_SWITCH_TIMEOUT 10000

#define FURI_HAL_BT_HARDFAULT_INFO_MAGIC 0x1170FD0F

typedef struct {
    FuriMutex* core2_mtx;
    FuriTimer* hardfault_check_timer;
    FuriHalBtStack stack;
} FuriHalBt;

static FuriHalBt furi_hal_bt = {
    .core2_mtx = NULL,
    .hardfault_check_timer = NULL,
    .stack = FuriHalBtStackUnknown,
};

typedef void (*FuriHalBtProfileStart)(void);
typedef void (*FuriHalBtProfileStop)(void);

typedef struct {
    FuriHalBtProfileStart start;
    FuriHalBtProfileStart stop;
    GapConfig config;
    uint16_t appearance_char;
    uint16_t advertise_service_uuid;
} FuriHalBtProfileConfig;

FuriHalBtProfileConfig profile_config[FuriHalBtProfileNumber] = {
    [FuriHalBtProfileSerial] =
        {
            .start = furi_hal_bt_serial_start,
            .stop = furi_hal_bt_serial_stop,
            .config =
                {
                    .adv_service_uuid = 0x3080,
                    .appearance_char = 0x8600,
                    .bonding_mode = true,
                    .pairing_method = GapPairingPinCodeShow,
                    .mac_address = FURI_HAL_BT_DEFAULT_MAC_ADDR,
                    .conn_param =
                        {
                            .conn_int_min = 0x18, // 30 ms
                            .conn_int_max = 0x24, // 45 ms
                            .slave_latency = 0,
                            .supervisor_timeout = 0,
                        },
                },
        },
    [FuriHalBtProfileHidKeyboard] =
        {
            .start = furi_hal_bt_hid_start,
            .stop = furi_hal_bt_hid_stop,
            .config =
                {
                    .adv_service_uuid = HUMAN_INTERFACE_DEVICE_SERVICE_UUID,
                    .appearance_char = GAP_APPEARANCE_KEYBOARD,
                    .bonding_mode = true,
                    .pairing_method = GapPairingPinCodeVerifyYesNo,
                    .mac_address = FURI_HAL_BT_DEFAULT_MAC_ADDR,
                    .conn_param =
                        {
                            .conn_int_min = 0x18, // 30 ms
                            .conn_int_max = 0x24, // 45 ms
                            .slave_latency = 0,
                            .supervisor_timeout = 0,
                        },
                },
        },
};
FuriHalBtProfileConfig* current_profile = NULL;

static void furi_hal_bt_hardfault_check(void* context) {
    UNUSED(context);
    if(furi_hal_bt_get_hardfault_info()) {
        furi_crash("ST(R) Copro(R) HardFault");
    }
}

void furi_hal_bt_init() {
    furi_hal_bus_enable(FuriHalBusHSEM);
    furi_hal_bus_enable(FuriHalBusIPCC);
    furi_hal_bus_enable(FuriHalBusAES2);
    furi_hal_bus_enable(FuriHalBusPKA);
    furi_hal_bus_enable(FuriHalBusCRC);

    if(!furi_hal_bt.core2_mtx) {
        furi_hal_bt.core2_mtx = furi_mutex_alloc(FuriMutexTypeNormal);
        furi_assert(furi_hal_bt.core2_mtx);
    }

    if(!furi_hal_bt.hardfault_check_timer) {
        furi_hal_bt.hardfault_check_timer =
            furi_timer_alloc(furi_hal_bt_hardfault_check, FuriTimerTypePeriodic, NULL);
        furi_timer_start(furi_hal_bt.hardfault_check_timer, 5000);
    }

    // Explicitly tell that we are in charge of CLK48 domain
    furi_check(LL_HSEM_1StepLock(HSEM, CFG_HW_CLK48_CONFIG_SEMID) == 0);

    // Start Core2
    ble_glue_init();
}

void furi_hal_bt_lock_core2() {
    furi_assert(furi_hal_bt.core2_mtx);
    furi_check(furi_mutex_acquire(furi_hal_bt.core2_mtx, FuriWaitForever) == FuriStatusOk);
}

void furi_hal_bt_unlock_core2() {
    furi_assert(furi_hal_bt.core2_mtx);
    furi_check(furi_mutex_release(furi_hal_bt.core2_mtx) == FuriStatusOk);
}

static bool furi_hal_bt_radio_stack_is_supported(const BleGlueC2Info* info) {
    bool supported = false;
    if(info->StackType == INFO_STACK_TYPE_BLE_LIGHT) {
        if(info->VersionMajor >= FURI_HAL_BT_STACK_VERSION_MAJOR &&
           info->VersionMinor >= FURI_HAL_BT_STACK_VERSION_MINOR) {
            furi_hal_bt.stack = FuriHalBtStackLight;
            supported = true;
        }
    } else if(info->StackType == INFO_STACK_TYPE_BLE_FULL) {
        if(info->VersionMajor >= FURI_HAL_BT_STACK_VERSION_MAJOR &&
           info->VersionMinor >= FURI_HAL_BT_STACK_VERSION_MINOR) {
            furi_hal_bt.stack = FuriHalBtStackFull;
            supported = true;
        }
    } else {
        furi_hal_bt.stack = FuriHalBtStackUnknown;
    }
    return supported;
}

bool furi_hal_bt_start_radio_stack() {
    bool res = false;
    furi_assert(furi_hal_bt.core2_mtx);

    furi_mutex_acquire(furi_hal_bt.core2_mtx, FuriWaitForever);

    // Explicitly tell that we are in charge of CLK48 domain
    furi_check(LL_HSEM_1StepLock(HSEM, CFG_HW_CLK48_CONFIG_SEMID) == 0);

    do {
        // Wait until C2 is started or timeout
        if(!ble_glue_wait_for_c2_start(FURI_HAL_BT_C2_START_TIMEOUT)) {
            FURI_LOG_E(TAG, "Core2 start failed");
            ble_glue_thread_stop();
            break;
        }

        // If C2 is running, start radio stack fw
        if(!furi_hal_bt_ensure_c2_mode(BleGlueC2ModeStack)) {
            break;
        }

        // Check whether we support radio stack
        const BleGlueC2Info* c2_info = ble_glue_get_c2_info();
        if(!furi_hal_bt_radio_stack_is_supported(c2_info)) {
            FURI_LOG_E(TAG, "Unsupported radio stack");
            // Don't stop SHCI for crypto enclave support
            break;
        }
        // Starting radio stack
        if(!ble_glue_start()) {
            FURI_LOG_E(TAG, "Failed to start radio stack");
            ble_glue_thread_stop();
            ble_app_thread_stop();
            break;
        }
        res = true;
    } while(false);
    furi_mutex_release(furi_hal_bt.core2_mtx);

    return res;
}

FuriHalBtStack furi_hal_bt_get_radio_stack() {
    return furi_hal_bt.stack;
}

bool furi_hal_bt_is_ble_gatt_gap_supported() {
    if(furi_hal_bt.stack == FuriHalBtStackLight || furi_hal_bt.stack == FuriHalBtStackFull) {
        return true;
    } else {
        return false;
    }
}

bool furi_hal_bt_is_testing_supported() {
    if(furi_hal_bt.stack == FuriHalBtStackFull) {
        return true;
    } else {
        return false;
    }
}

bool furi_hal_bt_start_app(FuriHalBtProfile profile, GapEventCallback event_cb, void* context) {
    furi_assert(event_cb);
    furi_assert(profile < FuriHalBtProfileNumber);
    bool ret = false;

    do {
        if(!ble_glue_is_radio_stack_ready()) {
            FURI_LOG_E(TAG, "Can't start BLE App - radio stack did not start");
            break;
        }
        if(!furi_hal_bt_is_ble_gatt_gap_supported()) {
            FURI_LOG_E(TAG, "Can't start Ble App - unsupported radio stack");
            break;
        }
        GapConfig* config = &profile_config[profile].config;
        // Configure GAP
        if(profile == FuriHalBtProfileSerial) {
            // Set mac address
            memcpy(
                config->mac_address, furi_hal_version_get_ble_mac(), sizeof(config->mac_address));
            // Set advertise name
            strlcpy(
                config->adv_name,
                furi_hal_version_get_ble_local_device_name_ptr(),
                FURI_HAL_VERSION_DEVICE_NAME_LENGTH);

            config->adv_service_uuid |= furi_hal_version_get_hw_color();
        } else if(profile == FuriHalBtProfileHidKeyboard) {
            // Change MAC address for HID profile
            const uint8_t* normal_mac = furi_hal_version_get_ble_mac();
            uint8_t empty_mac[sizeof(config->mac_address)] = FURI_HAL_BT_EMPTY_MAC_ADDR;
            uint8_t default_mac[sizeof(config->mac_address)] = FURI_HAL_BT_DEFAULT_MAC_ADDR;
            if(memcmp(config->mac_address, empty_mac, sizeof(config->mac_address)) == 0 ||
               memcmp(config->mac_address, normal_mac, sizeof(config->mac_address)) == 0 ||
               memcmp(config->mac_address, default_mac, sizeof(config->mac_address)) == 0) {
                memcpy(config->mac_address, normal_mac, sizeof(config->mac_address));
                config->mac_address[2]++;
            }
            // Change name Flipper -> Control
            if(strnlen(config->adv_name, FURI_HAL_VERSION_DEVICE_NAME_LENGTH) < 2 ||
               strnlen(config->adv_name + 1, FURI_HAL_VERSION_DEVICE_NAME_LENGTH - 1) < 1) {
                snprintf(
                    config->adv_name,
                    FURI_HAL_VERSION_DEVICE_NAME_LENGTH,
                    "%cControl %s",
                    AD_TYPE_COMPLETE_LOCAL_NAME,
                    furi_hal_version_get_name_ptr());
            }
        }
        if(!gap_init(config, event_cb, context)) {
            gap_thread_stop();
            FURI_LOG_E(TAG, "Failed to init GAP");
            break;
        }
        // Start selected profile services
        if(furi_hal_bt_is_ble_gatt_gap_supported()) {
            profile_config[profile].start();
        }
        ret = true;
    } while(false);
    current_profile = &profile_config[profile];

    return ret;
}

void furi_hal_bt_reinit() {
    furi_hal_power_insomnia_enter();
    FURI_LOG_I(TAG, "Disconnect and stop advertising");
    furi_hal_bt_stop_advertising();

    FURI_LOG_I(TAG, "Stop current profile services");
    current_profile->stop();

    // Magic happens here
    hci_reset();

    FURI_LOG_I(TAG, "Stop BLE related RTOS threads");
    ble_app_thread_stop();
    gap_thread_stop();

    FURI_LOG_I(TAG, "Reset SHCI");
    furi_check(ble_glue_reinit_c2());

    furi_delay_ms(100);
    ble_glue_thread_stop();

    furi_hal_bus_disable(FuriHalBusHSEM);
    furi_hal_bus_disable(FuriHalBusIPCC);
    furi_hal_bus_disable(FuriHalBusAES2);
    furi_hal_bus_disable(FuriHalBusPKA);
    furi_hal_bus_disable(FuriHalBusCRC);

    FURI_LOG_I(TAG, "Start BT initialization");
    furi_hal_bt_init();

    furi_hal_bt_start_radio_stack();
    furi_hal_power_insomnia_exit();
}

bool furi_hal_bt_change_app(FuriHalBtProfile profile, GapEventCallback event_cb, void* context) {
    furi_assert(event_cb);
    furi_assert(profile < FuriHalBtProfileNumber);
    bool ret = true;

    furi_hal_bt_reinit();

    ret = furi_hal_bt_start_app(profile, event_cb, context);
    if(ret) {
        current_profile = &profile_config[profile];
    }
    return ret;
}

bool furi_hal_bt_is_active() {
    return gap_get_state() > GapStateIdle;
}

bool furi_hal_bt_is_connected() {
    return gap_get_state() == GapStateConnected;
}

void furi_hal_bt_start_advertising() {
    if(gap_get_state() == GapStateIdle) {
        gap_set_discoverable(flippaa_settings.bt_is_discoverable);
        while(gap_get_discoverable() != flippaa_settings.bt_is_discoverable) { // TODO PROBABLY VERY DUMB!!!
            furi_delay_tick(1);
        }
        gap_start_advertising();
    }
}

void furi_hal_bt_stop_advertising() {
    if(furi_hal_bt_is_active()) {
        gap_stop_advertising();
        while(furi_hal_bt_is_active()) {
            furi_delay_tick(1);
        }
    }
}

void furi_hal_bt_set_discoverable(bool discoverable) {
    if(furi_hal_bt_is_active()) {
        FURI_LOG_D(TAG, "Setting discoverable to %d", discoverable);
        // TODO remove start stop hack (and above)
        gap_set_discoverable(discoverable);
    }
    // flippaa_settings.bt_is_discoverable = discoverable;
    // XTREME_SETTINGS_SAVE();
}

bool furi_hal_bt_get_discoverable() {
    bool discoverable = false;
    if(furi_hal_bt_is_active()) {
        discoverable = gap_get_discoverable();
        FURI_LOG_D(TAG, "Got BT discoverable: %d", discoverable);
    }
    return discoverable;
}

void furi_hal_bt_update_battery_level(uint8_t battery_level) {
    if(battery_svc_is_started()) {
        battery_svc_update_level(battery_level);
    }
}

void furi_hal_bt_update_power_state() {
    if(battery_svc_is_started()) {
        battery_svc_update_power_state();
    }
}

void furi_hal_bt_get_key_storage_buff(uint8_t** key_buff_addr, uint16_t* key_buff_size) {
    ble_app_get_key_storage_buff(key_buff_addr, key_buff_size);
}

void furi_hal_bt_set_key_storage_change_callback(
    BleGlueKeyStorageChangedCallback callback,
    void* context) {
    furi_assert(callback);
    ble_glue_set_key_storage_changed_callback(callback, context);
}

void furi_hal_bt_nvm_sram_sem_acquire() {
    while(LL_HSEM_1StepLock(HSEM, CFG_HW_BLE_NVM_SRAM_SEMID)) {
        furi_thread_yield();
    }
}

void furi_hal_bt_nvm_sram_sem_release() {
    LL_HSEM_ReleaseLock(HSEM, CFG_HW_BLE_NVM_SRAM_SEMID, 0);
}

bool furi_hal_bt_clear_white_list() {
    furi_hal_bt_nvm_sram_sem_acquire();
    tBleStatus status = aci_gap_clear_security_db();
    if(status) {
        FURI_LOG_E(TAG, "Clear while list failed with status %d", status);
    }
    furi_hal_bt_nvm_sram_sem_release();
    return status != BLE_STATUS_SUCCESS;
}

void furi_hal_bt_dump_state(FuriString* buffer) {
    if(furi_hal_bt_is_alive()) {
        uint8_t HCI_Version;
        uint16_t HCI_Revision;
        uint8_t LMP_PAL_Version;
        uint16_t Manufacturer_Name;
        uint16_t LMP_PAL_Subversion;

        tBleStatus ret = hci_read_local_version_information(
            &HCI_Version, &HCI_Revision, &LMP_PAL_Version, &Manufacturer_Name, &LMP_PAL_Subversion);

        furi_string_cat_printf(
            buffer,
            "Ret: %d, HCI_Version: %d, HCI_Revision: %d, LMP_PAL_Version: %d, Manufacturer_Name: %d, LMP_PAL_Subversion: %d",
            ret,
            HCI_Version,
            HCI_Revision,
            LMP_PAL_Version,
            Manufacturer_Name,
            LMP_PAL_Subversion);
    } else {
        furi_string_cat_printf(buffer, "BLE not ready");
    }
}

bool furi_hal_bt_is_alive() {
    return ble_glue_is_alive();
}

void furi_hal_bt_start_tone_tx(uint8_t channel, uint8_t power) {
    aci_hal_set_tx_power_level(0, power);
    aci_hal_tone_start(channel, 0);
}

void furi_hal_bt_stop_tone_tx() {
    aci_hal_tone_stop();
}

void furi_hal_bt_start_packet_tx(uint8_t channel, uint8_t pattern, uint8_t datarate) {
    hci_le_enhanced_transmitter_test(channel, 0x25, pattern, datarate);
}

void furi_hal_bt_start_packet_rx(uint8_t channel, uint8_t datarate) {
    hci_le_enhanced_receiver_test(channel, datarate, 0);
}

uint16_t furi_hal_bt_stop_packet_test() {
    uint16_t num_of_packets = 0;
    hci_le_test_end(&num_of_packets);
    return num_of_packets;
}

void furi_hal_bt_start_rx(uint8_t channel) {
    aci_hal_rx_start(channel);
}

float furi_hal_bt_get_rssi() {
    float val;
    uint8_t rssi_raw[3];

    if(aci_hal_read_raw_rssi(rssi_raw) != BLE_STATUS_SUCCESS) {
        return 0.0f;
    }

    // Some ST magic with rssi
    uint8_t agc = rssi_raw[2] & 0xFF;
    int rssi = (((int)rssi_raw[1] << 8) & 0xFF00) + (rssi_raw[0] & 0xFF);
    if(rssi == 0 || agc > 11) {
        val = -127.0;
    } else {
        val = agc * 6.0f - 127.0f;
        while(rssi > 30) {
            val += 6.0;
            rssi >>= 1;
        }
        val += (float)((417 * rssi + 18080) >> 10);
    }
    return val;
}

/** fill the RSSI of the remote host of the bt connection and returns the last 
 *  time the RSSI was updated
 * 
*/
uint32_t furi_hal_bt_get_conn_rssi(uint8_t* rssi) {
    int8_t ret_rssi = 0;
    uint32_t since = gap_get_remote_conn_rssi(&ret_rssi);

    if(ret_rssi == 127 || since == 0) return 0;

    *rssi = (uint8_t)abs(ret_rssi);

    return since;
}

bool furi_hal_bt_custom_adv_set(const uint8_t* adv_data, size_t adv_len) {
    tBleStatus status = aci_gap_additional_beacon_set_data(adv_len, adv_data);
    if(status) {
        FURI_LOG_E(TAG, "custom_adv_set failed %d", status);
        return false;
    } else {
        FURI_LOG_D(TAG, "custom_adv_set success");
        return true;
    }
}

bool furi_hal_bt_custom_adv_start(
    uint16_t min_interval,
    uint16_t max_interval,
    uint8_t mac_type,
    const uint8_t mac_addr[GAP_MAC_ADDR_SIZE],
    uint8_t power_amp_level) {
    tBleStatus status = aci_gap_additional_beacon_start(
        min_interval / 0.625, // Millis to gap time
        max_interval / 0.625, // Millis to gap time
        0b00000111, // All 3 channels
        mac_type,
        mac_addr,
        power_amp_level);
    if(status) {
        FURI_LOG_E(TAG, "custom_adv_start failed %d", status);
        return false;
    } else {
        FURI_LOG_D(TAG, "custom_adv_start success");
        return true;
    }
}

bool furi_hal_bt_custom_adv_stop() {
    tBleStatus status = aci_gap_additional_beacon_stop();
    if(status) {
        FURI_LOG_E(TAG, "custom_adv_stop failed %d", status);
        return false;
    } else {
        FURI_LOG_D(TAG, "custom_adv_stop success");
        return true;
    }
}

void furi_hal_bt_reverse_mac_addr(uint8_t mac_addr[GAP_MAC_ADDR_SIZE]) {
    uint8_t tmp;
    for(size_t i = 0; i < GAP_MAC_ADDR_SIZE / 2; i++) {
        tmp = mac_addr[i];
        mac_addr[i] = mac_addr[GAP_MAC_ADDR_SIZE - 1 - i];
        mac_addr[GAP_MAC_ADDR_SIZE - 1 - i] = tmp;
    }
}

void furi_hal_bt_set_profile_adv_name(
    FuriHalBtProfile profile,
    const char name[FURI_HAL_BT_ADV_NAME_LENGTH]) {
    furi_assert(profile < FuriHalBtProfileNumber);
    furi_assert(name);

    if(strlen(name) == 0) {
        memset(&(profile_config[profile].config.adv_name[1]), 0, FURI_HAL_BT_ADV_NAME_LENGTH);
    } else {
        profile_config[profile].config.adv_name[0] = AD_TYPE_COMPLETE_LOCAL_NAME;
        strlcpy(&(profile_config[profile].config.adv_name[1]), name, FURI_HAL_BT_ADV_NAME_LENGTH);
    }
}

const char* furi_hal_bt_get_profile_adv_name(FuriHalBtProfile profile) {
    furi_assert(profile < FuriHalBtProfileNumber);
    return &(profile_config[profile].config.adv_name[1]);
}

void furi_hal_bt_set_profile_mac_addr(
    FuriHalBtProfile profile,
    const uint8_t mac_addr[GAP_MAC_ADDR_SIZE]) {
    furi_assert(profile < FuriHalBtProfileNumber);
    furi_assert(mac_addr);

    memcpy(profile_config[profile].config.mac_address, mac_addr, GAP_MAC_ADDR_SIZE);
}

const uint8_t* furi_hal_bt_get_profile_mac_addr(FuriHalBtProfile profile) {
    furi_assert(profile < FuriHalBtProfileNumber);
    return profile_config[profile].config.mac_address;
}

void furi_hal_bt_set_profile_pairing_method(FuriHalBtProfile profile, GapPairing pairing_method) {
    furi_assert(profile < FuriHalBtProfileNumber);
    profile_config[profile].config.pairing_method = pairing_method;
}

GapPairing furi_hal_bt_get_profile_pairing_method(FuriHalBtProfile profile) {
    furi_assert(profile < FuriHalBtProfileNumber);
    return profile_config[profile].config.pairing_method;
}

uint32_t furi_hal_bt_get_transmitted_packets() {
    uint32_t packets = 0;
    aci_hal_le_tx_test_packet_number(&packets);
    return packets;
}

void furi_hal_bt_stop_rx() {
    aci_hal_rx_stop();
}

bool furi_hal_bt_ensure_c2_mode(BleGlueC2Mode mode) {
    BleGlueCommandResult fw_start_res = ble_glue_force_c2_mode(mode);
    if(fw_start_res == BleGlueCommandResultOK) {
        return true;
    } else if(fw_start_res == BleGlueCommandResultRestartPending) {
        // Do nothing and wait for system reset
        furi_delay_ms(C2_MODE_SWITCH_TIMEOUT);
        furi_crash("Waiting for FUS->radio stack transition");
        return true;
    }

    FURI_LOG_E(TAG, "Failed to switch C2 mode: %d", fw_start_res);
    return false;
}

const FuriHalBtHardfaultInfo* furi_hal_bt_get_hardfault_info() {
    /* AN5289, 4.8.2 */
    const FuriHalBtHardfaultInfo* info = (FuriHalBtHardfaultInfo*)(SRAM2A_BASE);
    if(info->magic != FURI_HAL_BT_HARDFAULT_INFO_MAGIC) {
        return NULL;
    }
    return info;
}
