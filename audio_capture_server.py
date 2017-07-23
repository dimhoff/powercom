#!/usr/bin/env python
import traceback
import socket
import struct
import subprocess
import sys
import threading
from time import sleep

RATE = 2000

MIN_PACKET_SIZE = 1400

server_ip = '0.0.0.0'
server_port = 12345

#ENV: AUDIODEV=hw:1,0
SOX_COMMAND = ( 'sox', '|rec -c 1 -r 48000 -p', '-b', '32', '-e', 'float', '-t', 'raw', '-', 'rate', str(RATE) )

def AudioReadThread():
    while True:
        try:
            sox_process = subprocess.Popen(SOX_COMMAND, stdin=None, stdout=subprocess.PIPE)
        except:
            print("Failed to start SOX recording")
            traceback.print_exc(file=sys.stdout)
            sleep(5)
            continue

        try:
            while sox_process.poll() is None:
                out_buf = sox_process.stdout.read(MIN_PACKET_SIZE)

                with clients_lock:
                    for conn in clients:
                        try:
                            #print("sending {} bytes to {}".format(len(out_buf), conn))
                            conn.send(out_buf)
                        except:
                            print("Closed connection {}".format(conn));
                            conn.close()
                            clients.remove(conn)

        except:
            print("Exception in audio reader:")
            traceback.print_exc(file=sys.stdout)
            try:
                sox_process.kill()
            except:
                pass

        with clients_lock:
            for conn in clients:
                print("Closed connection {}".format(conn));
                conn.close()
                clients.remove(conn)

        sox_process.stdout.close()
        sox_process.wait()

    p.terminate()

# Setup server socket
s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind((server_ip, server_port))
s.listen(1)

clients = []
clients_lock = threading.Lock()

# Start Audio capture thread
serial_read_thread = threading.Thread(target=AudioReadThread, name='Audio Reader')
serial_read_thread.setDaemon(True)
serial_read_thread.start()

while True:
    conn, addr = s.accept()
    print("New connection from {}".format(addr))
    with clients_lock:
        clients.append(conn)


