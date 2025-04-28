#!/usr/bin/python3

import random
import serial
import os
import re
import time
import subprocess
import signal

current_process = None
ser = None
procs = {}

def OpenSerial():
    global ser
    ser = serial.Serial('/dev/ttyS0', 115200, timeout=1)
    print("opened /dev/ttyS0")

def CloseSerial():
    ser.close()

def ReceiveSwipe():
    dir_length = int.from_bytes(ser.read(1), 'big')
    dir = ser.read(dir_length)
    print(f"Receiving swipe {dir}")
    for d in dir:
        letter = = chr(d)
        # do whatever you need with the character L or R @johnathon

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

def LoopFolder(state):
    print("LoopFolder("+state+")")
    folder_name = "images/"+state
    global current_process
    # kill previous process if running
    # try:
        # print(f"PID={current_process.pid}") 
    # except:
        # print("pass")
    if current_process == None:
        os.killpg(os.getpgid(procs[state].pid), signal.SIGCONT)
        current_process = state
        print(current_process+" start")
    if current_process != None:
        os.killpg(os.getpgid(procs[current_process].pid), signal.SIGUSR1)
        os.killpg(os.getpgid(procs[current_process].pid), signal.SIGSTOP)
        os.killpg(os.getpgid(procs[state].pid), signal.SIGCONT)
        print(current_process+" -> "+state)
        current_process = state
        # current_process.wait()
        # print("Previous process terminated")
        # time.sleep(1)

    # try:
    #     # while True:
    #     files = os.listdir(folder_name)
    #     # images = [f for f in files if f.lower().endswith(('.png', '.jpg', '.jpeg'))]
    #     print(f"Images in {folder_name}:")
    #     # for img in images:
    #     print(folder_name + '/*.png')
    #     filepath = folder_name + '/*.png'
    #     filepath2 = folder_name + '_Idle/*.png'
    #     try:
    #         command = f'/home/dietpi/rpi-led-hologram/utils/led-image-viewer --led-rows=64 --led-cols=64 --led-pwm-dither-bits=2 --led-slowdown-gpio=3 --led-pwm-bits=4 --led-pwm-lsb-nanoseconds=50 --led-pixel-mapper="Rotate:270" -f -w0.0008 {filepath} idle {filepath2}'
    #         current_process = subprocess.Popen(command, shell=True, preexec_fn=os.setsid)
    #         print(f"Started viewer for {filepath}")
    #         print(f"PID={current_process.pid}") 
    #         print(f"GPID={os.getpgid(current_process.pid)}") 
    #     except Exception as e:
    #         print(f"Failed to run viewer: {e}")
    #         current_process = None

    #         # time.sleep(2)  ##### time unit to wait.  need to adjust to match the motor driver unit
    # except KeyboardInterrupt:
    #     print("Stopped folder loop")
    # except:
    #     print("Excepted on Loop Folder")
    # return current_process

def InitFolders():
    p = 0
    for folder_name in os.listdir("/home/dietpi/rpi-led-hologram/utils/images/"):
        if folder_name.endswith("_Idle"):
            continue
        print(folder_name + '/*.png')
        filepath = "/home/dietpi/rpi-led-hologram/utils/images/" + folder_name + '/*.png'
        filepath2 = "/home/dietpi/rpi-led-hologram/utils/images/" + folder_name + '_Idle/*.png'
        try:
            command = f'/home/dietpi/rpi-led-hologram/utils/led-image-viewer --led-rows=64 --led-cols=64 --led-limit-refresh=1000 --led-pwm-dither-bits=2 --led-slowdown-gpio=3 --led-pwm-bits=4 --led-pwm-lsb-nanoseconds=50 --led-pixel-mapper="Rotate:270" -f -w0.0008 {filepath} idle {filepath2}'
            procs[folder_name] = subprocess.Popen(command, shell=True, preexec_fn=os.setsid)
            print(f"Started viewer for {filepath}")
            print(f"PID={procs[folder_name].pid}") 
            print(f"GPID={os.getpgid(procs[folder_name].pid)}")
            time.sleep(3)
            os.killpg(os.getpgid(procs[folder_name].pid), signal.SIGUSR1)
            os.killpg(os.getpgid(procs[folder_name].pid), signal.SIGSTOP)
        except Exception as e:
            print(f"Failed to run viewer: {e}")
    print(procs)


def MainLoop():
    t = time.time()
    test = time.time()
    try:
        InitFolders()
        LoopFolder("Idle")
        while True:
            # test
            # if current_process == None:
            #     LoopFolder("snoozing")
            # 
            test = time.time()
            if test - t > 3:
                t = test
                f = random.choice(list(procs.keys()))
                LoopFolder(f)
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
                # receive swipe
                elif control_byte == b'\x03':
                    ReceiveSwipe()
    except KeyboardInterrupt:
        for key in procs:
            print(key, procs[key])
            os.killpg(os.getpgid(procs[key].pid), signal.SIGCONT)
            time.sleep(0.1)
            os.killpg(os.getpgid(procs[key].pid), signal.SIGTERM)
            procs[key].wait()
    except:
        if current_process != None:
            os.killpg(os.getpgid(procs[current_process].pid), signal.SIGTERM)
            procs[current_process].wait()
        for key in procs:
            if key == current_process:
                continue
            os.killpg(os.getpgid(procs[key].pid), signal.SIGCONT)
            time.sleep(1)
            os.killpg(os.getpgid(procs[key].pid), signal.SIGTERM)
            procs[key].wait()

os.chdir("/home/dietpi/rpi-led-hologram/utils/")

OpenSerial()
MainLoop()
#CloseSerial() # currently not doing this for infinite loop
