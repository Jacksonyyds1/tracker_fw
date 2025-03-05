/*
 * Copyright (c) 2023 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/zbus/zbus.h>

#include "message_channel.h"

//#define FORMAT_STRING "Hello MQTT! Current uptime is: %d"
#define FORMAT_STRING "{\"P\":5,\"MID\":\"manu_nrf9160_test\",\"MK\":\"US\",\"B\":35,\"T\":1,\"M\":{\"BATT\":\"%f\"}}"
/* Register log module */
LOG_MODULE_REGISTER(sampler, CONFIG_MQTT_SAMPLE_SAMPLER_LOG_LEVEL);

/* Register subscriber */
ZBUS_SUBSCRIBER_DEFINE(sampler, CONFIG_MQTT_SAMPLE_SAMPLER_MESSAGE_QUEUE_SIZE);

static float last_battery_level = 0.0;

static void sample(void)
{
    struct payload payload = { 0 };
    //uint32_t uptime = k_uptime_get_32();
    int err, len;

    /* The payload is user defined and can be sampled from any source.
     * Default case is to populate a string and send it on the payload channel.
     */

    len = snprintk(payload.string, sizeof(payload.string), FORMAT_STRING, last_battery_level);
    if ((len < 0) || (len >= sizeof(payload))) {
        LOG_ERR("Failed to construct message, error: %d", len);
        SEND_FATAL_ERROR();
        return;
    }

    //LOG_DBG("payload.string: %s", payload.string);

    err = zbus_chan_pub(&PAYLOAD_CHAN, &payload, K_SECONDS(1));
    if (err) {
        LOG_ERR("zbus_chan_pub, error:%d", err);
        SEND_FATAL_ERROR();
    }
}

static void sampler_task(void)
{
    const struct zbus_channel *chan;
    while (!zbus_sub_wait(&sampler, &chan, K_FOREVER)) {
        if (&TRIGGER_CHAN == chan) {
            float batt_level = 0.0;
            int err;

            err = zbus_chan_read(&TRIGGER_CHAN, &batt_level, K_SECONDS(1));
            if (err) {
                LOG_ERR("zbus_chan_read, error: %d", err);
                SEND_FATAL_ERROR();
                return;
            }
            // filter less than 0 values as they are from trigger.c and will be removed
            if (batt_level > 0.0) {
                last_battery_level = batt_level;
                continue;
            }
            sample();
        }
    }
}

K_THREAD_DEFINE(sampler_task_id,
        CONFIG_MQTT_SAMPLE_SAMPLER_THREAD_STACK_SIZE,
        sampler_task, NULL, NULL, NULL, 3, 0, 0);
