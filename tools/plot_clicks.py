#!/usr/bin/python3

import argparse
import os
import sys
import numpy as np
import matplotlib.pyplot as plt
from matplotlib.backends.backend_pdf import PdfPages
from scipy.io import wavfile

def find_and_plot_anomalies(filename, threshold, window=20):
    # 1. Read WAV file
    try:
        sample_rate, raw_data = wavfile.read(filename)
    except Exception as e:
        print(f"Error reading {filename}: {e}")
        sys.exit(1)

    # Cast to float64 to prevent integer overflow on differences
    data = raw_data.astype(np.float64)

    # Standardize to 2D array (samples, channels)
    if data.ndim == 1:
        data = data.reshape(-1, 1)

    total_samples, num_channels = data.shape
    anomalies = []

    # First derivative: compares N to N-1
    diffs = np.diff(data, axis=0)
    abs_diffs = np.abs(diffs)
    
    for ch in range(num_channels):
        # --- A. Find jumps exceeding the threshold ---
        jump_indices = np.where(abs_diffs[:, ch] >= threshold)[0]
        for idx in jump_indices:
            jump_val = diffs[idx, ch]
            # Store as: (start_idx, end_idx, channel, value, type, length)
            anomalies.append((idx, idx + 1, ch, jump_val, 'jump', 2))
            
        # --- B. Find consecutive identical samples (flatlines < 10 samples) ---
        # A difference of 0 means sample N and N+1 are identical
        is_zero = (diffs[:, ch] == 0)
        
        # Pad with False at the ends to reliably detect edges of flatline blocks
        padded = np.concatenate(([False], is_zero, [False]))
        
        # Find where the boolean array changes state (False->True or True->False)
        changes = np.where(padded[:-1] != padded[1:])[0]
        
        # The starts are the even indices, the ends are the odd indices
        starts = changes[0::2]
        ends = changes[1::2]
        
        for start_idx, end_idx in zip(starts, ends):
            # The number of zeros in the diff array is (end_idx - start_idx)
            # The number of identical samples is the number of zeros + 1
            num_identical = (end_idx - start_idx) + 1
            
            # Rule: Flag if several in a row, but fewer than 10
            if 1 < num_identical < 10:
                sample_val = data[start_idx, ch]
                # Store the start and end of the identical block
                anomalies.append((start_idx, end_idx, ch, sample_val, 'flatline', num_identical))

    # Sort anomalies chronologically by start sample index, then by channel
    anomalies.sort(key=lambda x: (x[0], x[2]))

    if not anomalies:
        print(f"No jumps (>= {threshold}) or short flatlines (< 10 samples) found.")
        return

    print(f"Found {len(anomalies)} anomalie(s). Generating PDF...")

    # 3. Setup PDF output
    base_name = os.path.splitext(filename)[0]
    pdf_filename = f"{base_name}-clicks.pdf"

    with PdfPages(pdf_filename) as pdf:
        for start_idx, end_idx, ch, val, mtype, length in anomalies:
            # Determine window bounds (show N samples before the start and after the end)
            window_start = max(0, start_idx - window)
            window_end = min(total_samples, end_idx + window + 1)

            # Sample indices and values for the region
            x_indices = np.arange(window_start, window_end)
            y_values = data[window_start:window_end, ch]

            # Standard Letter Page Dimensions
            fig, ax = plt.subplots(figsize=(8.5, 5))

            # Plot raw audio points and connecting line
            ax.plot(x_indices, y_values, marker='o', color='tab:blue', 
                    markersize=4, linewidth=1.5, label='Audio Samples')

            # Highlight the anomaly based on the type
            if mtype == 'jump':
                ax.plot([start_idx, end_idx], [data[start_idx, ch], data[end_idx, ch]], 
                        color='red', linewidth=2.5, label='Vertical Jump')
                metric_label = f"Jump Level: {val:+.2f}"
                title_prefix = "Jump Click"
            else: # mtype == 'flatline'
                # Generate x and y points for the exact flatline segment
                flat_x = np.arange(start_idx, end_idx + 1)
                flat_y = data[start_idx:end_idx + 1, ch]
                
                ax.plot(flat_x, flat_y, color='darkorange', linewidth=3.0, 
                        label=f'Flatline ({length} samples)')
                metric_label = f"Sample Value: {val:.2f}"
                title_prefix = f"Flatline ({length} samples)"

            # Build metadata title
            ch_str = f"Channel {ch}" if num_channels > 1 else "Mono"
            
            ax.set_title(
                f"{title_prefix} at Sample {start_idx} | {ch_str} | {metric_label}",
                fontsize=12, fontweight='bold', pad=12
            )

            ax.set_xlabel("Sample Number")
            ax.set_ylabel("Amplitude")
            ax.grid(True, linestyle='--', alpha=0.6)
            ax.legend(loc='upper right')
            ax.xaxis.get_major_formatter().set_useOffset(False)
            
            plt.tight_layout()
            pdf.savefig(fig)
            plt.close(fig)

    print(f"Successfully created: {pdf_filename}")

if __name__ == "__main__":
    parser = argparse.ArgumentParser(
        description="Locate audio jumps and short identical-sample flatlines, plotting them to a PDF."
    )
    parser.add_argument("input_file", help="Path to the input WAV file")
    parser.add_argument("threshold", type=float, help="Minimum jump magnitude to be considered a click.")
    parser.add_argument("--window", type=int, default=20, help="Samples to display before/after (default: 20)")

    args = parser.parse_args()

    find_and_plot_anomalies(args.input_file, args.threshold, args.window)
