#include <stdint.h>
#include <stdbool.h>
#include <zephyr/logging/log.h>
#include <zephyr/posix/time.h>
#include <zephyr/sys/byteorder.h>

LOG_MODULE_REGISTER(utils, LOG_LEVEL_DBG);

// current time with millisecond resolution
uint64_t utils_get_currentmillis(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_REALTIME, &ts);

	uint64_t t = (ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
	return sys_cpu_to_be64(t);
}
