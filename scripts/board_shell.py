import socket, time, sys

IP = "192.168.1.4"

def run_cmd(cmd, wait=2.0, port=23):
    s = socket.socket()
    s.settimeout(2.0)
    try:
        s.connect((IP, port))
    except Exception as e:
        return f"CONNECT_ERR {e}"

    # Handle Telnet IAC negotiation
    end = time.time() + 2.0
    while time.time() < end:
        try:
            c = s.recv(1024)
            if not c: break
            out = b""
            i = 0
            while i < len(c):
                if c[i] == 0xff and i + 1 < len(c):
                    op = c[i+1]
                    if op in (0xfb, 0xfc):
                        if i + 2 < len(c): out += b"\xff\xfe" + bytes([c[i+2]])
                    elif op in (0xfd, 0xfe):
                        if i + 2 < len(c): out += b"\xff\xfc" + bytes([c[i+2]])
                    i += 3
                else:
                    i += 1
            if out:
                s.sendall(out)
            if b"#" in c:
                break
        except (socket.timeout, socket.error):
            break

    # Send command
    try:
        s.sendall(cmd.encode("ascii", errors="replace") + b"\n")
    except Exception as e:
        s.close()
        return f"SEND_ERR {e}"

    time.sleep(wait)
    out = b""
    read_end = time.time() + 3.0
    while time.time() < read_end:
        try:
            chunk = s.recv(4096)
            if not chunk: break
            out += chunk
        except socket.timeout:
            break

    try:
        s.sendall(b"exit\n")
    except Exception:
        pass
    s.close()
    return out.decode("utf-8", errors="replace")

if __name__ == "__main__":
    commands = sys.argv[1:]
    for cmd in commands:
        print("=== CMD:", cmd, "===")
        print(run_cmd(cmd))