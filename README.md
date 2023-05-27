# BMS for Tesla module using an ESP32-S3
This software allows you to use the included BMS on Tesla battery modules from Model S and X (2012-2020) using a cheap ESP32 arduino (T-Display-S3).

It currently shows a few metrics on the attached display, like voltage, state of charge and balance status.

Eventually, it will be able to connect to wifi and upload the BMS values to a server.

NOTE: The original work was done by [collin80](https://github.com/collin80/TeslaBMS) and I just ported his work over to the ESP32S3 and added the display items.

# Hardware
You can obtain the ESP32 board at Amazon and Aliexpress. There are various links on [this page](https://github.com/Xinyuan-LilyGO/T-Display-S3).

For connecting to the BMS on the Tesla modules, you will need a bunch of MOLEX connectors like [these ones](https://www.mouser.com/ProductDetail/Molex/15-97-5101) and the [connector pins](https://www.mouser.com/ProductDetail/Molex/39-00-0038-Cut-Strip).

# Software installation
- Install the latest version of the [Arduino IDE](https://www.arduino.cc/en/software)
- Follow the Quick Start Arduino installation instructions on [this page](https://github.com/Xinyuan-LilyGO/T-Display-S3)
- Open the `tesla-bms-esp32s3.ino` file project in the Arduino IDE
- Open `bms-config.h` and update the `BMS_NUM_SERIES` and `BMS_NUM_PARALLEL` values to suit your module configuration.
- Upload to the board.

# Cabling

Here is a [diagram of the layout](images/tesla_battery_module_layout.png). Note that the MOLEX pin numbers are from the top of the connector.

## Pinout

|Molex   |ESP32S3
|--------|--------
|RX (2)  |GPIO01
|TX (4)  |GPIO02
|GND (3) |G
|5v (5)  |+5V

You can find the pinout of the ESP32S3 board [here](images/esp32-pinout.jpg).

# TODO

- Finish wifi and metrics upload support

