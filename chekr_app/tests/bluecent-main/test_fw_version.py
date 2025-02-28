import time
import struct
import asyncio
import ble_comm
import chekrapplink as ckr


async def test_data1():
    comm = ble_comm.BleComm('indata.json')
    await comm.connect()
    time.sleep(1)
    #test for getting device system information
    data = bytes.fromhex('00')
    tx_fr = ckr.start_stop_per_dash_frame(data)
    await comm.write(tx_fr)
    hex_str = ' '.join(format(x, '02x') for x in tx_fr)
    print("tx_fr",hex_str)
    time.sleep(1)
    rx_fr = await comm.read()
    tdata = ckr.CommFrame.from_bytes(rx_fr)  #parsing in byte array
    hex_str = ' '.join(format(x, '02x') for x in rx_fr) #byte array to hex
    print("Getting device system information",hex_str)
    
     
    await comm.disconnect()

asyncio.run(test_data1())
