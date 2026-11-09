/* Time spent in the calls libOSMesa makes on the SweetAdreno user client. */
#ifndef SWEET_GPU_CALLS_H
#define SWEET_GPU_CALLS_H

#include <stdbool.h>
#include <stdint.h>

struct sweet_gpu_calls {
	uint64_t submit_us, gem_new_us, cpu_prep_us, wait_us, map_us;
	uint32_t submits, gem_news, cpu_preps, waits, maps;
};

bool sweet_gpu_calls_install(void);
void sweet_gpu_calls_sample(struct sweet_gpu_calls *out);

#endif
