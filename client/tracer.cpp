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
  uint64_t cycles;
  uint64_t instr;
  uint64_t reg_reads;
  uint64_t reg_writes;
  uint64_t instr_per_sample; // actual instructions traversed, not executed

  accumulated_stats_t()
      : cycles(0), instr(0), reg_reads(0), reg_writes(0), instr_per_sample(0) {}
};

struct per_thread_t {
  int cycles_fd;
  int instr_fd;

  accumulated_stats_t stats;
};

static const uint64_t INSTR_SAMPLE_THRESHOLD = 1000;
static const bool PRINT_PER_INSTR = false;

static int tls_idx;

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

  for (instr_t *ins = instrlist_first_app(bb); ins != NULL;
       ins = instr_get_next_app(ins)) {
    count_regs(t, ins);
    t->stats.instr_per_sample++;

    if (PRINT_PER_INSTR)
      dr_fprintf(STDERR, "pc=%p op=%s\n", instr_get_app_pc(ins),
                 decode_opcode_name(instr_get_opcode(ins)));
  }

  if (t->stats.instr_per_sample >= INSTR_SAMPLE_THRESHOLD) {
    dump_stats(t, "SAMPLE");
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
