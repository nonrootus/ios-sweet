#include "sweet_gmu_memory.h"
#include "sweet_gpu_memory.h"
#include <IOKit/IOLib.h>
extern "C" {
#ifdef KERNEL
#include "../../../bsd/dev/sweet_gmu_layout.h"
#include "../../../bsd/dev/sweet_gpu_tables.h"
#include "../../../bsd/dev/sweet_gpu_hfi_table.h"
#else
#include "sweet_gmu_layout.h"
#include "sweet_gpu_tables.h"
#include "sweet_gpu_hfi_table.h"
#endif
}

enum { DATA_PAGES=131, TABLE_PAGES=6, GRANULE=4096 };
struct sweet_gmu_memory {
    sweet_gmu_layout layout;
    sweet_gpu_memory *data[SWEET_GMU_REGION_COUNT], *tables;
    sweet_gpu_table_page data_page[DATA_PAGES], table_page[TABLE_PAGES];
    uint64_t scratch[SWEET_GPU_TABLE_MAX_PAGES];
    unsigned used, refs;
    bool ready, destroy_requested;
};
static int alloc_table(void *opaque, sweet_gpu_table_page *out)
{
    auto *s=static_cast<sweet_gmu_memory *>(opaque);
    for(unsigned i=1;i<TABLE_PAGES;i++) if(!(s->used&(1u<<i))) {
        s->used|=1u<<i; *out=s->table_page[i];return 0;
    }
    return -1;
}
static void release_table(void *opaque, sweet_gpu_table_page page)
{
    auto *s=static_cast<sweet_gmu_memory *>(opaque);
    for(unsigned i=1;i<TABLE_PAGES;i++)
        if(s->table_page[i].pa==page.pa && s->table_page[i].cpu==page.cpu) {
            bzero(page.cpu,GRANULE);s->used&=~(1u<<i);return;
        }
}
static uint64_t *resolve_table(void *opaque, uint64_t pa)
{
    auto *s=static_cast<sweet_gmu_memory *>(opaque);
    for(unsigned i=0;i<TABLE_PAGES;i++)
        if((s->used&(1u<<i)) && s->table_page[i].pa==pa) return s->table_page[i].cpu;
    return nullptr;
}
static int clean_table(void *opaque, uint64_t *cpu, size_t bytes)
{
    auto *s=static_cast<sweet_gmu_memory *>(opaque);
    if(bytes!=GRANULE) return -1;
    for(unsigned i=0;i<TABLE_PAGES;i++)
        if((s->used&(1u<<i)) && s->table_page[i].cpu==cpu)
            return sweet_gpu_memory_clean(s->tables,i);
    return -1;
}
static int barrier(void *) {return 0;} /* memory_clean completes dsb sy */
void sweet_gmu_memory_destroy(sweet_gmu_memory *s)
{
    if(!s) return;
    if(s->refs) {s->destroy_requested=true;return;}
    sweet_gpu_memory_destroy(s->tables);
    for(unsigned i=0;i<SWEET_GMU_REGION_COUNT;i++) sweet_gpu_memory_destroy(s->data[i]);
    IOFree(s,sizeof(*s));
}
int sweet_gmu_memory_retain_mapping(sweet_gmu_memory *s,uint64_t *root)
{
    if(root) *root=0;
    if(!s || !root || !s->ready || s->destroy_requested || s->refs==UINT32_MAX) return -1;
    s->refs++;*root=s->table_page[0].pa;return 0;
}
int sweet_gmu_memory_release_mapping(sweet_gmu_memory *s)
{
    if(!s || !s->refs) return -1;
    if(--s->refs==0 && s->destroy_requested) sweet_gmu_memory_destroy(s);
    return 0;
}
static bool isolated(sweet_gmu_memory *s)
{
    /* Canonical allocations must never alias data/table physical pages, even
     * at the native 16K boundary; padded allocation tails remain unmapped. */
    unsigned region[DATA_PAGES+TABLE_PAGES], at=0;
    for(unsigned i=0;i<SWEET_GMU_REGION_COUNT;i++)
        for(unsigned j=0;j<s->layout.region[i].bytes/GRANULE;j++) region[at++]=i;
    for(unsigned j=0;j<TABLE_PAGES;j++) region[at++]=SWEET_GMU_REGION_COUNT;
    if(at!=DATA_PAGES+TABLE_PAGES) return false;
    for(unsigned i=0;i<at;i++) {
        auto a=i<DATA_PAGES?s->data_page[i]:s->table_page[i-DATA_PAGES];
        if(!a.cpu || !a.pa || (a.pa&4095) || a.pa>=(1ull<<36) ||
            (reinterpret_cast<uintptr_t>(a.cpu)&4095)) return false;
        for(unsigned j=0;j<i;j++) {
            auto b=j<DATA_PAGES?s->data_page[j]:s->table_page[j-DATA_PAGES];
            if(a.pa==b.pa || a.cpu==b.cpu) return false;
            if(region[i]!=region[j] && ((a.pa>>14)==(b.pa>>14) ||
                (reinterpret_cast<uintptr_t>(a.cpu)>>14)==(reinterpret_cast<uintptr_t>(b.cpu)>>14))) return false;
        }
    }
    return true;
}
sweet_gmu_memory *sweet_gmu_memory_create_legacy(void)
{
    auto *s=static_cast<sweet_gmu_memory *>(IOMallocZero(sizeof(sweet_gmu_memory)));
    if(!s) return nullptr;
    if(sweet_gmu_legacy_layout(&s->layout,0,0) || s->layout.data_pages!=DATA_PAGES ||
        s->layout.table_pages!=TABLE_PAGES) {sweet_gmu_memory_destroy(s);return nullptr;}
    unsigned at=0;
    for(unsigned i=0;i<SWEET_GMU_REGION_COUNT;i++) {
        unsigned count=s->layout.region[i].bytes/GRANULE;
        s->data[i]=sweet_gpu_memory_create_nc(count);
        if(!s->data[i]) {sweet_gmu_memory_destroy(s);return nullptr;}
        for(unsigned j=0;j<count;j++) {
            void *cpu=nullptr;
            if(at>=DATA_PAGES || sweet_gpu_memory_page(s->data[i],j,&cpu,&s->data_page[at].pa)) {
                sweet_gmu_memory_destroy(s);return nullptr;
            }
            s->data_page[at].cpu=static_cast<uint64_t *>(cpu);
            if(j && reinterpret_cast<uintptr_t>(cpu)!=
                reinterpret_cast<uintptr_t>(s->data_page[at-j].cpu)+j*GRANULE) {
                sweet_gmu_memory_destroy(s);return nullptr;
            }
            at++;
        }
    }
    if(at!=DATA_PAGES) {sweet_gmu_memory_destroy(s);return nullptr;}
    s->tables=sweet_gpu_memory_create(TABLE_PAGES);
    if(!s->tables) {sweet_gmu_memory_destroy(s);return nullptr;}
    for(unsigned i=0;i<TABLE_PAGES;i++) {
        void *cpu=nullptr;
        if(sweet_gpu_memory_page(s->tables,i,&cpu,&s->table_page[i].pa)) {
            sweet_gmu_memory_destroy(s);return nullptr;
        }
        s->table_page[i].cpu=static_cast<uint64_t *>(cpu);
    }
    if(!isolated(s)) {sweet_gmu_memory_destroy(s);return nullptr;}
    s->used=1;
    sweet_gpu_table_ops ops={s,alloc_table,release_table,resolve_table};
    sweet_gpu_table_publish_ops pub={s,clean_table,barrier};
    at=0;
    for(unsigned i=0;i<SWEET_GMU_REGION_COUNT;i++) {
        auto r=s->layout.region[i];
        if(i==SWEET_GMU_HFI && sweet_gpu_hfi_table_init(s->data_page[at].cpu,r.bytes,r.iova)) {
            sweet_gmu_memory_destroy(s);return nullptr;
        }
        for(unsigned j=0;j<r.bytes/GRANULE;j++,at++) {
            if(sweet_gpu_table_map_access(s->table_page[0],&ops,
                r.iova+j*GRANULE,s->data_page[at].pa,0,r.privileged) ||
                sweet_gpu_memory_clean(s->data[i],j)) {
                sweet_gmu_memory_destroy(s);return nullptr;
            }
        }
    }
    if(s->used!=63 || sweet_gpu_table_publish_inactive(s->table_page[0],&ops,&pub,
        s->scratch,SWEET_GPU_TABLE_MAX_PAGES)) {sweet_gmu_memory_destroy(s);return nullptr;}
    s->ready=true;return s;
}
int sweet_gmu_memory_info(const sweet_gmu_memory *s,uint64_t *root,
    uint32_t *hfi,uint32_t *dump,uint32_t *log)
{
    if(root) *root=0;
    if(hfi) *hfi=0;
    if(dump) *dump=0;
    if(log) *log=0;
    if(!s || !s->ready || s->destroy_requested || !root || !hfi || !dump || !log) return -1;
    *root=s->table_page[0].pa;
    *hfi=s->layout.region[SWEET_GMU_HFI].iova;
    *dump=s->layout.region[SWEET_GMU_DUMP].iova;
    *log=s->layout.region[SWEET_GMU_LOG].iova;
    return 0;
}
