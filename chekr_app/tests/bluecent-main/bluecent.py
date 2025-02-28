#HareKrsna

'''
Bluecent Project main source file.

The project is to connect and communicate with BLE peripheral device,
which is running ChekrAppLink protocol.

The App is the BLE Central.
'''

# Import packages required for BLE communications.  
import asyncio
from bleak import BleakScanner
from bleak import BleakClient

# Import packages for handling json files.
import json

# Import packages required for os interaction.
import os
import sys

# Import packages required for time.
import time

# Import packages required for ChekrAppLink protocol.
import chekrapplink as comm
import struct

rec_ses_uid = 1234

# Print the current working directory.
print(os.getcwd())

# Get the directory of the current script
dir_path = os.path.dirname(os.path.realpath(__file__))

# Change the current working directory to the directory containing the script
os.chdir(dir_path)

# Print the current working directory.
print(os.getcwd())

# Read the JSON file.
with open('indata.json') as f:
    data = json.load(f)

# Read Mac address and Device name.
if len(data) == 1:
    for device in data:
        mac_address = device['mac_address']
        device_name = device['device_name']
        service_uuid = device['service_uuid']
        tx_uuid = device['tx_uuid']
        rx_uuid = device['rx_uuid']
        # print(f"MAC Address: {mac_address}")
        # print(f"Device Name: {device_name}")
        # print(f"Service UUID: {service_uuid}")
        # print(f"Tx Characterstic UUID: {tx_uuid}")
        # print(f"Rx Characterstic UUID: {rx_uuid}")
else:
    print("More than one/no device found in the input data.")
    
# Scan for BLE devices and print them. 
# async def scan_ble_devices():
#     devices = await BleakScanner.discover()
#     for d in devices:
#         print(d)

#asyncio.run(scan_ble_devices())

# Read device available services and characterstics. 
# async def read_dev_chrcs(address):
#     async with BleakClient(address) as client:
#         services = await client.get_services()
#         for service in services:
#             print("Service UUID:", service.uuid)
#             for char in service.characteristics:
#                 print("\tCharacteristic UUID:", char.uuid)

# asyncio.run(read_dev_chrcs(mac_address))

#
async def send_data(address, tx_chrc_uuid, rx_chrc_uuid):
    async with BleakClient(address) as client:
        x = await client.is_connected()
        print("Connected: {0}".format(x))

        # Get the characteristic to write to
        #characteristic = await client.get_services  get_gatt_characteristic(tx_chrc_uuid)

        while True:
            # Read input from the console
            data =  input("Enter data to send to the peripheral (or x to exit): ")

            if data == 'mac':
                dat = bytes.fromhex('00')
                data_fr = comm.get_mac_addr_frame(dat)
                #data_fr =  bytes.fromhex("0107F10100D748")
            if data == 'sete':
                epoch_time_ms = int(time.time() * 1000)
                dat = struct.pack('>Q', epoch_time_ms)
                data_fr = comm.set_epoch_time_frame(dat)
            if data == 'gete':
                dat = bytes.fromhex('00')
                data_fr = comm.get_epoch_time_frame(dat)
            if data == 'start':
                dat = bytearray()
                start_stop = 0x01
                dat.append(start_stop)
                uid_data = struct.pack('>Q', rec_ses_uid)
                dat += uid_data  
                data_fr = comm.start_stop_rec_sess(dat)
            if data == 'stop':
                dat = bytearray()
                start_stop = 0x00
                dat.append(start_stop)
                uid_data = struct.pack('>Q', rec_ses_uid)
                dat += uid_data  
                data_fr = comm.start_stop_rec_sess(dat)   
            if data == 'x':
                break

            # Convert the string to bytes and write to the characteristic
            await client.write_gatt_char(tx_chrc_uuid, data_fr)

            # Capture current milles to compare time difference.
            #if data_fr[3] == 0x05:
            curr_epoch_time_ms = int(time.time() * 1000)

            # Sleep for 100ms.
            time.sleep(0.1)  

            # Convert the string to bytes and write to the characteristic
            rx_data = await client.read_gatt_char(rx_chrc_uuid)

            # Parse the received frame.
            parsed_data = comm.CommFrame.from_bytes(rx_data)
            #hex_str = ' '.join(format(x, '02x') for x in parsed_data.data)
            #print(hex_str)

            # Process received data.
            # MAC Address.
            if parsed_data.command == 0x01:
                hex_str = ' '.join(format(x, '02x') for x in parsed_data.data)
                print("MAC Address: ", hex_str)         

            # Get epoch time frame.
            if parsed_data.command == 0x05:
                #curr_epoch_time_ms = int(time.time() * 1000)                
                get_epoch_time_ms = struct.unpack('>Q', parsed_data.data)[0]
                t_diff = curr_epoch_time_ms - get_epoch_time_ms
                print(f"Time difference: {t_diff}")

            # Set epoch time frame.
            if parsed_data.command == 0x06:
                print("Epoch time set success!")

            # Start/Stop recording session.
            if parsed_data.command == 0x10:
                print("Start/Stop recording session success!")
            
            # Print received frame
            hex_str = ' '.join(format(x, '02x') for x in rx_data)
            print("Received Frame: ", hex_str) 


if __name__ == "__main__":
    loop = asyncio.get_event_loop()
    loop.run_until_complete(send_data(mac_address, tx_uuid, rx_uuid))
