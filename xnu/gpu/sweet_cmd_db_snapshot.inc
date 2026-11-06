/* Included by sweet_rpmh.c: share its existing mapping and lock. This
 * diagnostic emits only the firmware command database, never scans RAM,
 * sends a TCS command, or changes a resource vote. The exact size is the
 * observed sweet_k6a database size; a different layout needs review. */
int sweet_rpmh_cmd_db_snapshot(void);
int
sweet_rpmh_cmd_db_snapshot(void)
{
    static unsigned attempted;
    uint32_t hash = 2166136261u;
    int error;
    if (__atomic_exchange_n(&attempted, 1, __ATOMIC_ACQ_REL)) return EALREADY;
    if (__atomic_exchange_n(&rh_lock, 1, __ATOMIC_ACQUIRE)) return EBUSY;
    if (rh_poisoned) { error = EIO; goto out; }
    error = rh_map();
    if (error) goto out;
    if (!rh_db || rh_db_size != 0x1078u) { error = EINVAL; goto out; }
    printf("SWEET CMDDB: begin bytes=%08x\n", rh_db_size);
    for (uint32_t off = 0; off < rh_db_size; off += 16u) {
        uint32_t words[4] = {0, 0, 0, 0};
        unsigned n = (rh_db_size - off) / 4u;
        if (n > 4) n = 4;
        for (unsigned j = 0; j < n; ++j) {
            words[j] = rh_read(rh_db, off + 4u * j);
            for (unsigned b = 0; b < 4; ++b) {
                hash ^= (words[j] >> (8u * b)) & 255u;
                hash *= 16777619u;
            }
        }
        printf("SWEET CMDDB: data %04x %u %08x %08x %08x %08x\n",
            off, n, words[0], words[1], words[2], words[3]);
    }
    printf("SWEET CMDDB: end bytes=%08x fnv1a=%08x\n", rh_db_size, hash);
out:
    __atomic_store_n(&rh_lock, 0, __ATOMIC_RELEASE);
    return error;
}
