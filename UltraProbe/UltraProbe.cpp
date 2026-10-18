// UltraProbe: read-only diagnostics for S3 sleep/wake on Intel desktops with
// a separate PCH (Arrow Lake-S / Z890), written to find why the system woke
// by itself and why a key press gave a DarkWake.
//
// It takes a snapshot on three events:
//   sleep       kIOMessageSystemWillSleep
//   dark-wake   kIOMessageSystemCapabilityChange, DidChange, from no
//               capabilities to CPU without Graphics (the S3 resume)
//   full-wake   kIOMessageSystemCapabilityChange, DidChange, Graphics gained
// (Priority sleep/wake clients get kIOMessageSystemHasPoweredOn only on the
// full wake, so it is not used to detect the resume.)
//
// A snapshot holds:
//   soc     SoC PMC: PM1 status/enable, GPE0 status/enable (base + 0x60/0x70,
//           four dwords, GPE 0x00-0x7F) and GPE1 status/enable (base +
//           0x10/0x1C, three dwords, GPE 0x80-0xDF). Base from SoCACPIBase.
//   pch     the same layout at the PCH PMC base, read through ACPI from
//           \_SB.<PCHACPIDevice>.<PCHBaseName> (\_SB.PC02.ABAS). On Z890 the
//           PCH GPE1 dwords are the PRR2 S210..S212 registers of the PchGpe
//           SSDT and GPE0 dword 3 is PRR2 S200.
//   pci     PMCS of every IOPCIDevice with PME_Status or PME_En, and Root
//           Control/Status of every PCIe root port with a PME pending.
//   scan    PMCS of every function on the ScanBuses root buses, read by
//           address through an IOPCIDevice on that bus
//           (IOPCIDevice::configRead(space, offset), the path the IOPCIFamily
//           configurator uses to probe empty slots), so functions without an
//           IOPCIDevice (hidden PCH devices) are seen too; only functions
//           with PME_Status or PME_En are listed.
//   PTS/WAK the register snapshots that SSDT-UltraSleep-Debug stores in
//           \_SB.PRR2 in _PTS and _WAK (layout WSPV = 3), if loaded.
//   typed   ACPI devices with acpi-wake-type and their acpi-wake-gpe, and
//           IOPMrootDomain "Wake Type" / "Wake Reason".
//
// Nothing is written to the hardware: status registers are write-one-to-clear
// and are only read here.
//
// Each line goes to IOLog (kernel message buffer, `sudo dmesg`; OpenCore-
// injected kexts do not reach the unified log) and into the last kHistoryMax
// lines of the "UltraProbeLog" property, which `ioreg -rc UltraProbe -w0 -a`
// reads without root and which a burst of wake messages cannot push out.

#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOMessage.h>
#include <IOKit/IOLocks.h>
#include <IOKit/pwr_mgt/RootDomain.h>
#include <IOKit/pwr_mgt/IOPM.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <IOKit/acpi/IOACPIPlatformDevice.h>
#include <libkern/c++/OSArray.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSString.h>
#include <kern/clock.h>
#include <architecture/i386/pio.h>

static const unsigned kHistoryMax = 160;
static const unsigned kMaxScanBuses = 4;

class UltraProbe : public IOService
{
    OSDeclareDefaultStructors(UltraProbe)

public:
    bool start(IOService *provider) override;
    void stop(IOService *provider) override;
    void free() override;

private:
    IONotifier *fNotifier;
    IOLock     *fLock;              // fHistory
    OSArray    *fHistory;
    UInt32      fCycle;

    UInt16      fSocBase;
    char        fPchDevice[8];
    char        fBaseName[8];
    UInt16      fPchBase;           // 0 until resolved
    UInt8       fScanBuses[kMaxScanBuses];
    unsigned    fScanBusCount;

    static IOReturn powerHandler(void *target, void *refCon, UInt32 messageType,
                                 IOService *provider, void *messageArgument,
                                 vm_size_t argSize);
    void readConfig();
    void snapshot(const char *when);
    void dumpPmc(const char *when, const char *tag, UInt16 base);
    void dumpPci(const char *when);
    void scanBuses(const char *when);
    void dumpAcpiSnapshots(const char *when);
    void dumpWakeTypes(const char *when);
    bool resolvePchBase();
    void record(const char *line);
    void publish();
};

OSDefineMetaClassAndStructors(UltraProbe, IOService)

// ---------------------------------------------------------------------------
// Helpers

static void copyString(IORegistryEntry *entry, const char *key, char *buf, size_t size)
{
    OSObject *obj = entry->copyProperty(key);
    OSString *str = OSDynamicCast(OSString, obj);
    if (str && str->getLength() > 0 && str->getLength() < size) {
        strlcpy(buf, str->getCStringNoCopy(), size);
    }
    OSSafeReleaseNULL(obj);
}

// Registry properties are read with copyProperty, which returns a retained
// object: a getProperty result belongs to the entry and is released as soon
// as the property is replaced or removed by another thread.
static bool copyNumber(IORegistryEntry *entry, const char *key, UInt32 *value)
{
    OSObject *obj = entry->copyProperty(key);
    OSNumber *num = OSDynamicCast(OSNumber, obj);
    bool      ok = num != nullptr;
    if (ok) {
        *value = num->unsigned32BitValue();
    }
    OSSafeReleaseNULL(obj);
    return ok;
}

// The first registered IOACPIPlatformDevice with this name, retained.
static IOACPIPlatformDevice *findAcpiDevice(const char *name)
{
    OSDictionary *match = IOService::serviceMatching("IOACPIPlatformDevice");
    OSIterator   *iter = match ? IOService::getMatchingServices(match) : nullptr;
    OSSafeReleaseNULL(match);
    if (!iter) {
        return nullptr;
    }
    IOACPIPlatformDevice *found = nullptr;
    OSObject *obj;
    while ((obj = iter->getNextObject())) {
        IOACPIPlatformDevice *dev = OSDynamicCast(IOACPIPlatformDevice, obj);
        if (dev && dev->getName() && !strncmp(dev->getName(), name, 5)) {
            found = dev;
            found->retain();
            break;
        }
    }
    iter->release();
    return found;
}

// An IOPCIDevice on this bus, retained; config reads by address through it
// go to the host bridge of its segment.
static IOPCIDevice *findPciOnBus(UInt8 bus)
{
    OSDictionary *match = IOService::serviceMatching("IOPCIDevice");
    OSIterator   *iter = match ? IOService::getMatchingServices(match) : nullptr;
    OSSafeReleaseNULL(match);
    if (!iter) {
        return nullptr;
    }
    IOPCIDevice *found = nullptr;
    OSObject *obj;
    while ((obj = iter->getNextObject())) {
        IOPCIDevice *dev = OSDynamicCast(IOPCIDevice, obj);
        if (dev && !dev->isInactive() && dev->getBusNumber() == bus) {
            found = dev;
            found->retain();
            break;
        }
    }
    iter->release();
    return found;
}

// ---------------------------------------------------------------------------
// Life cycle

void UltraProbe::readConfig()
{
    UInt32 soc = 0x1800;
    copyNumber(this, "SoCACPIBase", &soc);
    fSocBase = (UInt16)soc;

    strlcpy(fPchDevice, "PC02", sizeof(fPchDevice));
    strlcpy(fBaseName, "ABAS", sizeof(fBaseName));
    copyString(this, "PCHACPIDevice", fPchDevice, sizeof(fPchDevice));
    copyString(this, "PCHBaseName", fBaseName, sizeof(fBaseName));

    fScanBusCount = 0;
    OSObject *obj = copyProperty("ScanBuses");
    OSArray  *arr = OSDynamicCast(OSArray, obj);
    if (arr) {
        for (unsigned i = 0; i < arr->getCount() && fScanBusCount < kMaxScanBuses; i++) {
            OSNumber *num = OSDynamicCast(OSNumber, arr->getObject(i));
            if (num && num->unsigned32BitValue() <= 0xFF) {
                fScanBuses[fScanBusCount++] = (UInt8)num->unsigned32BitValue();
            }
        }
    }
    OSSafeReleaseNULL(obj);
}

bool UltraProbe::start(IOService *provider)
{
    if (!IOService::start(provider)) {
        return false;
    }
    readConfig();
    fLock = IOLockAlloc();
    fHistory = OSArray::withCapacity(kHistoryMax + 1);
    if (!fLock || !fHistory) {
        return false;
    }
    fNotifier = registerPrioritySleepWakeInterest(&UltraProbe::powerHandler, this, nullptr);

    char line[128];
    snprintf(line, sizeof(line), "UP: started, SoC base 0x%x, PCH base from %s.%s, sleep/wake messages %s",
             fSocBase, fPchDevice, fBaseName, fNotifier ? "registered" : "NOT registered");
    record(line);
    publish();
    registerService();
    return true;
}

void UltraProbe::stop(IOService *provider)
{
    if (fNotifier) {
        fNotifier->remove();
        fNotifier = nullptr;
    }
    IOService::stop(provider);
}

void UltraProbe::free()
{
    OSSafeReleaseNULL(fHistory);
    if (fLock) {
        IOLockFree(fLock);
        fLock = nullptr;
    }
    IOService::free();
}

IOReturn UltraProbe::powerHandler(void *target, void *refCon, UInt32 messageType,
                                  IOService *provider, void *messageArgument,
                                  vm_size_t argSize)
{
    UltraProbe *self = static_cast<UltraProbe *>(target);
    if (!self) {
        return kIOReturnSuccess;
    }
    if (messageType == kIOMessageSystemWillSleep) {
        self->fCycle++;
        self->snapshot("sleep");
    } else if (messageType == kIOMessageSystemCapabilityChange && messageArgument) {
        const IOPMSystemCapabilityChangeParameters *cap =
            static_cast<const IOPMSystemCapabilityChangeParameters *>(messageArgument);
        if (!(cap->changeFlags & kIOPMSystemCapabilityDidChange)) {
            return kIOReturnSuccess;
        }
        if (cap->fromCapabilities == 0 &&
            (cap->toCapabilities & kIOPMSystemCapabilityCPU) &&
            !(cap->toCapabilities & kIOPMSystemCapabilityGraphics)) {
            self->snapshot("dark-wake");
        } else if (!(cap->fromCapabilities & kIOPMSystemCapabilityGraphics) &&
                   (cap->toCapabilities & kIOPMSystemCapabilityGraphics)) {
            self->snapshot("full-wake");
        }
    }
    return kIOReturnSuccess;
}

// ---------------------------------------------------------------------------
// Snapshot

void UltraProbe::snapshot(const char *when)
{
    clock_sec_t  secs = 0;
    clock_usec_t usecs = 0;
    clock_get_calendar_microtime(&secs, &usecs);
    char line[96];
    snprintf(line, sizeof(line), "UP: c%u %s t=%lu.%03u", fCycle, when,
             (unsigned long)secs, (unsigned)(usecs / 1000));
    record(line);

    dumpPmc(when, "soc", fSocBase);
    if (resolvePchBase()) {
        dumpPmc(when, "pch", fPchBase);
    }
    dumpPci(when);
    scanBuses(when);
    dumpAcpiSnapshots(when);
    dumpWakeTypes(when);
    publish();
}

// PMC ACPI I/O block: PM1 status/enable at +0/+2, GPE1 status/enable at
// +0x10/+0x1C (three dwords), GPE0 status/enable at +0x60/+0x70 (four dwords).
void UltraProbe::dumpPmc(const char *when, const char *tag, UInt16 base)
{
    UInt32 g0s[4], g0e[4], g1s[3], g1e[3];
    for (unsigned i = 0; i < 4; i++) {
        g0s[i] = inl((UInt16)(base + 0x60 + 4 * i));
        g0e[i] = inl((UInt16)(base + 0x70 + 4 * i));
    }
    for (unsigned i = 0; i < 3; i++) {
        g1s[i] = inl((UInt16)(base + 0x10 + 4 * i));
        g1e[i] = inl((UInt16)(base + 0x1C + 4 * i));
    }
    char line[288];
    snprintf(line, sizeof(line),
             "UP: c%u %s %s@%x PM1=%04x/%04x"
             " G0S=%08x.%08x.%08x.%08x G0E=%08x.%08x.%08x.%08x"
             " G1S=%08x.%08x.%08x G1E=%08x.%08x.%08x",
             fCycle, when, tag, base, inw(base), inw((UInt16)(base + 2)),
             g0s[0], g0s[1], g0s[2], g0s[3], g0e[0], g0e[1], g0e[2], g0e[3],
             g1s[0], g1s[1], g1s[2], g1e[0], g1e[1], g1e[2]);
    record(line);
}

// \_SB.<PCHACPIDevice>.<PCHBaseName> through ACPI, accepted when it looks
// like an ACPI I/O base (I/O space above the legacy range, 256-byte aligned)
// and is not the SoC base. ACPI devices are registered after this kext
// starts, so the first lookup happens on the first sleep.
bool UltraProbe::resolvePchBase()
{
    if (fPchBase) {
        return true;
    }
    IOACPIPlatformDevice *dev = findAcpiDevice(fPchDevice);
    if (!dev) {
        return false;
    }
    UInt32   base = 0;
    IOReturn ret = dev->evaluateInteger(fBaseName, &base);
    dev->release();

    bool ok = ret == kIOReturnSuccess && base >= 0x400 && base <= 0xFF00 && !(base & 0xFF) && base != fSocBase;
    char line[128];
    snprintf(line, sizeof(line), "UP: %s.%s = 0x%x (ret 0x%x) %s", fPchDevice, fBaseName, base, ret,
             ok ? "used as PCH base" : "rejected");
    record(line);
    if (ok) {
        fPchBase = (UInt16)base;
    }
    return ok;
}

void UltraProbe::dumpPci(const char *when)
{
    OSDictionary *match = IOService::serviceMatching("IOPCIDevice");
    OSIterator   *iter = match ? IOService::getMatchingServices(match) : nullptr;
    OSSafeReleaseNULL(match);
    if (!iter) {
        return;
    }
    OSObject *obj;
    while ((obj = iter->getNextObject())) {
        IOPCIDevice *dev = OSDynamicCast(IOPCIDevice, obj);
        if (!dev || dev->isInactive()) {
            continue;
        }
        UInt32 ids = dev->configRead32((UInt8)0);
        if (ids == 0xFFFFFFFF || ids == 0) {
            continue;
        }

        UInt16 pmcs = 0;
        UInt8  pmOff = 0;
        if (dev->findPCICapability(kIOPCIPowerManagementCapability, &pmOff) && pmOff) {
            pmcs = dev->configRead16((UInt8)(pmOff + 4));
        }

        UInt8  exOff = 0;
        bool   rootPort = false;
        UInt32 rootStatus = 0;
        UInt16 rootControl = 0;
        if (dev->findPCICapability(kIOPCIPCIExpressCapability, &exOff) && exOff) {
            UInt16 exCaps = dev->configRead16((UInt8)(exOff + 2));
            rootPort = ((exCaps >> 4) & 0xF) == 4;
            if (rootPort) {
                rootControl = dev->configRead16((UInt8)(exOff + 0x1C));
                rootStatus  = dev->configRead32((UInt8)(exOff + 0x20));
            }
        }

        bool pmeFlag = (pmcs & 0x8100) != 0;               // PME_Status or PME_En
        bool rootPme = rootPort && (rootStatus & 0x30000);  // PME Status or PME Pending
        if (!pmeFlag && !rootPme) {
            continue;
        }

        char line[224];
        int  pos = snprintf(line, sizeof(line), "UP: c%u %s pci %02x:%02x.%x %04x:%04x %s pmcs=%04x%s%s",
                            fCycle, when,
                            dev->getBusNumber(), dev->getDeviceNumber(), dev->getFunctionNumber(),
                            ids & 0xFFFF, ids >> 16,
                            dev->getName() ? dev->getName() : "?",
                            pmcs,
                            (pmcs & 0x8000) ? " PME_Status" : "",
                            (pmcs & 0x0100) ? " PME_En" : "");
        if (rootPort && pos > 0 && pos < (int)sizeof(line)) {
            // Root Status: bits 15:0 PME Requester ID, bit 16 PME Status, bit 17 PME Pending.
            snprintf(line + pos, sizeof(line) - (size_t)pos, " root-ctl=%04x root-sts=%08x req=%02x:%02x.%x",
                     rootControl, rootStatus,
                     (rootStatus >> 8) & 0xFF, (rootStatus >> 3) & 0x1F, rootStatus & 0x7);
        }
        record(line);
    }
    iter->release();
}

void UltraProbe::scanBuses(const char *when)
{
    char line[160];
    for (unsigned b = 0; b < fScanBusCount; b++) {
        UInt8        bus = fScanBuses[b];
        IOPCIDevice *gw = findPciOnBus(bus);
        if (!gw) {
            snprintf(line, sizeof(line), "UP: c%u %s scan bus %02x: no IOPCIDevice on it", fCycle, when, bus);
            record(line);
            continue;
        }
        unsigned present = 0, flagged = 0;
        for (UInt8 devNum = 0; devNum < 32; devNum++) {
            for (UInt8 fn = 0; fn < 8; fn++) {
                IOPCIAddressSpace space;
                space.bits = 0;
                space.s.busNum = bus;
                space.s.deviceNum = devNum & 0x1F;
                space.s.functionNum = fn & 0x7;
                UInt32 ids = gw->configRead32(space, 0x00);
                if (ids == 0xFFFFFFFF || ids == 0 || (ids & 0xFFFF) == 0xFFFF) {
                    if (fn == 0) {
                        break;
                    }
                    continue;
                }
                present++;
                UInt8  header = gw->configRead8(space, 0x0E);
                UInt16 status = gw->configRead16(space, 0x06);
                UInt16 pmcs = 0;
                bool   hasPm = false;
                if (status & 0x10) {    // capability list present
                    UInt8 ptr = gw->configRead8(space, 0x34) & 0xFC;
                    for (unsigned guard = 0; ptr >= 0x40 && guard < 48; guard++) {
                        UInt16 cap = gw->configRead16(space, ptr);
                        if ((cap & 0xFF) == kIOPCIPowerManagementCapability) {
                            pmcs = gw->configRead16(space, (UInt8)(ptr + 4));
                            hasPm = true;
                            break;
                        }
                        ptr = (UInt8)((cap >> 8) & 0xFC);
                    }
                }
                if (hasPm && (pmcs & 0x8100)) {
                    flagged++;
                    snprintf(line, sizeof(line),
                             "UP: c%u %s scan %02x:%02x.%x %04x:%04x class=%06x pmcs=%04x%s%s",
                             fCycle, when, bus, devNum, fn, ids & 0xFFFF, ids >> 16,
                             gw->configRead32(space, 0x08) >> 8, pmcs,
                             (pmcs & 0x8000) ? " PME_Status" : "",
                             (pmcs & 0x0100) ? " PME_En" : "");
                    record(line);
                }
                if (fn == 0 && !(header & 0x80)) {
                    break;  // single-function device
                }
            }
        }
        snprintf(line, sizeof(line), "UP: c%u %s scan bus %02x: %u functions, %u with PME flags",
                 fCycle, when, bus, present, flagged);
        record(line);
        gw->release();
    }
}

// Snapshots stored by SSDT-UltraSleep-Debug in \_SB.PRR2 (layout WSPV = 3).
static const char *const kPtsNames[] = {
    "PTSN", "PCHB", "PA00", "PA10", "PA11", "PA12", "PAS0", "PB00", "PB10", "PB11", "PB12", "PBS0",
    "PC00", "PC10", "PCS0", "PCE0", "PCE1", "PCES"
};
static const char *const kWakNames[] = {
    "WAKN", "WAP1", "WA00", "WA10", "WA11", "WA12", "WAS0"
};

static void appendNames(IOACPIPlatformDevice *dev, char *line, size_t size, int pos,
                        const char *const *names, unsigned count)
{
    for (unsigned i = 0; i < count && pos > 0 && pos < (int)size; i++) {
        UInt32   value = 0;
        IOReturn ret = dev->evaluateInteger(names[i], &value);
        if (ret == kIOReturnSuccess) {
            pos += snprintf(line + pos, size - (size_t)pos, " %s=%x", names[i], value);
        } else {
            pos += snprintf(line + pos, size - (size_t)pos, " %s=err%x", names[i], ret);
        }
    }
}

void UltraProbe::dumpAcpiSnapshots(const char *when)
{
    IOACPIPlatformDevice *prr2 = findAcpiDevice("PRR2");
    if (!prr2) {
        return;
    }
    UInt32 layout = 0;
    if (prr2->evaluateInteger("WSPV", &layout) != kIOReturnSuccess || layout != 3) {
        prr2->release();
        return;     // SSDT-UltraSleep-Debug not loaded
    }
    char line[288];
    int  pos = snprintf(line, sizeof(line), "UP: c%u %s PTS", fCycle, when);
    appendNames(prr2, line, sizeof(line), pos, kPtsNames, sizeof(kPtsNames) / sizeof(kPtsNames[0]));
    record(line);

    pos = snprintf(line, sizeof(line), "UP: c%u %s WAK", fCycle, when);
    appendNames(prr2, line, sizeof(line), pos, kWakNames, sizeof(kWakNames) / sizeof(kWakNames[0]));
    record(line);
    prr2->release();
}

// Walks the whole ACPI plane: most ACPI nodes (XHCI, PXSX) are not registered
// services and a service match would miss them.
void UltraProbe::dumpWakeTypes(const char *when)
{
    char line[288];
    int  pos = snprintf(line, sizeof(line), "UP: c%u %s typed:", fCycle, when);

    IORegistryIterator *iter = IORegistryIterator::iterateOver(gIOACPIPlane, kIORegistryIterateRecursively);
    if (iter) {
        IORegistryEntry *obj;
        while ((obj = iter->getNextObject())) {
            IOACPIPlatformDevice *dev = OSDynamicCast(IOACPIPlatformDevice, obj);
            UInt32 type = 0, gpe = 0xFFFF;
            if (!dev || !copyNumber(dev, "acpi-wake-type", &type)) {
                continue;
            }
            copyNumber(dev, "acpi-wake-gpe", &gpe);
            IORegistryEntry *parent = dev->getParentEntry(gIOACPIPlane);
            if (pos > 0 && pos < (int)sizeof(line)) {
                pos += snprintf(line + pos, sizeof(line) - (size_t)pos, " %s.%s(gpe %x)=%u",
                                parent && parent->getName() ? parent->getName() : "?",
                                dev->getName() ? dev->getName() : "?", gpe, type);
            }
        }
        iter->release();
    }
    record(line);

    IOPMrootDomain *root = getPMRootDomain();
    if (root) {
        OSObject *wto = root->copyProperty("Wake Type");
        OSObject *wro = root->copyProperty("Wake Reason");
        OSString *wt = OSDynamicCast(OSString, wto);
        OSString *wr = OSDynamicCast(OSString, wro);
        snprintf(line, sizeof(line), "UP: c%u %s root Wake Type=\"%s\" Wake Reason=\"%.60s\"", fCycle, when,
                 wt ? wt->getCStringNoCopy() : "", wr ? wr->getCStringNoCopy() : "");
        OSSafeReleaseNULL(wto);
        OSSafeReleaseNULL(wro);
        record(line);
    }
}

// ---------------------------------------------------------------------------
// History

void UltraProbe::record(const char *line)
{
    IOLog("%s\n", line);
    if (!fHistory || !fLock) {
        return;
    }
    OSString *s = OSString::withCString(line);
    if (!s) {
        return;
    }
    IOLockLock(fLock);
    fHistory->setObject(s);
    while (fHistory->getCount() > kHistoryMax) {
        fHistory->removeObject(0);
    }
    IOLockUnlock(fLock);
    s->release();
}

// Publishes a copy, so a reader serializing the property never sees the
// array change under it.
void UltraProbe::publish()
{
    if (!fHistory || !fLock) {
        return;
    }
    IOLockLock(fLock);
    OSArray *copy = OSArray::withArray(fHistory);
    IOLockUnlock(fLock);
    if (copy) {
        setProperty("UltraProbeLog", copy);
        copy->release();
    }
}
