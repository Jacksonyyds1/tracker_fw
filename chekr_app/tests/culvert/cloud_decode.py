import struct
from datetime import datetime
import argparse
import struct
import base64
import sys

RAW_FRAMES_PER_RECORD = 10

def parse_raw_imu_data_record(binary_data):
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

def parse_raw_activity_data_record(binary_data):
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
    print(f"Record Number: {record_num}")
    print(f"Timestamp: {formatted_timestamp}")

    # Logging the unpacked data
    print(f"Model Type: {model_type}, Current Activity: {activity_type}, Repetition: {rep_count:.2f}, JH: {joint_health}")

def main():
    parser = argparse.ArgumentParser(description="decode recorded data session obtained from the cloud")
    parser.add_argument("--filename", "-f", help="filename", required=True)
    parser.add_argument("--type", "-t", help="type ('activity' or 'imu')", required=True)
    args = parser.parse_args()
    filename = args.filename
    type = args.type
    if type != "activity" and type != "imu":
         print("type must be 'activity' or 'imu'")
         sys.exit(-1)
    print(f"opening: {filename}")

    with open(filename, 'r') as f:
        line_number = 0
        for line in f:
            encoded_data = line.strip()

            # Decode the base64 data for each line
            try:
                decoded_data = base64.b64decode(encoded_data, validate=True)
            except Exception as e:
                print(f"Error decoding line: {e}")

            if type == "imu":
                # parse IMU record skipping header and checksum
                parsed_data = parse_raw_imu_data_record(decoded_data[1:-2])
                print(f"imu record {line_number}: {parsed_data}")

            elif type == "activity":
                # parse Activity record skipping header and checksum
                print(f"parsing activity record {line_number}:")
                parse_raw_activity_data_record(decoded_data[1:-2])

            line_number += 1



if __name__ == "__main__":
    main()
