import time
import struct
import asyncio
import ble_comm
import chekrapplink as ckr

async def test_data3():
    comm = ble_comm.BleComm('indata.json')
    # await comm.scan_ble_devices()
    await comm.connect()

    time.sleep(1)
    # arr=[1,1,1,1,1,1,1,1,1,1,1,1,1,1]
    # data=bytearray(arr)
    # tx_fr = ckr.base(data)
    # await comm.write(tx_fr)
    # time.sleep(1)
    # rx_fr = await comm.read()

    num = 0
    while 1:
        #test for getting mac address
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


        #test set the dog collar position
        data = bytes.fromhex('05')
        tx_fr = ckr.set_ndc_position(data)
        await comm.write(tx_fr)
        time.sleep(1)
        rx_fr = await comm.read()
        tdata = ckr.CommFrame.from_bytes(rx_fr)  #parsing in byte array
        hex_str = ' '.join(format(x, '02x') for x in rx_fr) #byte array to hex
        print("dog collar position",hex_str)

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
        time.sleep(0.1)
        rx_fr = await comm.read()
        tdata = ckr.CommFrame.from_bytes(rx_fr)
        hex_str = ' '.join(format(x, '02x') for x in tdata.data)
        curr_epoch_time_ms = int(time.time() * 1000)
        t_diff = curr_epoch_time_ms - epoch_time_ms
        print(f"Time difference: {t_diff}")
        print("get epoch time",hex_str)

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
        print("read record session details",hex_str)
        
        num = num + 1
        if num > 16:
            break

    await comm.disconnect()


asyncio.run(test_data3())
