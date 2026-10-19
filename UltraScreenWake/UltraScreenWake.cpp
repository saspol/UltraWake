// UltraScreenWake: turn a USB-keyboard DarkWake into a full wake.
//
// On Arrow Lake-S / Z890 desktops a key press wakes the system from S3
// through the PCH XHCI wake event (PCH GPE second tier, S210 bit 0x200).
// macOS comes up in DarkWake (no display) because:
//   - the platform driver does not publish Wake Type "User" for this GPE, and
//     on x86 IOPMrootDomain leaves DarkWake by itself only for "User"/"Alarm";
//   - the PCH XHCI loses power in S3 and every USB device is enumerated again
//     on wake, so the key press that woke the system never reaches IOHIDFamily
//     and the display wrangler is not tickled until the next key press.
//
// UltraScreenWake waits for the DarkWake (kIOMessageSystemCapabilityChange,
// kIOPMSystemCapabilityDidChange, from no capabilities to CPU without
// Graphics), reads the PCH wake status register and, if the XHCI bit is set,
// calls activityTickle(0, 0) on IODisplayWrangler after a short delay - the
// same call an HID event makes and IOPMrootDomain::requestUserActive makes on
// x86. The kernel then requests the full wake ("Requesting full wake due to
// dark wake activity tickle"). A tickle that arrives after the full wake is
// ignored by the kernel.
//
// Requirements:
//   - The XHCI bit must belong to the current wake. UltraSleep clears it in
//     _PTS before every sleep; without it the bit stays set from boot and
//     every DarkWake (Power Nap, network) would be promoted.
//   - The tickle is sent from a timer 1.5 s and again 4 s after the DarkWake
//     notification (TickleDelaysMS); that timing is tested. A tickle sent
//     from inside the notification has not been tested.
//
// UltraScreenWake only reads hardware (one 32-bit I/O port read per DarkWake). The
// PCH ACPI base comes from the ACPI name given in Info.plist (default
// \_SB.PC02.ABAS, the field the firmware stores in the PC02 NVS area).
//
// Info.plist personality properties:
//   PCHACPIDevice     name of the ACPI device that owns the base (PC02)
//   PCHBaseName       name of the base object in that device (ABAS)
//   WakeStatusOffset  offset of the wake status register from the base (0x10)
//   WakeStatusMask    bits that mean a USB wake (0x200, PCH XHCI)
//   TickleDelaysMS    delays of the tickles after the DarkWake ([1500, 4000])
//
// State for `ioreg -rc UltraScreenWake -w0`: PCHBase, DarkWakes, USBWakes,
// Tickles, LastWakeStatus.

#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOMessage.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/IOLocks.h>
#include <IOKit/pwr_mgt/RootDomain.h>
#include <IOKit/pwr_mgt/IOPM.h>
#include <IOKit/acpi/IOACPIPlatformDevice.h>
#include <libkern/c++/OSArray.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSString.h>
#include <architecture/i386/pio.h>

static const unsigned kMaxTickles = 4;

class UltraScreenWake : public IOService
{
    OSDeclareDefaultStructors(UltraScreenWake)

public:
    bool start(IOService *provider) override;
    void stop(IOService *provider) override;
    void free() override;

private:
    IONotifier         *fNotifier;
    IOWorkLoop         *fWorkLoop;
    IOTimerEventSource *fTimer;
    IOLock             *fLock;           // fTickle* between power thread and timer

    char                fPchDevice[8];
    char                fBaseName[8];
    UInt32              fStatusOffset;
    UInt32              fStatusMask;
    UInt32              fDelays[kMaxTickles];
    unsigned            fDelayCount;

    UInt16              fPchBase;        // 0 until resolved
    unsigned            fTickleIndex;    // next delay to use
    unsigned            fTicklesPending;

    UInt32              fDarkWakes;
    UInt32              fUsbWakes;
    UInt32              fTickles;

    static IOReturn powerHandler(void *target, void *refCon, UInt32 messageType,
                                 IOService *provider, void *messageArgument,
                                 vm_size_t argSize);
    static void timerFired(OSObject *owner, IOTimerEventSource *sender);

    void readConfig();
    bool resolvePchBase();
    void onDarkWake();
    void publishCounters(UInt32 lastStatus);
    void setNumber(const char *key, UInt64 value, unsigned bits);
};

OSDefineMetaClassAndStructors(UltraScreenWake, IOService)

// Copy a short string property of this service into buf; keep the default
// when the property is missing or does not fit.
static void copyString(IORegistryEntry *entry, const char *key, char *buf, size_t size)
{
    OSObject *obj = entry->copyProperty(key);
    OSString *str = OSDynamicCast(OSString, obj);
    if (str && str->getLength() > 0 && str->getLength() < size) {
        strlcpy(buf, str->getCStringNoCopy(), size);
    }
    OSSafeReleaseNULL(obj);
}

static UInt32 copyNumber(IORegistryEntry *entry, const char *key, UInt32 fallback)
{
    OSObject *obj = entry->copyProperty(key);
    OSNumber *num = OSDynamicCast(OSNumber, obj);
    UInt32    value = num ? num->unsigned32BitValue() : fallback;
    OSSafeReleaseNULL(obj);
    return value;
}

void UltraScreenWake::readConfig()
{
    strlcpy(fPchDevice, "PC02", sizeof(fPchDevice));
    strlcpy(fBaseName, "ABAS", sizeof(fBaseName));
    copyString(this, "PCHACPIDevice", fPchDevice, sizeof(fPchDevice));
    copyString(this, "PCHBaseName", fBaseName, sizeof(fBaseName));
    fStatusOffset = copyNumber(this, "WakeStatusOffset", 0x10) & 0xFC;
    fStatusMask = copyNumber(this, "WakeStatusMask", 0x200);

    fDelayCount = 0;
    OSObject *obj = copyProperty("TickleDelaysMS");
    OSArray  *arr = OSDynamicCast(OSArray, obj);
    if (arr) {
        for (unsigned i = 0; i < arr->getCount() && fDelayCount < kMaxTickles; i++) {
            OSNumber *num = OSDynamicCast(OSNumber, arr->getObject(i));
            if (num && num->unsigned32BitValue() > 0 && num->unsigned32BitValue() <= 30000) {
                fDelays[fDelayCount++] = num->unsigned32BitValue();
            }
        }
    }
    OSSafeReleaseNULL(obj);
    if (fDelayCount == 0) {
        fDelays[0] = 1500;
        fDelays[1] = 4000;
        fDelayCount = 2;
    }
}

bool UltraScreenWake::start(IOService *provider)
{
    if (!IOService::start(provider)) {
        return false;
    }
    readConfig();

    fLock = IOLockAlloc();
    fWorkLoop = IOWorkLoop::workLoop();
    fTimer = IOTimerEventSource::timerEventSource(this, &UltraScreenWake::timerFired);
    if (!fLock || !fWorkLoop || !fTimer || fWorkLoop->addEventSource(fTimer) != kIOReturnSuccess) {
        IOLog("UltraScreenWake: cannot create the timer, not starting\n");
        return false;
    }

    fNotifier = registerPrioritySleepWakeInterest(&UltraScreenWake::powerHandler, this, nullptr);
    if (!fNotifier) {
        IOLog("UltraScreenWake: cannot register for sleep/wake messages, not starting\n");
        return false;
    }

    publishCounters(0);
    IOLog("UltraScreenWake: started, %s.%s + 0x%x mask 0x%x, %u tickle(s) from %u ms\n",
          fPchDevice, fBaseName, fStatusOffset, fStatusMask, fDelayCount, fDelays[0]);
    registerService();
    return true;
}

void UltraScreenWake::stop(IOService *provider)
{
    if (fNotifier) {
        fNotifier->remove();
        fNotifier = nullptr;
    }
    if (fTimer) {
        fTimer->cancelTimeout();
        if (fWorkLoop) {
            fWorkLoop->removeEventSource(fTimer);
        }
        OSSafeReleaseNULL(fTimer);
    }
    OSSafeReleaseNULL(fWorkLoop);
    IOService::stop(provider);
}

void UltraScreenWake::free()
{
    // start() may have failed half way; release what it created.
    if (fTimer) {
        fTimer->cancelTimeout();
        if (fWorkLoop) {
            fWorkLoop->removeEventSource(fTimer);
        }
        OSSafeReleaseNULL(fTimer);
    }
    OSSafeReleaseNULL(fWorkLoop);
    if (fLock) {
        IOLockFree(fLock);
        fLock = nullptr;
    }
    IOService::free();
}

IOReturn UltraScreenWake::powerHandler(void *target, void *refCon, UInt32 messageType,
                                 IOService *provider, void *messageArgument,
                                 vm_size_t argSize)
{
    UltraScreenWake *self = static_cast<UltraScreenWake *>(target);
    if (!self || messageType != kIOMessageSystemCapabilityChange || !messageArgument) {
        return kIOReturnSuccess;
    }
    const IOPMSystemCapabilityChangeParameters *cap =
        static_cast<const IOPMSystemCapabilityChangeParameters *>(messageArgument);
    if ((cap->changeFlags & kIOPMSystemCapabilityDidChange) &&
        cap->fromCapabilities == 0 &&
        (cap->toCapabilities & kIOPMSystemCapabilityCPU) &&
        !(cap->toCapabilities & kIOPMSystemCapabilityGraphics)) {
        self->onDarkWake();
    }
    return kIOReturnSuccess;
}

// \_SB.<PCHACPIDevice>.<PCHBaseName> through ACPI, read once. ACPI devices
// are registered long after this kext starts, so this runs on the first
// DarkWake.
bool UltraScreenWake::resolvePchBase()
{
    if (fPchBase) {
        return true;
    }
    OSDictionary *match = IOService::serviceMatching("IOACPIPlatformDevice");
    OSIterator   *iter = match ? IOService::getMatchingServices(match) : nullptr;
    OSSafeReleaseNULL(match);
    if (!iter) {
        return false;
    }
    IOACPIPlatformDevice *dev = nullptr;
    OSObject *obj;
    while ((obj = iter->getNextObject())) {
        IOACPIPlatformDevice *acpi = OSDynamicCast(IOACPIPlatformDevice, obj);
        if (acpi && acpi->getName() && !strncmp(acpi->getName(), fPchDevice, sizeof(fPchDevice))) {
            dev = acpi;
            dev->retain();
            break;
        }
    }
    iter->release();
    if (!dev) {
        IOLog("UltraScreenWake: ACPI device %s not found\n", fPchDevice);
        return false;
    }

    UInt32   base = 0;
    IOReturn ret = dev->evaluateInteger(fBaseName, &base);
    dev->release();
    // An ACPI I/O base: in I/O space, 256-byte aligned, not the legacy range.
    if (ret != kIOReturnSuccess || base < 0x400 || base > 0xFF00 || (base & 0xFF)) {
        IOLog("UltraScreenWake: %s.%s = 0x%x (ret 0x%x) is not an I/O base\n", fPchDevice, fBaseName, base, ret);
        return false;
    }
    fPchBase = (UInt16)base;
    setNumber("PCHBase", fPchBase, 16);
    return true;
}

void UltraScreenWake::onDarkWake()
{
    fDarkWakes++;
    if (!resolvePchBase()) {
        publishCounters(0);
        return;
    }
    UInt32 status = inl((UInt16)(fPchBase + fStatusOffset));
    if (status & fStatusMask) {
        fUsbWakes++;
        IOLockLock(fLock);
        fTickleIndex = 0;
        fTicklesPending = fDelayCount;
        IOLockUnlock(fLock);
        fTimer->setTimeoutMS(fDelays[0]);
    }
    publishCounters(status);
    IOLog("UltraScreenWake: DarkWake, wake status 0x%08x%s\n", status,
          (status & fStatusMask) ? ", USB wake: promoting" : "");
}

void UltraScreenWake::timerFired(OSObject *owner, IOTimerEventSource *sender)
{
    UltraScreenWake *self = OSDynamicCast(UltraScreenWake, owner);
    if (!self) {
        return;
    }
    IOLockLock(self->fLock);
    if (!self->fTicklesPending) {
        IOLockUnlock(self->fLock);
        return;
    }
    self->fTicklesPending--;
    unsigned index = self->fTickleIndex++;
    bool     more = self->fTicklesPending > 0;
    IOLockUnlock(self->fLock);

    OSDictionary *match = IOService::serviceMatching("IODisplayWrangler");
    IOService    *wrangler = match ? IOService::copyMatchingService(match) : nullptr;
    OSSafeReleaseNULL(match);
    if (wrangler) {
        wrangler->activityTickle(0, 0);
        wrangler->release();
        self->fTickles++;
        self->setNumber("Tickles", self->fTickles, 32);
    } else {
        IOLog("UltraScreenWake: IODisplayWrangler not found\n");
    }

    if (more && index + 1 < self->fDelayCount) {
        UInt32 next = self->fDelays[index + 1];
        UInt32 prev = self->fDelays[index];
        sender->setTimeoutMS(next > prev ? next - prev : 1);
    }
}

void UltraScreenWake::setNumber(const char *key, UInt64 value, unsigned bits)
{
    OSNumber *num = OSNumber::withNumber(value, bits);
    if (num) {
        setProperty(key, num);
        num->release();
    }
}

void UltraScreenWake::publishCounters(UInt32 lastStatus)
{
    setNumber("DarkWakes", fDarkWakes, 32);
    setNumber("USBWakes", fUsbWakes, 32);
    setNumber("Tickles", fTickles, 32);
    setNumber("LastWakeStatus", lastStatus, 32);
}
