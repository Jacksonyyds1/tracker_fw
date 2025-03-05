import struct
from utils import pack_command, HEADER_FORMAT, CRC_FORMAT, FT_REQUEST, SB_MOBILE_APP_TO_DEVICE, WRITE_CHARACTERISTIC_UUID
from command import send_command
from datetime import datetime
import logging
logger = logging.getLogger('d1')

# commands
START_STOP_RECORDING_SESSION = 16

READ_RECORDING_SESSION_IMU_DETAILS = 17
READ_RECORDING_SESSION_IMU_DATA = 18

READ_RECORDING_SESSION_ACTIVITY_DETAILS = 19
READ_RECORDING_SESSION_ACTIVITY_DATA = 20

# start/stop byte + UID
START_STOP_RECORDING_SESSION_FORMAT = HEADER_FORMAT + "B8s" + CRC_FORMAT

READ_RECORDING_SESSION_IMU_DETAILS_FORMAT = HEADER_FORMAT + "8sB" + CRC_FORMAT
READ_RECORDING_SESSION_IMU_DATA_FORMAT = HEADER_FORMAT + "8sHHB" + CRC_FORMAT

READ_RECORDING_SESSION_ACTIVITY_DETAILS_FORMAT = HEADER_FORMAT + "8sB" + CRC_FORMAT
READ_RECORDING_SESSION_ACTIVITY_DATA_FORMAT = HEADER_FORMAT + "8sHHB" + CRC_FORMAT

async def start_recording_session(client, epoch_time_bytes):
    flag = bytes([1])
    data = flag + epoch_time_bytes

    # Calculate the length for the packed command
    frame_len = struct.calcsize(START_STOP_RECORDING_SESSION_FORMAT)
    packed_data = pack_command(SB_MOBILE_APP_TO_DEVICE, frame_len, FT_REQUEST, START_STOP_RECORDING_SESSION, data)

    response = await send_command(client, packed_data)
    logger.info(f"start recording response: {response.hex(' ')}")
    return epoch_time_bytes

async def stop_recording_session(client, timestamp):

    flag = bytes([0])
    data = flag + timestamp

    # Calculate the length for the packed command
    frame_len = struct.calcsize(START_STOP_RECORDING_SESSION_FORMAT)
    packed_data = pack_command(SB_MOBILE_APP_TO_DEVICE, frame_len, FT_REQUEST, START_STOP_RECORDING_SESSION, data)
    response = await send_command(client, packed_data)
    logger.info(f"stop recording response: {response.hex(' ')}")

async def read_recording_session_imu_details(client, timestamp):

    data = timestamp + bytes([0])

    # Calculate the length for the packed command
    frame_len = struct.calcsize(READ_RECORDING_SESSION_IMU_DETAILS_FORMAT)
    packed_data = pack_command(SB_MOBILE_APP_TO_DEVICE, frame_len, FT_REQUEST, READ_RECORDING_SESSION_IMU_DETAILS, data)
    response = await send_command(client, packed_data)
    logger.info(f"read recording session response: {response.hex(' ')}")
    num_records = struct.unpack_from('>H', response, 13)[0]
    logger.info(f"num_records: {num_records}")
    return num_records        

async def read_recording_session_imu_data(client, uid, record_num):

    record_num_be = struct.pack('>H', record_num)
    reserved = bytes([0, 0, 0])

    data = uid + reserved + record_num_be
 
    # Calculate the length for the packed command
    frame_len = struct.calcsize(READ_RECORDING_SESSION_IMU_DATA_FORMAT)
    packed_data = pack_command(SB_MOBILE_APP_TO_DEVICE, frame_len, FT_REQUEST, READ_RECORDING_SESSION_IMU_DATA, data)
    response = await send_command(client, packed_data)
    #logger.info(f"response len: {len(response)}")
    #logger.info(f"raw data: {response.hex(' ')}")
    parsed_data = parse_raw_imu_data_record(response[1:-2])
    logger.info(f"read recording session data for record {record_num}: {parsed_data}")




async def read_recording_session_activity_details(client, timestamp):

    data = timestamp + bytes([0])

    # Calculate the length for the packed command
    frame_len = struct.calcsize(READ_RECORDING_SESSION_ACTIVITY_DETAILS_FORMAT)
    packed_data = pack_command(SB_MOBILE_APP_TO_DEVICE, frame_len, FT_REQUEST, READ_RECORDING_SESSION_ACTIVITY_DETAILS, data)
    response = await send_command(client, packed_data)
    logger.info(f"read activity details response: {response.hex(' ')}")
    num_records = struct.unpack_from('>H', response, 13)[0]
    logger.info(f"num_records: {num_records}")
    return num_records        

async def read_recording_session_activity_data(client, uid, record_num):

    record_num_be = struct.pack('>H', record_num)
    reserved = bytes([0, 0, 0])

    data = uid + reserved + record_num_be
 
    # Calculate the length for the packed command
    frame_len = struct.calcsize(READ_RECORDING_SESSION_ACTIVITY_DATA_FORMAT)
    packed_data = pack_command(SB_MOBILE_APP_TO_DEVICE, frame_len, FT_REQUEST, READ_RECORDING_SESSION_ACTIVITY_DATA, data)
    response = await send_command(client, packed_data)
    logger.info(f"activity data len: {len(response)}")
    logger.info(f"activity data: {response.hex(' ')}")
    parse_activity_data_record(response[1:-2])
    logger.info(f"read recording session data for record {record_num}")




def parse_activity_data_record(binary_data):
    # Format string for big-endian part: record_num
    format_string_big_endian = '>I'
    # Length of the big-endian part
    big_endian_length = struct.calcsize(format_string_big_endian)

    # Unpack big-endian part (record_num)
    record_num = struct.unpack(format_string_big_endian, binary_data[:big_endian_length])[0]

    # Format string for the little-endian parts: timestamp, start_byte, model_type, activity_type, rep_count, joint_health
    format_string_little_endian = '<QBBBfB'
    # Unpack little-endian parts
    little_endian_data = struct.unpack(format_string_little_endian, binary_data[big_endian_length:])
    timestamp, start_byte, model_type, activity_type, rep_count, joint_health = little_endian_data

    timestamp_seconds = timestamp / 1000.0
    formatted_timestamp = datetime.utcfromtimestamp(timestamp_seconds).strftime('%Y-%m-%d %H:%M:%S.%f')[:-3]  # Format with milliseconds

    # Logging the unpacked data
    logger.info(f"record_num: {record_num}, timestamp: {formatted_timestamp}")
    logger.info(f"sb: {start_byte}, type: {model_type}, activity: {activity_type}, rep: {rep_count:.2f}, joint: {joint_health}")


def parse_raw_imu_data_record(binary_data):
    RAW_FRAMES_PER_RECORD = 10
    # Define the format strings
    big_endian_format = '>IQ'
    little_endian_format = '6f' * RAW_FRAMES_PER_RECORD
    
    # Calculate the sizes for each part
    big_endian_size = struct.calcsize(big_endian_format)
    little_endian_size = struct.calcsize(little_endian_format)
    
    # Ensure the binary data has the expected size
    expected_size = big_endian_size + little_endian_size
    if len(binary_data) != expected_size:
        raise ValueError(f"Binary data has length {len(binary_data)}, but expected {expected_size}")

    # Unpack the big-endian part
    unpacked_big_endian = struct.unpack(big_endian_format, binary_data[:big_endian_size])
    
    # Unpack the little-endian part
    unpacked_little_endian = struct.unpack(little_endian_format, binary_data[big_endian_size:])
    
    timestamp_seconds = unpacked_big_endian[1] / 1000.0
    formatted_timestamp = datetime.utcfromtimestamp(timestamp_seconds).strftime('%Y-%m-%d %H:%M:%S.%f')[:-3]  # Format with milliseconds

    record = {
        "record_num": unpacked_big_endian[0],
        "timestamp": formatted_timestamp,
        "raw_data": []
    }
    
    # Split and organize the frame data
    for i in range(RAW_FRAMES_PER_RECORD):
        start = i * 6  # 6 values per frame
        end = start + 6
        frame_data = unpacked_little_endian[start:end]
        frame = {
            "ax": f"{frame_data[0]:.2f}",
            "ay": f"{frame_data[1]:.2f}",
            "az": f"{frame_data[2]:.2f}",
            "gx": f"{frame_data[3]:.2f}",
            "gy": f"{frame_data[4]:.2f}",
            "gz": f"{frame_data[5]:.2f}",
        }
        record["raw_data"].append(frame)

    return record
