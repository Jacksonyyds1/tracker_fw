import csv
import time
import struct
import asyncio
import ble_comm
import chekrapplink as ckr
import os

def imu_rec_unpack(imu_rec):
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
    print(f"Time Stamp {ts}")
    fieldnames = ["ts", "rn", "ax", "ay", "az", "gx", "gy", "gz"]
    output_file = "output_2.csv"
    for i in range(60): 
        start_bit = 13 + (i*4)
        end_bit = 17 + (i*4)
        z = struct.unpack('<f',imu_rec[start_bit:end_bit])[0]
        temp_data[i] = z
    data=[]
    for i in range(0,len(temp_data),6):
        row_data = {
            # "suid":suid if i == 0 else None,
            "ts": ts if i == 0 else None,
            "rn": rnum if i == 0 else None,
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
    with open(output_file, mode="a+", newline="") as csv_file:
        writer = csv.DictWriter(csv_file, fieldnames=fieldnames)
        if(csv_file.tell() == 0):
            writer.writeheader()
        for row in data:
            writer.writerow(row)

async def test_data1():
    comm = ble_comm.BleComm('indata.json')
    await comm.connect()
    time.sleep(1)


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

    """Recording for 1st uuid"""
    #test for Start/Stop recording sessiom
    epoch_time_ms_1 = int(time.time() * 1000)
    data = bytearray()
    data.append(0x01)
    data += struct.pack('>Q', epoch_time_ms_1)
    tx_fr = ckr.start_stop_rec_sess(data)
    await comm.write(tx_fr)
    time.sleep(0.1)
    rx_fr = await comm.read()
    tdata = ckr.CommFrame.from_bytes(rx_fr)  #parsing in byte array
    hex_str = ' '.join(format(x, '02x') for x in rx_fr) #byte array to hex
    print("start",hex_str)
    
    time.sleep(5)

    #test for Start/Stop recording sessiom
    data = bytearray()
    data.append(0x00)
    data += struct.pack('>Q', epoch_time_ms_1)
    tx_fr = ckr.start_stop_rec_sess(data)
    await comm.write(tx_fr)
    time.sleep(0.1)
    rx_fr = await comm.read()
    tdata = ckr.CommFrame.from_bytes(rx_fr)  #parsing in byte array
    hex_str = ' '.join(format(x, '02x') for x in rx_fr) #byte array to hex
    print("stop",hex_str)

    #Get session detial
    data = bytearray()
    data += struct.pack('>Q', epoch_time_ms_1)#uid
    data.append(0x00)#reser
    tx_fr = ckr.read_rec_details(data)
    await comm.write(tx_fr)
    time.sleep(0.1)
    rx_fr = await comm.read()
    tdata = ckr.CommFrame.from_bytes(rx_fr)  #parsing in byte array
    hex_str = ' '.join(format(x, '02x') for x in rx_fr) #byte array to hex
    print("session detial 1",hex_str)
    no = int(tdata.data[8])
    print("number", int(tdata.data[8]))

    """Recording for 2nd uuid
    #test for Start/Stop recording sessiom
    epoch_time_ms_2 = int(time.time() * 1000)
    data = bytearray()
    data.append(0x01)
    data += struct.pack('>Q', epoch_time_ms_2)
    tx_fr = ckr.start_stop_rec_sess(data)
    await comm.write(tx_fr)
    time.sleep(0.1)
    rx_fr = await comm.read()
    tdata = ckr.CommFrame.from_bytes(rx_fr)  #parsing in byte array
    hex_str = ' '.join(format(x, '02x') for x in rx_fr) #byte array to hex
    print("start",hex_str)
    
    time.sleep(5)

    #test for Start/Stop recording sessiom
    data = bytearray()
    data.append(0x00)
    data += struct.pack('>Q', epoch_time_ms_2)
    tx_fr = ckr.start_stop_rec_sess(data)
    await comm.write(tx_fr)
    time.sleep(0.1)
    rx_fr = await comm.read()
    tdata = ckr.CommFrame.from_bytes(rx_fr)  #parsing in byte array
    hex_str = ' '.join(format(x, '02x') for x in rx_fr) #byte array to hex
    print("stop",hex_str)

    #Get session detial
    data = bytearray()
    data += struct.pack('>Q', epoch_time_ms_2)#uid
    data.append(0x00)#reser
    tx_fr = ckr.read_rec_details(data)
    await comm.write(tx_fr)
    time.sleep(0.1)
    rx_fr = await comm.read()
    tdata = ckr.CommFrame.from_bytes(rx_fr)  #parsing in byte array
    hex_str = ' '.join(format(x, '02x') for x in rx_fr) #byte array to hex
    print("session detial 2",hex_str)
    print("number", int(tdata.data[8]))"""

    """Reading SUID1 Session data"""
    for i in range(no):
        #read rec data
        data = bytearray(12)
        data[0:7]= struct.pack('>Q', epoch_time_ms_1)#uid
        data[12]= i
        tx_fr = ckr.read_rec_data(data)
        await comm.write(tx_fr)
        time.sleep(0.1)   
        rx_fr = await comm.read()
        time.sleep(0.1)
        # # tdata = ckr.CommFrame.from_bytes(rx_fr)  #parsing in byte array
        # hex_str = ' '.join(format(x, '02x') for x in rx_fr) #byte array to hex
        # print(hex_str)
        imu_rec_unpack(rx_fr)
        # # time.sleep(1.5)
    

    time.sleep(1)

    """Reading SUID2 Session data
    #read rec data
    data = bytearray(12)
    data[0:7]= struct.pack('>Q', epoch_time_ms_2)#uid
    # # data = bytes.fromhex('00')
    tx_fr = ckr.read_rec_data(data)
    hex_str1 = ' '.join(format(x, '02x') for x in tx_fr) #byte array to hex
    print("session data receive tx_fr",hex_str1) 
    await comm.write(tx_fr)
    time.sleep(0.1)
    rx_fr2 = await comm.read()
    tdata = ckr.CommFrame.from_bytes(rx_fr)  #parsing in byte array
    hex_str = ' '.join(format(x, '02x') for x in rx_fr2) #byte array to hex
    print("session data",hex_str)"""

    # imu_rec_unpack(rx_fr1,epoch_time_ms_1)
    # imu_rec_unpack(rx_fr2,epoch_time_ms_2)
     
    await comm.disconnect()

asyncio.run(test_data1())
