/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef NMSV_READER_PROTOCOL_H
#define NMSV_READER_PROTOCOL_H

#include <stdint.h>

#define NMSV_SERVICE_NAME "nmsv:r"
#define NMSV_MAGIC UINT32_C(0x56534d4e) /* Little-endian bytes: NMSV. */
#define NMSV_PROTOCOL_VERSION UINT32_C(1)
#define NMSV_CMD_GET_HANDSHAKE UINT32_C(0)
/* Draft, offline-only command. The NRO diagnostic client MUST NOT call it. */
#define NMSV_CMD_PROBE_GPT_HEADER UINT32_C(1)

#define NMSV_CAP_DIAGNOSTIC_ONLY (UINT32_C(1) << 0)
#define NMSV_CAP_PHYSICAL_RAW_READ_AVAILABLE (UINT32_C(1) << 1)
#define NMSV_BACKEND_NONE UINT32_C(0)
#define NMSV_BACKEND_HOST_FIXTURE UINT32_C(1)
#define NMSV_BACKEND_FS_ORIGINAL_ACCESSOR UINT32_C(2)
#define NMSV_VALIDATION_NOT_PERFORMED UINT32_C(0)

/* Exactly 32 bytes, little endian. All current implementations report
 * NOT_PERFORMED. A handshake is not evidence that sysMMC was read. */
typedef struct NmsvHandshake {
    uint32_t magic;
    uint32_t protocolVersion;
    uint32_t structBytes;
    uint32_t capabilityFlags;
    uint32_t maxReadBytes;
    uint32_t backendKind;
    uint32_t runtimeValidation;
    uint32_t reserved0;
} NmsvHandshake;

#if defined(__cplusplus)
static_assert(sizeof(NmsvHandshake) == 32, "NMSV handshake ABI");
#else
_Static_assert(sizeof(NmsvHandshake) == 32, "NMSV handshake ABI");
#endif

#endif
