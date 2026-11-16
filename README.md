# Nartis RF433 Linux Library

Standalone C API and CLI for talking to Nartis RF433 meters from a Linux computer, initially Raspberry Pi. Two radios are supported, selected at runtime: a **CMT2300A** on bit-banged 3-wire SPI, or a **CC1101** on a kernel SPI device (`/dev/spidevX.Y`).

The project ports the working ESPHome component into Linux-friendly layers:

- `radio`: a small vtable so the protocol layers are radio-agnostic.
- `cmt2300a`: bit-banged 3-wire SPI, register banks, FIFO, RSSI scan, TX/RX.
- `cc1101`: kernel spidev driver; hardware sync off on TX with a clean preamble + `F6 72` from the FIFO, 16/16 sync on RX, per-module crystal correction.
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

### CC1101 radio

Pass `--radio cc1101` and a SPI device instead of the CMT2300A GPIO pins. The CC1101 uses the kernel SPI interface, so no bit-bang pins are needed.

```sh
# wiring sanity check
./build/nartisctl chip-id --radio cc1101 --spi-dev /dev/spidev0.0

# pair, then read
./build/nartisctl pair \
  --radio cc1101 --spi-dev /dev/spidev0.0 \
  --meter-serial 012345678901 --session ./meter.session --alternative-channels

./build/nartisctl read \
  --radio cc1101 --spi-dev /dev/spidev0.0 \
  --meter-serial 012345678901 --session ./meter.session --alternative-channels \
  --obis 1.0.32.7.0.255 --obis 1.0.1.7.0.255
```

CC1101 options: `--spi-dev PATH` (default `/dev/spidev0.0`), `--gdo0 N` (optional GDO0 GPIO; polled over SPI if omitted), `--freq-offset HZ` (signed crystal correction applied to TX and RX), `--tx-preamble N` (default 12). Each CC1101 module's crystal is a few kHz off; if the meter never replies, read the FREQEST a sniffer reports on a received frame and pass it as `--freq-offset`.

### Session transplant

Instead of pairing, a device can start already paired by loading a session copied from another already-paired device. Generate the session file with `nartis_session_gen` (it uses the library's own store format) and read with the matching `--ciu-address`. Only one device may use a given session at a time, and set `--frame-counter` above the source device's last TX counter to avoid replay rejection.

```sh
./build/nartis_session_gen \
  --key   <32 hex: meter serial ASCII + 4 assigned bytes> \
  --meter <16 hex meter RF address> \
  --ciu   <16 hex CIU RF address> \
  --frame-counter 1000 --out ./meter.session

./build/nartisctl read --radio cc1101 --spi-dev /dev/spidev0.0 \
  --meter-serial 012345678901 --ciu-address <same 16 hex CIU> \
  --session ./meter.session --alternative-channels --obis 1.0.1.7.0.255
```

## Examples

- [examples/read_cc1101.sh](examples/read_cc1101.sh): full CC1101 CLI flow (chip-id, pair, read) with session-transplant notes.
- [examples/read_active_power.py](examples/read_active_power.py): end-to-end Python example that sets GPIO pins, meter serial, and reads OBIS `1.0.1.7.0.255` Active Power through `nartisctl` (CMT2300A).

## API

Main public headers:

- `include/nartis/nartis.h`: high-level meter API and `nartis_config_t`.
- `include/nartis/nartis_radio.h`: radio vtable and `radio_type` selection.
- `include/nartis/cmt2300a.h`, `include/nartis/cc1101.h`: low-level radio APIs.
- `include/nartis/nartis_backend.h`: platform backend interface.
- `include/nartis/nartis_types.h`: OBIS, value, config and error types.

Typical flow:

1. Open a backend, for example `nartis_sysfs_backend_open`.
2. Fill `nartis_config_t` (set `radio_type` and either `pins` for CMT2300A or `cc1101` for CC1101).
3. Call `nartis_open`, `nartis_init`.
4. Call `nartis_pair` once, or let `nartis_read` pair if no session exists.
5. Call `nartis_read` with one or more `nartis_attr_t`.

## Notes

Leave a gap between reads. The meter will not open a fresh session immediately after the previous one; polling it again within a few tens of seconds tends to fail. Space `read` invocations by about 120 s (this is the caller's responsibility; the library does not enforce it). Within a single read the library now retries the wake, re-sends the get-request, and handles a mid-session re-key.

The sysfs GPIO backend is dependency-light but timing-jittery. The backend interface is deliberately small so a faster Raspberry Pi backend can be added later using `libgpiod`, `pigpio`, or a kernel helper without touching the protocol layers.
