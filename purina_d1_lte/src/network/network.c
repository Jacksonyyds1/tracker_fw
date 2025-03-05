/*
 * Copyright (c) 2023 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/zbus/zbus.h>
#include <zephyr/net/conn_mgr_connectivity.h>
#include <zephyr/net/conn_mgr_monitor.h>
#include <hw_id.h>
#include "network.h"
#include "transport.h"
#include "zbus_msgs.h"
#include "status.h"
#include <nrf_socket.h>
#include <modem/lte_lc.h>
#include <modem/nrf_modem_lib.h>
#include <nrf_modem_at.h>
#include <net/download_client.h>
#include <zephyr/sys/crc.h>
/* Register log module */
LOG_MODULE_REGISTER(network, 4);

ZBUS_SUBSCRIBER_DEFINE(network_client, 10);

extern bool disable_modem;
static bool airplane_mode = false;
static bool lte_mode = 2;

typedef enum network_work_types {
	RCC_CONNECTED,
	CELL_UPDATE,
	RCC_IDLE,
	CELL_MEAS_UPDATE,
} network_work_types_t;

#define PROGRESS_WIDTH 30
#define STARTING_OFFSET 0

static struct download_client downloader;
static uint8_t download_client_handle;
static int64_t download_ref_time;
static bool download_in_progress = false;
struct k_work_q network_work_q;
K_THREAD_STACK_DEFINE(network_work_stack_area, 3072);
typedef struct network_work_info {
    struct k_work network_work;
    network_work_types_t type;
} network_work_info_t;
static int sec_tag_list[] = { 123 };
static struct download_client_cfg config = {
	.sec_tag_list = sec_tag_list,
	.sec_tag_count = ARRAY_SIZE(sec_tag_list),
	.set_tls_hostname = true,
};

K_FIFO_DEFINE(neighbor_fifo);
#define MAX_NEIGHBORS 5

network_work_info_t my_network_work_info;

int client_id_get(char *const buffer, size_t buffer_size)
{
	int ret;

    ret = hw_id_get(buffer, buffer_size);
    if (ret) {
        return ret;
    }

	return 0;
}

int network_get_lte_mode(){
	return lte_mode;
}


void network_work_handler(struct k_work *item) {
	network_work_info_t *info = CONTAINER_OF(item, network_work_info_t, network_work);
	switch (info->type) {
		case RCC_CONNECTED:
			LOG_DBG("doing RCC_CONNECTED work");
			break;
		case CELL_UPDATE:
			LOG_DBG("doing CELL_UPDATE work");
			break;
		case RCC_IDLE:
			LOG_DBG("doing RCC_IDLE work");
			break;
		case CELL_MEAS_UPDATE:
			LOG_DBG("doing CELL_MEAS_UPDATE work");
			break;
		default:
			LOG_ERR("Unknown work type: %d", info->type);
			break;
	}
}


static void lte_handler(const struct lte_lc_evt *const evt)
{
	enum network_status status = -1;
	//LOG_DBG("LTE event: %d", evt->type);
	char buf[256];

        switch (evt->type) {
        case LTE_LC_EVT_NW_REG_STATUS:
			if (evt->nw_reg_status != LTE_LC_NW_REG_REGISTERED_HOME &&
				evt->nw_reg_status != LTE_LC_NW_REG_REGISTERED_ROAMING) {
					LOG_WRN("Disconnected from network");
					config_set_lte_connected(false);
					status = NETWORK_DISCONNECTED;
					break;
				}
			status = NETWORK_CONNECTED;
			LOG_WRN("Connected to: %s network\n",
			evt->nw_reg_status == LTE_LC_NW_REG_REGISTERED_HOME ? "home" : "roaming");
			if (modem_setdnsaddr("1.1.1.1") == 0) {
				LOG_INF("DNS secondary address set");
			} else {
				LOG_ERR("Failed to set DNS address");
			}
			config_set_lte_connected(true);
			config_set_lte_working(true);
			LOG_DBG("sending zbus connection event: %d", status);
			int err = zbus_chan_pub(&NETWORK_CHAN, &status, K_SECONDS(1));
			if (err) {
				LOG_ERR("zbus_chan_pub, error: %d", err);
				SEND_FATAL_ERROR();
			}
            break;

        case LTE_LC_EVT_PSM_UPDATE:
			snprintk(buf, sizeof(buf), "PSM parameter update: TAU: %d, Active time: %d  --  requested %s/%s", evt->psm_cfg.tau, evt->psm_cfg.active_time, CONFIG_LTE_PSM_REQ_RPTAU, CONFIG_LTE_PSM_REQ_RAT);
			LOG_CLOUD_INF(MODEM_ERROR_NONE, buf);
			break;
        case LTE_LC_EVT_EDRX_UPDATE:
			snprintk(buf, sizeof(buf), "eDRX parameter update: eDRX: %lf, PTW: %f  --  requested %s/%s", evt->edrx_cfg.edrx, evt->edrx_cfg.ptw, CONFIG_LTE_EDRX_REQ_VALUE_LTE_M, CONFIG_LTE_PTW_VALUE_LTE_M);
			LOG_CLOUD_INF(MODEM_ERROR_NONE, buf);
			break;
        case LTE_LC_EVT_RRC_UPDATE:
			if (evt->rrc_mode == 0) {
				LOG_DBG("RRC mode: IDLE");
			} else if (evt->rrc_mode == 1) {
				LOG_DBG("RRC mode: CONNECTED");
			} else {
				LOG_DBG("RRC mode: Unknown");
			}
			break;
        case LTE_LC_EVT_CELL_UPDATE:
			LOG_DBG("LTE cell changed: Cell ID: %d, Tracking area: %d", evt->cell.id, evt->cell.tac);
			my_network_work_info.type = CELL_UPDATE;
			k_work_submit_to_queue(&network_work_q, &my_network_work_info.network_work);
			status = NETWORK_CELL_CHANGED;
			err = zbus_chan_pub(&NETWORK_CHAN, &status, K_SECONDS(1));
			if (err) {
				LOG_ERR("zbus_chan_pub, error: %d", err);
				SEND_FATAL_ERROR();
			}
			break;
        case LTE_LC_EVT_LTE_MODE_UPDATE:
			if (evt->lte_mode == 7) {
				LOG_DBG("Active LTE mode: LTE-M");
				lte_mode = 0;
			} else if (evt->lte_mode == 9) {
				LOG_DBG("Active LTE mode: NB-IoT");
				lte_mode = 1;
			} else {
				LOG_DBG("Active LTE mode: None");
				lte_mode = 2;
			}
			status = NETWORK_CELL_CHANGED;
			err = zbus_chan_pub(&NETWORK_CHAN, &status, K_SECONDS(1));
			if (err) {
				LOG_ERR("zbus_chan_pub, error: %d", err);
				SEND_FATAL_ERROR();
			}
			break;
        case LTE_LC_EVT_TAU_PRE_WARNING:
			LOG_DBG("TAU pre-warning: %lld seconds", evt->time);
			break;
        case LTE_LC_EVT_NEIGHBOR_CELL_MEAS:
			LOG_DBG("Neighbor cell measurement: ");
			break;
        case LTE_LC_EVT_MODEM_SLEEP_EXIT_PRE_WARNING:
			LOG_DBG("Modem sleep exit pre-warning: %lld seconds", evt->time);
			break;
        case LTE_LC_EVT_MODEM_SLEEP_EXIT:
			LOG_DBG("Modem sleep exit");
			break;
        case LTE_LC_EVT_MODEM_SLEEP_ENTER:
			LOG_DBG("Modem sleep enter");
			break;
        case LTE_LC_EVT_MODEM_EVENT:
			LOG_DBG("Modem generic event: %d", evt->modem_evt);
                /* Handle LTE events */
                break;

        default:
                break;
        }
}


int modem_setdnsaddr(const char *ip_address)
{
        struct nrf_in_addr in4_addr;
        struct nrf_in6_addr in6_addr;
        int family = NRF_AF_INET;
        void *in_addr = NULL;
        nrf_socklen_t in_size = 0;
        int ret = 0;

        if (strlen(ip_address) > 0) {
                in_addr = &in4_addr;
                in_size = sizeof(in4_addr);
                ret = nrf_inet_pton(family, ip_address, in_addr);

                if (ret != 1) {
                        family = NRF_AF_INET6;
                        in_addr = &in6_addr;
                        in_size = sizeof(in6_addr);
                        ret = nrf_inet_pton(family, ip_address, in_addr);
                }

                if (ret != 1) {
                        LOG_ERR("Invalid IP address: %s", ip_address);
                        return -EINVAL;
                }
        }

        // if (link_sett_is_dnsaddr_enabled() && ret == 1) {
                ret = nrf_setdnsaddr(family, in_addr, in_size);
                if (ret != 0) {
                        LOG_ERR("Error setting DNS address: %d", errno);
                        return -errno;
                }
        // } else {
        //         (void)nrf_setdnsaddr(family, NULL, 0);
        // }

        return 0;
}


static void progress_print(size_t downloaded, size_t file_size)
{
	LOG_DBG("download update: %d/%d bytes (%d%%)", downloaded, file_size, (downloaded * 100) / file_size);
}

static int downloader_callback(const struct download_client_evt *event) {
	static size_t downloaded;
	static size_t file_size;
	uint32_t speed;
	int64_t ms_elapsed;
	download_update_t download_update;
	static uint32_t crc32 = 0;

	download_update.download_handle = download_client_handle;

	if (downloaded == 0) {
		download_client_file_size_get(&downloader, &file_size);
		downloaded += STARTING_OFFSET;

	}

	switch (event->id) {
	case DOWNLOAD_CLIENT_EVT_FRAGMENT:
		crc32 = crc32_ieee_update(crc32, event->fragment.buf, event->fragment.len);
		downloaded += event->fragment.len;
		if (file_size) {
			progress_print(downloaded, file_size);
		} else {
			LOG_DBG("\r[ %d bytes ] ", downloaded);
		}
		download_update.download_status = DOWNLOAD_INPROGRESS;
		download_update.download_progress_bytes = downloaded;
		download_update.download_progress_percent = (downloaded * 100) / file_size;
		download_update.download_total_size = file_size;
		download_update.current_dl_amount = event->fragment.len;
		download_update.download_data_ptr = (uint8_t*) event->fragment.buf;
		download_update.crc = crc32;

		// mbedtls_sha256_update(&sha256_ctx,
		// 	event->fragment.buf, event->fragment.len);

		goto update_status;

	case DOWNLOAD_CLIENT_EVT_DONE:
		ms_elapsed = k_uptime_delta(&download_ref_time);
		speed = ((float)file_size / ms_elapsed) * MSEC_PER_SEC;
		LOG_INF("\nDownload completed in %lld ms @ %d bytes per sec, total %d bytes\n",
		       ms_elapsed, speed, downloaded);

		//uint8_t hash_str[64 + 1];

		// mbedtls_sha256_finish(&sha256_ctx, hash);
		// mbedtls_sha256_free(&sha256_ctx);

		//bin2hex(crc32, sizeof(crc32), hash_str, sizeof(hash_str));

		LOG_WRN("Downloaded file CRC: 0x%08x\n", crc32);

		//(void)conn_mgr_if_disconnect(net_if);

		download_update.download_status = DOWNLOAD_COMPLETE;
		download_update.download_handle = download_client_handle;
		download_update.download_progress_bytes	= downloaded;
		download_update.download_progress_percent = (downloaded * 100) / file_size;
		download_update.download_total_size = file_size;
		download_update.current_dl_amount = 0;
		download_update.download_data_ptr = NULL;
		download_update.crc = crc32;

		downloaded = 0;
		download_client_handle = 0;
		download_in_progress = false;
		crc32 = 0;
		goto update_status;

	case DOWNLOAD_CLIENT_EVT_ERROR:
		LOG_ERR("Error %d during download\n", event->error);
		if (event->error == -ECONNRESET) {
			/* With ECONNRESET, allow library to attempt a reconnect by returning 0 */
			LOG_ERR("Download ECONNRESET, attempting reconnect");
		} else {
			//(void)conn_mgr_if_disconnect(net_if);
			/* Stop download */


			download_update.download_status = DOWNLOAD_ERROR;
			download_update.download_handle = download_client_handle;

			download_client_handle = 0;
			download_in_progress = false;
			return -1;
		}
		goto update_status;
	case DOWNLOAD_CLIENT_EVT_CLOSED:
		LOG_ERR("Download Socket closed\n");

		return 0;
	}

update_status:
	int err = zbus_chan_pub(&DOWNLOAD_UPDATE_CHANNEL, &download_update, K_SECONDS(1));
	if (err) {
		LOG_ERR("zbus_chan_pub, error: %d", err);
		SEND_FATAL_ERROR();
	}
	return 0;
}


static void network_task(void)
{
	int err;

	err = nrf_modem_lib_init();
	if (err) {
		LOG_ERR("Modem library initialization failed, error: %d", err);
		return;
	}


	err = lte_lc_init();
	if (err) {
		return;
	}

	int init = NETWORK_INITIALIZING;
	err = zbus_chan_pub(&NETWORK_CHAN, &init, K_SECONDS(1));
	if (err) {
		LOG_ERR("zbus_chan_pub, error: %d", err);
		SEND_FATAL_ERROR();
	}

	// Manu if you want at+cereg? to come out 0,4 then you need to move this 'while' above 'lte_lc_init'
	while (disable_modem) {
		k_sleep(K_SECONDS(1));
	}

	if (modem_setdnsaddr("1.1.1.1") == 0) {
		LOG_INF("DNS secondary address set");
	} else {
		LOG_ERR("Failed to set DNS address");
	}	

	config_set_lte_enabled(true);
	lte_lc_connect_async(lte_handler);

    k_work_queue_init(&network_work_q);
    struct k_work_queue_config network_work_q_cfg = {
        .name = "network_work_q",
        .no_yield = 0,
    };
    k_work_queue_start(&network_work_q, network_work_stack_area,
			K_THREAD_STACK_SIZEOF(network_work_stack_area), 1,
			&network_work_q_cfg);
	k_work_init(&my_network_work_info.network_work, network_work_handler);

	err = download_client_init(&downloader, downloader_callback);
	if (err) {
		printk("Failed to initialize the download client, err %d", err);
	}

	/* Resend connection status if the sample is built for Native Posix.
	 * This is necessary because the network interface is automatically brought up
	 * at SYS_INIT() before main() is called.
	 * This means that NET_EVENT_L4_CONNECTED fires before the
	 * appropriate handler l4_event_handler() is registered.
	 */
	if (IS_ENABLED(CONFIG_BOARD_NATIVE_POSIX)) {
		conn_mgr_mon_resend_status();
	}

	// now loop on zbus waiting for disconnect
	LOG_DBG("network task started, waiting for events");
	const struct zbus_channel *chan;
	while (!zbus_sub_wait(&network_client, &chan, K_FOREVER)) {
		int status;
		if (&NETWORK_CHAN == chan) {

			err = zbus_chan_read(&NETWORK_CHAN, &status, K_SECONDS(10));
			if (err) {
				LOG_ERR("zbus_chan_read, error: %d", err);
				SEND_FATAL_ERROR();
				return;
			}

			if (status == NETWORK_AIRPLANE_MODE_ON) {
				LOG_DBG("airplane mode");
				if (airplane_mode) {
					LOG_DBG("already in airplane mode");
				}
				airplane_mode = true;
			}

			if (status == NETWORK_AIRPLANE_MODE_OFF) {
				LOG_DBG("airplane mode off");
				if (!airplane_mode) {
					LOG_DBG("already out of airplane mode");
				}
				airplane_mode = false;
				lte_lc_connect_async(lte_handler);
				if (err) {
					printk("lte_lc_connect_async, error: %d\n", err);
				}
			}

			if (status == NETWORK_DISCONNECTED) {
				if (airplane_mode) {
					LOG_DBG("airplane mode, not reconnecting");
				}
				else {
					LOG_DBG("network disconnected, trying to reconnect");
					lte_lc_connect_async(lte_handler);
					if (err) {
						printk("lte_lc_connect_async, error: %d\n", err);
					}
				}
			}
		}
		else if (&DOWNLOAD_REQUEST_CHANNEL == chan) {
			download_request_t download_request;
			err = zbus_chan_read(&DOWNLOAD_REQUEST_CHANNEL, &download_request, K_SECONDS(10));
			if (err) {
				LOG_ERR("zbus_chan_read, error: %d", err);
				SEND_FATAL_ERROR();
				return;
			}

			if (download_in_progress) {
				LOG_ERR("download already in progress");

				if (strlen(download_request.download_url) == 0) {
					LOG_ERR("Cancelling active download");
					download_client_disconnect(&downloader);
					continue;
				}
				download_update_t download_update = {
					.download_status = -EBUSY,
					.download_handle = download_request.download_handle
				};
				int err = zbus_chan_pub(&DOWNLOAD_UPDATE_CHANNEL, &download_update, K_SECONDS(1));
				if (err) {
					LOG_ERR("zbus_chan_pub, error: %d", err);
					SEND_FATAL_ERROR();
				}
				continue;
			}
			download_ref_time = k_uptime_get();
			LOG_DBG("download request received, url: %s", download_request.download_url);
			//err = download_client_start(&downloader, download_request.download_url, 0);
			err = download_client_get(&downloader,
									download_request.download_url,
									&config,
									download_request.download_url,
									STARTING_OFFSET);
			if (err) {
				printk("Failed to start download, err %d", err);
				download_update_t download_update = {
					.download_status = 1,  // error
					.download_handle = download_request.download_handle
				};
				int err = zbus_chan_pub(&DOWNLOAD_UPDATE_CHANNEL, &download_update, K_SECONDS(1));
				if (err) {
					LOG_ERR("zbus_chan_pub, error: %d", err);
					SEND_FATAL_ERROR();
				}
				return;
			}
			download_client_handle = download_request.download_handle;
			download_in_progress = true;
		}
		else {
			LOG_ERR("unknown channel");
		}
	}
}

K_THREAD_DEFINE(network_task_id,
		3072,
		network_task, NULL, NULL, NULL, 3, 0, 0);
