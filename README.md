# macmini-g4-thermal

`g4temp` reads the temperature sensor of a Mac mini G4 (PowerMac10,1 / 10,2) under Mac OS X 10.4–10.5. OS X itself never shows it: no driver attaches to the sensor, and tools like iStat Menus or Temperature Monitor report nothing on this model.

    $ sudo g4temp
    MAX6642 on PMU I2C bus 1, address 0x90
    local  (temp1, chip on board):   53.50 C  limit  70 C  high alarm: no
    remote (temp2, external diode):  49.00 C  limit 120 C  high alarm: no  diode: ok
    status 0x80 (conversion in progress)

`g4temp -p` prints the same data in Prometheus text format, named like the Linux hwmon metrics of node-exporter (`node_hwmon_temp_celsius{chip="max6642",sensor="temp1|temp2"}`, `..._max_celsius`, `..._max_alarm`, `node_hwmon_temp_fault`). That makes it easy to feed a textfile collector.

## Install

With [Tigerbrew](https://github.com/mistydemeo/tigerbrew):

    brew tap 747500/leopard
    brew install g4temp

Or from source, with the Xcode that ships for the OS (gcc 4.0/4.2 is enough):

    make && sudo make install

The tool needs root: the I2C user client refuses other users.

## How it works

The I/O Registry has a node for the sensor that nothing drives:

    +-o temp-monitor@190  <class AppleK2Device>
          "compatible" = <"max6642">
          "reg" = <00000190>

It is a **Maxim MAX6642** on the I2C bus behind the PMU: `reg` 0x190 = bus 1, 8-bit address 0x90. The chip measures its own die ("local", on the logic board) and one external diode ("remote").

The parent `…/via-pmu/AppleVIA/pmu-i2c/PPCI2CInterface` publishes `IOUserClientClass = I2CUserClient`. That is Apple's own user client, documented in [IOI2CFamily](https://github.com/apple-oss-distributions/IOI2CFamily) (`IOI2CDefs.h`, `IOI2CUser/IOI2C.c`). `g4temp`:

1. finds the `temp-monitor` node and takes bus and address from `reg`, without hardcoding them;
2. opens its parent with `IOServiceOpen` and calls method 0 (`kI2CUCLock`). This is the user client's "open"; without it every read fails with `kIOReturnNotReady`;
3. reads the manufacturer ID (0xFE). It must be 0x4D (Maxim), otherwise the tool stops;
4. reads the registers with method 2 (`kI2CUCRead`) in combined mode (write the register number, then read one byte): temperatures, high limits, status;
5. calls method 1 (`kI2CUCUnlock`) and closes.

**It never writes to the chip or to the PMU.**

Registers, as in the MAX6642 datasheet and Linux [`drivers/hwmon/max6642.c`](https://github.com/torvalds/linux/blob/v6.6/drivers/hwmon/max6642.c):

| reg | meaning |
|---|---|
| 0x00 / 0x11 | local temperature, °C / bits 7:6 = 0.25 °C steps |
| 0x01 / 0x10 | remote temperature, °C / bits 7:6 = 0.25 °C steps |
| 0x05 / 0x07 | local / remote high limit (on this machine 70 / 120 °C) |
| 0x02 | status: bit 6 local high, bit 4 remote high, bit 2 remote diode open, bit 7 busy (conversion in progress) |
| 0xFE | manufacturer ID, 0x4D |

## Load test

`loadtest.sh`: 2 minutes idle, 8 minutes of one `yes` (100% of the single core), 4 minutes of cool-down, a sample every 15 s. Results on a PowerMac10,2 (overclocked to 1.83 GHz, mounted on a wall; room temperature not measured):

| phase | local | remote |
|---|---|---|
| idle | 53.0 °C | 49.3 °C |
| load, +30 s | **57.8** (peak) | **51.5** (peak) |
| load, end of 8 min | 53.3 | 47.3 |
| cool-down, +45 s | **48.8** (minimum) | **45.0** |
| cool-down, +4 min | 53.0 | 49.5 |

The fan speed cannot be read, but its control is visible: both sensors peak right after the load starts and then drop below idle while the load continues. As soon as the load stops they undershoot, then slowly come back to idle. The PMU speeds the fan up, and later slows it down, with a delay.

Under load local rose by +4.8 °C and remote by only +2.3 °C. So it is not confirmed that remote is the CPU diode: it may sit somewhere in the airflow.

## What does not work, and what was not used

- **Fan speed.** The fan is driven by the PMU itself (`pmu-pwm-fans`). This tool does not read it, and I found no documented, read-only way to do so.
- **Raw PMU commands.** Code circulating on the net "talks to the PMU directly" by sending it made-up commands through the ApplePMU user client. The PMU also handles power, reset and the clock, so sending it unknown commands is a good way to reboot or hang the machine. `g4temp` goes through the regular I2C user client instead, and only reads.

## License

MIT, see [LICENSE](LICENSE).
