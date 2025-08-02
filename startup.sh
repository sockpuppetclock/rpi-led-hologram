#!/bin/bash

echo -e "    Welcome!\n \n    Display \n   starting... \n \n       IP " > ./welcome.txt
echo " $(ifconfig wlan0 | awk '/inet / {gsub("", $2); print $2}')" >> ./welcome.txt
cat "./welcome.txt" | ./examples-api-use/text-example --led-rows=64 --led-cols=64 --led-limit-refresh=1500 --led-pixel-mapper="Rotate:270" -C 255,255,255 -f /home/dietpi/u8g2/tools/font/bdf/4x6.bdf -y 9 &
pp="$!"
sleep 25
kill -SIGKILL "$pp"
./utils/text-scroller --led-rows=64 --led-cols=64 --led-limit-refresh=1500 -C0,255,0 "Starting..." --led-pixel-mapper="Rotate:270" -s 3.2 -f /home/dietpi/u8g2/tools/font/bdf/10x20.bdf -y 23 &
pp="$!"
sleep 6
kill -SIGKILL "$pp"

./utils/hologram-viewer --led-rows=64 --led-cols=64 --led-pwm-dither-bits=2 --led-slowdown-gpio=2 --led-pwm-bits=3 --led-pwm-lsb-nanoseconds=50 --led-pixel-mapper="Rotate:270" --led-limit-refresh=1500