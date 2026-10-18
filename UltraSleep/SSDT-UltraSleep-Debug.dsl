/*
 * UltraSleep (debug): SSDT-UltraSleep plus register snapshots in _PTS and
 * _WAK for UltraProbe. Use instead of SSDT-UltraSleep, not together with it.
 *
 * _PTS: snapshot (PA..), clear PCH S210..S212 by writing back their set bits,
 *       PCH S200 0x46800 (as PL6B/PL6E), then SoC 0x186C 0x6800 (GPE 0x6E,
 *       0x6D, 0x6B), snapshot (PB..), original _PTS (ZPTS), snapshot (PC..).
 * _WAK: snapshot (WA..), then the original _WAK (ZWAK).
 * The snapshots are Names in \_SB.PRR2 (layout WSPV = 3): PA00/PB00/PC00/WA00
 * are PCH S200, PA1x/PB1x/PC10/WA1x PCH S210..S212, PAS0/PBS0/PCS0/WAS0 SoC
 * GPE0 0x186C, PCE0/PCE1 PCH E200/E210, PCES SoC GPE0 enable 0x187C, WAP1
 * SoC PM1 status, PCHB \_SB.PC02.ABAS, PTSN/WAKN call counts. UltraProbe
 * prints them as its PTS/WAK lines.
 *
 * Requires the UltraSleep patches (PRR2 ABAS, _PTS -> ZPTS) and the debug
 * patch _WAK -> ZWAK (OpenCore-patches.plist, disabled by default).
 */
DefinitionBlock ("", "SSDT", 2, "ULTRA", "UltraSlD", 0x00001000)
{
    External (ZPTS, MethodObj)
    External (ZWAK, MethodObj)
    External (_SB_.PRR2, DeviceObj)
    External (_SB_.PRR2.ABAS, IntObj)
    External (_SB_.PC02.ABAS, FieldUnitObj)
    External (_SB_.PRR2.S210, FieldUnitObj)
    External (_SB_.PRR2.S211, FieldUnitObj)
    External (_SB_.PRR2.S212, FieldUnitObj)
    External (_SB_.PRR2.E210, FieldUnitObj)
    External (_SB_.PRR2.E211, FieldUnitObj)
    External (_SB_.PRR2.E212, FieldUnitObj)
    External (_SB_.PRR2.S200, FieldUnitObj)
    External (_SB_.PRR2.E200, FieldUnitObj)

    Scope (\_SB.PRR2)
    {
        Name (WSPV, 0x03)   // layout version of these Names
        Name (PTSN, Zero)   // _PTS calls since boot
        Name (WAKN, Zero)   // _WAK calls since boot
        Name (PCHB, Zero)   // \_SB.PC02.ABAS (firmware NVS) seen by _PTS
        // _PTS entry
        Name (PA00, Zero)   // PCH S200
        Name (PA10, Zero)   // PCH S210
        Name (PA11, Zero)   // PCH S211
        Name (PA12, Zero)   // PCH S212
        Name (PAS0, Zero)   // SoC GPE0 0x186C
        // _PTS after clearing, before the original _PTS
        Name (PB00, Zero)
        Name (PB10, Zero)
        Name (PB11, Zero)
        Name (PB12, Zero)
        Name (PBS0, Zero)
        // _PTS after the original _PTS
        Name (PC00, Zero)
        Name (PC10, Zero)
        Name (PCS0, Zero)
        Name (PCE0, Zero)   // PCH E200
        Name (PCE1, Zero)   // PCH E210
        Name (PCES, Zero)   // SoC GPE0 enable 0x187C
        // _WAK entry, before the original _WAK
        Name (WA00, Zero)
        Name (WA10, Zero)
        Name (WA11, Zero)
        Name (WA12, Zero)
        Name (WAS0, Zero)
        Name (WAP1, Zero)   // SoC PM1 status (0x1800)
    }

    // SoC PMC registers (ACPI base 0x1800): PM1 status, GPE0 status and
    // enable for GPE 0x60-0x7F.
    OperationRegion (WSPM, SystemIO, 0x1800, 0x04)
    Field (WSPM, WordAcc, NoLock, Preserve)
    {
        WPM1,   16
    }

    OperationRegion (SGP0, SystemIO, 0x186C, 0x14)
    Field (SGP0, DWordAcc, NoLock, Preserve)
    {
        SGS3,   32,
        Offset (0x10),
        SGE3,   32
    }

    Method (_PTS, 1, NotSerialized)
    {
        If (CondRefOf (\_SB.PRR2.S210))
        {
            \_SB.PRR2.PTSN++
            \_SB.PRR2.PCHB = \_SB.PC02.ABAS
            \_SB.PRR2.PA00 = \_SB.PRR2.S200
            \_SB.PRR2.PA10 = \_SB.PRR2.S210
            \_SB.PRR2.PA11 = \_SB.PRR2.S211
            \_SB.PRR2.PA12 = \_SB.PRR2.S212
            \_SB.PRR2.PAS0 = SGS3

            // Only when the base the ABAS patch put into PRR2 is the one the
            // firmware gave the PCH (after a BIOS change it may differ, and
            // PRR2 would then point at other ports).
            If ((\_SB.PRR2.ABAS == \_SB.PC02.ABAS))
            {
                // PCH second tier: write back the set bits (write-one-to-clear).
                Local0 = \_SB.PRR2.S210
                \_SB.PRR2.S210 = Local0
                Local0 = \_SB.PRR2.S211
                \_SB.PRR2.S211 = Local0
                Local0 = \_SB.PRR2.S212
                \_SB.PRR2.S212 = Local0
                // PCH first tier, the bits PL6B and PL6E clear.
                \_SB.PRR2.S200 = 0x00046800
            }

            // SoC GPE 0x6E (0x4000), 0x6D (0x2000), 0x6B (0x0800); this
            // takes effect only after the PCH bits are clear.
            SGS3 = 0x6800

            \_SB.PRR2.PB00 = \_SB.PRR2.S200
            \_SB.PRR2.PB10 = \_SB.PRR2.S210
            \_SB.PRR2.PB11 = \_SB.PRR2.S211
            \_SB.PRR2.PB12 = \_SB.PRR2.S212
            \_SB.PRR2.PBS0 = SGS3
        }

        ZPTS (Arg0)

        If (CondRefOf (\_SB.PRR2.S210))
        {
            \_SB.PRR2.PC00 = \_SB.PRR2.S200
            \_SB.PRR2.PC10 = \_SB.PRR2.S210
            \_SB.PRR2.PCS0 = SGS3
            \_SB.PRR2.PCE0 = \_SB.PRR2.E200
            \_SB.PRR2.PCE1 = \_SB.PRR2.E210
            \_SB.PRR2.PCES = SGE3
        }
    }

    Method (_WAK, 1, NotSerialized)
    {
        If (CondRefOf (\_SB.PRR2.S210))
        {
            \_SB.PRR2.WAKN++
            \_SB.PRR2.WAP1 = WPM1
            \_SB.PRR2.WA00 = \_SB.PRR2.S200
            \_SB.PRR2.WA10 = \_SB.PRR2.S210
            \_SB.PRR2.WA11 = \_SB.PRR2.S211
            \_SB.PRR2.WA12 = \_SB.PRR2.S212
            \_SB.PRR2.WAS0 = SGS3
        }

        Return (ZWAK (Arg0))
    }
}
