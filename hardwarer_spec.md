# Hardware chips used

NRF52833-qdaa-r7

## i2c connects to
the i2c lines are on P0.04 for SCL and P0.01 for SDA
BMI270 @ 0x68
FRTC8900 @ 0x32
BH1750 @ 0x23
BQ27441 @ 0x55

## there are interrupts at
P0.00 for the FRTC8900 interrupt
P0.11 for BQ27441 Battery monitor BIN pin
P0.30 for IMU int 1
P0.31 for IMU int 2

# led data lines WS2812B
There are 4 data lines going to the leds on pins 
P0.29 for led data 1
P0.28 for led data 2
P0.02 for led data 3
P0.03 for led data 4

# Other Gpio used is
P0.05 for chrg indicator from an SGM41524
P0.15 button right
P0.17 button left
P1.09 battery monitor gpout