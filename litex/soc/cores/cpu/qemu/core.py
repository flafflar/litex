#
# This file is part of LiteX.
#
# Copyright (c) 2026 Florent Kermarrec <florent@enjoy-digital.fr>
# SPDX-License-Identifier: BSD-2-Clause

from migen import *

from litex.gen import *

from litex.build.generic_platform import Pins, Subsignal

from litex.soc.interconnect import wishbone
from litex.soc.cores.cpu import CPU, CPU_GCC_TRIPLE_RISCV32, CPU_GCC_TRIPLE_RISCV64

# Variants -----------------------------------------------------------------------------------------

CPU_VARIANTS = ["standard", "rv32", "rv64"]

# GCC Flags ----------------------------------------------------------------------------------------

GCC_FLAGS = {
    "rv32": "-march=rv32i2p0_m    -mabi=ilp32 ",
    "rv64": "-march=rv64i2p0_mac  -mabi=lp64 -mcmodel=medany ",
}

# QEMU ---------------------------------------------------------------------------------------------

class QEMU(CPU):
    category             = "emulator"
    family               = "riscv"
    name                 = "qemu"
    human_name           = "QEMU RISC-V"
    variants             = CPU_VARIANTS
    data_width           = 32
    endianness           = "little"
    gcc_triple           = CPU_GCC_TRIPLE_RISCV32
    linker_output_format = "elf32-littleriscv"
    nop                  = "nop"

    def __init__(self, platform, variant="standard"):
        if variant == "standard":
            variant = "rv32"

        self.platform = platform
        self.variant  = variant
        self.xlen     = 64 if variant == "rv64" else 32
        self.reset    = Signal()

        # QEMU drives a single 32-bit Wishbone master into the regular LiteX
        # interconnect. Wider CPU accesses are split in the simulator bridge.
        self.bus = wishbone.Interface(data_width=32, address_width=32, addressing="word")
        self.periph_buses = [self.bus]
        self.memory_buses = []

        self.interrupt = Signal(32)
        self.interrupts = {}

        if self.xlen == 64:
            self.data_width           = 64
            self.gcc_triple           = CPU_GCC_TRIPLE_RISCV64
            self.linker_output_format = "elf64-littleriscv"
            self.mem_map = {
                "clint"    : 0x0200_0000,
                "plic"     : 0x0c00_0000,
                "rom"      : 0x1000_0000,
                "sram"     : 0x1100_0000,
                "csr"      : 0x1200_0000,
                "ethmac"   : 0x3000_0000,
                "main_ram" : 0x8000_0000,
            }
            self.io_regions = {0x1200_0000: 0x6e00_0000}
        else:
            self.data_width           = 32
            self.gcc_triple           = CPU_GCC_TRIPLE_RISCV32
            self.linker_output_format = "elf32-littleriscv"
            self.mem_map = {
                "rom"      : 0x0000_0000,
                "sram"     : 0x1000_0000,
                "main_ram" : 0x4000_0000,
                "csr"      : 0xf000_0000,
            }
            self.io_regions = {0x8000_0000: 0x8000_0000}

        self._add_sim_pads(platform)

    @property
    def gcc_flags(self):
        flags  = "-mno-save-restore "
        flags += GCC_FLAGS[self.variant]
        flags += "-D__qemu__ -DUART_POLLING "
        return flags

    def _add_sim_pads(self, platform):
        wb = self.bus
        platform.add_extension([
            ("qemu_wishbone", 0,
                Subsignal("adr",   Pins(len(wb.adr))),
                Subsignal("dat_w", Pins(len(wb.dat_w))),
                Subsignal("dat_r", Pins(len(wb.dat_r))),
                Subsignal("sel",   Pins(len(wb.sel))),
                Subsignal("cyc",   Pins(1)),
                Subsignal("stb",   Pins(1)),
                Subsignal("ack",   Pins(1)),
                Subsignal("we",    Pins(1)),
                Subsignal("cti",   Pins(len(wb.cti))),
                Subsignal("bte",   Pins(len(wb.bte))),
                Subsignal("err",   Pins(1)),
            ),
            ("qemu_irq", 0, Pins(len(self.interrupt))),
        ])
        self.comb += wb.connect_to_pads(platform.request("qemu_wishbone"), mode="slave")
        self.comb += platform.request("qemu_irq").eq(self.interrupt)

    def set_reset_address(self, reset_address):
        self.reset_address = reset_address

    def add_jtag(self, pads):
        pass

# QEMU Shared RAM ----------------------------------------------------------------------------------

class QEMUSharedRAM(LiteXModule):
    def __init__(self, platform, name="qemu_shared_ram"):
        self.bus = wishbone.Interface(data_width=32, address_width=32, addressing="word")
        self._add_sim_pads(platform, name)

    def _add_sim_pads(self, platform, name):
        wb = self.bus
        platform.add_extension([
            (name, 0,
                Subsignal("adr",   Pins(len(wb.adr))),
                Subsignal("dat_w", Pins(len(wb.dat_w))),
                Subsignal("dat_r", Pins(len(wb.dat_r))),
                Subsignal("sel",   Pins(len(wb.sel))),
                Subsignal("cyc",   Pins(1)),
                Subsignal("stb",   Pins(1)),
                Subsignal("ack",   Pins(1)),
                Subsignal("we",    Pins(1)),
                Subsignal("cti",   Pins(len(wb.cti))),
                Subsignal("bte",   Pins(len(wb.bte))),
                Subsignal("err",   Pins(1)),
            ),
        ])
        self.comb += wb.connect_to_pads(platform.request(name), mode="master")
