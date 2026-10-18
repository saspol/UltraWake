/*
 * UltraSleep: clear the latched PCH wake source before every sleep.
 *
 * Arrow Lake-S / Z890 has two PMCs: the SoC PMC at ACPI base 0x1800 (FADT,
 * DSDT PMBA) and the Z890 PCH PMC at \_SB.PC02.ABAS (0x8C00 on the Gigabyte
 * Z890 AORUS ELITE WIFI7, BIOS F17). The PchGpe SSDT describes the PCH
 * second-tier GPE registers as \_SB.PRR2.S210..S212 / S200 at that base and
 * clears them in \_GPE._L6B / _L6E.
 *
 * In macOS neither handler runs: GPE 0x6B and 0x6E are wake GPEs and are not
 * enabled at run time. A wake from the PCH XHCI leaves this chain set:
 *   PCH S210 bit 0x200 (XHCI)    -> PCH S200 bit 0x2000
 *   -> SoC GPE0 0x186C bits 0x4800 (GPE 0x6B, 0x6E)
 * and every following S3 entry wakes again within 9-17 s. The SoC bits can
 * be cleared only after the PCH bits.
 *
 * _PTS: if \_SB.PRR2.ABAS equals the firmware base \_SB.PC02.ABAS, write back
 *       the set bits of PCH S210..S212 and the PL6B/PL6E bits 0x46800 of PCH
 *       S200 (write-one-to-clear), then write 0x6800 (GPE 0x6E, 0x6D, 0x6B) to
 *       SoC GPE0 0x186C; then run the original _PTS (ZPTS).
 *
 * Requires two OpenCore ACPI patches (UltraSleep/OpenCore-patches.plist):
 *   - PchGpe SSDT: Name (ABAS, 0x8C00) in Device (PRR2). The table sets ABAS
 *     with a module-level Store that the macOS ACPICA skips, so PRR2 points
 *     at I/O port 0 without the patch.
 *   - DSDT: _PTS -> ZPTS.
 * Both patches are bound to the table lengths of BIOS F17.
 */
DefinitionBlock ("", "SSDT", 2, "ULTRA", "UltraSlp", 0x00001000)
{
    External (ZPTS, MethodObj)
    External (_SB_.PC02.ABAS, FieldUnitObj)
    External (_SB_.PRR2.ABAS, IntObj)
    External (_SB_.PRR2.S210, FieldUnitObj)
    External (_SB_.PRR2.S211, FieldUnitObj)
    External (_SB_.PRR2.S212, FieldUnitObj)
    External (_SB_.PRR2.S200, FieldUnitObj)

    // SoC GPE0 status for GPE 0x60-0x7F (SoC ACPI base 0x1800 + 0x6C).
    OperationRegion (USG0, SystemIO, 0x186C, 0x04)
    Field (USG0, DWordAcc, NoLock, Preserve)
    {
        USS3,   32
    }

    Method (_PTS, 1, NotSerialized)
    {
        If (CondRefOf (\_SB.PRR2.S210))
        {
            // Only while PRR2 points at the base the firmware gave the PCH:
            // after a BIOS change the base may move, and PRR2 would then
            // describe other ports.
            If ((\_SB.PRR2.ABAS == \_SB.PC02.ABAS))
            {
                Local0 = \_SB.PRR2.S210
                \_SB.PRR2.S210 = Local0
                Local0 = \_SB.PRR2.S211
                \_SB.PRR2.S211 = Local0
                Local0 = \_SB.PRR2.S212
                \_SB.PRR2.S212 = Local0
                \_SB.PRR2.S200 = 0x00046800
            }
        }

        USS3 = 0x6800
        ZPTS (Arg0)
    }
}
