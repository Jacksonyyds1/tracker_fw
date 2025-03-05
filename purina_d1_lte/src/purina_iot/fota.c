#include "fota.h"
#include "zbus_msgs.h"
#include "status.h"
#include <zephyr/kernel.h>
#include "transport.h"
#include <zephyr/logging/log.h>
#include <net/fota_download.h>
#include <zephyr/sys/util.h>
#include <zephyr/net/http/parser_url.h>
#include "status.h"
#include "nrf91_upgrade.h"
#include "wi.h"

LOG_MODULE_REGISTER(fota, 4);
ZBUS_SUBSCRIBER_DEFINE(fota_client, 10);
struct k_work_q fota_work_q;
K_THREAD_STACK_DEFINE(fota_work_stack_area, 4096);

enum fota_task_types {
    FOTA_DOWNLOAD,
    FOTA_CANCEL,
    FOTA_UPLOAD
};

typedef struct fota_work_info {
    workref_t *fota_work;
    enum fota_task_types type;
    void* data;
} fota_work_info_t;


int handleFotaMQTTMessage(char *topic, int topicLength, char *message, int messageLength)
{

    return 0;
}

static void strncpy_nullterm(char *dst, const char *src, size_t maxlen)
{
	size_t len = strlen(src) + 1;

	memcpy(dst, src, MIN(len, maxlen));
	if (len > maxlen) {
		dst[maxlen - 1] = '\0';
	}
}

static void download_evt_handler(const struct fota_download_evt *evt)
{
    //LOG_ERR("FOTA download event: %d", evt->id);
    switch (evt->id) {
        case FOTA_DOWNLOAD_EVT_ERROR:
            switch(evt->cause) {
                case FOTA_DOWNLOAD_ERROR_CAUSE_NO_ERROR:
                    LOG_ERR("FOTA download error: no error");
                    setFOTAState(2, FOTA_DOWNLOAD_ERROR_CAUSE_NO_ERROR);
                    break;
                case FOTA_DOWNLOAD_ERROR_CAUSE_DOWNLOAD_FAILED:
                    LOG_ERR("FOTA download error: download failed");
                    setFOTAState(2, FOTA_DOWNLOAD_ERROR_CAUSE_DOWNLOAD_FAILED);
                    break;
                case FOTA_DOWNLOAD_ERROR_CAUSE_INVALID_UPDATE:
                    LOG_ERR("FOTA download error: invalid update");
                    setFOTAState(2, FOTA_DOWNLOAD_ERROR_CAUSE_INVALID_UPDATE);
                    break;
                case FOTA_DOWNLOAD_ERROR_CAUSE_TYPE_MISMATCH:
                    LOG_ERR("FOTA download error: type mismatch");
                    setFOTAState(2, FOTA_DOWNLOAD_ERROR_CAUSE_TYPE_MISMATCH);
                    break;
                case FOTA_DOWNLOAD_ERROR_CAUSE_INTERNAL:
                    LOG_ERR("FOTA download error: internal error");
                    setFOTAState(2, FOTA_DOWNLOAD_ERROR_CAUSE_INTERNAL);
                    break;
                default:
                    LOG_ERR("FOTA download error: unknown error");
                    setFOTAState(2, 99);
                    break;
            }
            //setFOTAState(99, evt->cause);
            config_set_fota_in_progress(false);
            break;
        case FOTA_DOWNLOAD_EVT_PROGRESS:
            uint8_t progress = evt->progress;
            LOG_DBG("FOTA download progress: %d", progress);
            if (progress == 100) {
                progress = 99;  // dont send 100 till event FOTA_DOWNLOAD_EVT_FINISHED
            }
            setFOTAState(1, progress);
            break;
        case FOTA_DOWNLOAD_EVT_FINISHED:
            LOG_INF("FOTA download complete");
            setFOTAState(1, 100);
            k_sleep(K_MSEC(5000));
            SEND_SYS_REBOOT();
            break;
        case FOTA_DOWNLOAD_EVT_CANCELLED:
            LOG_INF("FOTA download cancelled");
            setFOTAState(99, 0);
            config_set_fota_in_progress(false);
            break;
        default:
            LOG_ERR("FOTA download event: unknown event");
            break;
    }
}

int fotaCancel() {
    int err = fota_download_cancel();
    if (err != 0) {
        LOG_ERR("fota_download_cancel error %d", err);
        return err;
    }
    return 0;
}

int handleFotaDownloadHTTPsMessage(char *url, int sec_tag)
{
    int err = 0;
    struct http_parser_url u;

    LOG_DBG("Downloading firmware from %s", url);


    err = fota_download_init(download_evt_handler);
    if (err != 0) {
		LOG_ERR("fota_download_init error %d", err);
		return err;
	}
    char protocol_buf[8] = {0};
    char hostname_buf[128] = {0};
    char file_path_buf[2048] = {0};
    char port_buf[8] = {0};
    char hostname_final[192] = {0};
    setFOTAState(0, 0);
    

    http_parser_url_init(&u);
	http_parser_parse_url(url, strlen(url), false, &u);

    uint16_t parsed_prot_len = u.field_data[UF_SCHEMA].len + 1;
    uint16_t parsed_host_len = u.field_data[UF_HOST].len + 1;
    uint16_t parsed_port_len = u.field_data[UF_PORT].len + 1;
    uint16_t parsed_file_len = u.field_data[UF_PATH].len
                    + u.field_data[UF_QUERY].len + 1;

    if ((parsed_prot_len > 8) ||
        (parsed_host_len > 128) ||
        (parsed_file_len > 2048)) {
        LOG_ERR("URL too long");
        return -1;
    }
    
    strncpy_nullterm(protocol_buf,
            url + u.field_data[UF_SCHEMA].off,
            parsed_prot_len);
    strncpy_nullterm(hostname_buf,
            url + u.field_data[UF_HOST].off,
            parsed_host_len);
    strncpy_nullterm(file_path_buf,
            url + u.field_data[UF_PATH].off + 1,
            parsed_file_len);
    strncpy_nullterm(port_buf,
            url + u.field_data[UF_PORT].off,
            parsed_port_len);
    
    
    if (hostname_buf[0] == 0 || file_path_buf[0] == 0) {
        LOG_ERR("Invalid URL");
        return -EINVAL;
    }

    if (strncmp(url, "https", 5) == 0) {
        if (CONFIG_PURINA_D1_DOWNLOAD_SECURITY_TAG == -1) {
            LOG_ERR("Trying to use https without sec tag configured.");
        }
	}
    else {
        if (port_buf[0] == 0) {
            snprintk(port_buf, sizeof(port_buf), "80");
        }
    }

    if (strlen(port_buf) > 0) {
        snprintk(hostname_final, sizeof(hostname_final), "%s://%s:%s", protocol_buf, hostname_buf, port_buf);
    }
    else {
        snprintk(hostname_final, sizeof(hostname_final), "%s://%s", protocol_buf, hostname_buf);
    }

    LOG_DBG("hostname: %s, filePath: %s", hostname_final, file_path_buf);
    err = fota_download_start(hostname_final, file_path_buf, sec_tag, 0, 0);  
    if (err) {
        LOG_ERR("Error (%d) when trying to start firmware download", err);
        return err;
    }
    config_set_fota_in_progress(true);  
    return 0;
}


void fota_work_handler(struct k_work *work) {
    workref_t *wr = CONTAINER_OF(work, workref_t, work);
	fota_work_info_t *task = (fota_work_info_t*)wr->reference;
    //LOG_DBG("FOTA task type: %d", task->type);
    switch (task->type) {
        case FOTA_DOWNLOAD:
            download_request_t* url_data = (download_request_t*)task->data;
            LOG_DBG("FOTA_DOWNLOAD: %s", url_data->download_url);
            int ret = handleFotaDownloadHTTPsMessage(&(url_data->download_url[0]), CONFIG_PURINA_D1_DOWNLOAD_SECURITY_TAG);
            if (ret != 0) {
                LOG_ERR("handleFotaDownloadHTTPsMessage error %d", ret);
            }
            k_free(url_data);
            break;
        case FOTA_UPLOAD:
            firmware_upload_data_t* upload_data = (firmware_upload_data_t*)task->data;
            LOG_DBG("FOTA_UPLOAD: chunk_num: %d, chunk_total: %d, data_len: %d, handle: %d, crc: 0x%08x", upload_data->chunk_num, upload_data->chunk_total, upload_data->data_len, upload_data->handle, upload_data->crc);

            if (upload_data->chunk_num == 0) {
                nrf91_start_upgrade();
            }
            
            nrf91_upgrade_with_mem_chunk(upload_data->data, upload_data->data_len);

            if (upload_data->chunk_num == upload_data->chunk_total) {
                nrf91_finish_upgrade();
            }

            k_free(upload_data->data);
            k_free(task->data);
            firmware_upload_data_t reply = {
                .chunk_num = upload_data->chunk_num,
                .chunk_total = upload_data->chunk_total,
                .crc = upload_data->crc,
                .data_len = 0,
                .return_code = 0,
                .handle = upload_data->handle
            };
            //LOG_DBG("response: %d %d %d %d %d", reply.chunk_num, reply.chunk_total, reply.data_len, reply.crc, reply.return_code);
            int err = zbus_chan_pub(&FW_UPLOAD_RESP_CHANNEL, &reply, K_SECONDS(1));
            if (err) {
                LOG_ERR("zbus_chan_pub, error:%d", err);
                //SEND_FATAL_ERROR();
            }
            break;
        case FOTA_CANCEL:
            fotaCancel();
            break;
        default:
            LOG_ERR("Unknown FOTA task type: %d", task->type);
            break;
    }
    k_free(task);
    wr_put(wr);
}


///////////////////////////////
/// 
///     fota_task 
///
static void fota_task(void) {
    k_work_queue_init(&fota_work_q);
    struct k_work_queue_config fota_work_q_cfg = {
        .name = "fota_work_q",
        .no_yield = 0,
    };
    k_work_queue_start(&fota_work_q, fota_work_stack_area,
			K_THREAD_STACK_SIZEOF(fota_work_stack_area), 1,
			&fota_work_q_cfg);
    const struct zbus_channel *chan;
    int err;
    while(true) {
		if (zbus_sub_wait(&fota_client, &chan, K_FOREVER) == 0) {
            
			if (&FW_UPLOAD_CHANNEL == chan) {
                firmware_upload_data_t ztask;
				err = zbus_chan_read(&FW_UPLOAD_CHANNEL, &ztask, K_SECONDS(1));
				if (err) {
					LOG_ERR("zbus_chan_read, error: %d", err);
					SEND_FATAL_ERROR();
					return;
				}   
                fota_work_info_t *work_details = k_malloc(sizeof(fota_work_info_t));
                if (work_details == NULL) {
                    LOG_ERR("Failed to allocate memory for firmware upload");
                    return;
                }
                firmware_upload_data_t* task = k_malloc(sizeof(firmware_upload_data_t));
                if (task == NULL) {
                    LOG_ERR("Failed to allocate memory for firmware upload");
                    k_free(work_details);
                    return;
                }
                memcpy(task, &ztask, sizeof(firmware_upload_data_t));

                //LOG_DBG("FOTA_UPLOAD: chunk_num: %d, chunk_total: %d, data_len: %d, handle: %d, crc: %d", task->chunk_num, task->chunk_total, task->data_len, task->handle, task->crc);
                work_details->fota_work = wr_get(work_details, __LINE__);
                k_work_init(&work_details->fota_work->work, fota_work_handler);
                work_details->type = FOTA_UPLOAD;
                work_details->data = task;
                k_work_submit_to_queue(&fota_work_q, &work_details->fota_work->work);
            }
            if (&FOTA_DL_CHANNEL == chan) {
                download_request_t url_data;
                err = zbus_chan_read(&FOTA_DL_CHANNEL, &url_data, K_SECONDS(1));
				if (err) {
					LOG_ERR("zbus_chan_read, error: %d", err);
					//SEND_FATAL_ERROR();
					return;
				}  
                fota_work_info_t *work_details = k_malloc(sizeof(fota_work_info_t));
                if (work_details == NULL) {
                    LOG_ERR("Failed to allocate memory for firmware upload");
                    return;
                }
                download_request_t* url_data_ptr = k_malloc(sizeof(download_request_t));
                if (url_data_ptr == NULL) {
                    LOG_ERR("Failed to allocate memory for download request");
                    return;
                }
                memcpy(url_data_ptr, &url_data, sizeof(download_request_t));
                url_data_ptr->download_handle = url_data.download_handle;

                LOG_DBG("FOTA_DL_CHANNEL: %s", url_data_ptr->download_url);
                work_details->fota_work = wr_get(work_details, __LINE__);
                k_work_init(&work_details->fota_work->work, fota_work_handler);
                work_details->type = FOTA_DOWNLOAD;
                work_details->data = url_data_ptr;
                k_work_submit_to_queue(&fota_work_q, &work_details->fota_work->work);
            }
        }
    }
}


K_THREAD_DEFINE(fota_task_id,
		3072,
		fota_task, NULL, NULL, NULL, 3, 0, 0);