#include <zephyr/logging/log_ctrl.h>
#include <zephyr/logging/log.h>
#include <zephyr/shell/shell.h>
#include <zephyr/random/rand32.h>
#include <stdlib.h>
#include <stdio.h>

#include "chekr.h"
#include "imu.h"
#include "storage.h"
#include "utils.h"
#include "app_version.h"

LOG_MODULE_REGISTER(shell, LOG_LEVEL_DBG);

static int version(const struct shell *sh, size_t argc, char **argv)
{
	shell_fprintf(sh, SHELL_NORMAL, "chekr_app version %d.%d.%d (%s)\r\n", APP_VERSION_MAJOR,
		      APP_VERSION_MINOR, APP_VERSION_PATCH, GIT_HASH);

	shell_fprintf(sh, SHELL_NORMAL, "Built on %s by %s\r\n", DBUILD_DATE, DBUILD_MACHINE);
	return 0;
}

static int write_samples(const struct shell *sh, size_t argc, char **argv)
{
	uint16_t iterations = strtoul(argv[1], NULL, 10);
	uint16_t freq = strtoul(argv[2], NULL, 10);
	uint16_t seconds = strtoul(argv[3], NULL, 10);

	if (argc < 4 || seconds == 0) {
		shell_fprintf(sh, SHELL_NORMAL,
			      "usage: write_samples <iterations> <freq> <seconds>\r\n");
		return -1;
	}

	// random session number
	uint32_t rand = sys_rand32_get();

	for (int i = 0; i < iterations; i++) {
		char filename[32] = {0};
		sprintf(filename, "run_%u-%d", rand, i);
		LOG_DBG("writing file: %s", filename);
		file_handle_t handle = storage_open_file(filename);
		if (handle == NULL) {
			LOG_ERR("failed to open file %s", filename);
			return -1;
		}

		chekr_set_filehandle(handle);

		LOG_INF("writing IMU samples at %d Hz for %d seconds to file: %s...", freq, seconds,
			filename);
		LOG_PANIC(); // flush

		int ret = imu_enable(freq, chekr_record_samples);
		if (ret) {
			LOG_ERR("failed to enable IMU");
			storage_close_file(handle);
			return -1;
		}

		k_sleep(K_SECONDS(seconds));
		imu_enable(IMU_ODR_0_HZ, NULL);

		storage_close_file(handle);

		int count = imu_get_trigger_count();

		LOG_INF("complete, wrote %d samples, avg=%.2f/s", count,
			(float)count / (float)seconds);
	}
	return 0;
}

// callback for print_samples, called on every imu trigger
int print_samples_cb(imu_sample_t data)
{
	LOG_INF("%08llu,%f,%f,%f,%f,%f,%f", data.timestamp, data.ax, data.ay, data.az, data.gx,
		data.gy, data.gz);

	return 0;
}

static int print_samples(const struct shell *sh, size_t argc, char **argv)
{
	uint16_t freq = strtoul(argv[1], NULL, 10);
	uint16_t seconds = strtoul(argv[2], NULL, 10);

	if (argc < 3 || seconds == 0) {
		shell_fprintf(sh, SHELL_NORMAL, "usage: print_samples <freq> <seconds>\r\n");
		return -1;
	}

	LOG_INF("printing IMU samples at %d Hz for %d seconds...", freq, seconds);
	LOG_PANIC(); // flush

	int ret = imu_enable(freq, print_samples_cb);
	if (ret) {
		LOG_ERR("failed to enable IMU");
		return -1;
	}

	k_sleep(K_SECONDS(seconds));
	imu_enable(IMU_ODR_0_HZ, NULL);

	int count = imu_get_trigger_count();

	LOG_INF("complete, printed %d samples, avg=%.2f/s", count, (float)count / (float)seconds);
	return 0;
}

SHELL_CMD_REGISTER(version, NULL, "app version", version);
SHELL_CMD_REGISTER(write_samples, NULL, "usage: write_samples <iterations> <freq> <seconds>",
		   write_samples);
SHELL_CMD_REGISTER(print_samples, NULL, "usage: print_samples <freq> <seconds>", print_samples);
