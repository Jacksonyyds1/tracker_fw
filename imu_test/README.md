# imu_test
This sample project enables the IMU on D1 and prints the values to the console

## Building and Running
Run `make` to build, `make flash` to build and flash

## Sample Output

Try the `print_samples` command to print IMU data at 15Hz for 3 seconds:
```
uart:~$ print_samples 15 3
[00:00:38.414,886] <inf> shell: printing IMU samples at 15 Hz for 3 seconds...
[00:00:38.424,041] <dbg> imu: imu_enable: setting output data rate to 15
[00:00:38.432,922] <inf> imu: Set sensor type and channel successful.
[00:00:38.502,014] <inf> shell: 00038501,-7.182655,-1.327418,4.552344,-0.735787,0.014202,0.187382
[00:00:38.570,648] <inf> shell: 00038570,-8.186444,-1.521236,5.182255,-0.004886,-0.012064,-0.006719
... <snip> ...
[00:00:41.315,124] <inf> shell: 00041315,-8.179864,-1.520638,5.206781,-0.004428,-0.000763,-0.007941
[00:00:41.383,728] <inf> shell: 00041383,-8.182855,-1.518844,5.200799,-0.004886,-0.007177,-0.007330
[00:00:41.440,582] <dbg> imu: imu_enable: setting output data rate to 0
[00:00:41.449,096] <inf> imu: Set sensor type and channel successful.
[00:00:41.456,695] <inf> shell: complete, printed 43 samples, avg=14.33/s
```
