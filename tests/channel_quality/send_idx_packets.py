#!/usr/bin/env python
import argparse
import os
import struct
import time

parser = argparse.ArgumentParser(
        description='Send indexed packet for packet loss testing')
parser.add_argument('carrier', type=int,
        help='Carrier wave frequency in Hz')
parser.add_argument('bit_periods', type=int,
        help='Amount of carrier wave periods per bit')

args = parser.parse_args()


for i in xrange(0, 256):
    print("packet " + str(i))
    data = struct.pack("BB", i, i ^ 0xff)
    with open("/tmp/test.dat", 'wb') as outf:
        outf.write(data)

    os.system("./powercom_send -M bpsk -E packet -c " + str(args.carrier) + " -p " + str(args.bit_periods) + " -f /tmp/test.dat")

    time.sleep(1)
