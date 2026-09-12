# Examples

## Read Active Power With Python

Build the C library and CLI first:

```sh
cmake -S . -B build
cmake --build build
```

Then read OBIS `1.0.1.7.0.255` Active Power:

```sh
python3 examples/read_active_power.py \
  --meter-serial 012345678901 \
  --sdio 23 \
  --sclk 18 \
  --csb 27 \
  --fcsb 26 \
  --gpio3 14 \
  --alternative-channels
```

The script stores/restores pairing state in `./<meter-serial>.session` unless
you pass `--session PATH`.

Pins are BCM GPIO numbers, not physical Raspberry Pi header pin numbers.

