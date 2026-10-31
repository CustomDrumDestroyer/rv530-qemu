#!/usr/bin/env python3
"""Connect a slirp guestfwd connection to a unix socket.

    guestfwd=tcp:10.0.2.100:7651-cmd:python3 relay.py SOCK

slirp runs this per guest connection with the guest's TCP stream on
stdin/stdout; it copies bytes both ways to SOCK (the rv530 device's
clipboard chardev, a QEMU socket server) until either side closes.
"""
import os, select, socket, sys


def main():
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.connect(sys.argv[1])
    fin, fout = sys.stdin.fileno(), sys.stdout.fileno()
    while True:
        r, _, _ = select.select([fin, s], [], [])
        if fin in r:
            b = os.read(fin, 65536)
            if not b:
                break
            s.sendall(b)
        if s in r:
            b = s.recv(65536)
            if not b:
                break
            while b:
                b = b[os.write(fout, b):]


if __name__ == '__main__':
    main()
