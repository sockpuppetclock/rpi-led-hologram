#!/usr/bin/python3

import random
import serial
import os
import re
import time
import subprocess
import signal
import zmq

context = zmq.Context()
socket = context.socket(zmq.REP)
ser = None

def OpenSerial():
    global ser
    ser = serial.Serial('/dev/ttyS0', 115200, timeout=1)
    print("opened /dev/ttyS0")

def CloseSerial():
    ser.close()

# serial controls #

def ReceiveAnimState():
    folder_length = int.from_bytes(ser.read(1), 'big')
    folder_name = ser.read(folder_length).decode('utf-8')
    print("anim state:"+state)
    socket.send(state)

def ReceiveSwipe():
    dir_length = int.from_bytes(ser.read(1), 'big')
    dir = ser.read(dir_length)
    print(f"Receiving swipe {dir}")
    for d in dir:
        letter = chr(d)

def ReceiveGesture():
    dir_length = int.from_bytes(ser.read(1), 'big')
    dir = ser.read(dir_length)
    print(f"Receiving gesture {dir}")
    for d in dir:
        letter = chr(d)

def ReceiveImage():
    filename_length = int.from_bytes(ser.read(1), 'big')
    filename = ser.read(filename_length).decode('utf-8')
    print(f"Receiving file: {filename}")

    match = re.match(r"([^\d]+)", os.path.splitext(filename)[0])
    folder_name = match.group(1) if match else "unknown"
    folder_name = "images/"+folder_name
    os.makedirs("images",exist_ok=True)
    os.makedirs(folder_name, exist_ok=True)

    file_size = int.from_bytes(ser.read(4), 'big')
    received_data = b''
    while len(received_data) < file_size:
        chunk = ser.read(file_size - len(received_data))
        if not chunk:
            break
        received_data += chunk

    filepath = os.path.join(folder_name, filename)
    with open(filepath, 'wb') as f:
        f.write(received_data)

    print(f"File saved: {filepath}")

### UART controller RECEIVES through SERIAL and SENDS through ZEROMQ

def main():
    os.chdir("/hologram/utils/")
    OpenSerial()
    socket.bind("tcp://*:5555") # server
    t = time.time()
    test = time.time()
    try:
        # main loop #
        while True:
            message = socket.recv()
            message = bytes(message)
            print(f"Received request : {message}")
            if(message == b'\x01'):
                socket.send_string("World")
                print("Sent World")
            else:
                socket.send_string("0")
                print("Sent")
            
            if ser.in_waiting:
                control_byte = ser.read(1)
                print(f"control: {control_byte}")

                # sending image
                if control_byte == b'\x01':
                    ReceiveImage()

                # change animation state
                elif control_byte == b'\x02':
                    ReceiveAnimState()
                    ChangeAnimState(folder_name)

                # receive swipe
                elif control_byte == b'\x03':
                    ReceiveSwipe()

                # receive gesture
                # elif control_byte == b'\x04':
                #     ReceiveGesture()
    except KeyboardInterrupt:
        CloseSerial()

main()