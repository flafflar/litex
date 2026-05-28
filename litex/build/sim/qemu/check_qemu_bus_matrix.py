#!/usr/bin/env python3

#
# This file is part of LiteX.
#
# Copyright (c) 2026 Florent Kermarrec <florent@enjoy-digital.fr>
# SPDX-License-Identifier: BSD-2-Clause

import sys
import argparse
import subprocess


BUS_STANDARDS = ["wishbone", "axi-lite", "axi"]
CPU_VARIANTS  = ["rv32", "rv64"]


def split_csv(value, supported, name):
    items = [item.strip() for item in value.split(",") if item.strip()]
    for item in items:
        if item not in supported:
            raise SystemExit("Unsupported {}: {}".format(name, item))
    return items


def run_smoke(variant, bus_standard, extra_args, with_shared_ram=False):
    cmd = [
        sys.executable,
        "-m", "litex.tools.litex_sim",
        "--cpu-type=qemu",
        "--cpu-variant={}".format(variant),
        "--bus-standard={}".format(bus_standard),
        "--qemu-no-run",
        "--no-compile",
    ] + extra_args
    if with_shared_ram:
        cmd += ["--integrated-main-ram-size=0x100000"]
    print("+ {}".format(" ".join(cmd)))
    subprocess.run(cmd, check=True)


def main():
    parser = argparse.ArgumentParser(description="Smoke QEMU CPU with LiteX bus-standard selections.")
    parser.add_argument("--variants", default="rv32,rv64", help="Comma-separated CPU variants: rv32,rv64.")
    parser.add_argument("--bus-standards", default="wishbone,axi-lite,axi", help="Comma-separated bus standards.")
    parser.add_argument("--with-shared-ram", action="store_true", help="Also expose QEMU shared main RAM in each bus standard.")
    parser.add_argument("litex_args", nargs=argparse.REMAINDER, help="Extra arguments passed after -- to litex_sim.")
    args = parser.parse_args()

    extra_args = args.litex_args
    if extra_args[:1] == ["--"]:
        extra_args = extra_args[1:]

    variants = split_csv(args.variants, CPU_VARIANTS, "variant")
    bus_standards = split_csv(args.bus_standards, BUS_STANDARDS, "bus standard")
    for variant in variants:
        for bus_standard in bus_standards:
            run_smoke(variant, bus_standard, extra_args)
            if args.with_shared_ram:
                run_smoke(variant, bus_standard, extra_args, with_shared_ram=True)


if __name__ == "__main__":
    main()
