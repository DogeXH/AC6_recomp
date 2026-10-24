#!/usr/bin/env python3
# Tiny UDP echo for the rig self-test (TESTING_WINDOWS.md): win_udp_echo.py PORT [seconds=1800].
# Replies "echo:<payload>" to each datagram and logs it. Stop it by its PID.
import socket, sys, time
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); s.bind(("0.0.0.0", int(sys.argv[1])))
end = time.time() + (int(sys.argv[2]) if len(sys.argv) > 2 else 1800)
while time.time() < end:
    s.settimeout(5)
    try:
        d, a = s.recvfrom(2048)
    except socket.timeout:
        continue
    print(time.strftime("%H:%M:%S"), a, d[:80], flush=True)
    s.sendto(b"echo:" + d, a)
