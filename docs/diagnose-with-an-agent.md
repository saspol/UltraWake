# Diagnose your machine with an AI agent

A short guide for running the same diagnosis on your own hackintosh with a
coding agent that can run shell commands on the Mac (for example Claude Code).
The agent does the reading and comparing; you do the steps that need your
hands or your password.

## Do you have the same problem?

This guide is only for an **Intel Core Ultra 200S desktop processor
(Arrow Lake-S, LGA 1851) on a Z890 motherboard**. On other platforms
(LGA 1700, laptops, AMD) the same symptoms have other causes, and this
project does not apply.

On that platform you likely have the same problem if:

- the first sleep after boot works, but every later sleep ends by itself
  within about 10-20 seconds, without anyone touching the machine;
- a key press wakes the machine, but the screen comes on only after a second
  key press.

The agent confirms both from the logs in step 1 below.

## Before you start

- Keep a working copy of your `config.plist` next to the one you change
  (for example `EFI/OC/config.working.plist`) and know how to switch back
  without macOS: from Windows (`diskpart` → `assign letter=X` to the EFI
  partition, then `copy`), from the OpenCore shell, or from a USB stick with
  OpenCore.
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

Steps marked **You:** need your hands; the agent tells you when.

1. **Collect the current state and confirm the symptoms** (no changes yet).
   **You:** sleep the machine twice in a row and wake it with a key.
   ```bash
   pmset -g log | grep -E 'Sleep  |Wake  |DarkWake  ' | tail -20
   log show --last boot --predicate 'process == "kernel" AND eventMessage CONTAINS "PMRD"' | tail -200
   ioreg -rc IOACPIPlatformDevice -n PRR2 -w0 | grep IODeviceMemory
   ioreg -p IOACPIPlane -lw0 -a > acpi-plane.plist
   ```
   What to look for:
   - in `pmset -g log`, a second sleep followed 10-20 s later by
     `DarkWake from Normal Sleep` with no key press;
   - after a key press, `DarkWake to FullWake … due to UserActivity Assertion`
     only at the second press; in the `PMRD` lines, `display wrangler
     tickled` / `Requesting full wake due to dark wake activity tickle` appear
     only then;
   - `due to …` in the DarkWake line lists every device whose `_PRW` uses the
     GPE that fired (`RP01 RP09 … PXSX …` on Z890); the same list for the
     self-wake and the key-press wake means both came through the same GPE
     (`0x6B`). It does not name the source device;
   - `IODeviceMemory` with `address` `0` for `PRR2`: the PCH table did not get
     its base - the same fault as here.

2. **Get the original ACPI tables and read them.** **You:** boot once with a
   DEBUG build of OpenCore and `Misc/Debug/SysReport` = true; the tables are
   then in `SysReport/ACPI` on the EFI partition (RELEASE builds do not write
   the report). Tables taken from macOS IORegistry already contain your
   current OpenCore patches and are not suitable for writing `Find` bytes.
   The agent disassembles them with `iasl` and finds:
   - `Device (PRR2)` and `ABAS = \_SB.PC02.ABAS` in the PchGpe SSDT;
   - `_L6B` / `PL6B`, the `S210`-`S212` / `S200` fields and which bit means
     XHCI (`H210`);
   - the `_PTS` (and, for the debug table, `_WAK`) method headers in the DSDT
     and the lengths of the DSDT and the PchGpe SSDT.

3. **Install UltraProbe** (read-only, safe to keep while testing). The agent
   builds it and adds it to the EFI. **You:** reboot, then sleep → wake with a
   key → sleep again → wait. The agent reads the record (see
   `UltraProbe/README.md`):
   - `UP: PC02.ABAS = 0x… … used as PCH base` - your PCH base;
   - `pch@…` in the `dark-wake` snapshot: is the first `G1S` dword (`S210`)
     `0x200` and the last `G0S` dword (`S200`) `0x2000`?
   - `soc@1800`: is the last `G0S` dword `0x4800` and still set in the next
     `sleep` snapshot?

4. **Adapt and test UltraSleep.** The agent sets your PCH base and table
   lengths in `UltraSleep/OpenCore-patches.plist`, checks that each `Find`
   occurs exactly once in your tables, applies the patches with
   `tools/ocpatch.py` and runs `_PTS` in `acpiexec`, then installs it.
   **You:** reboot and repeat the double sleep; the second sleep must not end
   by itself. For a closer look the agent can use `SSDT-UltraSleep-Debug` with
   the `_WAK` patch (set to your DSDT length too): its `PTS` line in
   UltraProbe shows the registers before (`PA..`) and after (`PB..`) the
   clearing.

5. **Install UltraScreenWake.** **You:** reboot, sleep, wake with one key
   press. The agent checks the kernel log for `display wrangler tickled` and
   `Requesting full wake due to dark wake activity tickle` about 1.5 s after
   the DarkWake, and `ioreg -rc UltraScreenWake -w0` for `USBWakes`.

6. **Remove UltraProbe** (and the debug table, if used) when done.

## What the agent should not do

- Write to `EFI` without your confirmation, or without a backup.
- Install a kernel extension it did not build and check (`kmutil libraries
  -p … --undef-symbols` must resolve every symbol to a library listed in
  `OSBundleLibraries`).
- Guess: every claim about the cause should point at a register value, a
  table line or a log line.
