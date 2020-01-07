# ESPHome Configs

Home for my ESPHome config files that power various pieces of DIY hardware around my house.

## Air Sensor

Air-quality sensor to monitor temperature, humidity, particulate count, CO2, and TVOCs with an ESP32 and some cheap-ish sensors.

* ESP32
* [PMS5003 High Precision Laser Dust Sensor](https://www.ebay.com/itm/PMS5003-High-Precision-Laser-Dust-Sensor-Module-PM1-0-PM2-5-PM10-Built-in-Fan-N-/263421941788?hash=item3d552bd81c)
* [MHZ-19 Intelligent Infrared CO2 Module](http://www.winsen-sensor.com/products/ndir-co2-sensor/mh-z19.html)
* [DHT22 Temperature & Humidity Sensor](https://www.adafruit.com/product/385)
* CCS811 eCO2 and TVOC sensor

This project also exposes a service to run the calibration on the MHZ19, so I can automate calibration to happen after the house has been unoccupied for a period.

## Garage Door

Garage door controller that uses a relay and a magnetic reed switch.

Hardware:
* [NodeMCU 1.0](https://www.amazon.com/HiLetgo-Version-NodeMCU-Internet-Development/dp/B010O1G1ES) ESP8266 Wifi microcontroller (though I'd probably use a [Wemos D1 Mini](https://www.amazon.com/Makerfocus-NodeMcu-Development-ESP8266-ESP-12F/dp/B01N3P763C) if I started over.)
* [5V relay](https://www.amazon.com/Tolako-Arduino-Indicator-Channel-Official/dp/B00VRUAHLE) to trigger the door
* [Magnetic reed switch](https://www.amazon.com/uxcell-Window-Sensor-Magnetic-Recessed/dp/B00HR8CT8E) to detect whether the door is open or closed.

The electronics are very simple. The relay is switched by a digital output from the microcontroller (the relay board linked above has a transistor to supply the necessary current to the relay). The reed switch is connected to ground and one of the digital inputs (because the ESP8266 has internal pullups, no extra resistor is necessary). Just connect to the normally disconnected side of your relay to the same two wires running to your existing garage door button.

![Fritzing diagram](https://raw.githubusercontent.com/johnboiles/esp-garage-opener/assets/images/fritzing.png)

## IR Blaster

IR blaster for sending IR remote codes to my TV and soundbar. Electronics consist of two IR LEDs and a PN2222 transistor to switch the LEDs (since the ESP8266 can't source enough current to drive those LEDs).

Hardware:
* [Wemos D1 Mini](https://www.amazon.com/gp/product/B01N3P763C)
* IR LED (from my parts box)
* PN2222 transistor (from my parts box)

## Basement Fan

Small DC exhaust blower fan to exhaust fumes from my basement. I use this to extract fumes from my solder station and other burn-y tools.

Hardware:
* [Wemos D1 Mini](https://www.amazon.com/gp/product/B01N3P763C)
* [Attwood Quiet Blower](https://www.amazon.com/Attwood-1749-4-Blower-Resistant-4-Inch/dp/B003EX02DA)
* [MOSFET board](https://www.amazon.com/gp/product/B01J78FX9S)
* [4 inch aluminum duct](https://www.amazon.com/gp/product/B01N6DV33G)
* [12V Power Supply](https://www.amazon.com/gp/product/B00MBBOWAU)

# Installing / Running

ESPHome makes it very simple to install the software for this project.

## First time installation

Install the `esphome` tool

    sudo pip install esphome

Make a `secrets.yaml` with your WiFi credentials

    cp secrets.yaml.example secrets.yaml

If you don't have it already, you probably need a driver for the CH340 USB->Serial chip. [This janky looking website](http://www.wch.cn/download/CH341SER_MAC_ZIP.html) is the chip manufacturer's official download page.

## To upload the firmware

Replace `air_sensor.yaml` with the desired config and compile and upload the generated firmware. The first time you do this you'll need to connect over USB, but subsequent uploads can happen over wifi.

    esphome air_sensor.yaml run

## To read the logs while running

    esphome air_sensor.yaml logs
