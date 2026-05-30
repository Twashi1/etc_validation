#include "dr_api.h"
#include "drmgr.h"

#include <linux/perf_event.h>
#include <stdint.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

struct per_thread_t {
  int cycles_fd;
  int instr_fd;

  uint64_t cycles_prev;
  uint64_t instr_prev;

  uint64_t reg_reads;
  uint64_t reg_writes;
};

static int tls_idx;

static long perf_event_open(struct perf_event_attr *pe, pid_t pid, int cpu,
                            int group_fd, unsigned long flags) {
  return syscall(__NR_perf_event_open, pe, pid, cpu, group_fd, flags);
}

static void count_regs(per_thread_t *t, instr_t *ins) {
  int i, n;

  n = instr_num_srcs(ins);
  for (i = 0; i < n; i++) {
    opnd_t op = instr_get_src(ins, i);
    if (opnd_is_reg(op))
      t->reg_reads++;
  }

  n = instr_num_dsts(ins);
  for (i = 0; i < n; i++) {
    opnd_t op = instr_get_dst(ins, i);
    if (opnd_is_reg(op))
      t->reg_writes++;
  }
}

static int open_counter(uint64_t config) {
  struct perf_event_attr pe;
  memset(&pe, 0, sizeof(pe));

  pe.type = PERF_TYPE_HARDWARE;
  pe.size = sizeof(pe);
  pe.config = config;
  pe.disabled = 0;
  pe.exclude_kernel = 1;
  pe.exclude_hv = 1;

  return perf_event_open(&pe, 0, -1, -1, 0);
}

static void event_thread_init(void *drcontext) {
  per_thread_t *t =
      (per_thread_t *)dr_thread_alloc(drcontext, sizeof(per_thread_t));

  t->cycles_fd = open_counter(PERF_COUNT_HW_CPU_CYCLES);
  t->instr_fd = open_counter(PERF_COUNT_HW_INSTRUCTIONS);

  t->cycles_prev = 0;
  t->instr_prev = 0;

  t->reg_reads = 0;
  t->reg_writes = 0;

  drmgr_set_tls_field(drcontext, tls_idx, t);
}

static void event_thread_exit(void *drcontext) {
  per_thread_t *t = (per_thread_t *)drmgr_get_tls_field(drcontext, tls_idx);

  if (!t)
    return;

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

  uint64_t cycles_now = 0;
  uint64_t instr_now = 0;

  if (t->cycles_fd >= 0)
    read(t->cycles_fd, &cycles_now, sizeof(cycles_now));

  if (t->instr_fd >= 0)
    read(t->instr_fd, &instr_now, sizeof(instr_now));

  uint64_t dcycles = cycles_now - t->cycles_prev;
  uint64_t dinstr = instr_now - t->instr_prev;

  t->cycles_prev = cycles_now;
  t->instr_prev = instr_now;

  // TODO: collect and print every n instructions instead of per instruction
  for (instr_t *ins = instrlist_first_app(bb); ins != NULL;
       ins = instr_get_next_app(ins)) {
    count_regs(t, ins);

    // TODO: use format?
    dr_fprintf(
        STDERR, "pc=%p op=%s cycles=%llu instr=%llu regs=(r=%llu w=%llu)\n",
        instr_get_app_pc(ins), decode_opcode_name(instr_get_opcode(ins)),
        (unsigned long long)dcycles, (unsigned long long)dinstr,
        (unsigned long long)t->reg_reads, (unsigned long long)t->reg_writes);
  }

  return DR_EMIT_DEFAULT;
}

DR_EXPORT void dr_client_main(client_id_t id, int argc, const char *argv[]) {
  dr_set_client_name("modern tracer", "");

  drmgr_init();

  tls_idx = drmgr_register_tls_field();

  drmgr_register_thread_init_event(event_thread_init);
  drmgr_register_thread_exit_event(event_thread_exit);

  drmgr_register_bb_instrumentation_event(NULL, event_bb, NULL);
}
