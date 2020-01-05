# ESPHome Configs

Home for my ESPHome config files that power various pieces of DIY hardware around my house.

## Air Sensor

Air-quality sensor that uses

* MHZ19 CO2 sensor
* DHT22 temperature/humidity sensor
* PMS5003 particulate sensor
* CCS811 eCO2 and TVOC sensor

This project also exposes a service to run the calibration on the MHZ19, so I can automate calibration to happen after the house has been unoccupied for a period.

## Garage Door

Simple garage door controller that uses a relay and a magnetic reed switch.

Hardware:
* [NodeMCU 1.0](https://www.amazon.com/HiLetgo-Version-NodeMCU-Internet-Development/dp/B010O1G1ES) ESP8266 Wifi microcontroller (though I'd probably use a [Wemos D1 Mini](https://www.amazon.com/Makerfocus-NodeMcu-Development-ESP8266-ESP-12F/dp/B01N3P763C) if I started over.)
* [5V relay](https://www.amazon.com/Tolako-Arduino-Indicator-Channel-Official/dp/B00VRUAHLE) to trigger the door
* [Magnetic reed switch](https://www.amazon.com/uxcell-Window-Sensor-Magnetic-Recessed/dp/B00HR8CT8E) to detect whether the door is open or closed.

The electronics are very simple. The relay is switched by a digital output from the microcontroller (the relay board linked above has a transistor to supply the necessary current to the relay). The reed switch is connected to ground and one of the digital inputs (because the ESP8266 has internal pullups, no extra resistor is necessary). Just connect to the normally disconnected side of your relay to the same two wires running to your existing garage door button.

![Fritzing diagram](https://raw.githubusercontent.com/johnboiles/esp-garage-opener/assets/images/fritzing.png)

## IR Blaster

Simple IR blaster for sending IR remote codes to my TV and soundbar. Electronics consist of two IR LEDs and a PN2222 transistor to switch the LEDs (since the ESP8266 can't source enough current to drive those LEDs).

# Installing / Running

ESPHome makes it very simple to install the software for this project.

## First time installation

Install the `esphome` tool

    sudo pip install esphome

Make a `secrets.yaml` with your WiFi credentials

    cp secrets.yaml.example secrets.yaml

## To upload the firmware

Replace `air_sensor.yaml` with the desired config and compile and upload the generated firmware.

    esphome air_sensor.yaml run

## To read the logs while running

    esphome air_sensor.yaml logs
