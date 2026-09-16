# Flash

From the `build/` directory:

```sh
python -m esptool --chip esp32s3 -b 460800 --before default-reset \
  --after hard-reset write-flash "@flash_args"
```

The device enumerates as `/dev/cu.usbmodem*` (the number depends on the USB port).

## The DTR/RTS trap

**Never toggle DTR/RTS manually on the serial port.** Driving DTR high while releasing RTS pulls IO0 low and lands the chip in ROM download mode — silent to both serial and esptool until physically unplugged and replugged. Reset with esptool's own sequence, nothing else.

## Serial capture

Merely *opening* the port resets the chip (USB-Serial-JTAG behavior), even with DTR/RTS preset low, so every serial capture starts with a fresh boot. Capture with pyserial at 115200 with `dtr = False`, `rts = False` set before `open()`.

## Navigation

Prev: [Build](build.md) · Next: [Provisioning](provisioning.md)
