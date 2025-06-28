#!/bin/bash

echo -e "    Welcome!\n \n    Display \n   starting... \n \n       IP " > /hologram/welcome.txt
echo " $(ifconfig wlan0 | awk '/inet / {gsub("", $2); print $2}')" >> /hologram/welcome.txt
cat "/hologram/welcome.txt" | /hologram/examples-api-use/text-example --led-rows=64 --led-cols=64 --led-limit-refresh=1500 --led-pixel-mapper="Rotate:270" -C 255,255,255 -f /home/dietpi/u8g2/tools/font/bdf/4x6.bdf -y 9 &
pp="$!"
sleep 25
kill -SIGKILL "$pp"
/hologram/utils/text-scroller --led-rows=64 --led-cols=64 --led-limit-refresh=1500 -C0,255,0 "Starting..." --led-pixel-mapper="Rotate:270" -s 3.1 -f /home/dietpi/u8g2/tools/font/bdf/10x20.bdf -y 23 &
pp="$!"
sleep 5
kill -SIGKILL "$pp"

/hologram/utils/hologram-uart.py &
upp="$!"
/hologram/utils/hologram-viewer -f --led-rows=64 --led-cols=64 --led-pwm-dither-bits=2 --led-slowdown-gpio=2 --led-pwm-bits=3 --led-pwm-lsb-nanoseconds=50 --led-pixel-mapper="Rotate:270" --led-limit-refresh=1500
kill -SIGKILL "$upp"