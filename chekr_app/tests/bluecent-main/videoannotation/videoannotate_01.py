#HareKrsna

import cv2
import time

# Path to the video file
video_path = "tenmin (2).mp4"

# Create a VideoCapture object
cap = cv2.VideoCapture(video_path)

# Get the total number of frames in the video
total_frames = int(cap.get(cv2.CAP_PROP_FRAME_COUNT))

# Get the start time in milliseconds
start_time = int(time.time() * 1000)

# Iterate over each frame
for frame_num in range(total_frames):
    # Read the frame
    ret, frame = cap.read()

    if not ret:
        break

    # Calculate the timestamp for the frame
    timestamp = start_time + (frame_num * 1000)

    # Convert the timestamp to a string
    timestamp_str = str(timestamp)

    # Add the timestamp to the frame
    cv2.putText(frame, timestamp_str, (10, 30), cv2.FONT_HERSHEY_SIMPLEX, 1, (0, 255, 0), 2)

    # Display the frame (optional)
    cv2.imshow('Frame', frame)
    if cv2.waitKey(1) & 0xFF == ord('q'):
        break

# Release the VideoCapture object and close the windows
cap.release()
cv2.destroyAllWindows()
