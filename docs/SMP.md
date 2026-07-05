# MyOS — SMP (Symmetric Multiprocessing)

Этап 2: запуск Application Processors (AP) через Limine, per-CPU состояние,
глобальная очередь LWKT под spinlock, LAPIC timer для вытеснения.

---

## 1. Архитектура

```
                    ┌─────────────────────────────────┐
                    │  Global LWKT run_queues[]       │
                    │  + spinlock sched_lock          │
                    └───────────────┬─────────────────┘
                                    │
         ┌──────────────────────────┼──────────────────────────┐
         ▼                          ▼                          ▼
   ┌───────────┐            ┌───────────┐            ┌───────────┐
   │ CPU 0 BSP │            │ CPU 1 AP  │            │ CPU N AP  │
   │ GS→cpu[0] │            │ GS→cpu[1] │            │ GS→cpu[N] │
   │ idle/cur  │            │ idle/cur  │            │ idle/cur  │
   │ TSS[0]    │            │ TSS[1]    │            │ TSS[N]    │
   │ LAPIC ti  │            │ LAPIC ti  │            │ LAPIC ti  │
   └───────────┘            └───────────┘            └───────────┘
```

- **Один пул** `thread_pool[]` и **одна** глобальная очередь приоритетов.
- **Per-CPU:** `current`, `idle`, `preempt_requested`, `sched_active`, свой TSS.
- **Миграция потоков** между CPU возможна (поток снимается с очереди на CPU A,
  может выполняться на CPU B) — явного pin пока нет.

---

## 2. Загрузка

1. Limine передаёт `limine_smp_response` (запрос в `kernel/main.c`).
2. BSP: `cpu_init_bsp()` — LAPIC, `struct cpu`, MSR `KERNEL_GS_BASE`.
3. `smp_init()` — для каждого AP: атомарная запись `goto_address = smp_lapic_entry` (Limine будит AP; **не вызывать** эту функцию с BSP).
4. AP: `cpu_init_ap()`, барьер, `interrupts_enable()`, `lwkt_cpu_init_idle()`, LAPIC timer, `lwkt_ap_bootstrap()`.
5. BSP: `smp_release_aps()` — PIT off, LAPIC timer на BSP, `lwkt_sched_enable()`.
6. BSP: `lwkt_bootstrap_first()` — shell как `RUNNING` на BSP, затем `smp_start_aps()`.
7. AP: LAPIC timer, `lwkt_sched_enable()`, `lwkt_ap_bootstrap()`.

При загрузке в консоли:

```
SMP: booting 1 AP(s), BSP lapic=...
SMP: all CPUs online (2 total)
```

---

## 3. Таймер и вытеснение

| Режим | Таймер |
|-------|--------|
| До `smp_release_aps` (ранний init) | PIT ~100 Hz (только если 1 CPU в `lwkt_sched_start`) |
| После SMP release | LAPIC timer vector **64** на каждом CPU |
| Wake / unblock | IPI vector **65** → `lwkt_preempt_check()` на удалённых CPU |

Обработчик таймера: `interrupt.c` → `lwkt_preempt_request()` → `lwkt_preempt_check()`.

IPI reschedule: `lapic_ipi_reschedule_others()` из `lwkt_unblock()` / `lwkt_create()`.

---

## 4. Per-CPU доступ

```c
struct cpu *cpu_current(void);  /* mov gs:0 — поле self в struct cpu */
```

Каждый CPU при инициализации:

```c
cpu->self = cpu;
wrmsr(KERNEL_GS_BASE, cpu);
gdt_load_tss(cpu->id);
```

---

## 5. Блокировки (SMP-safe)

| Ресурс | Механизм |
|--------|----------|
| Run queue / LWKT pool | `sched_lock` (spinlock) |
| `proc_table` | `proc_table_lock` |
| Page allocator | `page_lock` |
| Console | `console_lock` |
| Proc mutex | `proc_mutex.guard` + wait queue |

---

## 6. Проверка SMP

```bash
make run    # QEMU: -smp 2
```

В shell:

```text
cpus              # оба CPU: sched on, idle/shell, счётчик switches
exec threads.elf  # многопоточный тест
cpus              # switches растут на CPU 0 и CPU 1
threads           # колонка CPU у running-потоков
```

Без `-smp` Limine может отдать 1 CPU — ядро пишет `SMP: single CPU (no APs)`.

---

## 7. Ограничения

- Нет per-CPU run queue и балансировки (одна глобальная очередь).
- `msgport` всё ещё использует `cli`/`sti` — нужна доработка под SMP.
- IPI broadcast «всем кроме себя» — простой wake; без точечной доставки на один CPU.

---

## 8. Ключевые файлы

| Файл | Назначение |
|------|------------|
| `include/cpu.h` | `struct cpu`, SMP API |
| `kernel/arch/x86_64/smp.c` | Limine AP boot, барьер |
| `kernel/arch/x86_64/lapic.c` | LAPIC MMIO, timer |
| `kernel/arch/x86_64/gdt.c` | TSS per CPU |
| `kernel/sched/lwkt.c` | Планировщик + `sched_lock` |
| `kernel/main.c` | `limine_smp_request` |

---

## 9. Следующие шаги

- Spinlock в `msgport.c` вместо `cli`
- Per-CPU run queue (опционально)
- Точечный IPI на конкретный lapic_id

См. [DEVELOPMENT.md](DEVELOPMENT.md), [THREADS_DEMO.md](THREADS_DEMO.md).
