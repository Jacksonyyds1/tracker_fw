#harekrsna

import cv2
import time
from datetime import datetime

# Path to the video file
#video_path = "testvid1.mp4"

# Create a VideoCapture object
#cap = cv2.VideoCapture(video_path)

# Get the start time in milliseconds
start_time = int(time.time() * 1000)

# Set video dimensions and frames per second
width = 640
height = 480
fps = 25

# Create a VideoWriter object to save the annotated video
output_path = "output_2.mp4"
fourcc = cv2.VideoWriter_fourcc(*"mp4v")
#fps = cap.get(cv2.CAP_PROP_FPS)
out = cv2.VideoWriter(output_path, fourcc, fps, (width, height))

# Open the default camera and start recording
cap = cv2.VideoCapture(0)

frame_num = 0
while True:
    # Capture a frame from the camera
    ret, frame = cap.read()

    frame_num = frame_num + 1
    if ret:
        # Calculate the timestamp for the frame
        #timestamp = start_time + (frame_num * 1000)
        timestamp = time.time()

        # Convert the timestamp to a string
        #timestamp_str = str(timestamp)

        # Convert the timestamp to datetime object
        datetime_obj = datetime.fromtimestamp(timestamp)
        # Format the datetime object as "dd-mm-yyyy hh:mm:ss.000"
        timestamp_str = datetime_obj.strftime('%Y-%m-%d %H:%M:%S.%f')[:-3]
                                    
        # Add the timestamp to the frame
        cv2.putText(frame, timestamp_str, (10, (height - 120)), cv2.FONT_HERSHEY_SIMPLEX, 1, (0, 255, 0), 2)
        
        # Write the frame to the video file
        out.write(frame)

        # Display the resulting frame
        cv2.imshow('frame', frame)

        # Press 'q' to stop recording and exit
        if cv2.waitKey(1) & 0xFF == ord('q'):
            break
    else:
        break
  

# Release the VideoCapture and VideoWriter objects
cap.release()
out.release()

# Close the OpenCV windows
cv2.destroyAllWindows()
