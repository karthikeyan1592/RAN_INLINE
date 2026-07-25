#pragma once
/*
 * desc_ring.h — lock-free SPSC descriptor ring buffer.
 *
 * Lives in shared memory (or any mapped region) so the bpftime uprobe
 * handler (producer) and the busy-poll consumer (consumer) both see it.
 *
 * Single-Producer / Single-Consumer: one uprobe site, one poller.
 * If OAI calls LDPCdecoder from multiple threads concurrently, use one
 * ring PER producer thread (keyed by tid) — Phase 3 resolves this with
 * thread-ID evidence before choosing single vs multi-ring.
 *
 * Memory ordering: C11 atomics, release/acquire pairs.
 * No locks, no syscalls, no CAS — just power-of-2 index arithmetic.
 */

#include <stdint.h>
#include <stdatomic.h>
#include <string.h>
#include <assert.h>

/* ---------- descriptor -------------------------------------------------- */

struct ldpc_desc {
    uint64_t timestamp_ns;  /* bpf_ktime_get_ns() at uprobe fire */
    uint32_t slot_id;       /* OAI slot counter (passed via arg) */
    uint32_t cb_index;      /* code-block index within slot (0..C-1) */
    uint64_t llr_off;       /* byte offset into CXL region for LLR input */
    uint32_t llr_len;       /* byte length of LLR buffer */
    uint64_t out_off;       /* byte offset into CXL region for decoded output */
    uint32_t out_len;       /* byte length of output buffer */
    uint32_t seq;           /* producer sequence number (for ordering check) */
    uint8_t  bg;            /* base graph (1 or 2) */
    uint8_t  Zc;            /* lifting size */
    uint8_t  pad[6];
} __attribute__((packed));

/* ---------- ring --------------------------------------------------------- */

#define DESC_RING_CAPACITY  256   /* power of 2; >= 2 * C_MAX * pipeline_depth */
#define DESC_RING_MASK      (DESC_RING_CAPACITY - 1)

typedef struct {
    _Atomic uint64_t head;   /* producer writes here (next slot to write) */
    _Atomic uint64_t tail;   /* consumer reads here (next slot to read) */
    uint8_t pad[48];         /* keep head/tail on separate cache lines */
    struct ldpc_desc entries[DESC_RING_CAPACITY];
} desc_ring_t;

/* Initialise a ring (zero it). Call once before any producer/consumer starts. */
static inline void desc_ring_init(desc_ring_t *r)
{
    memset(r, 0, sizeof(*r));
    atomic_store_explicit(&r->head, 0, memory_order_relaxed);
    atomic_store_explicit(&r->tail, 0, memory_order_relaxed);
}

/*
 * Producer: try to push a descriptor.
 * Returns 1 on success, 0 if ring is full (producer must retry or drop).
 */
static inline int desc_ring_try_push(desc_ring_t *r, const struct ldpc_desc *d)
{
    uint64_t head = atomic_load_explicit(&r->head, memory_order_relaxed);
    uint64_t tail = atomic_load_explicit(&r->tail, memory_order_acquire);

    if ((head - tail) >= DESC_RING_CAPACITY)
        return 0;  /* full */

    r->entries[head & DESC_RING_MASK] = *d;

    /* release-store: makes the entry visible before advancing head */
    atomic_store_explicit(&r->head, head + 1, memory_order_release);
    return 1;
}

/*
 * Consumer: try to pop a descriptor.
 * Returns 1 and fills *d on success, 0 if ring is empty.
 */
static inline int desc_ring_try_pop(desc_ring_t *r, struct ldpc_desc *d)
{
    uint64_t tail = atomic_load_explicit(&r->tail, memory_order_relaxed);
    uint64_t head = atomic_load_explicit(&r->head, memory_order_acquire);

    if (tail == head)
        return 0;  /* empty */

    *d = r->entries[tail & DESC_RING_MASK];

    /* release-store: signals to producer that slot is free */
    atomic_store_explicit(&r->tail, tail + 1, memory_order_release);
    return 1;
}

/* Current fill level (approximate — can race). */
static inline uint64_t desc_ring_size(const desc_ring_t *r)
{
    uint64_t h = atomic_load_explicit(&r->head, memory_order_relaxed);
    uint64_t t = atomic_load_explicit(&r->tail, memory_order_relaxed);
    return h - t;
}
