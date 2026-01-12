#SETUP FOR MASTER?OUTSTATION SETUP
-> Create folder holding all files in TCPMonitor
-> Change IP ADDR's in each file to YOUR used Network IP
-> install Cmake, Clone opendnp3 (https://github.com/dnp3/opendnp3)
-> mkdir build in opendnp3 download location, cd build, then run "cmake .." -> then run "make"
-> update system apt and install
-> Next, Cd into Your created folder for the stations
-> mkdir build && cd build
-> run "cmake .."
-> run "make" 
-> in two seperate terminals launch ./master and ./gateway -> make sure to be in build
-> Now stations are built and tested to work -> check logExamples for proper outputs of each terminal
-> MOSQUITTO INSTALL
-> sudo apt-get update
-> sudo apt-get install mosquitto mosquitto-clients
-> edit config files of your mosquitto install with nano
-> set mosquitto.conf to contents in ExMosquitto.conf
-> Run mosquitto with command in masterLCD.py
-> once running and proven to be working
-> sudo killall mosquitto
-> Next, have used MCU's connected to power with proper code.
-> download start_scada.sh
-> Make executable and Run


