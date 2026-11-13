/*
 * Minimal device-tree platform expert for OnePlus 2 (MSM8994).
 * Uses XNU's normal platform, power-domain and nub initialization.
 */
#include <pexpert/arm64/board_config.h>

#if defined(ARM64_BOARD_CONFIG_MSM8994)
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <IOKit/IOLib.h>
#include "OP2InterruptController.h"
extern "C" {
#include <prng/random.h>
void ml_init_max_cpus(unsigned int max_cpus);
unsigned int ml_get_cpu_count(void);
}

class OP2PlatformExpert : public IODTPlatformExpert {
	OSDeclareDefaultStructors(OP2PlatformExpert);

public:
	bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
	void processTopLevel(IORegistryEntry *root) APPLE_KEXT_OVERRIDE;
	const char *deleteList(void) APPLE_KEXT_OVERRIDE;
	const char *excludeList(void) APPLE_KEXT_OVERRIDE;
	bool getModelName(char *name, int maxLength) APPLE_KEXT_OVERRIDE;
	bool getMachineName(char *name, int maxLength) APPLE_KEXT_OVERRIDE;
	long getGMTTimeOfDay(void) APPLE_KEXT_OVERRIDE;
	void setGMTTimeOfDay(long secs) APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(OP2PlatformExpert, IODTPlatformExpert);

static uint64_t sGMTBaseSec = 1794571200ULL; /* Nov 13, 2026 baseline epoch */
static uint64_t sBootMachTime = 0;

long
OP2PlatformExpert::getGMTTimeOfDay(void)
{
	if (sBootMachTime == 0) {
		sBootMachTime = mach_absolute_time();
	}
	uint64_t elapsed_ticks = mach_absolute_time() - sBootMachTime;
	uint64_t elapsed_sec = elapsed_ticks / 19200000ULL; /* MSM8994 19.2 MHz generic timer */
	return (long)(sGMTBaseSec + elapsed_sec);
}

void
OP2PlatformExpert::setGMTTimeOfDay(long secs)
{
	if (secs > 1500000000L) {
		if (sBootMachTime == 0) {
			sBootMachTime = mach_absolute_time();
		}
		uint64_t elapsed_ticks = mach_absolute_time() - sBootMachTime;
		uint64_t elapsed_sec = elapsed_ticks / 19200000ULL;
		sGMTBaseSec = (uint64_t)secs - elapsed_sec;
		IOLog("OP2: setGMTTimeOfDay updated baseline epoch to %llu (target %ld)\n",
		    sGMTBaseSec, secs);
	}
}

bool
OP2PlatformExpert::start(IOService *provider)
{
	IOLog("OP2: starting device-tree platform expert\n");
	op2_prng_bootstrap();
	unsigned int ncpus = ml_get_cpu_count();
	if (ncpus == 0) ncpus = 1;
	IOLog("OP2: initializing max cpus to %u\n", ncpus);
	ml_init_max_cpus(ncpus);

	if (!IODTPlatformExpert::start(provider)) {
		IOLog("OP2: IODTPlatformExpert::start failed\n");
		return false;
	}

	OP2InterruptController *intc = new OP2InterruptController;
	if (intc) {
		if (intc->init()) {
			intc->attach(this);
			intc->start(this);
		} else {
			intc->release();
		}
	}

	publishResource("IORTC", kOSBooleanTrue);
	registerService();
	IOLog("OP2: platform expert registered successfully\n");
	return true;
}

const char *
OP2PlatformExpert::deleteList(void)
{
	return "()";
}

const char *
OP2PlatformExpert::excludeList(void)
{
	return "('chosen', 'defaults', 'cpus', 'options')";
}

void
OP2PlatformExpert::processTopLevel(IORegistryEntry *root)
{
	/*
	 * Stock IODTPlatformExpert requires an Apple NVRAM /options node.
	 * OP2 has no Apple NVRAM controller; publish real CPU and device nodes
	 * directly without failing. createNubs adds the topology's logical CPU/cluster IDs.
	 */
	IORegistryEntry *cpus = root->childFromPath("cpus", gIODTPlane);
	if (cpus != NULL) {
		createNubs(this, IODTFindMatchingEntries(cpus, kIODTExclusive, NULL));
		cpus->release();
	}
	createNubs(this, IODTFindMatchingEntries(root, kIODTExclusive, excludeList()));

	/*
	 * No AppleARMIO driver publishes the /arm-io children: do it here so
	 * built-in drivers (the display, AppleCLCD) can match them. The GIC is
	 * driven by the platform code and gets no nub. The nubs hang off this
	 * platform expert: IOPlatformDevice::compareName asks its provider.
	 */
	IORegistryEntry *armio = root->childFromPath("arm-io", gIODTPlane);
	if (armio != NULL) {
		createNubs(this, IODTFindMatchingEntries(armio, kIODTExclusive, "('gic')"));
		armio->release();
	}
}

bool
OP2PlatformExpert::getModelName(char *name, int maxLength)
{
	if (name == NULL || maxLength <= 0) {
		return false;
	}
	strlcpy(name, "iPhone7,1", (size_t)maxLength);
	return true;
}

bool
OP2PlatformExpert::getMachineName(char *name, int maxLength)
{
	if (name == NULL || maxLength <= 0) {
		return false;
	}
	strlcpy(name, "iPhone 6 Plus", (size_t)maxLength);
	return true;
}
#endif /* ARM64_BOARD_CONFIG_MSM8994 */
