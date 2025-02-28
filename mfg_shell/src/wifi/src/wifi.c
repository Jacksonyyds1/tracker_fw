#include "wifi.h"
#include <zephyr/logging/log.h>
#include "d1_json.h"

#if (CONFIG_USE_UART_TO_DA16200)
#include "wifi_uart.h"
#else
#include "wifi_spi.h"
#endif

#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include "pmic.h"

LOG_MODULE_REGISTER(wifi, LOG_LEVEL_DBG);

K_THREAD_STACK_DEFINE(wifi_stack, 2048);
static struct k_work_q wifi_work_q;
static struct k_work wifi_work;
static void wifi_work_fn();

const struct device *gpio_p0 = NULL;
const struct device *gpio_p1 = NULL;

void wifi_timer_handler(struct k_timer *dummy)
{
	k_work_submit_to_queue(&wifi_work_q, &wifi_work);
}

K_TIMER_DEFINE(wifi_timer, wifi_timer_handler, NULL);

void wifi_insert_certs();

int wifi_init()
{
	int ret = 0;

	 wifi_1v8_on();

	gpio_p1 = device_get_binding("gpio@842800");
	if (gpio_p1 == NULL) {
		LOG_ERR("Error: didn't find %s device", "GPIO_1");
		return -1;
	} else {
		int ret = gpio_pin_configure(gpio_p1, 4, GPIO_OUTPUT_ACTIVE);
		if (ret != 0) {
			LOG_ERR("Error %d: failed to configure %s pin %d", ret, "GPIO_1", 4);
		} else {
			LOG_DBG("turned on the WIFI 3v3?_enable");
		}
		ret = gpio_pin_configure(gpio_p1, 14, GPIO_OUTPUT_ACTIVE);
		if (ret != 0) {
			LOG_ERR("Error %d: failed to configure %s pin %d", ret, "GPIO_1", 14);
		} else {
			LOG_DBG("configured WIFI level shifter");
		}
		ret = gpio_pin_set(gpio_p1, 14, 1);
		if (ret != 0) {
			LOG_ERR("Error %d: failed to turn on level shifter, %s pin %d", ret,
				"GPIO_1", 14);
		} else {
			LOG_DBG(" WIFI level shifter turned on");
		}
		ret = gpio_pin_configure(gpio_p1, 8, GPIO_OUTPUT_ACTIVE);
		if (ret != 0) {
			LOG_ERR("Error %d: failed to configure %s pin %d", ret, "GPIO_1", 8);
		} else {
			LOG_DBG("configured WIFI wakeup");
		}
#if (CONFIG_USE_UART_TO_DA16200)
#else
		// set up the gpio that lets the DA tell us when data is ready on SPI
		ret = gpio_pin_configure(gpio_p1, 7, GPIO_INPUT);
		if (ret != 0) {
			LOG_ERR("Error %d: failed to configure %s pin %d", ret, "GPIO_1", 7);
		} else {
			LOG_DBG("configured DA data ready gpio");
		}
#endif
	}

	gpio_p0 = device_get_binding("gpio@842500");
	if (gpio_p0 == NULL) {
		LOG_ERR("Error: didn't find %s device", "GPIO_0");
		return -1;
	} else {
		int ret = gpio_pin_configure(gpio_p0, 28, GPIO_OUTPUT_ACTIVE);
		if (ret != 0) {
			LOG_ERR("Error %d: failed to configure %s pin %d", ret, "GPIO_0", 28);
		} else {
			LOG_DBG("turned on the WIFI power_key");
		}
	}

#if (CONFIG_USE_UART_TO_DA16200)
	LOG_DBG("Initializing UART to DA16200");
	ret = wifi_uart_init();
#else
	LOG_DBG("Initializing SPI to DA16200");
	ret = wifi_spi_init();
#endif

	// Start a work thread to send SSID and other data to the cloud every 5 minutes
	k_work_queue_start(&wifi_work_q, wifi_stack, K_THREAD_STACK_SIZEOF(wifi_stack),
			   CONFIG_SYSTEM_WORKQUEUE_PRIORITY, NULL);
	k_work_init(&wifi_work, wifi_work_fn);
	// use
	//  k_work_submit_to_queue(&wifi_work_q, &wifi_work);
	// to cause data to be sent to cloud

#if CONFIG_CULVERT_LOCATION_DEMO_MODE
	k_timer_start(&wifi_timer, K_SECONDS(WIFI_SEND_DATA_PERIOD_SECONDS),
		      K_SECONDS(WIFI_SEND_DATA_PERIOD_SECONDS));
#endif
	LOG_DBG("Wifi Enabled.");
	return ret;
}

void wifi_do_work()
{
	k_work_submit_to_queue(&wifi_work_q, &wifi_work);
}

int freq_to_channel(int freq)
{
	int freq_conv[21][3] = {// ch, start_freq, end_freq
				{1, 2401, 2423},   {2, 2406, 2428},   {3, 2411, 2433},
				{4, 2416, 2438},   {5, 2421, 2443},   {6, 2426, 2448},
				{7, 2431, 2453},   {8, 2436, 2458},   {9, 2441, 2463},
				{10, 2446, 2468},  {11, 2451, 2473},  {12, 2456, 2478},
				{13, 2461, 2483},  {14, 2473, 2495},  {184, 4910, 4930},
				{188, 4930, 4950}, {192, 4950, 4970}, {196, 4970, 4990},
				{8, 5030, 5050},   {12, 5050, 5070},  {16, 5070, 5090}};
	for (int i = 0; i < 21; i++) {
		if (freq >= freq_conv[i][1] && freq <= freq_conv[i][2]) {
			return freq_conv[i][0];
		}
	}
	return 0;
}

/////////////////////////////////////////////////////////
// get_ssid_list()
//
// Get an list of SSIDs seen by the DA
//
// @param buf - pointer to buffer to store the version
// @param len - length of the buffer
static wifi_arr_t g_last_ssid_list;
wifi_arr_t *get_ssid_list(k_timeout_t timeout)
{
	int ret = 0;
	int rssi;
	int freq;
	char errstr[50];
	char macstr[20];
	wifi_msg_t msg;

	wifi_flush_msgs();
	g_last_ssid_list.count = 0;
	k_timepoint_t timepoint = sys_timepoint_calc(timeout);

	ret = wifi_send_timeout("AT+WFSCAN", timeout);
	if (ret != 0) {
		LOG_ERR("Failed to send at+wfscan; ret:%d", ret);
		return &g_last_ssid_list;
	}
	while (1) {
		timeout = sys_timepoint_timeout(timepoint);
		ret = wifi_recv(&msg, timeout);
		if (ret < 0) {
			LOG_ERR("timeout while receiving ssids: %d", ret);
			return &g_last_ssid_list;
		}
		if (strstr(msg.data, "\r\nERROR") != NULL) {
			LOG_ERR("Error while receiving ssids");
			wifi_msg_free(&msg);
			return &g_last_ssid_list;
		}
		if (strstr(msg.data, "\r\nOK\r\n") != NULL) {
			wifi_msg_free(&msg);
			return &g_last_ssid_list;
		}
		char *sub;
		if ((sub = strstr(msg.data, "+WFSCAN:")) != NULL) {
			sub += 8;
			wifi_obj_t *entry = &g_last_ssid_list.wifi[g_last_ssid_list.count];

			while (strlen(sub) > 10) {
				char *nsub = strstr(sub, "\n");
				if (nsub != NULL) {
					*nsub = 0;
				}
				if (sscanf(sub, "%s\t%d\t%d\t%s\t%s", macstr, &freq, &rssi, errstr,
					   entry->ssid) == 5) {
					entry->rssi = rssi;
					entry->channel = freq_to_channel(freq);
					g_last_ssid_list.count++;
					entry = &g_last_ssid_list.wifi[g_last_ssid_list.count];
				} else if (sscanf(sub, "%s\t%d\t%d\t%s\t", macstr, &freq, &rssi,
						  errstr) == 4) {
					entry->ssid[0] = 0;
					entry->rssi = rssi;
					entry->channel = freq_to_channel(freq);
					g_last_ssid_list.count++;
					entry = &g_last_ssid_list.wifi[g_last_ssid_list.count];
				} else {
					LOG_ERR("Failed to parse:");
					LOG_HEXDUMP_DBG(sub, strlen(sub), "data");
				}
				if (nsub == NULL) {
					break;
				}
				sub = nsub + 1;
			}
			wifi_msg_free(&msg);
		}
	}

	return &g_last_ssid_list;
}

///////////////////////////////////////////////////////////////////////
// wfif_send_ok_err_atcmd()
//  Send a command to the DA and wait for a OK or ERROR response.
//
//  @param cmd - the command to send
//  @param timeout - timeout for the write
//
//  @return - true if OK was received
static char errstr[50];
bool wifi_send_ok_err_atcmd(char *cmd, k_timeout_t timeout)
{
	int ret;
	wifi_wait_array_t wait_msgs;

	k_timepoint_t timepoint = sys_timepoint_calc(timeout);
	if (wifi_send_timeout(cmd, timeout) != 0) {
		LOG_ERR("Timed out sending cmd"); // %10s", cmd);
		return false;
	}

	timeout = sys_timepoint_timeout(timepoint);
	wait_msgs.num_msgs = 0; // Initialize the structure
	wifi_add_wait_msg(&wait_msgs, "\r\nOK\r\n", true, 0);
	wifi_add_wait_msg(&wait_msgs, "\r\nERROR%50s\r\n", true, 1, errstr);
	ret = wifi_wait_for(&wait_msgs, timeout);
	if (ret == 0) {
		return true;
	}
	if (ret == 1) {
		LOG_ERR("Error received on cmd"); // %10s: %10s", cmd, errstr);
	} else {
		LOG_ERR("Command timed out");
	}
	return false;
}

////////////////////////////////////////////////////////////
// wifi_wake_DA()
//
// try to wake up the DA by pulsing the WAKEUP_RTC line
// The DA wakes on the falling edge of WAKEUP_RTC so if
// it is already low, we need to pulse it high first.
//
// @return - none
void wifi_wake_DA()
{
	int curr = gpio_pin_get_raw(gpio_p1, 8);
	if (curr == 0) {
		gpio_pin_set(gpio_p1, 8, 1); // Pulse high
		k_sleep(K_MSEC(100));
	}
	k_sleep(K_MSEC(100));
	gpio_pin_set(gpio_p1, 8, 0); // Turn off wakeup, triggered on falling edge
}

////////////////////////////////////////////////////////////////////////////////
// wifi_dpm_wake_no_sleep()
//
// Wake the DA from DPM sleep and tell it not to sleep for now.
// If the DA is not in DPM mode, the it won't respond to the WAKEUP
// line, so we will time out and return.
//
//  @return : 1 if we were in DPM mode, 0 if not, -1 on error
int wifi_dpm_wake_no_sleep()
{
	k_timeout_t timeout = K_MSEC(400);
	k_timepoint_t timepoint = sys_timepoint_calc(timeout);
	wifi_wait_array_t wait_msgs;
	int ret;

	wifi_wake_DA(); // Pulse the WEAKEUP_RTC line.

	wait_msgs.num_msgs = 0; // Initialize the structure
	wifi_add_wait_msg(&wait_msgs, "+INIT:WAKEUP", true, 0);
	ret = wifi_wait_for(&wait_msgs, timeout);
	if (ret != 0) {
		return 0;
	}

	// At this point, we know we are in DPM mode and the caller wants
	// us to pause sleep.  So send the commands to the DA to do that.
	timeout = sys_timepoint_timeout(timepoint);
	if (wifi_send_ok_err_atcmd("AT+MCUWUDONE", timeout) == false) {
		LOG_ERR("Err with MCU WU Done");
		return -1;
	}

	timeout = sys_timepoint_timeout(timepoint);
	if (wifi_send_ok_err_atcmd("AT+CLRDPMSLPEXT", timeout) == false) {
		LOG_ERR("Couldn't stop dpm sleep");
		return -1;
	}
	return 1;
}

////////////////////////////////////////////////////////////////////////////////
// wifi_dpm_back_to_sleep()
//
// Tell the DA it can go back to sleep if it was woken up from dpm mode
//
//  @return - none
void wifi_dpm_back_to_sleep()
{
	// At spi speed of 4Mhz, my tests show this never takes more then 10ms
	k_timeout_t timeout = K_MSEC(30);
	if (wifi_send_ok_err_atcmd("AT+SETDPMSLPEXT", timeout) == false) {
		LOG_ERR("Error allowing dmp sleep");
	}
}

extern int g_current_cert;
extern bool g_time_set;
extern char *certs[3][3];

void wifi_insert_certs()
{
	wifi_flush_msgs();

	g_current_cert = 2;
	for (int i = 0; i < 3; i++) {
		if (!wifi_send_ok_err_atcmd(certs[1][i], K_MSEC(1000))) {
			LOG_ERR("Error inserting cert");
			return;
		}
	}
}

//////////////////////////////////////////////////////////////////////
// wifi_send_mqtt()
//  Send lost dog MQTT messages. We must not be in DPM sleep, and
//  we must be connected to an AP.
//
//  @param msg - pointer to buffer containing the message to send
//  @return - none
int wifi_send_mqtt(char *msg)
{
	char *broker_cmd, *tls_cmd, *sub_topic;
	wifi_wait_array_t wait_msgs;
	char mqtt_state[200];

	if (g_time_set == false) {
		LOG_ERR("encrypted mqtt certs need the time set, use 'da16200 set_time'");
		return -1;
	}
	broker_cmd = "AT+NWMQBR=a3hoon64f0fuap-ats.iot.eu-west-1.amazonaws.com,8883";
	tls_cmd = "AT+NWMQTLS=1";
	sub_topic = "messages/35/1/35_eas-mpb-test-001/c2d";

	if (!wifi_send_ok_err_atcmd("AT+NWMQCL=0", K_MSEC(200))) {
		LOG_ERR("Error Stopping MQTT service");
		return -1;
	}

	if (!wifi_send_ok_err_atcmd(broker_cmd, K_MSEC(1000))) {
		LOG_ERR("Error Setting the MQTT broker");
		return -1;
	}

	if (!wifi_send_ok_err_atcmd("AT+NWMQTP=messages/35/1/35_eas-mpb-test-001/d2c",
				    K_MSEC(1000))) {
		LOG_ERR("Error Setting the default publish topic");
		return -1;
	}

	if (!wifi_send_ok_err_atcmd("AT+NWMQCID=35_eas-mpb-test-001", K_MSEC(1000))) {
		LOG_ERR("Error Setting the client id");
		return -1;
	}

	if (!wifi_send_ok_err_atcmd("AT+NWMQCS=1", K_MSEC(1000))) {
		LOG_ERR("Error Setting the clean session flag");
		return -1;
	}

	if (!wifi_send_ok_err_atcmd(tls_cmd, K_MSEC(1000))) {
		LOG_ERR("Error setting the TLS flag");
		return -1;
	}

	if (!wifi_send_ok_err_atcmd("AT+NWMQCL=1", K_MSEC(200))) {
		LOG_ERR("Error Starting MQTT service");
		return -1;
	}

	// Wait for the connection to be established
	wait_msgs.num_msgs = 0; // Initialize the structure
	wifi_add_wait_msg(&wait_msgs, "+NWMQCL:%1s", true, 1, mqtt_state);
	if (wifi_wait_for(&wait_msgs, K_MSEC(7000)) != 0) {
		LOG_ERR("Timed out waiting for mqtt service to start");
		return -1;
	}

	if (!wifi_send_ok_err_atcmd(msg, K_MSEC(3000))) {
		LOG_ERR("Error publishing a connectivity test messasge");
		LOG_ERR("%18s", errstr);
		return -1;
	}
	k_sleep(K_MSEC(1000));
	if (!wifi_send_ok_err_atcmd("AT+NWMQCL=0", K_MSEC(1000))) {
		LOG_ERR("Error Stopping MQTT service");
		return -1;
	}
	return 0;
}

int wifi_set_ntp_server()
{
	if (!wifi_send_ok_err_atcmd("AT+NWSNTP=1,pool.ntp.org,86400", K_MSEC(1000))) {
		LOG_ERR("Error Setting the client id");
		return -1;
	}
	return 0;
}

///////////////////////////////////////////////////////////////////////
// wifi_get_otp_register()
//  Send a command to the DA to read OTP memory and return it.
//  This command has a non-standard response in that it comes
//  back in multiple parts
//
//  @param reg - the OTP register to read
//  @param size - the size of the register to read
//  @param timeout - timeout for the write
//
//  @return - the value of the register or -1 on error
int wifi_get_otp_register(int reg, int size, k_timeout_t timeout)
{
	int ret;
	wifi_wait_array_t wait_msgs;
	char cmd[50];
	char data[50];

	sprintf(cmd, "AT+UOTPRDASC=%x,%d", reg, size);

	k_timepoint_t timepoint = sys_timepoint_calc(timeout);
	if (wifi_send_timeout(cmd, timeout) != 0) {
		LOG_ERR("Timed out sending cmd"); // %10s", cmd);
		return -1;
	}

	cmd[0] = 0; // This will hold the data we receive
	timeout = sys_timepoint_timeout(timepoint);
	wait_msgs.num_msgs = 0; // Initialize the structure
	wifi_add_wait_msg(&wait_msgs, "\r\nERROR%50s\r\n", true, 1, errstr);
	wifi_add_wait_msg(&wait_msgs, "\r\nOK\r\n", true, 0);
	wifi_add_wait_msg(&wait_msgs, "\r\n", false,
			  0); // This is here and before the %s to toss msgs of only \r\n
	wifi_add_wait_msg(&wait_msgs, "\r", false,
			  0); // This is here and before the %s to toss msgs of only  \r
	wifi_add_wait_msg(&wait_msgs, "%50s", true, 1,
			  data); // This will catch anything, only data msgs should be left
	while (1) {
		ret = wifi_wait_for(&wait_msgs, timeout);
		timeout = sys_timepoint_timeout(timepoint);
		if (ret <= 0) {
			return -1;
		}
		if (ret == 4) {
			strcat(cmd, data); // Add the digits to the ones received so far
			continue;
		}
		if (ret == 1) {
			// OK ends the response
			return strtol(cmd, NULL, 16);
		}
	}
	return -1;
}

///////////////////////////////////////////////////////////////////////
// wifi_get_xtal()
//  Send a command to the DA to read the current XTAL value
//
//  @param timeout - timeout for the write
//
//  @return - the value of the register or -1 on error
int wifi_get_xtal(k_timeout_t timeout)
{
	int ret;
	wifi_wait_array_t wait_msgs;
	char cmd[50] = "AT+XTALRD";

	//  This command has a non-standard response in that it comes
	//  back in multiple parts

	k_timepoint_t timepoint = sys_timepoint_calc(timeout);
	if (wifi_send_timeout(cmd, timeout) != 0) {
		LOG_ERR("Timed out sending at+xtalrd");
		return -1;
	}

	cmd[0] = 0; // This will hold the data we receive
	timeout = sys_timepoint_timeout(timepoint);
	wait_msgs.num_msgs = 0; // Initialize the structure
	wifi_add_wait_msg(&wait_msgs, "\r\nERROR%50s\r\n", true, 1, errstr);
	wifi_add_wait_msg(&wait_msgs, "\r\nOK\r\n", true, 0);
	wifi_add_wait_msg(&wait_msgs, "0x%50s", false, 1, cmd); // This will catch the return value
	while (1) {
		ret = wifi_wait_for(&wait_msgs, timeout);
		timeout = sys_timepoint_timeout(timepoint);
		if (ret <= 0) {
			LOG_DBG("timeout or error on xtalrd");
			return -1;
		}
		if (ret == 1) {
			return strtol(cmd, NULL, 16);
		}
	}
	return -1;
}

///////////////////////////////////////////////////////////////////////
// wifi_set_xtal()
//  Send a command to the DA to set the current XTAL value, temporarily
//
//  @param timeout - timeout for the write
//
//  @return - true on success, false on error or timeout
bool wifi_set_xtal(int newval, k_timeout_t timeout)
{
	char cmd[50];
	sprintf(cmd, "AT+XTALWR=%x", newval);
	return wifi_send_ok_err_atcmd(cmd, timeout);
}

///////////////////////////////////////////////////////////////////////
// wifi_set_otp_register()
//  Send a command to the DA to write OTP memory only if the value
//  already there is 0.    OTP works on the DA at a bit level so future
//  writes cause a bitwise OR of old and new value.  So we read it first
//  and don't write it, if its not 0.
//
//  @param reg - the OTP register to read
//  @param size - the size of the register to read
//  @param timeout - timeout for the write
//
//  @return - the value of the register or -1 on error
int wifi_set_otp_register(int reg, int size, int newval, k_timeout_t timeout)
{
	int ret;
	char cmd[50];

	k_timepoint_t timepoint = sys_timepoint_calc(timeout);
	ret = wifi_get_otp_register(reg, size, timeout);
	if (ret != 0) {
		LOG_DBG("OTP register %x already set to %d", reg, ret);
		return -1;
	}
	if (newval > (2 ^ size)) {
		LOG_ERR("New value %d too big for OTP register %x", newval, reg);
		return -1;
	}

	sprintf(cmd, "AT+UOTPWRASC=%x,%d,%x", reg, size, newval);
	timeout = sys_timepoint_timeout(timepoint);
	if (wifi_send_ok_err_atcmd(cmd, timeout) == true) {
		return 0;
	} else {
		LOG_ERR("Error writing OTP register %x", reg);
		return -1;
	}
}

///////////////////////////////////////////////////////////////////////
// wifi_stop_XTAL_test()
//  Reboot the DA into XTAL normal
//
void wifi_stop_XTAL_test()
{
	wifi_send_ok_err_atcmd("AT+TMRFNOINIT=0", K_MSEC(1000));
	wifi_send_ok_err_atcmd("AT+RESTART", K_MSEC(1000));
}

///////////////////////////////////////////////////////////////////////
// wifi_start_XTAL_test()
//  Reboot the DA into XTAL test mode
//
//  @param timeout - timeout for the write
//
//  @return - the value of the register or -1 on error
int wifi_start_XTAL_test()
{
	int ret = -1;

	if (wifi_send_ok_err_atcmd("AT+TMRFNOINIT=1", K_MSEC(1000)) == false) {
		LOG_ERR("Error setting RF Test mode");
		return ret;
	}
	if (wifi_send_ok_err_atcmd("AT+RESTART", K_MSEC(1000)) == false) {
		LOG_ERR("Error setting RF Test mode");
		return ret;
	}

	k_sleep(K_MSEC(3000));

	if (wifi_send_ok_err_atcmd("AT+RFTESTSTART", K_MSEC(1000)) == false) {
		LOG_ERR("Error setting RF Test mode");
		wifi_stop_XTAL_test();
		return -1;
	}
	if (wifi_send_ok_err_atcmd("AT+RFCWTEST=2412,0,15", K_MSEC(1000)) == false) {
		LOG_ERR("Error setting RF Test mode");
		wifi_stop_XTAL_test();
		return -1;
	}
	return 0;
}

extern char com_buf[COM_BUF_LEN];
static void wifi_work_fn()
{
	wifi_flush_msgs();
	int was_sleeping = wifi_dpm_wake_no_sleep();
	if (was_sleeping == -1) {
		LOG_ERR("Error waking DA");
		return;
	}

	if (g_current_cert != 2) {
		wifi_insert_certs();
	}

	if (g_time_set == false) {
		g_time_set = true;
		wifi_set_ntp_server();
		k_sleep(K_MSEC(2000));
		goto back_to_sleep;
	}
#if 0
	wifi_arr_t *list = get_ssid_list(K_MSEC(1500));
	json_gps_obj_t gps = {.lat = 123.123, .lon = 456.123, .alt = 789.123, .acc = 1};
	fuel_gauge_info_t fuel;

	if (fuel_gauge_get_latest(&fuel) != 0) {
		LOG_ERR("Failed to get fuel gauge info");
		fuel.soc = 0;
	}

	if (list->count > 0) {
		char *json = json_lost_dog(list, fuel.soc, get_charging_active(), &gps, RADIO_TYPE_WIFI);
		strcpy(com_buf, "AT+NWMQMSG='");
		strcat(com_buf, json);
		strcat(com_buf, "',messages/35/1/35_eas-mpb-test-001/d2c");
		if (wifi_send_mqtt(com_buf) == 0) {
			// LOG_ERR("SSIDs sent");
		}
	} else {
		LOG_ERR("No SSIDs found");
	}
#endif
back_to_sleep:
	if (was_sleeping == 1) {
		wifi_dpm_back_to_sleep();
	}
}

void wifi_set_print_txrx(bool print)
{
#if (CONFIG_USE_UART_TO_DA16200)
	wifi_uart_set_print_txrx(print);
#else
	wifi_spi_set_print_txrx(print);
#endif
}

int wifi_set_power_key(bool newState)
{
	int ret = 0;
	if (newState) {
		ret = gpio_pin_set(gpio_p0, 28, 1);
	} else {
		ret = gpio_pin_set(gpio_p0, 28, 0);
	}
	return ret;
}

int wifi_set_3v0_enable(bool newState)
{
	int ret = 0;
	if (newState) {
		ret = gpio_pin_set(gpio_p1, 4, 1);
	} else {
		ret = gpio_pin_set(gpio_p1, 4, 0);
	}
	return ret;
}

int wifi_set_wakeup(bool newState)
{
	int ret = 0;
	if (newState) {
		ret = gpio_pin_set(gpio_p1, 8, 1);
	} else {
		ret = gpio_pin_set(gpio_p1, 8, 0);
	}
	return ret;
}

//////////////////////////////////////////////////////////
// wifi_send
//
// Write an AT or ESC command to the DA16200.
//
//  @param data - pointer to buffer containing the AT command
//
//  @return - 0 on success, -1 on error or timeout
int wifi_send(char *buf)
{
	int ret = 0;
#if (CONFIG_USE_UART_TO_DA16200)
	ret = wifi_uart_send(buf);
#else
	ret = wifi_spi_send(buf);
#endif
	return ret;
}

//////////////////////////////////////////////////////////
// wifi_send_timeout
//
// Write an AT or ESC command to the DA16200.
//
//  @param data - pointer to buffer containing the AT command
//  @param timeout - timeout for the write
//
//  @return - 0 on success, -1 on error or timeout
int wifi_send_timeout(char *buf, k_timeout_t timeout)
{
	int ret = 0;
#if (CONFIG_USE_UART_TO_DA16200)
	ret = wifi_uart_send_timeout(buf, timeout);
#else
	ret = wifi_spi_send_timeout(buf, timeout);
#endif
	return ret;
}

//////////////////////////////////////////////////////////
//	Receive a message from the queue,
//
// @param msg  - pointer to the wifi_msg_t structure that
//               will be filled on success
// @param timeout - timeout for the read
//
// @return - 0 on success, -1 on error or timeout
// @note caller must call wifi_msg_free() to free the
//       memory allocated for the message
int wifi_recv(wifi_msg_t *msg, k_timeout_t timeout)
{
#if (CONFIG_USE_UART_TO_DA16200)
	return wifi_uart_recv(msg, timeout);
#else
	return wifi_spi_recv(msg, timeout);
#endif
}

//////////////////////////////////////////////////////////
// wifi_msg_free()
//
// Free the memory allocated for a wifi_msg_t
// @param msg - pointer to the wifi_msg_t to free
void wifi_msg_free(wifi_msg_t *msg)
{
#if (CONFIG_USE_UART_TO_DA16200)
	wifi_uart_msg_free(msg);
#else
	wifi_spi_msg_free(msg);
#endif
}

void wifi_set_rx_cb(wifi_on_rx_cb_t cb, void *user_data)
{
#if (CONFIG_USE_UART_TO_DA16200)
	wifi_uart_set_rx_cb(cb, user_data);
#else
	wifi_spi_set_rx_cb(cb, user_data);
#endif
}

//////////////////////////////////////////////////////////
//  wifi_flush_msgs()
//
//  Flush all messages from the receive queue
void wifi_flush_msgs()
{
#if (CONFIG_USE_UART_TO_DA16200)
	wifi_uart_flush_msgs();
#else
	wifi_spi_flush_msgs();
#endif
}

//////////////////////////////////////////////////////////
// wifi_msg_cnt
//
// Return the number of messages in the queue
//
//  @return - number of messages in the queue
int wifi_msg_cnt()
{
#if (CONFIG_USE_UART_TO_DA16200)
	return wifi_uart_msg_cnt();
#else
	return wifi_spi_msg_cnt();
#endif
}

//////////////////////////////////////////////////////////
//	Requeue a message that was recv'd
//
// @param msg  - a wifi_msg_t that was obtained
//               from wifi_recv()
//
// @note caller can call this instead of wifi_msg_free()
void wifi_requeue(wifi_msg_t *msg)
{
#if (CONFIG_USE_UART_TO_DA16200)
	wifi_uart_requeue(msg);
#else
	wifi_spi_requeue(msg);
#endif
}

int  wifi_1v8_on()
{
	set_switch_state(PMIC_SWITCH_WIFI, true);
	return 0;
}

int wifi_1v8_off()
{
	set_switch_state(PMIC_SWITCH_WIFI, false);
	return 0;
}

void print_wifi_wait_array(wifi_wait_array_t *arr)
{
	LOG_DBG("num_msgs: %d", arr->num_msgs);
	for (int i = 0; i < arr->num_msgs; i++) {
		LOG_DBG("msg[%d]: %s", i, arr->msgs[i]);
		LOG_DBG("num_params_per_msg[%d]: %d", i, arr->num_params_per_msg[i]);
		for (int j = 0; j < arr->num_params_per_msg[i]; j++) {
			LOG_DBG("param_ptrs[%d][%d]: %s", i, j,
				arr->param_ptrs[i][j] ? "not null" : "null");
		}
		LOG_DBG("stop_waiting[%d]: %d", i, arr->stop_waiting[i]);
		LOG_DBG("num_matched[%d]: %d", i, arr->num_matched[i]);
	}
}

//////////////////////////////////////////////////////////
//  wifi_add_wait_msg()
//  Add a message to the structure holding the list of
//  messages to wait for to be passed to wifi_wait_for()
//
// @param wait_msgs - pointer to the wifi_wait_array_t
//                    structure to add the message to
// @param msg - pointer to a scanf format str to match
//              incoming messages against. The format
//              string must only contain %s for each parameter
// @param stop_waiting - true if wifi_wait_for() should stop
//                       waiting after this message is received
// @param num_params - number of parameters in the format string
// @param ... - pointers to the char buffers to be filled in
//              when the message is matched
void wifi_add_wait_msg(wifi_wait_array_t *wait_msgs, char *msg, bool stop_waiting, int num_params,
		       ...)
{
	va_list args;
	va_start(args, num_params);

	int idx = wait_msgs->num_msgs;
	if (idx < WIFI_MAX_WAIT_MSGS) {
		wait_msgs->msgs[idx] = msg;
		wait_msgs->num_params_per_msg[idx] = num_params;
		for (int i = 0; i < num_params && i < WIFI_MAX_WAIT_MSG_PARAMS; i++) {
			wait_msgs->param_ptrs[idx][i] = va_arg(args, char *);
		}
		wait_msgs->stop_waiting[idx] = stop_waiting;
		wait_msgs->num_matched[idx] = 0;
		wait_msgs->num_msgs++;
	}
	va_end(args);
}

static int match_message(wifi_msg_t *msg, wifi_wait_array_t *wait_msgs)
{
	char *substr;
	char preface[10];
	int amt = WIFI_MSG_SIZE;

	for (int i = 0; i < wait_msgs->num_msgs && i < WIFI_MAX_WAIT_MSGS; i++) {
		// We want to find the format string anywhere in the message, so we
		// look for a match for everything up to the first % and do a
		// scanf at that point

		// Special case to grab the whole message, even line feeds, but respect
		// the amount in the format of %xxxxxs
		if ((strcmp("%s", wait_msgs->msgs[i]) == 0) ||
		    ((sscanf(wait_msgs->msgs[i], "%%%ds", &amt) == 1) &&
		     strlen(wait_msgs->msgs[i]) < 7)) {
			strncpy(wait_msgs->param_ptrs[i][0], msg->data, amt);
			return i;
		}

		char *tomatch = wait_msgs->msgs[i];
		char *end = strchr(wait_msgs->msgs[i], '%');
		if (end != NULL) {
			// There is at least one % in the format string
			int len = end - tomatch;
			if (len > 9) {
				len = 9;
			}
			memcpy(preface, tomatch, len);
			preface[len] = 0;
			substr = strstr(msg->data, preface);
			if (substr == NULL) {
				continue;
			}
		} else {
			// No % in the format string, so we just look for the whole thing
			substr = msg->data;
		}
		switch (wait_msgs->num_params_per_msg[i]) {
		case 0:
			if (strstr(substr, wait_msgs->msgs[i]) == NULL) {
				continue;
			}
			wait_msgs->num_matched[i] = 0;
			break;
		case 1:
			wait_msgs->num_matched[i] =
				sscanf(substr, wait_msgs->msgs[i], wait_msgs->param_ptrs[i][0]);
			break;
		case 2:
			wait_msgs->num_matched[i] =
				sscanf(substr, wait_msgs->msgs[i], wait_msgs->param_ptrs[i][0],
				       wait_msgs->param_ptrs[i][1]);
			break;
		case 3:
			wait_msgs->num_matched[i] =
				sscanf(substr, wait_msgs->msgs[i], wait_msgs->param_ptrs[i][0],
				       wait_msgs->param_ptrs[i][1], wait_msgs->param_ptrs[i][2]);
			break;
		case 4:
			wait_msgs->num_matched[i] =
				sscanf(substr, wait_msgs->msgs[i], wait_msgs->param_ptrs[i][0],
				       wait_msgs->param_ptrs[i][1], wait_msgs->param_ptrs[i][2],
				       wait_msgs->param_ptrs[i][3]);
			break;
		case 5:
			wait_msgs->num_matched[i] =
				sscanf(substr, wait_msgs->msgs[i], wait_msgs->param_ptrs[i][0],
				       wait_msgs->param_ptrs[i][1], wait_msgs->param_ptrs[i][2],
				       wait_msgs->param_ptrs[i][3], wait_msgs->param_ptrs[i][4]);
			break;
		default:
			continue;
		}
		return i;
	}
	return -1;
}

//////////////////////////////////////////////////////////
//	Wait for one of a few message to arrive, capturing
//  parameters
//
// @param wait_msgs - pointer to the wifi_wait_array_t
//                    holding what messager to wait for
// @param timeout - timeout for the read
//
// @return - index of the message that was matched, -1 on error or timeout
int wifi_wait_for(wifi_wait_array_t *wait_msgs, k_timeout_t timeout)
{
	k_timepoint_t timepoint = sys_timepoint_calc(timeout);
	wifi_msg_t msg;

	if (wait_msgs->num_msgs > WIFI_MAX_WAIT_MSGS) {
		LOG_ERR("exceeded max messages that can be waited for");
		return -1;
	}

	while (1) {
		timeout = sys_timepoint_timeout(timepoint);
		if (K_TIMEOUT_EQ(timeout, K_NO_WAIT)) {
			break;
		}
		if (wifi_recv(&msg, timeout) == 0) {
			int ret = match_message(&msg, wait_msgs);
			wifi_msg_free(&msg);
			// This msg matches something and that match stops the search
			if (ret > -1 && wait_msgs->stop_waiting[ret]) {
				return ret;
			}
		} else {
			// timeout
			break;
		}
	}
	return -1;
}

//////////////////////////////////////////////////////////
//	wifi_send_and_wait_for()
//  Send an cmd and wait for one of a few messages to
//  arrive in response, capturing parameters
//
// @param cmd - the atcmd
// @param wait_msgs - pointer to the wifi_wait_array_t
//                    holding what messager to wait for
// @param timeout - timeout for the whole operation
//
// @return - index of the message that was matched,
//           -1 on error or timeout
int wifi_send_and_wait_for(char *cmd, wifi_wait_array_t *wait_msgs, k_timeout_t timeout)
{
	k_timepoint_t timepoint = sys_timepoint_calc(timeout);
	if (wifi_send_timeout(cmd, timeout) != 0) {
		return -1;
	}

	timeout = sys_timepoint_timeout(timepoint);
	return wifi_wait_for(wait_msgs, timeout);
}