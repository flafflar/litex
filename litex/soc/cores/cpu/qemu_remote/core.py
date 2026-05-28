#
# This file is part of LiteX.
#
# Copyright (c) 2026 Florent Kermarrec <florent@enjoy-digital.fr>
# SPDX-License-Identifier: BSD-2-Clause

from migen import *

from litex.gen import *

from litex.soc.interconnect.csr import *
from litex.soc.cores.cpu import CPU, CPU_GCC_TRIPLE_RISCV32, CPU_GCC_TRIPLE_RISCV64
from litex.soc.integration.soc import SoCRegion

# Variants -----------------------------------------------------------------------------------------

CPU_VARIANTS = ["standard", "rv32", "rv64"]

# GCC Flags ----------------------------------------------------------------------------------------

GCC_FLAGS = {
    "rv32": "-march=rv32i2p0_mac  -mabi=ilp32 ",
    "rv64": "-march=rv64i2p0_mac  -mabi=lp64 -mcmodel=medany ",
}

# Helpers ------------------------------------------------------------------------------------------

def _region_is_in_io_regions(region_origin, region_size, io_regions):
    for origin, size in io_regions.items():
        if region_origin >= origin and region_origin + region_size <= origin + size:
            return True
    return False


# QEMURemote ---------------------------------------------------------------------------------------

class QEMURemote(CPU):
    category                 = "emulator"
    family                   = "riscv"
    name                     = "qemu_remote"
    human_name               = "QEMU RISC-V Remote"
    variants                 = CPU_VARIANTS
    data_width               = 32
    endianness               = "little"
    gcc_triple               = CPU_GCC_TRIPLE_RISCV32
    linker_output_format     = "elf32-littleriscv"
    nop                      = "nop"
    reset_address_check      = False
    integrated_rom_supported = False

    def __init__(self, platform, variant="standard"):
        if variant == "standard":
            variant = "rv32"

        self.platform = platform
        self.variant  = variant
        self.xlen     = 64 if variant == "rv64" else 32
        self.reset    = Signal()

        self.periph_buses = []
        self.memory_buses = []

        self.interrupt           = Signal(32)
        self.interrupts          = {}
        self.reserved_interrupts = {"noirq": 0}

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
                "clint"    : 0xf001_0000,
                "plic"     : 0xf0c0_0000,
                "rom"      : 0x0000_0000,
                "sram"     : 0x1000_0000,
                "main_ram" : 0x4000_0000,
                "csr"      : 0xf000_0000,
            }
            self.io_regions = {0x8000_0000: 0x8000_0000}

        self.irq = CSRStatus(32, description="QEMU Remote interrupt bitmap.")
        self.reset_status = CSRStatus(1, description="QEMU Remote latched CPU reset request.")
        self.reset_clear = CSRStorage(fields=[
            CSRField("clear", size=1, pulse=True, description="Clear the latched QEMU Remote CPU reset request."),
        ])

        # # #

        reset_latched = Signal()

        self.sync += [
            If(self.reset,
                reset_latched.eq(1)
            ).Elif(self.reset_clear.fields.clear,
                reset_latched.eq(0)
            )
        ]
        self.comb += [
            self.irq.status.eq(self.interrupt),
            self.reset_status.status.eq(reset_latched),
        ]

    @property
    def gcc_flags(self):
        flags  = "-mno-save-restore "
        flags += GCC_FLAGS[self.variant]
        flags += "-D__qemu__ -D__riscv_plic__ -DUART_POLLING "
        return flags

    def set_reset_address(self, reset_address):
        self.reset_address = reset_address

    def add_jtag(self, pads):
        pass

    def add_soc_components(self, soc):
        soc.add_config("CPU_COUNT", 1)
        soc.add_config("CPU_ISA",   "rv{}imac".format(self.xlen))
        soc.add_config("CPU_MMU",   {32: "sv32", 64: "sv39"}[self.xlen])

        for name, size in {
            "clint" : 0x1_0000,
            "plic"  : 0x40_0000,
        }.items():
            origin = soc.mem_map.get(name)
            cached = not _region_is_in_io_regions(
                region_origin = origin,
                region_size   = size,
                io_regions    = self.io_regions,
            )
            soc.bus.add_region(name, SoCRegion(
                origin = origin,
                size   = size,
                cached = cached,
                linker = True,
            ))
