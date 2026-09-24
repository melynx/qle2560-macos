# Tape control and tar streams

`build/qle-tape` uses macOS's SCSI task interface and currently selects only
endpoints beneath this project's QLE2560 driver. The driver must have attached,
initialized firmware, discovered the target and published its SCSI endpoint.

```sh
build/qle-tape list
build/qle-tape inquiry
build/qle-tape status
build/qle-tape position
build/qle-tape limits
```

When multiple devices are present, select one using `--device REGISTRY_ID`.
The CLI obtains exclusive access and prevents idle system sleep for the duration
of each device operation. Display blanking is still allowed. Forced sleep and
cable removal are not prevented.

## Backup and restore

Writing at the current tape position can replace existing data. Use a disposable
cartridge for testing. `rewind` positions at the beginning and does not erase;
the following write replaces data there.

```sh
set -o pipefail
build/qle-tape rewind
tar --format pax -cf - folder | build/qle-tape write
build/qle-tape safe-disconnect

# Resume without unplugging; allow target discovery to finish.
build/qle-tape reconnect
sleep 5
build/qle-tape status
build/qle-tape rewind
build/qle-tape read > restored.tar
mkdir -p restored
tar -xpf restored.tar -C restored
```

Check every exit status; never continue after a failed write or positioning
command assuming success. `reconnect` confirms only that resume was requested.
The delay is illustrative: readiness, not elapsed time, determines availability.

The CLI writes variable-length records, 8 MiB by default, with a shorter final
record as necessary. Successful EOF writes one terminating filemark. Reads use
8 MiB buffers and stop at a filemark or end of data. `--block-size BYTES` changes
the size up to 8 MiB; a read buffer must accommodate the largest recorded block.
The helper checks drive limits and selects variable-block mode.

Read the entire tape file to a local archive before extracting. An extractor
piped directly from the tape reader may exit at tar's end markers before the
reader consumes the tape filemark, causing a broken pipe and uncertain positioning.

Archive metadata support depends on the archive format, tar implementation,
restore filesystem, and user permissions. Verify a restore before relying on a
backup.

## Positioning and diagnostics

Commands: `rewind`, `filemark [count]`, `fsf [count]`, `bsf [count]`,
`fsr [count]`, `bsr [count]`, `eod`, `position`, `seek OBJECT`, `unload`,
`health`, `alerts`, `log-page PAGE`, `limits`, `inquiry`, `status`.
`seek` uses logical objects (records and filemarks), not byte offsets.
`--timeout-ms MILLISECONDS` overrides command timeouts. Run `--help` for syntax.
No erase/format command is implemented.

Reading TapeAlert consumes latched flags; preserve the output for diagnosis.
Some log interpretation is HP-specific. A failed data command's payload is not
accepted as valid merely because the device reports transferred bytes.

## Safe removal and failure recovery

After the writer finishes, `safe-disconnect` gates new commands, drains accepted
requests, issues non-immediate count-zero WRITE FILEMARKS to flush buffered data,
stops DMA and closes PCI. Only its successful reply confirms safe planned removal.
`flush` performs that tape flush without closing the adapter or adding a filemark.
A flush does not finish an incomplete tar archive.

Explicit disconnect has a 60-second drain/flush budget and requires a CLI timeout
of at least 90 seconds. Sleep uses a 10-second budget before acknowledging the
power transition. Expiry or command failure marks shutdown unsuccessful; sleep
is not blocked indefinitely. Writes are not automatically replayed. An application
may lose its SCSI connection and must report an interrupted stream as incomplete.

Unplugging without successful safe-disconnect is unexpected removal. Once the
path disappears, the driver cannot flush through it. Even if Ready returns after
reconnect, the final record may be unreadable and the filemark absent. Preserve
errors and verify the acknowledged prefix; do not blindly retry ambiguous writes.

See [compatibility and limitations](limitations.md).
