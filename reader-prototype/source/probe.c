/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "nmsv_probe.h"
#include <string.h>

static uint32_t read32(const unsigned char *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
        ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t read64(const unsigned char *p) {
    return (uint64_t)read32(p) | ((uint64_t)read32(p + 4) << 32);
}

uint32_t nmsv_crc32(const void *data, size_t size) {
    const unsigned char *p = (const unsigned char *)data;
    uint32_t crc = UINT32_MAX;
    for (size_t i = 0; i < size; ++i) {
        crc ^= p[i];
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ ((crc & 1u) ? UINT32_C(0xedb88320) : 0);
    }
    return ~crc;
}

NmsvStatus nmsv_parse_gpt_header(const unsigned char sector[NMSV_GPT_SECTOR_BYTES],
                                NmsvGptProbeReport *report) {
    unsigned char header[NMSV_GPT_SECTOR_BYTES];
    uint32_t bytes, storedCrc, count, entryBytes;
    uint64_t current, backup, first, last, entries, tableSectors;
    if (!sector || !report) return NMSV_INVALID_ARGUMENT;
    report->headerValid = 0;
    report->runtimeValidation = NMSV_VALIDATION_NOT_PERFORMED;
    if (memcmp(sector, "EFI PART", 8) != 0 ||
        read32(sector + 8) != UINT32_C(0x10000) || read32(sector + 20) != 0)
        return NMSV_INVALID_GPT_HEADER;
    bytes = read32(sector + 12);
    if (bytes < 92 || bytes > sizeof(header)) return NMSV_INVALID_GPT_HEADER;
    memcpy(header, sector, bytes);
    storedCrc = read32(header + 16);
    memset(header + 16, 0, 4);
    if (nmsv_crc32(header, bytes) != storedCrc) return NMSV_INVALID_GPT_HEADER;
    current = read64(sector + 24);
    backup = read64(sector + 32);
    first = read64(sector + 40);
    last = read64(sector + 48);
    entries = read64(sector + 72);
    count = read32(sector + 80);
    entryBytes = read32(sector + 84);
    /* Conservative probe limits, not a promise of support for every GPT. */
    if (current != 1 || count == 0 || count > 4096 || entryBytes < 128 ||
        entryBytes > 4096 || (entryBytes & (entryBytes - 1)) != 0 ||
        entries < 2 || first < entries || last < first || backup <= last)
        return NMSV_INVALID_GPT_HEADER;
    tableSectors = ((uint64_t)count * entryBytes + 511) / 512;
    if (tableSectors > first - entries) return NMSV_INVALID_GPT_HEADER;
    report->currentLba = current;
    report->backupLba = backup;
    report->firstUsableLba = first;
    report->lastUsableLba = last;
    report->entryTableLba = entries;
    report->entryCount = count;
    report->entryBytes = entryBytes;
    report->headerValid = 1;
    /* Disk GUID and matching/differing CRC are deliberately not used as
     * origin evidence: an emuMMC is commonly a clone of physical sysMMC. */
    return NMSV_OK;
}

static uint32_t backend_kind(const NmsvReaderState *state) {
    if (!state || state->backendKind > NMSV_BACKEND_FS_ORIGINAL_ACCESSOR)
        return NMSV_BACKEND_NONE;
    return state->backendKind;
}

NmsvStatus nmsv_dispatch(const NmsvReaderState *state, uint32_t command,
    const void *payload, size_t payloadBytes,
    void *response, size_t responseCapacity, size_t *responseBytes) {
    (void)payload;
    if (!responseBytes) return NMSV_INVALID_ARGUMENT;
    *responseBytes = 0;
    if (payloadBytes != 0 || !response) return NMSV_INVALID_ARGUMENT;
    if (command == NMSV_CMD_GET_HANDSHAKE) {
        const NmsvHandshake handshake = {
            NMSV_MAGIC, NMSV_PROTOCOL_VERSION, sizeof(NmsvHandshake),
            NMSV_CAP_DIAGNOSTIC_ONLY, 0, backend_kind(state),
            NMSV_VALIDATION_NOT_PERFORMED, 0
        };
        if (responseCapacity < sizeof(handshake)) return NMSV_BUFFER_TOO_SMALL;
        memcpy(response, &handshake, sizeof(handshake));
        *responseBytes = sizeof(handshake);
        return NMSV_OK;
    }
    if (command == NMSV_CMD_PROBE_GPT_HEADER) {
        unsigned char sector[NMSV_GPT_SECTOR_BYTES] = {0};
        NmsvGptProbeReport report;
        NmsvStatus status = NMSV_BACKEND_UNAVAILABLE;
        if (responseCapacity < sizeof(report)) return NMSV_BUFFER_TOO_SMALL;
        memset(&report, 0, sizeof(report));
        report.magic = NMSV_MAGIC;
        report.protocolVersion = NMSV_PROTOCOL_VERSION;
        report.structBytes = sizeof(report);
        report.backendKind = backend_kind(state);
        if (state && state->readGptHeader) {
            status = state->readGptHeader(state->context, sector, &report.driverResult);
            if (status == NMSV_OK) status = nmsv_parse_gpt_header(sector, &report);
        }
        report.status = (uint32_t)status;
        memcpy(response, &report, sizeof(report));
        *responseBytes = sizeof(report);
        return status;
    }
    /* No write, partition switch, arbitrary read, mount or import command. */
    return NMSV_UNSUPPORTED_COMMAND;
}
