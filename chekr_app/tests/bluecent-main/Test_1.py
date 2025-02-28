import time
import struct
import asyncio
import ble_comm
import chekrapplink as ckr

async def test_data():
    comm = ble_comm.BleComm('indata.json')
    # await comm.scan_ble_devices()
    await comm.connect()

    time.sleep(10)
    # arr=[1,1,1,1,1,1,1,1,1,1,1,1,1,1]
    # data=bytearray(arr)
    # tx_fr = ckr.base(data)
    # await comm.write(tx_fr)
    # time.sleep(1)
    # rx_fr = await comm.read()

    """#test for getting mac address
    data = bytes.fromhex('00')
    tx_fr = ckr.get_mac_addr_frame(data)
    await comm.write(tx_fr)
    time.sleep(1)
    rx_fr = await comm.read()
    tdata = ckr.CommFrame.from_bytes(rx_fr)  #parsing in byte array
    hex_str = ' '.join(format(x, '02x') for x in rx_fr) #byte array to hex
    print("MAC ADDRESS",hex_str)

    #test for getting device system information
    data = bytes.fromhex('00')
    tx_fr = ckr.get_dev_sys_info_frame(data)
    await comm.write(tx_fr)
    time.sleep(1)
    rx_fr = await comm.read()
    tdata = ckr.CommFrame.from_bytes(rx_fr)  #parsing in byte array
    hex_str = ' '.join(format(x, '02x') for x in rx_fr) #byte array to hex
    print("Getting device system information",hex_str)

    #test for getting mac address
    data = bytes.fromhex('00')
    tx_fr = ckr.get_user_info_frame(data)
    await comm.write(tx_fr)
    time.sleep(1)
    rx_fr = await comm.read()
    tdata = ckr.CommFrame.from_bytes(rx_fr)  #parsing in byte array
    hex_str = ' '.join(format(x, '02x') for x in rx_fr) #byte array to hex
    print("user ADDRESS",hex_str)


    #test set the dog collar position
    data = bytes.fromhex('05')
    tx_fr = ckr.set_ndc_position(data)
    await comm.write(tx_fr)
    time.sleep(1)
    rx_fr = await comm.read()
    tdata = ckr.CommFrame.from_bytes(rx_fr)  #parsing in byte array
    hex_str = ' '.join(format(x, '02x') for x in rx_fr) #byte array to hex
    print("dog collar position",hex_str)

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
    tx_fr = ckr.get_epoch_time_frame(data)
    await comm.write(tx_fr)
    time.sleep(1)
    rx_fr = await comm.read()
    curr_epoch_time_ms = int(time.time() * 1000)
    tdata = ckr.CommFrame.from_bytes(rx_fr)    
    get_epoch_time_ms = struct.unpack('>Q', tdata.data)[0]
    t_diff = curr_epoch_time_ms - get_epoch_time_ms
    hex_str = ' '.join(format(x, '02x') for x in rx_fr)
    print("get epoch time",hex_str)
    print(f"Time difference: {t_diff}")

        # num = num + 1
        # if num > 15:
        #     break

    #  test for raw data harvesting
    data = bytes.fromhex('00')
    tx_fr = ckr.raw_data_harv_frame(data)
    await comm.write(tx_fr)
    time.sleep(1)
    rx_fr = await comm.read()
    tdata = ckr.CommFrame.from_bytes(rx_fr)
    hex_str = ' '.join(format(x, '02x') for x in rx_fr)
    print("raw data harvesting",hex_str)

    #  test for activity data harvesting
    data = bytes.fromhex('00')
    tx_fr = ckr.activity_data_harv_frame(data)
    await comm.write(tx_fr)
    time.sleep(1)
    rx_fr = await comm.read()
    tdata = ckr.CommFrame.from_bytes(rx_fr)
    hex_str = ' '.join(format(x, '02x') for x in rx_fr)
    print("activity data harvesting",hex_str)

    #  test for start/stop periodic dashboard information
    data = bytes.fromhex('01')
    tx_fr = ckr.start_stop_per_dash_frame(data)
    await comm.write(tx_fr)
    time.sleep(1)
    rx_fr = await comm.read()
    tdata = ckr.CommFrame.from_bytes(rx_fr)
    hex_str = ' '.join(format(x, '02x') for x in rx_fr)
    print("start/stop periodic dashboard information",hex_str)

    #  test for BLE status show command
    data = bytes.fromhex('00')
    tx_fr = ckr.ble_status_show_frame(data)
    await comm.write(tx_fr)
    time.sleep(1)
    rx_fr = await comm.read()
    tdata = ckr.CommFrame.from_bytes(rx_fr)
    hex_str = ' '.join(format(x, '02x') for x in rx_fr)
    print("BLE status show command",hex_str)

    #  test for BLE connected show command
    data = bytes.fromhex('00')
    tx_fr = ckr.ble_connect_show_frame(data)
    await comm.write(tx_fr)
    time.sleep(1)
    rx_fr = await comm.read()
    tdata = ckr.CommFrame.from_bytes(rx_fr)
    hex_str = ' '.join(format(x, '02x') for x in rx_fr)
    print("BLE connected show command",hex_str)

    #  test for factor reset command
    data = bytes.fromhex('00')
    tx_fr = ckr.fact_rst_frame(data)
    await comm.write(tx_fr)
    time.sleep(1)
    rx_fr = await comm.read()
    tdata = ckr.CommFrame.from_bytes(rx_fr)
    hex_str = ' '.join(format(x, '02x') for x in rx_fr)
    print("factor reset command",hex_str)


    #  test for read record session details
    data = bytes.fromhex('00')
    tx_fr = ckr.read_rec_details(data)
    await comm.write(tx_fr)
    time.sleep(1)
    rx_fr = await comm.read()
    tdata = ckr.CommFrame.from_bytes(rx_fr)
    hex_str = ' '.join(format(x, '02x') for x in rx_fr)
    print("read record session details",hex_str) :"""

     #  test for read record session details
    data = bytes.fromhex('00')
    tx_fr = ckr.read_dashboard_data(data)
    await comm.write(tx_fr)
    hex_str = ' '.join(format(x, '02x') for x in tx_fr)
    print("tx fr ",hex_str)
    time.sleep(1)
    rx_fr = await comm.read()
    tdata = ckr.CommFrame.from_bytes(rx_fr)
    hex_str = ' '.join(format(x, '02x') for x in rx_fr)
    print("read dashboard details",hex_str)

    #unpack num
    x = struct.unpack("<9s",rx_fr[5:14])[0]
    print(f"Device Name {x}")
    i = struct.unpack('<B',rx_fr[14:15])[0]
    print(f"Application major version {i}")
    j = struct.unpack('<B',rx_fr[15:16])[0]
    print(f"Application minor version  {j}")
    k = struct.unpack('<B',rx_fr[16:17])[0]
    print(f"Application major version {k}")
    x = struct.unpack('>B',rx_fr[17:18])[0]
    print(f"Battery Percentage {x/100}")
    Y = struct.unpack('>H',rx_fr[18:20])[0]
    print(f"Battery Voltage {Y}")
    Y = struct.unpack('>H',rx_fr[20:22])[0]
    print(f"input power {Y}")
    Y = struct.unpack('>H',rx_fr[22:24])[0]
    print(f"Temperature {Y}") 

    """data = bytes.fromhex('00')
    tx_fr = ckr.rst_frame(data)
    await comm.write(tx_fr)
    hex_str = ' '.join(format(x, '02x') for x in tx_fr)
    print("tx fr ",hex_str)
    time.sleep(1)
    rx_fr = await comm.read()
    tdata = ckr.CommFrame.from_bytes(rx_fr)
    hex_str = ' '.join(format(x, '02x') for x in rx_fr)
    print("RESET DONE",hex_str) """

    
    await comm.disconnect()


asyncio.run(test_data())
