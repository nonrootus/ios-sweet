/* Loaded into backboardd by the GPU-enabled iOS root. */
#include <IOKit/IOKitLib.h>
#include <dlfcn.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>
#include <unistd.h>
#include <uuid/uuid.h>

static int renderer_requested(void)
{
    char args[1024];
    size_t size = sizeof(args) - 1;
    if (sysctlbyname("kern.bootargs", args, &size, NULL, 0) != 0 || size >= sizeof(args))
        return 0;
    args[size] = '\0';
    char *save;
    for (char *arg = strtok_r(args, " ", &save); arg; arg = strtok_r(NULL, " ", &save)) {
        if (strcmp(arg, "sweetcagpu=1") == 0)
            return 1;
    }
    return 0;
}

static bool supported_shared_cache(void)
{
    /* iPhone X iOS 16.7.16 (20H392). EAGL's function table is private ABI. */
    static const uuid_t supported = {
        0x8f, 0x97, 0x04, 0x52, 0xc9, 0xe3, 0x3e, 0x88,
        0xb1, 0x14, 0xfe, 0xb1, 0x86, 0x4b, 0xc9, 0xe0,
    };
    bool (*cache_uuid)(uuid_t) = (bool (*)(uuid_t))dlsym(RTLD_DEFAULT, "_dyld_get_shared_cache_uuid");
    uuid_t actual;
    return cache_uuid && cache_uuid(actual) && memcmp(actual, supported, sizeof(actual)) == 0;
}

__attribute__((constructor)) static void load_sweet_renderer(void)
{
    const char *name = getprogname();
    if (!name || strcmp(name, "backboardd") != 0 || !renderer_requested())
        return;
    if (!supported_shared_cache()) {
        fputs("SWEET CA: unsupported shared cache\n", stderr);
        return;
    }

    io_service_t gpu = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("SweetAdreno"));
    if (!gpu) {
        fputs("SWEET CA: Adreno service unavailable\n", stderr);
        return;
    }
    IOObjectRelease(gpu);

    /* The first backend waits for GPU work before reading pixels into IOSurface. */
    if (setenv("FD_MESA_DEBUG", "sysmem", 0) != 0)
        return;
    void *library = dlopen("/sweet/lib/libSweetEAGL.dylib", RTLD_NOW | RTLD_LOCAL);
    if (!library) {
        fprintf(stderr, "SWEET CA: EAGL load failed: %s\n", dlerror());
        return;
    }
    fputs("SWEET CA: EAGL library loaded\n", stderr);
}
