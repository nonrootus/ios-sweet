/*
 * Built-in AppleMobileFileIntegrity service for SWEET.
 *
 * Apple's AMFI kext also has an IOKit face. ExtensionFoundation in lsd reads the
 * extension point cache /System/Library/xpc/extensions.plist, verifies its detached
 * signature (extensions.plist.sig), and then asks this service whether the
 * signature's cdhash is in the trust cache: IOServiceMatching("AppleMobileFileIntegrity"),
 * IOServiceOpen type 0, IOConnectCallMethod selector 6 with one scalar (1) and the
 * cdhash as the input structure, no output (20H392 ExtensionFoundation
 * 0x193735c9c..0x193735e1c; kIOReturnNotFound means "not in trust cache"). Without the
 * service it says "Unable to find AMFI" and skips the cache, so no framework-declared
 * extension point exists (posterkit, widgetkit): no wallpaper, no widgets.
 *
 * SWEET has no trust caches and treats every signed binary on the root as a platform
 * binary (bsd/kern/sweet_amfi.c), so selector 6 answers "trusted". Every other
 * selector is kIOReturnUnsupported. The first calls are logged with caller, selector
 * and argument shapes, so the next thing the system asks for shows on the console.
 */
#include <pexpert/arm64/board_config.h>

#if defined(SWEET)
#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IOKitKeys.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <sys/proc.h>

#define SWEET_AMFI_CDHASH_IN_TRUST_CACHE 6u
#define SWEET_AMFI_LOGGED_CALLS          24u

class AppleMobileFileIntegrity : public IOService {
	OSDeclareDefaultStructors(AppleMobileFileIntegrity);

public:
	bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(AppleMobileFileIntegrity, IOService);

bool
AppleMobileFileIntegrity::start(IOService *provider)
{
	if (!IOService::start(provider)) {
		return false;
	}
	setProperty(kIOUserClientClassKey, "AppleMobileFileIntegrityUserClient");
	registerService();
	IOLog("SWEET AMFI: AppleMobileFileIntegrity service registered (cdhash queries answer trusted)\n");
	return true;
}

class AppleMobileFileIntegrityUserClient : public IOUserClient {
	OSDeclareDefaultStructors(AppleMobileFileIntegrityUserClient);

public:
	IOReturn clientClose(void) APPLE_KEXT_OVERRIDE;
	IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments *args,
	    IOExternalMethodDispatch *dispatch, OSObject *target,
	    void *reference) APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(AppleMobileFileIntegrityUserClient, IOUserClient);

IOReturn
AppleMobileFileIntegrityUserClient::clientClose(void)
{
	terminate();
	return kIOReturnSuccess;
}

IOReturn
AppleMobileFileIntegrityUserClient::externalMethod(uint32_t selector, IOExternalMethodArguments *args,
    __unused IOExternalMethodDispatch *dispatch, __unused OSObject *target,
    __unused void *reference)
{
	static uint32_t logged;
	static bool trusted_logged;
	IOReturn ret = selector == SWEET_AMFI_CDHASH_IN_TRUST_CACHE ? kIOReturnSuccess : kIOReturnUnsupported;
	uint32_t in_struct = args->structureInputDescriptor != NULL ?
	    (uint32_t)args->structureInputDescriptor->getLength() : args->structureInputSize;
	uint32_t out_struct = args->structureOutputDescriptor != NULL ?
	    (uint32_t)args->structureOutputDescriptor->getLength() : args->structureOutputSize;
	bool say = logged < SWEET_AMFI_LOGGED_CALLS;

	/* Whatever the caller made room for reads as zero, not as stack bytes. */
	for (uint32_t i = 0; i < args->scalarOutputCount; i++) {
		args->scalarOutput[i] = 0;
	}
	if (ret == kIOReturnSuccess && !trusted_logged) {
		trusted_logged = true;
		say = true;
	}
	if (say) {
		char name[32];

		logged++;
		proc_selfname(name, sizeof(name));
		IOLog("SWEET AMFI: service call from %s sel %u in %u/%u out %u/%u -> 0x%x\n", name, selector,
		    args->scalarInputCount, in_struct, args->scalarOutputCount, out_struct, ret);
	}
	return ret;
}
#endif /* SWEET */
