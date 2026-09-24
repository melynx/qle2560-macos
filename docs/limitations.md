# Compatibility and limitations

This is an experimental driver for the QLogic QLE2560 on Apple silicon and
macOS 26. Hardware validation is limited to a Thunderbolt PCIe enclosure and a
directly connected HP Ultrium 6-SCSI LTO-6 drive, firmware 25LS, at 8 Gb/s.
Other adapters, tape drives, Fibre Channel switches, and backup applications
are not qualified.

The driver matches PCI vendor/device `1077:2532` and subsystem `1077:015c`.
The tape utility selects devices attached through this driver. It does not
provide Linux-compatible `/dev/st0` or `/dev/nst0` character devices.

## Data integrity and recovery

- Use a disposable cartridge for initial evaluation and verify readback.
- Unexpected removal during a write can leave an unreadable final record and
  omit the terminating filemark. A Ready response after reconnect does not
  establish that the preceding backup is intact.
- Failed or interrupted writes are not replayed automatically. Applications
  must treat interrupted streams as incomplete.
- Sleep can close an application's SCSI connection. Automatic backup
  continuation after wake is not supported.
- Planned removal requires a successful `safe-disconnect` response. A failed
  flush or shutdown deadline means buffered data may not have reached tape.
- Recovery checks do not establish durability under power loss or every
  hardware, firmware, or DMA failure.

Full-capacity readback has been exercised on a prior build. It has not been
repeated for every revision. Protocol and state-machine tests do not emulate
the full adapter, tape mechanism, or macOS kernel DMA handling.

## Archives

Tape records are limited to 8 MiB by this implementation. Read buffers must be
large enough for the recorded block size. An archive and its final filemark must
both be read before proceeding to another tape file.

Metadata restoration depends on tar options, permissions, and the destination
filesystem. ACLs, cross-user ownership restoration, sparse allocation, and
cross-platform extraction are not qualified. See [usage](tape-usage.md).
