# Conceptual Truck-to-Truck Worm Repository Documentation

This repository contains the code for a conceptual worm that propagates between trucks via connected Electronic Logging Devices (ELDs). This code mimics real-world ELD configurations we found, specifically focusing on features such as simultaneous access point and station mode WiFi (APSTA mode), a weak default password (`deadbeef77`), a web server on the WiFi network for Over-The-Air (OTA) firmware updates, an additional API endpoint for firmware uploads, and the capability to interact with the truck's Controller Area Network (CAN).

## Detailed Demonstration Setup Instructions

Follow these steps to set up and demonstrate the worm:

1. **PlatformIO Installation**: This project requires PlatformIO. Install either the [PlatformIO IDE](https://platformio.org/platformio-ide) or its [VSCode extension](https://platformio.org/install/ide?install=vscode).

2. **Hardware Requirements**: The project is designed for DoIT ESP32 Rev. 1.1 boards or similar variants. If you have a different version of ESP32 boards, check their compatibility with PlatformIO and update the `platformio.ini` file accordingly. There's no guarantee of functionality with other versions. At least two boards are required to demonstrate the worm's behavior.

3. **Downloading the Repository**: Clone or download this repository to your local machine.

4. **Opening the Project**: Launch PlatformIO and open the project. Ensure the `platformio.ini` file is in the root directory for automatic environment setup.

5. **Environment Setup**: PlatformIO will detect the `platformio.ini` file and start setting up the development environment, which includes downloading the ESP-IDF. This process might take some time.

6. **Restarting PlatformIO**: After the setup is complete, you might need to restart PlatformIO.

7. **Configuration Changes**: In the `src/main.c` file, find the `MALICIOUS_ELD` preprocessor directive and set it to false.

8. **Firmware Flashing**: Connect an ESP32 board to your computer. Use the upload function in PlatformIO to compile and flash the firmware to the board. Refer to [PlatformIO documentation](https://docs.platformio.org/en/latest/integration/ide/vscode.html#ide-vscode) for further instructions.

9. **Repeat Flashing Process**: Flash each ESP32 board individually, following the same process as in step 8.

10. **Powering the Boards**: After flashing, disconnect the ESP32 boards from your computer and connect them to a 5V power source, like a USB power brick.

11. **Activating Malicious Mode**: Go back to `src/main.c`, change the `MALICIOUS_ELD` directive to true, and recompile the firmware.

12. **Locating the Malicious Firmware**: After compilation, the malicious firmware will be available at `.pio/build/esp32dotit-devkit-v1/firmware.bin`.

13. **Connecting to a Vulnerable Device**: Connect your computer to the WiFi network of a vulnerable device, typically named `VULN ELD: <MAC_ADDR>`.

14. **Accessing the Firmware Upload Page**: Go to 192.168.4.1 in your web browser to find the firmware upload interface.

15. **Uploading the Malicious Firmware**: Upload the malicious firmware file. The device may not provide feedback upon a successful upload but may respond if an error occurs. An alternative method to confirm the upload's success involves connecting the ESP32 board to a computer and check the serial output.

16. **Observing the Malicious Firmware in Action**: After a successful OTA update, the device reboots with the new firmware. The WiFi SSID should change to `COMP ELD: <MAC_ADDR>`, indicating the device is compromised. The device will then start scanning for and infecting other vulnerable devices within range.

17. **Monitoring the Worm's Spread**: Track the worm's propagation by observing changes in the WiFi SSIDs or by connecting to and monitoring the CAN bus. The infected devices will emit a malicious Petal Jam TSC1 message.