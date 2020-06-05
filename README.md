# Home Assistant Configs

This is my [Home Assistant](https://home-assistant.io) configuration. This also includes my [ESPHome](https://esphome.io) device configurations.

## Core Hardware

I run Home Assistant on a Raspberry Pi 4 (2GB model) with a USB SSD. Unfortunately, directly booting from USB isn't supported yet on the Pi4, so I also need an SD card for the boot partition. I also use the Pi PoE hat so I can power cycle the Pi remotely if necessary (via Unifi controller).

* [Raspberry Pi 4 Model B (2GB Ram)](https://www.adafruit.com/product/4292)
* [Sandisk Ultra A1 microSD](https://www.amazon.com/gp/product/B073JWXGNT)
* [StarTech.com SATA to USB Cable](https://www.amazon.com/gp/product/B00HJZJI84)
* [Kingston 120GB SATA SSD](https://www.amazon.com/gp/product/B01N6JQS8C)
* [Z-Wave.Me USB Stick](https://www.amazon.com/gp/product/B00QJEY6OC)
* [Raspberry Pi PoE Hat](https://www.adafruit.com/product/3953)

## Core Software

I run [Hass.io](https://www.home-assistant.io/hassio/) on [Raspbian Buster Lite](https://www.raspberrypi.org/downloads/raspbian/) using the [Generic Linux Docker install method](https://www.home-assistant.io/hassio/installation/#alternative-install-on-a-generic-linux-host). This gives me the convenience and simplicity of Hass.io but also gives me a full Linux environment I can hack around with.

## Integrations

Off-the-shelf stuff I've integrated with:
* Z-Wave (for wall switches, outlets, smoke alarms, and door sensors)
* Ecobee
* OctoPrint (3d-printer controller)
* Yeelight
* Apple HomeKit (for Siri and easy access from Apple devices)
* Amazon Alexa
* Unifi Controller
* Unifi Gateway (custom component)
* Network UPS Tools (NUT)
* TPLink outlet
* Route53 (for updating my DNS record)

Stuff I've built:
* [RTL433 MQTT](https://github.com/johnboiles/rtl-433-docker-pi) Temperature Sensors 
* [ESPHome IR Blaster](https://github.com/johnboiles/homeassistant-config/blob/master/esphome/ir_blaster.yaml) (controls my sound bar)
* [ESPHome Fan Controller](https://github.com/johnboiles/homeassistant-config/blob/master/esphome/basement_fan.yaml) (controls an exhaust fan in my basement)
* [ESPHome Air Sensor](https://github.com/johnboiles/homeassistant-config/blob/master/esphome/air_sensor.yaml) (reads CO2, temperature, humidity, particulates, and TVOCs in the air)
* [ESPHome Garage Controller](https://github.com/johnboiles/homeassistant-config/blob/master/esphome/garage_door.yaml) (opens/closes my garage and reports open/closed status)

## Automations

* Push notify when the 3d printer finishes (and attach a pic)
* Activate scenes when I double tap or triple tap my HomeSeer Z-Wave light switches
* Send an iOS critical notification if my garage is left open for 30 minutes
* Change an LED color on my HomeSeer WD200 light switch when the garage is open
* Set the default dim level for lights to dim at 9pm; set default level to full brightness at 7am
* Push notify when the power goes out (my Pi and network gear have a battery backup)
* Push notify when the smoke alarm is trigerred
* Push notify when a Home Assistant update is available
* Push notify when new firmware for one of my Unifi devices is available
* Push notify when my Unifi Controller has new alerts
* Recalibrate the CO2 sensor 4 hours after everyone leaves the house
* Switch the soundbar to AUX input when Alexa is playing music; switch the soundbar to OPTICAL (for the TV) when Alexa is not playing music
