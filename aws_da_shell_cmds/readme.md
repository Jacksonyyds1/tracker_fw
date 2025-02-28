# Staging cert test
### This branch tests connecting to the staging environment using the certificates.  It sends a connectivity message and gets it back.  This also can connect to test.mosquitto.org in both encrypted and non-encrypted way


>There is a bug that the second time I run the tests, it gets a memory error and reboots the 5340, not sure why, but wanted to get this checked in

to Run the tests

1.  First set the time for the DA, this is needed to connect via tls
- e.g. `da16200 set_time 2023 11 17 12 17`

2. Next insert the certificate for staging
- e.g. `da16200 insert_certs 2`

3. Next tell it to turn off mqtt server, set options then connect to broker, subscribe to the connectivity topic and publish a connectivity message
- e.g. `da16200 send_mqtt_msg`


Lots of output will happen