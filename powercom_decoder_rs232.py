#!/usr/bin/env python
# powercom_decoder_rs232.py - Recover data from rs232 encoded demodulated bit
#                              stream
#
# This does nearly the same as powercom_decoder_packet.py, but instead uses the
# RS-232 like data encoding. This encoding has less overhead for small amounts
# of data but doesn't work for PSK modulation, due to the lack of a preamble.
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

BITS_PER_FRAME = 7
STOP_BITS = 1

debug = False

server_ip = '127.0.0.1'
server_port = 5555

class PowercomDecoder:
    def __init__(self):
        self.idle = True
        self.last_bit = False
        self.sample_idx = 0
        self.last_edge_idx = 0

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
            print("{} ({:.2f}) - DEBUG: edge {}".format(self.sample_idx, float(self.sample_idx) / BIT_PERIOD, int(bit)))
            pass

        if self.idle:
            if edge: # Start bit
                if bit: # rising edge
                    #print("DEBUG: rising edge in idle state")
                    self.sample_idx = 0
                    self.idle = False
                    self.next_sample_idx = self.sample_idx + int(BIT_PERIOD * 1.5)
                    self.data = 0
                    self.bits_received = 0
        else:
            if self.sample_idx == self.next_sample_idx:
                if self.bits_received >= BITS_PER_FRAME: # Stop bits
                    if bit:
                        print("FRAMING ERROR: 0x{:02x} '{}'".format(self.data, chr(self.data & 0x7f)))
                        self.idle = True
                        return
                    else:
                        self.bits_received += 1
                        if self.bits_received == BITS_PER_FRAME + STOP_BITS:
                            if debug:
                                print("RECEIVED: 0x{:02x} '{}'".format(self.data, chr(self.data & 0x7f)))
                            else:
                                sys.stdout.write(chr(self.data & 0x7f))
                                sys.stdout.flush()
                            self.idle = True
                            return
                else: # Data bits
                    self.data = (self.data << 1) | bit
                    self.bits_received += 1

                self.next_sample_idx += BIT_PERIOD


# Parse command line arguments
parser = argparse.ArgumentParser(
        description='Decode powercom packets')
parser.add_argument('-d', '--debug',
        dest='debug', default=False, action='store_true',
        help='Enable debug output')

args = parser.parse_args()

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
