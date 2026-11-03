/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef NMSV_PROBE_H
#define NMSV_PROBE_H

#include "nmsv_reader_protocol.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NMSV_GPT_SECTOR_BYTES 512u

typedef enum NmsvStatus {
    NMSV_OK = 0,
    NMSV_INVALID_ARGUMENT = 1,
    NMSV_UNSUPPORTED_COMMAND = 2,
    NMSV_BUFFER_TOO_SMALL = 3,
    NMSV_BACKEND_UNAVAILABLE = 4,
    NMSV_BACKEND_FAILED = 5,
    NMSV_INVALID_GPT_HEADER = 6,
    NMSV_BACKEND_NOT_READY = 7
} NmsvStatus;

/* Deliberately no address/count/partition or write operation. An eventual
 * backend may return only primary GPP GPT sector 1 into this fixed buffer. */
typedef NmsvStatus (*NmsvReadGptHeader)(void *context,
    unsigned char sector[NMSV_GPT_SECTOR_BYTES], uint64_t *driverResult);

typedef struct NmsvReaderState {
    NmsvReadGptHeader readGptHeader;
    void *context;
    uint32_t backendKind;
} NmsvReaderState;

typedef struct NmsvGptProbeReport {
    uint32_t magic;
    uint32_t protocolVersion;
    uint32_t structBytes;
    uint32_t status;
    uint32_t backendKind;
    uint32_t runtimeValidation; /* Always NOT_PERFORMED: GPT is not provenance. */
    uint32_t headerValid;       /* Only structure and primary-header CRC. */
    uint32_t reserved0;
    uint64_t driverResult;
    uint64_t currentLba;
    uint64_t backupLba;
    uint64_t firstUsableLba;
    uint64_t lastUsableLba;
    uint64_t entryTableLba;
    uint32_t entryCount;
    uint32_t entryBytes;
} NmsvGptProbeReport;

/* This is an offline dispatch policy, NOT an IPC server. Commands take no
 * payload, have exact response sizes, and cannot request arbitrary sectors. */
NmsvStatus nmsv_dispatch(const NmsvReaderState *state, uint32_t command,
    const void *payload, size_t payloadBytes,
    void *response, size_t responseCapacity, size_t *responseBytes);

NmsvStatus nmsv_parse_gpt_header(const unsigned char sector[NMSV_GPT_SECTOR_BYTES],
    NmsvGptProbeReport *report);
uint32_t nmsv_crc32(const void *data, size_t size);

/* Hardware bridge is unavailable by default; it is not part of any NRO/KIP. */
NmsvStatus nmsv_experimental_fs_gpt_header(void *context,
    unsigned char sector[NMSV_GPT_SECTOR_BYTES], uint64_t *driverResult);

#ifdef __cplusplus
}
static_assert(sizeof(NmsvGptProbeReport) == 88, "NMSV probe ABI");
#else
_Static_assert(sizeof(NmsvGptProbeReport) == 88, "NMSV probe ABI");
#endif
#endif
