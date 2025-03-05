# How to test the mqtt functionality

- get into the uart shell
- drop into wifi shell and set the crystal tuning and make sure that the D1 connects to the AP
- go back to the uart shell
- run the command "da16200 set_time" to set the date and time 
- run the command "da16200 insert_certs 1" to insert the mosquitto certs (0 and 2 are also options for "no" and "aws" certs)
- In a mac terminal, install the mosquitto package via brew
- cd to the mqtt_info dir where this file and the certs are
- run the subscriber command so you can see the output
- >mosquitto_sub -p 8883 -h test.mosquitto.org -t messages/35/1/35_eas-mpb-test-001/d2c -q 1 --cafile mosquitto.org.crt
- go back to the uart shell and run "da16200 send_mqtt_msg"
Note: that is doesn't always work the first time.  I think its timing to when it connects to the broker

