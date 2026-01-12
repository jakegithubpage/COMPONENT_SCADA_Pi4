import time
import RPi.GPIO as GPIO
import board
import json
import paho.mqtt.publish as publish
import paho.mqtt.client as mqtt
import digitalio
import serial
from adafruit_character_lcd.character_lcd import Character_LCD_Mono

#INSTALL MOSQUITTO BEFORE RUNTIME
#sudo mosquitto -c /etc/mosquitto/mosquitto.conf -v
#^start mosquitto CMD for broker on 1885

GPIO.setmode(GPIO.BCM)

BROKER = "x.x.x.x"
TOPIC_DHT = "sensors/dht/0/cmd"
TOPIC_KPS = "sensors/keypad/0/cmd"
TOPIC_FSS = "sensors/mosense/0/cmd"
TOPIC_RES = "sensors/rotary/0/cmd"
PORT = 1885


BTN_L = 22
BTN_C = 27
BTN_R = 17

LED_R1 = 12
LED_R = 23
LED_C = 24
LED_L = 25



buttons = [BTN_L, BTN_C, BTN_R]

leds = [LED_L, LED_C, LED_R, LED_R1]

mqtt_client = mqtt.Client(client_id="master-menu")

mqtt_client.connect(BROKER, PORT, keepalive=60)
mqtt_client.loop_start()


for b in buttons:
    GPIO.setup(b, GPIO.IN, pull_up_down=GPIO.PUD_UP)
    
for l in leds:
    GPIO.setup(l, GPIO.OUT)
    GPIO.output(l, GPIO.LOW)
    
menu_index = 0
menu_state = [False, False, False, False]
global menu_change
menu_change = False

#send menu status dht, keypad, and Motion Sens
def send_dht(state):
    payload = {"enable": bool(state)}
    mqtt_client.publish(
        TOPIC_DHT,
        json.dumps(payload),
        qos=1,
        retain=True
    )
    
def send_kps(state):
    payload = {"enable": bool(state)}
    mqtt_client.publish(
        TOPIC_KPS,
        json.dumps(payload),
        qos=1,
        retain=True
    )
    
def send_fss(state):
    payload = {"enable": bool(state)}
    mqtt_client.publish(
        TOPIC_FSS,
        json.dumps(payload),
        qos=1,
        retain=True
    )

def send_res(state):
    payload = {"enable": bool(state)}
    mqtt_client.publish(
        TOPIC_RES,
        json.dumps(payload),
        qos=1,
        retain=True
    )
    
#LED UPDATE
def update_leds():
    for i, l in enumerate(leds):
        GPIO.output(l, GPIO.LOW if menu_state[i] else GPIO.HIGH)
#press funcs        
def left_press(channel):
    global menu_index, menu_change
    menu_index = (menu_index - 1) % len(menu_state)
    menu_change = True
    update_leds()
    
def right_press(channel):
    global menu_index, menu_change
    menu_index = (menu_index + 1) % len(menu_state)
    menu_change = True
    update_leds()
    
def center_press(channel):
    global menu_index, menu_change
    menu_change = True
    menu_state[menu_index] = not menu_state[menu_index]
    update_leds()
        
#ADD INTERUPTS
GPIO.add_event_detect(BTN_L, GPIO.FALLING,
                      callback=left_press, bouncetime=10)

GPIO.add_event_detect(BTN_R, GPIO.FALLING,
                      callback=right_press, bouncetime=10)

GPIO.add_event_detect(BTN_C, GPIO.FALLING,
                      callback=center_press, bouncetime=10)


#LCD SIZE
lcd_col = 16
lcd_row = 2


lcd_rs = digitalio.DigitalInOut(board.D26)
lcd_en = digitalio.DigitalInOut(board.D19)
lcd_d4 = digitalio.DigitalInOut(board.D13)
lcd_d5 = digitalio.DigitalInOut(board.D6)
lcd_d6 = digitalio.DigitalInOut(board.D5)
lcd_d7 = digitalio.DigitalInOut(board.D11)

lcd = Character_LCD_Mono(
    lcd_rs, lcd_en,
    lcd_d4, lcd_d5, lcd_d6, lcd_d7,
    lcd_col, lcd_row
)

lcd.clear()


    


last_display = ""

try:

    while True:
        time.sleep(1)
        
       
        
        if menu_change:
            update_leds()
            if menu_index == 0:
                menu_change = False
                if menu_state[menu_index]:
                    lcd.clear()
                    lcd.message = "DHT11 ENABLED\nMENU:1"
                   
                elif not menu_state[menu_index]:
                    lcd.clear()
                    lcd.message = "DHT11 DISABLED\nMENU:1"
                    
                send_dht(menu_state[menu_index])
                    
            elif menu_index == 1:
                menu_change = False
                if menu_state[menu_index]:
                    lcd.clear()
                    lcd.message = "KEYPAD ENABLED\nMENU:2"
                    
                    
                elif not menu_state[menu_index]:
                    lcd.clear()
                    lcd.message = "KEYPAD DISABLED\nMENU:2"
                
                send_kps(menu_state[menu_index])
                    
            elif menu_index == 2:
                menu_change = False
                if menu_state[menu_index]:
                    lcd.clear()
                    lcd.message = "MOTION SENSOR\n ENABLED MENU:3"
                elif not menu_state[menu_index]:
                    lcd.clear()
                    lcd.message = "MOTION SENSOR\nDISABLED MENU:3"
                
                send_fss(menu_state[menu_index])
                
            
            elif menu_index == 3:
                menu_change = False
                if menu_state[menu_index]:
                    lcd.clear()
                    lcd.message = "ROTARY ENCODER\n ENABLED MENU:4"
                elif not menu_state[menu_index]:
                    lcd.clear()
                    lcd.message = "ROTARY ENCODER\nDISABLED MENU:4"
                
                send_res(menu_state[menu_index])
                
except KeyboardInterrupt:
    GPIO.cleanup()

        
       
    
    
