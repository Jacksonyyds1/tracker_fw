import paho.mqtt.client as mqtt
import logging
import ssl
import secrets
import json
import time

is_connected = False;

# The callback for when the client receives a CONNACK response from the server.
def on_connect(client, userdata, flags, rc):
    global is_connected;
    print("Connected with result code "+str(rc))

    # Subscribing in on_connect() means that if we lose the connection and
    # reconnect then subscriptions will be renewed.
    client.subscribe("messages/35/10/35_eas-mpb-test-001/c2d")
    is_connected = True;

# The callback for when a PUBLISH message is received from the server.
def on_message(client, userdata, msg):
    print(msg.topic+" "+str(msg.payload))

client = mqtt.Client(client_id="35_eas-mpb-test-001", clean_session=True, userdata=None, protocol=mqtt.MQTTv31, transport="tcp")
client.on_connect = on_connect
client.on_message = on_message
#client.tls_set(ca_certs="/Users/erikscheelke/Documents/github/SimClient/server_root_cert_ats.crt", certfile="/Users/erikscheelke/Documents/github/SimClient/certs/787b21892a79bf6c5252be2321f4a5ec4e5a39d6264cd9962052e04db2aa42a1.crt", keyfile="/Users/erikscheelke/Documents/github/SimClient/certs/787b21892a79bf6c5252be2321f4a5ec4e5a39d6264cd9962052e04db2aa42a1.key", cert_reqs=ssl.CERT_REQUIRED, tls_version=ssl.PROTOCOL_TLS, ciphers=None)
client.tls_set(ca_certs="sim_CA_ats.crt", certfile="sim.crt", keyfile="sim.key", cert_reqs=ssl.CERT_REQUIRED, tls_version=ssl.PROTOCOL_TLS, ciphers=None)
#logging.basicConfig(filename='paho_example.log', level=logging.INFO)
logging.basicConfig(filename=None, level=logging.DEBUG)
client.enable_logger(logger=logging);

client.connect("a3hoon64f0fuap-ats.iot.eu-west-1.amazonaws.com", 8883, 60)

# Blocking call that processes network traffic, dispatches callbacks and handles reconnecting.
# Other loop() functions are available that give a threaded interface and a manual interface.
run = True
last = time.time()
while run:
    client.loop()
    now = time.time()
    if is_connected == True:
        if (now - last) > 2:
            last = now
            data = {
                "P": 2,
                "MID": "eas-mpb-test-001",
                "MK": "US",
                "B": 35,
                "T": 10,
                "M": {
                    "S": secrets.token_bytes(64).hex()
                }
            }
            print(json.dumps(data))
            client.publish("messages/35/10/35_eas-mpb-test-001/d2c", payload=json.dumps(data), qos=0, retain=False)

