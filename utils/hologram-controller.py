#!/usr/bin/python3

import random
import serial
import os
import re
import time
import subprocess
import signal
import zmq

ip = input("Enter Hologram IP ADDRESS (none for localhost) > ")

context = zmq.Context()
socket = context.socket(zmq.REQ)
if len(ip) < 2:
  ip = "tcp://localhost:5555"
else:
  ip = "tcp://" + ip + ":5555"

print(f"Connecting to {ip}")
socket.connect(ip)

last_cmd = ""

while True:
  cmd = input("> ")
  if len(cmd) == 0:
    socket.send_string(last_cmd)
  else:
    socket.send_string(cmd)
    last_cmd = cmd
  message = socket.recv()
  print(f"< {str(message)}")