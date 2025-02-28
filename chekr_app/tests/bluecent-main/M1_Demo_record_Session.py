# HareKrsna

import cv2
import time
import struct
import asyncio
import ble_comm
import chekrapplink as ckr
dev_name = 1122
comm = ble_comm.BleComm('indata.json')
session_uid = int(time.time() * 1000)
output_file = "output\\" + str(dev_name)+"_" +str(session_uid) + ".avi"

async def dev_connect():    
    await comm.connect()
    print("Connected to device")

async def dev_disconnect():    
    await comm.disconnect()
    print("Disconnected")

async def start_rec_ses():
    while 1:
        #set epoch time 
        epoch_time_ms = int(time.time() * 1000)
        dat = struct.pack('>Q', epoch_time_ms)
        tx_fr = ckr.set_epoch_time_frame(dat)
        await comm.write(tx_fr)
        time.sleep(0.1)
        rx_fr = await comm.read()
        tdata = ckr.CommFrame.from_bytes(rx_fr)  #parsing in byte array
        hex_str = ' '.join(format(x, '02x') for x in rx_fr) #byte array to hex
        print("Set epoch time",hex_str)

        #get epoch time
        data = bytes.fromhex('00')
        tx_fr = ckr.get_epoch_time_frame(data) # from mobile to device
        await comm.write(tx_fr)
        time.sleep(0.1)
        rx_fr = await comm.read()    #from device to mobile
        tdata = ckr.CommFrame.from_bytes(rx_fr)    
        curr_epoch_time_ms = int(time.time() * 1000)
        get_epoch_time_ms = struct.unpack('>Q', tdata.data)[0]
        t_diff = curr_epoch_time_ms - get_epoch_time_ms
        hex_str = ' '.join(format(x, '02x') for x in rx_fr)
        print("get epoch time",hex_str)
        print(f"Time difference: {t_diff}")

        if t_diff < 150:
            break

    """Recording Start"""
    #test for Start/Stop recording sessiom
    print("Session ID : ",session_uid)
    data = bytearray()
    data.append(0x01)
    data += struct.pack('>Q', session_uid)
    tx_fr = ckr.start_stop_rec_sess(data)
    hex_str2 = ' '.join(format(x, '02x') for x in tx_fr) #byte array to hex
    print("strt",hex_str2)
    await comm.write(tx_fr)
    time.sleep(0.1)
    rx_fr = await comm.read()
    tdata = ckr.CommFrame.from_bytes(rx_fr)  #parsing in byte array
    hex_str = ' '.join(format(x, '02x') for x in rx_fr) #byte array to hex
    print("start",hex_str)

async def stop_rec_ses():
     #test for Start/Stop recording sessiom
    data = bytearray()
    data.append(0x00)
    data += struct.pack('>Q', session_uid)
    tx_fr = ckr.start_stop_rec_sess(data)
    hex_str1 = ' '.join(format(x, '02x') for x in tx_fr) #byte array to hex
    print("stp",hex_str1)
    await comm.write(tx_fr)
    time.sleep(0.1)
    rx_fr = await comm.read()
    tdata = ckr.CommFrame.from_bytes(rx_fr)  #parsing in byte array
    hex_str = ' '.join(format(x, '02x') for x in rx_fr) #byte array to hex
    print("stop",hex_str)

    #Get session detial
    data = bytearray()
    data += struct.pack('>Q', session_uid)#uid
    data.append(0x00)#reser
    tx_fr = ckr.read_rec_details(data)
    await comm.write(tx_fr)
    time.sleep(0.1)
    rx_fr = await comm.read()
    tdata = ckr.CommFrame.from_bytes(rx_fr)  #parsing in byte array
    hex_str = ' '.join(format(x, '02x') for x in rx_fr) #byte array to hex
    print("session detial 1",hex_str)
    no = int.from_bytes(tdata.data[8:10], "big") #int(tdata.data[8:10])
    print("number", no)


# Set video dimensions and frames per second
width = 640
height = 480
fps = 30

# Define video codec and create VideoWriter object
fourcc = cv2.VideoWriter_fourcc(*'XVID')
out = cv2.VideoWriter(output_file, fourcc, fps, (width, height))

async def test_method():
    #Start recording 
    await dev_connect()
    await start_rec_ses()

    # Open the default camera and start recording
    cap = cv2.VideoCapture(0)

    video_first_fr_ts = int(time.time() * 1000)
    print("Video Start Frame Timestamp: ", video_first_fr_ts) 

    while True:
        # Capture a frame from the camera
        ret, frame = cap.read()

        if ret:
            # Write the frame to the video file
            out.write(frame)

            # Display the resulting frame
            cv2.imshow('frame', frame)

            # Press 'q' to stop recording and exit
            if cv2.waitKey(1) & 0xFF == ord('q'):
                break
        else:
            break

    await stop_rec_ses()
    await dev_disconnect()

    # Release everything and close the window
    cap.release()
    out.release()
    cv2.destroyAllWindows()


if __name__ == "__main__":
    asyncio.run(test_method())
