#HareKrsna

'''
MODBUS CRC16 calculation.
'''

def crc16(data):
    """
    Calculates the Modbus CRC16 checksum for a given byte array.

    :param data: the byte array for which to calculate the checksum
    :type data: bytes
    :return: the calculated checksum
    :rtype: int
    """
    crc = 0xFFFF
    for b in data:
        crc ^= b
        for _ in range(8):
            if crc & 0x0001:
                crc >>= 1
                crc ^= 0xA001
            else:
                crc >>= 1
    return crc

if __name__ == "__main__":
    in_fr = bytes.fromhex("0107F10101")
    crc_16 = crc16(in_fr)
    print(hex(crc_16))
    # b = crc_16.to_bytes(2, byteorder='big')
    # print(hex(b[0]))
     

    
