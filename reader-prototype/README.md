# Prototype: physical sysMMC reader from emuMMC

Status: **offline integration prototype, no working hardware service**.

This directory does not contain a deployable KIP, sysmodule, or physical-save
exporter. Nothing here has been installed on the console. Passing the host tests
does not establish that physical eMMC can be read from the running emuMMC.

## Implemented

- A fixed 32-byte versioned diagnostic handshake for the proposed `nmsv:r`
  service, command 0. The NRO may only use this command. It always reports
  diagnostic-only, no raw read capability, and no runtime validation.
- A portable command policy, not an IPC server. Its draft command 1 has no input
  payload and can only request the primary GPT header at GPP sector 1 through a
  callback. There is no arbitrary address, partition selector, write, mount,
  controller initialization or save import command.
- A bounded GPT-header parser with signature, size, CRC and layout validation.
  It validates neither the GPT entry array nor the contents of a NAND.
- An isolated experimental bridge source for the existing FS NAND accessor.
  The source is disabled by default and is not linked into the NRO or a KIP.
- Synthetic host tests. No fixture contains a console sector, key, identity or
  save. Even identical GPT data and a claimed physical backend leave provenance
  unverified: emuMMC often clones the original disk's GPT.

Run the offline checks from PowerShell:

```powershell
./reader-prototype/run-tests.ps1
```

GCC is required. The build directory is ignored. The test runner compiles only
the disabled hardware bridge. It does not test the enabled integration branch.

## Proposed integration, with evidence

The inspected Atmosphere release is **1.12.1**, commit
`6b0c077b4c608a3a2ee712b8dc35fee87ceb62e8`. The research target is HOS
23.0.1 and a Hekate `pkg3=atmosphere/package3` profile with emuMMC forced. This is
not equivalent to a fusee chainload; boot integration must match that profile.

The normal path is NRO -> libnx FS -> FS NAND accessor wrapper -> emuMMC on SD.
Changing a file-browser label to "original" would still read emuMMC.

The research path is:

1. Keep all existing emuMMC redirection unchanged.
2. Add an explicitly separate read-only probe inside the injected FS component,
   where the original NAND accessor and FS's locks/DMA buffers are available.
3. Give that probe a narrow IPC interface, then expose the workflow in the NRO.
4. Only after proving physical reads, add local BIS decryption, FAT and saveFS
   parsing. Export files from the source; import into emuMMC through normal FS,
   after a verified destination backup and explicit user confirmation.

Primary-code evidence:

- [emuMMC wrapper and original controller/accessor](https://github.com/Atmosphere-NX/Atmosphere/blob/6b0c077b4c608a3a2ee712b8dc35fee87ceb62e8/emummc/source/emuMMC/emummc.c):
  `sdmmc_accessor_nand`, `mutex_lock_handler`, `sdmmc_wrapper_controller_open`,
  `sdmmc_wrapper_read`. The original accessor is retained; ordinary NAND reads
  are redirected by the wrapper. This motivates the hypothesis, not proof that
  a new thread can safely use the original accessor.
- [DMA and SDMMC4 initialization](https://github.com/Atmosphere-NX/Atmosphere/blob/6b0c077b4c608a3a2ee712b8dc35fee87ceb62e8/emummc/source/emmc/sdmmc.c):
  `sdmmc_calculate_dma_addr`, `sdmmc_storage_init_mmc`. A second independent driver
  would compete for existing controller/power/DMA state; it is not implemented.
- [FS hooks and firmware-specific pointers](https://github.com/Atmosphere-NX/Atmosphere/blob/6b0c077b4c608a3a2ee712b8dc35fee87ceb62e8/emummc/source/main.c):
  `setup_hooks`, `populate_function_pointers`, `get_fs_offsets`. An extension
  must use the matching FS revision, not inferred offsets.
- [FS kernel capabilities used by emuMMC](https://github.com/Atmosphere-NX/Atmosphere/blob/6b0c077b4c608a3a2ee712b8dc35fee87ceb62e8/emummc/emummc.json):
  the existing list does not include `svcManageNamedPort`. A private named-port
  design therefore requires an explicit capability/integration change. An SM
  registered-service design instead needs reviewed service access controls;
  registering `nmsv:r` cannot be assumed to work from FS.
- [fusee KIP injection](https://github.com/Atmosphere-NX/Atmosphere/blob/6b0c077b4c608a3a2ee712b8dc35fee87ceb62e8/fusee/program/source/fusee_stratosphere.cpp):
  fusee loads its emuMMC KIP and copies capabilities into FS.
- [Hekate KIPM injection](https://github.com/CTCaer/hekate/blob/master/bootloader/hos/pkg2.c):
  `_kipm_inject` patches FS capabilities and code; emuMMC uses
  `bootloader/sys/emummc.kipm` in this upstream path. The exact installed Hekate
  build must be checked before any experimental integration is prepared.

The experimental bridge only expresses the smallest proposed operation: hold
the existing locks, require the logical GPP selection and an existing DMA
buffer, then invoke the original accessor for sector 1 in read mode. It never
opens/resets the controller or switches partitions. **The logical partition and
nonnull pointers do not prove the physical controller is ready.** Its enabled
branch has not been built against a complete pinned FS injection build or run
on hardware. No IPC thread calls it.

## Explicitly unimplemented / unresolved

- IPC registration, endpoint permissions, worker thread and lifecycle.
- Boot-path-specific KIP/KIPM generation and injection; rollback/recovery design.
- Independent physical-source provenance and on-device access verification.
- Coordination with FS initialization, physical partition state, power changes,
  sleep/wake, DMA ownership, timeouts and unexpected driver faults.
- Physical capacity/GPT entry verification and a restricted USER-partition view.
- Local BIS decryption/key access, FAT, saveFS and account/title identification.
  The app must not ask the user to send keys, save data or NAND images to a server.
- Export/import transactions, destination backups, validation and UI workflow.
- Compatibility beyond a specifically validated firmware and boot stack.

Existing implementations can inform later work: [Hekate BIS reader](https://github.com/CTCaer/hekate/blob/master/bdk/storage/nx_emmc_bis.c)
and [nxdumptool saveFS parser](https://github.com/DarkMatterCore/nxdumptool/blob/rewrite/source/core/save.c).
Their existence does not remove the runtime physical-access problem. They have
not been integrated here.

## Gates for any future hardware experiment

1. Review/build the complete pinned injection and endpoint offline. Keep an
   independent backup and a reviewed recovery path before replacing a component
   used for every NAND access. Do not deploy this sketch by toggling its macro.
2. Verify the probe's call path and readiness on the actual boot stack. Read only
   fixed GPT sectors initially. Repeated successful reads are necessary but do
   not establish physical origin, since both NANDs may have identical GPTs.
3. Establish origin through the instrumented original-accessor path plus an
   independent, known difference in source contents. Later, an independently
   verified original save inventory is a useful cross-check.
4. Verify that the new service has no data-write operation and that existing
   emuMMC access, error handling and suspend/resume remain correct.
5. Only then implement source save parsing/export and destination import.

This route needs a boot-loaded FS extension and an initial restart. If proven,
ordinary transfers could be initiated completely from the NRO without entering
the original OS. There is presently **no demonstrated NRO-only runtime step**
that provides this access without changing a system component. Runtime patching
FS or taking direct hardware ownership would not make the experiment safer and
is not provided here. No invisibility or ban-immunity guarantee is possible.

## License

Files in this directory are GPL-2.0-or-later. The experimental bridge follows
Atmosphere emuMMC's accessor/locking interface and retains attribution to m4xw,
Atmosphere-NX and CTCaer. `COPYING` contains GPL version 2 from that project's
license file. No Nintendo code, binaries, keys or console contents are included.
