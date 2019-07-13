#include <setjmp.h>
#include <sys/time.h>

#include "device.h"
#include "memory.h"
#include "mmu.h"
#include "monitor.h"
#include "nemu.h"

CPU_state cpu;

const char *regs[32] = {"0 ", "at", "v0", "v1", "a0", "a1", "a2", "a3",
                        "t0", "t1", "t2", "t3", "t4", "t5", "t6", "t7",
                        "s0", "s1", "s2", "s3", "s4", "s5", "s6", "s7",
                        "t8", "t9", "k0", "k1", "gp", "sp", "fp", "ra"};

#define UNLIKELY(cond) __builtin_expect(!!(cond), 0)
#define LIKELY(cond) __builtin_expect(!!(cond), 1)

#define MAX_INSTR_TO_PRINT 10

nemu_state_t nemu_state = NEMU_STOP;

static uint64_t nemu_start_time = 0;

// 1s = 10^3 ms = 10^6 us
uint64_t get_current_time() { // in us
  struct timeval t;
  gettimeofday(&t, NULL);
  return t.tv_sec * 1000000 + t.tv_usec - nemu_start_time;
}

void print_registers(uint32_t instr) {
  static unsigned int ninstr = 0;
  eprintf("$pc:    0x%08x", cpu.pc);
  eprintf("   ");
  eprintf("$hi:    0x%08x", cpu.hi);
  eprintf("   ");
  eprintf("$lo:    0x%08x", cpu.lo);
  eprintf("\n");

  eprintf("$ninstr: %08x", ninstr);
  eprintf("                  ");
  eprintf("$instr: %08x", instr);
  eprintf("\n");

  for (int i = 0; i < 32; i++) {
    eprintf("$%s:0x%08x%c", regs[i], cpu.gpr[i], (i + 1) % 4 == 0 ? '\n' : ' ');
  }

  ninstr++;
}

/* if you run your os with multiple processes, disable this
 */
#ifdef ENABLE_CAE_CHECK

#  define NR_GPR 32
static uint32_t saved_gprs[NR_GPR];

void save_usual_registers(void) {
  for (int i = 0; i < NR_GPR; i++) saved_gprs[i] = cpu.gpr[i];
}

void check_usual_registers(void) {
  for (int i = 0; i < NR_GPR; i++) {
    if (i == R_k0 || i == R_k1) continue;
    CPUAssert(saved_gprs[i] == cpu.gpr[i], "gpr[%d] %08x <> %08x after eret\n",
              i, saved_gprs[i], cpu.gpr[i]);
  }
}

#endif

void test() {
  cpu.cp0.status.CU = CU0_ENABLE;
  cpu.cp0.status.ERL = 1;
  cpu.cp0.status.BEV = 0;
  cpu.cp0.status.IM = 0x00;

  cpu.cp0.status.BEV = 0;
  cpu.cp0.status.IM = 0xFF;
  cpu.cp0.status.ERL = 0;
  cpu.cp0.status.EXL = 0;
  cpu.cp0.status.IE = 1;

  cpu.cp0.cause.IV = 1;

  cpu.cp0.cpr[CP0_STATUS][0] = 0;
  cpu.cp0.cpr[CP0_CAUSE][0] = 0;
}

int init_cpu(vaddr_t entry) {
  test();

  nemu_start_time = get_current_time();

  cpu.cp0.count[0] = 0;
  cpu.cp0.compare = 0xFFFFFFFF;

  cpu.cp0.status.CU = CU0_ENABLE;
  cpu.cp0.status.ERL = 1;
  cpu.cp0.status.BEV = 1;
  cpu.cp0.status.IM = 0x00;

  cpu.pc = entry;
  cpu.cp0.cpr[CP0_PRID][0] = 0x00018000; // MIPS32 4Kc

  // init cp0 config 0
  cpu.cp0.config.MT = 1; // standard MMU
  cpu.cp0.config.BE = 0; // little endian
  cpu.cp0.config.M = 1;  // config1 present

  // init cp0 config 1
  cpu.cp0.config1.DA = 3; // 4=3+1 ways dcache
  cpu.cp0.config1.DL = 1; // 4=2^(1 + 1) bytes per line
  cpu.cp0.config1.DS = 2; // 256=2^(2 + 6) sets

  cpu.cp0.config1.IA = 3; // 4=3+1 ways ways dcache
  cpu.cp0.config1.IL = 1; // 4=2^(1 + 1) bytes per line
  cpu.cp0.config1.IS = 2; // 256=2^(2 + 6) sets

  cpu.cp0.config1.MMU_size = 63; // 64 TLB entries

  return 0;
}

struct {
  vaddr_t vaddr;
  uint8_t *ptr;
} softmmu;

static inline void clear_softmmu() {
  softmmu.vaddr = 0;
  softmmu.ptr = NULL;
}

static inline void update_softmmu(vaddr_t vaddr, paddr_t paddr, device_t *dev) {
  if (dev->map) {
    softmmu.vaddr = vaddr & ~0xFFF;
    softmmu.ptr = dev->map((paddr & ~0xFFF) - dev->start, 0);
  }
}

static inline uint32_t load_mem(vaddr_t addr, int len) {
  if (softmmu.vaddr == (addr & ~0xFFF)) {
    return *((uint32_t *)&softmmu.ptr[addr & 0xFFF]) &
           (~0u >> ((4 - len) << 3));
  } else {
    paddr_t paddr = prot_addr(addr, MMU_LOAD);
    device_t *dev = find_device(paddr);
    CPUAssert(dev && dev->read, "bad addr %08x\n", addr);
    update_softmmu(addr, paddr, dev);
    return dev->read(paddr - dev->start, len);
  }
}

static inline void store_mem(vaddr_t addr, int len, uint32_t data) {
  if (softmmu.vaddr == (addr & ~0xFFF)) {
    memcpy(&softmmu.ptr[addr & 0xFFF], &data, len);
  } else {
    paddr_t paddr = prot_addr(addr, MMU_STORE);
    device_t *dev = find_device(paddr);
    CPUAssert(dev && dev->write, "bad addr %08x\n", addr);
    update_softmmu(addr, paddr, dev);
    dev->write(paddr - dev->start, len, data);
  }
}

void signal_exception(int code) {
  if (code == EXC_TRAP) {
    eprintf("\e[31mHIT BAD TRAP @%08x\e[0m\n", cpu.pc);
    exit(0);
  }

#ifdef ENABLE_CAE_CHECK
  save_usual_registers();
#endif

  if (cpu.is_delayslot) {
    cpu.cp0.epc = cpu.pc - 4;
    cpu.cp0.cause.BD = cpu.is_delayslot && cpu.cp0.status.EXL == 0;
    cpu.is_delayslot = false;
  } else {
    cpu.cp0.epc = cpu.pc;
  }

  // eprintf("signal exception %d@%08x, badvaddr:%08x\n",
  // code, cpu.pc, cpu.cp0.badvaddr);

  uint32_t ebase = 0xbfc00000;
  cpu.has_exception = true;

#ifdef __ARCH_LOONGSON__
  /* for loongson testcase, the only exception entry is
   * 'h0380' */
  cpu.br_target = ebase + 0x0380;
  if (code == EXC_TLBM || code == EXC_TLBL || code == EXC_TLBS)
    cpu.br_target = ebase;
#else
  /* reference linux: arch/mips/kernel/cps-vec.S */
  if (cpu.cp0.status.BEV) ebase = 0x80000000;

  switch (code) {
  case EXC_INTR:
    if (cpu.cp0.cause.IV) {
      cpu.br_target = ebase + 0x0200;
    } else {
      cpu.br_target = ebase + 0x0180;
    }
    break;
  case EXC_TLBM:
  case EXC_TLBL:
  case EXC_TLBS: cpu.br_target = ebase + 0x0000; break;
  default: /* usual exception */ cpu.br_target = ebase + 0x0180; break;
  }
#endif

#ifdef ENABLE_SEGMENT
  cpu.base = 0; // kernel segment base is zero
#endif

  cpu.cp0.status.EXL = 1;

  cpu.cp0.cause.ExcCode = code;
}

void check_ipbits(bool ie) {
  if (ie && (cpu.cp0.status.IM & cpu.cp0.cause.IP)) {
    signal_exception(EXC_INTR);
  }
}

void update_cp0_timer() {
  ++(*(uint64_t *)cpu.cp0.count);

  // update IP
  if (cpu.cp0.compare != 0 && cpu.cp0.count[0] == cpu.cp0.compare) {
    eprintf("cause INTR@%08x\n", cpu.pc);
    cpu.cp0.cause.IP |= CAUSE_IP_TIMER;
  }
}

/* Simulate how the CPU works. */
void cpu_exec(uint64_t n) {
  if (work_mode == MODE_GDB && nemu_state != NEMU_END) {
    /* assertion failure handler */
    extern jmp_buf gdb_mode_top_caller;
    int code = setjmp(gdb_mode_top_caller);
    if (code != 0) nemu_state = NEMU_END;
  }

  if (nemu_state == NEMU_END) {
    printf(
        "Program execution has ended. To restart the "
        "program, exit NEMU and run again.\n");
    return;
  }

  nemu_state = NEMU_RUNNING;

  for (; n > 0; n--) {
#ifdef ENABLE_INTR
    update_cp0_timer();
#endif

#ifdef DEBUG
    instr_enqueue_pc(cpu.pc);
#endif

#ifdef ENABLE_EXCEPTION
    if ((cpu.pc & 0x3) != 0) {
      cpu.cp0.badvaddr = cpu.pc;
      signal_exception(EXC_AdEL);
      goto check_exception;
    }
#endif

    Inst inst = {.val = load_mem(cpu.pc, 4)};

#ifdef DEBUG
    instr_enqueue_instr(inst.val);
#endif

#include "exec-handlers.h"

#ifdef DEBUG
    if (work_mode == MODE_LOG) print_registers(inst.val);
#endif

  check_exception:;
#if defined(ENABLE_EXCEPTION) || defined(ENABLE_INTR)
    bool ie =
        !(cpu.cp0.status.ERL) && !(cpu.cp0.status.EXL) && cpu.cp0.status.IE;
    check_ipbits(ie);
#endif

    if (cpu.has_exception) {
      cpu.has_exception = false;
      cpu.pc = cpu.br_target;
    }

    if (nemu_state != NEMU_RUNNING) { return; }
  }

  if (nemu_state == NEMU_RUNNING) { nemu_state = NEMU_STOP; }
}
