import asyncio
import time

WRITE_CHARACTERISTIC_UUID = "6765a69d-cd79-4df6-aad5-043df9425556"
READ_CHARACTERISTIC_UUID = "b6ab2ce3-a5aa-436a-817a-cc13a45aab76"
NOTIFY_CHARACTERISTIC_UUID = "207bdc30-c3cc-4a14-8b66-56ba8a826640"
WRITE_NO_RESP_CHARACTERISTIC_UUID = "6765a69d-cd79-4df6-aad5-043df9425557"

received_data = None

# send command, wait for response and return it
async def send_command(client, data):
    await client.write_gatt_char(WRITE_NO_RESP_CHARACTERISTIC_UUID, data)
    time.sleep(0.1)
    received_data = await client.read_gatt_char(READ_CHARACTERISTIC_UUID)

    return received_data
