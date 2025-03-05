#HareKrsna

'''BLE Communications with class implementation.'''

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

class BleComm:
    def __init__(self, filename):
        # Print the current working directory.
        #print(os.getcwd())

        # Get the directory of the current script
        dir_path = os.path.dirname(os.path.realpath(__file__))

        # Change the current working directory to the directory containing the script
        os.chdir(dir_path)

        # Print the current working directory.
        #print(os.getcwd())

        # Read the JSON configuration file.
        try:
            # Read the JSON file.
            with open(filename) as f:
                data = json.load(f)

            # Read the device configuration.
            # self.mac_addr = data['mac_address']
            # macOS doesn't expose MAC address, use UUID instead
            self.mac_addr= data['device_uuid']

            self.device_name = data.get('device_name')
            self.service_uuid = data.get('service_uuid')
            self.tx_uuid = data.get('tx_uuid')
            self.rx_uuid = data.get('rx_uuid')
            self.client = None
        except Exception as e:
            print(f"Error reading file: {e}")

    def get_device_name(self):
        return self.device_name

    async def connect(self):
        self.client = BleakClient(self.mac_addr)
        await self.client.connect()
        print(f"Connected: {self.client.is_connected}")
  

    async def disconnect(self):
        if self.client:
            await self.client.disconnect()
            self.client = None
            print("Disconnected")

    @staticmethod
    async def scan_ble_devices():
        devices = await BleakScanner.discover()
        for d in devices:
            print(d)   
        print("Modify indata.json file with the details of the device including MAC address etc.")        

    async def write(self, data_fr):
        await self.client.write_gatt_char(self.tx_uuid, data_fr)

    async def read(self):
        rx_data = await self.client.read_gatt_char(self.rx_uuid) 
        return rx_data
    
async def lmain():
    print("Scanning for BLE Devices...") 
    await BleComm.scan_ble_devices()
    print("BLE scan complete.\n")

    print("Reading input configuration file: indata.json\n")
    comm = BleComm('indata.json')

    dev = comm.get_device_name()
    print(f"Trying to connect with device: {dev}")

    await comm.connect()

    time.sleep(10)

    # Write some test data.
    tst_data_tx = bytearray()
    tst_data_tx.append(0x01)
    tst_data_tx.append(0x01)
    tst_data_tx.append(0x01)
    tst_data_tx.append(0x01)
    tst_data_tx.append(0x01)

    await comm.write(tst_data_tx)

    time.sleep(0.1)

    # Read response.
    tst_data_rx = await comm.read()
    print('Received :', tst_data_rx)

    await comm.disconnect()

# Testing the module when the file is run in standalone mode.
if __name__ == "__main__":
    # asyncio.run(lmain())
    asyncio.run(BleComm.scan_ble_devices())


