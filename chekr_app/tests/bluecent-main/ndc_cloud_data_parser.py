import csv
#import time
import struct
import os
import base64
import datetime

def convert_unix_epoch(milliseconds):
    seconds = milliseconds / 1000.0  # Convert milliseconds to seconds
    timestamp = datetime.datetime.fromtimestamp(seconds)
    return timestamp

# Example usage
# epoch_milliseconds = 1624285200000  # Example Unix epoch time in milliseconds
# readable_time = convert_unix_epoch(epoch_milliseconds)
# print(readable_time)

# Function to convert base64 srting to binary data.
def base64_to_binary(base64_string):
    binary_data = base64.b64decode(base64_string)
    return binary_data

# Function to print binary data stream in hex format.
def print_binary_as_hex(binary_data):
    tstr = ""
    for byte in binary_data:
        hex_value = hex(byte)[2:].zfill(2)  # Convert byte to hexadecimal and pad with zero if necessary
        tstr = tstr + hex_value.upper()
    print(tstr)

# Function to parse 'NDC Cloud Data' file lines and output data as CSV file. 
def imu_rec_unpack(imu_rec, output_file):
    #Validate Imu record length
    if len(imu_rec) != 255:
        print("invalid length: 255 data are not there ")
        return
    temp_data = [0]*60
    #unpack record num
    rnum = struct.unpack('<I',imu_rec[1:5])[0]
    print(f"Record number {rnum}")
    #unpack time stamp
    ts = struct.unpack('<Q',imu_rec[5:13])[0]
    #print(f"Time Stamp {ts}")
    ts_ms = convert_unix_epoch(ts) 
    fieldnames = ["timestamp", "rec num", "ax", "ay", "az", "gx", "gy", "gz"]
    for i in range(60): 
        start_bit = 13 + (i*4)
        end_bit = 17 + (i*4)
        z = struct.unpack('<f',imu_rec[start_bit:end_bit])[0]
        temp_data[i] = z
    data=[]
    for i in range(0,len(temp_data),6):
        row_data = {
            # "suid":suid if i == 0 else None,
            "timestamp": ts_ms if i == 0 else None,
            "rec num": rnum if i == 0 else None,
            "ax": temp_data[i],
            "ay": temp_data[i+1],
            "az": temp_data[i+2],
            "gx": temp_data[i+3],
            "gy": temp_data[i+4],
            "gz": temp_data[i+5]
        }
        data.append(row_data)
        if i == 54:
            rnum += 1
    # Open file in append mode, else create a file.        
    with open(output_file, mode="a+", newline="") as csv_file:
        writer = csv.DictWriter(csv_file, fieldnames=fieldnames)
        if(csv_file.tell() == 0):
            writer.writeheader()
        for row in data:
            writer.writerow(row)

# Try to open the given NDC Cloud Data file.
def open_ndc_input_file(filename):
    try:
        with open(filename, 'r') as file:
            lines = file.readlines()
            num_lines = len(lines)
            file_size = os.path.getsize(filename)

            # Print file summary.            
            print("File Summary:")
            print("--------------")
            print("File Name: {}".format(filename))
            print("Number of Lines: {}".format(num_lines))
            print("File Size: {} bytes".format(file_size))
            print("--------------")

            # Create output filename.
            out_filename = filename + ".csv"
            # Parse each line and write to CSV output file.
            for line in lines:
                # Convert base64 data to binary data.
                bin_line = base64_to_binary(line)
                # Print the line in hex format. 
                print_binary_as_hex(bin_line)
                # Parse the line and write/append data to CSV.
                imu_rec_unpack(bin_line, out_filename)                 
    except FileNotFoundError:
        print("File not found.")


#test_data1()
# Example usage
file_name = input("Enter the file name: ")
open_ndc_input_file(file_name)
