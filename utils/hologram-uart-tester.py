#!/usr/bin/python3

import random
import serial
import os
import re
import time
import subprocess
import signal
import zmq

#-# the uart controller doesnt use zeromq to receive

# context = zmq.Context()
# socket = context.socket(zmq.REQ)

# # serial controls #

# def main():
#     socket.connect("tcp://localhost:5555")
#     t = time.time()
#     test = time.time()
#     try:
#         # main loop #
#         while True:
#             try:
#                 print("Input : ")
#                 c = input()
#                 if(len(c) < 1):
#                     continue 
#                 c = bytes([int(c)])
#             except ValueError:
#                 print("Error: Received string is not a valid integer.")
#                 continue
#             if( c == b'\x02'):
#                 print("Anim name : ")
#                 anim = input()
#                 print(len(anim))
#                 c = c + bytes(len(anim)) + bytes(anim, 'utf-8')

#             print(c)
#             try:
#                 socket.send(c)
#             except TypeError:
#                 print("type error?")
#                 continue
#             msg = socket.recv()
#             print("Received:",msg)
#     except KeyboardInterrupt:
#         return
# main()