import struct

WRITE_CHARACTERISTIC_UUID = "6765a69d-cd79-4df6-aad5-043df9425556"

HEADER_FORMAT = "<BBBB"
CRC_FORMAT = "H"

SB_MOBILE_APP_TO_DEVICE = 1
FT_REQUEST = 0xF1

def crc16_modbus(data: bytes) -> int:
    """Calculate the CRC16-Modbus for a byte array."""
    crc = 0xFFFF
    for byte in data:
        crc ^= byte
        for _ in range(8):
            if crc & 0x0001:
                crc >>= 1
                crc ^= 0xA001
            else:
                crc >>= 1
    return crc

def pack_command(start_byte, frame_len, frame_type, cmd, data):
    packed_without_crc = struct.pack(HEADER_FORMAT, start_byte, frame_len, frame_type, cmd)
    if data:
        packed_without_crc = packed_without_crc + data
    crc = crc16_modbus(packed_without_crc)
    return packed_without_crc + struct.pack('>H', crc)
