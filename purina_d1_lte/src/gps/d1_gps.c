#include "d1_gps.h"
#include <zephyr/logging/log.h>
#include <nrf_modem_at.h>
#include <nrf_modem_gnss.h>
#include <modem/lte_lc.h>
#include <modem/nrf_modem_lib.h>
#include <date_time.h>
#include "status.h"
#include "zbus_msgs.h"
#include <zephyr/sys/timeutil.h>
#if defined(CONFIG_ARCH_POSIX) && defined(CONFIG_EXTERNAL_LIBC)
#include <time.h>
#else
#include <zephyr/posix/time.h>
#endif



LOG_MODULE_REGISTER(gps, 4);

#define PI 3.14159265358979323846
#define EARTH_RADIUS_METERS (6371.0 * 1000.0)

#define GNSS_WORKQ_THREAD_STACK_SIZE 2304
#define GNSS_WORKQ_THREAD_PRIORITY   5


static bool gps_initialized = false;
static bool gps_started = false;
static bool lte_initialized = false;

ZBUS_SUBSCRIBER_DEFINE(gps_listener, 10);

struct k_work_q gps_work_q;
struct k_work gps_work;
K_THREAD_STACK_DEFINE(gps_stack_area, 2048);

#if !defined(CONFIG_PURINA_D1_LTE_ASSISTANCE_NONE) || defined(CONFIG_PURINA_D1_LTE_MODE_TTFF_TEST)
static struct k_work_q gnss_work_q;

#define GNSS_WORKQ_THREAD_STACK_SIZE 2304
#define GNSS_WORKQ_THREAD_PRIORITY   5

K_THREAD_STACK_DEFINE(gnss_workq_stack_area, GNSS_WORKQ_THREAD_STACK_SIZE);
#endif /* !CONFIG_PURINA_D1_LTE_ASSISTANCE_NONE || CONFIG_PURINA_D1_LTE_MODE_TTFF_TEST */

#if !defined(CONFIG_PURINA_D1_LTE_ASSISTANCE_NONE)
#include "assistance.h"

static struct nrf_modem_gnss_agnss_data_frame last_agnss;
static struct k_work agnss_data_get_work;
static volatile bool requesting_assistance;
#endif /* !CONFIG_PURINA_D1_LTE_ASSISTANCE_NONE */

#if defined(CONFIG_PURINA_D1_LTE_MODE_TTFF_TEST)
static struct k_work_delayable ttff_test_got_fix_work;
static struct k_work_delayable ttff_test_prepare_work;
static struct k_work ttff_test_start_work;
static uint32_t time_to_fix;
#endif

static const char update_indicator[] = {'\\', '|', '/', '-'};

static struct nrf_modem_gnss_pvt_data_frame last_pvt;
static uint64_t fix_timestamp;
static uint32_t time_blocked;

/* Reference position. */
static bool ref_used;
static double ref_latitude;
static double ref_longitude;

static uint8_t tracked_cached = -1;
static uint8_t in_fix_cached = -1;
static uint8_t unhealthy_cached = -1;
static uint32_t seconds_since_last_fix = 0;
static uint64_t last_fix_timestamp = 0;
static uint32_t timeToLock = 0;
static bool TtLSet = false;
static bool sendFakeData = false;
gps_info_t latest_gps_info;
uint8_t gps_sample_period = 0;


K_MSGQ_DEFINE(nmea_queue, sizeof(struct nrf_modem_gnss_nmea_data_frame *), 10, 4);
static K_SEM_DEFINE(pvt_data_sem, 0, 1);
static K_SEM_DEFINE(time_sem, 0, 1);

static struct k_poll_event events[2] = {
	K_POLL_EVENT_STATIC_INITIALIZER(K_POLL_TYPE_SEM_AVAILABLE,
					K_POLL_MODE_NOTIFY_ONLY,
					&pvt_data_sem, 0),
	K_POLL_EVENT_STATIC_INITIALIZER(K_POLL_TYPE_MSGQ_DATA_AVAILABLE,
					K_POLL_MODE_NOTIFY_ONLY,
					&nmea_queue, 0),
};

BUILD_ASSERT(IS_ENABLED(CONFIG_LTE_NETWORK_MODE_LTE_M_GPS) ||
	     IS_ENABLED(CONFIG_LTE_NETWORK_MODE_NBIOT_GPS) ||
	     IS_ENABLED(CONFIG_LTE_NETWORK_MODE_LTE_M_NBIOT_GPS),
	     "CONFIG_LTE_NETWORK_MODE_LTE_M_GPS, "
	     "CONFIG_LTE_NETWORK_MODE_NBIOT_GPS or "
	     "CONFIG_LTE_NETWORK_MODE_LTE_M_NBIOT_GPS must be enabled");

BUILD_ASSERT((sizeof(CONFIG_PURINA_D1_LTE_REFERENCE_LATITUDE) == 1 &&
	      sizeof(CONFIG_PURINA_D1_LTE_REFERENCE_LONGITUDE) == 1) ||
	     (sizeof(CONFIG_PURINA_D1_LTE_REFERENCE_LATITUDE) > 1 &&
	      sizeof(CONFIG_PURINA_D1_LTE_REFERENCE_LONGITUDE) > 1),
	     "CONFIG_PURINA_D1_LTE_REFERENCE_LATITUDE and "
	     "CONFIG_PURINA_D1_LTE_REFERENCE_LONGITUDE must be both either set or empty");



/* Returns the distance between two coordinates in meters. The distance is calculated using the
 * haversine formula.
 */
static double distance_calculate(double lat1, double lon1,
				 double lat2, double lon2)
{
	double d_lat_rad = (lat2 - lat1) * PI / 180.0;
	double d_lon_rad = (lon2 - lon1) * PI / 180.0;

	double lat1_rad = lat1 * PI / 180.0;
	double lat2_rad = lat2 * PI / 180.0;

	double a = pow(sin(d_lat_rad / 2), 2) +
		   pow(sin(d_lon_rad / 2), 2) *
		   cos(lat1_rad) * cos(lat2_rad);

	double c = 2 * asin(sqrt(a));

	return EARTH_RADIUS_METERS * c;
}


static void print_distance_from_reference(struct nrf_modem_gnss_pvt_data_frame *pvt_data)
{
	if (!ref_used) {
		return;
	}

	double distance = distance_calculate(pvt_data->latitude, pvt_data->longitude,
					     ref_latitude, ref_longitude);

	if (IS_ENABLED(CONFIG_PURINA_D1_LTE_MODE_TTFF_TEST)) {
		LOG_INF("Distance from reference: %.01f", distance);
	} else {
		printf("\nDistance from reference: %.01f\n", distance);
	}
}

static void gnss_event_handler(int event)
{
	int retval;
	struct nrf_modem_gnss_nmea_data_frame *nmea_data;

	switch (event) {
	case NRF_MODEM_GNSS_EVT_PVT:
		retval = nrf_modem_gnss_read(&last_pvt, sizeof(last_pvt), NRF_MODEM_GNSS_DATA_PVT);
		if (retval == 0) {
			k_sem_give(&pvt_data_sem);
		}
		break;

#if defined(CONFIG_PURINA_D1_LTE_MODE_TTFF_TEST)
	case NRF_MODEM_GNSS_EVT_FIX:
		/* Time to fix is calculated here, but it's printed from a delayed work to avoid
		 * messing up the NMEA output.
		 */
		time_to_fix = (k_uptime_get() - fix_timestamp) / 1000;
		k_work_schedule_for_queue(&gnss_work_q, &ttff_test_got_fix_work, K_MSEC(100));
		k_work_schedule_for_queue(&gnss_work_q, &ttff_test_prepare_work,
					  K_SECONDS(CONFIG_PURINA_D1_LTE_MODE_TTFF_TEST_INTERVAL));
		break;
#endif /* CONFIG_PURINA_D1_LTE_MODE_TTFF_TEST */

	case NRF_MODEM_GNSS_EVT_NMEA:
		nmea_data = k_malloc(sizeof(struct nrf_modem_gnss_nmea_data_frame));
		if (nmea_data == NULL) {
			LOG_ERR("Failed to allocate memory for NMEA");
			break;
		}

		retval = nrf_modem_gnss_read(nmea_data,
					     sizeof(struct nrf_modem_gnss_nmea_data_frame),
					     NRF_MODEM_GNSS_DATA_NMEA);
		if (retval == 0) {
			retval = k_msgq_put(&nmea_queue, &nmea_data, K_NO_WAIT);
		}

		if (retval != 0) {
			k_free(nmea_data);
		}
		break;

	case NRF_MODEM_GNSS_EVT_AGNSS_REQ:
#if !defined(CONFIG_PURINA_D1_LTE_ASSISTANCE_NONE)
		retval = nrf_modem_gnss_read(&last_agnss,
					     sizeof(last_agnss),
					     NRF_MODEM_GNSS_DATA_AGNSS_REQ);
		if (retval == 0) {
			k_work_submit_to_queue(&gnss_work_q, &agnss_data_get_work);
		}
#endif /* !CONFIG_PURINA_D1_LTE_ASSISTANCE_NONE */
		break;

	default:
		break;
	}
}

#if !defined(CONFIG_PURINA_D1_LTE_ASSISTANCE_NONE)
#if defined(CONFIG_PURINA_D1_LTE_LTE_ON_DEMAND)
K_SEM_DEFINE(lte_ready, 0, 1);

static void lte_lc_event_handler(const struct lte_lc_evt *const evt)
{
	switch (evt->type) {
	case LTE_LC_EVT_NW_REG_STATUS:
		if ((evt->nw_reg_status == LTE_LC_NW_REG_REGISTERED_HOME) ||
		    (evt->nw_reg_status == LTE_LC_NW_REG_REGISTERED_ROAMING)) {
			LOG_INF("Connected to LTE network");
			k_sem_give(&lte_ready);
		}
		break;

	default:
		break;
	}
}

void lte_connect(void)
{
	int err;

	LOG_INF("Connecting to LTE network");

	err = lte_lc_func_mode_set(LTE_LC_FUNC_MODE_ACTIVATE_LTE);
	if (err) {
		LOG_ERR("Failed to activate LTE, error: %d", err);
		return;
	}

	k_sem_take(&lte_ready, K_FOREVER);

	/* Wait for a while, because with IPv4v6 PDN the IPv6 activation takes a bit more time. */
	k_sleep(K_SECONDS(1));
}

void lte_disconnect(void)
{
	int err;

	err = lte_lc_func_mode_set(LTE_LC_FUNC_MODE_DEACTIVATE_LTE);
	if (err) {
		LOG_ERR("Failed to deactivate LTE, error: %d", err);
		return;
	}

	LOG_INF("LTE disconnected");
}
#endif /* CONFIG_PURINA_D1_LTE_LTE_ON_DEMAND */

static const char *get_system_string(uint8_t system_id)
{
	switch (system_id) {
	case NRF_MODEM_GNSS_SYSTEM_INVALID:
		return "invalid";

	case NRF_MODEM_GNSS_SYSTEM_GPS:
		return "GPS";

	case NRF_MODEM_GNSS_SYSTEM_QZSS:
		return "QZSS";

	default:
		return "unknown";
	}
}

static void agnss_data_get_work_fn(struct k_work *item)
{
	ARG_UNUSED(item);

	int err;

	/* GPS data need is always expected to be present and first in list. */
	__ASSERT(last_agnss.system_count > 0,
		 "GNSS system data need not found");
	__ASSERT(last_agnss.system[0].system_id == NRF_MODEM_GNSS_SYSTEM_GPS,
		 "GPS data need not found");

#if defined(CONFIG_PURINA_D1_LTE_ASSISTANCE_SUPL)
	/* SUPL doesn't usually provide NeQuick ionospheric corrections and satellite real time
	 * integrity information. If GNSS asks only for those, the request should be ignored.
	 */
	if (last_agnss.system[0].sv_mask_ephe == 0 &&
	    last_agnss.system[0].sv_mask_alm == 0 &&
	    (last_agnss.data_flags & ~(NRF_MODEM_GNSS_AGNSS_NEQUICK_REQUEST |
				       NRF_MODEM_GNSS_AGNSS_INTEGRITY_REQUEST)) == 0) {
		LOG_INF("Ignoring assistance request for only NeQuick and/or integrity");
		return;
	}
#endif /* CONFIG_PURINA_D1_LTE_ASSISTANCE_SUPL */

#if defined(CONFIG_PURINA_D1_LTE_ASSISTANCE_MINIMAL)
	/* With minimal assistance, the request should be ignored if no GPS time or position
	 * is requested.
	 */
	if (!(last_agnss.data_flags & NRF_MODEM_GNSS_AGNSS_GPS_SYS_TIME_AND_SV_TOW_REQUEST) &&
	    !(last_agnss.data_flags & NRF_MODEM_GNSS_AGNSS_POSITION_REQUEST)) {
		LOG_INF("Ignoring assistance request because no GPS time or position is requested");
		return;
	}
#endif /* CONFIG_PURINA_D1_LTE_ASSISTANCE_MINIMAL */

	if (last_agnss.data_flags == 0 &&
	    last_agnss.system[0].sv_mask_ephe == 0 &&
	    last_agnss.system[0].sv_mask_alm == 0) {
		LOG_INF("Ignoring assistance request because only QZSS data is requested");
		return;
	}

	requesting_assistance = true;

	LOG_INF("Assistance data needed: data_flags: 0x%02x", last_agnss.data_flags);
	for (int i = 0; i < last_agnss.system_count; i++) {
		LOG_INF("Assistance data needed: %s ephe: 0x%llx, alm: 0x%llx",
			get_system_string(last_agnss.system[i].system_id),
			last_agnss.system[i].sv_mask_ephe,
			last_agnss.system[i].sv_mask_alm);
	}

#if defined(CONFIG_PURINA_D1_LTE_LTE_ON_DEMAND)
	lte_connect();
#endif /* CONFIG_PURINA_D1_LTE_LTE_ON_DEMAND */

	err = assistance_request(&last_agnss);
	if (err) {
		LOG_ERR("Failed to request assistance data");
	}

#if defined(CONFIG_PURINA_D1_LTE_LTE_ON_DEMAND)
	lte_disconnect();
#endif /* CONFIG_PURINA_D1_LTE_LTE_ON_DEMAND */

	requesting_assistance = false;
}
#endif /* !CONFIG_PURINA_D1_LTE_ASSISTANCE_NONE */

#if defined(CONFIG_PURINA_D1_LTE_MODE_TTFF_TEST)
static void ttff_test_got_fix_work_fn(struct k_work *item)
{
	LOG_INF("Time to fix: %u", time_to_fix);
	if (time_blocked > 0) {
		LOG_INF("Time GNSS was blocked by LTE: %u", time_blocked);
	}
	print_distance_from_reference(&last_pvt);
	LOG_INF("Sleeping for %u seconds", CONFIG_PURINA_D1_LTE_MODE_TTFF_TEST_INTERVAL);
}

static int ttff_test_force_cold_start(void)
{
	int err;
	uint32_t delete_mask;

	LOG_INF("Deleting GNSS data");

	/* Delete everything else except the TCXO offset. */
	delete_mask = NRF_MODEM_GNSS_DELETE_EPHEMERIDES |
		      NRF_MODEM_GNSS_DELETE_ALMANACS |
		      NRF_MODEM_GNSS_DELETE_IONO_CORRECTION_DATA |
		      NRF_MODEM_GNSS_DELETE_LAST_GOOD_FIX |
		      NRF_MODEM_GNSS_DELETE_GPS_TOW |
		      NRF_MODEM_GNSS_DELETE_GPS_WEEK |
		      NRF_MODEM_GNSS_DELETE_UTC_DATA |
		      NRF_MODEM_GNSS_DELETE_GPS_TOW_PRECISION;

	/* With minimal assistance, we want to keep the factory almanac. */
	if (IS_ENABLED(CONFIG_PURINA_D1_LTE_ASSISTANCE_MINIMAL)) {
		delete_mask &= ~NRF_MODEM_GNSS_DELETE_ALMANACS;
	}

	err = nrf_modem_gnss_nv_data_delete(delete_mask);
	if (err) {
		LOG_ERR("Failed to delete GNSS data");
		return -1;
	}

	return 0;
}

#if defined(CONFIG_PURINA_D1_LTE_ASSISTANCE_NRF_CLOUD)
static bool qzss_assistance_is_supported(void)
{
	char resp[32];

	if (nrf_modem_at_cmd(resp, sizeof(resp), "AT+CGMM") == 0) {
		/* nRF9160 does not support QZSS assistance, while nRF91x1 do. */
		if (strstr(resp, "nRF9160") != NULL) {
			return false;
		}
	}

	return true;
}
#endif /* CONFIG_PURINA_D1_LTE_ASSISTANCE_NRF_CLOUD */

static void ttff_test_prepare_work_fn(struct k_work *item)
{
	/* Make sure GNSS is stopped before next start. */
	nrf_modem_gnss_stop();

	if (IS_ENABLED(CONFIG_PURINA_D1_LTE_MODE_TTFF_TEST_COLD_START)) {
		if (ttff_test_force_cold_start() != 0) {
			return;
		}
	}

#if !defined(CONFIG_PURINA_D1_LTE_ASSISTANCE_NONE)
	if (IS_ENABLED(CONFIG_PURINA_D1_LTE_MODE_TTFF_TEST_COLD_START)) {
		/* All A-GNSS data is always requested before GNSS is started. */
		last_agnss.data_flags =
			NRF_MODEM_GNSS_AGNSS_GPS_UTC_REQUEST |
			NRF_MODEM_GNSS_AGNSS_KLOBUCHAR_REQUEST |
			NRF_MODEM_GNSS_AGNSS_NEQUICK_REQUEST |
			NRF_MODEM_GNSS_AGNSS_GPS_SYS_TIME_AND_SV_TOW_REQUEST |
			NRF_MODEM_GNSS_AGNSS_POSITION_REQUEST |
			NRF_MODEM_GNSS_AGNSS_INTEGRITY_REQUEST;
		last_agnss.system_count = 1;
		last_agnss.system[0].sv_mask_ephe = 0xffffffff;
		last_agnss.system[0].sv_mask_alm = 0xffffffff;
#if defined(CONFIG_PURINA_D1_LTE_ASSISTANCE_NRF_CLOUD)
		if (qzss_assistance_is_supported()) {
			last_agnss.system_count = 2;
			last_agnss.system[1].sv_mask_ephe = 0x3ff;
			last_agnss.system[1].sv_mask_alm = 0x3ff;
		}
#endif /* CONFIG_PURINA_D1_LTE_ASSISTANCE_NRF_CLOUD */

		k_work_submit_to_queue(&gnss_work_q, &agnss_data_get_work);
	} else {
		/* Start and stop GNSS to trigger possible A-GNSS data request. If new A-GNSS
		 * data is needed it is fetched before GNSS is started.
		 */
		nrf_modem_gnss_start();
		nrf_modem_gnss_stop();
	}
#endif /* !CONFIG_PURINA_D1_LTE_ASSISTANCE_NONE */

	k_work_submit_to_queue(&gnss_work_q, &ttff_test_start_work);
}

static void ttff_test_start_work_fn(struct k_work *item)
{
	LOG_INF("Starting GNSS");
	if (nrf_modem_gnss_start() != 0) {
		LOG_ERR("Failed to start GNSS");
		return;
	}

	fix_timestamp = k_uptime_get();
	time_blocked = 0;
}
#endif /* CONFIG_PURINA_D1_LTE_MODE_TTFF_TEST */

static void date_time_evt_handler(const struct date_time_evt *evt)
{
	k_sem_give(&time_sem);
}

static int modem_init(void)
{
	if (IS_ENABLED(CONFIG_DATE_TIME)) {
		date_time_register_handler(date_time_evt_handler);
	}

	if (lte_lc_init() != 0) {
		LOG_ERR("Failed to initialize LTE link controller");
		return -1;
	}

#if defined(CONFIG_PURINA_D1_LTE_LTE_ON_DEMAND)
	lte_lc_register_handler(lte_lc_event_handler);
#elif !defined(CONFIG_PURINA_D1_LTE_ASSISTANCE_NONE)
	lte_lc_psm_req(true);

	LOG_INF("Connecting to LTE network");

	if (lte_lc_connect() != 0) {
		LOG_ERR("Failed to connect to LTE network");
		return -1;
	}

	LOG_INF("Connected to LTE network");

	// if (IS_ENABLED(CONFIG_DATE_TIME)) {
	// 	LOG_INF("Waiting for current time");

	// 	/* Wait for an event from the Date Time library. */
	// 	k_sem_take(&time_sem, K_MINUTES(10));

	// 	if (!date_time_is_valid()) {
	// 		LOG_WRN("Failed to get current time, continuing anyway");
	// 	}
	// }
#endif

	return 0;
}

static int sample_init(void)
{
	int err = 0;

#if !defined(CONFIG_PURINA_D1_LTE_ASSISTANCE_NONE) || defined(CONFIG_PURINA_D1_LTE_MODE_TTFF_TEST)
	struct k_work_queue_config cfg = {
		.name = "gnss_work_q",
		.no_yield = false
	};

	k_work_queue_start(
		&gnss_work_q,
		gnss_workq_stack_area,
		K_THREAD_STACK_SIZEOF(gnss_workq_stack_area),
		GNSS_WORKQ_THREAD_PRIORITY,
		&cfg);
#endif /* !CONFIG_PURINA_D1_LTE_ASSISTANCE_NONE || CONFIG_PURINA_D1_LTE_MODE_TTFF_TEST */

#if !defined(CONFIG_PURINA_D1_LTE_ASSISTANCE_NONE)
	k_work_init(&agnss_data_get_work, agnss_data_get_work_fn);

	err = assistance_init(&gnss_work_q);
#endif /* !CONFIG_PURINA_D1_LTE_ASSISTANCE_NONE */

#if defined(CONFIG_PURINA_D1_LTE_MODE_TTFF_TEST)
	k_work_init_delayable(&ttff_test_got_fix_work, ttff_test_got_fix_work_fn);
	k_work_init_delayable(&ttff_test_prepare_work, ttff_test_prepare_work_fn);
	k_work_init(&ttff_test_start_work, ttff_test_start_work_fn);
#endif /* CONFIG_PURINA_D1_LTE_MODE_TTFF_TEST */

	return err;
}

static int gnss_init_and_start(void)
{
#if defined(CONFIG_PURINA_D1_LTE_ASSISTANCE_NONE) || defined(CONFIG_PURINA_D1_LTE_LTE_ON_DEMAND)
	/* Enable GNSS. */
	if (lte_lc_func_mode_set(LTE_LC_FUNC_MODE_ACTIVATE_GNSS) != 0) {
		LOG_ERR("Failed to activate GNSS functional mode");
		return -1;
	}
#endif /* CONFIG_PURINA_D1_LTE_ASSISTANCE_NONE || CONFIG_PURINA_D1_LTE_LTE_ON_DEMAND */

	/* Configure GNSS. */
	if (nrf_modem_gnss_event_handler_set(gnss_event_handler) != 0) {
		LOG_ERR("Failed to set GNSS event handler");
		return -1;
	}

	/* Enable all supported NMEA messages. */
	uint16_t nmea_mask = NRF_MODEM_GNSS_NMEA_RMC_MASK |
			     NRF_MODEM_GNSS_NMEA_GGA_MASK |
			     NRF_MODEM_GNSS_NMEA_GLL_MASK |
			     NRF_MODEM_GNSS_NMEA_GSA_MASK |
			     NRF_MODEM_GNSS_NMEA_GSV_MASK;

	if (nrf_modem_gnss_nmea_mask_set(nmea_mask) != 0) {
		LOG_ERR("Failed to set GNSS NMEA mask");
		return -1;
	}

	/* Make QZSS satellites visible in the NMEA output. */
	if (nrf_modem_gnss_qzss_nmea_mode_set(NRF_MODEM_GNSS_QZSS_NMEA_MODE_CUSTOM) != 0) {
		LOG_WRN("Failed to enable custom QZSS NMEA mode");
	}

	/* This use case flag should always be set. */
	uint8_t use_case = NRF_MODEM_GNSS_USE_CASE_MULTIPLE_HOT_START;

	if (IS_ENABLED(CONFIG_PURINA_D1_LTE_MODE_PERIODIC) &&
	    !IS_ENABLED(CONFIG_PURINA_D1_LTE_ASSISTANCE_NONE)) {
		/* Disable GNSS scheduled downloads when assistance is used. */
		use_case |= NRF_MODEM_GNSS_USE_CASE_SCHED_DOWNLOAD_DISABLE;
	}

	if (IS_ENABLED(CONFIG_PURINA_D1_LTE_LOW_ACCURACY)) {
		use_case |= NRF_MODEM_GNSS_USE_CASE_LOW_ACCURACY;
	}

	if (nrf_modem_gnss_use_case_set(use_case) != 0) {
		LOG_WRN("Failed to set GNSS use case");
	}

#if defined(CONFIG_NRF_CLOUD_AGNSS_ELEVATION_MASK)
	if (nrf_modem_gnss_elevation_threshold_set(CONFIG_NRF_CLOUD_AGNSS_ELEVATION_MASK) != 0) {
		LOG_ERR("Failed to set elevation threshold");
		return -1;
	}
	LOG_DBG("Set elevation threshold to %u", CONFIG_NRF_CLOUD_AGNSS_ELEVATION_MASK);
#endif

#if defined(CONFIG_PURINA_D1_LTE_MODE_CONTINUOUS)
	/* Default to no power saving. */
	uint8_t power_mode = NRF_MODEM_GNSS_PSM_DISABLED;

#if defined(CONFIG_PURINA_D1_LTE_POWER_SAVING_MODERATE)
	power_mode = NRF_MODEM_GNSS_PSM_DUTY_CYCLING_PERFORMANCE;
#elif defined(CONFIG_PURINA_D1_LTE_POWER_SAVING_HIGH)
	power_mode = NRF_MODEM_GNSS_PSM_DUTY_CYCLING_POWER;
#endif

	if (nrf_modem_gnss_power_mode_set(power_mode) != 0) {
		LOG_ERR("Failed to set GNSS power saving mode");
		return -1;
	}
#endif /* CONFIG_PURINA_D1_LTE_MODE_CONTINUOUS */

	/* Default to continuous tracking. */
	uint16_t fix_retry = 0;
	uint16_t fix_interval = 1;

#if defined(CONFIG_PURINA_D1_LTE_MODE_PERIODIC)
	fix_retry = CONFIG_PURINA_D1_LTE_PERIODIC_TIMEOUT;
	fix_interval = CONFIG_PURINA_D1_LTE_PERIODIC_INTERVAL;
#elif defined(CONFIG_PURINA_D1_LTE_MODE_TTFF_TEST)
	/* Single fix for TTFF test mode. */
	fix_retry = 0;
	fix_interval = 0;
#endif

	if (nrf_modem_gnss_fix_retry_set(fix_retry) != 0) {
		LOG_ERR("Failed to set GNSS fix retry");
		return -1;
	}

	if (nrf_modem_gnss_fix_interval_set(fix_interval) != 0) {
		LOG_ERR("Failed to set GNSS fix interval");
		return -1;
	}

#if defined(CONFIG_PURINA_D1_LTE_MODE_TTFF_TEST)
	k_work_schedule_for_queue(&gnss_work_q, &ttff_test_prepare_work, K_NO_WAIT);
#else /* !CONFIG_PURINA_D1_LTE_MODE_TTFF_TEST */
	if (nrf_modem_gnss_start() != 0) {
		LOG_ERR("Failed to start GNSS");
		return -1;
	}
#endif

	return 0;
}

static bool output_paused(void)
{
#if defined(CONFIG_PURINA_D1_LTE_ASSISTANCE_NONE) || defined(CONFIG_PURINA_D1_LTE_LOG_LEVEL_OFF)
	return false;
#else
	return (requesting_assistance || assistance_is_active()) ? true : false;
#endif
}

uint64_t get_unix_time() {
	struct timespec tp;
	struct tm tm;
	clock_gettime(CLOCK_REALTIME, &tp);
	gmtime_r(&tp.tv_sec, &tm);
	int64_t currTime = timeutil_timegm64(&tm);
    return currTime;
}

static void print_satellite_stats(struct nrf_modem_gnss_pvt_data_frame *pvt_data)
{
	uint8_t tracked   = 0;
	uint8_t in_fix    = 0;
	uint8_t unhealthy = 0;

	for (int i = 0; i < NRF_MODEM_GNSS_MAX_SATELLITES; ++i) {
		if (pvt_data->sv[i].sv > 0) {
			tracked++;

			if (pvt_data->sv[i].flags & NRF_MODEM_GNSS_SV_FLAG_USED_IN_FIX) {
				in_fix++;
			}

			if (pvt_data->sv[i].flags & NRF_MODEM_GNSS_SV_FLAG_UNHEALTHY) {
				unhealthy++;
			}
		}
	}

if (IS_ENABLED(CONFIG_PURINA_D1_LTE_PRINT_GPS_STATS)) {
	printf("\033[1;1H");
	printf("\033[2J");
	printf("Tracking: %2d Using: %2d Unhealthy: %d\n", tracked, in_fix, unhealthy);
}
else {
	/* Print the satellite stats in a single line. */
	if (tracked != tracked_cached || in_fix != in_fix_cached || unhealthy != unhealthy_cached) {
		LOG_DBG("GPS Tracking: %2d Using: %2d Unhealthy: %d - secs since last fix: %d", tracked, in_fix, unhealthy, seconds_since_last_fix);
	}
	
}


	tracked_cached = tracked;
	in_fix_cached = in_fix;
	unhealthy_cached = unhealthy;

	latest_gps_info.numSatellites = in_fix;
	latest_gps_info.secSinceLock = seconds_since_last_fix;
	latest_gps_info.timeToLock = 0;
	latest_gps_info.timestamp = get_unix_time();
}

static void print_fix_data(struct nrf_modem_gnss_pvt_data_frame *pvt_data)
{
if (IS_ENABLED(CONFIG_PURINA_D1_LTE_PRINT_GPS_STATS)) {
	printf("Latitude:       %.06f\n", pvt_data->latitude);
	printf("Longitude:      %.06f\n", pvt_data->longitude);
	printf("Altitude:       %.01f m\n", pvt_data->altitude);
	printf("Accuracy:       %.01f m\n", pvt_data->accuracy);
	printf("Speed:          %.01f m/s\n", pvt_data->speed);
	printf("Speed accuracy: %.01f m/s\n", pvt_data->speed_accuracy);
	printf("Heading:        %.01f deg\n", pvt_data->heading);
	printf("Date:           %04u-%02u-%02u\n",
	       pvt_data->datetime.year,
	       pvt_data->datetime.month,
	       pvt_data->datetime.day);
	printf("Time (UTC):     %02u:%02u:%02u.%03u\n",
	       pvt_data->datetime.hour,
	       pvt_data->datetime.minute,
	       pvt_data->datetime.seconds,
	       pvt_data->datetime.ms);
	printf("PDOP:           %.01f\n", pvt_data->pdop);
	printf("HDOP:           %.01f\n", pvt_data->hdop);
	printf("VDOP:           %.01f\n", pvt_data->vdop);
	printf("TDOP:           %.01f\n", pvt_data->tdop);
}
else {
		if (!sendFakeData) {
			/* Print the fix data in a single line. */
			LOG_DBG("GPS Fix: Lat: %.06f Lon: %.06f Alt: %.01f Acc: %.01f Date: %04u-%02u-%02u Time: %02u:%02u:%02u.%03u",
				pvt_data->latitude, pvt_data->longitude, pvt_data->altitude, pvt_data->accuracy,
				pvt_data->datetime.year, pvt_data->datetime.month, pvt_data->datetime.day, pvt_data->datetime.hour, pvt_data->datetime.minute, pvt_data->datetime.seconds, pvt_data->datetime.ms);
		}
	}
	if (TtLSet == false) {
		TtLSet = true;
		timeToLock = (k_uptime_get() - last_fix_timestamp) / 1000;
	}
	
	if (sendFakeData) {
		LOG_DBG("Sending fake data");
		latest_gps_info.latitude = (36.1052789 * 1000000);
		latest_gps_info.longitude = (-115.178236 * 1000000);
		latest_gps_info.altitude = (13.65 * 100);
		latest_gps_info.accuracy = (1 * 100);
		latest_gps_info.speed = (1 * 10);
		latest_gps_info.speed_accuracy = (1 * 10);
		latest_gps_info.numSatellites = in_fix_cached;
		latest_gps_info.secSinceLock = seconds_since_last_fix;
		latest_gps_info.heading = (pvt_data->heading * 10);
		latest_gps_info.timeToLock = timeToLock;
	}
	else {
		latest_gps_info.latitude = (pvt_data->latitude * 1000000);
		latest_gps_info.longitude = (pvt_data->longitude * 1000000);
		latest_gps_info.altitude = (pvt_data->altitude * 100);
		latest_gps_info.accuracy = (pvt_data->accuracy * 100);
		latest_gps_info.speed = (pvt_data->speed * 10);
		latest_gps_info.speed_accuracy = (pvt_data->speed_accuracy * 10);
		latest_gps_info.numSatellites = in_fix_cached;
		latest_gps_info.secSinceLock = seconds_since_last_fix;
		latest_gps_info.heading = (pvt_data->heading * 10);
		latest_gps_info.timeToLock = timeToLock;
	}
	latest_gps_info.timestamp = get_unix_time();
}

int gps_init() {
	
	

	LOG_INF("Starting GNSS");

	// err = nrf_modem_lib_init();
	// if (err) {
	// 	LOG_ERR("Modem library initialization failed, error: %d", err);
	// 	return err;
	// }

	if (modem_init() != 0) {
		LOG_ERR("Failed to initialize modem");
		return -1;
	}

	if (sample_init() != 0) {
		LOG_ERR("Failed to initialize sample");
		return -1;
	}

	if (gnss_init_and_start() != 0) {
		LOG_ERR("Failed to initialize and start GNSS");
		return -1;
	}

	gps_initialized = true;
	return 0;    
}


void do_gps_work(struct k_work *work){
	fix_timestamp = k_uptime_get();
	int window_time_blocked = 0;
	struct nrf_modem_gnss_nmea_data_frame *nmea_data;
	uint8_t cnt = 0;
	while(gps_started) {
		(void)k_poll(events, 2, K_FOREVER);

		if (events[0].state == K_POLL_STATE_SEM_AVAILABLE &&
		    k_sem_take(events[0].sem, K_NO_WAIT) == 0) {
			/* New PVT data available */

			if (IS_ENABLED(CONFIG_PURINA_D1_LTE_MODE_TTFF_TEST)) {
				/* TTFF test mode. */

				/* Calculate the time GNSS has been blocked by LTE. */
				if (last_pvt.flags & NRF_MODEM_GNSS_PVT_FLAG_DEADLINE_MISSED) {
					time_blocked++;
				}
			} else if (IS_ENABLED(CONFIG_PURINA_D1_LTE_NMEA_ONLY)) {
				/* NMEA-only output mode. */

				if (output_paused()) {
					goto handle_nmea;
				}

				if (last_pvt.flags & NRF_MODEM_GNSS_PVT_FLAG_FIX_VALID) {
					print_distance_from_reference(&last_pvt);
				}
			} else {
				/* PVT and NMEA output mode. */

				if (output_paused()) {
					goto handle_nmea;
				}

				if (IS_ENABLED(CONFIG_PURINA_D1_LTE_PRINT_GPS_STATS)) {
					print_satellite_stats(&last_pvt);
				}
				else {
					if (!(last_pvt.flags & NRF_MODEM_GNSS_PVT_FLAG_FIX_VALID)) {
						print_satellite_stats(&last_pvt);  // dont call this if there is a fix
					}
				}

				if (last_pvt.flags & NRF_MODEM_GNSS_PVT_FLAG_DEADLINE_MISSED) {
					if (IS_ENABLED(CONFIG_PURINA_D1_LTE_PRINT_GPS_STATS)) {
						printf("GNSS operation blocked by LTE\n");
					}
				}
				if (last_pvt.flags &
				    NRF_MODEM_GNSS_PVT_FLAG_NOT_ENOUGH_WINDOW_TIME) {
					if (IS_ENABLED(CONFIG_PURINA_D1_LTE_PRINT_GPS_STATS)) {
						printf("Insufficient GNSS time windows\n");
					}
					window_time_blocked++;
					if (window_time_blocked > 5) {
						if (IS_ENABLED(CONFIG_PURINA_D1_LTE_PRINT_GPS_STATS)) {
							LOG_ERR("GNSS operation blocked by LTE for too long");
						}
						nrf_modem_gnss_prio_mode_enable();
					}
				}
				if (last_pvt.flags & NRF_MODEM_GNSS_PVT_FLAG_SLEEP_BETWEEN_PVT) {
					if (IS_ENABLED(CONFIG_PURINA_D1_LTE_PRINT_GPS_STATS)) {
						printf("Sleep period(s) between PVT notifications\n");
					}
				}
				if (IS_ENABLED(CONFIG_PURINA_D1_LTE_PRINT_GPS_STATS)) {
					printf("-----------------------------------\n");
				}

				if ((last_pvt.flags & NRF_MODEM_GNSS_PVT_FLAG_FIX_VALID) || sendFakeData) {
					fix_timestamp = k_uptime_get();
					print_fix_data(&last_pvt);
					if (IS_ENABLED(CONFIG_PURINA_D1_LTE_PRINT_GPS_STATS)) {
						print_distance_from_reference(&last_pvt);
					}
				} else {
					seconds_since_last_fix = (uint32_t)((k_uptime_get() - fix_timestamp) / 1000);
					if (IS_ENABLED(CONFIG_PURINA_D1_LTE_PRINT_GPS_STATS)) {
						printf("Seconds since last fix: %d\n", seconds_since_last_fix);
						printf("Searching [%c]\n", update_indicator[cnt%4]);
					}
					cnt++;
					
				}
				if (IS_ENABLED(CONFIG_PURINA_D1_LTE_PRINT_GPS_STATS)) {
					printf("\nNMEA strings:\n\n");
				}
			}
		}

handle_nmea:
		if (events[1].state == K_POLL_STATE_MSGQ_DATA_AVAILABLE &&
		    k_msgq_get(events[1].msgq, &nmea_data, K_NO_WAIT) == 0) {
			/* New NMEA data available */

			if (!output_paused()) {
				if (IS_ENABLED(CONFIG_PURINA_D1_LTE_PRINT_GPS_STATS)) {
					printf("%s", nmea_data->nmea_str);
				}
			}
			k_free(nmea_data);
		}

		events[0].state = K_POLL_STATE_NOT_READY;
		events[1].state = K_POLL_STATE_NOT_READY;
	}
}


//K_WORK_DEFINE(my_gps_work, do_gps_work);
// void my_gps_work_timer_handler(struct k_timer *dummy)
// {
//     k_work_submit(&my_gps_work);
// }


int gps_start() {
	if (gps_started) {
		LOG_INF("GNSS already started");
		return 0;
	}
	if (!gps_initialized) {
		if (gps_init() != 0) {
			LOG_ERR("Failed to initialize GNSS");
			return -1;
		}
	}
	if (nrf_modem_gnss_start() != 0) {
		LOG_ERR("Failed to start GNSS");
		return -1;
	}
#if defined(CONFIG_GNSS_PRIORITY_ON_FIRST_FIX)
	int err = nrf_modem_gnss_prio_mode_enable();
	if (err) {
			LOG_ERR("Could not set priority mode for GNSS (%d)", err);
	}
#endif
	TtLSet = false;
	last_fix_timestamp = k_uptime_get();
	gps_started = true;
	timeToLock = 0;
	config_set_gpsEnabled(true);
	LOG_DBG("GNSS started");
	k_work_submit_to_queue(&gps_work_q, &gps_work);
	return 0;
}


int gps_stop() {
	if (!gps_started) {
		LOG_INF("GNSS already stopped");
		return 0;
	}
	if (gps_initialized && gps_started) {
		if (nrf_modem_gnss_stop() != 0) {
			LOG_ERR("Failed to stop GNSS");
			return -1;
		}
	}
	gps_started = false;
	LOG_DBG("GNSS stopped");
	config_set_gpsEnabled(false);
	return 0;
}


void my_gps_work_handler(struct k_work *work){
    int err = zbus_chan_pub(&GPS_DATA_CHANNEL, &latest_gps_info, K_SECONDS(1));
	if (err) {
		LOG_ERR("zbus_chan_pub, error:%d", err);
	}
}
K_WORK_DEFINE(my_gps_work, my_gps_work_handler);
void my_gps_work_timer_handler(struct k_timer *dummy)
{
    k_work_submit(&my_gps_work);
}
K_TIMER_DEFINE(my_gps_work_timer, my_gps_work_timer_handler, NULL);


static void gps_main_loop(void)
{
	int err;
	const struct zbus_channel *chan;

	k_work_queue_init(&gps_work_q);
    struct k_work_queue_config gps_work_q_cfg = {
        .name = "gps_work_q",
        .no_yield = 0,
    };
    k_work_queue_start(&gps_work_q, gps_stack_area,
                   K_THREAD_STACK_SIZEOF(gps_stack_area), 5,
                   &gps_work_q_cfg);
    k_work_init(&gps_work, do_gps_work);


	while(true) {
		if (zbus_sub_wait(&gps_listener, &chan, K_FOREVER) == 0) {
			

			if (&NETWORK_CHAN == chan) {
				enum network_status status;
				err = zbus_chan_read(&NETWORK_CHAN, &status, K_SECONDS(1));
				if (err) {
					LOG_ERR("zbus_chan_read, error: %d", err);
					SEND_FATAL_ERROR();
				}

				if ((status == NETWORK_CONNECTED) || (status == NETWORK_INITIALIZING)){
					lte_initialized = true;
				}

			}

			if (chan == &GPS_STATE_CHANNEL) {
				gps_settings_t newState;
				err = zbus_chan_read(&GPS_STATE_CHANNEL, &newState, K_SECONDS(1));
				if (err) {
					LOG_ERR("zbus_chan_read, error: %d", err);
					SEND_FATAL_ERROR();
				}

				switch (newState.gps_enable) {
					case 0:
						LOG_DBG("Stopping GNSS");
						k_timer_stop(&my_gps_work_timer);
						if (gps_stop() != 0) {
							LOG_ERR("Failed to stop GNSS");
						}
						break;
					case 1:
						if (lte_initialized) {							
							if (newState.gps_sample_period != 0) {
								if (gps_sample_period != newState.gps_sample_period) {
									LOG_DBG("Setting GPS sample period to %d", newState.gps_sample_period);
									if (newState.gps_sample_period != gps_sample_period) {
										LOG_DBG("Stopping GNSS - to get new timer period");
										k_timer_stop(&my_gps_work_timer);
									}
									gps_sample_period = newState.gps_sample_period;
								}
							}
							LOG_DBG("Starting GNSS");
							if (!gps_started) {
								if (gps_start() != 0) {
									LOG_ERR("Failed to start GNSS");
									break;
								}
							}

							k_timer_start(&my_gps_work_timer, K_SECONDS(gps_sample_period), K_SECONDS(gps_sample_period));
						}
						break;
				}
				switch (newState.gps_fakedata_enable) {
					case 1:
						sendFakeData = true;
						break;
					case 0:
						sendFakeData = false;
						break;
				}

			}
		}
	}
}




K_THREAD_DEFINE(gps_task_id,
		2048,
		gps_main_loop, NULL, NULL, NULL, 3, 0, 0);

