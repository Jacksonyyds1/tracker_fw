# LSM6DSV16X-D1 Driver

This is a clone of the standard Zephyr `lsm6dsv16x` sensor driver with a few local modifications:
* It has all the upstream commits from Zephyr mainline included.
* It includes optional motion detection, which reduces the XL ODR to 1.875Hz and powers
down the Gyroscope when no activity is detected.

## Converting from standard driver to LSM6DSV16x-D1

* Enable the module in `CMakeLists.txt`:
```
list(APPEND EXTRA_ZEPHYR_MODULES ${APPLICATION_PROJECT_DIR}/lib/drivers)
```
* Update `prj.conf` to use the new driver:
```
 # IMU
 CONFIG_SENSOR=y
-CONFIG_LSM6DSV16X=y
-CONFIG_LSM6DSV16X_TRIGGER_GLOBAL_THREAD=n
-CONFIG_LSM6DSV16X_TRIGGER_OWN_THREAD=y
-CONFIG_LSM6DSV16X_THREAD_STACK_SIZE=4096
+CONFIG_LSM6DSV16X_D1=y
+CONFIG_LSM6DSV16X_D1_TRIGGER_GLOBAL_THREAD=n
+CONFIG_LSM6DSV16X_D1_TRIGGER_OWN_THREAD=y
+CONFIG_LSM6DSV16X_D1_THREAD_STACK_SIZE=4096
+CONFIG_LSM6DSV16X_D1_ENABLE_MOTION=y
```
* Update the board overlay file:
```
        lsm6dsv16x@0 {
-               compatible = "st,lsm6dsv16x";
+               compatible = "st,lsm6dsv16x-d1";
                                status = "okay";
                                reg = <0>;
                                spi-max-frequency = <4000000>;
-                               irq-gpios = <&gpio0 5 GPIO_ACTIVE_HIGH>;
+                               int1-gpios = <&gpio0 5 GPIO_ACTIVE_HIGH>;
+                               int2-gpios = <&gpio0 6 GPIO_ACTIVE_HIGH>;
                                drdy-pin = <1>;
                                gyro-range = <2>; // LSM6DSV16X_DT_FS_500DPS
                                accel-range = <1>; // LSM6DSV16X_DT_FS_4G
```
