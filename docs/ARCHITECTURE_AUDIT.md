# MyOS — аудит архитектуры (proc-runner + SMP)

Полный разбор архитектуры, инвариантов, истории багов и оставшихся рисков.
Актуально после стабилизации `exec threads.elf` на 8 CPU (июль 2026).

Связано: [PROC_RUNNER.md](PROC_RUNNER.md), [SMP.md](SMP.md), [THREADS_DEMO.md](THREADS_DEMO.md),
[ROADMAP.md](ROADMAP.md), [DEVELOPMENT.md](DEVELOPMENT.md).

---

## 1. Итоговая оценка

| Область | Статус | Комментарий |
|---------|--------|-------------|
| Proc-runner (1 LWKT на процесс) | ✅ | `threads.elf`: mutex, join, yield |
| In-proc uthread scheduler | ✅ | `MYOS_ERR_AGAIN` + retry в userland |
| Syscall ↔ runner переход | ✅ | syscall stack, longjmp, asm restore |
| SMP LWKT scheduler | ⚠️ | work-steal есть; shell в READ держит BSP |
| Прерывания | ✅ | vector/rip фикс; preempt guard при `in_syscall` |
| Изоляция памяти (CR3) | ⚠️ | per-proc PT есть; safe copy user буферов — нет |
| Zombie uthread cleanup | ⚠️ | слоты не переиспользуются сразу после join |

**Вердикт:** архитектура **концептуально верна** для демо и отладки SMP.
Для production-grade нужны пункты из раздела 9.

---

## 2. Слои системы

```
┌─────────────────────────────────────────────────────────────┐
│  Userland (ring 3, per-proc CR3)                            │
│    shell.elf, threads.elf — int $0x80, MYOS_ERR_AGAIN retry │
└──────────────────────────┬──────────────────────────────────┘
                           │ syscall (vector 0x80)
┌──────────────────────────▼──────────────────────────────────┐
│  Syscall layer (syscall_stack, TSS.RSP0)                    │
│    stash RIP/RSP/callee-saved → dispatch → post_dispatch    │
└──────────────────────────┬──────────────────────────────────┘
                           │
┌──────────────────────────▼──────────────────────────────────┐
│  Proc-runner (1 LWKT p{N} на процесс)                       │
│    proc_runner_entry: setjmp → pick uthread → user_enter    │
│    uthread_yield/exit → runner_reswitch → runner_longjmp    │
└──────────────────────────┬──────────────────────────────────┘
                           │
┌──────────────────────────▼──────────────────────────────────┐
│  LWKT scheduler (per-CPU run_queues, work-steal)          │
│    p1 shell, p2 child, msgd, kbdd, idle×N                  │
└──────────────────────────┬──────────────────────────────────┘
                           │
┌──────────────────────────▼──────────────────────────────────┐
│  SMP (GS→cpu, LAPIC timer/IPI, cpu_list_lock)               │
└─────────────────────────────────────────────────────────────┘
```

### Ключевые сущности

| Сущность | Файл | Роль |
|----------|------|------|
| `struct proc` | `include/proc.h` | CR3, mutexes, `run_queue`, `runner`, `current_uthread` |
| `struct lwkt_thread` | `include/lwkt.h` | kernel thread: `stack`, `syscall_stack`, `user_proc`, `in_syscall` |
| `struct uthread` | `include/uthread.h` | user thread в proc: `user_rip/rsp`, callee-saved, `uthread_id` |
| `struct cpu` | `include/cpu.h` | per-CPU: `current`, `run_queues[]`, idle, `sched_active` |

---

## 3. Планирование

### 3.1. LWKT (межпроцессный / kernel)

- Per-CPU run queues по приоритету (`kernel/sched/lwkt.c`).
- **Work-steal:** idle CPU забирает поток с другого ядра (`dequeue_thread`).
- Kernel LWKT: `msgd`, `kbdd` — классические 1:1 потоки.
- User runners: `p1` (shell), `p2` (child) — один LWKT на процесс.

| Поток | Priority | Назначение |
|-------|----------|------------|
| shell `p1` | `LWKT_PRIO_HIGH` (2) | интерактивность |
| child `pN` | `LWKT_PRIO_HIGH` (2) при exec | быстрый старт |
| msgd / kbdd | NORMAL (8) | фоновые сервисы |
| idle | LOW (15) | ожидание на AP |

### 3.2. Uthread (внутри процесса)

```
proc_runner_entry:
  runner_setjmp(&runner_jmp)
  u = proc_sched_pick(p)
  user_enter(u->rip, u->rsp, u->user_rdi, u->user_syscall_ret, ...)
  # возврат через uthread_yield / uthread_exit → runner_longjmp
```

Переключение uthread из syscall:

1. `syscall_dispatch` сохраняет `user_rip/rsp/rdi` и callee-saved.
2. `uthread_yield()` — enqueue текущий, pick следующий.
3. `runner_reswitch = 1` → `syscall_post_dispatch` → `runner_longjmp`.
4. Runner подхватывает другой uthread → `user_enter`.

Блокирующие syscall (mutex, join): kernel → `MYOS_ERR_AGAIN`, userland — цикл в `user/myos.h`.

### 3.3. Критические инварианты планировщика

```
① int $0x80 НИКОГДА не вызывает lwkt_switch() напрямую
② in_syscall=1 блокирует lwkt_switch и preempt из IRQ
③ uthread reswitch только через runner_longjmp в syscall_post_dispatch
④ lwkt_syscall_wait_edge() — только sti;hlt, БЕЗ lwkt_yield
⑤ proc_mutex / uthread_join НЕ вызывают lwkt_syscall_resched после uthread_yield
⑥ pick_enqueue_cpu: при exec из syscall — runner на другой CPU
⑦ callee-saved (rbx,rbp,r12-r15) restore только в user_enter_asm
⑧ reload runner/p из lwkt_curthread() после каждого runner_setjmp
```

---

## 4. Syscall path

### Два стека на LWKT-runner

```c
struct lwkt_thread {
    uint8_t stack[STACK_SIZE];              // 8 KiB — runner, kernel code
    uint8_t syscall_stack[SYSCALL_STACK_SIZE]; // 8 KiB — int $0x80
};
```

TSS.RSP0 → вершина `syscall_stack`. `proc_runner_entry` работает на `stack`.

### Asm-поток (`kernel/arch/x86_64/isr.asm`)

```
isr128 → syscall_common:
  cli
  push всех регистров + callee-saved slot
  call syscall_dispatch
  call syscall_post_dispatch    # runner_longjmp если runner_reswitch
  iretq → user
```

`user_enter_asm` восстанавливает callee-saved из `struct uthread` **перед iretq** (не в C).

### Два пути возврата из syscall

| Путь | Когда | Механизм |
|------|-------|----------|
| iretq | тот же uthread продолжает | RAX = ret; callee из syscall frame |
| runner_longjmp | yield / join / mutex в proc | noreturn в `proc_runner_entry` |

---

## 5. Прерывания

| Vector | Источник | Действие |
|--------|----------|----------|
| 32–47 | PIC (timer, keyboard) | timer → `lwkt_preempt_request` |
| 64 | LAPIC timer | preempt + EOI |
| 65 | LAPIC IPI reschedule | будит AP для work-steal |
| 128 | `int $0x80` | syscall |
| 0–31 | CPU exceptions | panic: rip, cr2, lwkt name |

Защита при syscall:

- `interrupt.c`: `lwkt_preempt_check()` не вызывается при `lwkt_in_usersyscall()`.
- `lwkt_switch()`: no-op если `prev->in_syscall`.
- `syscall_common`: `cli` на время обработки (нет nested IRQ на syscall stack).

---

## 6. Изоляция памяти

### Модель

- Higher-half kernel (Limine HHDM).
- Per-proc CR3: `vmm_aspace_create()` — kernel half (PML4 256–511) общий, user half свой.
- User layout (`include/myos_abi.h`):
  - code: `0x400000`
  - heap: `0x500000+`
  - stack: `0x600000–0x700000`

Переключение: `lwkt_switch` → `lwkt_apply_cr3(next)` → `next->user_cr3 = p->cr3`.

### Сильные стороны

- Процессы не разделяют user physical pages.
- `user_page_alloc` / `user_stack_alloc` мапят только в CR3 текущего proc.
- `proc_destroy` → `vmm_aspace_destroy` (отложенно, если CR3 активен).

### Слабые стороны

| Проблема | Риск |
|----------|------|
| `copy_from_user` — цикл без fault handler | kernel trust user pointer |
| `sys_exec` без ACL | любой proc может exec любой модуль |
| Нет W^X / guard pages | stack overflow в user |
| Zombie uthread slots | косметическая утечка slot ID |

---

## 7. SMP

- BSP (CPU 0): shell runner `p1`.
- AP (1–7): idle + work-steal.
- `cpu_list_lock`: `cpu_count` публикуется после полной инициализации `cpus[idx]`.

### Exec при shell в READ

```
shell на CPU 0: READ + hlt (RUNNING, in_syscall=1)
  → child runner нельзя запустить на том же CPU
  → pick_enqueue_cpu: runner на другой CPU
  → exec_spawn → lwkt_sched_ipi_others()
  → AP забирает p2 через work-steal
```

### Idle AP

- Просыпается только на **user_proc runners** (`any_cpu_has_ready_user_runner`).
- Не крутится из-за `msgd` / `kbdd` в READY.
- `lwkt_syscall_wait_edge`: **без** IPI на каждый poll (избегаем livelock).

---

## 8. История багов и исправлений

### Фаза A — Boot / SMP

| # | Симптом | Причина | Фикс |
|---|---------|---------|------|
| A1 | Exception 0 на всех CPU | Неверные offset'ы args в `isr_common` | `rdi/rsi/rdx` из stack frame |
| A2 | Boot hang после SMP selftest | То же | То же |
| A3 | Редкие SMP краши | `cpu_count++` до init `cpus[idx]` | `cpu_list_lock` |

### Фаза B — `exec threads.elf` → reboot

| # | Симптом | Причина | Фикс |
|---|---------|---------|------|
| B1 | Page fault, битый user rbp | Callee-saved не восстанавливались | stash в dispatch, restore в `user_enter_asm` |
| B2 | Краш после longjmp | Stale locals после setjmp | reload `runner`/`p` после setjmp |
| B3 | Перекрытие стеков | syscall на стеке runner | `syscall_stack` + TSS.RSP0 |
| B4 | longjmp с глубокого syscall stack | longjmp из середины dispatch | `runner_reswitch` → `syscall_post_dispatch` |
| B5 | lwkt_yield до longjmp | `lwkt_syscall_resched` после `uthread_yield` | убрать resched; guard в resched |
| B6 | Mutex не блокирует sibling uthread | holder = LWKT, не uthread | `uthread_holder` в `proc_mutex` |
| B7 | Потеря хвоста LWKT queue | guard `MAX_THREADS` в enqueue | двойной указатель |

### Фаза C — после успешного threads.elf

| # | Симптом | Причина | Фикс |
|---|---------|---------|------|
| C1 | GPF #13 в `syscall_stash_user_callee` | `lwkt_yield` из wait_edge портил RSP | wait_edge = только `sti;hlt` |
| C2 | threads.elf долго в `ready` | p2 в очереди CPU 0, shell держит CPU | `pick_enqueue_cpu` + IPI после exec |
| C3 | Hang после `cpus` | IPI storm + idle spin на msgd READY | только `user_proc` runners; без IPI в wait |
| C4 | Битый `current_uthread` | запись по невалидному указателю | `uthread_ptr_valid()` |

### Хронология симптомов

```
Boot OK → shell OK
exec threads.elf → REBOOT           (B1–B7)
threads OK → cpus → GPF             (C1)
threads OK, долго в ready           (C2)
threads OK, cpus OK → hang          (C3)
Стабильно: threads + cpus + uthreads + prompt
```

---

## 9. Технический долг (приоритет)

1. **Zombie reaper** — переиспользование uthread slots после join.
2. **`copy_from_user_safe`** — page fault handler для user pointers.
3. **Exec ACL** — кто может запускать какие модули.
4. **Console serialisation** — prompt не должен перемешиваться с child output.
5. **Документация PROC_RUNNER** — синхронизировать с текущим wait_edge (без yield).

---

## 10. Регрессионный чеклист

После изменений в `uthread.c`, `lwkt.c`, `syscall.c`, `isr.asm`, `kbdd.c`, `smp.c`:

```bash
make run    # SMP=8 в Makefile
```

| # | Действие | Ожидание |
|---|----------|----------|
| 1 | `help` | список команд |
| 2 | `exec hello.elf` | hello, exit, prompt |
| 3 | `exec threads.elf` | workers, `join: … counter=10`, Process 2 exited |
| 4 | `cpus` | 8 CPU, p1 running |
| 5 | `uthreads` | shell running; zombies допустимы |
| 6 | несколько команд подряд | нет hang / GPF |
| 7 | htop на хосте | QEMU ~0–15% в idle |

### Инварианты — не ломать

```
□ syscall на syscall_stack
□ in_syscall блокирует lwkt_switch
□ runner_longjmp только из syscall_post_dispatch
□ uthread_yield без lwkt_syscall_resched следом
□ wait_edge: sti;hlt only
□ callee restore в user_enter_asm
□ reload runner/p после setjmp
□ exec runner → другой CPU при in_syscall
```

---

## 11. Ключевые файлы

| Файл | Содержимое |
|------|------------|
| `kernel/sched/uthread.c` | proc_runner, in-proc sched, yield/join/exit |
| `kernel/sched/lwkt.c` | SMP scheduler, idle, wait_edge, work-steal |
| `kernel/syscall/syscall.c` | dispatch, post_dispatch, stash context |
| `kernel/arch/x86_64/isr.asm` | syscall_common, user_enter_asm, runner setjmp/longjmp |
| `kernel/syscall/user.c` | user_enter, page alloc |
| `kernel/proc/proc_mutex.c` | uthread-level mutex, MYOS_ERR_AGAIN |
| `kernel/proc/exec.c` | spawn + IPI после runner |
| `kernel/sched/kbdd.c` | poll keyboard из syscall |
| `kernel/arch/x86_64/smp.c` | cpu_list_lock, cpu_list |
| `kernel/arch/x86_64/interrupt.c` | timer, preempt, exceptions |
| `kernel/mm/vmm.c` | per-proc CR3, aspace destroy |
| `user/myos.h` | MYOS_ERR_AGAIN retry loops |

---

## 12. Главный принцип

> **Два стека, два уровня планирования, один выход из syscall — либо iretq, либо longjmp.**

LWKT решает, **какой процесс владеет CPU**.
Uthread scheduler внутри proc решает, **какой user thread** выполняется на этом CPU.
Syscall — граница между ними; её нельзя пересекать через `lwkt_switch()`.
