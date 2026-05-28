#!/usr/bin/env python3

#
# This file is part of LiteX.
#
# Copyright (c) 2015-2020 Florent Kermarrec <florent@enjoy-digital.fr>
# Copyright (c) 2020 Antmicro <www.antmicro.com>
# Copyright (c) 2017 Pierre-Olivier Vauboin <po@lambdaconcept>
# Copyright (c) 2023 Victor Suarez Rovere <suarezvictor@gmail.com>
# Copyright (c) 2026 Aoba Fujino <41146f@gmail.com>
# SPDX-License-Identifier: BSD-2-Clause

import os
import sys
import json
import shlex
import shutil
import argparse
import subprocess

from migen import *

from litex.build.generic_platform import *
from litex.build.sim              import SimPlatform
from litex.build.sim.config       import SimConfig

from litex.soc.integration.common   import *
from litex.soc.integration.soc_core import *
from litex.soc.integration.builder  import *
from litex.soc.integration.soc      import *

from litex.soc.cores.bitbang import *
from litex.soc.cores.gpio    import GPIOTristate
from litex.soc.cores.cpu     import CPUS
from litex.soc.cores.video   import VideoGenericPHY

from liteeth.common             import *
from liteeth.phy.gmii           import LiteEthPHYGMII
from liteeth.phy.xgmii          import LiteEthPHYXGMII
from liteeth.phy.model          import LiteEthPHYModel
from liteeth.mac                import LiteEthMAC
from liteeth.core.arp           import LiteEthARP
from liteeth.core.ip            import LiteEthIP
from liteeth.core.udp           import LiteEthUDP
from liteeth.core.icmp          import LiteEthICMP
from liteeth.core               import LiteEthUDPIPCore
from liteeth.frontend.etherbone import LiteEthEtherbone

# IOs ----------------------------------------------------------------------------------------------

_io = [
    # Clk / Rst.
    ("sys_clk", 0, Pins(1)),
    ("sys_rst", 0, Pins(1)),

    # Serial.
    ("serial", 0,
        Subsignal("source_valid", Pins(1)),
        Subsignal("source_ready", Pins(1)),
        Subsignal("source_data",  Pins(8)),

        Subsignal("sink_valid",   Pins(1)),
        Subsignal("sink_ready",   Pins(1)),
        Subsignal("sink_data",    Pins(8)),
    ),

    # Ethernet (Stream Endpoint).
    ("eth_clocks", 0,
        Subsignal("tx", Pins(1)),
        Subsignal("rx", Pins(1)),
    ),
    ("eth", 0,
        Subsignal("source_valid", Pins(1)),
        Subsignal("source_ready", Pins(1)),
        Subsignal("source_data",  Pins(8)),

        Subsignal("sink_valid",   Pins(1)),
        Subsignal("sink_ready",   Pins(1)),
        Subsignal("sink_data",    Pins(8)),
    ),

    # Ethernet (XGMII).
    ("xgmii_eth", 0,
        Subsignal("rx_data",      Pins(64)),
        Subsignal("rx_ctl",       Pins(8)),
        Subsignal("tx_data",      Pins(64)),
        Subsignal("tx_ctl",       Pins(8)),
    ),

    # Ethernet (GMII).
    ("gmii_eth", 0,
        Subsignal("rx_data",      Pins(8)),
        Subsignal("rx_dv",        Pins(1)),
        Subsignal("rx_er",        Pins(1)),
        Subsignal("tx_data",      Pins(8)),
        Subsignal("tx_en",        Pins(1)),
        Subsignal("tx_er",        Pins(1)),
    ),

    # I2C.
    ("i2c", 0,
        Subsignal("scl",     Pins(1)),
        Subsignal("sda_out", Pins(1)),
        Subsignal("sda_in",  Pins(1)),
    ),

    # SPI-Flash (X1).
    ("spiflash", 0,
        Subsignal("cs_n", Pins(1)),
        Subsignal("clk",  Pins(1)),
        Subsignal("mosi", Pins(1)),
        Subsignal("miso", Pins(1)),
        Subsignal("wp",   Pins(1)),
        Subsignal("hold", Pins(1)),
    ),

    # SPI-Flash (X4).
    ("spiflash4x", 0,
        Subsignal("cs_n", Pins(1)),
        Subsignal("clk",  Pins(1)),
        Subsignal("dq",   Pins(4)),
    ),

    # Tristate GPIOs (for sim control/status).
    ("gpio", 0,
        Subsignal("oe", Pins(32)),
        Subsignal("o",  Pins(32)),
        Subsignal("i",  Pins(32)),
    ),

    # JTAG.
    ("jtag", 0,
        Subsignal("tck", Pins(1)),
        Subsignal("tms", Pins(1)),
        Subsignal("tdi", Pins(1)),
        Subsignal("tdo", Pins(1)),
        Subsignal("ntrst", Pins(1)),
    ),

    # Video (VGA).
    ("vga", 0,
        Subsignal("hsync", Pins(1)),
        Subsignal("vsync", Pins(1)),
        Subsignal("de",    Pins(1)),
        Subsignal("r",     Pins(8)),
        Subsignal("g",     Pins(8)),
        Subsignal("b",     Pins(8)),
    )
]

# Platform -----------------------------------------------------------------------------------------

class Platform(SimPlatform):
    def __init__(self):
        SimPlatform.__init__(self, "SIM", _io)

# Simulation SoC -----------------------------------------------------------------------------------

class SimSoC(SoCCore):
    supported_ethernet_phy_models = ["sim", "xgmii", "gmii"]

    def __init__(self,
        with_sdram             = False,
        with_sdram_bist        = False,
        with_ethernet          = False,
        ethernet_phy_model     = "sim",
        ethernet_local_ip      = "192.168.1.50",
        ethernet_remote_ip     = "192.168.1.100",
        with_etherbone         = False,
        with_analyzer          = False,
        sdram_module           = "MT48LC16M16",
        sdram_init             = [],
        sdram_data_width       = 32,
        sdram_spd_data         = None,
        sdram_verbosity        = 0,
        with_i2c               = False,
        with_sdcard            = False,
        with_spi_flash         = False,
        spi_flash_init         = [],
        with_gpio              = False,
        with_video_framebuffer = False,
        with_video_terminal    = False,
        with_video_colorbars   = False,
        sim_debug              = False,
        trace_reset_on         = False,
        with_jtag              = False,
        **kwargs):

        # Platform ---------------------------------------------------------------------------------
        platform = Platform()

        # Parameters -------------------------------------------------------------------------------
        sys_clk_freq = int(1e6)

        # CRG --------------------------------------------------------------------------------------
        self.crg = CRG(platform.request("sys_clk"))

        # SoCCore ----------------------------------------------------------------------------------
        SoCCore.__init__(self, platform, clk_freq=sys_clk_freq,
            ident = "LiteX Simulation",
            **kwargs)

        # BIOS Config ------------------------------------------------------------------------------
        # FIXME: Expose?
        #self.add_config("BIOS_NO_PROMPT")
        #self.add_config("BIOS_NO_DELAYS")
        #self.add_config("BIOS_NO_BUILD_TIME")
        #self.add_config("BIOS_NO_CRC")

        # SDRAM ------------------------------------------------------------------------------------
        if not self.integrated_main_ram_size and with_sdram:
            from litedram           import modules as litedram_modules
            from litedram.phy.model import sdram_module_nphases
            from litedram.phy.model import SDRAMPHYModel

            sdram_clk_freq = int(100e6) # FIXME: use 100MHz timings
            if sdram_spd_data is None:
                sdram_module_cls = getattr(litedram_modules, sdram_module)
                sdram_rate       = "1:{}".format(sdram_module_nphases[sdram_module_cls.memtype])
                sdram_module     = sdram_module_cls(sdram_clk_freq, sdram_rate)
            else:
                sdram_module = litedram_modules.SDRAMModule.from_spd_data(sdram_spd_data, sdram_clk_freq)
            self.sdrphy = SDRAMPHYModel(
                module     = sdram_module,
                data_width = sdram_data_width,
                clk_freq   = sdram_clk_freq,
                verbosity  = sdram_verbosity,
                init       = sdram_init)
            self.add_sdram("sdram",
                phy                     = self.sdrphy,
                module                  = sdram_module,
                l2_cache_size           = kwargs.get("l2_size", 8192),
                l2_cache_min_data_width = kwargs.get("min_l2_data_width", 128),
                l2_cache_reverse        = False,
                with_bist               = with_sdram_bist
            )
            if sdram_init != []:
                # Skip SDRAM test to avoid corrupting pre-initialized contents.
                self.add_constant("SDRAM_TEST_DISABLE")
            else:
                # Reduce memtest size for simulation speedup
                self.add_constant("MEMTEST_DATA_SIZE", 8*1024)
                self.add_constant("MEMTEST_ADDR_SIZE", 8*1024)

        # Ethernet / Etherbone PHY -----------------------------------------------------------------
        if with_ethernet or with_etherbone:
            if ethernet_phy_model == "sim":
                self.ethphy = LiteEthPHYModel(self.platform.request("eth", 0))
                self.add_constant("HW_PREAMBLE_CRC");
            elif ethernet_phy_model == "xgmii":
                self.ethphy = LiteEthPHYXGMII(None, self.platform.request("xgmii_eth", 0), model=True)
            elif ethernet_phy_model == "gmii":
                self.ethphy = LiteEthPHYGMII(None, self.platform.request("gmii_eth", 0), model=True)
            else:
                raise ValueError("Unknown Ethernet PHY model: {}.".format(ethernet_phy_model))

        # Etherbone with optional Ethernet ---------------------------------------------------------
        if with_etherbone:
            self.add_etherbone(
                phy              = self.ethphy,
                # Etherbone Parameters.
                ip_address       = convert_ip(ethernet_local_ip) + int(with_ethernet), # +1 when both to avoid conflict.
                mac_address      = 0x10e2d5000001,
                data_width       = 8,
                # Ethernet Parameters.
                with_ethmac      = with_ethernet,
                ethmac_address   = 0x10e2d5000000,
                ethmac_local_ip  = ethernet_local_ip,
                ethmac_remote_ip = ethernet_remote_ip,
            )

        # Ethernet only ----------------------------------------------------------------------------
        elif with_ethernet:
            # Ethernet MAC
            self.ethmac = ethmac = LiteEthMAC(
                phy        = self.ethphy,
                dw         = 64 if ethernet_phy_model == "xgmii" else 32,
                interface  = "wishbone",
                endianness = self.cpu.endianness
            )
            ethmac_rx_region_size = ethmac.rx_slots.constant*ethmac.slot_size.constant
            ethmac_tx_region_size = ethmac.tx_slots.constant*ethmac.slot_size.constant
            ethmac_region_size    = ethmac_rx_region_size + ethmac_tx_region_size
            self.bus.add_region("ethmac", SoCRegion(
                origin = self.mem_map.get("ethmac", None),
                size   = ethmac_region_size,
                linker = True,
                cached = False,
            ))
            ethmac_rx_region = SoCRegion(
                origin = self.bus.regions["ethmac"].origin + 0,
                size   = ethmac_rx_region_size,
                linker = True,
                cached = False,
            )
            self.bus.add_slave(name="ethmac_rx", slave=ethmac.bus_rx, region=ethmac_rx_region)
            ethmac_tx_region = SoCRegion(
                origin = self.bus.regions["ethmac"].origin + ethmac_rx_region_size,
                size   = ethmac_tx_region_size,
                linker = True,
                cached = False,
            )
            self.bus.add_slave(name="ethmac_tx", slave=ethmac.bus_tx, region=ethmac_tx_region)

            # Add IRQs (if enabled).
            if self.irq.enabled:
                self.irq.add("ethmac", use_loc_if_exists=True)

        # I2C --------------------------------------------------------------------------------------
        if with_i2c:
            pads = platform.request("i2c", 0)
            self.i2c = I2CMasterSim(pads)

        # JTAG -------------------------------------------------------------------------------------
        if with_jtag:
            jtag_pads = platform.request("jtag")
            self.cpu.add_jtag(jtag_pads)

        # SDCard -----------------------------------------------------------------------------------
        if with_sdcard:
            self.add_sdcard("sdcard", use_emulator=True)

        # SPI Flash --------------------------------------------------------------------------------
        if with_spi_flash:
            from litespi.phy.model import LiteSPIPHYModel
            from litespi.modules import S25FL128L
            from litespi.opcodes import SpiNorFlashOpCodes as Codes
            spiflash_module = S25FL128L(Codes.READ_1_1_4)
            if spi_flash_init is None:
                platform.add_sources(os.path.abspath(os.path.dirname(__file__)), "../build/sim/verilog/iddr_verilog.v")
                platform.add_sources(os.path.abspath(os.path.dirname(__file__)), "../build/sim/verilog/oddr_verilog.v")
            self.spiflash_phy = LiteSPIPHYModel(spiflash_module, init=spi_flash_init)
            self.add_spi_flash(phy=self.spiflash_phy, mode="4x", module=spiflash_module, with_master=True)

        # GPIO --------------------------------------------------------------------------------------
        if with_gpio:
            self.gpio = GPIOTristate(platform.request("gpio"), with_irq=True)
            self.irq.add("gpio", use_loc_if_exists=True)

        # Video Framebuffer ------------------------------------------------------------------------
        if with_video_framebuffer:
            video_pads = platform.request("vga")
            self.submodules.videophy = VideoGenericPHY(video_pads)
            self.add_video_framebuffer(phy=self.videophy, timings="640x480@60Hz", format="rgb888")

        # Video Terminal ---------------------------------------------------------------------------
        if with_video_terminal:
            self.submodules.videophy = VideoGenericPHY(platform.request("vga"))
            self.add_video_terminal(phy=self.videophy, timings="640x480@60Hz")

        # Video test pattern -----------------------------------------------------------------------
        if with_video_colorbars:
            self.submodules.videophy = VideoGenericPHY(platform.request("vga"))
            self.add_video_colorbars(phy=self.videophy, timings="640x480@60Hz")

        # Simulation debugging ----------------------------------------------------------------------
        if sim_debug:
            platform.add_debug(self, reset=1 if trace_reset_on else 0)
        else:
            self.comb += platform.trace.eq(1)

        # Analyzer ---------------------------------------------------------------------------------
        if with_analyzer:
            from litescope import LiteScopeAnalyzer

            analyzer_signals = [
                # IBus (could also just added as self.cpu.ibus)
                self.cpu.ibus.stb,
                self.cpu.ibus.cyc,
                self.cpu.ibus.adr,
                self.cpu.ibus.we,
                self.cpu.ibus.ack,
                self.cpu.ibus.sel,
                self.cpu.ibus.dat_w,
                self.cpu.ibus.dat_r,
                # DBus (could also just added as self.cpu.dbus)
                self.cpu.dbus.stb,
                self.cpu.dbus.cyc,
                self.cpu.dbus.adr,
                self.cpu.dbus.we,
                self.cpu.dbus.ack,
                self.cpu.dbus.sel,
                self.cpu.dbus.dat_w,
                self.cpu.dbus.dat_r,
            ]
            self.analyzer = LiteScopeAnalyzer(analyzer_signals,
                depth        = 512,
                clock_domain = "sys",
                csr_csv      = "analyzer.csv")

# Build --------------------------------------------------------------------------------------------

def generate_gtkw_savefile(builder, vns, trace_fst):
    from litex.build.sim import gtkwave as gtkw
    dumpfile = os.path.join(builder.gateware_dir, "sim.{}".format("fst" if trace_fst else "vcd"))
    savefile = os.path.join(builder.gateware_dir, "sim.gtkw")
    soc = builder.soc

    with gtkw.GTKWSave(vns, savefile=savefile, dumpfile=dumpfile) as save:
        save.clocks()
        save.fsm_states(soc)
        if "main_ram" in soc.bus.slaves.keys():
            save.add(soc.bus.slaves["main_ram"], mappers=[gtkw.wishbone_sorter(), gtkw.wishbone_colorer()])

        if hasattr(soc, "sdrphy"):
            # all dfi signals
            save.add(soc.sdrphy.dfi, mappers=[gtkw.dfi_sorter(), gtkw.dfi_in_phase_colorer()])

            # each phase in separate group
            with save.gtkw.group("dfi phaseX", closed=True):
                for i, phase in enumerate(soc.sdrphy.dfi.phases):
                    save.add(phase, group_name="dfi p{}".format(i), mappers=[
                        gtkw.dfi_sorter(phases=False),
                        gtkw.dfi_in_phase_colorer(),
                    ])

            # only dfi command/data signals
            def dfi_group(name, suffixes):
                save.add(soc.sdrphy.dfi, group_name=name, mappers=[
                    gtkw.regex_filter(gtkw.suffixes2re(suffixes)),
                    gtkw.dfi_sorter(),
                    gtkw.dfi_per_phase_colorer(),
                ])

            dfi_group("dfi commands", ["cas_n", "ras_n", "we_n"])
            dfi_group("dfi commands", ["wrdata"])
            dfi_group("dfi commands", ["wrdata_mask"])
            dfi_group("dfi commands", ["rddata"])

def _qemu_xlen(cpu_variant):
    return 64 if cpu_variant == "rv64" else 32

def _qemu_default_binary(cpu_variant):
    name = "qemu-system-riscv{}".format(_qemu_xlen(cpu_variant))
    repo_binary = os.path.abspath(os.path.join(
        os.path.dirname(__file__),
        "..", "..",
        "build", "qemu-litex", "bin",
        name,
    ))
    return repo_binary if os.path.exists(repo_binary) else name

def _qemu_machine_arg(soc, args):
    def region_origin(name, default=0):
        if name in soc.bus.regions:
            return soc.bus.regions[name].origin
        return soc.mem_map.get(name, default)

    def region_size(name, default=0):
        if name in soc.bus.regions:
            return soc.bus.regions[name].size
        return default

    cpu_variant = "rv32" if args.cpu_variant in [None, "standard"] else args.cpu_variant
    props = [
        "litex-sim",
        "xlen={}".format(_qemu_xlen(cpu_variant)),
        "bridge-host={}".format(args.qemu_bind),
        "bridge-port={}".format(args.qemu_port),
        "reset-addr=0x{:x}".format(getattr(soc.cpu, "reset_address", region_origin("rom"))),
        "rom-base=0x{:x}".format(region_origin("rom")),
        "sram-base=0x{:x}".format(region_origin("sram")),
        "main-ram-base=0x{:x}".format(region_origin("main_ram")),
        "csr-base=0x{:x}".format(region_origin("csr")),
        "csr-size=0x{:x}".format(region_size("csr")),
    ]
    if getattr(args, "qemu_shared_ram_enabled", False):
        props.append("memory-backend=litex_main_ram")
    return ",".join(props)

def _qemu_shared_ram_default_path(args):
    return os.path.abspath(os.path.join(args.output_dir, "qemu-main-ram.bin"))

def _qemu_shared_ram_path(args):
    return os.path.abspath(args.qemu_shared_ram_path or _qemu_shared_ram_default_path(args))

def _prepare_qemu_shared_ram_file(path, size, init_data=None, data_width=32):
    bytes_per_data = data_width//8

    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as f:
        f.truncate(size)

    if not init_data:
        return

    with open(path, "r+b") as f:
        for n, data in enumerate(init_data):
            offset = n*bytes_per_data
            if offset + bytes_per_data > size:
                raise ValueError("RAM init data is larger than QEMU shared RAM.")
            f.seek(offset)
            f.write(int(data).to_bytes(bytes_per_data, byteorder="little"))

def _qemu_command(builder, soc, args):
    cpu_variant = "rv32" if args.cpu_variant in [None, "standard"] else args.cpu_variant
    qemu_binary = args.qemu_binary or _qemu_default_binary(cpu_variant)
    qemu_ram_size = args.qemu_ram_size or args.integrated_main_ram_size or 64*1024*1024
    bios = args.qemu_firmware
    if bios is None:
        bios = args.rom_init or builder.get_bios_filename()

    cmd = [qemu_binary]
    if getattr(args, "qemu_shared_ram_enabled", False):
        cmd += [
            "-object",
            "memory-backend-file,id=litex_main_ram,mem-path={},size={},share=on".format(
                args.qemu_shared_ram_path_resolved,
                qemu_ram_size,
            ),
        ]
    cmd += [
        "-M", _qemu_machine_arg(soc, args),
        "-m", "{}B".format(qemu_ram_size),
        "-nographic",
        "-serial", "none",
        "-monitor", "none",
    ]
    if bios:
        cmd += ["-bios", bios]
    if args.qemu_kernel:
        cmd += ["-kernel", args.qemu_kernel]
    if args.qemu_dtb:
        cmd += ["-dtb", args.qemu_dtb]
    if args.qemu_initrd:
        cmd += ["-initrd", args.qemu_initrd]
    if args.qemu_append:
        cmd += ["-append", args.qemu_append]
    if args.qemu_extra_args:
        cmd += shlex.split(args.qemu_extra_args)
    return cmd

def _spawn_qemu_when_bridge_ready(cmd, host, port, timeout):
    waiter = r"""
import json
import os
import socket
import sys
import time

cmd = json.loads(sys.argv[1])
host = sys.argv[2]
port = int(sys.argv[3])
timeout = float(sys.argv[4])
deadline = None if timeout <= 0 else time.time() + timeout

while True:
    try:
        s = socket.create_connection((host, port), timeout=0.25)
        s.close()
        break
    except OSError:
        if deadline is not None and time.time() >= deadline:
            print("[litex_sim] ERROR: timed out waiting for qemu_wishbone bridge", file=sys.stderr)
            sys.exit(1)
        time.sleep(0.05)

print("[litex_sim] Starting QEMU: {}".format(" ".join(cmd)))
os.execvp(cmd[0], cmd)
"""
    return subprocess.Popen([
        sys.executable,
        "-c", waiter,
        json.dumps(cmd),
        host,
        str(port),
        str(timeout),
    ])

def sim_args(parser):
    # ROM / RAM.
    parser.add_argument("--rom-init",             default=None,            help="ROM init file (.bin or .json).")
    parser.add_argument("--ram-init",             default=None,            help="RAM init file (.bin or .json).")


    # UART.
    parser.add_argument("--uart-tcp",      action="store_true",            help="Use serial2tcp external module for UART.")
    parser.add_argument("--uart-tcp-port", type=int, default=1234,         help="TCP port for serial2tcp (default: 1234).")
    parser.add_argument("--uart-pty",      action="store_true",            help="Create a PTY bridged to the UART TCP port (requires socat).")
    parser.add_argument("--uart-pty-path", default="/tmp/litex_pty0",      help="Path for UART PTY (default: /tmp/litex_pty0).")

    # DRAM.
    parser.add_argument("--with-sdram",           action="store_true",     help="Enable SDRAM support.")
    parser.add_argument("--with-sdram-bist",      action="store_true",     help="Enable SDRAM BIST Generator/Checker modules.")
    parser.add_argument("--sdram-module",         default="MT48LC16M16",   help="Select SDRAM chip.")
    parser.add_argument("--sdram-data-width",     default=32,              help="Set SDRAM chip data width.")
    parser.add_argument("--sdram-init",           default=None,            help="SDRAM init file (.bin or .json).")
    parser.add_argument("--sdram-from-spd-dump",  default=None,            help="Generate SDRAM module based on data from SPD EEPROM dump.")
    parser.add_argument("--sdram-verbosity",      default=0,               help="Set SDRAM checker verbosity.")

    # Ethernet /Etherbone.
    parser.add_argument("--with-ethernet",      action="store_true",                                         help="Enable Ethernet support.")
    parser.add_argument("--ethernet-phy-model", default="sim", choices=SimSoC.supported_ethernet_phy_models, help="Ethernet PHY to simulate.")
    parser.add_argument("--with-etherbone",     action="store_true",                                         help="Enable Etherbone support.")
    parser.add_argument("--local-ip",           default="192.168.1.50",                                      help="Local IP address of SoC.")
    parser.add_argument("--remote-ip",          default="192.168.1.100",                                     help="Remote IP address of TFTP server.")

    # SDCard.
    parser.add_argument("--with-sdcard",          action="store_true",     help="Enable SDCard support.")

    # SPIFlash.
    parser.add_argument("--with-spi-flash",       action="store_true",     help="Enable SPI Flash (MMAPed).")
    parser.add_argument("--spi_flash-init",       default=None,            help="SPI Flash init file.")

    # I2C.
    parser.add_argument("--with-i2c",             action="store_true",     help="Enable I2C support.")

    # JTAG
    parser.add_argument("--with-jtagremote",      action="store_true", help="Enable jtagremote support")

    # QEMU co-simulation.
    parser.add_argument("--qemu-bind",         default="127.0.0.1", help="Bind address for the QEMU Wishbone bridge.")
    parser.add_argument("--qemu-port",         default=1235, type=int, help="TCP port for the QEMU Wishbone bridge.")
    parser.add_argument("--qemu-binary",       default=None, help="QEMU binary to launch (default: qemu-system-riscv32/64).")
    parser.add_argument("--qemu-firmware",     default=None, help="Firmware/BIOS passed to QEMU -bios; use 'none' to disable.")
    parser.add_argument("--qemu-kernel",       default=None, help="Kernel image passed to QEMU -kernel.")
    parser.add_argument("--qemu-dtb",          default=None, help="Device tree blob passed to QEMU -dtb.")
    parser.add_argument("--qemu-initrd",       default=None, help="Initrd image passed to QEMU -initrd.")
    parser.add_argument("--qemu-append",       default=None, help="Kernel command line passed to QEMU -append.")
    parser.add_argument("--qemu-ram-size",     default=None, type=auto_int, help="QEMU RAM size in bytes.")
    parser.add_argument("--qemu-shared-ram-path", default=None,        help="Shared RAM backing file used with QEMU integrated main RAM.")
    parser.add_argument("--qemu-no-shared-ram",   action="store_true", help="Disable shared QEMU/Verilator backing for integrated main RAM.")
    parser.add_argument("--qemu-extra-args",   default="", help="Extra arguments appended to the QEMU command line.")
    parser.add_argument("--qemu-wait-timeout", default=120.0, type=float, help="Seconds to wait for the QEMU bridge before launching QEMU; <= 0 waits forever.")
    parser.add_argument("--qemu-no-run",       action="store_true", help="Do not auto-launch QEMU when using --cpu-type=qemu.")

    # GPIO.
    parser.add_argument("--with-gpio",            action="store_true",     help="Enable Tristate GPIO (32 pins).")

    # Analyzer.
    parser.add_argument("--with-analyzer",        action="store_true",     help="Enable Analyzer support.")

    # Video.
    parser.add_argument("--with-video-framebuffer", action="store_true",   help="Enable Video Framebuffer.")
    parser.add_argument("--with-video-terminal",    action="store_true",   help="Enable Video Terminal.")
    parser.add_argument("--with-video-colorbars",   action="store_true",   help="Enable Video test pattern.")
    parser.add_argument("--video-vsync",            action="store_true",   help="Only render on frame vsync.")

    # Debug/Waveform.
    parser.add_argument("--sim-debug",            action="store_true",     help="Add simulation debugging modules.")
    parser.add_argument("--sim-speed",            action="store_true",     help="Report effective simulated sys_clk speed.")
    parser.add_argument("--sim-speed-interval",   default=5.0, type=float, help="Set simulation speed report interval in seconds.")
    parser.add_argument("--gtkwave-savefile",     action="store_true",     help="Generate GTKWave savefile.")
    parser.add_argument("--non-interactive",      action="store_true",     help="Run simulation without user input.")

def main():
    from litex.build.parser import LiteXArgumentParser
    parser = LiteXArgumentParser(description="LiteX SoC Simulation utility")
    parser.set_platform(SimPlatform)
    sim_args(parser)
    args = parser.parse_args()

    if args.with_sdram and args.integrated_main_ram_size is None and args.ram_init is not None:
        parser.error("--ram-init cannot be used with --with-sdram; use --sdram-init.")
    if args.sim_speed_interval <= 0:
        parser.error("--sim-speed-interval must be greater than 0.")

    soc_kwargs = soc_core_argdict(args)
    qemu_enabled = soc_kwargs.get("cpu_type", None) == "qemu"
    args.qemu_shared_ram_enabled = (
        qemu_enabled and
        bool(args.integrated_main_ram_size) and
        not args.qemu_no_shared_ram
    )
    args.qemu_shared_ram_path_resolved = _qemu_shared_ram_path(args)
    if args.qemu_shared_ram_enabled:
        if args.qemu_ram_size is not None and args.qemu_ram_size != args.integrated_main_ram_size:
            parser.error("--qemu-ram-size must match --integrated-main-ram-size when shared RAM is enabled.")
        soc_kwargs["integrated_main_ram_size"] = 0

    sys_clk_freq = int(1e6)
    sim_config           = SimConfig()
    sim_speed_interfaces = []
    sim_speed_console    = False
    sim_config.add_clocker("sys_clk", freq_hz=sys_clk_freq)

    # Configuration --------------------------------------------------------------------------------

    # UART.
    if soc_kwargs["uart_name"] == "serial":
        soc_kwargs["uart_name"] = "sim"
        # TCP-based UART bridge (serial2tcp).
        if args.uart_tcp or args.uart_pty:
            port = args.uart_tcp_port
            sim_config.add_module("serial2tcp", "serial", args={"port": port})
            # PTY.
            if args.uart_pty:
                port     = args.uart_tcp_port
                pty_path = args.uart_pty_path
                cmd = ["socat", f"pty,link={pty_path},raw,echo=0", f"tcp:127.0.0.1:{port},forever,interval=0.1"]
                try:
                    socat_proc = subprocess.Popen(cmd)
                    print(f"[litex_sim] UART PTY created at: {pty_path}")
                except FileNotFoundError:
                    print("[litex_sim] ERROR: 'socat' not found. Install socat or disable --uart-pty.")
                except Exception as e:
                    print(f"[litex_sim] ERROR: Failed to start socat for UART PTY: {e}")
        # Console (stdin/stdout) UART bridge (serial2console).
        else:
            sim_config.add_module("serial2console", "serial")
            sim_speed_interfaces.append("serial")
            sim_speed_console = True

    # QEMU co-simulation bridge.
    if qemu_enabled:
        sim_config.add_module("qemu_wishbone", ["qemu_wishbone", "qemu_irq"], clocks="sys_clk", args={
            "bind" : args.qemu_bind,
            "port" : args.qemu_port,
        })
        if args.qemu_shared_ram_enabled:
            sim_config.add_module("qemu_shared_ram", "qemu_shared_ram", clocks="sys_clk", args={
                "path" : args.qemu_shared_ram_path_resolved,
                "size" : args.integrated_main_ram_size,
            })
        if not args.qemu_no_run:
            qemu_variant = "rv32" if args.cpu_variant in [None, "standard"] else args.cpu_variant
            qemu_binary = args.qemu_binary or _qemu_default_binary(qemu_variant)
            if shutil.which(qemu_binary) is None and not os.path.exists(qemu_binary):
                parser.error("{} not found; install a patched QEMU or use --qemu-no-run.".format(qemu_binary))

    # Create config SoC that will be used to prepare/configure real one.
    conf_soc = SimSoC(**soc_kwargs)

    # ROM.
    if args.rom_init:
        soc_kwargs["integrated_rom_init"] = get_mem_data(args.rom_init,
            data_width = conf_soc.bus.data_width,
            endianness = conf_soc.cpu.endianness
        )

    # RAM / SDRAM.
    ram_boot_address = None
    main_ram_init = []
    soc_kwargs["integrated_main_ram_size"] = 0 if args.qemu_shared_ram_enabled else args.integrated_main_ram_size
    if args.integrated_main_ram_size:
        if args.ram_init is not None:
            main_ram_init = get_mem_data(args.ram_init,
                data_width = conf_soc.bus.data_width,
                endianness = conf_soc.cpu.endianness,
                offset     = conf_soc.mem_map["main_ram"]
            )
            if not args.qemu_shared_ram_enabled:
                soc_kwargs["integrated_main_ram_init"] = main_ram_init
            ram_boot_address = get_boot_address(args.ram_init)
    elif args.with_sdram:
        from litedram.modules   import parse_spd_hexdump

        soc_kwargs["sdram_module"]     = args.sdram_module
        soc_kwargs["sdram_data_width"] = int(args.sdram_data_width)
        soc_kwargs["sdram_verbosity"]  = int(args.sdram_verbosity)
        if args.sdram_from_spd_dump:
            soc_kwargs["sdram_spd_data"] = parse_spd_hexdump(args.sdram_from_spd_dump)
        if args.sdram_init is not None:
            soc_kwargs["sdram_init"] = get_mem_data(args.sdram_init,
                data_width = conf_soc.bus.data_width,
                endianness = conf_soc.cpu.endianness,
                offset     = conf_soc.mem_map["main_ram"]
            )
            ram_boot_address = get_boot_address(args.sdram_init)

    # Ethernet.
    if args.with_ethernet or args.with_etherbone:
        if args.ethernet_phy_model == "sim":
            sim_config.add_module("ethernet", "eth", args={"interface": "tap0", "ip": args.remote_ip})
        elif args.ethernet_phy_model == "xgmii":
            sim_config.add_module("xgmii_ethernet", "xgmii_eth", args={"interface": "tap0", "ip": args.remote_ip})
        elif args.ethernet_phy_model == "gmii":
            sim_config.add_module("gmii_ethernet", "gmii_eth", args={"interface": "tap0", "ip": args.remote_ip})
        else:
            raise ValueError("Unknown Ethernet PHY model: " + args.ethernet_phy_model)

    # I2C.
    if args.with_i2c:
        sim_config.add_module("spdeeprom", "i2c")

    # JTAG
    if args.with_jtagremote:
        sim_config.add_module("jtagremote", "jtag", args={'port': 44853})

    # Video.
    if args.with_video_framebuffer or args.with_video_terminal or args.with_video_colorbars:
        sim_config.add_module("video", "vga", args={"render_on_vsync": args.video_vsync})

    # Simulation speed reporting.
    if args.sim_speed:
        sim_config.add_module("sim_perf", sim_speed_interfaces, clocks="sys_clk", args={
            "freq_hz"    : sys_clk_freq,
            "interval_s" : args.sim_speed_interval,
            "console"    : sim_speed_console,
        })

    # SoC ------------------------------------------------------------------------------------------
    soc = SimSoC(
        with_sdram             = args.with_sdram,
        with_sdram_bist        = args.with_sdram_bist,
        with_ethernet          = args.with_ethernet,
        ethernet_phy_model     = args.ethernet_phy_model,
        ethernet_local_ip      = args.local_ip,
        ethernet_remote_ip     = args.remote_ip,
        with_etherbone         = args.with_etherbone,
        with_analyzer          = args.with_analyzer,
        with_i2c               = args.with_i2c,
        with_jtag              = args.with_jtagremote,
        with_sdcard            = args.with_sdcard,
        with_spi_flash         = args.with_spi_flash,
        with_gpio              = args.with_gpio,
        with_video_framebuffer = args.with_video_framebuffer,
        with_video_terminal    = args.with_video_terminal,
        with_video_colorbars   = args.with_video_colorbars,
        sim_debug              = args.sim_debug,
        trace_reset_on         = int(float(args.trace_start)) > 0 or int(float(args.trace_end)) > 0,
        spi_flash_init         = None if args.spi_flash_init is None else get_mem_data(args.spi_flash_init, endianness="big"),
        **soc_kwargs)
    if args.qemu_shared_ram_enabled:
        from litex.soc.cores.cpu.qemu.core import QEMUSharedRAM
        _prepare_qemu_shared_ram_file(
            path       = args.qemu_shared_ram_path_resolved,
            size       = args.integrated_main_ram_size,
            init_data  = main_ram_init,
            data_width = conf_soc.bus.data_width,
        )
        soc.add_module(name="main_ram", module=QEMUSharedRAM(soc.platform))
        soc.bus.add_slave(name="main_ram", slave=soc.main_ram.bus, region=SoCRegion(
            origin = soc.mem_map["main_ram"],
            size   = args.integrated_main_ram_size,
            mode   = "rwx",
        ))
        soc.integrated_main_ram_size = args.integrated_main_ram_size
    if ram_boot_address is not None:
        if ram_boot_address == 0:
            ram_boot_address = conf_soc.mem_map["main_ram"]
        soc.add_constant("ROM_BOOT_ADDRESS", ram_boot_address)
    if args.with_ethernet and (not args.with_etherbone): # FIXME: Remove.
        for i in range(4):
            soc.add_constant("LOCALIP{}".format(i+1), int(args.local_ip.split(".")[i]))
        for i in range(4):
            soc.add_constant("REMOTEIP{}".format(i+1), int(args.remote_ip.split(".")[i]))

    # Build/Run ------------------------------------------------------------------------------------
    qemu_proc = None

    def pre_run_callback(vns):
        nonlocal qemu_proc
        if args.trace:
            generate_gtkw_savefile(builder, vns, args.trace_fst)
        if qemu_enabled and not args.qemu_no_run:
            qemu_cmd = _qemu_command(builder, soc, args)
            print("[litex_sim] QEMU command: {}".format(" ".join(qemu_cmd)))
            qemu_proc = _spawn_qemu_when_bridge_ready(
                cmd     = qemu_cmd,
                host    = args.qemu_bind,
                port    = args.qemu_port,
                timeout = args.qemu_wait_timeout,
            )

    builder = Builder(soc, **parser.builder_argdict)
    try:
        builder.build(
            sim_config       = sim_config,
            interactive      = not args.non_interactive,
            video            = args.with_video_framebuffer or args.with_video_terminal or args.with_video_colorbars,
            pre_run_callback = pre_run_callback,
            **parser.toolchain_argdict,
        )
    finally:
        if qemu_proc is not None and qemu_proc.poll() is None:
            qemu_proc.terminate()

if __name__ == "__main__":
    main()
