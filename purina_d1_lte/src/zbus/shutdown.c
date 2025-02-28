#include <zephyr/sys/reboot.h>
#include <zephyr/zbus/zbus.h>
#include <zephyr/logging/log.h>
#include <zephyr/logging/log_ctrl.h>
#include <modem/lte_lc.h>
#include "zbus_msgs.h"
#include <zephyr/sys/poweroff.h>
#include "status.h"

/* Register log module */
LOG_MODULE_REGISTER(shutdown, 4);

void shutdown_callback(const struct zbus_channel *chan)
{
	const int* new_state;
	if (&POWER_STATE_CHANNEL == chan) {
		new_state = (const int*)zbus_chan_const_msg(chan);
		config_set_powered_off(true);
		switch(*new_state) {
			case SYS_REBOOT:
					LOG_CLOUD_ERR(MODEM_ERROR_NONE, "9160 rebooting");
					log_panic();
					sys_reboot(0);	
				break;
			case SYS_SHUTDOWN:
				LOG_5340_INF(MODEM_ERROR_NONE, "9160 shutting down in 5 seconds");
				LOG_INF("Told to go to sleep");
				LOG_INF("Powering off modem");
				lte_lc_offline();
				lte_lc_power_off();
				k_sleep(K_SECONDS(5)); //We need to give the modem time to shut down
				LOG_INF("Final Power off: 9160 - bye");
				LOG_PANIC();
				sys_poweroff();
				break;
			default:
				LOG_ERR("Unknown state");
				LOG_PANIC();
				break;
			
		}
	}
}

/* Register listener - error_handler will be called everytime a message is sent to the
 * POWER_STATE_CHANNEL.
 */
ZBUS_LISTENER_DEFINE(shutdown, shutdown_callback);
