# ELRS Netpack

> [!IMPORTANT]
> This project **is not** officially affiliated or supported by the ExpressLRS
> organization. They do not have an obligation to provide help or support to you
> if you plan to utilize this project.

The ELRS Netpack is firmware for the 
[Waveshare ESP32-S3 Ethernet](https://www.waveshare.com/esp32-s3-eth.htm)
development board to support interfacing with ExpressLRS backpack
compatible devices. This device is designed to act as the equivalent
of the timer backpack, but instead of interfacing with the host
device over a serial connection, a tcp socket connection is used
instead.

Since this board uses W5500 ethernet chip, the newest versions of ESP-IDF 
are used directly. The W5500 is not currently supported by the Arduino ESP32 
versions included in PlatformIO.

> [!NOTE]
> Support for the W5500 was added in v5.0 of ESP-IDF, PlatformIO is limited to
> version v2.0.17 of Arduino ESP32, which is based on v4.4.7 of ESP-IDF. While
> newer versions of Arduino ESP32 based on the latest version of ESP-IDF
> exist, PlatformIO does not officially support them.