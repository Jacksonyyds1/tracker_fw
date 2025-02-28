import atexit
import asyncio
from bleak import BleakClient, BleakScanner
import cbor2
import argparse
import log_config
import logging
from datetime import datetime
import numpy as np
import os
import time
log_config.setup_logging()
logger = logging.getLogger('collar')
from log_config import logging_set_filename
from bleak.exc import BleakError

SERVICE_UUID = "9d1589a6-cea6-4df1-96d9-1697cd4dc1e7"
SET_DEVICE_INFO_CHARACTERISTIC_UUID = "9d1589a6-cea6-4df1-96d9-1697cd4dc201"

bleak_client = None

# Initiate the parser
parser = argparse.ArgumentParser()

# Add long and short argument
parser.add_argument("--device", "-d", help="device name (BLE advertisement)")
parser.add_argument('--connect', nargs='+', metavar='ARGS',
                    help='Connect to an AP. Requires a NAME and PASSWORD, and optionally a SEC_FLAGS.')


# Read arguments from the command line
args = parser.parse_args()

async def scan():
    devices = await BleakScanner.discover()
    for device in devices:
        logger.info(f"checking {device.name} ({device.address})")
        if args.device:
            if device.name and device.name == args.device:
                logger.info(f"found {device.name} {device.address}")
                return device.address
        else:
            if device.name and ("NDC" in device.name or "Petivity" in device.name):
                logger.info(f"found {device.name} {device.address}")
                return device.address
    return None

# helper routine to handle the unresolved Bleak 'unlikely error' exception
async def write_with_response(client, uuid, data):
    try:
        await client.write_gatt_char(uuid, data)
    except BleakError as e:
        if "Unlikely error" in str(e):
            # FIXME: writing to a BT_GATT_CHRC_WRITE_WITHOUT_RESP works, but for some reason
            # we get this when writing to BT_GATT_CHRC_WRITE:
            # bleak.exc.BleakError: Failed to write characteristic 34:
            # Error Domain=CBATTErrorDomain Code=14 "Unlikely error." 
            # UserInfo={NSLocalizedDescription=Unlikely error.
            #
            # But the write works!  so let's just skip for now
            logger.info("Unlikely :(")
            pass


async def service_test(address):
    if not address:
        logger.info("No address provided!")
        return

    # device disconnects when this block exits
    async with BleakClient(address) as client:

        global bleak_client
        bleak_client = client
        # Confirm the device offers the tracker service
        services = client.services
        if SERVICE_UUID not in [s.uuid for s in services]:
            logger.info(f"Service {SERVICE_UUID} not found on device {address}")
            return

        logger.info("enabling notifications...")
        DEVICE_INFO_NOTIFY_UUID = "9d1589a6-cea6-4df1-96d9-1697cd4dc1e8"

        # handle notifications here
        async def device_info_notification_handler(sender: int, data: bytearray):
            decoded = cbor2.loads(data)
            logger.info(f"DeviceInfo notification: {decoded}")
            if 'SCAN_COUNT' in decoded:
                count = decoded['SCAN_COUNT']
                # read the wifi AP list
                await read_wifi_scan_list(client, count)
                # wait until ctrl-c
                logger.info("press <ctrl-c> to exit")

        await client.start_notify(DEVICE_INFO_NOTIFY_UUID, device_info_notification_handler)

        logger.info("setting time...")
        utc_time = np.int64(int(round(time.time() * 1000))) # currentmillis in a 64-bit value
        set_time = {
            "UTC_TIME" : int(utc_time)
        }
        set_time_encoded = cbor2.dumps(set_time)
        await write_with_response(client, SET_DEVICE_INFO_CHARACTERISTIC_UUID, set_time_encoded)

        logger.info("reading Device Info...")
        DEVICE_INFO_CHARACTERISTIC_UUID = "9d1589a6-cea6-4df1-96d9-1697cd4dc200"
        raw = await client.read_gatt_char(DEVICE_INFO_CHARACTERISTIC_UUID)
        decoded = cbor2.loads(raw)
        logger.info(f"FW Version: {decoded['VERSION']}")
        logger.info(f"Self-check status: {decoded['STATUS']}")
        timestamp_seconds = decoded['UTC_TIME'] / 1000.0
        formatted_timestamp = datetime.utcfromtimestamp(timestamp_seconds).strftime('%Y-%m-%d %H:%M:%S.%f')
        logger.info(f"System time: {formatted_timestamp}")
        logger.info(f"Serial Number: {decoded['SERIAL_NUM']}")

        logger.info("reading Battery Status...")
        BATTERY_STATUS_CHARACTERISTIC_UUID = "9d1589a6-cea6-4df1-96d9-1697cd4dc300"
        raw = await client.read_gatt_char(BATTERY_STATUS_CHARACTERISTIC_UUID)
        decoded = cbor2.loads(raw)
        logger.info(f"Battery %: {decoded['SOC']}")
        logger.info(f"Battery status: {decoded['CHARGING_STATUS']}")

        logger.info("Getting Pairing Nonce...")
        PAIRING_NONCE_CHARACTERISTIC_UUID = "9d1589a6-cea6-4df1-96d9-1697cd4dc900"
        raw = await client.read_gatt_char(PAIRING_NONCE_CHARACTERISTIC_UUID)
        decoded = cbor2.loads(raw)
        logger.info(f"NONCE: {decoded['NONCE'].hex(' ')}")

        logger.info("reading Modem Info...")
        MODEM_INFO_CHARACTERISTIC_UUID = "9d1589a6-cea6-4df1-96d9-1697cd4dc400"
        raw = await client.read_gatt_char(MODEM_INFO_CHARACTERISTIC_UUID)
        decoded = cbor2.loads(raw)
        logger.info(f"ICCID: {decoded['ICCID']}")
        logger.info(f"IMEI: {decoded['IMEI']}")
        logger.info(f"Version: {decoded['VERSION']}")
        logger.info(f"Modem Connection Status: {decoded['LTE_STATUS']}")

        logger.info("reading WiFi Info...")
        MODEM_INFO_CHARACTERISTIC_UUID = "9d1589a6-cea6-4df1-96d9-1697cd4dc500"
        raw = await client.read_gatt_char(MODEM_INFO_CHARACTERISTIC_UUID)
        decoded = cbor2.loads(raw)
        logger.info(f"WiFi MAC Addr: {decoded['MACADDR']}")
        logger.info(f"WiFi FW Version: {decoded['VERSION'].strip()}")
        logger.info(f"WiFi Connection Status: {decoded['STATUS'].strip()}")

        logger.info("setting onboarded flag...")
        set_onboarded = {
            "SET_ONBOARDED" : True
        }
        set_onboarded_encoded = cbor2.dumps(set_onboarded)
        await write_with_response(client, SET_DEVICE_INFO_CHARACTERISTIC_UUID, set_onboarded_encoded)

        logger.info("initiating FOTA for all CPU's...")
        initiate_fota = {
            "INITIATE_FOTA" : "ALL"
        }
        initiate_fota_encoded = cbor2.dumps(initiate_fota)
        await write_with_response(client, SET_DEVICE_INFO_CHARACTERISTIC_UUID, initiate_fota_encoded)

        logger.info("sending ping request...")
        PING_REQUEST_CHARACTERISTIC_UUID = "9d1589a6-cea6-4df1-96d9-1697cd4dc202"
        ping = {
            "PING" : "REQUEST"
        }
        ping_encoded = cbor2.dumps(ping)
        await write_with_response(client, PING_REQUEST_CHARACTERISTIC_UUID, ping_encoded)


        # should be empty after reboot as we haven't yet scanned
        await read_wifi_scan_list(client, 99)

        # trigger WiFi scan in a loop every 10s until ctrl-c
        while True:
            logger.info("triggering WiFi scan...")
            TRIGGER_SCAN_CHARACTERISTIC_UUID = "9d1589a6-cea6-4df1-96d9-1697cd4dc601"
            trigger_scan = {
                "SCAN_TRIG" : True
            }
            trigger_scan_encoded = cbor2.dumps(trigger_scan)
            await write_with_response(client, TRIGGER_SCAN_CHARACTERISTIC_UUID, trigger_scan_encoded)
            await asyncio.sleep(10)


async def read_wifi_scan_list(client, count):
        logger.info("reading WiFi scan list...")
        WIFI_SCAN_CHARACTERISTIC_UUID = "9d1589a6-cea6-4df1-96d9-1697cd4dc600"

        for _ in range(count):
            raw = await client.read_gatt_char(WIFI_SCAN_CHARACTERISTIC_UUID)
            decoded = cbor2.loads(raw)
            if not decoded:
                logger.info("no more AP's!")
                return

            key = list(decoded.keys())[0]
            ap = decoded[key]
            logger.info(f"{key}: {ap['NAME']}, rssi: {ap['RSSI']}, security: {ap['SEC_FLAGS']}")


async def clear_bonding(address):
    if not address:
        logger.info("No address provided!")
        return

    # device disconnects when this block exits
    async with BleakClient(address) as client:

        global bleak_client
        bleak_client = client
        # Confirm the device offers the tracker service
        services = client.services
        if SERVICE_UUID not in [s.uuid for s in services]:
            logger.info(f"Service {SERVICE_UUID} not found on device {address}")
            return
        clear_bonding = {
            "CLEAR_BONDING" : True
        }
        clear_bonding_encoded = cbor2.dumps(clear_bonding)
        await write_with_response(client, SET_DEVICE_INFO_CHARACTERISTIC_UUID, clear_bonding_encoded)
        logger.info(f"Cleared bonding, try again")
        await asyncio.Future()


async def connect(address, name, password, flags):
    if not address:
        logger.info("No address provided!")
        return

    # device disconnects when this block exits
    async with BleakClient(address) as client:

        global bleak_client
        bleak_client = client
        # Confirm the device offers the tracker service
        services = client.services
        if SERVICE_UUID not in [s.uuid for s in services]:
            logger.info(f"Service {SERVICE_UUID} not found on device {address}")
            return

        logger.info("enabling notifications...")
        DEVICE_INFO_NOTIFY_UUID = "9d1589a6-cea6-4df1-96d9-1697cd4dc1e8"

        # handle notifications here
        async def device_info_notification_handler(sender: int, data: bytearray):
            decoded = cbor2.loads(data)
            logger.info(f"DeviceInfo notification: {decoded}")
        await client.start_notify(DEVICE_INFO_NOTIFY_UUID, device_info_notification_handler)

        # connect to AP
        logger.info("connecting to AP...")
        WIFI_CONNECT_CHARACTERISTIC_UUID = "9d1589a6-cea6-4df1-96d9-1697cd4dc501"
        if flags:
            wifi_connect = {
                "SSID" : name,
                "PASSWD" : password,
                "SEC_FLAGS" : flags,
            }
        else:
            wifi_connect = {
                "SSID" : name,
                "PASSWD" : password,
            }

        wifi_connect_encoded = cbor2.dumps(wifi_connect)

        try:
            await write_with_response(client, WIFI_CONNECT_CHARACTERISTIC_UUID, wifi_connect_encoded)
        except BleakError as e:
            if "Encryption is insufficient" in str(e):
                #logger.info(f"{e}")
                logger.error(f"Failed due to insufficient encryption (did you 'forget' the device?).")
                # tell device to erase bonding info
                logger.info("Clearing bonding info...")
                await clear_bonding(address)

            elif "Peer removed pairing information" in str(e):
                #logger.info(f"{e}")
                logger.error(f"Failed to connect because the peer removed pairing information. Consider 'forgetting' device and re-pairing.")
            else:
                print(f"An unexpected error occurred: {e}")

        # wait forever until Ctrl-C
        logger.info("waiting for connection...  press <ctrl-c> to exit")
        await asyncio.Future()


async def main():
    if args.device:
        logger.info(f"looking for device: {args.device}...")
    else:
        logger.info(f"looking for any device...")

    address = await scan()
    if address:
        logging_set_filename(address)
        if args.connect:

            if len(args.connect) < 2:
                parser.error("--connect requires at least a NAME and PASSWORD.")
            elif len(args.connect) > 3:
                parser.error("--connect accepts at most three arguments: NAME, PASSWORD, and optionally SEC_FLAGS.")
            
            name, password = args.connect[:2]
            flags = args.connect[2] if len(args.connect) == 3 else None  # Default SEC_FLAGS to None if not provided
            logger.info(f"connecting to: {name}, with password: {password}, and flags: {flags}")
            await connect(address, name, password, flags)

        else:
            logger.info("running service test")
            await service_test(address)
    else:
        logger.info("Device not found!")

try:
    asyncio.run(main())
except KeyboardInterrupt:
    logger.info("Caught Ctrl-C (SIGINT), exiting.")

logger.info(f"Test complete!")
