# Ioniq Chargepower Reducer

## Motivation

The Hyundai Ioniq EV (28kWh) charges its battery with up to 67kW / 175A via CCS. This is around 2.5C, and leads to increased battery temperature up to 50°C at the second charging stop. Not good for a long car life. So the idea is: reduce the charging current in cases where the charging time does not matter.

## Current State

**Understood** (proven on the bench with a real CCM, pyPLC as charger and a vehicle simulation, see [doc/bench-testing.md](doc/bench-testing.md) and [doc/analysislog.md](doc/analysislog.md)):
- The target current of the CCM is **0x59C B4..B5** (LE16, 0.1 A). The CCM forwards it 1:1 as EVTargetCurrent to the charger, without own ramp, no checksum.
- A complete charging session needs only 5 vehicle messages (0x57F, 0x597, 0x599, 0x5A3, 0x59C). For a real charger also 0x59E (remaining times, maximum power).
- The car accepts a lower current than requested.

**Implemented** (firmware runs on the board):
- Routing CAN1 ⇄ CAN2, with diagnostics on the serial line (only the messages of interest: 0x59C, 0x599, 0x200, 0x394, 0x5E5, 0x5E9), watchdog. At full bus load (~2 210 frames/s, replay of a car trace) no frame is lost: if no TX mailbox is free, the frame waits in the RX ring.
- Limitation of 0x59C B4..B5 to the current limit.
- Pedal control of the limit ([doc/pedal-control.md](doc/pedal-control.md)): brake tap = delivered current −10 A, accelerator tap = delivered current +10 A, only during CurrentDemand, 30…200 A, 100 A at each session start.
- Bench automation ([tools/](tools/)): vehicle simulation, session runner with CCM power cycle and pyPLC, fast test in ~30 s.

Verified on the bench with the firmware: the vehicle requests 150 A, the charger gets 100 A. Pedal taps change the limit relative to the delivered current; with a charger limited to 150 A, a brake tap reduces the delivered current at once to 140 A.

**Not yet verified:**
- Whether the pedal signals react while the car is charging (not READY).
- Everything in the car.

**Known weak spots:** no feedback of the current limit to the driver (only serial line).

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
Alternative with STLink-V2 and ST-LINK_CLI (from the STM32 ST-LINK Utility), using the generated .hex file:

```sh
ST-LINK_CLI.exe -c SWD -P chargepower-reducer.hex -V -Rst
```

> **Note 1:** Software can disable the SWD of the GD32 by the SWJ_CFG bits in AFIO_MAPR. These bits are write-only; a read-modify-write of AFIO_MAPR (as done for a remap) may disable SWD. The original firmware of the board disables SWD. Workaround for the first flashing: pull NRES to ground and hold it for the first flashing. Try a second time, now programming and verification worked. This firmware writes SWJ_CFG explicitly (SWD on, JTAG off), so flashing works also while it runs.
> **Note 2:** STLink-V2 (clone): its "3.3V" pin is a supply output at only 2.8 V, not a voltage sense input, and it has no reset line connected here. The 3.3 V rail of the black board is load dependent: ~3.4 V while the controller does nothing (reset, blank flash), 3.30 V while it runs. The level difference between 2.8 V and 3.4 V makes the SWD communication unreliable. Solution: connect the STLink "3.3V" pin to the 3.3 V rail of the board. With this, ST-LINK_CLI flashing works reliably, also while the firmware runs. (Side effect: the board is side-powered by the STLink, so switching off its 12 V does not reset it completely.)
> **Note 3:** STLink-V2 together with openocd: not retested since the fix of Note 2. Before, a failed write erased the flash, so ST-LINK_CLI is preferred.
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
- Termination: 120 ohms on both CANs on the board

### CAN wiring at the CCM

The CAN lines are cut inside the CCM, and both sides are led out to Sub-D9 connectors: the outer CAN (car side) and the inner CAN (CCM side).
- Original function: a bridge plug connects outer and inner CAN physically.
- Limiter: the limiter board is plugged in between, CAN1 to the outer CAN (car), CAN2 to the inner CAN (CCM).

### Power supply in the car

The 12 V input of the board is not used (not really automotive capable). Instead, the 5 V of the board is connected to the internal 5 V of the CCM, whose regulator easily drives the additional ~50 mA. So the board is only powered while the CCM is.

### Bus termination

The CCM has no termination resistor. The black board terminates both CANs with 120 ohms. The car wiring stays unchanged; instead the board is adapted:

- CAN1 (car side): the 120 ohm termination on the board is removed. The car bus already has its two terminations, so the board must not add a third one (check: 60 ohms measured between CAN-H and CAN-L with all nodes unpowered).
- CAN2 (CCM side): the 120 ohm termination on the board stays. Board (120 ohms) ... CCM (none). This short stub has only one termination, which is acceptable at the short length and 500 kBaud.

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
- [x] Step 303: Patch the forward path to limit the current (0x59C B4..B5)
- [x] Step 304: Bench: verify the limitation in the firmware (`tools/benchsession.py --current 0:150,30:60` → 100 A, then 60 A)
- [x] Step 305: Cut the CAN lines inside the CCM, and lead outer and inner CAN to Sub-D9 connectors (see "CAN wiring at the CCM")
- [x] Step 306: Bench: verify the pedal control end-to-end (vehicle simulation sends pedal taps on 0x200 / 0x394)
- [ ] Step 307: Car, listen only: bridge plug in place, board CAN1 connected to the car CAN in parallel; charge, press the pedals, check the serial line
- [ ] Step 308: Insert the limiter board between the Sub-D9 connectors
    - [x] Before: remove the CAN1 (car side) 120 ohm termination on the board, see "Bus termination"
- [ ] Step 309: Check whether charging still works (with the default limit of 100 A)
- [ ] Step 310: Check whether the limitation is effective (change the limit with the pedals during charging)

### Phase 4: Further ideas

- [ ] Use a poti on analog input to adjust the limitation
- [x] Brake pedal and accelerator pedal to adjust the limitation, see [doc/pedal-control.md](doc/pedal-control.md). Verified on the bench (step 306), to be tested in the car (step 307).
- [ ] Feedback of the current limit to the driver
- [ ] EVMaximumPowerLimit (0x59E B6..B7) as additional lever, if a charger honors it
- [ ] Check the voltage regulator properties whether they are vehicle compatible long-term
- [ ] ... maybe more ...

### Todo Later

- [ ] Failure mode: if the board dies, the CCM is cut off from the car. Charging fails (safe), but the car may store a fault code or show a warning while driving. Manual fallback: replace the board by the bridge plug (see "CAN wiring at the CCM"). Option for an automatic fallback: a relay that bridges CAN1 and CAN2 when the board is not powered.

## References

- Ref1: Pinout of the blue board https://openinverter.org/forum/viewtopic.php?p=82635#p82635
- Ref2: how to unlock the flash https://openinverter.org/forum/viewtopic.php?p=83864#p83864
- Ref3: reverse engineering of the "green" and "blue" CAN bus filter https://dangerouspayload.com/2020/03/10/hacking-a-mileage-manipulator-can-bus-filter-device/
- Ref4: firmware binaries of green and blue boards https://github.com/andrewkabai/universal_can_filter
- Ref5: datasheet STM32F105 https://www.google.com/url?sa=t&rct=j&q=&esrc=s&source=web&cd=&ved=2ahUKEwiNwpzzitOXAxWtSfEDHeEvJPEQFnoECCcQAQ&url=https%3A%2F%2Fwww.st.com%2Fresource%2Fen%2Fdatasheet%2Fstm32f105r8.pdf&usg=AOvVaw0xi2c2MLBn8kCwDRR__5XS&opi=89978449
