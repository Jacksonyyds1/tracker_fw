
//  basic LTE connection management here

/*
 * Copyright (c) 2023 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */
#include "transport.h"
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/zbus/zbus.h>
#include <zephyr/smf.h>
#include <net/mqtt_helper.h>

#include "zbus_msgs.h"
#include "network.h"
#include "status.h"
#include <string.h>
#include "fota.h"
#include <cJSON.h>
#include <nrf_modem_at.h>
#include <curl/curl.h>
#include "modem_interface_types.h"  // from c_modules/modem/include so its shared with the 5340
#include "wi.h"
//#include "gps.h"

/* Register log module */
LOG_MODULE_REGISTER(mqtt_client, 4);

static void transport_listener(const struct zbus_channel *chan);
ZBUS_LISTENER_DEFINE(mqtt_client, transport_listener);

typedef struct mqtt_pub_work_info {
    workref_t *mqtt_work;
    mqtt_payload_t* mqtt_msg;
} mqtt_pub_work_info_t;


#define TRANSPORT_CLIENT_ID_BUFFER_SIZE 64

/* ID for subscribe topic - Used to verify that a subscription succeeded in on_mqtt_suback(). */
#define SUBSCRIBE_TOPIC_ID 2469

/* Forward declarations */
static volatile bool mqtt_settings_initialized = false;
static volatile bool mqtt_sub_topics_initialized = false;
static volatile bool mqtt_client_connect = false;
static volatile bool transport_radio_connected = false;

char iot_server_name[CONFIG_PURINA_D1_LTE_IOT_BROKER_HOST_NAME_SIZE_MAX] = "";
int iot_server_port = 0;
char iot_client_id[CONFIG_PURINA_D1_LTE_CLIENT_ID_SIZE_MAX] = "";

static const struct smf_state state[];
static void connect_work_fn(struct k_work *work);

/* Define connection work - Used to handle reconnection attempts to the MQTT broker */
static K_WORK_DELAYABLE_DEFINE(connect_work, connect_work_fn);

/* Define stack_area of application workqueue */
K_THREAD_STACK_DEFINE(stack_area, 1024);
K_THREAD_STACK_DEFINE(mqtt_stack_area, 2048);

/* Declare application workqueue. This workqueue is used to call mqtt_helper_connect(), and
 * schedule reconnectionn attempts upon network loss or disconnection from MQTT.
 */
static struct k_work_q transport_queue;
static struct k_work_q mqtt_queue;

/* Internal states */
enum module_state { MQTT_CONNECTED, MQTT_DISCONNECTED };

typedef struct subTopic {
	uint8_t topic[CONFIG_PURINA_D1_LTE_JSON_TOPIC_SIZE_MAX];
	uint8_t qos;
} subTopic_t;

static subTopic_t subTopics[CONFIG_PURINA_D1_LTE_CLIENT_MAX_SUBSCRIPTION_TOPICS];
static int subTopicsCount = 0;

/* User defined state object.
 * Used to transfer data between state changes.
 */
static struct s_object {
	/* This must be first */
	struct smf_ctx ctx;

	/* Last channel type that a message was received on */
	const struct zbus_channel *chan;

	/* Network status */
	enum network_status status;

} s_obj;

K_MSGQ_DEFINE(incoming_mqtt, sizeof(inc_mqtt_event_t), 10, 1);

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

/* Callback handlers from MQTT helper library.
 * The functions are called whenever specific MQTT packets are received from the broker, or
 * some library state has changed.
 */
static void on_mqtt_connack(enum mqtt_conn_return_code return_code)
{
	ARG_UNUSED(return_code);
	smf_set_state(SMF_CTX(&s_obj), &state[MQTT_CONNECTED]);
	config_set_mqtt_connected(true);
}

static void on_mqtt_disconnect(int result)
{
	ARG_UNUSED(result);
	smf_set_state(SMF_CTX(&s_obj), &state[MQTT_DISCONNECTED]);
	config_set_mqtt_connected(false);
}

int transport_allow_mqtt_connect(bool allow) {
	if (allow) {
		LOG_INF("Allowing MQTT connection: %d", allow);
		mqtt_client_connect	= true;
		config_set_mqtt_enabled(true);
		k_work_reschedule_for_queue(&transport_queue, &connect_work, K_NO_WAIT);
	}
	else {
		LOG_INF("Disallowing MQTT connection: %d", allow);
		mqtt_client_connect = false;
		config_set_mqtt_enabled(false);
		// disable the connection
		if (mqtt_helper_disconnect() == 0) { // this will call on_mqtt_dis
			LOG_INF("Disconnected from MQTT broker");
			config_set_mqtt_connected(false);
		}
		else {
			LOG_ERR("Failed to disconnect from MQTT broker");
		}
	}

	if (mqtt_settings_initialized && mqtt_sub_topics_initialized) {
		config_set_mqtt_initialized(true);
	}
	return 0;
}

int transport_set_settings(const char *settingsJsonStr)
{
	cJSON *json = cJSON_Parse(settingsJsonStr);
	if (json == NULL) {
		LOG_ERR("Failed to parse settings JSON");
		return -1;
	}
	const cJSON *host = cJSON_GetObjectItemCaseSensitive(json, "host");
    if (cJSON_IsString(host) && (host->valuestring != NULL)) {
        printf("MQTT Hostname: %s\n", host->valuestring);
		memcpy(iot_server_name, host->valuestring, strlen(host->valuestring));
    }
	const cJSON *port = cJSON_GetObjectItemCaseSensitive(json, "port");
    if (cJSON_IsNumber(port)) {
        printf("MQTT Host port: %d\n", port->valueint);
		iot_server_port = port->valueint;
    }
	const cJSON *client_id = cJSON_GetObjectItemCaseSensitive(json, "client_id");
    if (cJSON_IsString(client_id) && (client_id->valuestring != NULL)) {
        printf("My device ID: %s\n", client_id->valuestring);
		memcpy(iot_client_id, client_id->valuestring, strlen(client_id->valuestring));
    }

	cJSON_Delete(json);
	mqtt_settings_initialized = true;
	if (mqtt_settings_initialized && mqtt_sub_topics_initialized) {
		config_set_mqtt_initialized(true);
	}
	return 0;
}

int transport_set_subscription_topics(const char *topicsJsonStr)
{
	cJSON *json = cJSON_Parse(topicsJsonStr);
	if (json == NULL) {
		LOG_ERR("Failed to parse settings JSON");
		return -1;
	}

	const cJSON *topics = cJSON_GetObjectItemCaseSensitive(json, "topics");
	if (cJSON_IsArray(topics)) {
		cJSON *topic = NULL;
		//int num_topics = cJSON_GetArraySize(topics);
		int i = 0;
		subTopicsCount = 0;

		cJSON_ArrayForEach(topic, topics) {
			cJSON *topic_str = cJSON_GetObjectItemCaseSensitive(topic, "topic");
			cJSON *qos = cJSON_GetObjectItemCaseSensitive(topic, "qos");
			if (cJSON_IsString(topic_str) && cJSON_IsNumber(qos)) {
				LOG_DBG("Saving subscription topic: %s qos: %d", topic_str->valuestring, qos->valueint);
				memset(subTopics[i].topic, 0, CONFIG_PURINA_D1_LTE_JSON_TOPIC_SIZE_MAX);
				if (strlen(topic_str->valuestring) > CONFIG_PURINA_D1_LTE_JSON_TOPIC_SIZE_MAX) {
					LOG_ERR("Topic too long");
					continue;
				}
				memcpy(subTopics[i].topic, topic_str->valuestring, strlen(topic_str->valuestring));
				subTopics[i].qos = qos->valueint;
				i++;
				subTopicsCount++;
			}
		}
	}

	mqtt_sub_topics_initialized = true;
	if (mqtt_settings_initialized && mqtt_sub_topics_initialized) {
		config_set_mqtt_initialized(true);
	}
	cJSON_Delete(json);
	return 0;
}


///////////////////////////////
///
///     pub_zbus_mqtt_message
///
void pub_zbus_mqtt_message(struct k_work *work) {
	inc_mqtt_event_t mqtt_event = {0};
	while (k_msgq_get(&incoming_mqtt, &mqtt_event, K_NO_WAIT) == 0) {
		LOG_DBG("Publishing message on topic: \"%.*s\" \r\n\t- \"%.*s\"", mqtt_event.topic_length,
			mqtt_event.topic,
			mqtt_event.msg_length,
			mqtt_event.mqtt_msg
			);


		int err = zbus_chan_pub(&MQTT_CLOUD_TO_DEV_MESSAGE, &mqtt_event, K_SECONDS(1));
		if (err) {
			LOG_ERR("zbus_chan_pub, error:%d", err);
			k_free(mqtt_event.mqtt_msg);
			k_free(mqtt_event.topic);
			SEND_FATAL_ERROR();
		}
	}
}
K_WORK_DEFINE(my_zbus_pub_work, pub_zbus_mqtt_message);


///////////////////////////////
///
///     on_mqtt_from_cloud
///
static void on_mqtt_from_cloud(struct mqtt_helper_buf topic, struct mqtt_helper_buf payload)
{
	LOG_INF("Received payload: %.*s on topic: %.*s", payload.size,
			payload.ptr,
			topic.size,
			topic.ptr);

	if (payload.size == 0) {
		LOG_WRN("Received empty payload");
		return;
	}

	if (topic.size == 0) {
		LOG_WRN("Received empty topic");
		return;
	}

	char* msg = k_malloc(payload.size);  // k_free'd by spis_interface when sent to 5340
	if (msg == NULL) {
		LOG_ERR("Failed to allocate memory for payload");
		return;
	}
	memcpy(msg, payload.ptr, payload.size);


	char* topic_str = k_malloc(topic.size);   // k_free'd by spis_interface when sent to 5340
	if (topic_str == NULL) {
		LOG_ERR("Failed to allocate memory for topic");
		if(msg) k_free(msg);    // free previous malloc since this event is not happening
		return;
	}
	memcpy(topic_str, topic.ptr, topic.size);

	inc_mqtt_event_t event;
	event.mqtt_msg = msg;
	event.msg_length = payload.size;
	event.topic_length = topic.size;
	event.topic = topic_str;

	if (k_msgq_put(&incoming_mqtt, (void *)&event, K_NO_WAIT) != 0) {
		LOG_ERR("Failed to put message in queue");
		if(msg) k_free(msg);	// free previous malloc since this event is not happening
		if(topic_str) k_free(topic_str);	// free previous malloc since this event is not happening
	}

	k_work_submit_to_queue(&mqtt_queue, &my_zbus_pub_work);
}

///////////////////////////////
///
///     on_mqtt_suback
///
static void on_mqtt_suback(uint16_t message_id, int result)
{
	if ((message_id == SUBSCRIBE_TOPIC_ID) && (result == 0)) {
		LOG_INF("Subscribed to topic");
	} else if (result) {
		LOG_ERR("Topic subscription failed, error: %d", result);
	} else {
		LOG_WRN("Subscribed to unknown topic, id: %d", message_id);
	}
}



///////////////////////////////
///
///     publish
///
void publish(mqtt_payload_t *payload)
{
	int err;

	struct mqtt_publish_param param = {
		.message.payload.data = payload->string,
		.message.payload.len = payload->string_length,
		.message.topic.qos = payload->qos,
		.message_id = (uint16_t) k_uptime_get_32(),
		.message.topic.topic.size = payload->topic_length,
		.message.topic.topic.utf8 = payload->topic,
	};

	if (payload->string == NULL) {
		LOG_ERR("Payload is NULL");
		return;
	}

	err = mqtt_helper_publish(&param);
	if (err) {
		LOG_WRN("Failed to send payload, err: %d", err);
		return;
	}


	LOG_INF("Published message on topic: %s", (char *)param.message.topic.topic.utf8);

	spi_msg_response_t mqtt_response;
	mqtt_response.msgHandle = payload->msgHandle;
	mqtt_response.msgStatus = err;
	mqtt_response.length = 0;
	mqtt_response.data = NULL;
	err = zbus_chan_pub(&SPI_MSG_RESPONSE, &mqtt_response, K_SECONDS(1));
	if (err) {
		LOG_ERR("zbus_chan_pub, error:%d", err);
	}

}


///////////////////////////////
///
///     publish_work_fn
///
void publish_work_fn(struct k_work *work) {
    workref_t            *wr   = CONTAINER_OF(work, workref_t, work);
    mqtt_pub_work_info_t *info = (mqtt_pub_work_info_t *)wr->reference;
    mqtt_payload_t       *msg  = info->mqtt_msg;


	publish(msg);
	if (msg->string) {
		k_free(msg->string);
		msg->string = NULL;
	}
	if (msg->topic) {
		k_free(msg->topic);
		msg->topic = NULL;
	}
	k_free(msg);
	k_free(info);
	wr_put(wr);
}

///////////////////////////////
///
///     subscribe
///
static void subscribe(void)
{
	int err;
	struct mqtt_topic *topics = k_malloc(sizeof(struct mqtt_topic) * subTopicsCount);
	if (topics == NULL) {
		LOG_ERR("Failed to allocate memory for topics");
		return;
	}
	for(int i=0; i<subTopicsCount; i++) {
		topics[i].topic.utf8 = subTopics[i].topic;
		topics[i].topic.size = strlen(subTopics[i].topic);
		topics[i].qos = subTopics[i].qos;
	}

	struct mqtt_subscription_list list = {
		.list = topics,
		.list_count = subTopicsCount,
		.message_id = SUBSCRIBE_TOPIC_ID,
	};

	for (size_t i = 0; i < list.list_count; i++) {
		LOG_INF("Subscribing to(%d): %s", list.list[i].topic.size, (char *)list.list[i].topic.utf8);
	}

	err = mqtt_helper_subscribe(&list);
	if (err) {
		LOG_ERR("Failed to subscribe to topics, error: %d", err);
		k_free(topics);
		return;
	}

	//k_free(topics);
}


///////////////////////////////
///
///     connect_work_fn
///
///  Connect work - Used to establish a connection to the MQTT broker and schedule reconnection attempts.
static void connect_work_fn(struct k_work *work)
{
	ARG_UNUSED(work);

	LOG_WRN("Connecting to MQTT broker");

	if (!transport_radio_connected) {
		LOG_WRN("Waiting for network connection");
		return;
	}

	// // TODO: send from 5340
	// // MODEM_GNSS_LNA_COMMAND = at%XCOEX0=1,1,1570,1580
	// char atCommand[] = "AT\%XCOEX0=1,1,1570,1580";
	// char response[64];
	// int at_err = nrf_modem_at_cmd(response, sizeof(response), "%s", atCommand);
	// if (at_err) {
	// 	LOG_ERR("Failed to send AT command: %s", atCommand);
	// }

	int err;
	bool spam_prevention = false;
	if (mqtt_settings_initialized && mqtt_sub_topics_initialized && mqtt_client_connect) {

		LOG_DBG("Client ID: %s", iot_client_id);

		struct mqtt_helper_conn_params conn_params = {
			.hostname.ptr = iot_server_name,
			.hostname.size = strlen(iot_server_name),
			.device_id.ptr = iot_client_id,
			.device_id.size = strlen(iot_client_id),
		};


		err = mqtt_helper_connect(&conn_params);
		if (err) {
			if (err == 2) {  // cont find the error code table for this, hand verified to be 2
				LOG_CLOUD_ERR(MODEM_ERROR_GETADDR_ERROR, "9160 MQTT Connect: Failed to resolve hostname");
			}
			else if (err == CURLE_REMOTE_ACCESS_DENIED) {
				LOG_CLOUD_ERR(MODEM_ACCESS_DENIED, "9160 MQTT Connect: access denied");
			}
			else {
				char err_str[64];
				snprintf(err_str, sizeof(err_str), "MQTT Connect: error code: %d", err);
				LOG_CLOUD_ERR(MODEM_ERROR_UNKNOWN, err_str);
			}
		}
	}
	else {
		if (!mqtt_settings_initialized) {
			LOG_WRN("Waiting for MQTT settings to be initialized");
		}
		else if (!mqtt_sub_topics_initialized) {
			LOG_WRN("Waiting for MQTT subscription topics to be initialized");
		}
		else {
			// this can print a LOT while waiting for 5340 to tell us to turn on the connection
			if (!spam_prevention) {
				LOG_WRN("Waiting for MQTT client to be enabled");
				spam_prevention = true;
			}
		}
	}

	k_work_reschedule_for_queue(&transport_queue, &connect_work,
		K_SECONDS(CONFIG_PURINA_D1_LTE_MQTT_RECONNECTION_TIMEOUT_SECONDS));
}


///////////////////////////////
///
///     connect_work_fn
///
/// Function executed when the module enters the disconnected from MQTT state.
static void disconnected_entry(void *o)
{
	struct s_object *user_object = o;
	config_set_mqtt_connected(false);
	/* Reschedule a connection attempt if we are connected to network and we enter the
	 * disconnected state.
	 */
	if (user_object->status == NETWORK_CONNECTED) {
		LOG_WRN("Network connected, retrying connection to MQTT (if not already connected)");
		k_work_reschedule_for_queue(&transport_queue, &connect_work, K_NO_WAIT);
	}
}

/* Function executed when the module is in the disconnected state. */
static void disconnected_run(void *o)
{
	struct s_object *user_object = o;
	config_set_mqtt_connected(false);
	if ((user_object->status == NETWORK_DISCONNECTED) && (user_object->chan == &NETWORK_CHAN)) {
		/* If NETWORK_DISCONNECTED is received after the MQTT connection is closed,
		 * we cancel the connect work if it is onging.
		 */
		LOG_WRN("Network disconnected, cancelling connection to MQTT");
		k_work_cancel_delayable(&connect_work);
	}

	if ((user_object->status == NETWORK_CONNECTED)) {

		/* Wait for 5 seconds to ensure that the network stack is ready before
		 * attempting to connect to MQTT. This delay is only needed when building for
		 * Wi-Fi.
		 */
		LOG_WRN("Network connected, retrying connection to MQTT (if not already connected)");
		k_work_reschedule_for_queue(&transport_queue, &connect_work, K_NO_WAIT);
	}
}


///////////////////////////////
///
///     connected_entry
///
/// Function executed when the module enters the connected to MQTT state.
static void connected_entry(void *o)
{
	config_set_mqtt_connected(true);
	LOG_INF("Connected to MQTT broker");
	LOG_INF("Hostname: %s", iot_server_name);
	LOG_INF("Client ID: %s", iot_client_id);
	LOG_INF("Port: %d", CONFIG_MQTT_HELPER_PORT);
	LOG_INF("TLS: %s", IS_ENABLED(CONFIG_MQTT_LIB_TLS) ? "Yes" : "No");
	//config_set_mqtt_working(true);
	ARG_UNUSED(o);

	/* Cancel any ongoing connect work when we enter connected state */
	k_work_cancel_delayable(&connect_work);

	subscribe();
}


///////////////////////////////
///
///     connected_run
///
/// Function executed when the module is in the connected to MQTT state.
static void connected_run(void *o)
{
	struct s_object *user_object = o;
	if (((user_object->status == NETWORK_DISCONNECTED) || (user_object->status == NETWORK_AIRPLANE_MODE_ON))) {
		/* Explicitly disconnect the MQTT transport when losing network connectivity.
		 * This is to cleanup any internal library state.
		 * The call to this function will cause on_mqtt_disconnect() to be called.
		 */
		(void)mqtt_helper_disconnect();
		return;
	}
	config_set_mqtt_connected(true);
}


///////////////////////////////
///
///     connected_exit
///
/// Function executed when the module exits the connected state.
static void connected_exit(void *o)
{
	ARG_UNUSED(o);
	config_set_mqtt_connected(false);
	LOG_INF("Disconnected from MQTT broker");
}

/* Construct state table */
static const struct smf_state state[] = {
	[MQTT_DISCONNECTED] = SMF_CREATE_STATE(disconnected_entry, disconnected_run, NULL),
	[MQTT_CONNECTED] = SMF_CREATE_STATE(connected_entry, connected_run, connected_exit),
};


static void transport_listener(const struct zbus_channel *chan){
			if (&NETWORK_CHAN == chan) {
				bool do_state_change = false;
				const enum network_status *status = zbus_chan_const_msg(chan); // Direct message access
				LOG_WRN("Network status: %d", *status);
				if (*status == NETWORK_CONNECTED) {
					transport_radio_connected = true;
					LOG_DBG("Network status: connected");
					s_obj.status = *status;
					do_state_change = true;
				}
				else if (*status == NETWORK_DISCONNECTED) {
					transport_radio_connected = false;
					LOG_DBG("Network status: disconnected");
					s_obj.status = *status;
					do_state_change = true;
				}
				else if (*status == NETWORK_INITIALIZING) {
					LOG_DBG("Network status: initializing");
				}
				else if (*status == NETWORK_CELL_CHANGED) {
					LOG_DBG("Network status: cell changed");
					s_obj.status = NETWORK_CONNECTED;
					do_state_change = true;
				}
				else if (*status == NETWORK_CELL_NEIGHBORS_CHANGED) {
					LOG_DBG("Network status: cell neighbors changed");
					s_obj.status = NETWORK_CONNECTED;
					do_state_change = true;
				}
				else if (*status == NETWORK_AIRPLANE_MODE_ON) {
					LOG_DBG("Network status: airplane mode on");
					s_obj.status = NETWORK_DISCONNECTED;
					do_state_change = true;
				}
				else if (*status == NETWORK_AIRPLANE_MODE_OFF) {
					LOG_DBG("Network status: airplane mode off");
					s_obj.status = NETWORK_CONNECTED;
					do_state_change = true;
				}
				else {
					LOG_DBG("Network status: unknown");
				}

				if (do_state_change) {
					int err = smf_run_state(SMF_CTX(&s_obj));
					if (err) {
						LOG_ERR("smf_run_state, error: %d", err);
						SEND_FATAL_ERROR();
						return;
					}
				}
			}

			if (chan == &MQTT_DEV_TO_CLOUD_MESSAGE) {

				const mqtt_payload_t *payload= zbus_chan_const_msg(chan); // Direct message access
				LOG_WRN("MQTT_DEV_TO_CLOUD_MESSAGE: %.*s", payload->topic_length, payload->topic);

				mqtt_pub_work_info_t *info = k_malloc(sizeof(mqtt_pub_work_info_t));
				if (info == NULL) {
					LOG_ERR("Out of memory for handling incoming work");
					return;
				}

				mqtt_payload_t *msg = k_malloc(sizeof(mqtt_payload_t));
				if (msg == NULL) {
					LOG_ERR("Out of memory for handling incoming work");
					k_free(info);
					return;
				}
				msg->qos = payload->qos;
				msg->topic_length = payload->topic_length;
				msg->string_length = payload->string_length;
				msg->topic = payload->topic;
				msg->string = payload->string;
				msg->msgHandle = payload->msgHandle;

				info->mqtt_msg = msg;

				info->mqtt_work = wr_get(info, __LINE__);
				k_work_init(&info->mqtt_work->work, publish_work_fn);
				k_work_submit_to_queue(&mqtt_queue, &info->mqtt_work->work);
			}
}

///////////////////////////////
///
///     transport_task
///
void transport_init(void)
{
	int err;
	//const struct zbus_channel *chan;


	struct mqtt_helper_cfg cfg = {
		.cb = {
			.on_connack = on_mqtt_connack,
			.on_disconnect = on_mqtt_disconnect,
			.on_publish = on_mqtt_from_cloud,
			.on_suback = on_mqtt_suback,
		},
	};

	/* Initialize and start application workqueue.
	 * This workqueue can be used to offload tasks and/or as a timer when wanting to
	 * schedule functionality using the 'k_work' API.
	 */
	k_work_queue_init(&transport_queue);
	k_work_queue_start(&transport_queue, stack_area,
		K_THREAD_STACK_SIZEOF(stack_area),
		K_HIGHEST_APPLICATION_THREAD_PRIO,
		NULL);

	k_work_queue_init(&mqtt_queue);
	k_work_queue_start(&mqtt_queue, mqtt_stack_area,
		K_THREAD_STACK_SIZEOF(mqtt_stack_area),
		K_HIGHEST_APPLICATION_THREAD_PRIO+1,
		NULL);


	err = mqtt_helper_init(&cfg);
	if (err) {
		LOG_ERR("mqtt_helper_init, error: %d", err);
		SEND_FATAL_ERROR();
		return;
	}

	/* Set initial state */
	smf_set_initial(SMF_CTX(&s_obj), &state[MQTT_DISCONNECTED]);

}

