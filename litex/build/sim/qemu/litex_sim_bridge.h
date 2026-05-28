/*
 * LiteX SIM QEMU co-simulation bridge protocol.
 *
 * This header is intentionally standalone so it can be copied into a QEMU
 * checkout when adding the litex-sim RISC-V machine/device.
 */

#ifndef LITEX_SIM_QEMU_BRIDGE_H
#define LITEX_SIM_QEMU_BRIDGE_H

#include <stdint.h>

#define LITEX_SIM_QEMU_REQ_MAGIC 0x3051584c /* "LXQ0" */
#define LITEX_SIM_QEMU_RSP_MAGIC 0x3052584c /* "LXR0" */
#define LITEX_SIM_QEMU_VERSION   1
#define LITEX_SIM_QEMU_MSG_SIZE  32

enum litex_sim_qemu_op {
    LITEX_SIM_QEMU_OP_READ  = 0,
    LITEX_SIM_QEMU_OP_WRITE = 1,
    /* Status poll: response data bit 0 reports a latched CPU reset. */
    LITEX_SIM_QEMU_OP_IRQ   = 2,
};

enum litex_sim_qemu_status {
    LITEX_SIM_QEMU_STATUS_OK      = 0,
    LITEX_SIM_QEMU_STATUS_ERR     = 1,
    LITEX_SIM_QEMU_STATUS_BAD_REQ = 2,
};

#endif /* LITEX_SIM_QEMU_BRIDGE_H */
