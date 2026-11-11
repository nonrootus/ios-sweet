/*
 * sweetnet: internet over the USB cable for iOS on sweet_k6a (kernel ue0, sweetusb=4 RNDIS).
 *
 * The kernel gives ue0 192.168.137.2/24, beside Windows' Internet Connection Sharing at
 * 192.168.137.1 (which also answers DNS). configd never adopts ue0 by itself (it is not an IOKit
 * IONetworkInterface), and GUI apps (Safari, Sileo) gate on nw_path / SCNetworkReachability, which
 * see a network only when configd ranks a service: so this publishes a service for ue0 in the
 * dynamic store, as a VPN daemon would, and stays alive (launchd KeepAlive) to keep it.
 * Adapted from s9net of the Galaxy S9 fork (syncmodz/ios-s9, MIT), with this port's addresses.
 * SCDynamicStore and SCNetworkReachability are SystemConfiguration API; their iOS headers are
 * not public, so the few functions are declared here.
 */
#include <CoreFoundation/CoreFoundation.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>

typedef const struct __SCDynamicStore *SCDynamicStoreRef;
extern SCDynamicStoreRef SCDynamicStoreCreate(CFAllocatorRef, CFStringRef, void *, void *);
extern Boolean SCDynamicStoreSetValue(SCDynamicStoreRef, CFStringRef, CFPropertyListRef);

typedef const struct __SCNetworkReachability *SCNetworkReachabilityRef;
typedef uint32_t SCNetworkReachabilityFlags;
extern SCNetworkReachabilityRef SCNetworkReachabilityCreateWithName(CFAllocatorRef, const char *);
extern Boolean SCNetworkReachabilityGetFlags(SCNetworkReachabilityRef, SCNetworkReachabilityFlags *);

#define SERVICE_ID "Sweet-USB-Network"
#define SERVICE "State:/Network/Service/" SERVICE_ID
#define IFACE "ue0"
#define ROUTER "192.168.137.1"
#define SELF "192.168.137.2"

static CFArrayRef
strings(const char *const *items, int n)
{
	CFStringRef v[4];
	for (int i = 0; i < n; i++) {
		v[i] = CFStringCreateWithCString(NULL, items[i], kCFStringEncodingUTF8);
	}
	CFArrayRef a = CFArrayCreate(NULL, (const void **)v, n, &kCFTypeArrayCallBacks);
	for (int i = 0; i < n; i++) {
		CFRelease(v[i]);
	}
	return a;
}

static void
publish(SCDynamicStoreRef store, const char *key, CFDictionaryRef value)
{
	CFStringRef k = CFStringCreateWithCString(NULL, key, kCFStringEncodingUTF8);
	if (!SCDynamicStoreSetValue(store, k, value)) {
		fprintf(stderr, "sweetnet: could not set %s\n", key);
	}
	CFRelease(k);
}

static void
report(const char *when)
{
	SCNetworkReachabilityRef r = SCNetworkReachabilityCreateWithName(NULL, "www.apple.com");
	SCNetworkReachabilityFlags f = 0;
	Boolean ok = r && SCNetworkReachabilityGetFlags(r, &f);
	printf("sweetnet: reachability %s: %s flags=0x%x reachable=%d\n", when, ok ? "ok" : "FAILED", f, (f & 2) ? 1 : 0);
	fflush(stdout);
	if (r) {
		CFRelease(r);
	}
}

int
main(void)
{
	SCDynamicStoreRef store = SCDynamicStoreCreate(NULL, CFSTR("sweetnet"), NULL, NULL);
	if (store == NULL) {
		fprintf(stderr, "sweetnet: no dynamic store\n");
		return 1;
	}
	const char *self[] = { SELF };
	const char *mask[] = { "255.255.255.0" };
	const char *dns_servers[] = { ROUTER, "1.1.1.1", "8.8.8.8" };

	CFMutableDictionaryRef ip = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks,
	    &kCFTypeDictionaryValueCallBacks);
	CFArrayRef addresses = strings(self, 1), masks = strings(mask, 1);
	CFDictionarySetValue(ip, CFSTR("Addresses"), addresses);
	CFDictionarySetValue(ip, CFSTR("SubnetMasks"), masks);
	CFDictionarySetValue(ip, CFSTR("Router"), CFSTR(ROUTER));
	CFDictionarySetValue(ip, CFSTR("InterfaceName"), CFSTR(IFACE));

	CFMutableDictionaryRef dns = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks,
	    &kCFTypeDictionaryValueCallBacks);
	CFArrayRef servers = strings(dns_servers, 3);
	CFDictionarySetValue(dns, CFSTR("ServerAddresses"), servers);

	/* Link state, so IPMonitor treats ue0 as an active link. */
	CFMutableDictionaryRef link = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks,
	    &kCFTypeDictionaryValueCallBacks);
	CFDictionarySetValue(link, CFSTR("Active"), kCFBooleanTrue);

	report("before");
	publish(store, SERVICE "/IPv4", ip);
	publish(store, SERVICE "/DNS", dns);
	publish(store, "State:/Network/Interface/" IFACE "/Link", link);
	/*
	 * The primary service is left to IPMonitor's ranking: forcing it woke every network daemon
	 * at once on the S9 (their notes), which this port does not need either.
	 */
	printf("sweetnet: published %s on %s via %s\n", SERVICE_ID, IFACE, ROUTER);
	fflush(stdout);
	for (int i = 0; i < 5; i++) {
		sleep(2);
		report("after");
	}
	for (;;) {
		pause();        /* launchd's KeepAlive starts it again if the store session is ever lost */
	}
}
