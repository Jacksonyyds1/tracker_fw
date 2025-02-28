
# The file takes video and binary files as input,
# The video will be annotated with timestamp and,
# The binary file will be parsed and csv file is generated.

import csv
import time
import struct
import os
import base64
import datetime
import cv2

# Convert currentmillis to datetime format.
def currentmillis_2_datetime(currentmillis):
    seconds_ts = currentmillis / 1000.0  # Convert milliseconds to seconds, also holds milleseconds in timestamp
    timestamp = datetime.datetime.fromtimestamp(seconds_ts)
    return timestamp

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

# Function to get delta time (datetime format) to add to each sample in the record.
def get_delta_time(samples_per_sec, multiplier):
    milliseconds_to_add = (1000.0 * multiplier)/samples_per_sec
    delta_time = datetime.timedelta(milliseconds=milliseconds_to_add)
    return delta_time

# Function to parse 'NDC Cloud Data' file lines and output data as CSV file. 
def imu_rec_unpack(imu_rec, output_file):
    #Validate Imu record length
    if len(imu_rec) != 255:
        print("invalid length: 255 data are not there ")
        return
    temp_data = [0]*60      # For the 60 floats in a record.
    #unpack record num
    rnum = struct.unpack('<I',imu_rec[1:5])[0]
    #print(f"Record number {rnum}")
    #unpack time stamp
    ts = struct.unpack('<Q',imu_rec[5:13])[0]
    #print(f"Time Stamp {ts}")
    ts_ms = currentmillis_2_datetime(ts) 
    fieldnames = ["timestamp", "rec num", "ax", "ay", "az", "gx", "gy", "gz"]
    for i in range(60): 
        start_bit = 13 + (i*4)
        end_bit = 17 + (i*4)
        z = struct.unpack('<f',imu_rec[start_bit:end_bit])[0] # convert bytes to float.
        temp_data[i] = z
    data=[]
    for i in range(0,len(temp_data),6):
        ts_ms_sample = ts_ms + get_delta_time(25, (i/6))    # Calculate timestamp for each sample
        row_data = {
            # "suid":suid if i == 0 else None,            
            # "timestamp": ts_ms if i == 0 else None, # (i/6) * 40
            "timestamp": ts_ms_sample,
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
def open_binary_input_file(filename):
    try:
        with open(filename, 'r') as file:
            lines = file.readlines()
            num_lines = len(lines)
            samples = num_lines * 10
            file_size = os.path.getsize(filename)

            # Print file summary.            
            print("\nBinary File Summary:")
            print("---------------------")
            print("File Name: {}".format(filename))
            print("Number of Records: {}".format(num_lines))
            print("Number of Samples: {}".format(samples))
            print("File Size: {} bytes".format(file_size))
            print("---------------------\n")

            # Create output filename.
            out_filename = filename + ".csv"
            # Parse each line and write to CSV output file.
            for line in lines:
                # Convert base64 data to binary data.
                bin_line = base64_to_binary(line)
                # Print the line in hex format. 
                #print_binary_as_hex(bin_line)
                # Parse the line and write/append data to CSV.
                imu_rec_unpack(bin_line, out_filename) 

            print("Binary file conversion to CSV complete!\n")

    except FileNotFoundError:
        print("File not found.")

# Try to open the given NDC Cloud Data file.
def open_video_input_file(filename):
    try:
        with open(filename, 'r') as file:
            # Create a VideoCapture object
            cap = cv2.VideoCapture(filename)

            # Get the total number of frames in the video
            total_frames = int(cap.get(cv2.CAP_PROP_FRAME_COUNT))

            # Get the start time in milliseconds
            start_time = int(filename[:-4])

            # Get the width and height of the video frames
            width = int(cap.get(cv2.CAP_PROP_FRAME_WIDTH))
            height = int(cap.get(cv2.CAP_PROP_FRAME_HEIGHT))

            # Get the frames per second (FPS)
            fps = cap.get(cv2.CAP_PROP_FPS)

            print("\nVideo File Propeties:")
            print("---------------------")
            print("Input Video File:", filename)
            print("Total Frames In Video:", total_frames)
            print("Video Resolution:", width, "x", height)
            print("Frames Per Second (FPS):", fps)
            print("---------------------\n")

            # Create a VideoWriter object to save the annotated video
            output_path = "output_2.mp4"
            fourcc = cv2.VideoWriter_fourcc(*"mp4v")
            fps = cap.get(cv2.CAP_PROP_FPS)
            output = cv2.VideoWriter(output_path, fourcc, fps, (width, height))

            time_step_per_frame = 1/fps

            # Iterate over each frame
            for frame_num in range(total_frames):   # first frame is numbered 0
                # Read the frame
                ret, frame = cap.read()

                if not ret:
                    break

                # Calculate the timestamp for the frame
                timestamp = start_time + ((frame_num * 1000) * time_step_per_frame)

                # Convert the timestamp to a string
                #timestamp_str = str(timestamp)

                # Convert the timestamp to datetime object
                datetime_obj = datetime.datetime.fromtimestamp(timestamp / 1000)
                # Format the datetime object as "dd-mm-yyyy hh:mm:ss.000"
                timestamp_str = datetime_obj.strftime("%d-%m-%Y %H:%M:%S.%f")[:-3]

                # Add the timestamp to the frame
                cv2.putText(frame, timestamp_str, (10, (height - 120)), cv2.FONT_HERSHEY_SIMPLEX, 1, (0, 255, 0), 2)

                # Write the annotated frame to the output video
                output.write(frame)

                # Display the frame (optional)
            ##    cv2.imshow('Frame', frame)
            ##    if cv2.waitKey(1) & 0xFF == ord('q'):
            ##        break

            # Release the VideoCapture and VideoWriter objects
            cap.release()
            output.release()

            # Close the OpenCV windows
            cv2.destroyAllWindows()

            print("Video annotation complete!\n")


    except FileNotFoundError:
        print("File not found!")

# Example usage 1687937378022.bin 1687937378022.mp4

# Prompt user for video filename
file_name = input("Please enter the video filename with extension, Example: 1690194788816.mp4\n\
Press 'Return' to skip video annotation.\n\
Enter the video filename: ")
if file_name.strip():  # Check if the user entered a non-empty string
    open_video_input_file(file_name)

# Prompt user for binary filename
file_name = input("Please enter the binary filename with extension, Example: 1690194788816.bin\n\
Press 'Return to skip binary file parsing.\n\
Enter the binary filename: ")
if file_name.strip():  # Check if the user entered a non-empty string
    open_binary_input_file(file_name)
