/* CoreAnimation filter uniforms for the 20H392 EAGL renderer. */
#include <dlfcn.h>
#include <mach/mach.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <uuid/uuid.h>

extern void redwood_gl_function_values(void *context, unsigned function,
    unsigned index, size_t components, size_t count, const float *values);

typedef void (*uniform_fn)(void *, unsigned, size_t, size_t, const float *);
static uniform_fn original_uniform;

static void
forward_uniform(void *context, unsigned index, size_t components, size_t count,
    const float *values)
{
    const uint8_t *function = *(const uint8_t *const *)((const uint8_t *)context + 0x10);
    if (function != NULL) {
        redwood_gl_function_values(context, function[0x10], index, components, count, values);
    }
    original_uniform(context, index, components, count, values);
}

/* The vtable and original method are from the same iPhone X 16.7.16 cache
 * as the Topaz EAGL integration. The copy-on-write change is process-local. */
void
sweet_install_ca_uniforms(void)
{
    static const uuid_t cache_uuid = {
        0x8f, 0x97, 0x04, 0x52, 0xc9, 0xe3, 0x3e, 0x88,
        0xb1, 0x14, 0xfe, 0xb1, 0x86, 0x4b, 0xc9, 0xe0,
    };
    const char *name = getprogname();
    bool (*get_uuid)(uuid_t) = dlsym(RTLD_DEFAULT, "_dyld_get_shared_cache_uuid");
    const void *(*get_range)(size_t *) = dlsym(RTLD_DEFAULT, "_dyld_get_shared_cache_range");
    uuid_t actual;
    if (name == NULL || strcmp(name, "backboardd") != 0 || get_uuid == NULL ||
        !get_uuid(actual) || memcmp(actual, cache_uuid, sizeof(actual)) != 0 || get_range == NULL) {
        return;
    }

    const uintptr_t cache_base = 0x180000000UL;
    const uintptr_t slot_offset = 0x1d5b2f6e8UL + 0x10 + 0x388 - cache_base;
    const uintptr_t original_offset = 0x1884b21acUL - cache_base;
    size_t size = 0;
    uintptr_t cache = (uintptr_t)get_range(&size);
    if (cache == 0 || size < slot_offset + sizeof(uintptr_t) || size <= original_offset) {
        return;
    }

    uintptr_t *slot = (uintptr_t *)(cache + slot_offset);
    uintptr_t expected = cache + original_offset;
    if (*slot != expected) {
        fputs("SWEET CA: filter uniform slot mismatch\n", stderr);
        return;
    }
    vm_address_t page = (vm_address_t)slot & ~(vm_address_t)(vm_page_size - 1);
    kern_return_t rc = vm_protect(mach_task_self(), page, vm_page_size, false,
        VM_PROT_READ | VM_PROT_WRITE | VM_PROT_COPY);
    if (rc != KERN_SUCCESS) {
        fprintf(stderr, "SWEET CA: filter uniform table protection rc=%d\n", rc);
        return;
    }

    original_uniform = (uniform_fn)expected;
    *slot = (uintptr_t)forward_uniform;
    rc = vm_protect(mach_task_self(), page, vm_page_size, false, VM_PROT_READ);
    fprintf(stderr, "SWEET CA: filter uniform hook installed, restore rc=%d\n", rc);
}
