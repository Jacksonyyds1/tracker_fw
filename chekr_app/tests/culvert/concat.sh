#!/bin/bash
echo "running"
for file in *.bin; do
    echo "proces $file"
    python3.9 cloud_decode.py -f $file -t imu >> concat.txt
#  ./process_file.sh "$file"
done