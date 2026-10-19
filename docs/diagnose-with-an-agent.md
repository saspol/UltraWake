# Diagnose your machine with an AI agent

A short guide for running the same diagnosis on your own hackintosh with a
coding agent that can run shell commands on the Mac (for example Claude Code).
The agent does the reading and comparing; you do the steps that need your
hands or your password.

## Do you have the same problem?

Likely yes, if most of these are true:

- Intel Core Ultra 200S desktop processor (Arrow Lake-S, LGA 1851) on a
  Z890 motherboard. On other platforms (LGA 1700, laptops, AMD) the same
  symptoms have other causes, and this project does not apply;
- the first sleep after boot works, every later sleep ends by itself within
  about 10-20 seconds;
- `pmset -g log | grep -E 'DarkWake|Wake  '` shows
  `DarkWake from Normal Sleep [CDN] : due to RP01 RP09 … PXSX …` - a long list
  of PCIe root ports, the same for every wake;
- after a key press the machine wakes, but the screen comes on only after a
  second key press (`DarkWake to FullWake … due to UserActivity Assertion`).

## Before you start

- Keep a working copy of your `config.plist` next to the one you change
  (`EFI/OC/config.no-test.plist`) and know how to switch back without macOS:
  from Windows (`diskpart` → `assign letter=X` to the EFI partition, then
  `copy`), from the OpenCore shell, or from a USB stick with OpenCore.
- Change one thing per reboot. Let the agent check every change before you
  reboot: `ocvalidate` for the config, and `tools/ocpatch.py` + `acpiexec`
  for ACPI patches.
- Mount the EFI partition yourself (`sudo diskutil mount diskXs1`). The disk
  numbers can change between boots; the agent should check that the mounted
  partition has `EFI/OC/config.plist` before writing anything.

## A prompt to start with

> I have a hackintosh with the same sleep problems as described in the
> UltraWake repository, https://github.com/saspol/UltraWake. Board: …,
> BIOS: …, CPU: …, macOS: …, OpenCore: …. Clone the repository
> (`git clone https://github.com/saspol/UltraWake.git`) into a working
> folder and read README.md, AGENTS.md, docs/how-it-works.md and
> docs/diagnose-with-an-agent.md. Then diagnose my machine step by step:
> collect the current state first and change nothing until you have shown me
> what you found. Before every change to the EFI partition, make a backup,
> validate the config with ocvalidate, test ACPI patches with ocpatch.py and
> acpiexec on my dumped tables, show me the difference, and wait for my
> confirmation. One change per reboot. Keep a log of every finding and
> change in a Markdown file.

## Steps the agent takes

1. **Give the agent this repository and the facts.** Board, BIOS version,
   CPU, GPU, macOS and OpenCore versions, and the symptoms above.

2. **Let it collect the current state** (no changes yet):
   ```bash
   pmset -g log | grep -E 'Sleep  |Wake  |DarkWake  ' | tail -20
   log show --last boot --predicate 'process == "kernel" AND eventMessage CONTAINS "PMRD"' | tail -200
   ioreg -rc IOACPIPlatformDevice -n PRR2 -w0 | grep IODeviceMemory
   ioreg -p IOACPIPlane -lw0 -a > acpi-plane.plist
   ```
   `IODeviceMemory` with `address` `0` for `PRR2` means the PCH table did not
   get its base - the same fault as here.

3. **Dump the ACPI tables** with OpenCore (`Misc/Debug/SysReport` = true, boot
   once, the tables are in `SysReport/ACPI` on the EFI partition) and let the
   agent disassemble them with `iasl`. It should find:
   - `Device (PRR2)` and `ABAS = \_SB.PC02.ABAS` in the PchGpe SSDT;
   - `_L6B` / `PL6B`, the `S210`-`S212` / `S200` fields and which bit means
     XHCI (`H210`);
   - the `_PTS` method header in the DSDT and the table lengths.

4. **Install UltraProbe** (read-only, safe to keep while testing), reboot,
   then: sleep → wake with a key → sleep again → wait. Let the agent read the
   record (see `UltraProbe/README.md`). The lines to look at:
   - `UP: PC02.ABAS = 0x…` - your PCH base;
   - `pch@…` after a wake: is the first `G1S` dword (`S210`) `0x200` and the last
     `G0S` dword (`S200`) `0x2000`?
   - `soc@1800` last `G0S` dword `0x4800` and staying set at the next `sleep`.

5. **Adapt and test UltraSleep.** The agent sets your PCH base and table
   lengths in `UltraSleep/OpenCore-patches.plist`, checks that each `Find`
   occurs once, applies the patches to your dumped tables with
   `tools/ocpatch.py` and runs `_PTS` in `acpiexec`. Then install, reboot,
   and repeat the double sleep. For a closer look use
   `SSDT-UltraSleep-Debug` + the `_WAK` patch: the `PTS` line shows the
   registers before (`PA..`) and after (`PB..`) the clearing.

6. **Install UltraScreenWake** and test one-key wake. In the kernel log the
   agent should find `display wrangler tickled` and `Requesting full wake due
   to dark wake activity tickle` about 1.5 s after the DarkWake.

7. **Remove UltraProbe** when done.

## What the agent should not do

- Write to `EFI` without your confirmation, or without a backup.
- Install a kernel extension it did not build and check (`kmutil libraries
  -p … --undef-symbols` must resolve every symbol to a library listed in
  `OSBundleLibraries`).
- Guess: every claim about the cause should point at a register value, a
  table line or a log line.
