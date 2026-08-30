#!/usr/bin/python3
# Copyright (c) 2026 Elliott H. Liggett
# SPDX-License-Identifier: GPL-3.0-or-later

import argparse
import numpy as np
from scipy.io import wavfile
import sys

def find_clicks(filename, threshold):
    try:
        # Read the WAV file
        sample_rate, data = wavfile.read(filename)
    except Exception as e:
        print(f"Error reading {filename}: {e}")
        sys.exit(1)

    # Cast to float64 to prevent overflow when calculating the difference 
    # between consecutive samples (e.g., 32767 - (-32768) overflows int16)
    data = data.astype(np.float64)

    # Normalize mono to a 2D array (samples, 1 channel) for consistent processing
    if data.ndim == 1:
        data = data.reshape(-1, 1)

    num_channels = data.shape[1]
    
    # Calculate the step difference between consecutive samples
    # np.diff subtracts sample n from sample n+1
    diffs = np.diff(data, axis=0)
    abs_diffs = np.abs(diffs)

    clicks_found = 0

    # Search each channel for jumps exceeding the threshold
    for ch in range(num_channels):
        # np.where returns a tuple, we want the first array of indices
        click_indices = np.where(abs_diffs[:, ch] >= threshold)[0]
        
        for idx in click_indices:
            jump = diffs[idx, ch]
            channel_str = f"Channel {ch} | " if num_channels > 1 else ""
            print(f"{channel_str}Sample: {idx: <10} | Jump: {jump}")
            clicks_found += 1

    if clicks_found == 0:
        print(f"No clicks found exceeding threshold {threshold}.")
    else:
        print(f"\nTotal clicks found: {clicks_found}")

if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Find clicks (large sample steps) in a WAV file.")
    parser.add_argument("input_file", help="Path to the input WAV file")
    parser.add_argument("threshold", type=float, help="Minimum step magnitude between consecutive samples to be considered a click.")
    
    args = parser.parse_args()
    
    find_clicks(args.input_file, args.threshold)
