import csv
import time
import struct
import asyncio
import ble_comm
import chekrapplink as ckr
import os
import datetime
dev_name=1122
ses_uid = 1689168760073
output_file = "output\\" + str(dev_name) +"_" + str(ses_uid)+ ".csv"

def imu_rec_unpack(imu_rec,bt_volt):
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
            "battery percentage": bt_volt if i==0 else None
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

    data = bytes.fromhex('00')
    tx_fr = ckr.start_stop_per_dash_frame(data)
    await comm.write(tx_fr)
    time.sleep(1)
    rx_fr = await comm.read()
    bt_voltage = struct.unpack('<B',rx_fr[10:11])[0]
    print(f"Battery Voltage {bt_voltage}")


    #Get session detial
    data = bytearray()
    data += struct.pack('>Q', ses_uid)  #uid
    data.append(0x00)#reser
    tx_fr = ckr.ad_read_rec_details(data)
    await comm.write(tx_fr)
    time.sleep(0.1)
    rx_fr = await comm.read()
    tdata = ckr.CommFrame.from_bytes(rx_fr)  #parsing in byte array
    hex_str = ' '.join(format(x, '02x') for x in rx_fr) #byte array to hex
    print("session detial 1",hex_str)
    no = int.from_bytes(tdata.data[8:10], "big") #int(tdata.data[8:10])
    print("number", no)

    print(datetime.datetime.now())
    """Reading SUID1 Session data"""
    for i in range(no):
        #read rec data
        data = bytearray(12)
        data[0:7]= struct.pack('>Q', ses_uid)#uid
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
        # # tdata = ckr.CommFrame.from_bytes(rx_fr)  #parsing in byte array
        # hex_str = ' '.join(format(x, '02x') for x in rx_fr) #byte array to hex
        # print(hex_str)
        imu_rec_unpack(rx_fr,bt_voltage)
        # # time.sleep(1.5)

    print(datetime.datetime.now())
    await comm.disconnect()

asyncio.run(test_data1())
