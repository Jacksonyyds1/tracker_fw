#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/byteorder.h>
#include <zcbor_encode.h>
#include <zcbor_decode.h>
#include <zcbor_common.h>

#include <zephyr/fs/fs.h>

#include <stdarg.h>

LOG_MODULE_REGISTER(ml);

#include "baseLib.hpp"
#include "fqueue.h"
#include "pmic_leds.h"
#include "ml_encode.h"
#include "ml_decode.h"
#include "ml_types.h"
#include "ml.h"
#include "utils.h"

#define ML_PRIORITY 5

// NOTE: we assume a 15Hz IMU record sample rate, so we don't need to downsample when
// feeding ML model.  If it changes, ML_15HZ_DOWNSAMPLE_RATE must be adjusted
// static_assert(RECORD_SAMPLE_RATE == IMU_ODR_15_HZ, "Expected 15Hz Sample Rate");

static bool     m_verbose;
static int      m_record_num;
static fqueue_t ml_queue;

static fqueue_t *m_file_queue;    // file handle for writing Activity data
static bool      m_is_stopping;
K_MSGQ_DEFINE(ml_mesgq, sizeof(IMUData), 10, 4);
static struct k_thread ml_thread_data;
K_THREAD_STACK_DEFINE(ml_stack_area, 4096);
static k_tid_t ml_tid;

static void handle_result(InferResult result);

#ifdef CONFIG_ML_CAPTURE_IMU_DATA
static int file_num;
static int start_recording(struct fs_file_t *entry, int filenum)
{
    char fname[32];
    int  ret;

    snprintf(fname, sizeof(fname) - 1, "/lfs1/imu%d.dat", filenum);
    fs_file_t_init(entry);
    if ((ret = fs_open(entry, fname, FS_O_CREATE | FS_O_WRITE))) {
        LOG_ERR("Unable to create imu entry %s [%d]", fname, ret);
    }
    return ret;
}

static int end_recording(struct fs_file_t *entry)
{
    return fs_close(entry);
}

static int append_record(struct fs_file_t *entry, IMUData *data)
{
    int        ret;
    static int num_records = 0;

    ret = fs_write(entry, data, sizeof(IMUData));
    if (++num_records > 900) {
        end_recording(entry);
        start_recording(entry, ++file_num);
        num_records = 0;
    }
    return ret;
}
#endif

int ml_init(void)
{
    static bool is_initialized;

    if (!is_initialized) {
        LOG_DBG("ML Init ...");
        // TODO: any ML related initialization here
    }
    is_initialized = true;

    return 0;
}

const char *ml_version(void)
{
    return getVersion();
}

/*
 * Recv sample from IMU and place it in a message queue for retrieval by the ML thread
 */
int ml_feed_sample(imu_sample_t data, bool is_sleeping)
{
    static bool    was_sleeping     = false;
    static int64_t last_sample_time = -1;

    if (is_sleeping && !was_sleeping) {
        // just gone to sleep ... discard any partial samples fed till now
        resetActivityWindowCounter();
        last_sample_time = -1;
        LOG_DBG("IMU has switched to 1.875Hz sampling(asleep)");
    }
    was_sleeping = is_sleeping;
    if (!is_sleeping && data.sample_count % ML_15HZ_DOWNSAMPLE_RATE == 0) {
        // feed ML model

        IMUData imu_sample      = { 0 };
        imu_sample.timestamp    = data.timestamp;
        imu_sample.imuValues[0] = MS2_TO_G(data.ax);
        imu_sample.imuValues[1] = MS2_TO_G(data.ay);
        imu_sample.imuValues[2] = MS2_TO_G(data.az);
        imu_sample.imuValues[3] = RAD_PER_SEC_TO_DEG_PER_SEC(data.gx);
        imu_sample.imuValues[4] = RAD_PER_SEC_TO_DEG_PER_SEC(data.gy);
        imu_sample.imuValues[5] = RAD_PER_SEC_TO_DEG_PER_SEC(data.gz);
        k_msgq_put(&ml_mesgq, &imu_sample, K_NO_WAIT);
        // at 15Hz, we should get a sample every 67ms or so (66.666)
        if (last_sample_time > 0 && (data.timestamp - last_sample_time) > 68) {
            LOG_WRN("Intra sample time is %lldms, > 68ms",data.timestamp-last_sample_time);
            LOG_WRN("As if %lld Sample(s) were dropped", (data.timestamp - last_sample_time) / 67);
        }
        last_sample_time = data.timestamp;
    }

    return 0;
}
#define INCR(index)  ({ index = (index + 1) & 0x0F; })
#define OLDEST_IDX() (index > 0) ? (index - 1) : 15
#define NEWEST_IDX() (index)

static void ml_thread(void *p0, void *p1, void *p2)
{
    initiate(handle_result, false);
    resetActivityWindowCounter();
#ifdef CONFIG_ML_CAPTURE_IMU_DATA
    struct fs_file_t imu_raw_data;
    float            correlator_a[16] = { 0 };
    float            correlator_g[16] = { 0 };
    int              index            = 0;
    bool             is_recording     = false;
    typedef enum
    {
        IDLE,
        GYRO_TRIGGER,
        SET
    } state_t;
    state_t state_sm       = IDLE;
    int     sm_change_time = 0;
#endif
    while (!m_is_stopping) {
        IMUData imu_sample;

        if (k_msgq_get(&ml_mesgq, &imu_sample, K_SECONDS(1)) == 0) {
            if (m_verbose) {
                LOG_INF(
                    "feeding sample at %llu.%03llu %f, %f, %f, %f, %f, %f",
                    imu_sample.timestamp / 1000,
                    imu_sample.timestamp % 1000,
                    imu_sample.imuValues[0],
                    imu_sample.imuValues[1],
                    imu_sample.imuValues[2],
                    imu_sample.imuValues[3],
                    imu_sample.imuValues[4],
                    imu_sample.imuValues[5]);
            }
#ifdef CONFIG_ML_CAPTURE_IMU_DATA
            LED_API_STATE_t oled      = LED_API_IDLE;
            correlator_a[index]       = imu_sample.imuValues[2];    //newest sample overwrites oldest sample
            correlator_g[INCR(index)] = imu_sample.imuValues[4];    //newest sample overwrites oldest sample
            float corr_a_out          = correlator_a[OLDEST_IDX()] - correlator_a[NEWEST_IDX()];
            float corr_g_out          = correlator_g[OLDEST_IDX()] - correlator_g[NEWEST_IDX()];
            switch (state_sm) {
            case IDLE:
                if (abs(corr_g_out) > 100.0f) {
                    state_sm       = GYRO_TRIGGER;
                    sm_change_time = k_uptime_get();
                    LOG_WRN("Gyro trigger");
                }
                break;
            case GYRO_TRIGGER:
                if (k_uptime_get() - sm_change_time < 1000) {
                    if (corr_a_out < -1.9) {
                        state_sm       = SET;
                        sm_change_time = k_uptime_get();
                    } else {
                        state_sm       = IDLE;
                        sm_change_time = k_uptime_get();
                    }
                } else {
                    state_sm       = IDLE;
                    sm_change_time = k_uptime_get();
                }
                break;
            case SET:
                if (k_uptime_get() - sm_change_time < 3000) {
                    if (corr_a_out > 1.9f) {
                        state_sm     = IDLE;
                        is_recording = !is_recording;
                        if (is_recording) {
                            LOG_ERR("WE ARE TRIGGERED ON");
                            start_recording(&imu_raw_data, 0);
                            oled = led_api_set_state(LED_REC_IMU);
                        } else {
                            LOG_ERR("WE ARE TRIGGERED OFF - restore led %d", oled);
                            end_recording(&imu_raw_data);
                            led_api_set_state(oled);
                        }
                        sm_change_time = k_uptime_get();
                    }
                } else {
                    state_sm = IDLE;
                }
                break;
            }
#if 0
            LOG_WRN(
                "CORR ====>%d, %.2f,%.2f = %.2f: %d",
                index,
                correlator_g[OLDEST_IDX()],
                correlator_g[NEWEST_IDX()],
                corr_g_out,
                state_sm);
#endif
            if (is_recording) {
                append_record(&imu_raw_data, &imu_sample);
            }
#endif
            processIMUData(imu_sample);
        }
    }
}
/*
*****************************
Required functions for ML library
*****************************
*/
void *ei_malloc(size_t size)
{
    return k_malloc(size);
}

void *ei_calloc(size_t nitems, size_t size)
{
    return k_calloc(nitems, size);
}

void ei_free(void *ptr)
{
    k_free(ptr);
}

uint64_t ei_read_timer_ms()
{
    // LOG_DBG("ei_read_timer_ms");
    return utils_get_currentmillis();
}

uint64_t ei_read_timer_us()
{
    // LOG_DBG("ei_read_timer_ms");
    return utils_get_currentmicros();
}

// handle inference results
static void handle_result(InferResult result)
{
    uint8_t cbor_buffer[sizeof(InferResult) * 2];    // allow some overhead
    // an InferResult and a struct Inference are essentially the same type and should
    // be directly copyable. However, for clarity and to avoid any confusion, copy them element
    // by element ...
    struct Inference inference;

    inference._Inference_activity    = result.pred_class;
    inference._Inference_probability = result.pred_probability;
    inference._Inference_reps        = result.pred_reps;
    inference._Inference_start       = result.start_timestamp;
    inference._Inference_end         = result.end_timestamp;
    inference._Inference_am          = result.accel_mag_mean_val;
    inference._Inference_gm          = result.gyro_mag_mean_val;
    inference._Inference_as          = result.accel_mag_sd_val;
    inference._Inference_gs          = result.gyro_mag_sd_val;

    uint32_t len;
    if (cbor_encode_Inference(cbor_buffer, sizeof(cbor_buffer), &inference, &len) != 0) {
        LOG_ERR("Failed to encode inference result (size = %zd, len=%d)", sizeof(cbor_buffer), len);
        return;
    }

    // Print the inference result
    LOG_INF(
        "ML: [%08llu.%03llu - %08llu.%03llu] Record #%d: Activity: %s / %d, Repetition: "
        "%f, Probability: %f",
        result.start_timestamp / 1000,
        result.start_timestamp % 1000,
        result.end_timestamp / 1000,
        result.end_timestamp % 1000,
        m_record_num++,
        ACTIVITY_STR(result.pred_class),
        result.pred_class,
        result.pred_reps,
        result.pred_probability);
    LOG_INF(
        "ML: Accel Mean %f, SD %f; Gyro Mean %f, SD %f",
        result.accel_mag_mean_val,
        result.accel_mag_sd_val,
        result.gyro_mag_mean_val,
        result.gyro_mag_sd_val);
    // write the file
    fqueue_put(m_file_queue, cbor_buffer, len);
}

static int ml_start_stop(bool start, fqueue_t *handle)
{
    const char *op;
    static bool is_running;

    if (start && is_running) {
        LOG_WRN("ML is already running!");
        return -EINVAL;
    }
    if (start) {
        op            = "starting";
        m_record_num  = 0;
        m_is_stopping = false;

        ml_tid = k_thread_create(
            &ml_thread_data,
            ml_stack_area,
            K_THREAD_STACK_SIZEOF(ml_stack_area),
            ml_thread,
            NULL,
            NULL,
            NULL,
            ML_PRIORITY,
            0,
            K_NO_WAIT);
        k_thread_name_set(ml_tid, "ML");
        m_file_queue = handle;
        is_running   = true;
    } else {
        op            = "stopping";
        m_file_queue  = NULL;
        m_is_stopping = true;
        LOG_DBG("Waiting for ML thread to terminate ...");
        k_thread_join(&ml_thread_data, K_FOREVER);
        is_running = false;
    }

    LOG_DBG("%s stream to ML model", op);

    return 0;
}

int ml_start(void)
{
    fqueue_init(&ml_queue, "ml", FQ_WRITE, false);
    int ret = ml_start_stop(true, &ml_queue);
    if (ret) {
        return ret;
    }
    ret = imu_enable(IMU_ODR_15_HZ, ml_feed_sample);
    if (ret) {
        LOG_ERR("Unable to start IMU (%d); no ML", ret);
    } else {
        LOG_WRN("ML Started");
    }
    return ret;
}

int ml_stop(void)
{
    int ret = ml_start_stop(false, NULL);
    if (ret == 0) {
        ret = imu_enable(IMU_ODR_0_HZ, NULL);
    }
    if (ret) {
        LOG_ERR("Unable to stop IMU (%d); no ML", ret);
    } else {
        LOG_WRN("ML stopped");
    }
    return ret;
}

//***************************
#ifdef CONFIG_ML_CAPTURE_IMU_DATA
static int ml_clean(const struct shell *sh, size_t argc, char **argv)
{
    int                     res = 0;
    int                     ret = 0;
    struct fs_dir_t         dirp;
    static struct fs_dirent entry;

    fs_dir_t_init(&dirp);

    /* Verify fs_opendir() */
    res = fs_opendir(&dirp, "/lfs1");

    while (res == 0) {
        /* Verify fs_readdir() */
        ret = fs_readdir(&dirp, &entry);

        /* entry.name[0] == 0 means end-of-dir */
        if (ret || entry.name[0] == 0) {
            if (ret < 0) {
                LOG_ERR("Error reading dir [%d]", ret);
            }
            break;
        }
        if (entry.name[0] == 'i' && entry.name[1] == 'm' && entry.name[2] == 'u') {
            char fname[280];
            snprintf(fname, sizeof(fname) - 1, "/lfs1/%s", entry.name);
            LOG_WRN("Removing %s", fname);
            ret = fs_unlink(fname);
            if (ret < 0) {
                LOG_ERR("Unable to remove %s. Is ml running???", entry.name);
            }
        }
    }

    /* Verify fs_closedir() */
    fs_closedir(&dirp);
    if (res == 0) {
        res = ret;
    }
    return res;
}
#endif


static int ml_init_shell(const struct shell *sh, size_t argc, char **argv)
{
    ml_init();
    return 0;
}

static int ml_print_shell(const struct shell *sh, size_t argc, char **argv)
{
    fqueue_t fq;
    fqueue_init(&fq, "ml", FQ_READ, false);
    while (1) {
        uint8_t          cbor_buffer[sizeof(InferResult) * 2];    // allow some overhead
        struct Inference result;
        size_t           size = sizeof(cbor_buffer);
        if (fqueue_get(&fq, cbor_buffer, &size, K_NO_WAIT)) {
            break;
        }
        cbor_decode_Inference(cbor_buffer, size, &result, &size);

        int record_num = 0;
        LOG_WRN(
            "ML: [%08llu.%03llu - %08llu.%03llu] Record #%d: Activity: %s / %d, "
            "Repetition: %f",
            result._Inference_start / 1000,
            result._Inference_start % 1000,
            result._Inference_end / 1000,
            result._Inference_end % 1000,
            record_num++,
            ACTIVITY_STR(result._Inference_activity),
            result._Inference_activity,
            result._Inference_reps);
        LOG_WRN(
            "ML: Accel Mean %f, SD %f; Gyro Mean %f, SD %f",
            result._Inference_am,
            result._Inference_as,
            result._Inference_gm,
            result._Inference_gs);
    }
    return 0;
}

static int ml_start_shell(const struct shell *sh, size_t argc, char **argv)
{
    ml_start();
    return 0;
}

static int ml_stop_shell(const struct shell *sh, size_t argc, char **argv)
{
    ml_stop();
    return 0;
}

static int ml_verbose_shell(const struct shell *sh, size_t argc, char **argv)
{
    m_verbose = !m_verbose;
    shell_print(sh, "Verbose %s", m_verbose ? "On" : "Off");
    return 0;
}

static int ml_version_shell(const struct shell *sh, size_t argc, char **argv)
{
    shell_fprintf(sh, SHELL_NORMAL, "version: %s\r\n", getVersion());
    return 0;
}

int ml_test_shell(const struct shell *sh, size_t argc, char **argv);
#ifndef CONFIG_ML_TEST
int ml_test_shell(const struct shell *sh, size_t argc, char **argv)
{
    shell_error(sh, "Not supported");
    return -EOPNOTSUPP;
}
#endif

SHELL_STATIC_SUBCMD_SET_CREATE(
    sub_ml,
#ifdef CONFIG_ML_CAPTURE_IMU_DATA
    SHELL_CMD(clean, NULL, "Remove raw imu data files", ml_clean),
#endif
    SHELL_CMD(init, NULL, "Initialize ml subsystem", ml_init_shell),
    SHELL_CMD(print, NULL, "Print (and drain) the ml queue", ml_print_shell),
    SHELL_CMD(start, NULL, "Start ml subsystem", ml_start_shell),
    SHELL_CMD(stop, NULL, "Stop ml subsystem", ml_stop_shell),
    SHELL_CMD(test, NULL, "Feed test vectors to library", ml_test_shell),
    SHELL_CMD(verbose, NULL, "ML toggle verbosity", ml_verbose_shell),
    SHELL_CMD(version, NULL, "Print version of ML library", ml_version_shell),
    SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(ml, &sub_ml, "Commands to work with the ML subsystem", NULL);
