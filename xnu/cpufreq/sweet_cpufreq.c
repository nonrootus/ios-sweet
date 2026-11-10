/*
 * sweet_k6a CPU clocks. Nothing else asks the OSM (Qualcomm's operating state manager) for a
 * frequency, so without this the cores stay wherever the boot loader left them.
 *
 * Registers follow Xiaomi 10c33d40 drivers/clk/qcom/clk-cpu-osm.c for sdmmagpie: the table
 * (sweet_osm_lut.inc), the wanted row at 0x920 + 4 * core (one vote per core when bit 0 of
 * CORE_DCVS_CTRL 0xbc is set, otherwise core 0's vote is the group's), the enable bit at 0x0.
 * Bases from the live FDT node qcom,cpucc@18321000: L3 0x18321000, the six A55 0x18323000,
 * the two A76 0x18325800.
 *
 * The governor follows the Redmi K20 Pro port (hakios.cpufreq.c): every 20 ms it reads each
 * core's idle time; a group runs at its fastest non-boost row while one of its cores was busy
 * in the last 200 ms and at its slowest row otherwise. The first 30 s and 300 ms after a touch
 * are always fast. L3 follows the CPUs. Boot-arg sweetcpufreq=0 leaves everything alone,
 * sweetcpufreq=2 keeps every group at its fastest non-boost row.
 */
#if defined(ARM64_BOARD_CONFIG_SWEET)
#include <stdint.h>
#include <kern/kern_types.h>
#include <kern/thread.h>
#include <kern/processor.h>
#include <kern/recount.h>
#include <kern/sched_prim.h>
#include <kern/clock.h>
#include <kern/misc_protos.h>
#include <arm/machine_routines.h>
#include <pexpert/pexpert.h>
#include <machine/atomic.h>

#include "sweet_osm_lut.inc"

void sweet_cpufreq_start(void);
void sweet_cpufreq_boost(void);

#define OSM_SIZE                0x1400u
#define OSM_ENABLE              0x0u
#define OSM_CORE_DCVS_CTRL      0xbcu
#define OSM_LUT(i)              (0x110u + 32u * (i))
#define OSM_WANTED(n)           (0x920u + 4u * (n))
#define PERIOD_MS               20u
#define QUIET_SAMPLES           10u     /* 200 ms */
#define BUSY_PERCENT            20u
#define BOOT_SAMPLES            1500u   /* 30 s */
#define TOUCH_MS                300u
#define REPORT_SAMPLES          1500u
#define GROUPS                  3u
#define CPU_GROUPS              2u
#define MAX_CORES               8u

struct osm_group {
	const char *name;
	uint64_t phys;
	unsigned first_core, cores, owned, row;
	int cpu, per_core;
	vm_offset_t base;
	uint32_t saved;
	struct sweet_osm_table table;
};

static struct osm_group groups[GROUPS] = {
	{ .name = "A55", .phys = 0x18323000, .first_core = 0, .cores = 6, .cpu = 1 },
	{ .name = "A76", .phys = 0x18325800, .first_core = 6, .cores = 2, .cpu = 1 },
	{ .name = "L3", .phys = 0x18321000, .first_core = 0, .cores = 1, .cpu = 0 },
};
static uint32_t cpufreq_mode = 1;
static uint64_t boost_until;
static int governor_event;

static uint32_t
osm_read(const struct osm_group *g, uint32_t offset)
{
	return *(volatile uint32_t *)(g->base + offset);
}

static void
osm_set_row(struct osm_group *g, unsigned row)
{
	unsigned votes = g->per_core ? g->cores : 1;

	for (unsigned n = 0; n < votes; n++) {
		*(volatile uint32_t *)(g->base + OSM_WANTED(n)) = row;
	}
	__asm__ volatile ("dsb sy" ::: "memory");
	g->row = row;
}

static unsigned
core_of(const ml_topology_cpu_t *cpu)
{
	return (cpu->phys_id >> 8) & 0xffu;   /* mkadt: reg = MPIDR Aff1:Aff0, Aff1 = core */
}

static unsigned
group_of(unsigned core)
{
	return core < 6 ? 0u : 1u;
}

/* How fast a plain loop runs on this core, in thousands of iterations per millisecond. */
static uint32_t
loop_speed(void)
{
	volatile uint64_t sink = 0;
	uint64_t start = mach_absolute_time(), nanos = 0;

	for (uint32_t i = 0; i < 2000000u; i++) {
		sink += i;
	}
	absolutetime_to_nanoseconds(mach_absolute_time() - start, &nanos);
	return nanos != 0 ? (uint32_t)(2000000ull * 1000ull / nanos) : 0;
}

static processor_t
processor_of_core(unsigned core)
{
	const ml_topology_info_t *topology = ml_get_topology_info();

	for (unsigned i = 0; i < topology->num_cpus; i++) {
		if (core_of(&topology->cpus[i]) == core && topology->cpus[i].cpu_id < MAX_SCHED_CPUS) {
			return processor_array[topology->cpus[i].cpu_id];
		}
	}
	return PROCESSOR_NULL;
}

/* Loop speed on the first core of each group we own, at the current rows. */
static void
measure(uint32_t speed[CPU_GROUPS])
{
	for (unsigned g = 0; g < CPU_GROUPS; g++) {
		processor_t processor = groups[g].base != 0 ? processor_of_core(groups[g].first_core) : PROCESSOR_NULL;

		speed[g] = 0;
		if (processor == PROCESSOR_NULL) {
			continue;
		}
		thread_bind(processor);
		thread_block(THREAD_CONTINUE_NULL);
		speed[g] = loop_speed();
	}
	thread_bind(PROCESSOR_NULL);
	thread_block(THREAD_CONTINUE_NULL);
}

static uint64_t
idle_time_of(processor_t processor)
{
	struct recount_usage usage = { 0 };
	uint64_t idle = 0;

	recount_processor_usage(&processor->pr_recount, &usage, &idle);
	return idle;
}

__attribute__((noreturn)) static void
governor(void *arg __unused, wait_result_t result __unused)
{
	const ml_topology_info_t *topology = ml_get_topology_info();
	unsigned count = topology->num_cpus < MAX_CORES ? topology->num_cpus : MAX_CORES;
	processor_t processors[MAX_CORES] = { PROCESSOR_NULL };
	unsigned group[MAX_CORES] = { 0 }, quiet[CPU_GROUPS] = { 0 }, samples = 0;
	uint32_t before[CPU_GROUPS] = { 0 }, after[CPU_GROUPS] = { 0 };
	uint64_t idle_before[MAX_CORES] = { 0 }, then;

	/* Evidence first: the same loop on each group before and after its fastest row. */
	measure(before);
	for (unsigned g = 0; g < GROUPS; g++) {
		if (groups[g].base != 0) {
			osm_set_row(&groups[g], groups[g].table.high);
		}
	}
	delay_for_interval(2, NSEC_PER_MSEC);
	measure(after);
	printf("SWEET CPUFREQ: loop speed A55 %u -> %u, A76 %u -> %u thousand/ms (mode %u)\n",
	    before[0], after[0], before[1], after[1], cpufreq_mode);
	then = mach_absolute_time();
	for (unsigned i = 0; i < count; i++) {
		unsigned id = topology->cpus[i].cpu_id;

		if (id < MAX_SCHED_CPUS && processor_array[id] != PROCESSOR_NULL) {
			processors[i] = processor_array[id];
			group[i] = group_of(core_of(&topology->cpus[i]));
			idle_before[i] = idle_time_of(processors[i]);
		}
	}
	for (;;) {
		uint32_t busy[CPU_GROUPS] = { 0 };
		uint64_t now, elapsed;
		int boosted, any_fast = 0;

		assert_wait_timeout((event_t)&governor_event, THREAD_UNINT, PERIOD_MS, NSEC_PER_MSEC);
		thread_block(THREAD_CONTINUE_NULL);
		now = mach_absolute_time();
		elapsed = now - then;
		then = now;
		for (unsigned i = 0; i < count; i++) {
			uint64_t idle, delta;
			uint32_t load;

			if (processors[i] == PROCESSOR_NULL) {
				continue;
			}
			idle = idle_time_of(processors[i]);
			delta = idle - idle_before[i];
			idle_before[i] = idle;
			load = elapsed != 0 && delta < elapsed ? (uint32_t)((elapsed - delta) * 100u / elapsed) : 0;
			if (load > busy[group[i]]) {
				busy[group[i]] = load;
			}
		}
		boosted = cpufreq_mode == 2 || samples < BOOT_SAMPLES || now < os_atomic_load(&boost_until, relaxed);
		for (unsigned g = 0; g < CPU_GROUPS; g++) {
			struct osm_group *osm = &groups[g];
			unsigned wanted;

			if (osm->base == 0) {
				continue;
			}
			quiet[g] = busy[g] < BUSY_PERCENT ? quiet[g] + 1 : 0;
			wanted = boosted || quiet[g] < QUIET_SAMPLES ? osm->table.high : osm->table.low;
			if (wanted != osm->row) {
				osm_set_row(osm, wanted);
			}
			any_fast |= wanted == osm->table.high;
		}
		if (groups[2].base != 0) {
			unsigned wanted = any_fast ? groups[2].table.high : groups[2].table.low;

			if (wanted != groups[2].row) {
				osm_set_row(&groups[2], wanted);
			}
		}
		if (++samples % REPORT_SAMPLES == 0) {
			printf("SWEET CPUFREQ: load A55 %u%% A76 %u%%, MHz A55 %u A76 %u L3 %u\n", busy[0], busy[1],
			    groups[0].base != 0 ? groups[0].table.khz[groups[0].row] / 1000 : 0,
			    groups[1].base != 0 ? groups[1].table.khz[groups[1].row] / 1000 : 0,
			    groups[2].base != 0 ? groups[2].table.khz[groups[2].row] / 1000 : 0);
		}
	}
}

void
sweet_cpufreq_boost(void)
{
	uint64_t deadline = 0;

	if (groups[0].base == 0 && groups[1].base == 0) {
		return;
	}
	clock_interval_to_deadline(TOUCH_MS, NSEC_PER_MSEC, &deadline);
	os_atomic_store(&boost_until, deadline, relaxed);
	thread_wakeup((event_t)&governor_event);
}

void
sweet_cpufreq_start(void)
{
	const ml_topology_info_t *topology = ml_get_topology_info();
	thread_t thread = THREAD_NULL;

	PE_parse_boot_argn("sweetcpufreq", &cpufreq_mode, sizeof(cpufreq_mode));
	if (cpufreq_mode == 0) {
		printf("SWEET CPUFREQ: off (sweetcpufreq=0), firmware frequencies kept\n");
		return;
	}
	for (unsigned i = 0; i < topology->num_cpus; i++) {
		groups[group_of(core_of(&topology->cpus[i]))].owned++;
	}
	groups[2].owned = groups[0].owned + groups[1].owned;
	for (unsigned g = 0; g < GROUPS; g++) {
		struct osm_group *osm = &groups[g];
		uint32_t words[SWEET_OSM_ROWS];

		if (osm->owned == 0) {
			printf("SWEET CPUFREQ: %s runs none of our threads, left alone\n", osm->name);
			continue;
		}
		osm->base = ml_io_map(osm->phys, OSM_SIZE);
		if (osm->base == 0 || (osm_read(osm, OSM_ENABLE) & 1u) == 0) {
			printf("SWEET CPUFREQ: %s OSM at %#llx not enabled by firmware\n", osm->name, (unsigned long long)osm->phys);
			osm->base = 0;
			continue;
		}
		for (unsigned i = 0; i < SWEET_OSM_ROWS; i++) {
			words[i] = osm_read(osm, OSM_LUT(i));
		}
		if (!sweet_osm_parse(words, osm->cpu, &osm->table)) {
			printf("SWEET CPUFREQ: %s has no usable table row\n", osm->name);
			osm->base = 0;
			continue;
		}
		osm->per_core = osm->cpu && (osm_read(osm, OSM_CORE_DCVS_CTRL) & 1u) != 0;
		osm->saved = osm_read(osm, OSM_WANTED(0));
		osm->row = osm->saved;
		printf("SWEET CPUFREQ: %s rows=%u per_core=%d firmware row %u (%u MHz), using rows %u..%u (%u..%u MHz)\n",
		    osm->name, osm->table.rows, osm->per_core, osm->saved,
		    osm->saved < osm->table.rows ? osm->table.khz[osm->saved] / 1000 : 0,
		    osm->table.low, osm->table.high, osm->table.khz[osm->table.low] / 1000, osm->table.khz[osm->table.high] / 1000);
		for (unsigned i = 0; i < osm->table.rows; i += 8) {
			printf("SWEET CPUFREQ: %s table %u: %08x %08x %08x %08x %08x %08x %08x %08x\n", osm->name, i,
			    words[i], words[i + 1], words[i + 2], words[i + 3], words[i + 4], words[i + 5], words[i + 6], words[i + 7]);
		}
	}
	/* The governor thread measures and raises; bsd_init's thread is never bound here. */
	if (kernel_thread_start(governor, NULL, &thread) != KERN_SUCCESS) {
		printf("SWEET CPUFREQ: governor did not start, firmware rows kept\n");
		return;
	}
	thread_set_thread_name(thread, "sweet cpufreq");
	thread_deallocate(thread);
}
#endif /* ARM64_BOARD_CONFIG_SWEET */
