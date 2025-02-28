#HareKrsna

'''ChekrAppLink communication protocol frame
dependencies: mcrc.py'''

import struct
import time
from mcrc16 import crc16


START_BYTE = 0x01
FRAME_TYPE = 0xF1

RX_START_BYTE = 0x02
RX_FRAME_TYPE = 0xF2
SUCCESS_ACK = 0x11

MIN_RX_FRAME_LEN = 7

class CommFrame:

    # Functions to make the frame.
    def __init__(self, command, data):
        self.command = command
        self.data = data
    
    def to_bytes(self, crc):
        data_len = len(self.data)
        frame_len = 4 + data_len + 2

        # Build the frame as a bytes object
        frame = bytearray()
        frame.append(START_BYTE)
        frame.append(frame_len)
        frame.append(FRAME_TYPE)
        frame.append(self.command)
        frame += self.data
        frame += struct.pack('>H', crc)

        return bytes(frame)
    
    def calculate_crc(self):
        # Calculate the CRC-16 checksum of the frame
        return crc16(bytes([START_BYTE, 4 + len(self.data) + 2, FRAME_TYPE, self.command]) + self.data)

    # Functions to parse the frame.
    @staticmethod
    def from_bytes(frame_bytes):

        # Verify minimum frame length.
        if (len(frame_bytes) < MIN_RX_FRAME_LEN):
            print("Invalid minimum frame length.")
            return

        # Verify frame length
        frame_len = frame_bytes[1]
        if frame_len !=  len(frame_bytes):
            print("Invalid frame: length")
            return
        
        # Verify start byte
        if frame_bytes[0] != RX_START_BYTE:
            print("Invalid frame: start byte")
            return

        # Calculate CRC
        crc = crc16(frame_bytes[:-2])
        crc_expected, = struct.unpack('>H', frame_bytes[-2:])

        # Verify CRC
        if crc != crc_expected:
            print(f"Invalid frame: CRC expected {crc_expected:04X}, actual {crc:04X}")
            return

        # Verify frame type
        frame_type = frame_bytes[2]
        if frame_type != RX_FRAME_TYPE:
            print("Invalid frame: frame type")
            return
                   

        # Verify acknowledgement.
        ack = frame_bytes[4]
        if ack != SUCCESS_ACK:
            print(f"Ack fail : {frame_bytes.hex(' ')}")
            return  

        # Extract command and data
        command = frame_bytes[3]
        data = frame_bytes[5:-2]

        # Create CommFrame object
        return CommFrame(command, data)

# error
def base(data):
    cmd = 0x00
    tx_fr = CommFrame(cmd, data)  
    crc = tx_fr.calculate_crc()
    b_str = tx_fr.to_bytes(crc)
    return b_str

# Get MAC Address - 0x01
def get_mac_addr_frame(data):
    cmd = 0x01
    tx_fr = CommFrame(cmd, data)  
    crc = tx_fr.calculate_crc()
    b_str = tx_fr.to_bytes(crc)
    return b_str

# Get Device System Information - 0x02
def get_dev_sys_info_frame(data):
    cmd = 0x02
    tx_fr = CommFrame(cmd, data)  
    crc = tx_fr.calculate_crc()
    b_str = tx_fr.to_bytes(crc)
    return b_str

# Get user Information - 0x03
def get_user_info_frame(data):
    cmd = 0x03
    tx_fr = CommFrame(cmd, data)  
    crc = tx_fr.calculate_crc()
    b_str = tx_fr.to_bytes(crc)
    return b_str

# Set the Dog Collar Position - 0x04
def set_ndc_position(data):
    cmd = 0x04
    tx_fr = CommFrame(cmd, data)  
    crc = tx_fr.calculate_crc()
    b_str = tx_fr.to_bytes(crc)
    return b_str


# Get epoch real time clock in device - 0x05
def get_epoch_time_frame(data):
    cmd = 0x05
    tx_fr = CommFrame(cmd, data)  
    crc = tx_fr.calculate_crc()
    b_str = tx_fr.to_bytes(crc)
    return b_str

# Set epoch real time clock in device - 0x06
def set_epoch_time_frame(data):
    cmd = 0x06
    tx_fr = CommFrame(cmd, data)  
    crc = tx_fr.calculate_crc()
    b_str = tx_fr.to_bytes(crc)
    return b_str

# Raw data harvesting - 0x07
def raw_data_harv_frame(data):
    cmd = 0x07
    tx_fr = CommFrame(cmd, data)
    crc = tx_fr.calculate_crc()
    b_str = tx_fr.to_bytes(crc)
    return b_str

# Activity data harvesting - 0x08
def activity_data_harv_frame(data):
    cmd = 0x08
    tx_fr = CommFrame(cmd, data)
    crc = tx_fr.calculate_crc()
    b_str = tx_fr.to_bytes(crc)
    return b_str

# Start/Stop periodic dashboard status information - 0x09
# 0 to Stop, 1 to Start
def start_stop_per_dash_frame(data):
    cmd = 0x09
    tx_fr = CommFrame(cmd, data)
    crc = tx_fr.calculate_crc()
    b_str = tx_fr.to_bytes(crc)
    return b_str

# BLE Status Show Command - 0x0A
def ble_status_show_frame(data):
    cmd = 0x0A
    tx_fr = CommFrame(cmd, data)
    crc = tx_fr.calculate_crc()
    b_str = tx_fr.to_bytes(crc)
    return b_str

# BLE Connected Show Command - 0x0B
def ble_connect_show_frame(data):
    cmd = 0x0B
    tx_fr = CommFrame(cmd, data)
    crc = tx_fr.calculate_crc()
    b_str = tx_fr.to_bytes(crc)
    return b_str

# Factory reset command - 0x0C
def fact_rst_frame(data):
    cmd = 0x0C
    tx_fr = CommFrame(cmd, data)
    crc = tx_fr.calculate_crc()
    b_str = tx_fr.to_bytes(crc)
    return b_str

# Reboot command - 0x0D
def rst_frame(data):
    cmd = 0x0D
    tx_fr = CommFrame(cmd, data)
    crc = tx_fr.calculate_crc()
    b_str = tx_fr.to_bytes(crc)
    return b_str



# Start/Stop recording session - 0x10
# 0 to Stop, 1 to Start
def start_stop_rec_sess(data):
    cmd = 0x10
    tx_fr = CommFrame(cmd, data)
    crc = tx_fr.calculate_crc()
    b_str = tx_fr.to_bytes(crc)
    return b_str   

# Read recording Session Details - 0x11
def read_rec_details(data):
    cmd = 0x11
    tx_fr = CommFrame(cmd, data)
    crc = tx_fr.calculate_crc()
    b_str = tx_fr.to_bytes(crc)
    return b_str  

# Read recording Session data - 0x12
def read_rec_data(data):
    cmd = 0x12
    tx_fr = CommFrame(cmd, data)
    crc = tx_fr.calculate_crc()
    b_str = tx_fr.to_bytes(crc)
    return b_str  

def ad_read_rec_details(data):
    cmd = 19
    tx_fr = CommFrame(cmd, data)
    crc = tx_fr.calculate_crc()
    b_str = tx_fr.to_bytes(crc)
    return b_str  

# Read recording Session data - 0x12
def ad_read_rec_data(data):
    cmd = 20
    tx_fr = CommFrame(cmd, data)
    crc = tx_fr.calculate_crc()
    b_str = tx_fr.to_bytes(crc)
    return b_str  

# Read DASHBOARD INFO - 0x13
def read_dashboard_data(data):
    cmd = 0x16
    tx_fr = CommFrame(cmd, data)
    crc = tx_fr.calculate_crc()
    b_str = tx_fr.to_bytes(crc)
    return b_str  

# Testing the module when the file is run in standalone mode.
if __name__ == "__main__":  
    data = bytes.fromhex('00')  
    b_str = get_mac_addr_frame(data)
    hex_str = ' '.join(format(x, '02x') for x in b_str)
    print(hex_str)

    data = bytes.fromhex('00')  
    b_str = get_epoch_time_frame(data)
    hex_str = ' '.join(format(x, '02x') for x in b_str)
    print(hex_str)  

    epoch_time_ms = int(time.time() * 1000)
    data = struct.pack('>Q', epoch_time_ms)
    b_str = set_epoch_time_frame(data)
    hex_str = ' '.join(format(x, '02x') for x in b_str)
    print(hex_str)      

    data = bytearray()
    start_stop = 0x01
    data.append(start_stop)
    rec_ses_uid = 1234
    uid_data = struct.pack('>Q', rec_ses_uid)
    data += uid_data  
    b_str = start_stop_rec_sess(data)
    hex_str = ' '.join(format(x, '02x') for x in b_str)
    print(hex_str)      

