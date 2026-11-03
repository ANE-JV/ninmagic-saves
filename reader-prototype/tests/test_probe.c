/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "nmsv_probe.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

typedef struct Fixture {
    unsigned char sector[512];
    unsigned calls;
    NmsvStatus result;
} Fixture;

static void put32(unsigned char *p, uint32_t n) {
    for (unsigned i = 0; i < 4; ++i) p[i] = (unsigned char)(n >> (8 * i));
}
static void put64(unsigned char *p, uint64_t n) {
    put32(p, (uint32_t)n);
    put32(p + 4, (uint32_t)(n >> 32));
}
static void fix_crc(unsigned char sector[512]) {
    put32(sector + 16, 0);
    put32(sector + 16, nmsv_crc32(sector, 92));
}
static Fixture synthetic_gpt(void) {
    Fixture fixture;
    memset(&fixture, 0, sizeof(fixture));
    memcpy(fixture.sector, "EFI PART", 8);
    put32(fixture.sector + 8, 0x10000);
    put32(fixture.sector + 12, 92);
    put64(fixture.sector + 24, 1);
    put64(fixture.sector + 32, 99999);
    put64(fixture.sector + 40, 34);
    put64(fixture.sector + 48, 99966);
    put64(fixture.sector + 72, 2);
    put32(fixture.sector + 80, 128);
    put32(fixture.sector + 84, 128);
    fix_crc(fixture.sector);
    return fixture;
}
static NmsvStatus fixture_read(void *context, unsigned char sector[512], uint64_t *rc) {
    Fixture *fixture = (Fixture *)context;
    fixture->calls++;
    *rc = fixture->result == NMSV_OK ? 0 : 0x1234;
    if (fixture->result == NMSV_OK) memcpy(sector, fixture->sector, 512);
    return fixture->result;
}

static void test_abi_and_unavailable(void) {
    NmsvHandshake info;
    NmsvGptProbeReport report;
    unsigned char sector[512];
    size_t bytes;
    uint64_t rc = UINT64_MAX;
    assert(nmsv_crc32("123456789", 9) == UINT32_C(0xcbf43926));
    assert(nmsv_dispatch(NULL, 0, NULL, 0, &info, sizeof(info), &bytes) == NMSV_OK);
    assert(bytes == 32 && info.magic == NMSV_MAGIC && info.structBytes == 32);
    assert(info.capabilityFlags == NMSV_CAP_DIAGNOSTIC_ONLY);
    assert(info.maxReadBytes == 0 && info.backendKind == NMSV_BACKEND_NONE);
    assert(info.runtimeValidation == 0 && info.reserved0 == 0);
    assert(nmsv_dispatch(NULL, 1, NULL, 0, &report, sizeof(report), &bytes)
        == NMSV_BACKEND_UNAVAILABLE);
    assert(bytes == sizeof(report) && report.status == NMSV_BACKEND_UNAVAILABLE);
    assert(report.headerValid == 0 && report.runtimeValidation == 0);
    memset(sector, 0xa5, sizeof(sector));
    assert(nmsv_experimental_fs_gpt_header(NULL, sector, &rc) == NMSV_BACKEND_UNAVAILABLE);
    assert(rc == 0);
    for (size_t i = 0; i < sizeof(sector); ++i) assert(sector[i] == 0);
}

static void test_request_boundaries(void) {
    Fixture fixture = synthetic_gpt();
    NmsvReaderState state = { fixture_read, &fixture, NMSV_BACKEND_HOST_FIXTURE };
    NmsvGptProbeReport report;
    size_t bytes = 99;
    uint64_t arbitraryAddress = UINT64_MAX;
    assert(nmsv_dispatch(&state, 1, &arbitraryAddress, sizeof(arbitraryAddress),
        &report, sizeof(report), &bytes) == NMSV_INVALID_ARGUMENT);
    assert(bytes == 0 && fixture.calls == 0);
    assert(nmsv_dispatch(&state, 1, NULL, 0, &report, sizeof(report) - 1, &bytes)
        == NMSV_BUFFER_TOO_SMALL);
    assert(fixture.calls == 0 && bytes == 0);
    assert(nmsv_dispatch(&state, UINT32_MAX, NULL, 0, &report, sizeof(report), &bytes)
        == NMSV_UNSUPPORTED_COMMAND);
    assert(fixture.calls == 0 && bytes == 0);
    assert(nmsv_dispatch(&state, 1, NULL, 0, NULL, 0, &bytes) == NMSV_INVALID_ARGUMENT);
    assert(nmsv_dispatch(&state, 1, NULL, 0, &report, sizeof(report), NULL)
        == NMSV_INVALID_ARGUMENT);
    assert(fixture.calls == 0);
}

static void test_header_not_provenance(void) {
    Fixture fixture = synthetic_gpt();
    Fixture identicalClone = fixture;
    NmsvReaderState state = { fixture_read, &fixture, NMSV_BACKEND_HOST_FIXTURE };
    NmsvGptProbeReport first, second;
    size_t bytes;
    assert(nmsv_dispatch(&state, 1, NULL, 0, &first, sizeof(first), &bytes) == NMSV_OK);
    assert(fixture.calls == 1 && first.headerValid == 1);
    assert(first.runtimeValidation == 0 && first.backendKind == NMSV_BACKEND_HOST_FIXTURE);
    assert(first.currentLba == 1 && first.entryTableLba == 2 && first.entryCount == 128);
    state.context = &identicalClone;
    assert(nmsv_dispatch(&state, 1, NULL, 0, &second, sizeof(second), &bytes) == NMSV_OK);
    assert(memcmp(&first, &second, sizeof(first)) == 0);
    /* Even a backend claiming FS original accessor cannot self-certify. */
    state.backendKind = NMSV_BACKEND_FS_ORIGINAL_ACCESSOR;
    assert(nmsv_dispatch(&state, 1, NULL, 0, &second, sizeof(second), &bytes) == NMSV_OK);
    assert(second.headerValid == 1 && second.runtimeValidation == 0);
}

static void test_rejected_header_and_driver_error(void) {
    Fixture fixture = synthetic_gpt();
    NmsvReaderState state = { fixture_read, &fixture, NMSV_BACKEND_HOST_FIXTURE };
    NmsvGptProbeReport report;
    size_t bytes;
    fixture.sector[56] ^= 1; /* Header integrity failure. */
    assert(nmsv_dispatch(&state, 1, NULL, 0, &report, sizeof(report), &bytes)
        == NMSV_INVALID_GPT_HEADER);
    assert(report.headerValid == 0 && report.runtimeValidation == 0);
    fixture = synthetic_gpt();
    put32(fixture.sector + 12, 513);
    assert(nmsv_dispatch(&state, 1, NULL, 0, &report, sizeof(report), &bytes)
        == NMSV_INVALID_GPT_HEADER);
    fixture = synthetic_gpt();
    put64(fixture.sector + 72, UINT64_MAX);
    fix_crc(fixture.sector);
    assert(nmsv_dispatch(&state, 1, NULL, 0, &report, sizeof(report), &bytes)
        == NMSV_INVALID_GPT_HEADER);
    fixture = synthetic_gpt();
    put32(fixture.sector + 80, UINT32_MAX);
    fix_crc(fixture.sector);
    assert(nmsv_dispatch(&state, 1, NULL, 0, &report, sizeof(report), &bytes)
        == NMSV_INVALID_GPT_HEADER);
    fixture = synthetic_gpt();
    put64(fixture.sector + 40, 33); /* Partition array overlaps usable region. */
    fix_crc(fixture.sector);
    assert(nmsv_dispatch(&state, 1, NULL, 0, &report, sizeof(report), &bytes)
        == NMSV_INVALID_GPT_HEADER);
    fixture = synthetic_gpt();
    fixture.result = NMSV_BACKEND_FAILED;
    assert(nmsv_dispatch(&state, 1, NULL, 0, &report, sizeof(report), &bytes)
        == NMSV_BACKEND_FAILED);
    assert(report.driverResult == 0x1234 && report.headerValid == 0);
}

int main(void) {
    test_abi_and_unavailable();
    test_request_boundaries();
    test_header_not_provenance();
    test_rejected_header_and_driver_error();
    puts("PASS: offline request/GPT policy; hardware reader NOT implemented/validated.");
    return 0;
}
