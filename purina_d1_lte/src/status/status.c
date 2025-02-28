#include "status.h"
#include <stdio.h>
#include <stdbool.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <nrf_modem_at.h>

#include <zephyr/zbus/zbus.h>
#include "zbus_msgs.h"
#include "network.h"
#include <modem/modem_info.h>
#include "d1_gps.h"


#define UPDATE_RSSI_TIMER 30

LOG_MODULE_REGISTER(status, CONFIG_PURINA_D1_LTE_LOG_LEVEL);

ZBUS_SUBSCRIBER_DEFINE(status_client, 10);

void modem_info_work_handler(struct k_work *work);
K_WORK_DEFINE(my_modem_info_work, modem_info_work_handler);

struct k_work_q status_work_q;
K_THREAD_STACK_DEFINE(status_q_stack_area, 2048);

// UPDATE as new values are added for proper initialization
static uint32_t status_bits = 0;
static modem_status_t current_status;
static modem_info_t modem_info;
static char modem_fw_ver[32];
bool modem_info_initialized = false;
static bool time_set_first_time = false;
static bool status_initialized = false;

cell_info_t current_cell_info;
cell_info_t current_neighbor_cell_info[32];
uint8_t current_neighbor_cell_info_count = 0;
struct modem_param_info my_modem_param_info;

K_FIFO_DEFINE(outgoing_status_fifo);


void printCellInfo(cell_info_t* info, char* prefix) {
    LOG_DBG("Cell Info: %s", prefix);
    LOG_DBG("  MCC: %d", info->mcc);
    LOG_DBG("  MNC: %d", info->mnc);
    LOG_DBG("  TAC: %d", info->tracking_area);
    LOG_DBG("  RSSI: %d", info->rssi);
    LOG_DBG("  Band: %d", info->lte_band);
    LOG_DBG("  Mode: %d", info->lte_nbiot_mode);
    LOG_DBG("  Cell ID: %s", info->cellID);
    LOG_DBG("  IP: %d.%d.%d.%d", info->ip[0], info->ip[1], info->ip[2], info->ip[3]);

}


cell_info_t* getCellInfo() {
    return &current_cell_info;
}


void getModemRSSI(struct k_work *work) {
    char* atCommand = "AT+CESQ\0";
    int err;

    if (!config_get_lte_connected()) {
        current_status.rssi = 0;
        return;
    }

    char response[64];
    err = nrf_modem_at_cmd(response, sizeof(response), "%s", atCommand);
    if (err < 0) {
            LOG_DBG("error with nrf_modem_at_cmd");
            strcpy(response, "ERROR\0");
    }
    else {
            int dummy[5];
            int rssi;
            sscanf(response, "+CESQ: %d,%d,%d,%d,%d,%d", &(dummy[0]), &(dummy[1]), &(dummy[2]), &(dummy[3]), &(dummy[4]), &rssi);
            LOG_DBG("rssi = %d", -140 + rssi);
            current_status.rssi = -140 + rssi;
    }
}
K_WORK_DEFINE(my_get_rssi_work, getModemRSSI);
void my_get_rssi_work_timer_handler(struct k_timer *dummy)
{
    k_work_submit_to_queue(&status_work_q, &my_get_rssi_work);
}
K_TIMER_DEFINE(my_get_rssi_timer, my_get_rssi_work_timer_handler, NULL);


modem_status_t* getSystemStatus(void) {
    return &current_status;
}

modem_info_t getModemInfo(void) {
    return modem_info;
}


// Function to get the value of a specific bit in the integer
static bool getBit(int pos) {
    return (current_status.status_flags >> pos) & 1;
}

void copySystemStatus(modem_status_t *dst_status, modem_status_t *src_status) {
    dst_status->status_flags = src_status->status_flags;
    dst_status->rssi = src_status->rssi;
    dst_status->fota_state = src_status->fota_state;
    dst_status->fota_percentage = src_status->fota_percentage;
    // int i;
    // unsigned char* charPtr = (unsigned char*)&(src_status->uptime);
    // LOG_ERR("src_status->uptime: ");
    // for(i=0;i<sizeof(uint64_t);i++) LOG_ERR("%02x ",charPtr[i]);

    uint8_t* s = (uint8_t*)&(src_status->uptime);
    uint8_t* t = (uint8_t*)&(dst_status->uptime);
    t[0] = s[0];
    t[1] = s[1];
    t[2] = s[2];
    t[3] = s[3];
    t[4] = s[4];
    t[5] = s[5];
    t[6] = s[6];
    t[7] = s[7];
    // charPtr = (unsigned char*)&(dst_status->uptime);
    // LOG_ERR("dst_status->uptime: ");
    // for(i=0;i<sizeof(uint64_t);i++) LOG_ERR("%02x ",charPtr[i]);
    memset(dst_status->timestamp, 0, 32);
    strncpy(dst_status->timestamp, src_status->timestamp, 32);
}


void copyStaticSystemStatus(modem_status_t *status) {
    if (status == NULL) {
        return;
    }
    if (status_initialized == false) {
        status_initialized = true;
        status->status_flags = 0;
        status->rssi = 0;
        status->fota_state = 0;
        status->fota_percentage = 0;
        status->uptime = 1;
        memset(status->timestamp, 0, 32);
    }
    copySystemStatus(status, &current_status);
}

modem_status_t* getStatusFromFifo() {
    if (k_fifo_is_empty(&outgoing_status_fifo)) {
		return NULL;
	}
    modem_status_t* status = k_fifo_get(&outgoing_status_fifo, K_NO_WAIT);
    return status;
}

void send_zbus_status_event() {
    modem_status_t* status = k_malloc(sizeof(modem_status_t));
    if(status == NULL) {
        LOG_ERR("malloc failed");
        return;
    }
    current_status.uptime = k_uptime_get();
    copyStaticSystemStatus(status);
    k_fifo_alloc_put(&outgoing_status_fifo, status);
    uint8_t dummy = 0;
    int err = zbus_chan_pub(&STATUS_UPDATE, &dummy, K_SECONDS(1));
    if (err) {
        LOG_ERR("zbus_chan_pub, error:%d", err);
        //SEND_FATAL_ERROR();
    }
}

// Function to set a specific bit in the integer
static void setBit( bool state, int pos) {
    int pos2 = -1;
    if (getBit(pos) == state) {
        return;  // unchanged
    }

    // special cases:
    if ((pos == STATUS_LTE_CONNECTED) && (state == true)) {  // LTE connects working == true forever
        pos2 = STATUS_LTE_WORKING;
    }

    if ((pos == STATUS_MQTT_CONNECTED) && (state == true)) {   // MQTT connects working == true forever
        pos2 = STATUS_MQTT_WORKING;
    }

    if (pos2 != -1) {
        current_status.status_flags |= (1 << pos2);
    }

    if (state) {
        current_status.status_flags |= (1 << pos);
    } else {
        current_status.status_flags &= ~(1 << pos);
    }

    send_zbus_status_event();
}


char* getModemFWVersion() {
    return modem_fw_ver;
}

char* getStatusTime() {
    if (!config_get_lte_working()) {
        return NULL;
    }
    if (modem_info_initialized == false) {
        return NULL;
    }
    char sbuf[64];
    int ret = modem_info_string_get(MODEM_INFO_DATE_TIME, sbuf, sizeof(sbuf));
    if (ret > 64) {
        LOG_ERR("modem_info_string_get, error: %d", ret);
        memset(current_status.timestamp, 0, sizeof(current_status.timestamp));
        return NULL;
    }
    else {
        //LOG_DBG("current time and date: %s", sbuf);
        strncpy(current_status.timestamp, sbuf, sizeof(current_status.timestamp) );
        if (time_set_first_time == false) {
            time_set_first_time = true;
            send_zbus_status_event();
        }
    }
    return current_status.timestamp;
}

// Function to set bit0 in the status
void config_set_lte_connected(bool state) {
    setBit( state, STATUS_LTE_CONNECTED);
    // reset/set/clear anything that will have changed based on this
    if (state) {
        config_set_fota_in_progress(false);
        setFOTAState(0, 0);
        config_set_lte_working(true);
    } else {
        config_set_fota_in_progress(false);
        setFOTAState(0, 0);
        //  config_set_mqtt_connected(false);  // let this be handled by the mqtt status
    }
}

void setFOTAState(uint32_t state, uint8_t percentage) {
    current_status.fota_state = state;
    current_status.fota_percentage = percentage;
    send_zbus_status_event();
}

// Function to get the value of bit0 in the status
bool config_get_lte_connected() {
    return getBit(STATUS_LTE_CONNECTED);
}

// Function to set bit1 in the status
void config_set_lte_enabled(bool state) {
    setBit( state, STATUS_LTE_ENABLED);
}

// Function to get the value of bit1 in the status
bool config_get_lte_enabled() {
    return getBit(STATUS_LTE_ENABLED);
}

// Function to set bit2 in the status
void config_set_lte_working(bool state) {
    // handled in special case in setBit
}

// Function to get the value of bit2 in the status
bool config_get_lte_working() {
    return getBit(STATUS_LTE_WORKING);
}

// Function to set bit3 in the status
void config_set_fota_in_progress(bool state) {
    setBit( state, STATUS_FOTA_IN_PROGRESS);
}

// Function to get the value of bit3 in the status
bool config_get_fota_in_progress() {
    return getBit(STATUS_FOTA_IN_PROGRESS);
}

// Function to set bit4 in the status
void config_set_mqtt_connected(bool state) {
    //LOG_INF("SETTING MQTT CONNECTED STATUS BIT FLAG: %d", state);
    if (state == true) {
        LOG_CLOUD_INF(MODEM_ERROR_NONE, "9160 MQTT: connected");
    } else {
        LOG_CLOUD_INF(MODEM_ERROR_NONE, "9160 MQTT: disconnected");
    }
    setBit( state, STATUS_MQTT_CONNECTED);
}

// Function to get the value of bit4 in the status
bool config_get_mqtt_connected() {
    return getBit(STATUS_MQTT_CONNECTED);
}

// Function to set bit5 in the status
void config_set_bit5(bool state) {
    setBit( state, STATUS_GPS_CONNECTED);
}

// Function to get the value of bit5 in the status
bool config_get_bit5() {
    return getBit(STATUS_GPS_CONNECTED);
}

// Function to set bit6 in the status
void config_set_mqtt_initialized(bool state) {
    setBit( state, STATUS_MQTT_INITIALIZED);
}

// Function to get the value of bit6 in the status
bool config_get_mqtt_initialized() {
    return getBit(STATUS_MQTT_INITIALIZED);
}

// Function to set bit7 in the status
void config_set_powered_off(bool state) {
    //current_status.status_flags &= (0 << STATUS_POWERED_ON);
    setBit( !state, STATUS_POWERED_ON);  // special case, dont call setBit as that generates an event and this is called from an handler only.
}

// Function to get the value of bit7 in the status
bool config_get_bit7() {
    return getBit(STATUS_POWERED_ON);
}

// Function to set bit8 in the status
void config_set_mqtt_working(bool state) {
    // handled in special case in setBit
}

// Function to get the value of bit8 in the status
bool config_get_mqtt_working() {
    return getBit(STATUS_MQTT_WORKING);
}

// Function to set bit9 in the status
void config_set_certs_loaded(bool state) {
    setBit( state, STATUS_CERTS_LOADED);
}

// Function to get the value of bit9 in the status
bool config_get_bit9() {
    return getBit(STATUS_CERTS_LOADED);
}

// Function to set bit10 in the status
void config_set_airplane_mode(bool state) {
    setBit( state, STATUS_AIRPLANE_MODE);
}

// Function to get the value of bit7 in the status
bool config_get_airplane_mode() {
    return getBit(STATUS_AIRPLANE_MODE);
}

// Function to set bit11 in the status
void config_set_mqtt_enabled(bool state) {
    LOG_INF("config_set_mqtt_enabled: %d", state);
    setBit( state, STATUS_MQTT_ENABLED);
}

//
bool config_get_mqtt_enabled() {
    return getBit(STATUS_MQTT_ENABLED);
}

void config_set_gpsEnabled(bool state) {
    setBit( state, STATUS_GPS_ENABLED);
}

// Function to get the value of bit12 in the status
bool config_get_bit12() {
    return getBit(STATUS_GPS_ENABLED);
}

// Function to set bit13 in the status
void config_set_cell_data_changed(bool state) {
    setBit( state, STATUS_CELL_DATA_CHANGED);
}

// Function to get the value of bit13 in the status
bool config_get_cell_data_changed() {
    return getBit(STATUS_CELL_DATA_CHANGED);
}

// Function to set bit14 in the status
void config_set_cell_tracking_data_changed(bool state) {
    setBit( state, STATUS_CELL_TRACKING_CHANGED);
}

// Function to get the value of bit14 in the status
bool config_get_cell_tracking_data_changed() {
    return getBit(STATUS_CELL_TRACKING_CHANGED);
}

// Function to set bit15 in the status
void config_set_bit15(bool state) {
    setBit( state, STATUS_BIT15);
}

// Function to get the value of bit15 in the status
bool config_get_bit15() {
    return getBit(STATUS_BIT15);
}



void rssi_change_callback(char rsrp_value)
{
    LOG_DBG("rssi_change_callback: %d dBm", (-140 + rsrp_value));
    current_status.rssi = (-140 + rsrp_value);
    current_cell_info.rssi = (-140 + rsrp_value);
    send_zbus_status_event();
}


uint32_t getStatus() {
    return status_bits;
}

// ((status->status_flags >> STATUS_LTE_CONNECTED) & 1) ? "yes" : "no");

void printStatus(modem_status_t* status) {
    if (status == NULL) {
        LOG_DBG("status_bits: %u", status_bits);
        LOG_DBG("LTE_CONNECTED: %s", (config_get_lte_connected()) ? "yes" : "no");
        LOG_DBG("LTE_ENABLED: %s", (config_get_lte_enabled()) ? "yes" : "no");
        LOG_DBG("LTE_WORKING: %s", (config_get_lte_working()) ? "yes" : "no");
        LOG_DBG("MQTT_CONNECTED: %s", (config_get_mqtt_connected()) ? "yes" : "no");
        LOG_DBG("MQTT_INITIALIZED: %s", (config_get_mqtt_initialized()) ? "yes" : "no");
        LOG_DBG("MQTT_WORKING: %s", (config_get_mqtt_working()) ? "yes" : "no");
        LOG_DBG("MQTT_ENABLED: %s", (config_get_mqtt_enabled()) ? "yes" : "no");
        LOG_DBG("FOTA_IN_PROGRESS: %s", (config_get_fota_in_progress()) ? "yes" : "no");
        LOG_DBG("GPS_CONNECTED: %s", (config_get_bit5()) ? "yes" : "no");
        LOG_DBG("GPS_ENABLED: %s", (config_get_bit12()) ? "yes" : "no");
        LOG_DBG("POWERED_OFF: %s", (config_get_bit7()) ? "yes" : "no");
        LOG_DBG("CERTS_LOADED: %s", (config_get_bit9()) ? "yes" : "no");
        LOG_DBG("AIRPLANE_MODE: %s", (config_get_airplane_mode()) ? "yes" : "no");
        LOG_DBG("CELL_DATA_CHANGED: %s", (config_get_cell_data_changed()) ? "yes" : "no");
        LOG_DBG("CELL_TRACKING_CHANGED: %s", (config_get_cell_tracking_data_changed()) ? "yes" : "no");
        LOG_DBG("BIT15: %s", (config_get_bit15()) ? "yes" : "no");
    } else {
        LOG_DBG("status_bits: %u", status->status_flags);
        LOG_DBG("LTE_CONNECTED: %s", ((status->status_flags >> STATUS_LTE_CONNECTED) & 1) ? "yes" : "no");
        LOG_DBG("LTE_ENABLED: %s", ((status->status_flags >> STATUS_LTE_CONNECTED) & 1) ? "yes" : "no");
        LOG_DBG("LTE_WORKING: %s", ((status->status_flags >> STATUS_LTE_WORKING) & 1) ? "yes" : "no");
        LOG_DBG("MQTT_CONNECTED: %s", ((status->status_flags >> STATUS_MQTT_CONNECTED) & 1) ? "yes" : "no");
        LOG_DBG("MQTT_INITIALIZED: %s", ((status->status_flags >> STATUS_MQTT_INITIALIZED) & 1) ? "yes" : "no");
        LOG_DBG("MQTT_WORKING: %s", ((status->status_flags >> STATUS_MQTT_WORKING) & 1) ? "yes" : "no");
        LOG_DBG("MQTT_ENABLED: %s", ((status->status_flags >> STATUS_MQTT_ENABLED) & 1) ? "yes" : "no");
        LOG_DBG("FOTA_IN_PROGRESS: %s", ((status->status_flags >> STATUS_FOTA_IN_PROGRESS) & 1) ? "yes" : "no");
        LOG_DBG("GPS_CONNECTED: %s", ((status->status_flags >> STATUS_GPS_CONNECTED) & 1) ? "yes" : "no");
        LOG_DBG("GPS_ENABLED: %s", ((status->status_flags >> STATUS_GPS_ENABLED) & 1) ? "yes" : "no");
        LOG_DBG("POWERED_OFF: %s", ((status->status_flags >> STATUS_POWERED_ON) & 1) ? "yes" : "no");
        LOG_DBG("CERTS_LOADED: %s", ((status->status_flags >> STATUS_CERTS_LOADED) & 1) ? "yes" : "no");
        LOG_DBG("AIRPLANE_MODE: %s", ((status->status_flags >> STATUS_AIRPLANE_MODE) & 1) ? "yes" : "no");
        LOG_DBG("BIT13: %s", ((status->status_flags >> STATUS_CELL_DATA_CHANGED) & 1) ? "yes" : "no");
        LOG_DBG("BIT14: %s", ((status->status_flags >> STATUS_CELL_TRACKING_CHANGED) & 1) ? "yes" : "no");
        LOG_DBG("BIT15: %s", ((status->status_flags >> STATUS_BIT15) & 1) ? "yes" : "no");
    }
}

// convert ip address string to array of integers
void ipStringToBytes(const char* str, uint8_t* bytes) {
    int i;
    int len = strlen(str);
    int num = 0;
    int j = 0;
    for (i = 0; i < len; i++) {
        if (str[i] == '.') {
            bytes[j] = num;
            num = 0;
            j++;
        } else {
            num = num * 10 + (str[i] - '0');
        }
    }
    bytes[j] = num;
}

void modem_info_work_handler(struct k_work *work)
{
    if (!config_get_lte_connected()) {
        return;
    }
    int ret;

    if ((ret = modem_info_params_init(&my_modem_param_info)) == 0) {
        LOG_DBG("modem_info_params_init success");
    } else {
        LOG_ERR("modem_info_params_init failed - %d", ret);
        return;
    }

    if ((ret = modem_info_params_get(&my_modem_param_info)) == 0) {
        LOG_DBG("modem_info_params_get success");

        LOG_DBG("====== Network Params Info ======");
        LOG_DBG("Current LTE band: %d", my_modem_param_info.network.current_band.value);
        current_cell_info.lte_band = my_modem_param_info.network.current_band.value;
        LOG_DBG("Supported LTE bands: %s", my_modem_param_info.network.sup_band.value_string);
        LOG_DBG("Tracking area code: %s", my_modem_param_info.network.area_code.value_string);
        current_cell_info.tracking_area = (int)strtol(my_modem_param_info.network.area_code.value_string, NULL, 16);
        LOG_DBG("Current operator name: %s", my_modem_param_info.network.current_operator.value_string);
        memcpy(modem_info.operator, my_modem_param_info.network.current_operator.value_string, strlen(my_modem_param_info.network.current_operator.value_string));
        LOG_DBG("Mobile country code: %d", my_modem_param_info.network.mcc.value);
        current_cell_info.mcc = my_modem_param_info.network.mcc.value;
        LOG_DBG("Mobile network code: %d", my_modem_param_info.network.mnc.value);
        current_cell_info.mnc = my_modem_param_info.network.mnc.value;
        LOG_DBG("Cell ID of the device: %s", my_modem_param_info.network.cellid_hex.value_string);
        memcpy(modem_info.cellID, my_modem_param_info.network.cellid_hex.value_string, strlen(my_modem_param_info.network.cellid_hex.value_string));
        memcpy(current_cell_info.cellID, my_modem_param_info.network.cellid_hex.value_string, strlen(my_modem_param_info.network.cellid_hex.value_string));
        LOG_DBG("IP address of the device: %s", my_modem_param_info.network.ip_address.value_string);
        ipStringToBytes( my_modem_param_info.network.ip_address.value_string, current_cell_info.ip);
        LOG_DBG("Current mode: %s", my_modem_param_info.network.ue_mode.value_string);
        LOG_DBG("LTE-M support mode: %s", my_modem_param_info.network.lte_mode.value_string);
        LOG_DBG("NB-IoT support mode: %s", my_modem_param_info.network.nbiot_mode.value_string);
        LOG_DBG("GPS support mode: %s", my_modem_param_info.network.gps_mode.value_string);
        LOG_DBG("Mobile network time and date: %s", my_modem_param_info.network.date_time.value_string);
        memcpy(current_status.timestamp, my_modem_param_info.network.date_time.value_string, strlen(my_modem_param_info.network.date_time.value_string));
        LOG_DBG("Access point name: %s", my_modem_param_info.network.apn.value_string);
        memcpy(modem_info.ap, my_modem_param_info.network.apn.value_string, strlen(my_modem_param_info.network.apn.value_string));
        LOG_DBG("Received signal strength: %d", my_modem_param_info.network.rsrp.value - 140);
        rssi_change_callback(my_modem_param_info.network.rsrp.value);
        LOG_DBG("Cell ID of the device (in decimal format): %f", my_modem_param_info.network.cellid_dec);

        LOG_DBG("====== Sim Params Info ======");
        LOG_DBG("UICC state: %s", my_modem_param_info.sim.uicc.value_string);
        LOG_DBG("ICCID: %s", my_modem_param_info.sim.iccid.value_string);
        memcpy(modem_info.iccid, my_modem_param_info.sim.iccid.value_string, strlen(my_modem_param_info.sim.iccid.value_string));
        LOG_DBG("IMEI: %s", my_modem_param_info.device.imei.value_string);
        memcpy(modem_info.imei, my_modem_param_info.device.imei.value_string, strlen(my_modem_param_info.device.imei.value_string));
        memcpy(modem_info.subscriber, my_modem_param_info.device.imei.value_string, strlen(my_modem_param_info.device.imei.value_string));

        LOG_DBG("====== Device Params Info ======");
        LOG_DBG("Modem firmware version: %s", my_modem_param_info.device.modem_fw.value_string);
        memcpy(modem_fw_ver, my_modem_param_info.device.modem_fw.value_string, strlen(my_modem_param_info.device.modem_fw.value_string));
        LOG_DBG("Battery voltage: %d", my_modem_param_info.device.battery.value);
        LOG_DBG("Modem serial number: %s", my_modem_param_info.device.imei.value_string);
        LOG_DBG("Board version: %s", my_modem_param_info.device.board);
        LOG_DBG("Application version: %s", my_modem_param_info.device.app_version);
        LOG_DBG("Application name: %s", my_modem_param_info.device.app_name);
    } else {
        LOG_ERR("modem_info_params_get failed - %d", ret);
        return;
    }


    int temp;
    if (modem_info_get_temperature(&temp) == 0) {
        current_status.temperature = temp;
        LOG_DBG("Temperature: %d", temp);
    }
    else {
        current_status.temperature = 0;
    }

    // int tx, rx;
    // if (modem_info_get_connectivity_stats(&tx, &rx) == 0) {
    //     LOG_DBG("tx_bytes: %d, rx_bytes: %d", tx, rx);
    // }

    char hw_version[32];
    if (modem_info_get_hw_version(hw_version, sizeof(hw_version))) {
        LOG_DBG("Hardware version: %s", hw_version);
    }

    LOG_DBG("===============================");

    current_cell_info.lte_nbiot_mode = network_get_lte_mode();

    config_set_cell_data_changed(true);

    cell_info_t c_info = *getCellInfo();

    int err = zbus_chan_pub(&CELL_INFO_CHANNEL, &c_info, K_SECONDS(1));
    if (err) {
        LOG_ERR("zbus_chan_pub, error:%d", err);
        //SEND_FATAL_ERROR();
    }
}


void my_modem_info_work_timer_handler(struct k_timer *dummy)
{
    k_work_submit_to_queue(&status_work_q, &my_modem_info_work);
}
K_TIMER_DEFINE(my_modem_info_timer, my_modem_info_work_timer_handler, NULL);



void modem_clock_work_handler(struct k_work *work){
    getStatusTime();
    uint64_t uptime = k_uptime_get();
    current_status.uptime = uptime;
}
K_WORK_DEFINE(my_modem_clock_work, modem_clock_work_handler);
void my_modem_clock_work_timer_handler(struct k_timer *dummy)
{
    k_work_submit_to_queue(&status_work_q, &my_modem_clock_work);
}
K_TIMER_DEFINE(my_modem_clock_timer, my_modem_clock_work_timer_handler, NULL);




void check_for_certs() {
    char response[1024];
    int at_err = nrf_modem_at_cmd(response, sizeof(response), "%s", "AT\%CMNG=1");
    //LOG_WRN("recieved atCommand response = %s", response);
    if (at_err < 0) {
            LOG_DBG("error determining if there are certs loaded");
    }
    else {
            // parse response for cert in 123 slots
            char* cert0 = strstr(response, "CMNG: 123,0");
            char* cert1 = strstr(response, "CMNG: 123,1");
            char* cert2 = strstr(response, "CMNG: 123,2");
            if (cert0 != NULL && cert1 != NULL && cert2 != NULL) {
                config_set_certs_loaded(true);
            }
            else {
                config_set_certs_loaded(false);
            }
    }
}

// this gets called by SPI every time a status message gets sent to the 5340,
// any status flags that were set as a one-time status status should be cleared here
void clearOneTimeStatuses() {
    if (current_status.fota_state == 99) {
        current_status.fota_state = 0;
    }
}

static void status_init(void) {
    current_status.status_flags = 0;
    memset(current_status.timestamp, 0, sizeof(current_status.timestamp));
    current_status.rssi = 0;

    memset(modem_info.ap, 0, sizeof(modem_info.ap));
    memset(modem_info.cellID, 0, sizeof(modem_info.cellID));
    memset(modem_info.iccid, 0, sizeof(modem_info.iccid));
    memset(modem_info.imei, 0, sizeof(modem_info.imei));
    memset(modem_info.operator, 0, sizeof(modem_info.operator));
    memset(modem_info.subscriber, 0, sizeof(modem_info.subscriber));
    memset(modem_fw_ver, 0, sizeof(modem_fw_ver));

    k_work_queue_init(&status_work_q);
    struct k_work_queue_config status_work_q_cfg = {
        .name = "status_work_q",
        .no_yield = 0,
    };
    k_work_queue_start(&status_work_q, status_q_stack_area,
                   K_THREAD_STACK_SIZEOF(status_q_stack_area), 3,
                   &status_work_q_cfg);

    current_status.fota_state = 0;
    current_status.fota_percentage = 0;
    current_status.uptime = 1;  // it hates 0 for reasons I stopped caring about
    status_initialized = true;

    const struct zbus_channel *chan;

    int err;

    LOG_DBG("Status Initialized");

    send_zbus_status_event();

    while (true) {
        if (zbus_sub_wait(&status_client, &chan, K_FOREVER) == 0) {
            if (&NETWORK_CHAN == chan) {
                enum network_status net_status;
                err = zbus_chan_read(&NETWORK_CHAN, &net_status, K_MSEC(1));
                if (err) {
                    LOG_ERR("zbus_chan_read, error: %d", err);
                    return;
                }
                switch(net_status) {
                    case NETWORK_CONNECTED:
                        LOG_DBG("NETWORK_CONNECTED");
                        config_set_lte_connected(true);
                        config_set_lte_working(true);
                        k_timer_start(&my_modem_info_timer, K_MSEC(5), K_NO_WAIT);
                        break;
                    case NETWORK_DISCONNECTED:
                        LOG_DBG("NETWORK_DISCONNECTED");
                        config_set_lte_connected(false);
                        current_status.rssi = 0;
                        break;
                    case NETWORK_INITIALIZING:
                        LOG_DBG("NETWORK_INITIALIZING");
                        if (modem_info_initialized == false) {
                        int err = modem_info_init();
                        if (err) {
                            LOG_ERR("Failed to initialize modem info: %d", err);
                        }
                        modem_info_rsrp_register(rssi_change_callback);
                        //modem_info_connectivity_stats_init();
                        modem_info_initialized = true;
                        }
                        check_for_certs();
                        config_set_powered_off(false);
                        k_timer_start(&my_modem_info_timer, K_MSEC(10), K_NO_WAIT);
                        k_timer_start(&my_modem_clock_timer, K_MSEC(1),  K_MSEC(1000));
                        break;
                    case NETWORK_AIRPLANE_MODE_ON:
                        LOG_DBG("NETWORK_AIRPLANE_MODE_ON");
                        config_set_airplane_mode(true);
                        break;
                    case NETWORK_AIRPLANE_MODE_OFF:
                        LOG_DBG("NETWORK_AIRPLANE_MODE_OFF");
                        config_set_airplane_mode(false);
                        break;
                    case NETWORK_CELL_CHANGED:
                        LOG_DBG("NETWORK_CELL_CHANGED");
                        k_timer_start(&my_modem_info_timer, K_MSEC(10), K_NO_WAIT);
                        break;
                    case NETWORK_CELL_NEIGHBORS_CHANGED:
                        LOG_DBG("NETWORK_CELL_NEIGHBORS_CHANGED");
                        break;
                    default:
                        LOG_ERR("Unknown network status: %d", net_status);
                        break;
                }
            }
        }
    }
}
const k_tid_t status_task_id;
K_THREAD_DEFINE(status_task_id,
		2048,
		status_init, NULL, NULL, NULL, 3, 0, 0);
