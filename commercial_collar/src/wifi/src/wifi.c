/* Copyright (c) 2024, Nestle Purina Pet Care. All rights reserved */
/****************************************************************
 *  wifi.c
 *  SPDX-License-Identifier: LicenseRef-Proprietary
 *
 * The wifi code is layered as follows:
 *     wifi_spi.c or wifi_uart.c - Talked to hardware to transfer data to and from the DA1200
 *     wifi.c - HW independent layer to send adn receive messages to and from the DA1200
 *     wifi_at.c - AT command layer to send and receive AT commands to and from the DA1200
 *     net_mgr.c - The network (wifi and lte) api layer, manages the state of the da and
 *                 lte chip and network communication state machine and publishes zbus
 *                 message when revevent stuff happens
 *     wifi_shell.c - A collection of shell commands to test and debug during development
 */
#include "wifi.h"
#include <zephyr/logging/log.h>
#include "wifi_spi.h"
#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include "pmic.h"

static int num_of_heap_elements = 0;

LOG_MODULE_REGISTER(d1_wifi, CONFIG_D1_WIFI_LOG_LEVEL);

// A queue to stores wifi_msg_t elements.  The data buffer
// should be k_heap_free()d when done with the message
#define WIFI_MSG_Q_SIZE 40
K_MSGQ_DEFINE(wifi_msgq, sizeof(wifi_msg_t), WIFI_MSG_Q_SIZE, 4);
K_HEAP_DEFINE(wifi_heap, WIFI_MSG_SIZE * 7);

// We want to serialize access to the DA so that two threads can't
// step on each others toes.  This mutex is used to do that.
K_MUTEX_DEFINE(DA_mutex);

// EAS XXX: DA ambiguity
// g_last_wake_time is used to tell if a INIT:DONE message was
// send as a result of us WAKING the DA up or because of a reboot.
// We save when we last sent a wake and ignore INIT:DONE messages
// soon after a wake.
// We don't want to miss the first INIT after a power up/reboot so
// initialize this to 10 seconds past boot (i.e. 10000)
uint64_t             g_last_wake_time           = 10000;
bool                 g_DA_needs_one_time_config = true;
const struct device *gpio_p0                    = NULL;
const struct device *gpio_p1                    = NULL;

wifi_tx_rx_cb_t      wifi_tx_rx_cb[WIFI_NUM_RX_CB]          = { NULL, NULL, NULL, NULL, NULL };
void                *wifi_tx_rx_cb_userData[WIFI_NUM_RX_CB] = { NULL, NULL, NULL, NULL, NULL };
static volatile bool g_wifi_power_key                       = false;
static volatile bool g_wifi_1v8_on                          = false;
static volatile bool g_wifi_3v0_on                          = false;

//////////////////////////////////////////////////////////
// wifi_init()
//
// Initialize the wifi module and the underlying hardware
//
// @return - 0 on success, -1 on error
int wifi_init()
{
    int ret = 0;

    wifi_1v8_on();

    gpio_p1 = device_get_binding("gpio@842800");
    if (gpio_p1 == NULL) {
        LOG_ERR("Error: didn't find %s device", "GPIO_1");
        return -1;
    } else {
        int ret = gpio_pin_configure(gpio_p1, WIFI_3V0_PIN, GPIO_OUTPUT_ACTIVE);
        if (ret != 0) {
            LOG_ERR("Error %d: failed to configure %s pin %d", ret, "GPIO_1", WIFI_3V0_PIN);
        } else {
            g_wifi_3v0_on = true;
            LOG_DBG("turned on the WIFI 3v0_enable");
        }
        ret = gpio_pin_configure(gpio_p1, WIFI_LEVEL_SHIFTER_PIN, GPIO_OUTPUT_ACTIVE);
        if (ret != 0) {
            LOG_ERR("Error %d: failed to configure %s pin %d", ret, "GPIO_1", WIFI_LEVEL_SHIFTER_PIN);
        } else {
            LOG_DBG("configured WIFI level shifter");
        }
        ret = gpio_pin_set(gpio_p1, WIFI_LEVEL_SHIFTER_PIN, 1);
        if (ret != 0) {
            LOG_ERR("Error %d: failed to turn on level shifter, %s pin %d", ret, "GPIO_1", WIFI_LEVEL_SHIFTER_PIN);
        } else {
            LOG_DBG(" WIFI level shifter turned on");
        }
        ret = gpio_pin_configure(gpio_p1, WIFI_WAKEUP_PIN, GPIO_OUTPUT_ACTIVE);
        if (ret != 0) {
            LOG_ERR("Error %d: failed to configure %s pin %d", ret, "GPIO_1", WIFI_WAKEUP_PIN);
        } else {
            LOG_DBG("configured WIFI wakeup");
        }
    }

    gpio_p0 = device_get_binding("gpio@842500");
    if (gpio_p0 == NULL) {
        LOG_ERR("Error: didn't find %s device", "GPIO_0");
        return -1;
    } else {
        int ret = gpio_pin_configure(gpio_p0, WIFI_POWER_KEY_PIN, GPIO_OUTPUT_ACTIVE);
        if (ret != 0) {
            LOG_ERR("Error %d: failed to configure %s pin %d", ret, "GPIO_0", WIFI_POWER_KEY_PIN);
        } else {
            g_wifi_power_key = true;
            LOG_DBG("turned on the WIFI power_key");
        }
    }

    LOG_DBG("Initializing SPI to DA16200");
    ret = wifi_spi_init();
    if (ret != 0) {
        LOG_ERR("Error initializing wifi");
        return ret;
    }

    LOG_DBG("Wifi Enabled.");
    return ret;
}

//////////////////////////////////////////////////////////
// wifi_add_rtx_x_cb
//
// Add a callback to be called when a message is received
//
// @param cb - callback to call when a message is received
// @param user_data - user data to pass to the callback
//
// @return - On success a >0 id to be used when removing the
//           callback, -1 on error
// @note the callback should return true if it consumed the
//       message and false if it didn't
int wifi_add_tx_rx_cb(wifi_tx_rx_cb_t cb, void *user_data)
{
    for (int i = 0; i < WIFI_NUM_RX_CB; i++) {
        if (wifi_tx_rx_cb[i] == NULL) {
            wifi_tx_rx_cb[i]          = cb;
            wifi_tx_rx_cb_userData[i] = user_data;
            return i;
        }
    }
    return -1;
}

//////////////////////////////////////////////////////////
// wifi_rem_tx_rx_cb
//
// Remove a callback that was added with wifi_spi_add_tx_rx_cb
//
// @param id - id returned from wifi_spi_add_tx_rx_cb
void wifi_rem_tx_rx_cb(int id)
{
    if (id < WIFI_NUM_RX_CB) {
        wifi_tx_rx_cb[id]          = NULL;
        wifi_tx_rx_cb_userData[id] = NULL;
    }
}

//////////////////////////////////////////////////////////
// wifi_notify_cbs
//
// This gets called when a message is received on the SPI.
//
// Notify all the registered callbacks that a message is
// about to be sent or has just been received.
//
// Messages start with ref_count set to 1.
// If a callback is interested in the message, it should
// call wifi_msg_inc() to prevent the message from being
// freed until it is done with it.
//
// After the callbacks are called, the message will be
// placed on the message queue for later processing.
// Message left on the queue may be freed by the system
// if the queue is full, we are out of memory, or someone
// calls wifi_flush_msgs().
//
// @param msg - pointer to the message
//
// @return - true if the message was consumed by a callback
int wifi_notify_cbs(wifi_msg_t *msg)
{
    int ret;

    for (int i = 0; i < WIFI_NUM_RX_CB; i++) {
        if (wifi_tx_rx_cb[i] != NULL) {
            // If the callback has consumed the message it will return
            // true. If it was a received messsage, don't place it on
            // the message queue and free the memory
            if (msg->ref_count > 0) {
                wifi_tx_rx_cb[i](msg, wifi_tx_rx_cb_userData[i]);
            }
            // EAS Delta caution.  The biggest difference with the new ref counting
            // is that consumed message aren't freed at this point.  They could be
            // freed by the callback, so the cbs retain that ability
            // If the cbs don't, then more messages will be put into the queue
            // I think this is ok because of flushes and the code in
            // wifi_alloc_msg() that frees messages if the heap is full. Fingers crossed
        }
    }
    if (msg->ref_count > 0 && msg->incoming == 1) {
        // Copy the wifi_msg_t into the msg response queue
        ret = k_msgq_put(&wifi_msgq, msg, K_NO_WAIT);
        if (ret != 0) {
            LOG_ERR("Failed to put data on queue");
            return ret;
        }
    }
    return 0;
}

////////////////////////////////////////////////////////////
// wifi_wake_DA()
//
// try to wake up the DA by pulsing the WAKEUP_RTC line
// The DA wakes on the falling edge of WAKEUP_RTC so if
// it is already low, we need to pulse it high first.
//
// @param ms_delay - delay in ms after the pulse

// @return - none
void wifi_wake_DA(int ms_delay)
{
    g_last_wake_time = k_uptime_get();
    gpio_pin_set(gpio_p1, WIFI_WAKEUP_PIN, 1);    // Pulse high
    k_sleep(K_MSEC(ms_delay));
    gpio_pin_set(gpio_p1, WIFI_WAKEUP_PIN, 0);    // Turn off wakeup, triggered on falling edge
}

int wifi_set_power_key(bool newState)
{
    int ret = 0;
    if (newState) {
        g_wifi_power_key = true;
        g_last_wake_time = k_uptime_get();
        ret              = gpio_pin_set(gpio_p0, WIFI_POWER_KEY_PIN, 1);
    } else {
        g_wifi_power_key = false;
        ret              = gpio_pin_set(gpio_p0, WIFI_POWER_KEY_PIN, 0);
    }
    return ret;
}

int wifi_set_3v0_enable(bool newState)
{
    int ret = 0;
    if (newState) {
        ret           = gpio_pin_set(gpio_p1, WIFI_3V0_PIN, 1);
        g_wifi_3v0_on = true;
    } else {
        g_DA_needs_one_time_config = true;
        g_wifi_3v0_on              = false;
        ret                        = gpio_pin_set(gpio_p1, WIFI_3V0_PIN, 0);
    }
    return ret;
}

bool wifi_get_3v0()
{
    return g_wifi_3v0_on;
}

//////////////////////////////////////////////////////////
// wifi_1v8_on()
//
// This call tells the PMIC to not power the 1.8v line
// to the DA which it uses to read the data lines from
// the 5340.  The 3v3_enable is used to power the DA
// and the level shifter.
int wifi_1v8_on()
{
    set_switch_state(PMIC_SWITCH_WIFI, true);
    g_wifi_1v8_on = true;
    return 0;
}

int wifi_1v8_off()
{
    set_switch_state(PMIC_SWITCH_WIFI, false);
    g_wifi_1v8_on = false;
    return 0;
}

bool wifi_get_1v8()
{
    return g_wifi_1v8_on;
}

int wifi_set_wakeup(bool newState)
{
    int ret = 0;
    if (newState) {
        ret = gpio_pin_set(gpio_p1, WIFI_WAKEUP_PIN, 1);
    } else {
        ret = gpio_pin_set(gpio_p1, WIFI_WAKEUP_PIN, 0);
    }
    return ret;
}

//////////////////////////////////////////////////////////
// wifi_reset()
//  Power cycle the DA16200
void wifi_reset()
{
    wifi_1v8_off();
    wifi_set_power_key(0);
    wifi_set_3v0_enable(0);
    wifi_set_level_shifter(false);
    k_msleep(1);
    wifi_1v8_on();
    wifi_set_power_key(1);
    wifi_set_3v0_enable(1);
    wifi_set_level_shifter(true);
    k_msleep(1);
}

//////////////////////////////////////////////////////////
// wifi_get_power_key
//
// Return the state of the power key
//
//  @return - true if da is on, false otherwise
bool wifi_get_power_key()
{
    return g_wifi_power_key;
}

//////////////////////////////////////////////////////////
// wifi_send_timeout
//
// Write an AT or ESC command to the DA16200.
//
//  @param data - pointer to buffer containing the AT command
//  @param timeout - timeout for the write
//
//  @return - 0 on success,
//            -EBUSY if mutex is taken
//			  -errno if 1 on error or timeout
int wifi_send_timeout(char *buf, k_timeout_t timeout)
{
    int ret = 0;
    if (wifi_get_mutex(timeout, __func__) != 0) {
        LOG_ERR("Failed to get mutex");
        return -EBUSY;
    }

    wifi_msg_t msg = { .timestamp = k_uptime_get(), .incoming = 0, .data = buf, .data_len = strlen(buf), .ref_count = 1 };

    // Notify the callbacks that we are about to send a message
    // to allow them to track the DA state or prevent the message
    wifi_notify_cbs(&msg);    // There isn't an error it can return that affects us

    ret = wifi_spi_send(buf);
    wifi_release_mutex();
    return ret;
}

//////////////////////////////////////////////////////////
//	Receive a message from the queue,
//
// @param msg  - pointer to the wifi_msg_t structure that
//               will be filled on success
// @param timeout - timeout for the read
//
// @return - 0 on success,
//		     -EBUSY if the mutex is already locked
//           -EAGAIN if timeout
//           -ENOMSG if there is not msg queued
//
// @note caller must call wifi_msg_free() on the data
// field of the msg to free the mem allocated
int wifi_recv(wifi_msg_t *msg, k_timeout_t timeout)
{
    int           ret       = -1;
    k_timepoint_t timepoint = sys_timepoint_calc(timeout);

    if (wifi_get_mutex(timeout, __func__) != 0) {
        return -EBUSY;
    }
    timeout = sys_timepoint_timeout(timepoint);
    if (msg != NULL) {
        ret = k_msgq_get(&wifi_msgq, msg, timeout);
    }
    wifi_release_mutex();
    return ret;
}

//////////////////////////////////////////////////////////
// wifi_msg_alloc()
//
// Alloc memory from the wifi heap
static int   alloc_fails_in_a_row = 0;
static void *wifi_msg_alloc(int amount)
{
    void      *data = NULL;
    wifi_msg_t msg;
    uint64_t   now = k_uptime_get();
    int        ret, qsize = k_msgq_num_used_get(&wifi_msgq);

    for (int i = 0; i < WIFI_MSG_Q_SIZE; i++) {
        data = k_heap_alloc(&wifi_heap, amount, K_NO_WAIT);
        if (data != NULL) {
#if CONFIG_DEBUG_WIFI_HEAP == 1
            LOG_DBG("[%d]Allocated %d bytes at %p, qsize = %d", num_of_heap_elements, amount, data, qsize);
#endif
            alloc_fails_in_a_row = 0;
            num_of_heap_elements++;
            return data;
        }
        // No heap, flush message in msg q until we have room
        if (qsize <= 0) {
            LOG_ERR("No heap but nothing in the msgq");
            alloc_fails_in_a_row++;
            if (alloc_fails_in_a_row > 5) {
                LOG_ERR("Too many alloc fails in a row, rebooting");
                pmic_reboot("wifi_msg_alloc");
            }
            return NULL;
        }
        ret = k_msgq_peek(&wifi_msgq, &msg);
        if (ret != 0) {
            LOG_ERR("Failed to peek at wifi msgq (cnt = %d)", qsize);
            alloc_fails_in_a_row++;
            if (alloc_fails_in_a_row > 5) {
                LOG_ERR("Too many alloc fails in a row, rebooting");
                pmic_reboot("wifi_msg_alloc");
            }
            return NULL;
        }

        if (now - msg.timestamp <= WIFI_FLUSH_AGE) {
            LOG_ERR("No heap and eveything in the msgq is recent, msgq cnt = %d", qsize);
            alloc_fails_in_a_row++;
            if (alloc_fails_in_a_row > 5) {
                LOG_ERR("Too many alloc fails in a row, rebooting");
                pmic_reboot("wifi_msg_alloc");
            }
            return NULL;
        }

        ret = k_msgq_get(&wifi_msgq, &msg, K_NO_WAIT);
        if (ret != 0) {
            LOG_ERR("Failed to grab msg from wifi msgq, cnt = %d", qsize);
            alloc_fails_in_a_row++;
            if (alloc_fails_in_a_row > 5) {
                LOG_ERR("Too many alloc fails in a row, rebooting");
                pmic_reboot("wifi_msg_alloc");
            }
            return NULL;
        }
        qsize--;
        wifi_msg_free(&msg);
    }
    return data;
}

//////////////////////////////////////////////////////////
// wifi_init_new_msg()
//
// Initialize a msg structure and allocate memory for the
// data part of that structure.
//
// Implementation note: The msg structs are expected to
// be copyable so need to be just data, but the data
// fields is a allocated pointer that needs to be freed.
int wifi_init_new_msg(wifi_msg_t *msg, bool incoming, int buf_size)
{
    if (msg == NULL) {
        return -EINVAL;
    }
    msg->ref_count = 1;
    msg->incoming  = incoming;
    msg->timestamp = k_uptime_get();
    msg->data      = wifi_msg_alloc(buf_size);
    if (msg->data == NULL) {
        msg->ref_count = 0;
        return -ENOMEM;
    }
    msg->data_len = buf_size;
    return 0;
}

//////////////////////////////////////////////////////////
//	Increment the reference count of a message.
//
// @param msg  - pointer to the wifi_msg_t structure that
//               will be filled on success
//
// @return - 0 on success,
int wifi_inc_ref_count(wifi_msg_t *msg)
{
    if (msg != NULL) {
        msg->ref_count++;
        return 0;
    }
    return -1;
}

//////////////////////////////////////////////////////////
// wifi_msg_free()
//
// Free the memory helpt by the data element of a
// wifi_msg_t
//
// @param msg - pointer to a wifi_msg_t
void wifi_msg_free(wifi_msg_t *msg)
{
    if (msg == NULL) {
        LOG_ERR("NULL msg");
        return;
    }
    msg->ref_count--;
    if (msg->ref_count > 0) {
        return;
    }
    if (msg->ref_count < 0) {
        LOG_ERR("msg at %p(data %p) has ref count < 0 (%d)", msg, msg->data, msg->ref_count);
        return;
    }
    if (msg->data == NULL) {
        LOG_ERR("msg at %p data is null", msg);
        return;
    }
    num_of_heap_elements--;
#if CONFIG_DEBUG_WIFI_HEAP == 1
    LOG_DBG("[%d]Freeing %p", num_of_heap_elements, msg->data);
#endif
    k_heap_free(&wifi_heap, msg->data);
    msg->data = NULL;
}

//////////////////////////////////////////////////////////
//  wifi_flush_msgs()
//
//  Flush all messages from the receive queue
void wifi_flush_msgs()
{
    wifi_msg_t msg;
    if (wifi_get_mutex(K_NO_WAIT, __func__) != 0) {
        return;
    }
    while (k_msgq_get(&wifi_msgq, &msg, K_NO_WAIT) == 0) {
        wifi_msg_free(&msg);
    }
    wifi_release_mutex();
}

//////////////////////////////////////////////////////////
// wifi_msg_cnt
//
// Return the number of messages in the queue
//
//  @return - number of messages in the queue
int wifi_msg_cnt()
{
    return k_msgq_num_used_get(&wifi_msgq);
}

int wifi_peek_msg(wifi_msg_t *msg, int index)
{
    return k_msgq_peek_at(&wifi_msgq, msg, index);
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
    if (wifi_get_mutex(K_NO_WAIT, __func__) != 0) {
        return;
    }
    if (msg != NULL) {
        k_msgq_put(&wifi_msgq, msg, K_NO_WAIT);
    }
    wifi_release_mutex();
}

//////////////////////////////////////////////////////////
//  wifi_set_level_shifter()
//  Change the state of the level shifter for the wifi
//
// @param new_state - true to turn on the level shifter,
//
// @return - 0 on success, -1 on error
int wifi_set_level_shifter(bool new_state)
{
    return gpio_pin_set(gpio_p1, WIFI_LEVEL_SHIFTER_PIN, (new_state ? 1 : 0));
}

void print_wifi_wait_array(wifi_wait_array_t *arr)
{
    LOG_DBG("num_msgs: %d", arr->num_msgs);
    for (int i = 0; i < arr->num_msgs; i++) {
        LOG_DBG("msg[%d]: %s", i, arr->msgs[i]);
        LOG_DBG("num_params_per_msg[%d]: %d", i, arr->num_params_per_msg[i]);
        for (int j = 0; j < arr->num_params_per_msg[i]; j++) {
            LOG_DBG("param_ptrs[%d][%d]: %s", i, j, arr->param_ptrs[i][j] ? "not null" : "null");
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
void wifi_add_wait_msg(wifi_wait_array_t *wait_msgs, char *msg, bool stop_waiting, int num_params, ...)
{
    va_list args;
    va_start(args, num_params);

    int idx = wait_msgs->num_msgs;
    if (idx < WIFI_MAX_WAIT_MSGS) {
        wait_msgs->msgs[idx]               = msg;
        wait_msgs->num_params_per_msg[idx] = num_params;
        for (int i = 0; i < num_params && i < WIFI_MAX_WAIT_MSG_PARAMS; i++) {
            wait_msgs->param_ptrs[idx][i] = va_arg(args, char *);
        }
        wait_msgs->stop_waiting[idx] = stop_waiting;
        wait_msgs->num_matched[idx]  = 0;
        wait_msgs->num_msgs++;
    }
    va_end(args);
}

//////////////////////////////////////////////////////////
// match_message()
//
// Match a message to a format string and capture the
// parameters
//
// @param msg - pointer to the wifi_msg_t to match
// @param wait_msgs - pointer to the wifi_wait_array_t
//                    holding what messager to wait for
//
// @return - >= 0 - index of the message that was matched,
//			-NOMSG  if not matched
//////////////////////////////////////////////////////////
static int match_message(wifi_msg_t *msg, wifi_wait_array_t *wait_msgs)
{
    char *substr;
    char  preface[10];
    int   amt = WIFI_MSG_SIZE;

    for (int i = 0; i < wait_msgs->num_msgs && i < WIFI_MAX_WAIT_MSGS; i++) {
        // We want to find the format string anywhere in the message, so we
        // look for a match for everything up to the first % and do a
        // scanf at that point

        // Special case to grab the whole message, even line feeds, but respect
        // the amount in the format of %xxxxxs
        if (wait_msgs->msgs[i][0] == '%') {
            int num_digits = strspn(wait_msgs->msgs[i] + 1, "0123456789");
            if (wait_msgs->msgs[i][num_digits + 1] == 's') {
                if (num_digits > 0) {
                    amt = MIN(atoi(wait_msgs->msgs[i] + 1), WIFI_MSG_SIZE);
                }
                int maxcpy = MIN(amt, msg->data_len);
                memcpy(wait_msgs->param_ptrs[i][0], msg->data, maxcpy);
                return i;
            }
        }

        char *tomatch = wait_msgs->msgs[i];
        char *end     = strchr(wait_msgs->msgs[i], '%');
        if (end != NULL) {
            // There is at least one % in the format string
            int len = end - tomatch;
            if (len > 9) {
                len = 9;
            }
            memcpy(preface, tomatch, len);
            preface[len] = 0;
            substr       = strstr(msg->data, preface);
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
            wait_msgs->num_matched[i] = sscanf(substr, wait_msgs->msgs[i], wait_msgs->param_ptrs[i][0]);
            break;
        case 2:
            wait_msgs->num_matched[i] =
                sscanf(substr, wait_msgs->msgs[i], wait_msgs->param_ptrs[i][0], wait_msgs->param_ptrs[i][1]);
            break;
        case 3:
            wait_msgs->num_matched[i] = sscanf(
                substr,
                wait_msgs->msgs[i],
                wait_msgs->param_ptrs[i][0],
                wait_msgs->param_ptrs[i][1],
                wait_msgs->param_ptrs[i][2]);
            break;
        case 4:
            wait_msgs->num_matched[i] = sscanf(
                substr,
                wait_msgs->msgs[i],
                wait_msgs->param_ptrs[i][0],
                wait_msgs->param_ptrs[i][1],
                wait_msgs->param_ptrs[i][2],
                wait_msgs->param_ptrs[i][3]);
            break;
        case 5:
            wait_msgs->num_matched[i] = sscanf(
                substr,
                wait_msgs->msgs[i],
                wait_msgs->param_ptrs[i][0],
                wait_msgs->param_ptrs[i][1],
                wait_msgs->param_ptrs[i][2],
                wait_msgs->param_ptrs[i][3],
                wait_msgs->param_ptrs[i][4]);
            break;
        default:
            continue;
        }
        return i;
    }
    return -ENOMSG;
}

//////////////////////////////////////////////////////////
//	Wait for one of a few message to arrive, capturing
//  parameters
//
// @param wait_msgs - pointer to the wifi_wait_array_t
//                    holding what messager to wait for
// @param timeout - timeout for the read
//
// @return - index of the message that was matched, i
//           -EINVAL if the number of messages is too large
//           -EBUSY if the mutex is already locked
//           -EAGAIN if timeout
//           -ENOMSG if there is no msg queued
int wifi_wait_for(wifi_wait_array_t *wait_msgs, k_timeout_t timeout)
{
    k_timepoint_t timepoint = sys_timepoint_calc(timeout);
    wifi_msg_t    msg;

    if (wait_msgs->num_msgs > WIFI_MAX_WAIT_MSGS) {
        LOG_ERR("exceeded max messages that can be waited for");
        return -EINVAL;
    }

    if (wifi_get_mutex(timeout, __func__) != 0) {
        return -EBUSY;
    }

    int ret = -EAGAIN;
    while (1) {
        timeout = sys_timepoint_timeout(timepoint);
        if (sys_timepoint_expired(timepoint) || K_TIMEOUT_EQ(timeout, K_NO_WAIT)) {
            LOG_ERR("Timeout expired waiting for message");
            break;
        }
        ret = wifi_recv(&msg, timeout);
        if (ret == -ENOMSG) {
            continue;
        }
        if (ret != 0) {
            break;
        }
        ret = match_message(&msg, wait_msgs);
        wifi_msg_free(&msg);
        // This msg matches something and that match stops the search
        if (ret > -1 && wait_msgs->stop_waiting[ret]) {
            break;
        }
        // else we didn't match what the caller is
        // is looking for, so check the next msg
    }
    wifi_release_mutex();
    return ret;
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
    int           ret;
    if (wifi_get_mutex(timeout, __func__) != 0) {
        return -1;
    }
    timeout = sys_timepoint_timeout(timepoint);
    if (wifi_send_timeout(cmd, timeout) != 0) {
        wifi_release_mutex();
        return -1;
    }

    timeout = sys_timepoint_timeout(timepoint);
    ret     = wifi_wait_for(wait_msgs, timeout);

    wifi_release_mutex();
    return ret;
}

static char ownerThread[40];
static char ownerInfo[100];
//////////////////////////////////////////////////////////
//	wifi_get_mutex()
//  Get the mutex for the wifi driver
//	@param timeout - timeout for the lock
//	@param oInfo - info about the caller (usually the function)
//
//	@return - 0 on success, -EBUSY if the mutex is already locked
int wifi_get_mutex(k_timeout_t timeout, const char *oInfo)
{
    int ret = k_mutex_lock(&DA_mutex, timeout);

    if (ret == 0) {
        if (DA_mutex.lock_count == 1) {
            strncpy(ownerThread, k_thread_name_get(k_current_get()), 39);
            ownerThread[39] = 0;
            strncpy(ownerInfo, oInfo, 99);
            ownerInfo[99] = 0;
        }
    } else {
        LOG_WRN(
            "Thread '%s'(%s,%p) failed to get wifi Mutex. Current owner %s (%s)",
            k_thread_name_get(k_current_get()),
            oInfo,
            k_current_get(),
            ownerThread,
            ownerInfo);
    }

    return ret;
}

//////////////////////////////////////////////////////////
//	wifi_release_mutex()
//  Release the mutex for the wifi driver
void wifi_release_mutex()
{
    k_mutex_unlock(&DA_mutex);
    // LOG_DBG("Mutex released, LC=%d", DA_mutex.lock_count);
}

//////////////////////////////////////////////////////////
// wifi_get_mutex_count()
//
// Return the lock count of the mutex
//
// @return - the lock count
int wifi_get_mutex_count()
{
    return DA_mutex.lock_count;
}

////////////////////////////////////////////////////////////////////////////////
// wstrerr()
//
// Return a string representation of an error code
//
//  @param err - the error code
//
//  @return : a string representation of the error code
typedef struct
{
    int         err;
    const char *str;
} aerrs_t;
static aerrs_t aerrs[] = { { 801, "SSID invalid" },
                           { 802, "SSID bad PW" },
                           { 803, "SSID bad Sec" },
                           { 804, "SSID bad KeyIdx" },
                           { 805, "SSID bad Enc" },
                           { 806, "SSID bad Hidden" },
                           { 807, "SSID bad Safe" },
                           { 808, "SSID bad WPA type" },
                           { 809, "SSID bad WPA rng" },
                           { 810, "SSID bad Enc rng" },
                           { 811, "SSID bad Safe rng" },
                           { 812, "SSID Idx empty" },
                           { 813, "SSID bad Idx" },
                           { 814, "SSID Idx in use" },
                           { 815, "SSID Hid save err" },
                           { 816, "SSID Key save err" },
                           { 817, "SSID Enc save err" },
                           { 818, "SSID Aut save err" },
                           { 819, "SSIDLIST save err" },
                           { 820, "SSIDLIST Mem err" },
                           { 821, "SSIDLIST Decry err" },
                           { 822, "SSIDLIST Encry err" },
                           { 823, "SSIDLIST Crypto mismatch" },
                           { 824, "SSIDLIST Crypto verify err" } };

const char *wstrerr(int err)
{
    switch (err) {
    case 0:
        return "OK";
    case EAGAIN:
        return "Timeout";
    case EBUSY:
        return "Mutex busy";
    case EBADE:
        return "Error response";
    case ENXIO:
        return "DA is sleeping";
    case EINVAL:
        return "Invalid argument";
    case ENOMEM:
        return "Out of memory";
    case EFAULT:
        return "No saved SSID found to connect to";
    case ENOTCONN:
        return "MQTT not connected";
    case EALREADY:
        return "Too soon to wake DA";
    case EFBIG:
        return "Msg is too large fo the DA16200";
    case 634:
        return "DA reports MQTT is not active";
    case ENODEV:
        return "DA or LTE not powered";
    default:
        for (int i = 0; i < sizeof(aerrs) / sizeof(aerrs_t); i++) {
            if (aerrs[i].err == err) {
                return aerrs[i].str;
            }
        }
        return strerror(err);
    }
}
