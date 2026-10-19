# UltraWake

Sleep and wake fixes for macOS on **Intel Core Ultra 200S desktop processors
(Arrow Lake-S, LGA 1851) on motherboards with the Intel Z890 chipset**.
Found and tested on a Gigabyte Z890 AORUS ELITE WIFI7 (BIOS F17) with
macOS 15 Sequoia and OpenCore 1.0.8.

> **Only for Core Ultra 200S + Z890.** The fix depends on the two power
> management controllers of this platform (one in the processor, one in the
> Z890 chipset) and on the ACPI tables of Z890 boards. It does not apply to
> 12th-14th generation Core processors (LGA 1700, Z690/Z790 and older),
> laptops (Core Ultra mobile), or AMD systems, even if the symptoms look the
> same. Other Arrow Lake-S chipsets (B860, H810) have not been tested.

> **Recommended: use it together with a coding agent** (for example Claude
> Code), the way the fix was found and tested. The PCH base address and the
> table lengths in the patches depend on your board and BIOS; the agent can
> read your registers and logs, check every patch against your dumped ACPI
> tables before you reboot, and change the EFI only with a backup and your
> confirmation. Start with [docs/diagnose-with-an-agent.md](docs/diagnose-with-an-agent.md)
> (it has a ready prompt); the rules for the agent are in [AGENTS.md](AGENTS.md).

The project has three parts:

| Part | What it is | What it fixes |
|---|---|---|
| **UltraSleep** | an SSDT and two OpenCore ACPI patches | every sleep after the first one ends by itself within 9-17 seconds |
| **UltraScreenWake** | a kernel extension | a key press wakes the machine, but the screen stays off until a second key press |
| **UltraProbe** | a read-only diagnostic kernel extension | nothing; it records the registers that show why the machine woke |

UltraScreenWake needs UltraSleep. UltraProbe is only for investigation.

## The problems

1. **The machine wakes by itself.** The first sleep after boot works. Every
   later sleep ends 9-17 seconds after it starts, and with Power Nap the
   machine keeps waking every few seconds.
2. **The screen does not come on.** A key press wakes the machine, but macOS
   stays in DarkWake (no display) until the keyboard is pressed again.

Both come from how this platform reports a USB wake. The details, with the
register values and kernel log lines that show them, are in
[docs/how-it-works.md](docs/how-it-works.md). In short:

- Arrow Lake-S has two power management controllers (PMCs): one in the SoC at
  ACPI I/O base `0x1800` (the one the FADT describes) and one in the Z890 PCH
  at its own base (`0x8C00` here). A wake from the PCH USB controller (XHCI)
  sets a status bit in the PCH, which keeps GPE `0x6B`/`0x6E` set in the SoC.
- The firmware clears that chain in its `_L6B`/`_L6E` GPE handlers. macOS never
  runs them, because both GPEs are wake-only and are not enabled while the
  system runs. The bits stay set, and the next sleep wakes immediately.
- The PCH table gets the PCH base with a module-level `Store`, which the ACPICA
  in macOS skips, so its registers point at I/O port 0.
- On every wake the PCH XHCI has lost power and macOS enumerates all USB
  devices again. The key press that woke the machine never reaches the HID
  system, nothing tells the display that the user is there, and the platform
  driver does not mark this wake as a user wake. macOS stays in DarkWake.

## UltraSleep

`UltraSleep/SSDT-UltraSleep.aml` adds a `_PTS` (prepare to sleep) method that,
before the original one runs:

1. writes back the set bits of the PCH second-tier status registers
   (`\_SB.PRR2.S210`-`S212`) and the bits `0x46800` of the PCH first-tier
   register (`S200`) - what the firmware handlers `PL6B`/`PL6E` do;
2. then writes `0x6800` to the SoC GPE0 status register at `0x186C`
   (GPE `0x6B`, `0x6D`, `0x6E`). The SoC bits clear only after the PCH bits.

The PCH part runs only while `\_SB.PRR2.ABAS` (set by the patch below) equals
the base the firmware gave the PCH (`\_SB.PC02.ABAS`), so after a BIOS change
the table does not write to ports that belong to something else.

It needs two OpenCore ACPI patches, both in
`UltraSleep/OpenCore-patches.plist`:

| Patch | Table | Why |
|---|---|---|
| `Name (ABAS, 0x8C00)` in `Device (PRR2)` | PchGpe SSDT | macOS skips the module-level `Store` that sets it |
| `_PTS` → `ZPTS` | DSDT | lets SSDT-UltraSleep provide `_PTS` and call the original |

Both patches carry the table lengths of BIOS F17 (`TableLength`) and do
nothing on another BIOS. The value `0x8C00` is this board's PCH base; check
yours before using it (see *Other boards*).

`SSDT-UltraSleep-Debug.aml` is the same table plus register snapshots in
`_PTS` and `_WAK` for UltraProbe. It needs the third patch in the file,
`_WAK` → `ZWAK`, which is disabled there. Use it instead of SSDT-UltraSleep,
never together.

## UltraScreenWake

A small kernel extension that turns a USB-keyboard DarkWake into a full wake.

When macOS enters DarkWake after S3 (a power capability change from nothing to
CPU without graphics), UltraScreenWake reads the PCH wake status register
(`PCH base + 0x10`). If the PCH XHCI bit (`0x200`) is set, the wake came from a
USB device: UltraSleep clears that bit before every sleep, so it belongs to
this wake. UltraScreenWake then calls `activityTickle(0, 0)` on
`IODisplayWrangler` 1.5 s and 4 s later - the call an HID event makes - and the
kernel requests the full wake ("Requesting full wake due to dark wake activity
tickle"). The screen comes on about 2 seconds after the key press.

It only reads hardware (one I/O port read per DarkWake). Wakes from the timer,
the power button or the network do not set the XHCI bit and stay as macOS
decides. Power Nap and maintenance wakes stay dark.

Settings (personality properties in `Info.plist`):

| Key | Default | Meaning |
|---|---|---|
| `PCHACPIDevice` | `PC02` | ACPI device that owns the PCH base |
| `PCHBaseName` | `ABAS` | name of the base object in that device |
| `WakeStatusOffset` | `0x10` | wake status register offset from the base |
| `WakeStatusMask` | `0x200` | bits that mean a USB wake |
| `TickleDelaysMS` | `[1500, 4000]` | delays of the tickles after the DarkWake |

State: `ioreg -rc UltraScreenWake -w0` shows `PCHBase`, `DarkWakes`,
`USBWakes`, `Tickles` and `LastWakeStatus`.

## UltraProbe

A read-only diagnostic kernel extension. On sleep, on DarkWake and on full
wake it records the SoC and PCH PMC registers, PCI power management state of
all devices (including PCH functions macOS does not show), the snapshots of
SSDT-UltraSleep-Debug, the ACPI devices with a wake type, and the wake type
and reason of the power manager. Read the record without root:

```bash
ioreg -rc UltraProbe -w0 -a | plutil -extract 0.UltraProbeLog json -o - - | python3 -c 'import json,sys; print("\n".join(json.load(sys.stdin)))'
```

The line format is described in [UltraProbe/README.md](UltraProbe/README.md).

To diagnose your own machine with a coding agent, see
[docs/diagnose-with-an-agent.md](docs/diagnose-with-an-agent.md).

## Installation (OpenCore)

1. Build (see *Building*) or take the release files.
2. Copy `SSDT-UltraSleep.aml` to `EFI/OC/ACPI/` and `UltraScreenWake.kext` to
   `EFI/OC/Kexts/`.
3. In `config.plist`:
   - add the `ACPI/Add` entry and the two enabled `ACPI/Patch` entries from
     `UltraSleep/OpenCore-patches.plist`;
   - add a `Kernel/Add` entry for `UltraScreenWake.kext`
     (`ExecutablePath` `Contents/MacOS/UltraScreenWake`,
     `PlistPath` `Contents/Info.plist`).
4. Keep a copy of the old `config.plist` next to it, so you can boot with it
   if something goes wrong.
5. Reboot and check:
   - `ioreg -rc IOACPIPlatformDevice -n PRR2 -w0 | grep IODeviceMemory` shows
     `address` `35840` (`0x8C00`);
   - `ioreg -rc UltraScreenWake -w0` shows the counters;
   - sleep twice in a row; the second sleep must not end by itself;
   - wake with one key press; the screen must come on in about 2 seconds.

`acpi-wake-type` in `DeviceProperties` for the USB controller is not needed
and does not help on this platform.

## Other Z890 boards

Other Core Ultra 200S + Z890 boards will likely need the same fix with
different values. Check these first:

- **PCH base.** UltraScreenWake reads it at run time. The UltraSleep patch has
  it as a constant. Check it with UltraProbe: the line
  `UP: PC02.ABAS = 0x8c00 ... used as PCH base`. If yours differs, change the
  last two bytes of the patch's `Replace` (`0B 00 8C` is `0x8C00`, little
  endian).
- **Table lengths.** The patches check `TableLength` (DSDT 608545, PchGpe
  11475 on F17). Dump your original tables (OpenCore DEBUG build,
  `SysReport`; see step 2 of
  [docs/diagnose-with-an-agent.md](docs/diagnose-with-an-agent.md)), check
  that the `Find` bytes occur exactly once, and set your lengths.
- **SoC base.** `0x1800` (FADT PM1a event block). SSDT-UltraSleep uses
  `0x186C` = `0x1800 + 0x6C`.
- **Wake bit.** `0x200` is the PCH XHCI in `S210` on Z890 (see `H210` in the
  PchGpe SSDT).

## Building

Requires the Command Line Tools (`xcode-select --install`); no Xcode.
`iasl` from ACPICA compiles the tables.

```bash
./build.sh
```

builds `UltraScreenWake/build/UltraScreenWake.kext`,
`UltraProbe/build/UltraProbe.kext` and the `.aml` files in `UltraSleep/`.
Set `SDK=` for another SDK path and `IASL=` for another `iasl`.
`tools/ocpatch.py` applies the `ACPI/Patch` entries of a `config.plist` to a
dumped table the way OpenCore does, to check the patches before booting.

## Status

Tested on one machine: Gigabyte Z890 AORUS ELITE WIFI7, BIOS F17, Intel Core
Ultra 200S (Arrow Lake-S), AMD Radeon RX 6900 XT, macOS 15.8.1, OpenCore 1.0.8. Not yet
tested: long sleep, wake by timer.

## License

MIT, see [LICENSE](LICENSE). The kernel extensions and the ACPI patches change
how the machine sleeps and wakes; use them at your own risk and keep a working
`config.plist` to boot with.
