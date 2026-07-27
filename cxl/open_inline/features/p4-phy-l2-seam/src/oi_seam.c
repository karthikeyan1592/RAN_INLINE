/* oi_seam.c — see oi_seam.h for the API contract. Backing mechanism (P4-R10): a REGULAR file
 * (not /dev/shm tmpfs) mmap'd MAP_SHARED -- the file itself, sitting on a docker NAMED VOLUME
 * (compose.p4.yml's own concern, not this code's), is what survives either container's
 * independent restart. This C file has no opinion on what's backing the directory it's told to
 * open a file in; that's the deployment layer's job.
 *
 * Epoch field synchronization note: `epoch` (both header and slot) is declared plain uint32_t
 * (matching the LLD's own struct definition literally, oi_seam_ring.h), not _Atomic. Its
 * visibility is carried transitively by the release/acquire operations on the ADJACENT head/tail/
 * status atomics that always surround every real read or write of it (a restart always bumps
 * epoch immediately before resetting head/tail with atomic stores; every reader observes epoch
 * only right after an atomic load of head/tail or a slot's status) -- the same informal-but-real
 * pattern the CXL PoC's own desc_ring_t uses (not every field there is individually atomic either,
 * only head/tail). SIM-tier code, not a from-scratch memory-model proof.
 */
#define _POSIX_C_SOURCE 200809L
#include "oi_seam.h"

#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

struct oi_seam_ring_s {
  int fd;
  void* map;
  size_t map_len;
  oi_seam_ring_hdr_t* hdr;
  oi_seam_slot_t* slots;
  char consumer_state_path[512];
  int have_consumer_state_path;
};

static size_t ring_bytes(uint32_t capacity) {
  return sizeof(oi_seam_ring_hdr_t) + (size_t)capacity * sizeof(oi_seam_slot_t);
}

/* Bounded backoff: spin -> sched_yield -> nanosleep, escalating -- no deadline (SIM tier, P4-R7),
 * callers needing a bound (test harnesses) impose their own external timeout. */
static void backoff(unsigned* attempt) {
  if (*attempt < 1000) {
    /* pure spin */
  } else if (*attempt < 100000) {
    sched_yield();
  } else {
    struct timespec ts = {0, 1000000}; /* 1ms */
    nanosleep(&ts, NULL);
  }
  (*attempt)++;
}

oi_seam_ring_t* oi_seam_open(const oi_seam_config_t* cfg, int create, oi_seam_status_code_t* out_status) {
  oi_seam_status_code_t st = OI_SEAM_OK;
  oi_seam_ring_t* r = (oi_seam_ring_t*)calloc(1, sizeof(*r));
  if (!r) {
    st = OI_SEAM_ERR_OPEN;
    goto fail_no_fd;
  }
  r->fd = -1;

  size_t want_len = ring_bytes(cfg->ring_capacity);

  int flags = create ? (O_RDWR | O_CREAT) : O_RDWR;
  r->fd = open(cfg->ring_path, flags, 0644);
  if (r->fd < 0) {
    st = create ? OI_SEAM_ERR_OPEN : OI_SEAM_ERR_ABSENT;
    goto fail;
  }

  struct stat sb;
  if (fstat(r->fd, &sb) != 0) {
    st = OI_SEAM_ERR_OPEN;
    goto fail;
  }
  int is_fresh = (sb.st_size == 0);
  if (!create && is_fresh) {
    st = OI_SEAM_ERR_ABSENT; /* consumer found a zero-length file: producer never initialized it */
    goto fail;
  }
  if (create && is_fresh) {
    if (ftruncate(r->fd, (off_t)want_len) != 0) {
      st = OI_SEAM_ERR_OPEN;
      goto fail;
    }
  } else if ((size_t)sb.st_size != want_len) {
    /* Existing file, but the WRONG size for this cfg -- either header mismatch or the file
     * predates a different ring_capacity. Treat as header mismatch either way (P4-R14): safer
     * than partially mapping/reinterpreting a differently-sized segment. */
    st = OI_SEAM_ERR_HEADER_MISMATCH;
    goto fail;
  }

  r->map = mmap(NULL, want_len, PROT_READ | PROT_WRITE, MAP_SHARED, r->fd, 0);
  if (r->map == MAP_FAILED) {
    st = OI_SEAM_ERR_OPEN;
    goto fail;
  }
  r->map_len = want_len;
  r->hdr = (oi_seam_ring_hdr_t*)r->map;
  r->slots = (oi_seam_slot_t*)((uint8_t*)r->map + sizeof(oi_seam_ring_hdr_t));

  if (create && is_fresh) {
    /* First-ever init: zero everything, write a fresh header, epoch=0. */
    memset(r->map, 0, want_len);
    r->hdr->magic = OI_SEAM_MAGIC;
    r->hdr->format_version = cfg->format_version;
    r->hdr->ring_capacity = cfg->ring_capacity;
    r->hdr->slot_bytes = (uint32_t)sizeof(oi_seam_slot_t);
    atomic_store_explicit(&r->hdr->head, 0, memory_order_relaxed);
    atomic_store_explicit(&r->hdr->tail, 0, memory_order_relaxed);
    r->hdr->epoch = 0;
  } else if (create && !is_fresh) {
    /* Producer (re)start against an EXISTING segment: validate, then bump epoch + reset
     * head/tail (P4-R8) -- crash-recovery / redeploy path. */
    if (r->hdr->magic != OI_SEAM_MAGIC || r->hdr->format_version != cfg->format_version ||
       r->hdr->ring_capacity != cfg->ring_capacity || r->hdr->slot_bytes != sizeof(oi_seam_slot_t)) {
      st = OI_SEAM_ERR_HEADER_MISMATCH;
      goto fail_mapped;
    }
    r->hdr->epoch = r->hdr->epoch + 1;
    atomic_store_explicit(&r->hdr->head, 0, memory_order_release);
    atomic_store_explicit(&r->hdr->tail, 0, memory_order_release);
  } else {
    /* Consumer attach to an existing segment: validate only, never modify. */
    if (r->hdr->magic != OI_SEAM_MAGIC || r->hdr->format_version != cfg->format_version ||
       r->hdr->ring_capacity != cfg->ring_capacity || r->hdr->slot_bytes != sizeof(oi_seam_slot_t)) {
      st = OI_SEAM_ERR_HEADER_MISMATCH;
      goto fail_mapped;
    }
  }

  if (cfg->consumer_state_path) {
    strncpy(r->consumer_state_path, cfg->consumer_state_path, sizeof(r->consumer_state_path) - 1);
    r->have_consumer_state_path = 1;
  }

  if (out_status) *out_status = OI_SEAM_OK;
  return r;

fail_mapped:
  munmap(r->map, r->map_len);
fail:
  if (r->fd >= 0) close(r->fd);
  free(r);
fail_no_fd:
  if (out_status) *out_status = st;
  return NULL;
}

void oi_seam_close(oi_seam_ring_t* r) {
  if (!r) return;
  if (r->map) munmap(r->map, r->map_len);
  if (r->fd >= 0) close(r->fd);
  free(r);
}

oi_seam_slot_t* oi_seam_reserve(oi_seam_ring_t* r, uint64_t* out_seq) {
  unsigned attempt = 0;
  uint64_t head;
  for (;;) {
    head = atomic_load_explicit(&r->hdr->head, memory_order_relaxed);
    uint64_t tail = atomic_load_explicit(&r->hdr->tail, memory_order_acquire);
    if (head - tail < r->hdr->ring_capacity) {
      uint64_t expected = head;
      if (atomic_compare_exchange_weak_explicit(&r->hdr->head, &expected, head + 1, memory_order_relaxed,
                                                memory_order_relaxed)) {
        break; /* claimed seq == head */
      }
      continue; /* lost the race (single-producer design, Q4 -- defensive only) */
    }
    backoff(&attempt);
  }
  uint64_t idx = head % r->hdr->ring_capacity;
  oi_seam_slot_t* slot = &r->slots[idx];
  slot->seq = head;
  slot->epoch = r->hdr->epoch;
  atomic_store_explicit(&slot->status, OI_SEAM_RESERVED, memory_order_relaxed);
  if (out_seq) *out_seq = head;
  return slot;
}

void oi_seam_publish(oi_seam_slot_t* s, oi_seam_status_t st) {
  atomic_store_explicit(&s->status, (uint32_t)st, memory_order_release);
}

oi_seam_slot_t* oi_seam_wait_status(oi_seam_ring_t* r, uint64_t idx, oi_seam_status_t want) {
  oi_seam_slot_t* slot = &r->slots[idx % r->hdr->ring_capacity];
  unsigned attempt = 0;
  for (;;) {
    uint32_t st = atomic_load_explicit(&slot->status, memory_order_acquire);
    if (st == (uint32_t)want) return slot;
    backoff(&attempt);
  }
}

static void persist_consumer_state(oi_seam_ring_t* r, uint32_t epoch, uint64_t tail) {
  if (!r->have_consumer_state_path) return;
  char tmp_path[600];
  snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", r->consumer_state_path);
  FILE* f = fopen(tmp_path, "w");
  if (!f) return;
  fprintf(f, "{\"schema\":\"oi-p4-consumer-state/1\",\"epoch\":%u,\"tail\":%llu}\n", epoch,
         (unsigned long long)tail);
  fclose(f);
  rename(tmp_path, r->consumer_state_path); /* atomic replace, never a torn read for a concurrent reader */
}

void oi_seam_release(oi_seam_ring_t* r, uint64_t idx) {
  uint64_t phys = idx % r->hdr->ring_capacity;
  oi_seam_slot_t* slot = &r->slots[phys];
  atomic_store_explicit(&slot->status, (uint32_t)OI_SEAM_DONE, memory_order_release);
  uint64_t new_tail = atomic_fetch_add_explicit(&r->hdr->tail, 1, memory_order_release) + 1;
  persist_consumer_state(r, r->hdr->epoch, new_tail);
}

uint32_t oi_seam_epoch(const oi_seam_ring_t* r) { return r->hdr->epoch; }

uint64_t oi_seam_head(const oi_seam_ring_t* r) {
  return atomic_load_explicit(&r->hdr->head, memory_order_acquire);
}
uint64_t oi_seam_tail(const oi_seam_ring_t* r) {
  return atomic_load_explicit(&r->hdr->tail, memory_order_acquire);
}
oi_seam_slot_t* oi_seam_slot_at(oi_seam_ring_t* r, uint64_t idx) { return &r->slots[idx % r->hdr->ring_capacity]; }

int oi_seam_read_consumer_state(const char* path, uint32_t* out_epoch, uint64_t* out_tail) {
  *out_epoch = 0;
  *out_tail = 0;
  FILE* f = fopen(path, "r");
  if (!f) return 0;
  unsigned epoch = 0;
  unsigned long long tail = 0;
  int found = fscanf(f, "{\"schema\":\"oi-p4-consumer-state/1\",\"epoch\":%u,\"tail\":%llu}", &epoch, &tail);
  fclose(f);
  if (found != 2) return 0;
  *out_epoch = epoch;
  *out_tail = (uint64_t)tail;
  return 1;
}
