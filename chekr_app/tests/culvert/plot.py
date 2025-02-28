import re
import ast
import matplotlib.pyplot as plt
import argparse
from datetime import datetime
import numpy as np
from collections import Counter
from matplotlib.ticker import MaxNLocator

def main():
    # Read the data from the file
    parser = argparse.ArgumentParser(description="decode recorded data session obtained from the cloud")
    parser.add_argument("--filename", "-f", help="filename", required=True)
    args = parser.parse_args()
    filename = args.filename
    print(f"opening: {filename}")
    with open(filename, 'r') as f:
        lines = f.readlines()

    # Initialize lists to store accelerometer and gyroscope data
    ax_data = []
    ay_data = []
    az_data = []
    gx_data = []
    gy_data = []
    gz_data = []
    freq = []

    # Regular expression to match the record data in the log lines
    pattern = re.compile(r"{'record_num': \d+, 'timestamp': '.*?', 'raw_data': \[.*?\]}")

    # Process each line
    last_datetime = None
    for line in lines:
        match = pattern.search(line)
        if match:
            record_str = match.group(0)
            record_dict = ast.literal_eval(record_str)
            raw_data = record_dict['raw_data']


            # Accumulate sample frequency
            timestamp = record_dict['timestamp']
            # The format string that includes milliseconds
            format_str = "%Y-%m-%d %H:%M:%S.%f"
            curr_datetime = datetime.strptime(timestamp, format_str)
            if last_datetime:
                diff = curr_datetime - last_datetime
                diff_ms = diff.total_seconds() * 1000  # diff.total_seconds() returns microseconds
                #print(diff_ms)  # ms difference for 10 samples

                # Since the total duration covers 10 samples, calculate the sampling period for one sample
                # and then convert to frequency
                sampling_period_per_sample_ms = diff_ms / 10

                # Convert the sampling period to frequency in Hz
                if sampling_period_per_sample_ms != 0:
                    sampling_frequency_hz = 1000 / sampling_period_per_sample_ms

                val = int(round(sampling_frequency_hz))
                print(f"{val}")
                if val > 0:
                    freq.append(val)
            last_datetime = curr_datetime
            
            # Accumulate the data
            for frame in raw_data:
                ax_data.append(float(frame['ax']))
                ay_data.append(float(frame['ay']))
                az_data.append(float(frame['az']))
                gx_data.append(float(frame['gx']))
                gy_data.append(float(frame['gy']))
                gz_data.append(float(frame['gz']))

    # Plotting the accelerometer data
    plt.figure(figsize=(10, 6))
    plt.subplot(3, 1, 1)
    plt.plot(ax_data, label='ax')
    plt.plot(ay_data, label='ay')
    plt.plot(az_data, label='az')
    plt.title('Accelerometer Data')
    plt.legend()

    # Plotting the gyroscope data
    plt.subplot(3, 1, 2)
    plt.plot(gx_data, label='gx')
    plt.plot(gy_data, label='gy')
    plt.plot(gz_data, label='gz')
    plt.title('Gyroscope Data')
    plt.legend()

    # Plotting the sampling freq
    plt.subplot(3, 1, 3)
    x = np.arange(len(freq))
    plt.bar(x, freq, label='frequency (Hz)')
    plt.title('Sampling Frequency')
    plt.xticks(np.arange(min(x), max(x)+1, 10))
    plt.legend()

    plt.tight_layout()
    plt.show()


    #Count the frequency of each integer
    frequency = Counter(freq)

    # Separate the data into labels (the unique integers) and their corresponding frequencies
    labels, heights = zip(*frequency.items())

    # Sorting the labels and heights based on labels to make the bar graph ordered
    labels, heights = zip(*sorted(zip(labels, heights)))

    # Create a bar graph
    plt.bar(labels, heights)

    # Set the x-axis to show integer labels between 10 and 20
    plt.xticks(range(10, 21))

    # Ensure the y-axis only shows integer values
    plt.gca().yaxis.set_major_locator(MaxNLocator(integer=True))

    # Adding labels and title for clarity
    plt.xlabel('Sampling Frequency (Hz)')
    plt.ylabel('Number of Occurrences')
    plt.title('Occurence of Sampling Frequencies')

    # Show the plot
    plt.show()

if __name__ == "__main__":
    main()
