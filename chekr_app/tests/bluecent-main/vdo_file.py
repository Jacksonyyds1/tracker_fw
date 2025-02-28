# HareKrsna
# EC:25:65:DB:A0:20 //1098
# C8:DA:20:49:69:C5 //1002
# EA:C3:28:D1:06:9E //1001

#import cv2
import csv
import time
import struct
import asyncio
import ble_comm
import datetime
import chekrapplink as ckr

comm = ble_comm.BleComm('indata.json')
session_uid = int(time.time() * 1000)
dev_name=1122
output_file = "output\\" + str(dev_name) +"_" + str(session_uid)+ ".csv"

async def dev_connect():    
    await comm.connect()
    print("Connected to device")

async def dev_disconnect():    
    await comm.disconnect()
    print("Disconnected")

def imu_rec_unpack(imu_rec):
    #Validate Imu record length
    if len(imu_rec) != 255:
        print("invalid length: 255 data are not there ")
        return
    temp_data = [0]*60
    #unpack record num
    rnum = struct.unpack('<I',imu_rec[1:5])[0]
    # print(f"Record number {rnum}")
    #unpack time stamp
    ts = struct.unpack('<Q',imu_rec[5:13])[0]
    #print(f"Time Stamp {ts}")
    fieldnames = ["timestamp", "record no", "ax", "ay", "az", "gx", "gy", "gz","battery percentage"]
    for i in range(60): 
        start_bit = 13 + (i*4)
        end_bit = 17 + (i*4)
        z = struct.unpack('<f',imu_rec[start_bit:end_bit])[0]
        temp_data[i] = z
    data=[]
    for i in range(0,len(temp_data),6):
        row_data = {
            # "suid":suid if i == 0 else None,
            "timestamp": ts if i == 0 else None,
            "record no": rnum if i == 0 else None,
            "ax": temp_data[i],
            "ay": temp_data[i+1],
            "az": temp_data[i+2],
            "gx": temp_data[i+3],
            "gy": temp_data[i+4],
            "gz": temp_data[i+5],
            # "battery percentage": bt_volt if i==0 else None
        }
        data.append(row_data)
        if i == 54:
            rnum += 1
    with open(output_file, mode="a+", newline="") as csv_file:
        writer = csv.DictWriter(csv_file, fieldnames=fieldnames)
        if(csv_file.tell() == 0):
            writer.writeheader()
        for row in data:
            writer.writerow(row)
    

async def start_rec_ses():
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

    """Recording Start"""
    #test for Start/Stop recording sessiom
    print("Session ID : ",session_uid)
    data = bytearray()
    data.append(0x01)
    data += struct.pack('>Q', session_uid)
    tx_fr = ckr.start_stop_rec_sess(data)
    await comm.write(tx_fr)
    hex_str = ' '.join(format(x, '02x') for x in tx_fr) #byte array to hex
    print("Start tx_fr frame",hex_str)
    time.sleep(0.1)
    rx_fr = await comm.read()
    tdata = ckr.CommFrame.from_bytes(rx_fr)  #parsing in byte array
    hex_str = ' '.join(format(x, '02x') for x in rx_fr) #byte array to hex
    print("start reply frame",hex_str)

async def stop_rec_ses():
    #test for Start/Stop recording sessiom
    data = bytearray()
    data.append(0x00)
    data += struct.pack('>Q', session_uid)
    tx_fr = ckr.start_stop_rec_sess(data)
    await comm.write(tx_fr)
    hex_str = ' '.join(format(x, '02x') for x in tx_fr) #byte array to hex
    print("stop tx frame",hex_str)
    time.sleep(0.1)
    rx_fr = await comm.read()
    tdata = ckr.CommFrame.from_bytes(rx_fr)  #parsing in byte array
    hex_str = ' '.join(format(x, '02x') for x in rx_fr) #byte array to hex
    # imu_rec_unpack(rx_fr)
    print("stop rx frame",hex_str)

    time.sleep(1)

    #Get session detial
    data = bytearray()
    data += struct.pack('>Q', session_uid)  #uid
    data.append(0x00)#reser
    tx_fr = ckr.read_rec_details(data)
    await comm.write(tx_fr)
    time.sleep(0.1)
    rx_fr = await comm.read()
    tdata = ckr.CommFrame.from_bytes(rx_fr)  #parsing in byte array
    hex_str = ' '.join(format(x, '02x') for x in rx_fr) #byte array to hex
    print("session detial 1",hex_str)
    #print(f"tdata.data: {tdata.data}")
    no = int.from_bytes(tdata.data[8:10], "big") #int(tdata.data[8:10])
    print("number", no)

    # print(datetime.datetime.now())
    # """Reading SUID1 Session data"""
    # for i in range(no):
    #     #read rec data
    #     data = bytearray(12)
    #     data[0:7]= struct.pack('>Q', session_uid)#uid
    #     bytes_val = i.to_bytes(2, 'big')
    #     x=bytes_val[0]
    #     y=bytes_val[1]
    #     data[11]= x #msb
    #     data[12]=y #lsb
    #     tx_fr = ckr.read_rec_data(data)
    #     await comm.write(tx_fr)
    #     time.sleep(0.1)   
    #     rx_fr = await comm.read()
    #     time.sleep(0.1)
    #     # # tdata = ckr.CommFrame.from_bytes(rx_fr)  #parsing in byte array
    #     # hex_str = ' '.join(format(x, '02x') for x in rx_fr) #byte array to hex
    #     # print(hex_str)
    #     imu_rec_unpack(rx_fr)
    #     # # time.sleep(1.5)

    # print(datetime.datetime.now())



# # Set video dimensions and frames per second
# width = 640
# height = 480
# fps = 30

# Define video codec and create VideoWriter object
# fourcc = cv2.VideoWriter_fourcc(*'XVID')
# out = cv2.VideoWriter('output.avi', fourcc, fps, (width, height))

async def test_method():
    #Start recording 
    await dev_connect()
    await start_rec_ses()

    time.sleep(60)

    # Open the default camera and start recording
    # cap = cv2.VideoCapture(1)

    # while True:
    #     # Capture a frame from the camera
    #     ret, frame = cap.read()

    #     if ret:
    #         # Write the frame to the video file
    #         out.write(frame)

    #         # Display the resulting frame
    #         cv2.imshow('frame', frame)

    #         # Press 'q' to stop recording and exit
    #         if cv2.waitKey(1) & 0xFF == ord('q'):
    #             break
    #     else:
    #         break

    await stop_rec_ses()
    
    await dev_disconnect()

    # Release everything and close the window
    # cap.release()
    # out.release()
    # cv2.destroyAllWindows()


if __name__ == "__main__":
    asyncio.run(test_method())


