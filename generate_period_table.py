#!/usr/bin/env python3
"""
Generate period lookup table for 1V/oct pitch control.
Period = delay in samples, inversely proportional to frequency.
"""

import math

# Target: 341 entries per octave, 2:1 ratio across table
NUM_ENTRIES = 341

# Calibration:
# At k=2241 (C1, 0V input), oct=6, suboct=195
# We want ExpPeriod to return 1468 (C1 delay at 48kHz)
# ExpPeriod returns: period_vals[suboct] >> oct
# So: period_vals[195] >> 6 = 1468
# Therefore: period_vals[195] = 1468 * 64 = 93952
#
# Working backwards: period_vals[i] = BASE * 2^(-i/341)
# period_vals[195] = BASE * 2^(-195/341) = BASE * 0.6674 = 93952
# BASE = 93952 / 0.6674 = 140763

BASE_VALUE = 140763  # Calibrated for C1 at k=2241

print("// Period lookup table - direct delay values (no division needed)")
print("// period_vals[i] = {} * 2^(-i/{}), pre-computed for precision".format(BASE_VALUE, NUM_ENTRIES))
print("// Values scaled so period_vals[0] >> 6 gives ~1514 samples (C1 delay)")
print("// Table has exact 2:1 ratio from index 0 to 340")
print("static const int32_t period_vals[{}] = {{".format(NUM_ENTRIES))

values = []
for i in range(NUM_ENTRIES):
    # period decreases exponentially: period = base * 2^(-i/341)
    val = round(BASE_VALUE * math.pow(2, -i / NUM_ENTRIES))
    values.append(val)

# Print in rows of 10
for i in range(0, NUM_ENTRIES, 10):
    row = values[i:i+10]
    row_str = ", ".join(str(v) for v in row)
    if i + 10 < NUM_ENTRIES:
        print("    " + row_str + ",")
    else:
        print("    " + row_str)

print("};")

# Verify ratio
print("\n// Verification:")
print("// First value: {}".format(values[0]))
print("// Last value: {}".format(values[-1]))
print("// Ratio (should be ~2.0): {:.6f}".format(values[0] / values[-1]))
print("// At oct=6, index=0: {} >> 6 = {} samples".format(values[0], values[0] >> 6))
