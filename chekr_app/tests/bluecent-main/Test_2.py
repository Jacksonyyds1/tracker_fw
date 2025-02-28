import time
import struct
import asyncio
import ble_comm
import csv
import chekrapplink as ckr

def imu_rec_unpack(imu_rec):
    #Validate Imu record length
    if len(imu_rec) != 255:
        print("invalid")
        return
    #unpack record num
    x = struct.unpack('<I',imu_rec[1:5])[0]
    print(f"Record number {x}")
    #unpack time stamp
    y = struct.unpack('<Q',imu_rec[5:13])[0]
    print(f"Time Stamp {y}")
    #unpack samples
    fields = ['ax', 'ay','az','gx','gy','gz']
    data = [0]*10
    tum_data = [0]*6
    filename = "imu_data.csv"
    tnum = 0
    num = 0
    for i in range(60): #ax ay az gx gy gz *10 sample
        start_bit = 13 + (i*4)
        end_bit = 17 + (i*4)
        z = struct.unpack('<f',imu_rec[start_bit:end_bit])[0]
        tum_data[tnum] = z
        tnum = tnum + 1
        if(tnum > 5):
            data[num] = tum_data
            num = num + 1
            tum_data =[0]*6
            tnum = 0
            if num == 10:
                break
        print(f"Record Samples {i+1} : {z}") 
    # print(data) 
    with open(filename, 'w') as csvfile: 
        # creating a csv writer object 
        csvwriter = csv.writer(csvfile)             
        # writing the fields 
        csvwriter.writerow(fields)             
        # writing the data rows 
        csvwriter.writerows(data)  

async def test_data1():
    comm = ble_comm.BleComm('indata.json')
    await comm.connect()
    time.sleep(1)
    # num = 0
    # while 1:
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
        # num+=1
        # if num > 15:
        #     break
    #test for Start/Stop periodic dashboard status information
    data = bytes.fromhex('01')
    tx_fr = ckr.start_stop_rec_sess(data)
    await comm.write(tx_fr)
    time.sleep(1)
    rx_fr = await comm.read()
    tdata = ckr.CommFrame.from_bytes(rx_fr)  #parsing in byte array
    hex_str = ' '.join(format(x, '02x') for x in rx_fr) #byte array to hex
    print("start",hex_str)
    
    time.sleep(5)

    #test for Start/Stop periodic dashboard status information
    data = bytes.fromhex('00')
    tx_fr = ckr.start_stop_rec_sess(data)
    await comm.write(tx_fr)
    time.sleep(1)
    rx_fr = await comm.read()
    tdata = ckr.CommFrame.from_bytes(rx_fr)  #parsing in byte array
    hex_str = ' '.join(format(x, '02x') for x in rx_fr) #byte array to hex
    print("stop",hex_str)

    data = bytes.fromhex('00')
    tx_fr = ckr.read_rec_details(data)
    await comm.write(tx_fr)
    time.sleep(1)
    rx_fr = await comm.read()
    tdata = ckr.CommFrame.from_bytes(rx_fr)  #parsing in byte array
    hex_str = ' '.join(format(x, '02x') for x in rx_fr) #byte array to hex
    print("session detial",hex_str)
    print("number", int(tdata.data[8]))#sum(x)))


    # for i in range (int(tdata.data[8])):
    data = bytes.fromhex('00')
    tx_fr = ckr.read_rec_data(data)
    await comm.write(tx_fr)
    time.sleep(1)
    rx_fr = await comm.read()
    tdata = ckr.CommFrame.from_bytes(rx_fr)  #parsing in byte array
    hex_str = ' '.join(format(x, '02x') for x in rx_fr) #byte array to hex
    print("session data",hex_str) 
    imu_rec_unpack(rx_fr)    
     
    """num =0
    while 1:
        #test for Start/Stop periodic dashboard status information
        data = bytes.fromhex('01')
        tx_fr = ckr.start_stop_rec_sess(data)
        await comm.write(tx_fr)
        time.sleep(0.1)
        rx_fr = await comm.read()
        tdata = ckr.CommFrame.from_bytes(rx_fr)  #parsing in byte array
        hex_str = ' '.join(format(x, '02x') for x in rx_fr) #byte array to hex
        print("start",hex_str)
        
        time.sleep(10)

        #test for Start/Stop periodic dashboard status information
        data = bytes.fromhex('00')
        tx_fr = ckr.start_stop_rec_sess(data)
        await comm.write(tx_fr)
        time.sleep(10)
        rx_fr = await comm.read()
        tdata = ckr.CommFrame.from_bytes(rx_fr)  #parsing in byte array
        hex_str = ' '.join(format(x, '02x') for x in rx_fr) #byte array to hex
        print("stop",hex_str)

        data = bytes.fromhex('00')
        tx_fr = ckr.read_rec_details(data)
        await comm.write(tx_fr)
        time.sleep(1)
        rx_fr = await comm.read()
        tdata = ckr.CommFrame.from_bytes(rx_fr)  #parsing in byte array
        hex_str = ' '.join(format(x, '02x') for x in tdata.data) #byte array to hex
        print("session detial",tdata.data)
        print("number", int(tdata.data[8]))
        num = num + 1
        if num >1:
            break""" 

    await comm.disconnect()

asyncio.run(test_data1())
