# wrgii2mqtt Project

The wrgii2mqtt is a system which consists of a esp32 and a max485 modbus breakout board. It allows read and write data to a Meltem WRGII via MQTT from my Homeassistant system

## Main Function

I want to read actual data via modbus from my metltem wrgII and publish it via WLAN and MQTT to my Homeassistant system.

Because we use a USB for the project, the ESP32 should use OTA flashing and logging. 

## Implementation Phases

1. Implement all ESP32 functions except the Modbus part. In this phase, we want to debug the infrastructure, such as OTA flashing and debugging.
2. Implement the Modbus communication to the mwrgII. as a test read the reading registers 
3. Implement the modbus writing registers

## Tools

Use ESP-IDF tools for the project.

Create a new GitHub Repository on https://github.com/1984ozone1984/meltem_wrgII_2_MQTT