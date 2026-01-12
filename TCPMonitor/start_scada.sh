#!/bin/bash

set -e #stop if anything fails
echo "Starting SCADA stack"

#Paths
PROJECT_DIR="$HOME/TCPMonitor"
BUILD_DIR="$PROJECT_DIR/build"


# kill old processes first 
echo "[*] Killing old mosquitto (PORT 1885)"
pkill -f mosquitto || true
pkill -f gateway || true
pkill -f master || true

sleep 1


#start mosquitto
echo "[*] Starting Mosquitto on 1885"
mosquitto -c /etc/mosquitto/mosquitto.conf -v > ~/mosquitto.log 2>&1 &

sleep 2



#Build Gateway and Master
echo "[*] Building gateway & master"

cd "$PROJECT_DIR"

#Run cmake 

echo "[*] Running cmake"
cd ~/TCPMonitor
rm -rf build
mkdir build
cd build
cmake ..
echo "[*] Running make"
make -j$(nproc)


sleep 1

#Launch Pi-Menu
echo "[*] Launching Pi-menu"
python3 ~/masterLCD.py > ~/lcd.log 2>&1 &

sleep 1 
rm -rf logs
mkdir -p ~/logs

#Launch gateway 
echo "[*] Starting gateway" 
gnome-terminal --title="GATEWAY" -- bash -c "
cd ~/TCPMonitor/build || exit
./gateway
exec bash
"

sleep 1

#Launch master
echo "[*] Starting master"
gnome-terminal --title="MASTER" -- bash -c "
cd ~/TCPMonitor/build || exit
./master
exec bash
"



echo "SYSTEM ONLINE"
echo "LOGS:"
echo "	mosquitto.log"
echo "	gateway.log"
echo "	master.log"
echo "	lcd.log"

wait
