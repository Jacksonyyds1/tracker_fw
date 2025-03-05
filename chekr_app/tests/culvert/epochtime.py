import struct
import time
from utils import pack_command, HEADER_FORMAT, CRC_FORMAT, FT_REQUEST, SB_MOBILE_APP_TO_DEVICE
import logging
logger = logging.getLogger('d1')

# commands
GET_EPOCH_RTC = 5
SET_EPOCH_RTC = 6

# formats
SET_EPOCH_TIME_FORMAT = HEADER_FORMAT + "8s" + CRC_FORMAT
GET_EPOCH_TIME_FORMAT = HEADER_FORMAT + "B" + CRC_FORMAT

def send_epoch_time():
    # Convert the epoch time to bytes and ensure it's 8 bytes long
    utc_time = int(round(time.time() * 1000))
    epoch_time_bytes = (utc_time).to_bytes(8, byteorder='big')
    logger.info(f"Current time: {epoch_time_bytes.hex(' ')}")

    # Calculate the length for the packed command
    frame_len = struct.calcsize(SET_EPOCH_TIME_FORMAT)
    packed_data = pack_command(SB_MOBILE_APP_TO_DEVICE, frame_len, FT_REQUEST, SET_EPOCH_RTC, epoch_time_bytes)
    return packed_data

def get_epoch_time():
    frame_len = struct.calcsize(GET_EPOCH_TIME_FORMAT)
    reserved = bytes([0])
    packed_data = pack_command(SB_MOBILE_APP_TO_DEVICE, frame_len, FT_REQUEST, GET_EPOCH_RTC, reserved)
    return packed_data
