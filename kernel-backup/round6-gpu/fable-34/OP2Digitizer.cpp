/*
 * Built-in IOHIDEventService Digitizer for OnePlus 2 (MSM8994).
 *
 * backboardd's BKDigitizerArbiterAccess matches BKIOHIDService devices
 * claiming DeviceUsagePage = 13 (kHIDPage_Digitizer) and
 * DeviceUsage = 4 (kHIDUsage_Dig_TouchScreen), requiring Built-In == YES.
 *
 * This service publishes IOCFPlugInTypes pointing to
 * IOHIDFamily.kext/PlugIns/IOHIDLib.plugin (class IOHIDEventServiceClass).
 * IOHIDEventServiceClass maps an IOSharedDataQueue from this driver
 * via clientMemoryForType(0).
 *
 * Digitizer touch events are packaged as serialized IOHIDEvent packets
 * and enqueued into the IOSharedDataQueue, waking backboardd's event loop.
 */

#include <pexpert/arm64/board_config.h>

#if defined(ARM64_BOARD_CONFIG_MSM8994)
#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IOSharedDataQueue.h>
#include <IOKit/IOKitKeys.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/IOTimerEventSource.h>
#include <pexpert/pexpert.h>
#include <kern/clock.h>
#include <mach/mach_time.h>

#pragma pack(push, 1)
/* IOHIDSystemQueueElement (IOHIDEventData.h, IOHIDFamily-1090/iOS 12) */
struct IOHIDPacketHeader {
	uint64_t timestamp;       /* mach_absolute_time() */
	uint64_t senderID;        /* sender ID */
	uint32_t options;         /* 0 */
	uint32_t attributeLength; /* 0 */
	uint32_t eventCount;      /* 1 (MUST be >= 1 for _IOHIDEventCreateWithBytes) */
};

/* IOHIDDigitizerEventData (IOHIDEventStructDefs.h) + IOHIDEvent::digitizerEvent
 * (IOHIDEvent.cpp): finger => transducerType 2, identity 2, orientation
 * quality w/ major/minor radius 5.0. transducerIndex is the stable path ID. */
struct IOHIDDigitizerRecord {
	uint32_t size;            /* 0x00: 0x68 (104 bytes) */
	uint32_t type;            /* 0x04: 0x0b (11, kIOHIDEventTypeDigitizer) */
	uint32_t options;         /* 0x08: absolute|range|touch */
	uint32_t depth;           /* 0x0c: 0 */
	int32_t  x;               /* 0x10: Fixed 16.16 (fraction of display) */
	int32_t  y;               /* 0x14: Fixed 16.16 */
	int32_t  z;               /* 0x18: 0 */
	uint32_t transducerIndex; /* 0x1c: stable path ID (1) */
	uint32_t transducerType;  /* 0x20: 2 = finger */
	uint32_t identity;        /* 0x24: 2 = finger */
	uint32_t eventMask;       /* 0x28: 0 */
	uint32_t childEventMask;  /* 0x2c: 0 */
	uint32_t buttonMask;      /* 0x30: 1 down / 0 up */
	int32_t  pressure;        /* 0x34: Fixed 16.16 tip pressure */
	int32_t  auxPressure;     /* 0x38: 0 */
	int32_t  twist;           /* 0x3c: 0 */
	uint32_t orientationType; /* 0x40: 2 = quality */
	int32_t  orientation[6];  /* 0x44: [3]=[4]=[5] = major/minor radius 5.0 */
	uint32_t generationCount; /* 0x5c: 0 */
	uint32_t willUpdateMask;  /* 0x60: 0 */
	uint32_t didUpdateMask;   /* 0x64: 0 */
};

/* Multitouch frame: collection parent + 1 transducer child (backboardd's
 * arbiter requires >= 1 child; lone transducer events are dropped).
 * Serialized as header(eventCount=2) + parent + child (IOHIDEvent.cpp). */
struct IOHIDDigitizerPacket {
	IOHIDPacketHeader    hdr;
	IOHIDDigitizerRecord parent;
	IOHIDDigitizerRecord child;
};

struct IOHIDKeyboardRecord {
	uint32_t size;            /* 0x00: 0x18 (24 bytes) */
	uint32_t type;            /* 0x04: 0x03 (kIOHIDEventTypeKeyboard) */
	uint32_t options;         /* 0x08: 0 */
	uint32_t depth;           /* 0x0c: 0 */
	uint16_t usagePage;       /* 0x10: 0x000c (Consumer) */
	uint16_t usage;           /* 0x12: 0x0040 (Menu / Home Button) */
	uint32_t down;            /* 0x14: 1 if pressed, 0 if released */
};

struct IOHIDKeyboardPacket {
	IOHIDPacketHeader   hdr;
	IOHIDKeyboardRecord rec;
};
#pragma pack(pop)
static_assert(sizeof(struct IOHIDPacketHeader) == 28, "IOHIDPacketHeader size mismatch");
static_assert(sizeof(struct IOHIDDigitizerRecord) == 104, "IOHIDDigitizerRecord size mismatch");
static_assert(sizeof(struct IOHIDDigitizerPacket) == 236, "IOHIDDigitizerPacket size mismatch");
static_assert(sizeof(struct IOHIDKeyboardRecord) == 24, "IOHIDKeyboardRecord size mismatch");
static_assert(sizeof(struct IOHIDKeyboardPacket) == 52, "IOHIDKeyboardPacket size mismatch");

#pragma pack(push, 1)
struct IOHIDElementStruct {
	uint32_t cookieMin;          /* 0x00 */
	uint32_t cookieMax;          /* 0x04 */
	uint32_t parentCookie;       /* 0x08 */
	uint32_t type;               /* 0x0c */
	uint32_t collectionType;     /* 0x10 */
	uint32_t flags;              /* 0x14 */
	uint32_t usagePage;          /* 0x18 */
	uint32_t usageMin;           /* 0x1c */
	uint32_t usageMax;           /* 0x20 */
	int32_t  min;                /* 0x24 */
	int32_t  max;                /* 0x28 */
	int32_t  scaledMin;          /* 0x2c */
	int32_t  scaledMax;          /* 0x30 */
	uint32_t size;               /* 0x34 */
	uint32_t reportSize;         /* 0x38 */
	uint32_t reportCount;        /* 0x3c */
	uint32_t reportID;           /* 0x40 */
	uint32_t unit;               /* 0x44 */
	uint32_t unitExponent;       /* 0x48 */
	uint32_t duplicateValueSize; /* 0x4c */
	uint32_t reserved[4];        /* 0x50..0x5f: pad to 96 bytes (0x60) */
};
#pragma pack(pop)
static_assert(sizeof(struct IOHIDElementStruct) == 96, "IOHIDElementStruct size mismatch");

extern "C" void proc_selfname(char * buf, int size);

class IOHIDDevice : public IOService {
	OSDeclareDefaultStructors(IOHIDDevice);
};

OSDefineMetaClassAndStructors(IOHIDDevice, IOService);

class IOHIDEventService : public IOHIDDevice {
	OSDeclareDefaultStructors(IOHIDEventService);
};

OSDefineMetaClassAndStructors(IOHIDEventService, IOHIDDevice);

class OP2Digitizer : public IOHIDEventService {
	OSDeclareDefaultStructors(OP2Digitizer);

private:
	IOSharedDataQueue  *fQueue;
	IOLock             *fLock;
	IOWorkLoop         *fWorkLoop;
	IOTimerEventSource *fTapTimer;
	uint32_t            fTapStep;
	uint64_t            fSenderID;

public:
	virtual bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
	virtual void stop(IOService *provider) APPLE_KEXT_OVERRIDE;
	virtual void free(void) APPLE_KEXT_OVERRIDE;

	IOSharedDataQueue *getQueue(void) const { return fQueue; }
	void postDigitizerEvent(uint32_t touchDown, uint32_t x_fp16, uint32_t y_fp16, uint32_t z_raw = 0, bool quiet = false);
	void postHomeButtonEvent(uint32_t down);
	void startTapSequence(uint32_t delayMs);
	static void tapTimerFired(OSObject *owner, IOTimerEventSource *sender);
};

OSDefineMetaClassAndStructors(OP2Digitizer, IOHIDEventService);

static OP2Digitizer *gOP2Digitizer = NULL;

class OP2DigitizerUserClient : public IOUserClient {
	OSDeclareDefaultStructors(OP2DigitizerUserClient);

private:
	OP2Digitizer *fOwner;
	task_t        fTask;

public:
	virtual bool initWithTask(task_t owningTask, void *securityToken, UInt32 type) APPLE_KEXT_OVERRIDE;
	virtual bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
	virtual void stop(IOService *provider) APPLE_KEXT_OVERRIDE;
	virtual IOReturn clientClose(void) APPLE_KEXT_OVERRIDE;
	virtual IOReturn registerNotificationPort(mach_port_t port, UInt32 type, io_user_reference_t refCon) APPLE_KEXT_OVERRIDE;
	virtual IOReturn registerNotificationPort(mach_port_t port, UInt32 type, UInt32 refCon) APPLE_KEXT_OVERRIDE;
	virtual IOReturn clientMemoryForType(UInt32 type, IOOptionBits *options, IOMemoryDescriptor **memory) APPLE_KEXT_OVERRIDE;
	virtual IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments *arguments,
	    IOExternalMethodDispatch *dispatch = 0, OSObject *target = 0,
	    void *reference = 0) APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(OP2DigitizerUserClient, IOUserClient);

/* Separate Consumer/ConsumerControl button service (Home). backboardd
 * routes keyboard-typed records only from keyboard-typed services;
 * Menu packets posted on the digitizer queue never had UI effect
 * (46+ AUTOHOMEs and AT-Home all dead). Mirrors the proven ios-sweet
 * SweetHIDButtons split. */
class OP2Buttons : public IOHIDEventService {
	OSDeclareDefaultStructors(OP2Buttons);

private:
	IOSharedDataQueue  *fQueue;
	IOLock             *fLock;
	uint64_t            fSenderID;
	bool                fHasNotificationPort;

public:
	virtual bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
	virtual void stop(IOService *provider) APPLE_KEXT_OVERRIDE;
	virtual void free(void) APPLE_KEXT_OVERRIDE;

	IOSharedDataQueue *getQueue(void) const { return fQueue; }
	bool hasNotificationPort(void) const { return fHasNotificationPort; }
	void setHasNotificationPort(bool v) { fHasNotificationPort = v; }
	void key(uint32_t usage, uint32_t down);
};

OSDefineMetaClassAndStructors(OP2Buttons, IOHIDEventService);

static OP2Buttons *gOP2Buttons = NULL;

class OP2ButtonsUserClient : public IOUserClient {
	OSDeclareDefaultStructors(OP2ButtonsUserClient);

private:
	OP2Buttons *fOwner;
	task_t      fTask;

public:
	virtual bool initWithTask(task_t owningTask, void *securityToken, UInt32 type) APPLE_KEXT_OVERRIDE;
	virtual bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
	virtual void stop(IOService *provider) APPLE_KEXT_OVERRIDE;
	virtual IOReturn clientClose(void) APPLE_KEXT_OVERRIDE;
	virtual IOReturn registerNotificationPort(mach_port_t port, UInt32 type, io_user_reference_t refCon) APPLE_KEXT_OVERRIDE;
	virtual IOReturn registerNotificationPort(mach_port_t port, UInt32 type, UInt32 refCon) APPLE_KEXT_OVERRIDE;
	virtual IOReturn clientMemoryForType(UInt32 type, IOOptionBits *options, IOMemoryDescriptor **memory) APPLE_KEXT_OVERRIDE;
	virtual IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments *arguments,
	    IOExternalMethodDispatch *dispatch = 0, OSObject *target = 0,
	    void *reference = 0) APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(OP2ButtonsUserClient, IOUserClient);

bool
OP2Digitizer::start(IOService *provider)
{
	if (!IOHIDEventService::start(provider)) {
		return false;
	}

	fLock = IOLockAlloc();
	fQueue = IOSharedDataQueue::withCapacity(16384);
	if (!fLock || !fQueue) {
		IOLog("OP2 Digitizer: queue/lock allocation failed\n");
		return false;
	}

	/* Properties required by userland IOHIDEventSystem service matching */
	setProperty("HIDServiceSupport", kOSBooleanTrue);
	setProperty("QueueSize", 16384ULL, 32);

	/* Primary HID usage: Digitizer (13), TouchScreen (4) */
	setProperty("DeviceUsagePage", 13ULL, 32);
	setProperty("DeviceUsage", 4ULL, 32);
	setProperty("PrimaryUsagePage", 13ULL, 32);
	setProperty("PrimaryUsage", 4ULL, 32);
	setProperty("display-integrated", kOSBooleanTrue);

	/* DeviceUsagePairs array required by BKDigitizerArbiterAccess */
	OSArray *pairs = OSArray::withCapacity(1);
	if (pairs) {
		OSDictionary *pair = OSDictionary::withCapacity(2);
		if (pair) {
			OSNumber *numPage = OSNumber::withNumber(13ULL, 32);
			OSNumber *numUsage = OSNumber::withNumber(4ULL, 32);
			pair->setObject("DeviceUsagePage", numPage);
			pair->setObject("DeviceUsage", numUsage);
			OSSafeReleaseNULL(numPage);
			OSSafeReleaseNULL(numUsage);
			pairs->setObject(pair);
			pair->release();
		}
		setProperty("DeviceUsagePairs", pairs);
		pairs->release();
	}

	/* Flags required by backboardd -[BKIOHIDService isBuiltIn] and authentication */
	setProperty("Built-In", kOSBooleanTrue);
	setProperty("Authenticated", kOSBooleanTrue);
	setProperty("Transport", "Built-In");
	setProperty("Product", "OnePlus 2 Touchscreen");
	setProperty("Manufacturer", "Synaptics");
	setProperty("VendorID", 0x06cbULL, 32);
	setProperty("ProductID", 0x3320ULL, 32);

	/* Plugin type UUIDs pointing to stock IOHIDLib.plugin */
	OSDictionary *pluginTypes = OSDictionary::withCapacity(5);
	if (pluginTypes) {
		OSString *libPlugin = OSString::withCString("/System/Library/Extensions/IOHIDFamily.kext/PlugIns/IOHIDLib.plugin");
		pluginTypes->setObject("7DDEECA8-A7B4-11DA-8A0E-0014519758EF", libPlugin);
		pluginTypes->setObject("0516B563-B15B-11DA-96EB-0014519758EF", libPlugin);
		pluginTypes->setObject("0B842ADD-C395-4352-95F6-6585EB515623", libPlugin);
		pluginTypes->setObject("FA12FA38-6F1A-11D4-BA0C-0005028F18D5", libPlugin);
		pluginTypes->setObject("40A57A4E-26A0-11D8-9295-000A958A2C78", libPlugin);
		OSSafeReleaseNULL(libPlugin);
		setProperty("IOCFPlugInTypes", pluginTypes);
		pluginTypes->release();
	}

	setProperty(kIOUserClientClassKey, "OP2DigitizerUserClient");

	fSenderID = getRegistryEntryID();
	fTapStep = 0;

	/* Setup test tap timer to wake / unlock SpringBoard (fallback fires at T=220s if userland does not reschedule) */
	fWorkLoop = IOWorkLoop::workLoop();
	if (fWorkLoop) {
		fTapTimer = IOTimerEventSource::timerEventSource(this, &OP2Digitizer::tapTimerFired);
		if (fTapTimer && fWorkLoop->addEventSource(fTapTimer) == kIOReturnSuccess) {
			fTapTimer->setTimeoutMS(220000);
		}
	}

	gOP2Digitizer = this;
	registerService();
	IOLog("OP2 Digitizer: started and registered as built-in digitizer service (senderID 0x%llx)\n", fSenderID);
	return true;
}

void
OP2Digitizer::stop(IOService *provider)
{
	gOP2Digitizer = NULL;
	if (fTapTimer) {
		fTapTimer->cancelTimeout();
		if (fWorkLoop) {
			fWorkLoop->removeEventSource(fTapTimer);
		}
		fTapTimer->release();
		fTapTimer = NULL;
	}
	if (fWorkLoop) {
		fWorkLoop->release();
		fWorkLoop = NULL;
	}
	IOHIDEventService::stop(provider);
}

void
OP2Digitizer::free(void)
{
	if (fQueue) {
		fQueue->release();
		fQueue = NULL;
	}
	if (fLock) {
		IOLockFree(fLock);
		fLock = NULL;
	}
	IOService::free();
}

extern "C" void op2_digitizer_post(uint32_t touchDown, uint32_t x_fp16, uint32_t y_fp16, uint32_t z_raw);
extern "C" void op2_home_button_post(uint32_t down);

extern "C" void
op2_home_button_post(uint32_t down)
{
	if (gOP2Digitizer != NULL) {
		gOP2Digitizer->postHomeButtonEvent(down);
	} else if (gOP2Buttons != NULL) {
		gOP2Buttons->key(0x0040, down);
	}
}

extern "C" void
op2_digitizer_post(uint32_t touchDown, uint32_t x_fp16, uint32_t y_fp16, uint32_t z_raw)
{
	if (gOP2Digitizer != NULL) {
		gOP2Digitizer->postDigitizerEvent(touchDown, x_fp16, y_fp16, z_raw, true);
	}
}

void
OP2Digitizer::postDigitizerEvent(uint32_t touchDown, uint32_t x_fp16, uint32_t y_fp16, uint32_t z_raw, bool quiet)
{
	if (!fQueue) return;

	IOHIDDigitizerPacket pkt;
	bzero(&pkt, sizeof(pkt));

	pkt.hdr.timestamp = mach_absolute_time();
	pkt.hdr.senderID = fSenderID;
	pkt.hdr.options = 0;
	pkt.hdr.attributeLength = 0;
	pkt.hdr.eventCount = 2; /* parent + 1 child */

	/* Parent: collection frame (centroid = finger pos). Not a transducer:
	 * absolute|collection only, no range/touch, cumulative child mask. */
	pkt.parent.size = sizeof(IOHIDDigitizerRecord);
	pkt.parent.type = 0x0b;
	pkt.parent.options = 0x00000003; /* absolute | collection */
	pkt.parent.depth = 0;
	pkt.parent.x = x_fp16;
	pkt.parent.y = y_fp16;
	/* Parent carries the frame's update mask too: ContainsUpdates is
	 * evaluated on the top-level event. No cancel bit (0x80) ever. */
	pkt.parent.eventMask = touchDown ? 0x07 : 0x03;
	pkt.parent.childEventMask = touchDown ? 0x07 : 0x03;

	/* Child: the finger transducer (depth 1). */
	pkt.child.size = sizeof(IOHIDDigitizerRecord);
	pkt.child.type = 0x0b; /* kIOHIDEventTypeDigitizer */
	/* absolute(0x1) | range(0x10000) | touch(0x20000); lift = absolute only */
	pkt.child.options = touchDown ? 0x00030001 : 0x00000001;
	pkt.child.depth = 1;
	pkt.child.x = x_fp16;
	pkt.child.y = y_fp16;
	pkt.child.z = 0;
	pkt.child.transducerIndex = 1;
	pkt.child.transducerType = 2; /* finger */
	pkt.child.identity = 2; /* finger */
	/* Update mask (IOHIDDigitizerEventMask): BKSHIDEventContainsUpdates
	 * gates the arbiter on this. DOWN/MOVE = range|touch|position;
	 * UP = range|touch cleared. Mask 0x00 = "nothing updated" = ignored. */
	pkt.child.eventMask = touchDown ? 0x07 : 0x03;
	pkt.child.childEventMask = 0;
	pkt.child.buttonMask = touchDown ? 1 : 0;
	if (touchDown) {
		pkt.child.pressure = (z_raw != 0) ? (int32_t)((z_raw * 65536U) / 255U) : 0x8000;
	} else {
		pkt.child.pressure = 0;
	}
	pkt.child.auxPressure = 0;
	pkt.child.twist = 0;
	pkt.child.orientationType = 2; /* quality */
	pkt.child.orientation[3] = 5 << 16;
	pkt.child.orientation[4] = 5 << 16;
	pkt.child.orientation[5] = 5 << 16;

	IOLockLock(fLock);
	bool ok = fQueue->enqueue(&pkt, sizeof(pkt));
	IOLockUnlock(fLock);

	if (ok) {
		if (!quiet) {
			IOLog("OP2 Digitizer: posted %s at (%d%%, %d%%) [sender 0x%llx]\n",
			    touchDown ? "TOUCH_DOWN" : "TOUCH_UP",
			    (x_fp16 * 100) >> 16, (y_fp16 * 100) >> 16, fSenderID);
		}
	} else {
		IOLog("OP2 Digitizer: queue full, event dropped\n");
	}
}

void
OP2Digitizer::postHomeButtonEvent(uint32_t down)
{
	/*
	 * Deliver each press ONCE. Posting on both OP2Buttons and the digitizer
	 * queue made backboardd see two presses -> iOS double-click Home ->
	 * app switcher. boot-arg op2_homepath: 1 = OP2Buttons (default),
	 * 2 = digitizer queue, 3 = both (old behaviour).
	 */
	static uint32_t sHomePath = 0;
	if (sHomePath == 0) {
		sHomePath = 1;
		PE_parse_boot_argn("op2_homepath", &sHomePath, sizeof(sHomePath));
	}
	if ((sHomePath & 1) && gOP2Buttons != NULL) {
		gOP2Buttons->key(0x0040, down);
		if (!(sHomePath & 2)) {
			return;
		}
	}
	/* Fallback / mode 2: digitizer queue */
	if (!fQueue) return;

	IOHIDKeyboardPacket pkt;
	bzero(&pkt, sizeof(pkt));

	pkt.hdr.timestamp = mach_absolute_time();
	pkt.hdr.senderID = fSenderID;
	pkt.hdr.options = 0;
	pkt.hdr.attributeLength = 0;
	pkt.hdr.eventCount = 1;

	pkt.rec.size = sizeof(IOHIDKeyboardRecord);
	pkt.rec.type = 0x03; /* kIOHIDEventTypeKeyboard */
	pkt.rec.options = 0;
	pkt.rec.depth = 0;
	pkt.rec.usagePage = 0x000c; /* kHIDPage_Consumer */
	pkt.rec.usage = 0x0040;     /* kHIDUsage_Csmr_Menu (Home Button) */
	pkt.rec.down = down ? 1 : 0;

	IOLockLock(fLock);
	bool ok = fQueue->enqueue(&pkt, sizeof(pkt));
	IOLockUnlock(fLock);

	if (ok) {
		IOLog("OP2 Digitizer: posted HOME_BUTTON_%s [sender 0x%llx]\n",
		    down ? "DOWN" : "UP", fSenderID);
	} else {
		IOLog("OP2 Digitizer: queue full, home button event dropped\n");
	}
}

/* External display synchronization getters from OP2Framebuffer */
extern "C" uint32_t op2_fb_get_last_shown(void);
extern "C" uint32_t op2_fb_get_last_nz(void);

/* Unattended synthetic user: op2_tapms=N re-arms a periodic center-ish
 * tap every N ms after the one-shot setup sequence (default off = single
 * pass). Drives renders for crash reproduction with nobody watching. */
static uint32_t op2_autotap_ms = 0;
static int op2_autotap_parsed = 0;
static uint32_t op2_autoinput_mode = 1; /* boot-arg op2_autoinput */
static uint32_t op2_autotap_count = 0;

/*
 * op2_autoinput=3: the iPhone X gesture test (rounds fable-33+, MobileGestalt HomeButtonType 2). Touch only -- never
 * the Home key, which a phone without one should not see. 120 s after the first tick (SpringBoard up), then a
 * ~70 s cycle forever: unlock swipe, Home swipe, Control Center from the top-right, dismiss, app-switcher
 * swipe-and-hold, Home. Each gesture is logged as "OP2 Digitizer: G3 ..." so the backboardd frame captures
 * (every 40 s) can be read against it.
 */
struct op2_g3_gesture {
	const char *name;
	uint16_t x0, y0, x1, y1;   /* fp16 screen fractions */
	uint8_t samples;           /* moves, 16 ms apart */
	uint16_t hold_ms;          /* finger stays down at the end */
	uint32_t wait_ms;          /* after the lift */
};
static const struct op2_g3_gesture op2_g3[] = {
	{ "unlock: swipe up from the bottom edge",        0x8000, 0xfff0, 0x8000, 0x7000, 14, 0,   6000 },
	{ "home: swipe up from the bottom edge",          0x8000, 0xfff0, 0x8000, 0x7000, 14, 0,   6000 },
	{ "control center: swipe down from top-right",    0xe800, 0x0010, 0xd800, 0x8000, 14, 0,   6000 },
	{ "dismiss: swipe up from the bottom edge",       0x8000, 0xfff0, 0x8000, 0x7000, 14, 0,   6000 },
	{ "app switcher: swipe up and hold",              0x8000, 0xfff0, 0x8000, 0xa800, 20, 800, 6000 },
	{ "home: swipe up from the bottom edge",          0x8000, 0xfff0, 0x8000, 0x7000, 14, 0,  40000 },
};
static uint32_t op2_g3_index, op2_g3_sample, op2_g3_cycle;

static void
op2_g3_tick(OP2Digitizer *me, IOTimerEventSource *sender)
{
	static bool started;
	if (!started) {
		started = true;
		IOLog("OP2 Digitizer: G3 gesture test armed: first gesture in 120 s\n");
		sender->setTimeoutMS(120000);
		return;
	}
	const struct op2_g3_gesture *g = &op2_g3[op2_g3_index];
	uint32_t n = g->samples;
	if (op2_g3_sample == 0)
		IOLog("OP2 Digitizer: G3 cycle %u gesture %u: %s\n", op2_g3_cycle, op2_g3_index, g->name);
	if (op2_g3_sample <= n) {
		/* down at the start, then n moves to the end */
		uint32_t x = g->x0 + (int32_t)(g->x1 - g->x0) * (int32_t)op2_g3_sample / (int32_t)n;
		uint32_t y = g->y0 + (int32_t)(g->y1 - g->y0) * (int32_t)op2_g3_sample / (int32_t)n;
		me->postDigitizerEvent(1, x, y, 0, true);
		op2_g3_sample++;
		sender->setTimeoutMS(op2_g3_sample > n && g->hold_ms ? g->hold_ms : 16);
		return;
	}
	me->postDigitizerEvent(0, g->x1, g->y1, 0, true);
	op2_g3_sample = 0;
	if (++op2_g3_index == sizeof op2_g3 / sizeof op2_g3[0]) {
		op2_g3_index = 0;
		op2_g3_cycle++;
	}
	sender->setTimeoutMS(g->wait_ms);
}

void
OP2Digitizer::tapTimerFired(OSObject *owner, IOTimerEventSource *sender)
{
	OP2Digitizer *me = (OP2Digitizer *)owner;
	if (!me) return;

	if (!op2_autotap_parsed) {
		op2_autotap_parsed = 1;
		PE_parse_boot_argn("op2_tapms", &op2_autotap_ms, sizeof(op2_autotap_ms));
		PE_parse_boot_argn("op2_autoinput", &op2_autoinput_mode, sizeof(op2_autoinput_mode));
		IOLog("OP2 Digitizer: synthetic input mode %u (0=off, 1=wake-only, 2=full test script, 3=X gestures)\n",
		    op2_autoinput_mode);
	}

	/*
	 * Synthetic input is a test harness, not a feature. In normal use it
	 * fought the real user (Home presses / swipes / taps every 2.5 s and a
	 * forced re-unlock whenever an app created new surfaces). Default is
	 * wake-only: pulse Home until the Lock Screen is lit, then stop forever.
	 */
	if (op2_autoinput_mode == 0) {
		return;
	}
	if (op2_autoinput_mode == 3) {
		op2_g3_tick(me, sender);
		return;
	}
	if (op2_autoinput_mode == 1 && me->fTapStep != 0 && me->fTapStep != 18) {
		IOLog("OP2 Digitizer: wake-only mode: Lock Screen is up, synthetic input stopped\n");
		return;
	}

	switch (me->fTapStep) {
	case 0: {
		/*
		 * Phase 0: Synchronize with SpringBoard display presentation.
		 * SpringBoard starts in SBDashBoardScreenOff mode on boot.
		 * If no non-zero frame is rendered yet, pulse the Home button
		 * every 6 seconds to turn on the screen.
		 */
		static uint32_t sPollCycles = 0;
		sPollCycles++;
		if (op2_fb_get_last_shown() > 0 && op2_fb_get_last_nz() >= 500) {
			IOLog("OP2 Digitizer: === LOCK SCREEN DETECTED: SpringBoard surface %u active (nz=%u) ===\n",
			    op2_fb_get_last_shown(), op2_fb_get_last_nz());
			/* Lock Screen clock, date, and wallpaper are actively rendering! Proceed to interaction. */
			me->fTapStep = 1;
			sender->setTimeoutMS(2500);
			break;
		}
		if ((sPollCycles % 3) == 0) {
			IOLog("OP2 Digitizer: pulsing Home button to wake display (poll cycle %u)...\n", sPollCycles);
			me->postHomeButtonEvent(1);
			me->fTapStep = 18;
			sender->setTimeoutMS(120);
			break;
		}
		sender->setTimeoutMS(2000);
		break;
	}

	case 1:
		/* Phase 1: Lock Screen interaction - test touch delivery */
		IOLog("OP2 Digitizer: [LOCK SCREEN] test touch down on Lock Screen clock\n");
		me->postDigitizerEvent(1, 0x8000, 0x6000);
		me->fTapStep = 2;
		sender->setTimeoutMS(100);
		break;

	case 2:
		IOLog("OP2 Digitizer: [LOCK SCREEN] test touch up\n");
		me->postDigitizerEvent(0, 0x8000, 0x6000);
		me->fTapStep = 3;
		sender->setTimeoutMS(1500);
		break;

	case 3:
		/* Prime / Wake Home button press */
		IOLog("OP2 Digitizer: [LOCK SCREEN] Home button press (wake/prime)\n");
		me->postHomeButtonEvent(1);
		me->fTapStep = 4;
		sender->setTimeoutMS(120);
		break;

	case 4:
		IOLog("OP2 Digitizer: [LOCK SCREEN] Home button release\n");
		me->postHomeButtonEvent(0);
		/* Wait 2200ms for wake animation to finish so SpringBoard is in awake state */
		me->fTapStep = 5;
		sender->setTimeoutMS(2200);
		break;

	case 5:
		/*
		 * Phase 2: Home Screen Unlock Transition!
		 * Screen is awake (isInScreenOffMode == false, _isWakeAnimationInProgress == false).
		 * Press Home button to dismiss Lock Screen.
		 */
		IOLog("OP2 Digitizer: === UNLOCKING TO HOME SCREEN: pressing Home button while awake ===\n");
		me->postHomeButtonEvent(1);
		me->fTapStep = 6;
		sender->setTimeoutMS(120);
		break;

	case 6:
		IOLog("OP2 Digitizer: [HOME SCREEN] Home button released -> dismissing Lock Screen to Home Screen\n");
		me->postHomeButtonEvent(0);
		/* Wait 2500ms for unlock transition to complete and Home Screen icon grid to settle */
		me->fTapStep = 7;
		sender->setTimeoutMS(2500);
		break;

	case 7:
		/*
		 * Phase 3: Home Screen Icon Grid Navigation!
		 * Swipe horizontally left from right edge to center (x=0.80 -> x=0.20) to transition pages.
		 */
		IOLog("OP2 Digitizer: === HOME SCREEN ICON GRID: swiping left to page 2 (x=0.8 -> x=0.2) ===\n");
		me->postDigitizerEvent(1, 0xcccc, 0x8000);
		me->fTapStep = 8;
		sender->setTimeoutMS(60);
		break;

	case 8:
		me->postDigitizerEvent(1, 0x8000, 0x8000);
		me->fTapStep = 9;
		sender->setTimeoutMS(60);
		break;

	case 9:
		me->postDigitizerEvent(1, 0x3333, 0x8000);
		me->fTapStep = 10;
		sender->setTimeoutMS(60);
		break;

	case 10:
		IOLog("OP2 Digitizer: [HOME SCREEN] swipe left complete (page 2 displayed)\n");
		me->postDigitizerEvent(0, 0x3333, 0x8000);
		me->fTapStep = 11;
		sender->setTimeoutMS(2000);
		break;

	case 11:
		/* Swipe right back to main icon page */
		IOLog("OP2 Digitizer: === HOME SCREEN ICON GRID: swiping right back to main page (x=0.2 -> x=0.8) ===\n");
		me->postDigitizerEvent(1, 0x3333, 0x8000);
		me->fTapStep = 12;
		sender->setTimeoutMS(60);
		break;

	case 12:
		me->postDigitizerEvent(1, 0x8000, 0x8000);
		me->fTapStep = 13;
		sender->setTimeoutMS(60);
		break;

	case 13:
		me->postDigitizerEvent(1, 0xcccc, 0x8000);
		me->fTapStep = 14;
		sender->setTimeoutMS(60);
		break;

	case 14:
		IOLog("OP2 Digitizer: [HOME SCREEN] swipe right complete (main icon page displayed)\n");
		me->postDigitizerEvent(0, 0xcccc, 0x8000);
		me->fTapStep = 15;
		sender->setTimeoutMS(2000);
		break;

	case 15: {
		/*
		 * Phase 4: Continuous Home Screen Keep-Alive & Interactive Loop.
		 * Defeats idle timer and keeps screen permanently illuminated.
		 * Detects if SpringBoard crashed & respawned: when SpringBoard respawns,
		 * surface ID advances by >= 2 (e.g. from 2 to 10). When detected,
		 * reset state machine to Phase 0 to automatically wake and unlock back to Home Screen!
		 */
		static uint32_t s_last_tracked_shown = 0;
		uint32_t cur_shown = op2_fb_get_last_shown();
		if (s_last_tracked_shown != 0 && cur_shown >= s_last_tracked_shown + 2) {
			IOLog("OP2 Digitizer: SpringBoard restart detected (surface jumped %u -> %u)! Resetting unlock sequence...\n",
			    s_last_tracked_shown, cur_shown);
			s_last_tracked_shown = cur_shown;
			me->fTapStep = 0;
			sender->setTimeoutMS(1500);
			break;
		}
		if (s_last_tracked_shown == 0) {
			s_last_tracked_shown = cur_shown;
		}

		/* Pure micro-tap keepalive (keeps display awake without button triggers) */
		uint32_t n = ++op2_autotap_count;
		if ((n % 40) == 0) {
			IOLog("OP2 Digitizer: micro-tap keepalive active (tick %u)\n", n);
		}
		me->postDigitizerEvent(1, 0x0100, 0x0100, 0, true);
		me->fTapStep = 16;
		sender->setTimeoutMS(50);
		break;
	}
	case 16: {
		me->postDigitizerEvent(0, 0x0100, 0x0100, 0, true);
		me->fTapStep = 15;
		sender->setTimeoutMS(2500); /* 2.5s interval keeps screen permanently awake! */
		break;
	}
	case 17:
		me->postHomeButtonEvent(0);
		/* Wait 1200ms for wake, then pulse Home a second time to unlock if screen was locked */
		me->fTapStep = 19;
		sender->setTimeoutMS(1200);
		break;

	case 18:
		me->postHomeButtonEvent(0);
		me->fTapStep = 0;
		sender->setTimeoutMS(1500);
		break;

	case 19:
		IOLog("OP2 Digitizer: [HOME SCREEN] periodic Home unlock pulse\n");
		me->postHomeButtonEvent(1);
		me->fTapStep = 20;
		sender->setTimeoutMS(120);
		break;

	case 20:
		me->postHomeButtonEvent(0);
		me->fTapStep = 15;
		sender->setTimeoutMS(2500);
		break;

	default:
		break;
	}
}

void
OP2Digitizer::startTapSequence(uint32_t delayMs)
{
	if (fTapTimer) {
		fTapTimer->cancelTimeout();
		fTapStep = 0;
		fTapTimer->setTimeoutMS(delayMs);
		IOLog("OP2 Digitizer: scheduled simulated tap sequence in %u ms\n", delayMs);
	}
}

/* User Client implementation */
bool
OP2DigitizerUserClient::initWithTask(task_t owningTask, void *securityToken, UInt32 type)
{
	if (!IOUserClient::initWithTask(owningTask, securityToken, type)) {
		return false;
	}
	fTask = owningTask;
	fOwner = NULL;
	return true;
}

bool
OP2DigitizerUserClient::start(IOService *provider)
{
	if (!IOUserClient::start(provider)) {
		return false;
	}
	fOwner = OSDynamicCast(OP2Digitizer, provider);
	if (!fOwner) {
		return false;
	}
	char name[32] = {0};
	proc_selfname(name, sizeof(name));
	IOLog("OP2 Digitizer: user client started for task %p (%s)\n", (void *)fTask, name);
	return true;
}

void
OP2DigitizerUserClient::stop(IOService *provider)
{
	fOwner = NULL;
	IOUserClient::stop(provider);
}

IOReturn
OP2DigitizerUserClient::clientClose(void)
{
	char name[32] = {0};
	proc_selfname(name, sizeof(name));
	IOLog("OP2 Digitizer: client close called by %s\n", name);
	terminate();
	return kIOReturnSuccess;
}

IOReturn
OP2DigitizerUserClient::registerNotificationPort(mach_port_t port, UInt32 type, io_user_reference_t refCon)
{
	if (fOwner && fOwner->getQueue()) {
		char name[32] = {0};
		proc_selfname(name, sizeof(name));
		if (strcmp(name, "backboardd") == 0) {
			IOLog("OP2 Digitizer: backboardd set notification port %p (refCon %llu)\n", (void *)port, refCon);
			fOwner->getQueue()->setNotificationPort(port);
			/* Start checking for SpringBoard UI presentation immediately (polls every 2s in case 0) */
			fOwner->startTapSequence(2000);
		} else {
			IOLog("OP2 Digitizer: ignoring notification port from %s (preserving backboardd port)\n", name);
		}
		return kIOReturnSuccess;
	}
	return kIOReturnNotReady;
}

IOReturn
OP2DigitizerUserClient::registerNotificationPort(mach_port_t port, UInt32 type, UInt32 refCon)
{
	return registerNotificationPort(port, type, (io_user_reference_t)refCon);
}

IOReturn
OP2DigitizerUserClient::clientMemoryForType(UInt32 type, IOOptionBits *options, IOMemoryDescriptor **memory)
{
	if (type == 0 && fOwner && fOwner->getQueue()) {
		IOMemoryDescriptor *md = fOwner->getQueue()->getMemoryDescriptor();
		if (md) {
			md->retain();
			*memory = md;
			char name[32] = {0};
			proc_selfname(name, sizeof(name));
			IOLog("OP2 Digitizer: %s mapped data queue memory (%llu bytes)\n", name, (uint64_t)md->getLength());
			return kIOReturnSuccess;
		}
	}
	return kIOReturnBadArgument;
}

IOReturn
OP2DigitizerUserClient::externalMethod(uint32_t selector, IOExternalMethodArguments *arguments,
    IOExternalMethodDispatch *dispatch, OSObject *target, void *reference)
{
	char name[32] = {0};
	proc_selfname(name, sizeof(name));

	switch (selector) {
	case 0: /* kIOHIDLibUserClientOpen */
		IOLog("OP2 Digitizer: %s client open (selector 0)\n", name);
		return kIOReturnSuccess;

	case 1: /* kIOHIDLibUserClientClose */
		IOLog("OP2 Digitizer: %s client close (selector 1)\n", name);
		return kIOReturnSuccess;

	case 2: /* kIOHIDLibUserClientStop */
		IOLog("OP2 Digitizer: %s client stop (selector 2)\n", name);
		return kIOReturnSuccess;

	case 14: /* kIOHIDLibUserClientGetElementCount */
		if (arguments->scalarOutput && arguments->scalarOutputCount >= 2) {
			arguments->scalarOutput[0] = 1; /* 1 element */
			arguments->scalarOutput[1] = 1; /* 1 report handler */
			arguments->scalarOutputCount = 2;
		}
		IOLog("OP2 Digitizer: %s getElementCount (selector 14) -> elements=1 reports=1\n", name);
		return kIOReturnSuccess;

	case 15: /* kIOHIDLibUserClientGetElements */
		if (arguments->structureOutput && arguments->structureOutputSize >= sizeof(struct IOHIDElementStruct)) {
			struct IOHIDElementStruct *elem = (struct IOHIDElementStruct *)arguments->structureOutput;
			bzero(elem, sizeof(struct IOHIDElementStruct));
			uint32_t type = (arguments->scalarInput && arguments->scalarInputCount > 0) ? (uint32_t)arguments->scalarInput[0] : 0;
			if (type == 0) {
				elem->cookieMin = 1;
				elem->cookieMax = 1;
				elem->parentCookie = 0;
				elem->type = 1; /* kIOHIDElementTypeInput_Misc */
				elem->collectionType = 0;
				elem->flags = 0;
				elem->usagePage = 13; /* kHIDPage_Digitizer */
				elem->usageMin = 4;  /* kHIDUsage_Dig_TouchScreen */
				elem->usageMax = 4;
				elem->min = 0;
				elem->max = 65535;
				elem->scaledMin = 0;
				elem->scaledMax = 65535;
				elem->size = 32;
				elem->reportSize = 32;
				elem->reportCount = 1;
				elem->reportID = 0;
			} else {
				elem->cookieMin = 2;
				elem->cookieMax = 2;
				elem->parentCookie = 0;
				elem->type = 1;
				elem->usagePage = 13;
				elem->usageMin = 4;
				elem->usageMax = 4;
				elem->reportID = 0;
			}
			arguments->structureOutputSize = sizeof(struct IOHIDElementStruct);
			IOLog("OP2 Digitizer: %s getElements (selector 15, type %u) -> returned element struct (%lu bytes)\n",
			    name, type, (unsigned long)sizeof(struct IOHIDElementStruct));
		}
		return kIOReturnSuccess;

	default:
		IOLog("OP2 Digitizer: %s method selector %u (inScalars %u, inStruct %lu)\n",
		    name, selector, arguments->scalarInputCount, (unsigned long)arguments->structureInputSize);
		return kIOReturnSuccess;
	}
}

bool
OP2Buttons::start(IOService *provider)
{
	if (!IOHIDEventService::start(provider)) {
		return false;
	}

	fLock = IOLockAlloc();
	fQueue = IOSharedDataQueue::withCapacity(4096);
	if (!fLock || !fQueue) {
		IOLog("OP2 Buttons: queue/lock allocation failed\n");
		return false;
	}

	/* Properties required by userland IOHIDEventSystem service matching */
	setProperty("HIDServiceSupport", kOSBooleanTrue);
	setProperty("QueueSize", 4096ULL, 32);

	/* Primary HID usage: Consumer (12), ConsumerControl (1) */
	setProperty("DeviceUsagePage", 12ULL, 32);
	setProperty("DeviceUsage", 1ULL, 32);
	setProperty("PrimaryUsagePage", 12ULL, 32);
	setProperty("PrimaryUsage", 1ULL, 32);

	OSArray *pairs = OSArray::withCapacity(1);
	if (pairs) {
		OSDictionary *pair = OSDictionary::withCapacity(2);
		if (pair) {
			OSNumber *numPage = OSNumber::withNumber(12ULL, 32);
			OSNumber *numUsage = OSNumber::withNumber(1ULL, 32);
			pair->setObject("DeviceUsagePage", numPage);
			pair->setObject("DeviceUsage", numUsage);
			OSSafeReleaseNULL(numPage);
			OSSafeReleaseNULL(numUsage);
			pairs->setObject(pair);
			pair->release();
		}
		setProperty("DeviceUsagePairs", pairs);
		pairs->release();
	}

	setProperty("Built-In", kOSBooleanTrue);
	setProperty("Authenticated", kOSBooleanTrue);
	setProperty("Transport", "Built-In");
	setProperty("Product", "OnePlus 2 Buttons");
	setProperty("Manufacturer", "OnePlus");
	setProperty("VendorID", 0x05c6ULL, 32);
	setProperty("ProductID", 0x3321ULL, 32);

	OSDictionary *pluginTypes = OSDictionary::withCapacity(5);
	if (pluginTypes) {
		OSString *libPlugin = OSString::withCString("/System/Library/Extensions/IOHIDFamily.kext/PlugIns/IOHIDLib.plugin");
		pluginTypes->setObject("7DDEECA8-A7B4-11DA-8A0E-0014519758EF", libPlugin);
		pluginTypes->setObject("0516B563-B15B-11DA-96EB-0014519758EF", libPlugin);
		pluginTypes->setObject("0B842ADD-C395-4352-95F6-6585EB515623", libPlugin);
		pluginTypes->setObject("FA12FA38-6F1A-11D4-BA0C-0005028F18D5", libPlugin);
		pluginTypes->setObject("40A57A4E-26A0-11D8-9295-000A958A2C78", libPlugin);
		OSSafeReleaseNULL(libPlugin);
		setProperty("IOCFPlugInTypes", pluginTypes);
		pluginTypes->release();
	}

	setProperty(kIOUserClientClassKey, "OP2ButtonsUserClient");

	fHasNotificationPort = false;
	fSenderID = getRegistryEntryID();
	gOP2Buttons = this;
	registerService();
	IOLog("OP2 Buttons: started and registered as built-in consumer service (senderID 0x%llx)\n", fSenderID);
	return true;
}

void
OP2Buttons::stop(IOService *provider)
{
	gOP2Buttons = NULL;
	IOHIDEventService::stop(provider);
}

void
OP2Buttons::free(void)
{
	if (fQueue) {
		fQueue->release();
		fQueue = NULL;
	}
	if (fLock) {
		IOLockFree(fLock);
		fLock = NULL;
	}
	IOService::free();
}

void
OP2Buttons::key(uint32_t usage, uint32_t down)
{
	if (!fQueue) return;

	IOHIDKeyboardPacket pkt;
	bzero(&pkt, sizeof(pkt));

	pkt.hdr.timestamp = mach_absolute_time();
	pkt.hdr.senderID = fSenderID;
	pkt.hdr.options = 0;
	pkt.hdr.attributeLength = 0;
	pkt.hdr.eventCount = 1;

	pkt.rec.size = sizeof(IOHIDKeyboardRecord);
	pkt.rec.type = 0x03; /* kIOHIDEventTypeKeyboard */
	pkt.rec.options = 0;
	pkt.rec.depth = 0;
	pkt.rec.usagePage = 0x000c; /* kHIDPage_Consumer */
	pkt.rec.usage = (uint16_t)usage;
	pkt.rec.down = down ? 1 : 0;

	IOLockLock(fLock);
	bool ok = fQueue->enqueue(&pkt, sizeof(pkt));
	IOLockUnlock(fLock);

	if (ok) {
		IOLog("OP2 Buttons: posted key 0x%x %s [sender 0x%llx]\n",
		    usage, down ? "DOWN" : "UP", fSenderID);
	} else {
		IOLog("OP2 Buttons: queue full, key event dropped\n");
	}
}

bool
OP2ButtonsUserClient::initWithTask(task_t owningTask, void *securityToken, UInt32 type)
{
	if (!IOUserClient::initWithTask(owningTask, securityToken, type)) {
		return false;
	}
	fTask = owningTask;
	fOwner = NULL;
	return true;
}

bool
OP2ButtonsUserClient::start(IOService *provider)
{
	if (!IOUserClient::start(provider)) {
		return false;
	}
	fOwner = OSDynamicCast(OP2Buttons, provider);
	if (!fOwner) {
		return false;
	}
	char name[32] = {0};
	proc_selfname(name, sizeof(name));
	IOLog("OP2 Buttons: user client started for task %p (%s)\n", (void *)fTask, name);
	return true;
}

void
OP2ButtonsUserClient::stop(IOService *provider)
{
	fOwner = NULL;
	IOUserClient::stop(provider);
}

IOReturn
OP2ButtonsUserClient::clientClose(void)
{
	char name[32] = {0};
	proc_selfname(name, sizeof(name));
	IOLog("OP2 Buttons: client close called by %s\n", name);
	terminate();
	return kIOReturnSuccess;
}

IOReturn
OP2ButtonsUserClient::registerNotificationPort(mach_port_t port, UInt32 type, io_user_reference_t refCon)
{
	if (fOwner && fOwner->getQueue()) {
		char name[32] = {0};
		proc_selfname(name, sizeof(name));
		if (strcmp(name, "backboardd") == 0) {
			IOLog("OP2 Buttons: backboardd set notification port %p (refCon %llu)\n", (void *)port, refCon);
			fOwner->setHasNotificationPort(true);
			fOwner->getQueue()->setNotificationPort(port);
		} else {
			IOLog("OP2 Buttons: ignoring notification port from %s (preserving backboardd port)\n", name);
		}
		return kIOReturnSuccess;
	}
	return kIOReturnNotReady;
}

IOReturn
OP2ButtonsUserClient::registerNotificationPort(mach_port_t port, UInt32 type, UInt32 refCon)
{
	return registerNotificationPort(port, type, (io_user_reference_t)refCon);
}

IOReturn
OP2ButtonsUserClient::clientMemoryForType(UInt32 type, IOOptionBits *options, IOMemoryDescriptor **memory)
{
	if (!fOwner || !fOwner->getQueue()) {
		return kIOReturnNotReady;
	}
	IOMemoryDescriptor *md = fOwner->getQueue()->getMemoryDescriptor();
	if (!md) {
		return kIOReturnNoMemory;
	}
	md->retain();
	*memory = md;
	if (options) {
		*options = 0;
	}
	return kIOReturnSuccess;
}

IOReturn
OP2ButtonsUserClient::externalMethod(uint32_t selector, IOExternalMethodArguments *arguments,
    IOExternalMethodDispatch *dispatch, OSObject *target, void *reference)
{
	char name[32] = {0};
	proc_selfname(name, sizeof(name));

	switch (selector) {
	case 0: /* kIOHIDLibUserClientOpen */
		IOLog("OP2 Buttons: %s client open (selector 0)\n", name);
		return kIOReturnSuccess;

	case 1: /* kIOHIDLibUserClientClose */
		IOLog("OP2 Buttons: %s client close (selector 1)\n", name);
		return kIOReturnSuccess;

	case 2: /* kIOHIDLibUserClientStop */
		IOLog("OP2 Buttons: %s client stop (selector 2)\n", name);
		return kIOReturnSuccess;

	case 14: /* kIOHIDLibUserClientGetElementCount */
		if (arguments->scalarOutput && arguments->scalarOutputCount >= 2) {
			arguments->scalarOutput[0] = 1; /* 1 element */
			arguments->scalarOutput[1] = 1; /* 1 report handler */
			arguments->scalarOutputCount = 2;
		}
		IOLog("OP2 Buttons: %s getElementCount (selector 14) -> elements=1 reports=1\n", name);
		return kIOReturnSuccess;

	case 15: /* kIOHIDLibUserClientGetElements */
		if (arguments->structureOutput && arguments->structureOutputSize >= sizeof(struct IOHIDElementStruct)) {
			struct IOHIDElementStruct *elem = (struct IOHIDElementStruct *)arguments->structureOutput;
			bzero(elem, sizeof(struct IOHIDElementStruct));
			elem->cookieMin = 1;
			elem->cookieMax = 1;
			elem->parentCookie = 0;
			elem->type = 2; /* kIOHIDElementTypeInput_Button */
			elem->collectionType = 0;
			elem->flags = 0;
			elem->usagePage = 12; /* kHIDPage_Consumer */
			elem->usageMin = 0x40;  /* kHIDUsage_Csmr_Menu */
			elem->usageMax = 0x40;
			elem->min = 0;
			elem->max = 1;
			elem->scaledMin = 0;
			elem->scaledMax = 1;
			elem->size = 8;
			elem->reportSize = 8;
			elem->reportCount = 1;
			elem->reportID = 0;
			arguments->structureOutputSize = sizeof(struct IOHIDElementStruct);
			IOLog("OP2 Buttons: %s getElements (selector 15) -> consumer/menu button element (%lu bytes)\n",
			    name, (unsigned long)sizeof(struct IOHIDElementStruct));
		}
		return kIOReturnSuccess;

	default:
		IOLog("OP2 Buttons: %s method selector %u (inScalars %u, inStruct %lu)\n",
		    name, selector, arguments->scalarInputCount, (unsigned long)arguments->structureInputSize);
		return kIOReturnSuccess;
	}
}

#endif /* ARM64_BOARD_CONFIG_MSM8994 */
