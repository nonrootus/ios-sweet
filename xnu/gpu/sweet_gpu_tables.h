#ifndef SWEET_GPU_TABLES_H
#define SWEET_GPU_TABLES_H
#include <stddef.h>
#include <stdint.h>
struct sweet_gpu_table_page { uint64_t *cpu; uint64_t pa; };
struct sweet_gpu_table_ops {
    void *ctx;
    int (*alloc)(void *ctx, struct sweet_gpu_table_page *out);
    void (*release)(void *ctx, struct sweet_gpu_table_page page);
    uint64_t *(*resolve)(void *ctx, uint64_t pa);
};
/* All operations require external synchronization. Allocated pages must be
 * owned, 4K-aligned, zero-filled and accessible. This does not synchronize
 * caches, invalidate a TLB, set an SMMU context, or enforce page ownership. */
int sweet_gpu_table_map(struct sweet_gpu_table_page root,
    const struct sweet_gpu_table_ops *ops, uint64_t iova, uint64_t pa, int read_only);
int sweet_gpu_table_map_access(struct sweet_gpu_table_page root,
    const struct sweet_gpu_table_ops *ops, uint64_t iova, uint64_t pa,
    int read_only, int privileged);
int sweet_gpu_table_unmap(struct sweet_gpu_table_page root,
    const struct sweet_gpu_table_ops *ops, uint64_t iova);
/* Publish a newly built, inactive 32-bit IOVA tree to a non-coherent walker.
 * sync cleans one 4096-byte CPU page to the point of coherency; barrier must
 * complete all preceding cleans before returning. Both callbacks must report
 * errors. Requires exclusive ownership, no SMMU access until success, and a
 * live context and TLB setup separately. On failure discard/rebuild the tree;
 * this does not support live map/unmap or invalidate any TLB. */
struct sweet_gpu_table_publish_ops {
    void *ctx;
    int (*sync)(void *ctx, uint64_t *cpu, size_t size);
    int (*barrier)(void *ctx);
};
/* Caller provides scratch for at least 2053 addresses in heap/static memory,
 * disjoint from all tables and never on the limited kernel stack. resolve
 * must be stable, one-to-one and reflect owned pages throughout all passes.
 * The caller must also prove every data leaf belongs to an owned pinned pool. */
#define SWEET_GPU_TABLE_MAX_PAGES 2053u
int sweet_gpu_table_publish_inactive(struct sweet_gpu_table_page root,
    const struct sweet_gpu_table_ops *ops, const struct sweet_gpu_table_publish_ops *pub,
    uint64_t *scratch, size_t scratch_count);
#endif
