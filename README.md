# ESP8266-WS2811

## Requirements

* Linux (I've not tried other environments)
* The [esp-open-sdk](https://github.com/pfalcon/esp-open-sdk)
* An environment variable "ESP_ROOT" pointing to the root of the above sdk
* An addition to the `PATH` environment variable, pointing into `$ESP_ROOT/xtensa-lx106-elf/bin/`

## Setup

* Copy the user_config.h.template header to user_config.h (Within the `include/` directory) and enter your SSID and password.
* Get your hardware setup - the signal will be sent to the uart RX pin - see the KiCad project and schematic in `pcb/` for more information.
* Connect your ESP and find out which port it is connected to.
* Get your ESP ready for flashing over UART
* Comile and flash by running `make flash PORT=<esp>`, where `<esp>` would usually be something like `/dev/ttyUSB0`

## The protocol

You can then control the led strip by sending UDP packets with the following format:

    [Packet] 	:= [LED] * ledCount
    [LED]		:= [r: byte][g: byte][b: byte]

##### Example:

Set the first driver to magenta:

```java
byte[] packet = new byte[]{0xFF, 0x00, 0xFF};
```

## Timing

More timing information can be found in the [Timing](Timing) file

![High](High.bmp)

©2017 Yamakaja
