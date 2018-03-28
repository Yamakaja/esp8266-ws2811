#!/usr/bin/python
import socket
import time

s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)

def getAll(r, g, b):
    result = bytes(0)
    for i in range(19):
        result += bytes((g, r, b))
    result += b'\xFE'
    return result

while True:
    input()
    s.sendto(getAll(255, 0, 0), ("192.168.2.107", 1234))
    input()
    s.sendto(getAll(0, 0, 0), ("192.168.2.107", 1234))

