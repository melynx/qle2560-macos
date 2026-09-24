# QLE2560 for macOS

Experimental native DriverKit Fibre Channel initiator for the QLogic QLE2560,
with a tape-control CLI. Developed and tested on an Apple M4 Pro running macOS 26,
with a Thunderbolt PCIe enclosure and one directly connected HP Ultrium 6-SCSI
LTO-6 drive (firmware 25LS, 8 Gb FC).

## Status

Version **0.19.0** implements firmware loading, NVRAM/WWN validation, DMA queues,
interrupts with polling fallback, direct FC discovery, SCSI command transport,
sense/residual handling, task timeout/abort, and coordinated shutdown/recovery.
Matching is currently restricted to PCI `1077:2532`, subsystem `1077:015c`.

`qle-tape` provides tape read/write streams, filemarks, positioning, diagnostics,
and software safe-disconnect/reconnect. It currently discovers only devices
attached through this driver. There is no `/dev/st0` or `/dev/nst0` interface.

See [compatibility and limitations](docs/limitations.md) before using the driver.

**Use scratch media while evaluating this driver.** Unexpected removal during
writing can leave an unreadable terminal record and an incomplete backup.
Ready status alone does not prove data integrity. Failed writes are never
replayed automatically. This is experimental software, not a broadly qualified
storage driver; other adapters, drives, FC switches and backup applications have
not been validated.

## Build

Requires Apple silicon, full Xcode with DriverKit and SCSIControllerDriverKit SDKs,
Python 3, and ripgrep (`rg`) for the test script. The project targets macOS 26 and DriverKit 25.5.

```sh
/bin/sh scripts/test.sh
/bin/sh scripts/build.sh
```

The build verifies the pinned firmware SHA-256 and generates its embedded header.
Outputs are `build/qle-tape` and the unsigned containing app at
`build/DerivedData/Build/Products/Debug/QLE2560 Driver.app`.
Tests cover protocol encoding, firmware/NVRAM parsing, reset/mailbox faults,
request ownership, sense/residual handling and shutdown state transitions, using
AddressSanitizer/UndefinedBehaviorSanitizer and Python tests. They do not emulate
the complete hardware or macOS kernel DMA bookkeeping.

## Signing and loading

Open `QLE2560Driver.xcodeproj` in Xcode and select your development team for both
targets. Use app identifiers and provisioning profiles belonging to your team;
the checked-in identifiers are `org.iiyume.qle2560` for the app and
`org.iiyume.qle2560.driver` for the extension. The app's
embedded-extension identifier in `app/main.m` must match the driver's identifier.
Changing either identifier requires a matching App ID and provisioning profile;
the driver App ID must have the required DriverKit capabilities enabled.
Required capabilities include DriverKit, PCI transport and SCSI controller support,
plus system-extension installation for the containing app. See the entitlement
files under `app/` and `driver/`; development PCI entitlement scope is broader than
the device match. No certificates, private keys or provisioning profiles are included.

Install the signed containing app in `/Applications`, request activation in the
app, and complete macOS's driver approval. Registration is separate from actual
hardware attachment. System Integrity Protection can remain enabled. No signed installer or
notarized public binary release is provided by this source repository.

Fault injection is compiled out by default in Debug and Release.

## Usage

See [tape and tar usage](docs/tape-usage.md). Writes replace tape contents at the
current position; select and position a disposable cartridge first.

```sh
set -o pipefail
build/qle-tape list
build/qle-tape status
build/qle-tape rewind
tar --format pax -cf - folder | build/qle-tape write
build/qle-tape safe-disconnect
```

Only a successful `safe-disconnect` response confirms planned removal. After
software `reconnect`, wait for discovery and check status before further I/O.

## License and provenance

Driver, protocol and helper code use **GPL-2.0-only**; see [LICENSE](LICENSE).
The Linux qla2xxx reference version and source URL are recorded in
`references/upstream/SOURCE.txt`.

The unmodified QLogic firmware is separately licensed under
[firmware/LICENCE.qla2xxx](firmware/LICENCE.qla2xxx); provenance is recorded in
`firmware/SOURCE.txt`. Its license limits use to QLogic-authorized products.
Both license texts and firmware provenance are included in the built app.
