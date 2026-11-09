/*
 * Rebinds libOSMesa's two imports that reach the GPU driver, in this process
 * only, to learn how much of a GL call is the driver's. The kernel's submit
 * returns when the GPU has finished, so what is not in these calls is CPU work.
 * If the imports are not found nothing is changed and the times stay zero.
 */
#include "sweet-gpu-calls.h"

#include <IOKit/IOKitLib.h>
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <mach-o/nlist.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { kSweetGemNew = 0x42, kSweetCpuPrep = 0x44, kSweetSubmit = 0x46, kSweetWaitFence = 0x47 };

typedef kern_return_t (*struct_call_fn)(mach_port_t, uint32_t, const void *, size_t, void *, size_t *);
typedef kern_return_t (*map_call_fn)(io_connect_t, uint32_t, task_port_t, mach_vm_address_t *, mach_vm_size_t *, IOOptionBits);

static struct_call_fn original_struct_call;
static map_call_fn original_map_call;
static struct sweet_gpu_calls ticks;
static mach_port_t driver_connection;

static kern_return_t
timed_struct_call(mach_port_t connection, uint32_t selector, const void *input, size_t input_size, void *output, size_t *output_size)
{
	uint64_t started = mach_absolute_time();
	kern_return_t result;

	driver_connection = connection;
	result = original_struct_call(connection, selector, input, input_size, output, output_size);
	uint64_t spent = mach_absolute_time() - started;

	switch (selector) {
	case kSweetSubmit:
		ticks.submit_us += spent;
		ticks.submits++;
		break;
	case kSweetGemNew:
		ticks.gem_new_us += spent;
		ticks.gem_news++;
		break;
	case kSweetCpuPrep:
		ticks.cpu_prep_us += spent;
		ticks.cpu_preps++;
		break;
	case kSweetWaitFence:
		ticks.wait_us += spent;
		ticks.waits++;
		break;
	default:
		break;
	}
	return result;
}

static kern_return_t
timed_map_call(io_connect_t connection, uint32_t type, task_port_t task, mach_vm_address_t *address, mach_vm_size_t *size, IOOptionBits options)
{
	uint64_t started = mach_absolute_time();
	kern_return_t result = original_map_call(connection, type, task, address, size, options);

	ticks.map_us += mach_absolute_time() - started;
	ticks.maps++;
	return result;
}

void
sweet_gpu_calls_sample(struct sweet_gpu_calls *out)
{
	static mach_timebase_info_data_t base;

	if (base.denom == 0) {
		mach_timebase_info(&base);
	}
	*out = ticks;
	out->submit_us = out->submit_us * base.numer / base.denom / 1000;
	out->gem_new_us = out->gem_new_us * base.numer / base.denom / 1000;
	out->cpu_prep_us = out->cpu_prep_us * base.numer / base.denom / 1000;
	out->wait_us = out->wait_us * base.numer / base.denom / 1000;
	out->map_us = out->map_us * base.numer / base.denom / 1000;
}

int
sweet_gpu_call(uint32_t selector, void *data, size_t size)
{
	size_t returned = size;

	if (original_struct_call == NULL || driver_connection == MACH_PORT_NULL) {
		return -1;
	}
	return original_struct_call(driver_connection, selector, data, size, data, &returned);
}

/* The page may be read-only once the image is bound; the change stays in this process. */
static bool
rebind(void **slot, void *replacement, void **original, bool read_only)
{
	vm_address_t page = (vm_address_t)slot & ~(vm_address_t)(vm_page_size - 1);

	if (vm_protect(mach_task_self(), page, vm_page_size, false, VM_PROT_READ | VM_PROT_WRITE | VM_PROT_COPY) != KERN_SUCCESS) {
		return false;
	}
	*original = *slot;
	*slot = replacement;
	if (read_only) {
		(void)vm_protect(mach_task_self(), page, vm_page_size, false, VM_PROT_READ);
	}
	return true;
}

bool
sweet_gpu_calls_install(void)
{
	const char *name = getprogname();
	const struct mach_header_64 *header = NULL;
	intptr_t slide = 0;
	unsigned struct_slots = 0, map_slots = 0;

	if (name == NULL || strcmp(name, "backboardd") != 0) {
		return false;
	}
	for (uint32_t i = 0; i < _dyld_image_count(); i++) {
		const char *image = _dyld_get_image_name(i);

		if (image != NULL && strstr(image, "libOSMesa") != NULL) {
			header = (const struct mach_header_64 *)_dyld_get_image_header(i);
			slide = _dyld_get_image_vmaddr_slide(i);
			break;
		}
	}
	if (header == NULL) {
		fputs("SWEET RB: gpu call timing: no libOSMesa\n", stderr);
		return false;
	}

	const struct segment_command_64 *linkedit = NULL;
	const struct symtab_command *symtab = NULL;
	const struct dysymtab_command *dysymtab = NULL;
	const uint8_t *cursor = (const uint8_t *)(header + 1);

	for (uint32_t i = 0; i < header->ncmds; i++) {
		const struct load_command *command = (const struct load_command *)cursor;

		if (command->cmd == LC_SEGMENT_64 && strcmp(((const struct segment_command_64 *)command)->segname, SEG_LINKEDIT) == 0) {
			linkedit = (const struct segment_command_64 *)command;
		} else if (command->cmd == LC_SYMTAB) {
			symtab = (const struct symtab_command *)command;
		} else if (command->cmd == LC_DYSYMTAB) {
			dysymtab = (const struct dysymtab_command *)command;
		}
		cursor += command->cmdsize;
	}
	if (linkedit == NULL || symtab == NULL || dysymtab == NULL || dysymtab->nindirectsyms == 0) {
		fputs("SWEET RB: gpu call timing: no symbol tables\n", stderr);
		return false;
	}

	uintptr_t base = (uintptr_t)slide + linkedit->vmaddr - linkedit->fileoff;
	const struct nlist_64 *symbols = (const struct nlist_64 *)(base + symtab->symoff);
	const char *strings = (const char *)(base + symtab->stroff);
	const uint32_t *indirect = (const uint32_t *)(base + dysymtab->indirectsymoff);

	cursor = (const uint8_t *)(header + 1);
	for (uint32_t i = 0; i < header->ncmds; i++) {
		const struct load_command *command = (const struct load_command *)cursor;

		cursor += command->cmdsize;
		if (command->cmd != LC_SEGMENT_64) {
			continue;
		}
		const struct segment_command_64 *segment = (const struct segment_command_64 *)command;
		bool read_only = strcmp(segment->segname, "__DATA_CONST") == 0;

		if (!read_only && strcmp(segment->segname, SEG_DATA) != 0) {
			continue;
		}
		const struct section_64 *section = (const struct section_64 *)(segment + 1);

		for (uint32_t s = 0; s < segment->nsects; s++, section++) {
			uint32_t type = section->flags & SECTION_TYPE;

			if (type != S_LAZY_SYMBOL_POINTERS && type != S_NON_LAZY_SYMBOL_POINTERS) {
				continue;
			}
			void **slots = (void **)((uintptr_t)slide + section->addr);
			size_t count = section->size / sizeof(void *);

			for (size_t j = 0; j < count && section->reserved1 + j < dysymtab->nindirectsyms; j++) {
				uint32_t symbol = indirect[section->reserved1 + j];

				if ((symbol & (INDIRECT_SYMBOL_ABS | INDIRECT_SYMBOL_LOCAL)) != 0 || symbol >= symtab->nsyms) {
					continue;
				}
				const char *import = strings + symbols[symbol].n_un.n_strx;

				if (strcmp(import, "_IOConnectCallStructMethod") == 0) {
					struct_slots += rebind(&slots[j], (void *)timed_struct_call, (void **)&original_struct_call, read_only);
				} else if (strcmp(import, "_IOConnectMapMemory64") == 0) {
					map_slots += rebind(&slots[j], (void *)timed_map_call, (void **)&original_map_call, read_only);
				}
			}
		}
	}
	fprintf(stderr, "SWEET RB: gpu call timing installed struct=%u map=%u\n", struct_slots, map_slots);
	return struct_slots != 0;
}
