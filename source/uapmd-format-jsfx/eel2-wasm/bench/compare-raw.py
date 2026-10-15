# Compares two raw float32 sample files and reports the largest difference and where outputs first diverge.
import struct
import sys

a = open(sys.argv[1], 'rb').read()
b = open(sys.argv[2], 'rb').read()
if len(a) != len(b):
    print(f"LENGTH {len(a)} vs {len(b)}")
    sys.exit(0)
n = len(a) // 4
fa = struct.unpack(f'<{n}f', a)
fb = struct.unpack(f'<{n}f', b)
first = -1
worst = 0.0
for i in range(n):
    if fa[i] != fb[i] and not (fa[i] != fa[i] and fb[i] != fb[i]):
        if first < 0:
            first = i
        worst = max(worst, abs(fa[i] - fb[i]))
print("EXACT" if first < 0 else f"DIFF max={worst:g} first={first}")
