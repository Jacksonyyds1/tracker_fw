import struct
from utils import pack_command, HEADER_FORMAT, CRC_FORMAT, FT_REQUEST, SB_MOBILE_APP_TO_DEVICE
import logging
logger = logging.getLogger('d1')

# commands
READ_DASHBOARD_INFO = 22
SET_DOG_COLLAR_POSITION = 4
SET_DOG_SIZE = 26

# formats
READ_DASHBOARD_INFO_FORMAT = HEADER_FORMAT + "B" + CRC_FORMAT
SET_DOG_COLLAR_POSITION_FORMAT = HEADER_FORMAT + "B" + CRC_FORMAT
SET_DOG_SIZE_FORMAT = HEADER_FORMAT + "B" + CRC_FORMAT

def read_dashboard_info():
    frame_len = struct.calcsize(READ_DASHBOARD_INFO_FORMAT)
    reserved = bytes([0])
    packed_data = pack_command(SB_MOBILE_APP_TO_DEVICE, frame_len, FT_REQUEST, READ_DASHBOARD_INFO, reserved)
    return packed_data

def set_dog_collar_position():
    frame_len = struct.calcsize(SET_DOG_COLLAR_POSITION_FORMAT)
    position = bytes([12])
    packed_data = pack_command(SB_MOBILE_APP_TO_DEVICE, frame_len, FT_REQUEST, SET_DOG_COLLAR_POSITION, position)
    return packed_data

def set_dog_size():
    frame_len = struct.calcsize(SET_DOG_SIZE_FORMAT)
    size = bytes([3])   # small size
    packed_data = pack_command(SB_MOBILE_APP_TO_DEVICE, frame_len, FT_REQUEST, SET_DOG_COLLAR_POSITION, size)
    return packed_data

def pretty_print_dashboard_info(data: bytes):

    logger.info(f"dashboard data: {data.hex(' ')}")

    # Check if the data has the expected length
    if len(data) != 19:  # 19 bytes based on the format string
        raise ValueError(f"Data does not match the expected length. Got {len(data)} bytes instead of 19.")

    # Unpack the data    
    format_str = '>9B4BHhH'
    fields = struct.unpack(format_str, data)
    
    # Extract the fields
    device_name = fields[0:9]
    app_fw_major = fields[9]
    app_fw_minor = fields[10]
    app_fw_patch = fields[11]
    battery_level = fields[12]  # Assuming this is an 8-bit value
    battery_voltage = fields[13]  # 16-bit big-endian value
    input_current = fields[14]  # 16-bit big-endian value
    temperature = fields[15]/100.0  # 16-bit big-endian value
    
    # Pretty-print the results
    logger.info(f"Device Name: {''.join([chr(x) for x in device_name])}")
    logger.info(f"App Firmware Version: {app_fw_major}.{app_fw_minor}.{app_fw_patch}")
    logger.info(f"Battery Level: {battery_level} %")
    logger.info(f"Battery Voltage: {battery_voltage} mV")
    logger.info(f"Charging Current: {input_current} mA")
    logger.info(f"Temperature: {temperature:.2f}°F")
