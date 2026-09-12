#!/usr/bin/env python3
"""Read Active Power from a Nartis RF433 meter via nartisctl.

This example uses the command-line tool built by this repository, so it works
without Python bindings:

  cmake -S . -B build
  cmake --build build

  python3 examples/read_active_power.py \
    --meter-serial 012345678901 \
    --sdio 23 --sclk 18 --csb 27 --fcsb 26 --gpio3 14 \
    --alternative-channels

Pins are BCM GPIO numbers.
"""

from __future__ import annotations

import argparse
import json
import subprocess
from pathlib import Path


ACTIVE_POWER_OBIS = "1.0.1.7.0.255"


def repo_root() -> Path:
    return Path(__file__).resolve().parents[1]


def default_nartisctl() -> Path:
    return repo_root() / "build" / "nartisctl"


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Read OBIS 1.0.1.7.0.255 Active Power from a Nartis RF433 meter."
    )
    parser.add_argument("--nartisctl", type=Path, default=default_nartisctl())
    parser.add_argument("--meter-serial", required=True, help="12-digit meter serial from the nameplate")
    parser.add_argument("--session", type=Path, help="Session file path; defaults to ./<meter>.session")
    parser.add_argument("--sdio", type=int, required=True, help="CMT2300A SDIO BCM GPIO")
    parser.add_argument("--sclk", type=int, required=True, help="CMT2300A SCLK BCM GPIO")
    parser.add_argument("--csb", type=int, required=True, help="CMT2300A CSB BCM GPIO")
    parser.add_argument("--fcsb", type=int, required=True, help="CMT2300A FCSB BCM GPIO")
    parser.add_argument("--gpio3", type=int, required=True, help="CMT2300A GPIO3/INT2 BCM GPIO")
    parser.add_argument("--ciu-serial", help="Optional CIU serial used to derive CIU RF address")
    parser.add_argument("--ciu-address", help="Optional exact CIU RF address as 16 hex chars")
    parser.add_argument("--fixed-channel", type=int, choices=range(4), metavar="0..3")
    parser.add_argument("--alternative-channels", action="store_true")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    session = args.session or Path(f"{args.meter_serial}.session")

    cmd = [
        str(args.nartisctl),
        "read",
        "--meter-serial",
        args.meter_serial,
        "--session",
        str(session),
        "--sdio",
        str(args.sdio),
        "--sclk",
        str(args.sclk),
        "--csb",
        str(args.csb),
        "--fcsb",
        str(args.fcsb),
        "--gpio3",
        str(args.gpio3),
        "--obis",
        f"{ACTIVE_POWER_OBIS},class=3,attr=2",
    ]

    if args.ciu_serial:
        cmd += ["--ciu-serial", args.ciu_serial]
    if args.ciu_address:
        cmd += ["--ciu-address", args.ciu_address]
    if args.fixed_channel is not None:
        cmd += ["--fixed-channel", str(args.fixed_channel)]
    if args.alternative_channels:
        cmd.append("--alternative-channels")

    result = subprocess.run(cmd, text=True, capture_output=True, check=False)
    if result.returncode != 0:
        print(result.stderr.strip() or result.stdout.strip())
        return result.returncode

    data = json.loads(result.stdout)
    value = data["values"][0]

    print(json.dumps(data, indent=2))
    if value["valid"]:
        print(f"Active Power ({ACTIVE_POWER_OBIS}): {value['number']} W")
    else:
        print(f"Active Power ({ACTIVE_POWER_OBIS}): no value returned")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

