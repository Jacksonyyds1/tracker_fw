/* Copyright (c) 2024, Nestle Purina Pet Care. All rights reserved */
/****************************************************************
 *  wifi.h
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
#pragma once
#include <stdlib.h>
#include <stdio.h>
#include <stddef.h>
#include <zephyr/kernel.h>

// The DA can send about 2K of data for large message but most will much
// smaller. The wifi_spi will store a wifi_msg_t at the end of this buffer
// so it needs to be at least sizeof(wifi_msg_t)
#define WIFI_MSG_SIZE 2100

// When the receive queue has old messages in them that aren't yet
// processed, they take memory.  We clear out old messages when we
// can't get memory for a new message.  Messages younger then
// WIFI_FLUSH_AGE (in ms) will not be flushed
#define WIFI_FLUSH_AGE (2000)

#define WIFI_LEVEL_SHIFTER_PIN (14)
#define WIFI_3V0_PIN           (4)
#define WIFI_POWER_KEY_PIN     (28)
#define WIFI_WAKEUP_PIN        (8)

// wifi_msg_t
// The msg structs are expected to
// be copyable so need to be just data, but the data
// fields is a allocated pointer that needs to be freed.
typedef struct wifi_msg
{
    uint64_t timestamp;    // When the message was created
    int32_t  ref_count;
    uint8_t  incoming;    // 1 if from the DA, 0 if to the DA
    uint16_t data_len;
    uint8_t *data;
} wifi_msg_t;

#define OUT_OF_TIME(to, tp) (K_TIMEOUT_EQ((to = sys_timepoint_timeout(tp)), K_NO_WAIT))

// True if the DA was recently powered on
extern bool g_DA_needs_one_time_config;

// This holds the last time we triggered a wake up of the DA
// This is used to determine if the INIT:DONE we get is from
// a wake up or a power cycle
extern uint64_t g_last_wake_time;

#define WIFI_CB_IGNORED            0
#define WIFI_CB_CONSUMED_PLS_FREE  1
#define WIFI_CB_CONSUMED_DONT_FREE 2

// The callback function will be called when data is both sent
// or received from the DA.  It allows us to monitor the flow
// of message to keep a local state machine for the DA.
//
// The message passed in is the next message to be sent or just
// received and not yet put into a msg queue yet.
// If the callback wants to keep the packet around it should'
// call wifi_inc_ref_found()
typedef void (*wifi_tx_rx_cb_t)(wifi_msg_t *msg, void *user_data);

//////////////////////////////////////////////////////////
// wifi_init()
//
// Initialize the wifi module and the underlying hardware
//
// @return - 0 on success, -1 on error
int wifi_init();

//////////////////////////////////////////////////////////
//	wifi_get_mutex()
//  Get the mutex for the wifi driver
//	@param timeout - timeout for the lock
//	@param oInfo - info about the caller (usually the function)
//
//	@return - 0 on success, -EBUSY if the mutex is already locked
int wifi_get_mutex(k_timeout_t timeout, const char *oInfo);

//////////////////////////////////////////////////////////
//	wifi_release_mutex()
//  Release the mutex for the wifi driver
void wifi_release_mutex();

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
int wifi_recv(wifi_msg_t *msg, k_timeout_t timeout);

//////////////////////////////////////////////////////////
// wifi_init_new_msg()
//
// Initialize a msg structure and allocate memory for the
// data part of that structure.
//
// Implementation note: The msg structs are expected to
// be copyable so need to be just data, but the data
// fields is a allocated pointer that needs to be freed.
int wifi_init_new_msg(wifi_msg_t *msg, bool incoming, int buf_size);

//////////////////////////////////////////////////////////
//	Increment the reference count of a message.
//
// @param msg  - pointer to the wifi_msg_t structure that
//               will be filled on success
//
// @return - 0 on success,
int wifi_inc_ref_count(wifi_msg_t *msg);

//////////////////////////////////////////////////////////
// wifi_msg_free()
//
// Free the memory allocated for a wifi_msg_t
// Call this on the data field of a wifi_msg_t
// as the wifi_msg_t itself is managed by the
// queue
// @param msg - pointer to a wifi_msg_t
//              or what was returned by wifi_msg_alloc()
void wifi_msg_free(wifi_msg_t *msg);

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
int wifi_send_timeout(char *data, k_timeout_t timeout);

//////////////////////////////////////////////////////////
//  wifi_flush_msgs()
//
//  Flush all messages from the receive queue
void wifi_flush_msgs();

// Define the number of callbacks that can be registered for
// when a message is received
#define WIFI_NUM_RX_CB 5

//////////////////////////////////////////////////////////
// wifi_add_tx_rx_cb
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
int wifi_add_tx_rx_cb(wifi_tx_rx_cb_t cb, void *user_data);

//////////////////////////////////////////////////////////
// wifi_rem_tx_rx_cb
//
// Remove a callback that was added with wifi_spi_add_tx_rx_cb
//
// @param id - id returned from wifi_spi_add_tx_rx_cb
void wifi_rem_tx_rx_cb(int id);

// Internal function - not used outside of wifi_spi or uart.c
int wifi_notify_cbs(wifi_msg_t *msg);

//////////////////////////////////////////////////////////
//	Requeue a message that was recv'd
//
// @param msg  - a wifi_msg_t that was obtained
//               from wifi_recv()
//
// @note caller can call this instead of wifi_msg_free()
void wifi_requeue(wifi_msg_t *msg);

#define WIFI_MAX_WAIT_MSGS       5
#define WIFI_MAX_WAIT_MSG_PARAMS 5

typedef struct wifi_wait_array
{
    uint8_t num_msgs;
    char   *msgs[WIFI_MAX_WAIT_MSGS];
    uint8_t num_params_per_msg[WIFI_MAX_WAIT_MSGS];
    char   *param_ptrs[WIFI_MAX_WAIT_MSGS][WIFI_MAX_WAIT_MSG_PARAMS];
    bool    stop_waiting[WIFI_MAX_WAIT_MSGS];
    int     num_matched[WIFI_MAX_WAIT_MSGS];
} wifi_wait_array_t;

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
void wifi_add_wait_msg(wifi_wait_array_t *wait_msgs, char *msg, bool stop_waiting, int num_params, ...);

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
int wifi_wait_for(wifi_wait_array_t *wait_msgs, k_timeout_t timeout);

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
int wifi_send_and_wait_for(char *cmd, wifi_wait_array_t *wait_msgs, k_timeout_t timeout);

int wifi_msg_cnt();
int wifi_peek_msg(wifi_msg_t *msg, int index);

int  wifi_1v8_on();
int  wifi_1v8_off();
bool wifi_get_1v8();

//////////////////////////////////////////////////////////
//  wifi_set_level_shifter()
//  Change the state of the level shifter for the wifi
//
// @param new_state - true to turn on the level shifter,
//
// @return - 0 on success, -1 on error
int wifi_set_level_shifter(bool new_state);

int  wifi_set_power_key(bool newState);
int  wifi_set_3v0_enable(bool newState);
bool wifi_get_3v0();
int  wifi_set_wakeup(bool newState);

//////////////////////////////////////////////////////////
// wifi_get_power_key
//
// Return the state of the power key
//
//  @return - true if da is on, false otherwise
bool wifi_get_power_key();

////////////////////////////////////////////////////////////
// wifi_wake_DA()
//
// try to wake up the DA by pulsing the WAKEUP_RTC line
// The DA wakes on the falling edge of WAKEUP_RTC so if
// it is already low, we need to pulse it high first.
//
// @param ms_delay - delay in ms after the pulse

// @return - none
void wifi_wake_DA(int ms_delay);

//////////////////////////////////////////////////////////
// wifi_reset()
//  Power cycle the DA16200
void wifi_reset();

////////////////////////////////////////////////////////////////////////////////
// wstrerr()
//
// Return a string representation of an error code
//
//  @param err - the error code
//
//  @return : a string representation of the error code
const char *wstrerr(int err);

//////////////////////////////////////////////////////////
// wifi_get_mutex_count()
//
// Return the lock count of the mutex
//
// @return - the lock count
int wifi_get_mutex_count();
