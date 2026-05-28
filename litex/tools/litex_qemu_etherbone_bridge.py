#
# This file is part of LiteX.
#
# Copyright (c) 2026 Florent Kermarrec <florent@enjoy-digital.fr>
# SPDX-License-Identifier: BSD-2-Clause

import argparse
import socket
import struct

from litex.soc.integration.soc import auto_int

from litex.tools.remote.comm_udp import CommUDP
from litex.tools.remote.etherbone import EtherbonePacket, EtherboneRecord, EtherboneWrites

# Constants ----------------------------------------------------------------------------------------

QEMU_REQ_MAGIC = 0x3051584c # "LXQ0"
QEMU_RSP_MAGIC = 0x3052584c # "LXR0"
QEMU_VERSION   = 1
QEMU_MSG_SIZE  = 32

QEMU_OP_READ  = 0
QEMU_OP_WRITE = 1
QEMU_OP_IRQ   = 2

QEMU_STATUS_OK      = 0
QEMU_STATUS_ERR     = 1
QEMU_STATUS_BAD_REQ = 2

_qemu_req = struct.Struct("<IHHI4xQQ")
_qemu_rsp = struct.Struct("<IHHIIQ8x")


# Helpers ------------------------------------------------------------------------------------------

def _recv_exact(sock, size):
    data = bytearray()
    while len(data) < size:
        chunk = sock.recv(size - len(data))
        if not chunk:
            return None
        data += chunk
    return bytes(data)


def _resolve_register(comm, name):
    if name is None or not hasattr(comm, "regs"):
        return None
    return comm.regs.d.get(name)


def _word_addr(addr):
    return addr & ~0x3


def _word_sel(addr, size):
    return ((1 << size) - 1) << (addr & 0x3)


# Etherbone Target ---------------------------------------------------------------------------------

class EtherboneQEMUTarget:
    def __init__(self, comm,
        irq_addr             = None,
        reset_addr           = None,
        reset_clear_addr     = None,
        irq_register         = None,
        reset_register       = None,
        reset_clear_register = None,
        debug                = False):
        self.comm                 = comm
        self.irq_addr             = irq_addr
        self.reset_addr           = reset_addr
        self.reset_clear_addr     = reset_clear_addr
        self.irq_register         = _resolve_register(comm, irq_register)
        self.reset_register       = _resolve_register(comm, reset_register)
        self.reset_clear_register = _resolve_register(comm, reset_clear_register)
        self.debug                = debug

    # Bus Access -----------------------------------------------------------------------------------

    def read32(self, addr):
        return self.comm.read(addr) & 0xffffffff

    def write32(self, addr, data, sel=0xf):
        data &= 0xffffffff
        if sel == 0xf:
            self.comm.write(addr, data)
            return

        record = EtherboneRecord(addr_size=self.comm.addr_width//8)
        record.byte_enable = sel
        record.writes = EtherboneWrites(
            addr_size = self.comm.addr_width//8,
            base_addr = addr,
            datas     = [data],
        )

        packet = EtherbonePacket(self.comm.addr_width)
        packet.records = [record]
        packet.encode()
        self.comm.socket.sendto(packet.bytes, (self.comm.server, self.comm.port))

    def read(self, addr, size):
        data      = 0
        shift     = 0
        remaining = size

        while remaining:
            offset = addr & 0x3
            count  = min(4 - offset, remaining)
            word   = self.read32(_word_addr(addr))

            for n in range(count):
                byte = (word >> ((offset + n)*8)) & 0xff
                data |= byte << ((shift + n)*8)

            addr      += count
            remaining -= count
            shift     += count

        return data

    def write(self, addr, size, data):
        shift     = 0
        remaining = size

        while remaining:
            offset = addr & 0x3
            count  = min(4 - offset, remaining)
            word   = 0

            for n in range(count):
                byte = (data >> ((shift + n)*8)) & 0xff
                word |= byte << ((offset + n)*8)

            self.write32(_word_addr(addr), word, _word_sel(addr, count))

            addr      += count
            remaining -= count
            shift     += count

    # Status ---------------------------------------------------------------------------------------

    def irq_status(self):
        if self.irq_register is not None:
            return self.irq_register.read() & 0xffffffff
        if self.irq_addr is not None:
            return self.read32(self.irq_addr)
        return 0

    def reset_status(self):
        if self.reset_register is not None:
            reset = self.reset_register.read()
        elif self.reset_addr is not None:
            reset = self.read32(self.reset_addr)
        else:
            reset = 0

        if reset:
            if self.reset_clear_register is not None:
                self.reset_clear_register.write(1)
            elif self.reset_clear_addr is not None:
                self.write32(self.reset_clear_addr, 1)

        return reset & 1


# QEMU Bridge --------------------------------------------------------------------------------------

class QEMUEtherboneBridge:
    def __init__(self, target, bind="127.0.0.1", port=1235, debug=False):
        self.target = target
        self.bind   = bind
        self.port   = port
        self.debug  = debug

    def _response(self, status=QEMU_STATUS_OK, irq=0, data=0):
        return _qemu_rsp.pack(
            QEMU_RSP_MAGIC,
            QEMU_VERSION,
            status,
            irq & 0xffffffff,
            0,
            data & 0xffffffffffffffff,
        )

    def handle_message(self, msg):
        if len(msg) != QEMU_MSG_SIZE:
            return self._response(QEMU_STATUS_BAD_REQ)

        magic, version, op, size, addr, data = _qemu_req.unpack(msg)
        if magic != QEMU_REQ_MAGIC or version != QEMU_VERSION:
            return self._response(QEMU_STATUS_BAD_REQ)
        if op == QEMU_OP_IRQ:
            if size != 0:
                return self._response(QEMU_STATUS_BAD_REQ)
        elif op not in [QEMU_OP_READ, QEMU_OP_WRITE] or size not in [1, 2, 4, 8]:
            return self._response(QEMU_STATUS_BAD_REQ)

        try:
            if op == QEMU_OP_READ:
                data = self.target.read(addr, size)
            elif op == QEMU_OP_WRITE:
                self.target.write(addr, size, data)
                data = 0
            else:
                data = self.target.reset_status()
            irq = self.target.irq_status()
        except (OSError, socket.timeout):
            return self._response(QEMU_STATUS_ERR)

        return self._response(QEMU_STATUS_OK, irq, data)

    def serve_forever(self):
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as listener:
            listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            listener.bind((self.bind, self.port))
            listener.listen(1)
            print("[litex_qemu_etherbone_bridge] listening on {}:{}".format(self.bind, self.port))

            while True:
                client, addr = listener.accept()
                with client:
                    print("[litex_qemu_etherbone_bridge] QEMU connected from {}:{}".format(*addr))
                    while True:
                        msg = _recv_exact(client, QEMU_MSG_SIZE)
                        if msg is None:
                            break
                        client.sendall(self.handle_message(msg))
                print("[litex_qemu_etherbone_bridge] QEMU disconnected")


# Run ----------------------------------------------------------------------------------------------

def main():
    parser = argparse.ArgumentParser(description="LiteX QEMU to Etherbone bridge.")

    # QEMU.
    parser.add_argument("--qemu-bind", default="127.0.0.1", help="QEMU bridge bind address.")
    parser.add_argument("--qemu-port", default=1235, type=int, help="QEMU bridge TCP port.")

    # Etherbone.
    parser.add_argument("--etherbone-host", default="192.168.1.50", help="Etherbone target IP address.")
    parser.add_argument("--etherbone-port", default=1234, type=int, help="Etherbone target UDP port.")
    parser.add_argument("--addr-width",     default=32, type=int, choices=[32, 64], help="Etherbone address width.")
    parser.add_argument("--timeout",        default=1.0, type=float, help="Etherbone socket timeout.")
    parser.add_argument("--csr-csv",        default=None, help="CSR CSV used to resolve status registers.")
    parser.add_argument("--no-probe",       action="store_true", help="Skip Etherbone probe on startup.")

    # Optional QEMU status sources.
    parser.add_argument("--irq-addr",             default=None, type=auto_int, help="MMIO address returning IRQ bitmap.")
    parser.add_argument("--reset-addr",           default=None, type=auto_int, help="MMIO address returning latched reset.")
    parser.add_argument("--reset-clear-addr",     default=None, type=auto_int, help="MMIO address clearing latched reset.")
    parser.add_argument("--irq-register",         default="cpu_irq", help="CSR register returning IRQ bitmap.")
    parser.add_argument("--reset-register",       default="cpu_reset_status", help="CSR register returning latched reset.")
    parser.add_argument("--reset-clear-register", default="cpu_reset_clear", help="CSR register clearing latched reset.")
    parser.add_argument("--debug",                action="store_true", help="Enable debug output.")

    args = parser.parse_args()

    comm = CommUDP(
        server     = args.etherbone_host,
        port       = args.etherbone_port,
        csr_csv    = args.csr_csv,
        debug      = args.debug,
        timeout    = args.timeout,
        addr_width = args.addr_width,
    )
    comm.open(probe=not args.no_probe)

    target = EtherboneQEMUTarget(
        comm                 = comm,
        irq_addr             = args.irq_addr,
        reset_addr           = args.reset_addr,
        reset_clear_addr     = args.reset_clear_addr,
        irq_register         = args.irq_register,
        reset_register       = args.reset_register,
        reset_clear_register = args.reset_clear_register,
        debug                = args.debug,
    )
    bridge = QEMUEtherboneBridge(
        target = target,
        bind   = args.qemu_bind,
        port   = args.qemu_port,
        debug  = args.debug,
    )

    try:
        bridge.serve_forever()
    finally:
        comm.close()


if __name__ == "__main__":
    main()
