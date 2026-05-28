#
# This file is part of LiteX.
#
# Copyright (c) 2026 Florent Kermarrec <florent@enjoy-digital.fr>
# SPDX-License-Identifier: BSD-2-Clause

import os
import sys
import tempfile
import unittest
import subprocess

from litex.tools.litex_qemu_etherbone_bridge import (
    QEMU_REQ_MAGIC,
    QEMU_RSP_MAGIC,
    QEMU_VERSION,
    QEMU_OP_READ,
    QEMU_OP_WRITE,
    QEMU_OP_IRQ,
    QEMU_STATUS_OK,
    QEMU_STATUS_BAD_REQ,
    QEMUEtherboneBridge,
    EtherboneQEMUTarget,
    _qemu_req,
    _qemu_rsp,
)


class MemoryEtherboneTarget(EtherboneQEMUTarget):
    def __init__(self):
        self.mem = {}
        self.irq_value = 0
        self.reset_value = 0
        self.reset_cleared = False

    def read32(self, addr):
        return self.mem.get(addr, 0)

    def write32(self, addr, data, sel=0xf):
        word = self.mem.get(addr, 0)
        for n in range(4):
            if sel & (1 << n):
                byte = (data >> (8*n)) & 0xff
                word &= ~(0xff << (8*n))
                word |= byte << (8*n)
        self.mem[addr] = word

    def irq_status(self):
        return self.irq_value

    def reset_status(self):
        reset = self.reset_value
        if reset:
            self.reset_cleared = True
            self.reset_value = 0
        return reset


def qemu_request(op, size=0, addr=0, data=0, magic=QEMU_REQ_MAGIC, version=QEMU_VERSION):
    return _qemu_req.pack(magic, version, op, size, addr, data)


def qemu_response(response):
    magic, version, status, irq, reserved, data = _qemu_rsp.unpack(response)
    return {
        "magic"    : magic,
        "version"  : version,
        "status"   : status,
        "irq"      : irq,
        "reserved" : reserved,
        "data"     : data,
    }


class TestQEMUEtherboneBridge(unittest.TestCase):
    def test_unaligned_read_reassembles_little_endian_data(self):
        target = MemoryEtherboneTarget()
        target.mem[0x1000] = 0x11223344
        target.mem[0x1004] = 0x55667788

        bridge = QEMUEtherboneBridge(target)
        rsp = qemu_response(bridge.handle_message(qemu_request(QEMU_OP_READ, 4, 0x1002)))

        self.assertEqual(rsp["magic"], QEMU_RSP_MAGIC)
        self.assertEqual(rsp["version"], QEMU_VERSION)
        self.assertEqual(rsp["status"], QEMU_STATUS_OK)
        self.assertEqual(rsp["data"], 0x77881122)

    def test_unaligned_write_updates_only_selected_byte_lanes(self):
        target = MemoryEtherboneTarget()
        target.mem[0x1000] = 0x11223344
        target.mem[0x1004] = 0x55667788

        bridge = QEMUEtherboneBridge(target)
        rsp = qemu_response(bridge.handle_message(qemu_request(QEMU_OP_WRITE, 4, 0x1002, 0xaabbccdd)))

        self.assertEqual(rsp["status"], QEMU_STATUS_OK)
        self.assertEqual(target.mem[0x1000], 0xccdd3344)
        self.assertEqual(target.mem[0x1004], 0x5566aabb)

    def test_irq_poll_returns_irq_and_latched_reset_status(self):
        target = MemoryEtherboneTarget()
        target.irq_value = 0x6
        target.reset_value = 1

        bridge = QEMUEtherboneBridge(target)
        rsp = qemu_response(bridge.handle_message(qemu_request(QEMU_OP_IRQ)))

        self.assertEqual(rsp["status"], QEMU_STATUS_OK)
        self.assertEqual(rsp["irq"], 0x6)
        self.assertEqual(rsp["data"], 1)
        self.assertTrue(target.reset_cleared)

    def test_bad_request_is_rejected(self):
        target = MemoryEtherboneTarget()
        bridge = QEMUEtherboneBridge(target)
        rsp = qemu_response(bridge.handle_message(qemu_request(QEMU_OP_READ, 3, 0x1000)))

        self.assertEqual(rsp["status"], QEMU_STATUS_BAD_REQ)

    def test_qemu_remote_etherbone_sim_elaborates(self):
        with tempfile.TemporaryDirectory() as tmp_dir:
            csr_csv = os.path.join(tmp_dir, "csr.csv")
            subprocess.check_call([
                sys.executable, "-m", "litex.tools.litex_sim",
                "--cpu-type=qemu_remote",
                "--cpu-variant=rv64",
                "--with-etherbone",
                "--soc-csv={}".format(csr_csv),
                "--output-dir={}".format(tmp_dir),
                "--no-compile",
            ])

            with open(csr_csv, encoding="utf-8") as f:
                csr = f.read()

            self.assertIn("csr_register,cpu_irq,", csr)
            self.assertIn("csr_register,cpu_reset_status,", csr)
            self.assertIn("csr_register,cpu_reset_clear,", csr)


if __name__ == "__main__":
    unittest.main()
