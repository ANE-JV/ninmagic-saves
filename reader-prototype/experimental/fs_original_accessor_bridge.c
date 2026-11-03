/* SPDX-License-Identifier: GPL-2.0-or-later
 * Experimental integration sketch based on Atmosphere-NX emuMMC's FS
 * accessor/locking interface (Copyright 2019 m4xw, Atmosphere-NX, CTCaer).
 * No upstream files are changed. This is NOT a working/deployable KIP.
 */
#include "nmsv_probe.h"
#include <string.h>

#ifndef NMSV_ENABLE_UNVALIDATED_FS_PROBE
#define NMSV_ENABLE_UNVALIDATED_FS_PROBE 0
#endif

#if NMSV_ENABLE_UNVALIDATED_FS_PROBE
/* Only meaningful when linked INSIDE the matching injected emuMMC/FS image.
 * These must come from the exact upstream version being inspected. */
#include "emuMMC/emummc.h"

extern _sdmmc_accessor_nand sdmmc_accessor_nand;
extern volatile int *active_partition;
extern void *sd_mutex;
extern void *nand_mutex;

NmsvStatus nmsv_experimental_fs_gpt_header(void *context,
    unsigned char sector[NMSV_GPT_SECTOR_BYTES], uint64_t *driverResult) {
    sdmmc_accessor_t *accessor;
    void *dma = NULL;
    NmsvStatus status = NMSV_BACKEND_NOT_READY;
    (void)context;
    if (!sector || !driverResult) return NMSV_INVALID_ARGUMENT;
    *driverResult = 0;
    memset(sector, 0, NMSV_GPT_SECTOR_BYTES);
    if (!sdmmc_accessor_nand || !active_partition || !sd_mutex || !nand_mutex)
        return NMSV_BACKEND_NOT_READY;

    /* Preserve established lock order. Do not initialize/reset controllers,
     * attach DMA mappings, switch partitions or change global redirection. */
    mutex_lock_handler(FS_SDMMC_EMMC);
    if (*active_partition != FS_EMMC_PARTITION_GPP) goto out;
    accessor = sdmmc_accessor_nand();
    if (!accessor || !accessor->vtab || !accessor->vtab->read_write ||
        !accessor->parent) goto out;
    for (unsigned i = 0; i < 3; ++i) {
        const sdmmc_dma_buffer_t *candidate = &accessor->parent->dmaBuffers[i];
        if (candidate->device_addr_buffer && candidate->device_addr_buffer_masked &&
            candidate->device_addr_buffer_size >= NMSV_GPT_SECTOR_BYTES) {
            dma = candidate->device_addr_buffer;
            break;
        }
    }
    if (!dma) goto out;

    /* Fixed primary GPT sector only. The final argument 1 selects READ in
     * upstream FS accessor calls. This original accessor route still needs
     * hardware provenance/readiness validation: success is not proof. */
    *driverResult = accessor->vtab->read_write(accessor, 1, 1, dma,
        NMSV_GPT_SECTOR_BYTES, 1);
    if (*driverResult != 0) {
        status = NMSV_BACKEND_FAILED;
        goto out;
    }
    memcpy(sector, dma, NMSV_GPT_SECTOR_BYTES);
    status = NMSV_OK;
out:
    mutex_unlock_handler(FS_SDMMC_EMMC);
    return status;
}
#else
NmsvStatus nmsv_experimental_fs_gpt_header(void *context,
    unsigned char sector[NMSV_GPT_SECTOR_BYTES], uint64_t *driverResult) {
    (void)context;
    if (!sector || !driverResult) return NMSV_INVALID_ARGUMENT;
    memset(sector, 0, NMSV_GPT_SECTOR_BYTES);
    *driverResult = 0;
    return NMSV_BACKEND_UNAVAILABLE;
}
#endif
