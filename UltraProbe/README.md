# UltraProbe

Read-only diagnostics for S3 sleep/wake on Intel desktops with a separate PCH.
UltraProbe never writes to hardware.

## When it records

| Event | Message | Line tag |
|---|---|---|
| going to sleep | `kIOMessageSystemWillSleep` | `sleep` |
| S3 resume, DarkWake | `kIOMessageSystemCapabilityChange`, DidChange, 0 → CPU without Graphics | `dark-wake` |
| full wake (screen on) | `kIOMessageSystemCapabilityChange`, DidChange, Graphics gained | `full-wake` |

`cN` in every line is the sleep number since boot.

## Reading the record

```bash
ioreg -rc UltraProbe -w0 -a | plutil -extract 0.UltraProbeLog json -o - - | python3 -c 'import json,sys; print("\n".join(json.load(sys.stdin)))'
```

The last 160 lines are kept in the `UltraProbeLog` property. The same lines
go to the kernel message buffer (`sudo dmesg | grep 'UP:'`).

## Line format

```
UP: c2 dark-wake t=1792356533.262
UP: c2 dark-wake soc@1800 PM1=0001/0520 G0S=….…. …00004800 G0E=… G1S=… G1E=…
UP: c2 dark-wake pch@8c00 PM1=0000/0400 G0S=…00002000 G0E=… G1S=00000200.… G1E=…
UP: c2 dark-wake pci 85:00.1 1002:ab28 pci1002,ab28 pmcs=8108 PME_Status PME_En
UP: c2 dark-wake scan bus 80: 17 functions, 0 with PME flags
UP: c2 dark-wake PTS PTSN=2 PCHB=8c00 PA00=2000 PA10=200 … PB00=0 …
UP: c2 dark-wake WAK WAKN=2 WAP1=1 WA00=2000 WA10=200 … WAS0=4800
UP: c2 dark-wake typed: LPCB.RTC(gpe ffff)=4 _SB.PWRB(gpe ffff)=1 RP01.PXSX(gpe 6b)=2
UP: c2 dark-wake root Wake Type="" Wake Reason="RP01 RP09 RP09 PXSX …"
```

| Field | Meaning |
|---|---|
| `t=` | calendar time, seconds since 1970 |
| `soc@base` / `pch@base` | PMC ACPI block: `PM1` status/enable; `G0S`/`G0E` GPE0 status/enable, four dwords (GPE 0x00-0x7F, last dword 0x60-0x7F); `G1S`/`G1E` GPE1 status/enable, three dwords |
| `pci b:d.f vid:did name` | an IOPCIDevice with `PME_Status` (bit 15) or `PME_En` (bit 8) in PMCS; for root ports also Root Control, Root Status and the PME requester ID |
| `scan b:d.f` | the same for a function found by reading config space by address; `scan bus N:` gives the counts |
| `PTS` / `WAK` | snapshots of SSDT-UltraSleep-Debug, see its header; absent without that table |
| `typed:` | ACPI devices with `acpi-wake-type`, their parent, `acpi-wake-gpe` and the type |
| `root` | `IOPMrootDomain` `Wake Type` and the first 60 characters of `Wake Reason` |

On Z890 the PCH `G1S` dwords are `S210`, `S211`, `S212` of the PchGpe SSDT
and the last PCH `G0S` dword is `S200`. `S210` bit `0x200` is the PCH XHCI.

## Settings

Personality properties in `Info.plist`:

| Key | Default | Meaning |
|---|---|---|
| `SoCACPIBase` | `0x1800` | SoC PMC ACPI I/O base (FADT PM1a event block) |
| `PCHACPIDevice` | `PC02` | ACPI device that owns the PCH base |
| `PCHBaseName` | `ABAS` | name of the base object in that device |
| `ScanBuses` | `[0x00, 0x80]` | root buses to scan by address (CPU and PCH segment) |

## Installation

Copy `UltraProbe.kext` to `EFI/OC/Kexts/` and add a `Kernel/Add` entry
(`ExecutablePath` `Contents/MacOS/UltraProbe`). It can run together with
UltraScreenWake. For `PTS`/`WAK` lines use `SSDT-UltraSleep-Debug.aml` with the
`_WAK` → `ZWAK` patch instead of `SSDT-UltraSleep.aml`.
