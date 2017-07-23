#!/usr/bin/env python
# powercom_decoder_packet.py - Recover packet data from demodulated bit stream
#
# This script takes the output of the powercom demodulator and recovers the
# data from it.
#
#
# Copyright (c) 2017 David Imhoff <dimhoff.devel@gmail.com>
#
# Permission is hereby granted, free of charge, to any person obtaining a copy
# of this software and associated documentation files (the "Software"), to deal
# in the Software without restriction, including without limitation the rights
# to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
# copies of the Software, and to permit persons to whom the Software is
# furnished to do so, subject to the following conditions:
#
# The above copyright notice and this permission notice shall be included in
# all copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
# AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
# OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
# SOFTWARE.
#
import argparse
import serial
import socket
import struct
import sys

SOCKET_READ_SIZE = 2 * 1024

# Amount of samples per bit, This is fixed in demodulator
BIT_PERIOD = 100

# Max. packet size
MAX_PKT_LEN = 16

debug = False
test_mode = False

server_ip = '127.0.0.1'
server_port = 5555

def near(x, y, max_dev):
    dev = abs(x-y)
    if debug:
        print("dev: {}, max_dev: {}".format(dev, max_dev))
    return (dev <= max_dev)

class PowercomDecoder:
    def __init__(self):
        self.state = 'idle'
        self.last_bit = False
        self.sample_idx = 0
        self.last_edge_idx = 0
        self.invert = False

    def process_data(self, data):
        for c in data:
            byte = ord(c)
            mask = 0x80
            while mask:
                bit = ((byte & mask) != 0)
                edge = (bit != self.last_bit)
                
                self.process_bit(bit, edge)

                if edge:
                    self.last_edge_idx = self.sample_idx

                self.last_bit = bit
                self.sample_idx += 1
                mask >>= 1

    def process_bit(self, bit, edge):
        if debug and edge:
            print("{} (+{}) - DEBUG: edge {}, state {}".format(self.sample_idx, self.sample_idx - self.last_edge_idx, int(bit), self.state))

        if self.state == 'idle':
            if edge:
                if near(self.sample_idx - self.last_edge_idx, BIT_PERIOD, 0.40 * BIT_PERIOD):
                    self.state = 'preamble'
        elif self.state == 'preamble':
            if edge:
                if near(self.sample_idx - self.last_edge_idx, 4 * BIT_PERIOD, 0.40 * 4 * BIT_PERIOD):
                    self.state = 'receive'
                    self.next_bit_sample_idx = int(self.sample_idx + 1.5 * BIT_PERIOD)
                    self.invert = 1 if bit == 0 else 0

                    self.data = ''
                    self.recv_len = 0
                    self.data_byte = 0
                    self.bits_received = 0
                elif not near(self.sample_idx - self.last_edge_idx, BIT_PERIOD, 0.40 * BIT_PERIOD):
                    self.state = 'idle'
        elif self.state == 'receive':
            if self.sample_idx >= self.next_bit_sample_idx:
                bit = bit ^ self.invert

                if self.bits_received < 8:
                    self.recv_len = ((self.recv_len << 1) | bit) & 0xff

                    # 0 byte packets don't exist, these are full packets instead
                    if self.bits_received == 7:
                        if self.recv_len == 0:
                            self.recv_len = 0x100

                        if (self.recv_len > MAX_PKT_LEN or
                                (test_mode and self.recv_len != 2)):
                            if debug:
                                print("Packet length to long: " + str(self.recv_len))
                            self.state = 'idle'
                else:
                    self.data_byte = ((self.data_byte << 1) | bit) & 0xff
                    if (self.bits_received + 1) % 8 == 0:
                        self.data += chr(self.data_byte)
                        if len(self.data) == self.recv_len:
                            if test_mode:
                                if ord(self.data[0]) == ord(self.data[1]) ^ 0xff:
                                    print("{}".format(ord(self.data[0])))
                                elif debug:
                                    print("CORRUPT ID: 0x{:02x} != 0x{:02x}".format(ord(data[0]), ord(data[1])))
                            elif debug:
                                for c in self.data:
                                    print("RECEIVED: 0x{:02x} '{}'".format(ord(c), c))
                            else:
                                sys.stdout.write(self.data)
                                sys.stdout.flush()

                            self.state = 'idle'

                self.bits_received += 1
                self.next_bit_sample_idx += BIT_PERIOD


# Parse command line arguments
parser = argparse.ArgumentParser(
        description='Decode powercom packets')
parser.add_argument('-t', '--test-mode',
        dest='test_mode', default=False, action='store_true',
        help='Receive test mode sequence packets')
parser.add_argument('-d', '--debug',
        dest='debug', default=False, action='store_true',
        help='Enable debug output')

args = parser.parse_args()

test_mode = args.test_mode
debug = args.debug

# Setup server socket
s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind((server_ip, server_port))
s.listen(1)

while True:
    conn, addr = s.accept()
    print('New Connection from {}'.format(addr))

    decoder = PowercomDecoder()

    buf = conn.recv(SOCKET_READ_SIZE)
    while len(buf) > 0:
        decoder.process_data(buf)

        buf = conn.recv(SOCKET_READ_SIZE)

    print('Client {} disconnected'.format(addr[0]));
    conn.close()
