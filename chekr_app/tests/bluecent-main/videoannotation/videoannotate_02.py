#harekrsna

import cv2
import time
from datetime import datetime

# Path to the video file
video_path = "testvid1.mp4"

# Create a VideoCapture object
cap = cv2.VideoCapture(video_path)

# Get the total number of frames in the video
total_frames = int(cap.get(cv2.CAP_PROP_FRAME_COUNT))

# Get the start time in milliseconds
start_time = int(time.time() * 1000)

# Get the width and height of the video frames
width = int(cap.get(cv2.CAP_PROP_FRAME_WIDTH))
height = int(cap.get(cv2.CAP_PROP_FRAME_HEIGHT))

print("Video File Propeties:\n")
print("Input Video File:", video_path)
print("Total Frames In Video:", total_frames)
print("Video Resolution:", width, "x", height)

# Create a VideoWriter object to save the annotated video
output_path = "output_2.mp4"
fourcc = cv2.VideoWriter_fourcc(*"mp4v")
fps = cap.get(cv2.CAP_PROP_FPS)
output = cv2.VideoWriter(output_path, fourcc, fps, (width, height))

# Iterate over each frame
for frame_num in range(total_frames):
    # Read the frame
    ret, frame = cap.read()

    if not ret:
        break

    # Calculate the timestamp for the frame
    timestamp = start_time + (frame_num * 1000)

    # Convert the timestamp to a string
    #timestamp_str = str(timestamp)

    # Convert the timestamp to datetime object
    datetime_obj = datetime.fromtimestamp(timestamp / 1000)
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
