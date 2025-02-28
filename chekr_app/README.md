# chekr_app
This application implements the ChekrAppLink API, used to train the ML models for the D1 project.

## Building and Running
Run `make` to build, `make flash` to build and flash

## Flashing release images

Use a command like the following:
```
nrfjprog --program chekr_app-v1.1.3-6a9fa19_merged.hex --sectorerase --verify
```

## Testing
You can use the `record_session.py` script in the `tests/culvert` directory to record and harvest a recording session.

```
% python3.9 record_session.py --help
usage: record_session.py [-h] [--iterations ITERATIONS] [--time TIME]

optional arguments:
  -h, --help            show this help message and exit
  --iterations ITERATIONS, -i ITERATIONS
                        number of iterations to run
  --time TIME, -t TIME  max time for recording sessions
  ```

Here's an example:

```
% python3.9 record_session.py -i 1 -t 5
2023-10-31 13:18:35,683 - iterations: 1
2023-10-31 13:18:35,684 - max time: 5
2023-10-31 13:18:35,685 - looking for a device...
2023-10-31 13:18:40,791 - found NDCDF6F99C750AA 45064E32-3277-FC99-5CC7-94F18594708A
2023-10-31 13:18:40,792 - ******** run 0 ********
/Users/mikevoyt/Development/Culvert/Purina-D1/purina-d1-workspace/purina-d1.git/chekr-app/tests/culvert/record_session.py:62: FutureWarning: This method will be removed future version, use the services property instead.
  services = await client.get_services()
2023-10-31 13:18:43,175 - Current time: 00 00 01 8b 87 63 71 67
2023-10-31 13:18:43,309 - send_epoch_time response: 02 08 f2 06 11 00 d1 3e
2023-10-31 13:18:43,441 - get_epoch_time response: 02 0f f2 05 11 00 00 01 8b 87 63 71 ed 3f 51
2023-10-31 13:18:43,576 - set_dog_collar_position response: 02 08 f2 04 11 00 11 9f
2023-10-31 13:18:43,713 - Device Name: nameme
2023-10-31 13:18:43,713 - App Firmware Version: 0.0.4
2023-10-31 13:18:43,713 - Battery Level: 81%
2023-10-31 13:18:43,714 - Battery Voltage: 1147 mV
2023-10-31 13:18:43,714 - Input Power: 22784 mW
2023-10-31 13:18:43,714 - Temperature: 0°C
2023-10-31 13:18:43,714 - Reserved: (0, 0, 0)
2023-10-31 13:18:43,714 - Current time: 00 00 01 8b 87 63 73 83
2023-10-31 13:18:43,714 - Device filename: 0000018B87637383
2023-10-31 13:18:43,937 - start recording response: 02 08 f2 10 11 00 15 df
2023-10-31 13:18:43,937 - sleeping for: 1.84 seconds
2023-10-31 13:18:45,783 - stopping recording
2023-10-31 13:18:45,917 - stop recording response: 02 08 f2 10 11 00 15 df
2023-10-31 13:18:46,053 - read recording session response: 02 11 f2 11 11 00 00 01 8b 87 63 73 83 00 16 05 b2
2023-10-31 13:18:46,054 - num_records: 22
2023-10-31 13:18:46,191 - read recording session data for record 0: {'record_num': 0, 'timestamp': '2023-10-31 20:18:43.725', 'raw_data': [{'ax': '0.23', 'ay': '0.54', 'az': '8.81', 'gx': '-0.65', 'gy': '0.23', 'gz': '-0.10'}, {'ax': '0.29', 'ay': '0.54', 'az': '9.84', 'gx': '-3.27', 'gy': '-0.27', 'gz': '0.97'}, {'ax': '0.30', 'ay': '0.55', 'az': '9.83', 'gx': '-0.74', 'gy': '0.04', 'gz': '0.23'}, {'ax': '0.30', 'ay': '0.54', 'az': '9.82', 'gx': '-0.60', 'gy': '-0.01', 'gz': '0.22'}, {'ax': '0.29', 'ay': '0.54', 'az': '9.83', 'gx': '-0.51', 'gy': '-0.04', 'gz': '0.21'}, {'ax': '0.29', 'ay': '0.54', 'az': '9.84', 'gx': '-0.02', 'gy': '-0.00', 'gz': '0.00'}, {'ax': '0.30', 'ay': '0.54', 'az': '9.83', 'gx': '-0.01', 'gy': '-0.01', 'gz': '-0.01'}, {'ax': '0.29', 'ay': '0.53', 'az': '9.81', 'gx': '-0.01', 'gy': '-0.01', 'gz': '-0.01'}, {'ax': '0.30', 'ay': '0.53', 'az': '9.82', 'gx': '-0.00', 'gy': '0.01', 'gz': '-0.01'}, {'ax': '0.29', 'ay': '0.54', 'az': '9.85', 'gx': '-0.00', 'gy': '0.00', 'gz': '-0.01'}]}
2023-10-31 13:18:46,370 - read recording session data for record 1: {'record_num': 1, 'timestamp': '2023-10-31 20:18:43.893', 'raw_data': [{'ax': '0.30', 'ay': '0.54', 'az': '9.82', 'gx': '-0.01', 'gy': '-0.00', 'gz': '-0.01'}, {'ax': '0.30', 'ay': '0.53', 'az': '9.83', 'gx': '-0.00', 'gy': '0.01', 'gz': '-0.01'}, {'ax': '0.29', 'ay': '0.53', 'az': '9.85', 'gx': '-0.00', 'gy': '0.00', 'gz': '-0.01'}, {'ax': '0.30', 'ay': '0.54', 'az': '9.84', 'gx': '-0.01', 'gy': '-0.01', 'gz': '-0.01'}, {'ax': '0.29', 'ay': '0.54', 'az': '9.82', 'gx': '-0.01', 'gy': '-0.01', 'gz': '-0.01'}, {'ax': '0.29', 'ay': '0.54', 'az': '9.83', 'gx': '-0.00', 'gy': '0.01', 'gz': '-0.01'}, {'ax': '0.29', 'ay': '0.54', 'az': '9.86', 'gx': '-0.00', 'gy': '0.00', 'gz': '-0.01'}, {'ax': '0.29', 'ay': '0.54', 'az': '9.84', 'gx': '-0.01', 'gy': '-0.01', 'gz': '-0.01'}, {'ax': '0.29', 'ay': '0.54', 'az': '9.81', 'gx': '-0.01', 'gy': '-0.01', 'gz': '-0.01'}, {'ax': '0.30', 'ay': '0.54', 'az': '9.84', 'gx': '-0.00', 'gy': '0.01', 'gz': '-0.01'}]}
--- <snip> ---
2023-10-31 13:18:49,909 - read recording session data for record 21: {'record_num': 21, 'timestamp': '2023-10-31 20:18:45.680', 'raw_data': [{'ax': '0.30', 'ay': '0.54', 'az': '9.85', 'gx': '-0.01', 'gy': '-0.01', 'gz': '-0.01'}, {'ax': '0.30', 'ay': '0.54', 'az': '9.82', 'gx': '-0.01', 'gy': '-0.01', 'gz': '-0.01'}, {'ax': '0.30', 'ay': '0.54', 'az': '9.83', 'gx': '-0.00', 'gy': '0.00', 'gz': '-0.01'}, {'ax': '0.29', 'ay': '0.54', 'az': '9.85', 'gx': '-0.00', 'gy': '0.00', 'gz': '-0.01'}, {'ax': '0.29', 'ay': '0.54', 'az': '9.84', 'gx': '-0.01', 'gy': '-0.01', 'gz': '-0.01'}, {'ax': '0.29', 'ay': '0.54', 'az': '9.83', 'gx': '-0.01', 'gy': '-0.00', 'gz': '-0.01'}, {'ax': '0.29', 'ay': '0.53', 'az': '9.85', 'gx': '-0.00', 'gy': '0.01', 'gz': '-0.01'}, {'ax': '0.29', 'ay': '0.54', 'az': '9.85', 'gx': '-0.00', 'gy': '0.00', 'gz': '-0.01'}, {'ax': '0.29', 'ay': '0.54', 'az': '9.85', 'gx': '-0.01', 'gy': '-0.01', 'gz': '-0.01'}, {'ax': '0.30', 'ay': '0.54', 'az': '9.84', 'gx': '-0.00', 'gy': '-0.00', 'gz': '-0.01'}]}
2023-10-31 13:18:52,917 - Test complete!
```

# Documentation
[Bluetooth Protocol Document for ChekrAppLink API Version 3.1](https://docs.google.com/document/d/1zA5mcdpcmkYI4j8YpMxNv5r5VdWYdoGl/edit?usp=sharing&ouid=113281281662803529466&rtpof=true&sd=true)
[Android apk](https://drive.google.com/file/d/1x_dvHHi0cAk20uf-cFrjSmLifTRqlP3i/view?usp=sharing)
