#!/bin/sh
# End-to-end CLI flow for reading a Nartis RF433 meter through a CC1101 on a
# Raspberry Pi (kernel SPI, e.g. /dev/spidev0.0). Run from the repo root after
#   cmake -S . -B build && cmake --build build
#
# Fill these in for your setup.
SERIAL=012345678901                  # meter serial from the nameplate (12 digits)
SPI=/dev/spidev0.0                   # CC1101 kernel SPI device
SESSION=./meter.session              # where the paired session is stored
OFFSET=0                             # CC1101 crystal correction, Hz (see note below)
CTL=./build/nartisctl

set -e

# 1) Sanity-check the radio wiring: should print "CC1101 connected".
"$CTL" chip-id --radio cc1101 --spi-dev "$SPI"

# 2) Pair once. This stores the session in $SESSION; later reads reuse it.
#    --alternative-channels is the frequency grid most meters reply on; drop it
#    if your meter uses the primary grid.
"$CTL" pair \
  --radio cc1101 --spi-dev "$SPI" \
  --meter-serial "$SERIAL" \
  --session "$SESSION" \
  --alternative-channels

# 3) Read one or more OBIS attributes. Output is JSON on stdout.
#    class/attr default to 3/2 (Register/value) and can be overridden per OBIS.
"$CTL" read \
  --radio cc1101 --spi-dev "$SPI" \
  --meter-serial "$SERIAL" \
  --session "$SESSION" \
  --alternative-channels \
  --batch-size 1 \
  --obis 1.0.32.7.0.255 \
  --obis 1.0.1.7.0.255 \
  --obis 1.0.1.8.0.255

# ---------------------------------------------------------------------------
# freq_offset: each CC1101 module's crystal is a few kHz off. If the meter never
# replies, read the FREQEST a sniffer reports on a received frame and pass it as
# --freq-offset <Hz> (applied to both TX and RX), e.g. --freq-offset 15000.
#
# Session transplant (start already paired, no pairing handshake): copy an
# already-paired device's session with nartis_session_gen, then skip step 2.
# Only one device may use a given session at a time.
#
#   ./build/nartis_session_gen \
#       --key   <32 hex: meter serial ASCII + 4 assigned bytes> \
#       --meter <16 hex meter RF address> \
#       --ciu   <16 hex CIU RF address> \
#       --frame-counter <above the source device's last TX counter> \
#       --out "$SESSION"
#
#   ./build/nartisctl read --radio cc1101 --spi-dev "$SPI" \
#       --meter-serial "$SERIAL" --ciu-address <same 16 hex CIU> \
#       --session "$SESSION" --alternative-channels --obis 1.0.1.7.0.255
# ---------------------------------------------------------------------------
