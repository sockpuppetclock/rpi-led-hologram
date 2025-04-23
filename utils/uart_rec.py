#!/usr/bin/python3

import serial
import os
import re
import time
import subprocess

ser = None
def OpenSerial():
    global ser
    ser = serial.Serial('/dev/ttyS0', 115200, timeout=1)
    print("opened /dev/ttyS0")

def CloseSerial():
    ser.close()

def ReceiveImage():
    filename_length = int.from_bytes(ser.read(1), 'big')
    filename = ser.read(filename_length).decode('utf-8')
    print(f"Receiving file: {filename}")

    match = re.match(r"([^\d]+)", os.path.splitext(filename)[0])
    folder_name = match.group(1) if match else "unknown"
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

def LoopFolder(folder_name):
    #print(f"Looping through folder: {folder_name}")
    p = None
    try:
        # while True:
        files = os.listdir(folder_name)
        # images = [f for f in files if f.lower().endswith(('.png', '.jpg', '.jpeg'))]
        print(f"Images in {folder_name}:")
        # for img in images:
        print(folder_name + '/*.png')
        filepath = folder_name + '/*.png'
        try:
            command = f'/home/dietpi/rpi-rgb-led-matrix/utils/led-image-viewer --led-rows=64 --led-cols=64 --led-show-refresh --led-pwm-dither-bits=2 --led-slowdown-gpio=3 --led-pwm-bits=4 --led-pwm-lsb-nanoseconds=50 -f -w0.0008 {filepath}'
            p = subprocess.run(command, shell=True, check=True)
        except subprocess.CalledProcessError as e:
            print(f"Failed to run ./viewer on {filepath}: {e}")

            # time.sleep(2)  ##### time unit to wait.  need to adjust to match the motor driver unit
    except KeyboardInterrupt:
        print("Stopped folder loop")
    except:
        if p != None:
            p.terminate()

def MainLoop():
    while True:
        if ser.in_waiting:
            # sending over an image
            control_byte = ser.read(1)
            print(control_byte)
            if control_byte == b'\x01':
                ReceiveImage()
            # change animation state
            elif control_byte == b'\x02':
                folder_length = int.from_bytes(ser.read(1), 'big')
                folder_name = ser.read(folder_length).decode('utf-8')
                LoopFolder(folder_name)

OpenSerial()
MainLoop()
#CloseSerial() # currently not doing this for infinite loop
