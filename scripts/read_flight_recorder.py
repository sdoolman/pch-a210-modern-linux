import sys, os
from pch_telnet import run_cmd

cmd = """/share/Apps/local/bin/python -c "
import os, struct
f = os.open('/dev/mem', os.O_RDONLY)
os.lseek(f, 0x61900, 0)
raw = os.read(f, 64)
os.close(f)
words = struct.unpack('<16I', raw)
for i, w in enumerate(words):
    print 'LRAM[0x%03x] = 0x%08x' % (0x900 + i*4, w)
"
"""

print(run_cmd(cmd, wait=2.0))
