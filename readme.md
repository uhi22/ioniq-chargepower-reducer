# Ioniq Chargepower Reducer

## Motivation

The Hyundai Ioniq EV (28kWh) charges its battery with up to 67kW / 175A via CCS. This is around 2.5C, and leads to increased battery temperature up to 50°C at the second charging stop. Not good for a long car life. So the idea is: reduce the charging current in cases where the charging time does not matter.

## News / Current State

- Transmit and receive works on both CANs. Routing with modification example works.
- Message overview from both sides is printed once a second to serial.

## How to Build

**Prerequisites** — one-time setup:

- [arm-none-eabi toolchain](https://developer.arm.com/Tools%20and%20Software/GNU%20Toolchain) and GNU make in PATH.
  On Windows the bundled tools from STM32CubeIDE work; run from Git Bash:
  ```sh
  export PATH="/c/ST/STM32CubeIDE_1.12.1/STM32CubeIDE/plugins/com.st.stm32cube.ide.mcu.externaltools.make.win32_2.0.200.202301161003/tools/bin:\
/c/ST/STM32CubeIDE_1.12.1/STM32CubeIDE/plugins/com.st.stm32cube.ide.mcu.externaltools.gnu-tools-for-stm32.10.3-2021.10.win32_1.0.200.202301161003/tools/bin:\
$PATH"
  ```
- [OpenOCD](https://openocd.org/) for flashing (ST-Link v2 or FT232H as SWD adapter).

**First clone:**
```sh
git submodule update --init
mingw32-make -C libopencm3    # build the library once
```

**Build firmware:**
```sh
mingw32-make
```

**Flash** (FT232H SWD connected to SWCLK=PA14/pin49, SWDIO=PA13/pin46):

```sh
/c/LegacyApp/openocd/OpenOCD-20250710-0.12.0/bin/openocd -f openocd_ft232h.cfg -c "program chargepower-reducer.elf verify reset exit"
```
> **Note 1:** The GD32 disables the SWD when software is running, for unknown reason. Workaround: pull NRES to ground and hold it for the first flashing. Try a second time, now programming and verification worked.
> **Note 2:** Using STLink-V2 together with openocd was *not* successful.
> **Note 3:** Using STLink-V2 with ST-LINK_CLI was only working sporadically. 
> **Note 4:** If the GD32 chip is read-protected, use STM32CubeProgrammer to unlock it first (see Ref2). This was not required on the board which was tested in this project.

## Approach

The ChargeControlModule (CCM) which lives under the driver seat is the gateway between the propulsion CAN (PCAN) and the powerline communication (PLC) of the CCS port. We can add a man-in-the-middle (MITM) on PCAN, which fakes the max current or max power which flows from the battery management system (BMS) to the CCM.

## Details

On Aliexpress, there are cheap dual CAN boards exactly for such purpose, something like "MB 18-in-1 universal emulator" or "CAN instrument filter". We use the "black" variant.

- Controller GD32F105RBT6, 64pin LQFP
- CAN transceiver SIT1040
- XTAL: 25Mhz
- Voltage regulator: 1117
- Secondary regulator U3, "ODN"

## Feature Plan

### Phase 1: Build environment and basic setup

- [x] Find an existing repo (openinverter.org?)
    - https://openinverter.org/forum/viewtopic.php?p=75803#p75803
    - https://github.com/jamiejones85/stm32-CANBridge
    - schematic of the "green" one: https://openinverter.org/forum/viewtopic.php?p=75807#p75807
- [x] setup the build chain
- [x] Identify the SWD pins and connect programmer
    - SWCLK pin 49, PA14
    - SWDIO pin 46, PA13
    - Boot0 pin has a pull-down (R3, 4k7) on the black board, so it will start from user flash.
    - The original software blocks the SWD as soon as it runs. Workaround: Hold the NRES low for first flash, this fails, but after releasing the NRES the flashing works.
- [x] flash and run

### Phase 2: Analysis

- [x] Step 201: Configure the CAN1 for 500kBaud
    - CAN1 is at pins 44+45 (PA11+PA12)
    - CAN2 is at pins 57+58 (PB5+PB6)
- [x] Step 202: Identify an UART pin for logging, and configure it to 115kBaud
    - (USART1_TX PA9 (pin 42) used on "green" board)
    - USART3_TX PB10 (pin 29) is on solder pad P2.3 on "black" board
    - (USART3_RX PB11 (pin 30) is on solder pad P2.4 on "black" board)
- [x] Step 203: Catch all messages from CAN, and print them to the serial. One snapshot per second is sufficient.
- [x] Step 204: Run a charging session and record the data, together with the user-provided charge power and voltage. Find out, which CAN signal provides the power limit or current limit from BMS to CCM.

### Phase 3: MITM

- [x] Step 301: Bring up the CAN2 with 500kBaud
- [x] Step 302: Route all messages from CAN1 to CAN2 and vice versa
- [ ] Step 303: Cut the CAN lines on the CCM and insert the MITM module
- [ ] Step 304: Check whether charging still works
- [ ] Step 305: Patch the forward path to limit the current
- [ ] Step 306: Check whether limited current is effective

### Phase 4: Further ideas

- [ ] Use a poti on analog input to adjust the limitation
- [ ] Brake pedal and accelerator pedal to adjust the limitation
- [ ] Check the voltage regulator properties whether they are vehicle compatible long-term
- [ ] ... maybe more ...


## References

- Ref1: Pinout of the blue board https://openinverter.org/forum/viewtopic.php?p=82635#p82635
- Ref2: how to unlock the flash https://openinverter.org/forum/viewtopic.php?p=83864#p83864
- Ref3: reverse engineering of the "green" and "blue" CAN bus filter https://dangerouspayload.com/2020/03/10/hacking-a-mileage-manipulator-can-bus-filter-device/
- Ref4: firmware binaries of green and blue boards https://github.com/andrewkabai/universal_can_filter
- Ref5: datasheet STM32F105 https://www.google.com/url?sa=t&rct=j&q=&esrc=s&source=web&cd=&ved=2ahUKEwiNwpzzitOXAxWtSfEDHeEvJPEQFnoECCcQAQ&url=https%3A%2F%2Fwww.st.com%2Fresource%2Fen%2Fdatasheet%2Fstm32f105r8.pdf&usg=AOvVaw0xi2c2MLBn8kCwDRR__5XS&opi=89978449
