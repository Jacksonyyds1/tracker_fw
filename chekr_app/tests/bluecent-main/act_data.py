import time
import struct
import csv
import asyncio
import ble_comm
import chekrapplink as ckr
import os 

session_uid = int(time.time() * 1000) 
directory =str(session_uid) + "_01"
parent_dir = "C:\\Users\\SM148\\OneDrive - Capgemini\\Documents\\06_Nestle_Dog_Collar\\communication_test\\bluecent\\output"
path = os.path.join(parent_dir, directory)  
dev_name = 1100
output_act_file = path +"\\"+"ACTIVITY_"+ str(dev_name) +"_" + str(session_uid)+ ".csv"
output_rec_file = path +"\\"+"REC_"+ str(dev_name) +"_" + str(session_uid)+ ".csv"

def imu_rec_rec_unpack(imu_rec):
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
    fieldnames = ["timestamp", "record no", "ax", "ay", "az", "gx", "gy", "gz"]
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
        }
        data.append(row_data)
        if i == 54:
            rnum += 1
    with open(output_rec_file, mode="a+", newline="") as csv_file:
        writer = csv.DictWriter(csv_file, fieldnames=fieldnames)
        if(csv_file.tell() == 0):
            writer.writeheader()
        for row in data:
            writer.writerow(row)


def imu_rec_act_unpack(imu_rec):
    #Validate Imu record length
    if len(imu_rec) != 255:
        print("invalid length: 255 data are not there ")
        return
    temp_data = [0]*54
    data = 0
    mnum = struct.unpack('<I',imu_rec[1:5])[0]
    # print(f"Model ID {mnum}")
    recnum = struct.unpack('<I',imu_rec[5:9])[0]
    # print(f"Record num  {recnum}")

    fieldnames = ["MODEL ID","RECORD NO","TIMESTAMP", "CANTER", "DRINK", "EAT", "GALLOP", "OTHER", "TROT", "WALK","padding"]
    for i in range(9,210,40):
        timestamp = struct.unpack('<Q',imu_rec[9:17])[0]
        # print(f"TIMESTAMP {timestamp}")
        temp_data[data]="{:d}".format(timestamp)
        data += 1
        for j in range(i+8,i+39,4): 
            start_bit = j
            end_bit = j+4
            z = struct.unpack('<f',imu_rec[start_bit:end_bit])[0]
            temp_data[data] = "{:0.7f}".format(z)
            data +=1
            
    data=[]
    for i in range(0,len(temp_data),9):
        row_data = {
            "MODEL ID": mnum if i == 0 else None,
            "RECORD NO": recnum if i == 0 else None,
            "TIMESTAMP":temp_data[i],
            "CANTER": temp_data[i+1],
            "DRINK": temp_data[i+2],
            "EAT": temp_data[i+3],
            "GALLOP": temp_data[i+4],
            "OTHER": temp_data[i+5],
            "TROT": temp_data[i+6],
            "WALK": temp_data[i+7],
            "padding": temp_data[i+8]
        }
        data.append(row_data)
    with open(output_act_file, mode="a+", newline="") as csv_file:
        writer = csv.DictWriter(csv_file, fieldnames=fieldnames)
        if(csv_file.tell() == 0):
            writer.writeheader()
        for row in data:
            writer.writerow(row)
    


async def test_data():
    comm = ble_comm.BleComm('indata.json')
    # await comm.scan_ble_devices()
    await comm.connect()
    os.mkdir(path)
    time.sleep(10)

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

    time.sleep(600)

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

    #Get raw session detial
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
    no = int.from_bytes(tdata.data[8:10], "big") #int(tdata.data[8:10])
    print("number", no)
    

    for i in range(no):
        #read raw rec data
        data = bytearray(12)
        data[0:7]= struct.pack('>Q', session_uid)#uid
        bytes_val = i.to_bytes(2, 'big')
        x=bytes_val[0]
        y=bytes_val[1]
        data[11]= x #msb
        data[12]=y #lsb
        tx_fr = ckr.read_rec_data(data)
        await comm.write(tx_fr)
        time.sleep(0.1)   
        rx_fr = await comm.read()
        time.sleep(0.1)
        print(i,"th raw record")
        # # tdata = ckr.CommFrame.from_bytes(rx_fr)  #parsing in byte array
        # hex_str = ' '.join(format(x, '02x') for x in rx_fr) #byte array to hex
        # print(hex_str)
        imu_rec_rec_unpack(rx_fr)

    #Get activity session detial
    data = bytearray()
    data += struct.pack('>Q', session_uid)  #uid
    data.append(0x00)#reser
    tx_fr = ckr.ad_read_rec_details(data)
    hex_str = ' '.join(format(x, '02x') for x in tx_fr) #byte array to hex
    print("activity session detial tx fr",hex_str)
    await comm.write(tx_fr)
    time.sleep(0.1)
    rx_fr = await comm.read()
    tdata = ckr.CommFrame.from_bytes(rx_fr)  #parsing in byte array
    hex_str = ' '.join(format(x, '02x') for x in rx_fr) #byte array to hex
    print("activity session detial ",hex_str)
    no = int.from_bytes(tdata.data[8:10], "big") #int(tdata.data[8:10])
    print("number", no)

    for i in range(no):
        #read activity rec data
        data = bytearray(12)
        data[0:7]= struct.pack('>Q', session_uid)#uid
        bytes_val = i.to_bytes(2, 'big')
        x=bytes_val[0]
        y=bytes_val[1]
        data[11]= x #msb
        data[12]=y #lsb
        tx_fr = ckr.ad_read_rec_data(data)
        await comm.write(tx_fr)
        time.sleep(0.1)   
        rx_fr = await comm.read()
        time.sleep(0.1)
        hex_str = ' '.join(format(x, '02x') for x in rx_fr) #byte array to hex
        print(i,"th activity record")
        # print(hex_str)
        imu_rec_act_unpack(rx_fr)

    await comm.disconnect()

if __name__ == "__main__":
    asyncio.run(test_data())
