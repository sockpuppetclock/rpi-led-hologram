#!/bin/bash

# cd "$(dirname "$0")" || exit 1
echo -e "    Welcome!\n \n    Display \n   starting... \n \n       IP " > /rpi-led-hologram/welcome.txt
echo " $(ifconfig wlan0 | awk '/inet / {gsub("", $2); print $2}')" >> /rpi-led-hologram/welcome.txt
cat "/rpi-led-hologram/welcome.txt" | /rpi-led-hologram/examples-api-use/text-example --led-rows=64 --led-cols=64 --led-limit-refresh=1500 --led-pixel-mapper="Rotate:270" -C 255,255,255 -f /home/dietpi/4x6.bdf -y 9 &
pp="$!"
if [[ "$1" != "q" ]];then
  sleep 25
else
  sleep 0
fi
kill -SIGKILL "$pp"
/rpi-led-hologram/utils/text-scroller --led-rows=64 --led-cols=64 --led-limit-refresh=1500 -C0,255,0 "Starting..." --led-pixel-mapper="Rotate:270" -s 3.2 -f /home/dietpi/10x20.bdf -y 23 &
pp="$!"
if [[ "$1" != "q" ]];then
  sleep 6
else
  sleep 0
fi
kill -SIGKILL "$pp"

/rpi-led-hologram/utils/hologram-viewer --led-rows=64 --led-cols=64 --led-pwm-dither-bits=2 --led-slowdown-gpio=2 --led-pwm-bits=3 --led-pwm-lsb-nanoseconds=50 --led-pixel-mapper="Rotate:270" --led-limit-refresh=1500 -r 6
