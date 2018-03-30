#!/usr/bin/python
import socket
import time

s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)

def getAll(r, g, b):
    result = bytes(0)
    for i in range(19):
        result += bytes((r, g, b))
    return result

s.sendto(getAll(0, 255, 0), ("192.168.2.107", 1234))

# while True:
#     for i in range(255):
#         s.sendto(getAll(0, i, 0), ("192.168.2.107", 1234))
#         time.sleep(0.05)
#         print(time.time())
#     for i in range(255):
#         s.sendto(getAll(0, 255-i, 0), ("192.168.2.107", 1234))
#         time.sleep(0.05)

