import time
import struct
import asyncio
import ble_comm
import chekrapplink as ckr

def batt_volt(btv):
    if len(btv) != 20:
        print("invalid dashboard frame length")
        return
    #unpack record num
    x = struct.unpack('>H',btv[8:10])[0]
    print(f"Battery Voltage {x/100}")
    Y = struct.unpack('<B',btv[10:11])[0]
    print(f"Battery Voltage {Y}")

async def test_data():
    comm = ble_comm.BleComm('indata.json')
    # await comm.scan_ble_devices()
    await comm.connect()

    time.sleep(1)

    #  test for start/stop periodic dashboard information
    data = bytes.fromhex('01')
    tx_fr = ckr.start_stop_per_dash_frame(data)
    await comm.write(tx_fr)
    time.sleep(1)
    rx_fr = await comm.read()
    hex_str = ' '.join(format(x, '02x') for x in rx_fr)
    batt_volt(rx_fr)
    print("start/stop periodic dashboard information",hex_str)


    await comm.disconnect()


asyncio.run(test_data())