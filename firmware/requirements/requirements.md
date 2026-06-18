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


the channel has to communicate with the brain.
the channel has to read the ADC (ADS1118IDGS) and set the DAC (MCP48FVB22)