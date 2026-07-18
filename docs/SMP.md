# MyOS — SMP (Symmetric Multiprocessing)

Этап 2: запуск Application Processors (AP) через Limine, per-CPU состояние,
per-CPU run queues и `queue_lock`, `thread_pool_lock`, LAPIC timer и targeted IPI.

---

## 1. Архитектура

```
              ┌──────────────────────────────────────┐
              │  thread_pool_lock (create/destroy)   │
              └──────────────────────────────────────┘
      ┌──────────────────────────┬──────────────────────────┐
      ▼                          ▼                          ▼
┌───────────┐            ┌───────────┐            ┌───────────┐
│ CPU 0 BSP │            │ CPU 1 AP  │            │ CPU N AP  │
│ queue_lock│            │ queue_lock│            │ queue_lock│
│ run_queues│            │ run_queues│            │ run_queues│
│ idle/cur  │            │ idle/cur  │            │ idle/cur  │
│ work steal│◄──────────►│ work steal│            │           │
└───────────┘            └───────────┘            └───────────┘
```

- **Per-CPU** `run_queues[MAX_PRIORITY]` и `queue_lock` в `struct cpu`.
- `thread_pool_lock` — только alloc/free слотов LWKT (никогда после `queue_lock`).
- Steal: `queue_lock_two` по возрастанию `cpu->id`; idle не ставится в очередь; yielder `queue_pinned` до локального dequeue; claim `RUNNING` до unlock (гонка с `lwkt_nudge`).
- С busy CPU крадём только при ≥2 stealable waiters; с idle — при любом backlog.
- Wake: enqueue под lock dest, затем **вне** lock — `lwkt_sched_ipi_cpu(dest)`.

Дорожная карта: [ROADMAP.md](ROADMAP.md).

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

## 3. Targeted IPI и метрики

| API | Назначение |
|-----|------------|
| `lwkt_sched_ipi_cpu(dest)` | resched на один CPU (local → `preempt_requested`) |
| `lwkt_sched_ipi_thread(t)` | dest = owner / `run_cpu` |
| `lwkt_sched_ipi_others()` | broadcast (kill wait, join record, fallback) |

Счётчики (см. `cpus` / `smpbalance` / `threads`):

- per-CPU: `steals`, `same_proc_pulls`, `ipi_rx`
- глобально: `ipi_targeted`, `ipi_local`, `ipi_broadcast`

Ожидание на горячем пути: **targeted ≫ broadcast**. Broadcast остаётся только как явный API `lwkt_sched_ipi_others` (сейчас без call sites на kill/join/exec/nudge).

---

## 4. Проверка (SMP=8)

```bash
make && qemu-system-x86_64 -M q35 -m 256M -smp 8 -cdrom build/myos.iso -boot d -serial stdio -display none
```

```text
ping
exec threads.elf
# worker start/done, join counter=10, main done, Process exited
smpbalance
cpus
```

Автотест: `expect tools/qemu_kse_test.exp`  
Регрессия: ≥20× `exec threads.elf` подряд на `-smp 8` без hang.

---

## 5. Следующий шаг

UART IRQ вместо serial poll в `wait_edge` (опционально; GUI-отладка достаточна). Дальнейший тюнинг steal/fairness.
