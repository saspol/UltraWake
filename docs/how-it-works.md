# How it works

Findings from the investigation on an Intel Core Ultra 200S (Arrow Lake-S)
desktop with a Gigabyte Z890 AORUS ELITE WIFI7 (BIOS F17), macOS 15.8.1
(xnu-11417.140.69), OpenCore 1.0.8. Register values
come from UltraProbe and its predecessors; kernel log lines come from the
`PMRD:` power management log (`log show --predicate 'process == "kernel"'`).

## 1. Two PMCs

Arrow Lake-S desktops have a power management controller in the SoC and
another one in the Z890 PCH. Each has an ACPI I/O block with the same layout:

| Offset | Register |
|---|---|
| `+0x00` / `+0x02` | PM1 status / enable |
| `+0x10`..`+0x18` | GPE1 status, three dwords |
| `+0x1C`..`+0x24` | GPE1 enable |
| `+0x60`..`+0x6C` | GPE0 status, four dwords (dword 3 = GPE 0x60-0x7F) |
| `+0x70`..`+0x7C` | GPE0 enable |

| PMC | Base | Where it comes from |
|---|---|---|
| SoC | `0x1800` | FADT (PM1a event block, GPE0 `0x1860`, GPE1 `0x1810`), DSDT `Name (PMBA, 0x1800)` |
| PCH | `0x8C00` | `\_SB.PC02.ABAS`, a field in the firmware NVS area of `PC02` (`OperationRegion (MTNV, SystemMemory, MNVB, …)`) |

The PchGpe SSDT describes the PCH registers as `Device (\_SB.PRR2)` with
`OperationRegion (PMC2, SystemIO, ABAS, 0xFF)`: `S210`-`S212`, `E210`-`E212`
at `+0x10`, `S200`/`E200` at `+0x6C`/`+0x7C`. `ABAS` is set at table load:

```
Device (PRR2)
{
    Name (ABAS, Zero)
    ABAS = \_SB.PC02.ABAS
    ...
```

The ACPICA in AppleACPIPlatform (2016) skips module-level `Store`
(opcode `0x70`) while it executes `If`/`Else`/`While`, so in macOS
`PRR2.ABAS` stays 0 and `PRR2` describes ports `0x00`-`0xFE`
(`IODeviceMemory = {address 0, length 255}` in IORegistry).

## 2. The wake chain

A USB wake from the PCH XHCI sets, in this order:

1. PCH `S210` (`0x8C10`) bit `0x200` - the XHCI source bit (`H210` in the
   PchGpe SSDT);
2. PCH `S200` (`0x8C6C`) bit `0x2000`;
3. SoC GPE0 dword 3 (`0x186C`) bits `0x0800` and `0x4000` - GPE `0x6B` and
   `0x6E`.

The firmware clears it in `\_GPE._L6B` → `PL6B` and `_L6E` → `PL6E`: read
`S210`-`S212`, notify the source devices, write the set bits back, write
`0x2000`/`0x0800`/`0x40000`/`0x4000` to `S200`. In macOS these handlers never
run: `0x6B` and `0x6E` are referenced by `_PRW` methods (39 devices on `0x6B`),
so ACPICA treats them as wake GPEs and does not enable them at run time (SoC
GPE0 enable dword 3 at run time: `0x000e8206`).

Measured with the probe (`G0S` = GPE0 status, `G1S` = GPE1 status):

| Moment | SoC `0x186C` | PCH `S200` | PCH `S210` |
|---|---|---|---|
| before the first sleep | `0x0000` | `0x0000` | `0x0200` (set since boot) |
| after a key-press wake | `0x4800` | `0x2000` | `0x0200` |
| `_PTS` of the next sleep, entry | `0x4800` | `0x2000` | `0x0200` |

With the bits still set, the next S3 entry wakes the machine at once
(9-17 s after "Entering Sleep state" in the power log).

## 3. What did not work, and why

| Attempt | Result | Reason |
|---|---|---|
| `PRR2.ABAS = 0x1800` and clearing in `_PTS` | no change | `0x1800` is the SoC block; `S210`-`S212` there are the SoC GPE1 registers |
| writing `0x6800` to SoC `0x186C` (from AML and with `outl`) | bits stay `0x4800` | the SoC bits follow the PCH: they clear only after the PCH bits |
| clearing PCH `S210`/`S200` (base `0x8C00`), then SoC | **works**, `0x186C` reads 0 after the write | - |

## 4. DarkWake

On x86, `IOPMrootDomain::powerChangeDone` decides right after
`sleepKernel()` returns:

- `Wake Type` "User" or "Alarm" → full wake (`darkWakeExit`);
- "Maintenance", "SleepService", "SleepTimer" → maintenance DarkWake;
- anything else → DarkWake, left only by a display wrangler tickle.

`Wake Type` is set by the platform driver before that point. For a wake
through GPE `0x6B` AppleACPIPlatform sets none. The `acpi-wake-type` property
did not change that:

| Attempt | Result |
|---|---|
| `acpi-wake-type = <01>` on the XHCI PCI device (`DeviceProperties`) | DarkWake |
| `acpi-wake-type = 1` on the XHCI ACPI node (`PC02.XHCI`, `acpi-wake-gpe` `0x6B`) | DarkWake |
| the same, and `acpi-wake-type` removed from the Intel NIC node (`PC02.RP01.PXSX`, type 2, also on `0x6B`) | DarkWake |
| boot argument `darkwake=2` | no effect: `PE_parse_boot_argn("darkwake", …)` is compiled only into DEVELOPMENT/DEBUG kernels |

The DarkWake ends with a tickle of `IODisplayWrangler`:
`IOService::activityTickle` → `IOPMrootDomain::handleActivityTickleForDisplayWrangler`
→ "display wrangler tickled" → "Requesting full wake due to dark wake activity
tickle". An HID event does this. The key press that woke the machine does
not, because the PCH XHCI loses power in S3 and macOS destroys and enumerates
every USB device again on wake:

```
05:49:35.035 AppleUSBHostPort::terminateDevice: destroying 0x05ac/024f/0104 (Keychron K1)
```

so the first key press is gone and only the second one tickles the display.

UltraScreenWake makes the tickle itself (`activityTickle(0, 0)`, what
`IOPMrootDomain::requestUserActive` does on x86; that method is exported only
to `com.apple.kpi.private`, which third-party kexts cannot link). Timing
matters:

| Attempt | Result | Reason |
|---|---|---|
| tickle from `kIOMessageSystemHasPoweredOn` | screen comes on only on the second key press | priority clients get `HasPoweredOn` only on the **full** wake |
| tickle 1.5 s / 4 s after `HasPoweredOn` | same | same |
| tickle 1.5 s / 4 s after `kIOMessageSystemCapabilityChange` (DidChange, 0 → CPU without Graphics) | **full wake 2 s after the key press**, "due to HID Activity" | the DarkWake itself |

Kernel log of a working wake:

```
06:02:26.511 PMRD: === FINISH (SLEEP_STATE->ON_STATE, 0->9, 0x2002)
06:02:28.005 PMRD: display wrangler tickled
06:02:28.005 PMRD: Requesting full wake due to dark wake activity tickle
06:02:28.005 PMRD: full wake request (reason 1) 2005 ms
```

UltraScreenWake checks PCH `S210` bit `0x200` first, so only USB wakes are
promoted. This needs UltraSleep: without it the bit is set from boot on and
every DarkWake would be promoted.

## 5. Notes for kext authors

- Kernel extensions injected by OpenCore do not reach the unified log;
  `IOLog` lines are only in the kernel message buffer (`sudo dmesg`), which
  the wake messages overflow within seconds. UltraProbe keeps its lines in
  an IORegistry property instead.
- `getProperty` returns an object owned by the registry entry. Reading it
  after `setProperty`/`removeProperty` of the same key uses freed memory; an
  early probe version panicked on sleep this way. Use `copyProperty`.
- Most ACPI nodes are not registered services; walk `gIOACPIPlane` to find
  them.
- Reading PCI config space of a function that has no `IOPCIDevice`:
  `IOPCIDevice::configRead32(IOPCIAddressSpace, offset)` on any device on the
  same bus goes to the host bridge, the same path the IOPCIFamily configurator
  uses to probe empty slots.
