# AGENTS.md

Instructions for coding agents working in this repository. Read README.md and
docs/how-it-works.md before changing anything; they explain the hardware
behaviour every part of the code relies on.

## What this is

macOS sleep/wake fixes for **Intel Core Ultra 200S desktop processors
(Arrow Lake-S) on Z890 motherboards**, used with OpenCore:

| Directory | Part | Kind |
|---|---|---|
| `UltraSleep/` | clears the latched PCH wake source in `_PTS` | SSDT (`.dsl`) + OpenCore ACPI patches (`OpenCore-patches.plist`) |
| `UltraScreenWake/` | turns a USB-keyboard DarkWake into a full wake | IOKit kernel extension (C++) |
| `UltraProbe/` | records sleep/wake registers | read-only diagnostic kernel extension (C++) |
| `tools/ocpatch.py` | applies OpenCore `ACPI/Patch` entries to a dumped table | test helper |
| `docs/` | how it works; how to diagnose another machine with an agent | documentation |

Do not suggest these parts for other platforms (LGA 1700, laptops, AMD): the
fix depends on the two PMCs of Arrow Lake-S + Z890. If a user's machine is
different, say so instead of adapting the values blindly.

## Building

```bash
./build.sh                     # all parts
./UltraScreenWake/build.sh     # one kext
IASL=/path/to/iasl ./UltraSleep/build.sh
```

- Command Line Tools only, no Xcode. `SDK=` overrides the SDK path (default
  `MacOSX15.5.sdk`). The kexts are x86_64, `-mkernel -fapple-kext`,
  `-Werror`; a build with warnings is a failed build.
- `iasl` from ACPICA compiles the tables (`-vw 3133`); 0 errors and 0 warnings
  are required.
- `build/` and `*.aml` are build output and are not committed.

## Checks before any change is considered done

For a kernel extension:

1. It builds without warnings.
2. `kmutil libraries -p <abs path to .kext> --undef-symbols` resolves every
   symbol, and only to libraries listed in `OSBundleLibraries` of its
   `Info.plist`. A symbol from a library that is not listed makes OpenCore
   fail to link the kext at boot.
3. `kmutil print-diagnostics -p <kext>` says `Dependencies: OK` (the ownership
   and signature remarks it also prints do not apply to OpenCore injection).
4. Never depend on `com.apple.kpi.private`: `kmutil` refuses third-party kexts
   that link it.
5. Count port writes: `otool -tv <binary> | grep -cE '\bout[lwb]?\b'` must be
   0 for UltraScreenWake and UltraProbe. Both only read hardware.

For an ACPI table or patch:

1. `iasl` compiles it without warnings.
2. Apply the patches to the dumped original tables with `tools/ocpatch.py`
   and check that every patch replaces exactly once (`Count`, `TableLength`
   and `OemTableId` guard them).
3. Load all tables of the machine plus the new SSDT in `acpiexec` and run
   `\_PTS 3` and `\_WAK 3`: no `AE_` errors, `_WAK` still returns its package.
   Test both firmware states, e.g. with `acpiexec -fi init.txt` where
   `init.txt` sets `\_SB.PC02.ABAS 0x8C00`, and without it.
4. Remember that the ACPICA in macOS skips module-level `Store`: never set a
   value at table load with `Store`/`=` outside a method; use `Name (X, value)`.

## Kernel code rules

These come from real panics and failed attempts in this project:

- Read registry properties with `copyProperty` and release the result. A
  `getProperty` result is owned by the entry and is freed when the property
  is replaced or removed; reading it after `setProperty`/`removeProperty`
  panicked the machine on sleep.
- Detect the S3 resume (DarkWake) with `kIOMessageSystemCapabilityChange`
  (`kIOPMSystemCapabilityDidChange`, from 0 to CPU without Graphics).
  Priority sleep/wake clients get `kIOMessageSystemHasPoweredOn` only on the
  full wake.
- Do not tickle `IODisplayWrangler` from inside the power notification; use a
  timer (`IOTimerEventSource` on the kext's own `IOWorkLoop`).
- Most ACPI nodes are not registered services: walk `gIOACPIPlane` to find
  them, or use `IOACPIPlatformDevice` matching only for registered ones
  (`PC02`, `PRR2`).
- OpenCore-injected kexts do not reach the unified log. `IOLog` goes to the
  kernel message buffer only; keep anything a user must read in IORegistry
  properties.
- Do not write to PMC or PCI registers from a kext. Clearing status bits is
  done in ACPI `_PTS` (UltraSleep), the last code that runs before S3.
- Keep the defaults in `Info.plist` personality properties, not hard-coded,
  when a value is board-specific (PCH base name, offsets, masks, delays).

## Changing a user's EFI (when helping someone install or test)

- Only on the user's explicit request, one change per reboot.
- Before writing: check that the mounted partition has `EFI/OC/config.plist`
  (disk numbers change between boots; the Windows EFI has no `OC`), make a
  backup of `config.plist` and of every file being replaced, on the EFI and
  locally.
- After writing: compare every copied file byte for byte with the build,
  run `ocvalidate` (matching the user's OpenCore version) on the new config
  and on the rollback config, check that every enabled `ACPI/Add` and
  `Kernel/Add` entry exists on the EFI, and apply the config's patches to the
  dumped tables with `tools/ocpatch.py` to confirm the result equals what was
  tested.
- Leave a rollback config on the EFI and tell the user how to restore it
  without macOS (Windows `diskpart` or the OpenCore shell).
- The user mounts EFI (`sudo diskutil mount …`); do not ask for their password.

## Diagnosing a user's machine

Follow docs/diagnose-with-an-agent.md. Every claim about a cause must point at
a register value, a table line or a kernel log line (`PMRD:` lines in
`log show --predicate 'process == "kernel"'`). Collect state before changing
anything.

## Documentation and commits

- Documentation, code comments and commit messages are in English.
- Describe the mechanism and the consequence literally: register, bit, value,
  message, log line. No figures of speech.
- When behaviour changes, update README.md and, if the reason is new,
  docs/how-it-works.md in the same commit.
- One logical change per commit, with a message that says what changed and
  why. Do not commit `build/`, `*.aml` or files with personal paths or
  addresses (check with `git grep` before committing).
