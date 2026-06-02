#include "dr_api.h"
#include "dr_ir_instrlist.h"
#include "dr_tools.h"
#include "drmgr.h"

#include <atomic>
#include <linux/perf_event.h>
#include <mutex>
#include <stdint.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <unistd.h>

// TODO: goal is to collect stats per basic block (static)
// - then collect the order of BB execution (can't without highly expensive
// instrumentation)
// - and also collect PMCs on some period
struct pmc_stats_t {
  // Actual stats (cumulative)
  uint64_t cycles;
  uint64_t instrs;
  uint16_t callee_bb;
};

// TODO: should consider attaching stats of each instruction type within the bb
struct bb_metadata_t {
  uint64_t exec_count{0};
  void *tag{nullptr};
  uint32_t id{0};

  // Note these are per basic block execution
  // Multiply by execution count to get real executed instructions
  uint32_t instr_count{0};
  uint32_t reg_reads{0};
  uint32_t reg_writes{0};

  uint32_t category_fp{0};
  uint32_t category_load{0};
  uint32_t category_store{0};
  uint32_t category_branch{0};
};

struct per_thread_t {
  // File descriptors of PMCs
  int cycles_fd;
  int instr_fd;
};

// BB table map structure
// TODO: wrap into a class
static std::atomic<uint16_t> bb_id_counter{0};
static constexpr uint64_t BB_TABLE_SIZE{8192};
static bb_metadata_t bb_table[BB_TABLE_SIZE] = {0};
static std::mutex bb_table_mutex;

static constexpr uint64_t PMC_LIST_SIZE{65536};
static pmc_stats_t pmc_reads[PMC_LIST_SIZE] = {0};

static int tls_idx;

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

  std::scoped_lock table_lock{bb_table_mutex};

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

  dr_fprintf(STDERR, "Ran out of BB slots\n");

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

static void increment_categories_of_instr(bb_metadata_t *stats, instr_t *ins) {
  if (instr_is_cbr(ins) || instr_is_ubr(ins))
    stats->category_branch++;
  if (instr_is_floating(ins))
    stats->category_fp++;
  if (instr_reads_memory(ins))
    stats->category_load++;
  if (instr_writes_memory(ins))
    stats->category_store++;
  // TODO: check opcode to determine other facts

  stats->instr_count++;
}

static long perf_event_open(struct perf_event_attr *pe, pid_t pid, int cpu,
                            int group_fd, unsigned long flags) {
  return syscall(__NR_perf_event_open, pe, pid, cpu, group_fd, flags);
}

static void count_regs(bb_metadata_t *stats, instr_t *ins) {
  int i, n;

  // Number of source registers
  n = instr_num_srcs(ins);
  for (i = 0; i < n; i++) {
    opnd_t op = instr_get_src(ins, i);
    if (opnd_is_reg(op))
      stats->reg_reads++;
  }

  // Number of destination registers
  n = instr_num_dsts(ins);
  for (i = 0; i < n; i++) {
    opnd_t op = instr_get_dst(ins, i);
    if (opnd_is_reg(op))
      stats->reg_writes++;
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

  drmgr_set_tls_field(drcontext, tls_idx, t);
}

static void dump_stats(per_thread_t *t, const char *tag) {
  uint64_t cycles{0};
  uint64_t instrs{0};

  if (t->cycles_fd >= 0)
    read(t->cycles_fd, &cycles, sizeof(uint64_t));

  if (t->instr_fd >= 0)
    read(t->instr_fd, &instrs, sizeof(uint64_t));

  dr_fprintf(STDERR, "[%s] cycles=%llu instr=%llu\n", tag,
             static_cast<unsigned long long>(cycles),
             static_cast<unsigned long long>(instrs));
}

static void event_thread_exit(void *drcontext) {
  per_thread_t *t = (per_thread_t *)drmgr_get_tls_field(drcontext, tls_idx);

  if (!t)
    return;

  dump_stats(t, "FINAL");

  // Calculate approximate instruction mix
  // TODO: dense hash map data structure would be ideal
  uint64_t est_instruction = 0;
  uint64_t est_floating = 0;
  uint64_t est_load = 0;
  uint64_t est_store = 0;
  uint64_t est_branch = 0;
  uint64_t est_reg_reads = 0;
  uint64_t est_reg_writes = 0;
  uint64_t block_count = 0;

  for (uint64_t i = 0; i < BB_TABLE_SIZE; i++) {
    if (bb_table[i].tag == nullptr)
      continue;

    ++block_count;

    bb_metadata_t *state = &bb_table[i];

    est_instruction += state->instr_count * state->exec_count;
    est_floating += state->category_fp * state->exec_count;
    est_load += state->category_load * state->exec_count;
    est_store += state->category_store * state->exec_count;
    est_branch += state->category_branch * state->exec_count;
    est_reg_reads += state->reg_reads * state->exec_count;
    est_reg_writes += state->reg_writes * state->exec_count;

    dr_fprintf(STDERR, "[BB_OUT] tag: %p, exec count: %llu\n",
               (void *)state->tag, (unsigned long long)state->exec_count);
  }

  dr_fprintf(
      STDERR,
      "Total block count: %llu, est instr: %llu, est float: %llu, reg "
      "reads: %llu, reg writes: %llu\n",
      (unsigned long long)block_count, (unsigned long long)est_instruction,
      (unsigned long long)est_floating, (unsigned long long)est_reg_reads,
      (unsigned long long)est_reg_writes);

  // Cleanup PMCs
  if (t->cycles_fd >= 0)
    close(t->cycles_fd);
  if (t->instr_fd >= 0)
    close(t->instr_fd);

  dr_thread_free(drcontext, t, sizeof(per_thread_t));
}

static dr_emit_flags_t event_bb_analysis(void *drcontext, void *tag,
                                         instrlist_t *bb, bool for_trace,
                                         bool translating, void **user_data) {
  per_thread_t *t = (per_thread_t *)drmgr_get_tls_field(drcontext, tls_idx);

  bb_metadata_t *bb_state = get_or_make_bb_state(tag);

  for (instr_t *ins = instrlist_first_app(bb); ins != NULL;
       ins = instr_get_next_app(ins)) {
    count_regs(bb_state, ins);
    increment_categories_of_instr(bb_state, ins);
  }

  return DR_EMIT_DEFAULT;
}

static dr_emit_flags_t event_bb_insertion(void *drcontext, void *tag,
                                          instrlist_t *bb, instr_t *instr,
                                          bool for_trace, bool translating,
                                          void *user_data) {
  instr_t *first = instrlist_first_app(bb);
  if (first == NULL)
    return DR_EMIT_DEFAULT;

  bb_metadata_t *bb_state = get_or_make_bb_state(tag);

  // instr_t *inc = INSTR_CREATE_add(
  //     drcontext, OPND_CREATE_ABSMEM(&bb_state->exec_count, OPSZ_8),
  //     OPND_CREATE_INT8(1));

  instr_t *inc2 = INSTR_CREATE_inc(
      drcontext, OPND_CREATE_ABSMEM(&bb_state->exec_count, OPSZ_8));

  instrlist_meta_preinsert(bb, instrlist_first(bb), inc2);

  // NOTE: previously we inserted a call to get PMCs here
  // but afaik, that PMC would include a lot of the overhead of DynamoRIO, since
  // it runs at instrumentation of the BB it also would've only executed once
  // per basic block

  return DR_EMIT_DEFAULT;
}

DR_EXPORT void dr_client_main(client_id_t id, int argc, const char *argv[]) {
  dr_set_client_name("extvalidation tracer", "");

  drmgr_init();

  tls_idx = drmgr_register_tls_field();

  drmgr_register_thread_init_event(event_thread_init);
  drmgr_register_thread_exit_event(event_thread_exit);

  drmgr_register_bb_instrumentation_event(event_bb_analysis, event_bb_insertion,
                                          NULL);
}
