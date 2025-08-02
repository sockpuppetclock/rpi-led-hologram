#!/usr/bin/python3

import random
import serial
import os
import re
import time
import subprocess
import signal
import zmq
import serial
import time
import os
import gpiod
import fcntl
import sys
#start_time = time.time()
pin_ri = 6
pin_le = 13
chip = gpiod.Chip('gpiochip4')

line_ri = chip.get_line(pin_ri)
line_le = chip.get_line(pin_le)
line_ri.request(consumer="input", type=gpiod.LINE_REQ_DIR_IN)
line_le.request(consumer="input", type=gpiod.LINE_REQ_DIR_IN)

context = zmq.Context()
socket = context.socket(zmq.REQ)

ip = ""
# with open("ip.txt", "r+") as f:
#    lines = f.readlines()
#    ip = lines[0].strip()
if len(sys.argv) > 1:
  ip = sys.argv[1]
if len(ip) < 2:
  ip = "tcp://localhost:5555"
else:
  ip = "tcp://" + ip + ":5555"
print(f"Connecting to {ip}")
socket.connect(ip)

last_cmd = ""
last_state = ""
last_mtime = 0.0
state = ""
period = 33

def DetectSwipe():
  # print(f"{line_le.get_value()}{line_ri.get_value()}")
  # ret = ''
  if line_ri.get_value() == 1:
    print("swipe right")
    socket.send_string(".r")
    print(socket.recv())
    # ret = ret + 'R'
  if line_le.get_value() == 1:
    print("swipe left")
    socket.send_string(".l")
    print(socket.recv())
    # ret = ret + 'L'
  # if ret:
      # SendSwipe(ret)

def CheckForState(filename="./state_queue.txt"):
  global last_mtime
  try:
    current_mtime = os.path.getmtime(filename)
    if current_mtime != last_mtime:
      print(f'{filename} was updated at {current_mtime}')
      last_mtime = current_mtime
      with open(filename, "r+") as f:
        # fcntl.flock(f, fcntl.LOCK_EX)  # Lock file exclusively

        lines = f.readlines()
        if not lines:
            print("No words found.")
        #     fcntl.flock(f, fcntl.LOCK_UN)
            return None

        first_line = lines[0].strip()
        # remaining_lines = lines[1:]

        # f.seek(0)
        # f.truncate()
        # f.writelines(remaining_lines)

        # fcntl.flock(f, fcntl.LOCK_UN)  # Unlock

        return first_line
  except FileNotFoundError:
    print(f"File {filename} not found.")
    return None

while True:
  # cmd = input("> ")
  # if len(cmd) == 0:
  #   socket.send_string(last_cmd)
  # else:
  #   socket.send_string(cmd)
  #   last_cmd = cmd
  try:
    DetectSwipe()
    period = period - 1
    if(period <= 0):
      period = 33
      state = CheckForState()
      if state != last_state and state != None:
        print(f"sending {state}")
        socket.send_string(str(state))
        print(socket.recv())
        last_state = state
    time.sleep(0.01)
  except KeyboardInterrupt:
     exit(0)
  # print(f"< {message}")