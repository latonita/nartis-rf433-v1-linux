# Nartis CMT2300A Linux Library

Standalone C API and CLI for talking to Nartis RF433 meters through a CMT2300A radio module attached to a Linux computer, initially Raspberry Pi.

The project ports the working ESPHome component into Linux-friendly layers:

- `cmt2300a`: bit-banged 3-wire SPI, register banks, FIFO, RSSI scan, TX/RX.
- `rf`: Nartis RF framing, CRC-16/DNP, AES-128-GCM, counters, nested response decrypt.
- `dlms`: OBIS parsing, DLMS get-request builders, response/value parsing.
- `session`: pairing, persistent session state, blocking read cycle.
- `platform`: GPIO/time/log backend. The included backend uses Linux sysfs GPIO.

## Build

```sh
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

OpenSSL crypto is required for AES-GCM.

## CLI

Pins are BCM GPIO numbers, not physical header pin numbers.

```sh
./build/nartisctl chip-id \
  --sdio 23 --sclk 18 --csb 27 --fcsb 26 --gpio3 14
```

```sh
./build/nartisctl pair \
  --meter-serial 012345678901 \
  --session ./012345678901.session \
  --sdio 23 --sclk 18 --csb 27 --fcsb 26 --gpio3 14 \
  --alternative-channels
```

```sh
./build/nartisctl read \
  --meter-serial 012345678901 \
  --session ./012345678901.session \
  --sdio 23 --sclk 18 --csb 27 --fcsb 26 --gpio3 14 \
  --alternative-channels \
  --obis 1.0.32.7.0.255,class=3,attr=2 \
  --obis 1.0.1.7.0.255,class=3,attr=2
```

If a meter is already paired with a real CIU, use `--ciu-address HEX16` to impersonate that exact 8-byte RF address.

## Examples

See [examples/read_active_power.py](examples/read_active_power.py) for an end-to-end Python example that sets GPIO pins, meter serial, and reads OBIS `1.0.1.7.0.255` Active Power through `nartisctl`.

## API

Main public headers:

- `include/nartis/nartis.h`: high-level meter API.
- `include/nartis/cmt2300a.h`: low-level radio API.
- `include/nartis/nartis_backend.h`: platform backend interface.
- `include/nartis/nartis_types.h`: OBIS, value, config and error types.

Typical flow:

1. Open a backend, for example `nartis_sysfs_backend_open`.
2. Fill `nartis_config_t`.
3. Call `nartis_open`, `nartis_init`.
4. Call `nartis_pair` once, or let `nartis_read` pair if no session exists.
5. Call `nartis_read` with one or more `nartis_attr_t`.

## Notes

The sysfs GPIO backend is dependency-light but timing-jittery. The backend interface is deliberately small so a faster Raspberry Pi backend can be added later using `libgpiod`, `pigpio`, or a kernel helper without touching the protocol layers.
