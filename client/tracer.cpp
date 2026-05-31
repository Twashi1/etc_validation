#include "dr_api.h"
#include "dr_tools.h"
#include "drmgr.h"

#include <atomic>
#include <linux/perf_event.h>
#include <stdint.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <unistd.h>

struct accumulated_stats_t {
  uint64_t cycles{0};
  uint64_t instr{0};
  uint64_t reg_reads{0};
  uint64_t reg_writes{0};
  uint64_t instr_per_sample{0}; // actual instructions traversed, not executed

  // TODO: these stats are of instructions traversed not executed; executed is
  // more useful for power analysis Not a full list of all categories, but
  // probably the main ones we care about
  uint64_t category_fp{0};
  uint64_t category_load{0};
  uint64_t category_store{0};
  uint64_t category_branch{0};
};

// TODO: should consider attaching stats of each instruction type within the bb
struct bb_metadata_t {
  uint64_t exec_count{0};
  void *tag{nullptr};
  uint32_t id{0};
};

struct per_thread_t {
  int cycles_fd;
  int instr_fd;

  accumulated_stats_t stats;
};

static const uint64_t INSTR_SAMPLE_THRESHOLD = 1000;
static const bool PRINT_PER_INSTR = false;
static std::atomic<uint64_t> bb_id_counter{0};
static constexpr uint64_t BB_TABLE_SIZE = 8192;
static bb_metadata_t bb_table[BB_TABLE_SIZE];

static inline uint64_t hash_tag(void *tag) {
  uintptr_t h = reinterpret_cast<uintptr_t>(tag);

  // murmur64 hash
  h ^= h >> 33;
  h *= 0xff51afd7ed558ccdULL;
  h ^= h >> 33;
  h *= 0xc4ceb9fe1a85ec53ULL;
  h ^= h >> 33;

  // relies on BB_TABLE_SIZE being power of 2
  return h & (BB_TABLE_SIZE - 1);
}

static bb_metadata_t *get_or_make_bb_state(void *tag) {
  uint64_t idx = hash_tag(tag);
  // TODO: check math here

  for (uint64_t max_search_count = BB_TABLE_SIZE - 1; max_search_count > 0;
       max_search_count--) {
    bb_metadata_t *slot = &bb_table[idx];

    if (slot->tag == nullptr) {
      slot->tag = tag;
      slot->id = bb_id_counter.fetch_add(1, std::memory_order_relaxed);
      return slot;
    }

    if (slot->tag == tag)
      return slot;

    // simple linear probing
    idx = (idx + 1) & (BB_TABLE_SIZE - 1);
  }

  return nullptr;
}

static bb_metadata_t *get_bb_state(void *tag) {
  uint64_t idx = hash_tag(tag);

  for (uint64_t max_search_count = BB_TABLE_SIZE - 1; max_search_count > 0;
       max_search_count--) {
    bb_metadata_t *slot = &bb_table[idx];

    if (slot->tag == nullptr)
      return nullptr;
    if (slot->tag == tag)
      return slot;

    idx = (idx + 1) & (BB_TABLE_SIZE - 1);
  }

  return nullptr;
}

static int tls_idx;

static void increment_categories_of_instr(accumulated_stats_t *stats,
                                          instr_t *ins) {
  if (instr_is_cbr(ins) || instr_is_ubr(ins))
    stats->category_branch++;
  if (instr_is_floating(ins))
    stats->category_fp++;
  if (instr_reads_memory(ins))
    stats->category_load++;
  if (instr_writes_memory(ins))
    stats->category_store++;
}

static long perf_event_open(struct perf_event_attr *pe, pid_t pid, int cpu,
                            int group_fd, unsigned long flags) {
  return syscall(__NR_perf_event_open, pe, pid, cpu, group_fd, flags);
}

static void count_regs(per_thread_t *t, instr_t *ins) {
  int i, n;

  // Number of source registers
  n = instr_num_srcs(ins);
  for (i = 0; i < n; i++) {
    opnd_t op = instr_get_src(ins, i);
    if (opnd_is_reg(op))
      t->stats.reg_reads++;
  }

  // Number of destination registers
  n = instr_num_dsts(ins);
  for (i = 0; i < n; i++) {
    opnd_t op = instr_get_dst(ins, i);
    if (opnd_is_reg(op))
      t->stats.reg_writes++;
  }
}

static int open_counter(uint64_t config) {
  struct perf_event_attr pe;
  memset(&pe, 0, sizeof(pe));

  pe.type = PERF_TYPE_HARDWARE;
  pe.size = sizeof(pe);
  pe.config = config;
  pe.disabled = 1;
  pe.exclude_kernel = 1;
  pe.exclude_hv = 1;

  int fd = perf_event_open(&pe, 0, -1, -1, 0);

  if (fd < 0) {
    dr_fprintf(STDERR,
               "Failed to open file descriptor of counter, id: %llu, fd value: "
               "%lli. Check kernel security restriction on PMCs\n",
               (unsigned long long)config, (long long)fd);

    return -1;
  }

  ioctl(fd, PERF_EVENT_IOC_RESET, 0);
  ioctl(fd, PERF_EVENT_IOC_ENABLE, 0);

  return fd;
}

static void event_thread_init(void *drcontext) {
  per_thread_t *t =
      (per_thread_t *)dr_thread_alloc(drcontext, sizeof(per_thread_t));

  /*
  PERF_COUNT_HW_CPU_CYCLES
  PERF_COUNT_HW_INSTRUCTIONS
  PERF_COUNT_HW_CACHE_REFERENCES
  PERF_COUNT_HW_CACHE_MISSES
  PERF_COUNT_HW_BRANCH_INSTRUCTIONS
  PERF_COUNT_HW_BRANCH_MISSES
  PERF_COUNT_HW_BUS_CYCLES
  PERF_COUNT_HW_STALLED_CYCLES_FRONTEND
  PERF_COUNT_HW_STALLED_CYCLES_BACKEND
  PERF_COUNT_HW_REF_CPU_CYCLES

  PERF_COUNT_HW_CACHE_L1D
  PERF_COUNT_HW_CACHE_L1I
  PERF_COUNT_HW_CACHE_LL
  PERF_COUNT_HW_CACHE_DTLB
  PERF_COUNT_HW_CACHE_ITLB
  PERF_COUNT_HW_CACHE_BPU

  PERF_COUNT_HW_CACHE_OP_READ
  PERF_COUNT_HW_CACHE_OP_WRITE
  PERF_COUNT_HW_CACHE_OP_PREFETCH
  */

  t->cycles_fd = open_counter(PERF_COUNT_HW_CPU_CYCLES);
  t->instr_fd = open_counter(PERF_COUNT_HW_INSTRUCTIONS);
  t->stats = accumulated_stats_t{};

  drmgr_set_tls_field(drcontext, tls_idx, t);
}

static void dump_stats(per_thread_t *t, const char *tag) {
  // TODO: unsure if should use deltas or should just read value
  if (t->cycles_fd >= 0)
    read(t->cycles_fd, &t->stats.cycles, sizeof(uint64_t));

  if (t->instr_fd >= 0)
    read(t->instr_fd, &t->stats.instr, sizeof(uint64_t));

  dr_fprintf(STDERR, "[%s] cycles=%llu instr=%llu regs(r=%llu w=%llu)\n", tag,
             (unsigned long long)t->stats.cycles,
             (unsigned long long)t->stats.instr,
             (unsigned long long)t->stats.reg_reads,
             (unsigned long long)t->stats.reg_writes);
}

static void event_thread_exit(void *drcontext) {
  per_thread_t *t = (per_thread_t *)drmgr_get_tls_field(drcontext, tls_idx);

  if (!t)
    return;

  dump_stats(t, "FINAL");

  if (t->cycles_fd >= 0)
    close(t->cycles_fd);
  if (t->instr_fd >= 0)
    close(t->instr_fd);

  dr_thread_free(drcontext, t, sizeof(per_thread_t));
}

static dr_emit_flags_t event_bb(void *drcontext, void *tag, instrlist_t *bb,
                                instr_t *instr, bool for_trace,
                                bool translating, void *user_data) {
  per_thread_t *t = (per_thread_t *)drmgr_get_tls_field(drcontext, tls_idx);

  bb_metadata_t *bb_state = get_or_make_bb_state(tag);
  bb_state->exec_count++;

  for (instr_t *ins = instrlist_first_app(bb); ins != NULL;
       ins = instr_get_next_app(ins)) {
    count_regs(t, ins);
    increment_categories_of_instr(&t->stats, ins);
    t->stats.instr_per_sample++;

    if (PRINT_PER_INSTR)
      dr_fprintf(STDERR, "pc=%p op=%s\n", instr_get_app_pc(ins),
                 decode_opcode_name(instr_get_opcode(ins)));
  }

  if (t->stats.instr_per_sample >= INSTR_SAMPLE_THRESHOLD) {
    dump_stats(t, "SAMPLE");
    dr_fprintf(STDERR, "[BB_ADD] tag=%p exec_count=%llu\n",
               (void *)bb_state->tag, (unsigned long long)bb_state->exec_count);
    t->stats.instr_per_sample = 0;
  }

  return DR_EMIT_DEFAULT;
}

DR_EXPORT void dr_client_main(client_id_t id, int argc, const char *argv[]) {
  dr_set_client_name("extvalidation tracer", "");

  drmgr_init();

  tls_idx = drmgr_register_tls_field();

  drmgr_register_thread_init_event(event_thread_init);
  drmgr_register_thread_exit_event(event_thread_exit);

  drmgr_register_bb_instrumentation_event(NULL, event_bb, NULL);
}
