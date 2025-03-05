import asyncio
from bleak import BleakClient, BleakScanner
import random
import time
from epochtime import send_epoch_time, get_epoch_time
from record import start_recording_session, stop_recording_session, read_recording_session_imu_details, read_recording_session_imu_data
from record import read_recording_session_activity_details, read_recording_session_activity_data
from command import send_command
from misc_cmds import read_dashboard_info, set_dog_collar_position, set_dog_size, pretty_print_dashboard_info
import argparse
import log_config
import logging
log_config.setup_logging()
logger = logging.getLogger('d1')
from log_config import logging_set_filename

SERVICE_UUID = "00c74f02-d1bc-11ed-afa1-0242ac120002"

timestamp = None
bleak_client = None

# defaults
iterations = 10
max_time = 20
random_time = False

# Initiate the parser
parser = argparse.ArgumentParser()

# Add long and short argument
parser.add_argument("--iterations", "-i", help="number of iterations to run", required=True)
parser.add_argument("--time", "-t", help="time for recording sessions", required=True)
parser.add_argument("--random", "-r", help="randomize time", action="store_true")
parser.add_argument("--device", "-d", help="device name (BLE advertisement)")
parser.add_argument("--bypass", "-b", help="bypass Start/Stop recording to fetch in-progress recording <name>")

# Read arguments from the command line
args = parser.parse_args()

random_time = args.random

iterations = int(args.iterations)
logger.info(f"iterations: {args.iterations}")

max_time = int(args.time)
if random_time:
    logger.info(f"max time: {args.time}")
else:
    logger.info(f"time: {args.time}")


async def scan():
    devices = await BleakScanner.discover()
    for device in devices:
        if args.device:
            if device.name and device.name == args.device:
                logger.info(f"found specified: {device.name} {device.address}")
                return device.address
        else:
            logger.info(f"{device.name} {device.address}")
            if device.name and ("NDC" in device.name or "Petivity" in device.name):
                logger.info(f"found general: {device.name} {device.address}")
                return device.address
    return None

async def recording_session(address):
    if not address:
        logger.info("No address provided!")
        return

    # device disconnects when this block exits
    async with BleakClient(address) as client:

        global bleak_client
        bleak_client = client
        # Check if the device offers the specified service
        services = await client.get_services()
        if SERVICE_UUID not in [s.uuid for s in services]:
            logger.info(f"Service {SERVICE_UUID} not found on device {address}")
            return

        value = send_epoch_time()
        response = await send_command(client, value)
        logger.info(f"send_epoch_time response: {response.hex(' ')}")

        value = get_epoch_time()
        response = await send_command(client, value)
        logger.info(f"get_epoch_time response: {response.hex(' ')}")

        # TODO: verify CRC, and it's within spec via an unpack method

        value = set_dog_collar_position()
        response = await send_command(client, value)
        logger.info(f"set_dog_collar_position response: {response.hex(' ')}")

        value = set_dog_size()
        response = await send_command(client, value)
        logger.info(f"set_dog_size response: {response.hex(' ')}")

        value = read_dashboard_info()
        response = await send_command(client, value)
        pretty_print_dashboard_info(response[5:-2])

        global timestamp
        # UID is timestamp
        utc_time = int(round(time.time() * 1000))
        timestamp = (utc_time).to_bytes(8, byteorder='big')
        logger.info(f"Current time: {timestamp.hex(' ')}")
        # this is the filename the device saves this session as
        filename = ''.join(f'{byte:02X}' for byte in timestamp)
        logger.info(f"Device filename: {filename}")

        if  not args.bypass:
            await start_recording_session(client, timestamp)

            # let it record for a bit...
            if random_time:
                delay = random.uniform(1, max_time)
            else:
                delay = max_time
            logger.info(f"sleeping for: {delay:.2f} seconds")
            await asyncio.sleep(delay)

            logger.info(f"stopping recording")
            await stop_recording_session(client, timestamp)
        else:
            # bypass start and stop recording to fetch in-progress recording
            filename = args.bypass.strip()
            logger.info(f"Bypassed Start/Stop, fetching {filename}")
            timestamp = bytes.fromhex(filename)

        logger.info(f"**************** RAW IMU DATA ***********")
        num_records = await read_recording_session_imu_details(client, timestamp)
        for record in range(num_records):
            await read_recording_session_imu_data(client, timestamp, record)

        logger.info(f"**************** ACTIVITY DATA ***********")
        num_records = await read_recording_session_activity_details(client, timestamp)
        for record in range(num_records):
            await read_recording_session_activity_data(client, timestamp, record)


async def main():
    if args.device:
        logger.info(f"looking for device: {args.device}...")
    else:
        logger.info(f"looking for any device...")

    address = await scan()
    if address:
        logging_set_filename(address)
        for i in range(iterations):
            logger.info(f"******** run {i} ********")
            await recording_session(address)
    else:
        logger.info("Device not found!")

asyncio.run(main())
logger.info(f"Test complete!")
