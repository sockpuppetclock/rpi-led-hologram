#!/usr/bin/python3
import serial
import time
import os
import gpiod
import fcntl
import sys
#start_time = time.time()
pin_ri = 13
pin_le = 6
chip = gpiod.Chip('gpiochip4')

line_ri = chip.get_line(pin_ri)
line_le = chip.get_line(pin_le)
line_ri.request(consumer="input", type=gpiod.LINE_REQ_DIR_IN)
line_le.request(consumer="input", type=gpiod.LINE_REQ_DIR_IN)

ser = None

def OpenSerial():
    global ser
    ser = serial.Serial('/dev/ttyAMA0', 2000000)

def CloseSerial():
    ser.close()

def SendImage(image_path):
    with open(image_path, 'rb') as f:
        data = f.read()

    filename = os.path.basename(image_path)
    filename_bytes = filename.encode('utf-8')
    size = len(filename_bytes) + len(data) + 3
   
    ser.write(b'\x01')  # Control byte for file transfer
    ser.write(size.to_bytes(2,'big'))
    ser.write(len(filename_bytes).to_bytes(1, 'big'))
    ser.write(filename_bytes)
    ser.write(len(data).to_bytes(2, 'big'))
    ser.write(data)

    #print(f"Sent image: {filename}")

# set which folder contains the desired animation state
def SendAnimationState(folder_name):
    folder_bytes = folder_name.encode('utf-8')
    ser.write(b'\x02')  # Control byte for animation state change
    ser.write(len(folder_bytes).to_bytes(1, 'big'))
    ser.write(folder_bytes)
    print(f"Requested folder monitoring: {folder_name}")

def SendSwipe(dir):
    payload = bytearray()
    ser.write(b'\x03') # Control byte for direction
    if 'L' in dir:
        payload.append(ord('L'))
    if 'R' in dir:
        payload.append(ord('R'))
    if not payload:
        raise ValueError(f"Cannot send direction {dir}")
    
    # send payload
    ser.write(bytes([len(payload)]))
    ser.write(payload)

    print(f"Sent command for {dir} with {len(payload)} payload byte(s).")
    
def DetectSwipe():
    ret = ''
    if line_ri.get_value() == 1:
        print("swipe right")
        ret = ret + 'R'
    elif line_ri.get_value() == 0:
        print("no swipe right")
    if line_le.get_value() == 1:
        print("swipe left")
        ret = ret + 'L'
    elif line_le.get_value() == 0:
        print("no swipe left")
    if ret:
        SendSwipe(ret)

def CheckForState(filename="state_queue.txt"):
    try:
        with open(filename, "r+") as f:
            fcntl.flock(f, fcntl.LOCK_EX)  # Lock file exclusively

            lines = f.readlines()
            if not lines:
                print("No words found.")
                fcntl.flock(f, fcntl.LOCK_UN)
                return None

            first_line = lines[0].strip()
            remaining_lines = lines[1:]

            f.seek(0)
            f.truncate()
            f.writelines(remaining_lines)

            fcntl.flock(f, fcntl.LOCK_UN)  # Unlock

            return first_line
    except FileNotFoundError:
        print(f"File {filename} not found.")
        return None


# program
OpenSerial()
# for i in range(0,100):
#     SendImage('/home/hwteam/slices/slice'+str(i)+'.png')
# SetFolder('slice')

# #send files
directory = '/home/hwteam/slices/'
for root, dirs, files in os.walk(directory):
    for file in files:
        full_path = os.path.join(root, file)
        SendImage(full_path)

# set animation
# SendAnimationState('Snoozing')
# time.sleep(10)
# SendAnimationState('Speaking')
# time.sleep(10)
# SendAnimationState('Listening')
# time.sleep(10)
# SendAnimationState('Processing')
# time.sleep(10)
# SendAnimationState('Idle')

# while True:
#     try:
#         CheckForState()
#         DetectSwipe()
#         time.sleep(0.25)
#     except KeyboardInterrupt:
#         pass
#     #finally:
#         CloseSerial()
#         print("Serial port closed")
#         sys.exit(1)

#SendAnimationState("shabyoingus")




#print("--- %s seconds ---" % (time.time() - start_time))