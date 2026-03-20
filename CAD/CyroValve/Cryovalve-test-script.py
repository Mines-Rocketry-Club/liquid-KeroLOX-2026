from machine import Pin
from machine import Pin, PWM
import time

pulse = Pin(14,Pin.OUT)
direction = Pin(13,Pin.OUT)


forward = Pin(19, Pin.IN, Pin.PULL_UP)
reverse = Pin(20, Pin.IN, Pin.PULL_UP)

led = Pin(25, Pin.OUT)
print('test')

pwm0 = PWM(pulse, freq=4000, duty_u16=0)
while True:
    if forward.value() == 0:
        direction.on()
        pwm0.duty_u16(32768)
        print('huh')
       
    elif reverse.value() == 0:
        direction.off()
        pwm0.duty_u16(32768)    
        print('what')
       
    else:
        pwm0.duty_u16(0)
   
    led.toggle()
    time.sleep(0.05)