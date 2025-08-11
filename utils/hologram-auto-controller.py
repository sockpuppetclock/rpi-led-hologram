#!/usr/bin/python3

# Sends zeromq strings to LED driver zmq server
# Reads touch sensor left/right GPIO
# IP address of LED driver is read from ./ip.txt and can update while running
# Sets animation state read from ./state_queue.txt while running
# For manual control use ./hologram-controller.py
# For zeromq commands see ./hologram-viewer.cc

import os
import time
import zmq
import time
import os
import gpiod
import sys

STATE_FILE = "/home/hwteam/SD25App/animation_state.txt"

# GPIO PIN ASSIGNMENTS
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
with open("ip.txt", "r+") as f:
   lines = f.readlines()
   ip = lines[0].strip()
# if len(sys.argv) > 1:
#   ip = sys.argv[1]
if len(ip) < 2:
  ip = "tcp://localhost:5555"
else:
  ip = "tcp://" + ip + ":5555"
print(f"Connecting to {ip}")
socket.connect(ip)

last_cmd = ""
last_state = ""
last_mtime = 0.0
last_mtime_ip = 0
state = ""
# time_now = 0
# time_then = 0
period = 33

last_swipe_ri = 0
last_swipe_le = 0
last_swipe_act = -1

def DetectSwipe():
  global last_swipe_ri, last_swipe_le, last_swipe_act
  # print(f"{line_le.get_value()}{line_ri.get_value()}")
  # ret = ''
  r = line_ri.get_value()
  l = line_le.get_value()
  if r == 1 and last_swipe_ri != 1:
    if last_swipe_act == 2:
      print("swipe right")
      socket.send_string(".r")
      print(socket.recv())
    else:
      last_swipe_act = 2
    # ret = ret + 'R'
  if l == 1 and last_swipe_le != 1:
    if last_swipe_act == 1:
      print("swipe left")
      socket.send_string(".l")
      print(socket.recv())
    else:
      last_swipe_act = 1
    # ret = ret + 'L'
  # if ret:
      # SendSwipe(ret)
  last_swipe_ri = r
  last_swipe_le = l

def CheckForState(filename=STATE_FILE,filename_ip="./ip.txt"):
  global last_mtime
  global last_mtime_ip
  global socket
  try:
    current_mtime = os.path.getmtime(filename_ip)
    if current_mtime != last_mtime_ip:
      print(f'{filename_ip} was updated at {current_mtime}')
      last_mtime_ip = current_mtime
      with open("ip.txt", "r+") as f:
        lines = f.readlines()
        ip = lines[0].strip()
        if len(ip) < 2 and ip != None:
          ip = "tcp://localhost:5555"
        else:
          ip = "tcp://" + ip + ":5555"
        print(f"Connecting to {ip}")
        socket.connect(ip)
  except FileNotFoundError:
    print(f"File {filename_ip} not found.")
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
    # check swipe
    DetectSwipe()
    # check state
    # time_now = time.time_ns()
    period = period - 1
    # if(time_now - time_then > 1e8 ):
    if period == 0:
      period = 33
      # time_then = time_now
      state = CheckForState()
      if state != last_state and state != None:
        print(f"sending {state}")
        socket.send_string(str(state))
        print(socket.recv())
        last_state = state
    
    # 100Hz
    time.sleep(0.01)
  except KeyboardInterrupt:
     exit(0)
  # print(f"< {message}")