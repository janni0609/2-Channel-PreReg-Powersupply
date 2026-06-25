the firmware is for a lab bench powersupply projekt. 
the architecture of the powersupply is as follows

RP2350 (Waveshare RP2350-Tiny) is the main "brain".

the Powersuppy has two channes. each channes has its own MCU (Attiny 1614) that communicates with the ADC and DAC + periphials.

each attiny (i will call it channel) is connected to the RP2350 (i will call it brain) via UART.


**requirements for the Brain:**

it has to communicate with the channes.
    the brain should be able to send the channel a set volatage, set current, output state(on/off)

the Display has to be used (see ????? for a referance)

the brain is connected to a TCAL9539RPWR via i2c. 
    the TCAL9539RPWR has a rotary encoder connected and 6 buttons + 1 button on the encoder
    it also has a buzzer connected




**requirements for the channes:**

Attiny1614 at 3,3V
programmed at with platformIO.

the channel has to communicate with the brain. UART: (Tx -- PB2; Rx -- PB3)
Data to send to the Brain:
Output Volatge
Output Current
Output Power
Temperatur
Output State  (on/off)
Error messages

Data to recieved from Brain:
Set Volt
Set Current
Output State (on/off)
Data for calibration


the channel has to read the ADC (ADS1118IDGS) (SPI: MOSI -- PA1; MISO -- PA2; SCK -- PA3; ADC_CS -- PA4;)
Vmeas = AIN1 and AIN0
Imeas = AIN2 and AIN3
Do autoscaling of the input PGA



set the DAC (MCP48FVB22) (SPI: MOSI -- PA1; MISO -- PA2; SCK -- PA3; DAC_CS -- PA7; DAC_LAT -- PA6)
Vset = VOUT0
Iset = VOUT1
use internal reference
output 0 to 2,5V

The ADC and DAC has to be calibrated by a 2 point calibration.
for the calibration: a voltage/current will be set on the cannel and the actual measured volatge / current will be send by the brain. channel has to calculate cal values and store them in the eeprom

It has to be able to turn the output on and off with (Enable_DCDC -- PB1; Enable_lin -- PA5)

It has to read a NTC 10KOHM 3380K (PB0), NTC is set up with a R devider Rtop = 10k; Rbot = NTC



a self test has to be performed on startup:
test propper communication with chips and check if temp is in range (-10 to 60°C)

Output has to be turned off if temp reaches >60°C