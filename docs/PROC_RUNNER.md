# MyOS — proc-runner (1 LWKT на user-процесс)

Модель планирования userland после фазы 7: **один LWKT-runner на процесс**,
**in-proc scheduler** для uthread. Связано: [ROADMAP.md](ROADMAP.md) фаза 7,
[DEVELOPMENT.md](DEVELOPMENT.md) §5–6, [THREADS_DEMO.md](THREADS_DEMO.md).

---

## 1. Модель

### Было (1:1)

```
user uthread  ──1:1──►  LWKT (u1, u2.1)  ──►  int 0x80 на стеке LWKT
```

- Shell и workers — разные LWKT на общей очереди → starvation на SMP.
- `lwkt_block()` / `lwkt_switch()` из syscall ломают сохранённый RSP.
- `myos_thread_create` возвращал `lwkt_id`.

### Стало (proc-runner)

```
┌─────────────────────────────────────────┐
│  proc (CR3, mutexes, run_queue)         │
│    uthread₁, uthread₂, …  (in-proc)     │
│         ▲                               │
│    proc_sched_pick / enqueue            │
│         │                               │
│    LWKT-runner p{N}  (один на proc)     │
└─────────────────────────────────────────┘
```

| Сущность | Роль |
|----------|------|
| `struct proc.runner` | LWKT, крутит `proc_runner_entry` |
| `proc.run_queue` | очередь uthread внутри proc (по `priority`) |
| `proc.current_uthread` | кто сейчас в user mode |
| `uthread.uthread_id` | стабильный TID для `JOIN` / `THREAD_CREATE` |
| kernel LWKT (msgd, kbdd) | по-прежнему 1:1 через `uthread_spawn` |

**Проверка в shell:**

```text
threads    # p1 running (shell), p2 ready (threads.elf) — не u1/u2
uthreads   # user uthread по PID, TID
ps         # Uthreads count
```

---

## 2. Ключевые файлы

| Файл | Содержимое |
|------|------------|
| `include/proc.h` | `runner`, `current_uthread`, `run_queue` |
| `include/lwkt.h` | `user_proc`, `in_syscall`, `runner_resume_rsp` |
| `include/uthread.h` | `uthread_state`, `uthread_id`, `user_rdi`, `user_syscall_ret` |
| `kernel/sched/uthread.c` | `proc_runner_entry`, `uthread_yield`, in-proc sched |
| `kernel/proc/exec.c` | `proc_start_runner` после первого uthread |
| `kernel/proc/proc_mutex.c` | mutex через runner + `MYOS_ERR_AGAIN` |
| `kernel/sched/kbdd.c` | poll клавиатуры из syscall (без msg wait) |
| `kernel/sched/lwkt.c` | idle SMP, `lwkt_block` в syscall, preempt guards |
| `kernel/syscall/syscall.c` | `in_syscall`, `uthread_yield` для `SYS_YIELD` |
| `user/myos.h` | `myos_thread_join` / `myos_mutex_lock` / `myos_msg_ping` — цикл на `MYOS_ERR_AGAIN` |

---

## 3. Потоки выполнения

### Запуск процесса

```
exec_spawn_elf()
  → uthread_spawn_in_proc()     # uthread в run_queue, без lwkt_create
  → proc_start_runner()         # lwkt_create("pN", proc_runner_entry)
```

### Runner loop

```
proc_runner_entry:
  pick = proc_sched_pick_other(p, skip=NULL)
  user_enter(rip, rsp, rdi, rax, &saved_kernel_rsp)
  # возврат через uthread_yield / uthread_exit → return_to_runner
```

### Syscall → переключение uthread

```
uthread_yield():
  user RIP/RSP/RDI уже сохранены на входе в syscall (isr.asm → syscall_dispatch)
  cur → RUNNABLE, proc_sched_enqueue
  next = proc_sched_pick_other(p, cur)
  если next == NULL → lwkt_syscall_wait_edge(); return
  runner->in_syscall = 0
  uthread_return_to_runner()    # ret на saved_kernel_rsp (после user_enter)
```

### Join / mutex из userland

- Kernel: `uthread_join` / `proc_mutex_lock` → `uthread_yield` + `MYOS_ERR_AGAIN`.
- Userland (`myos.h`): цикл `while (ret == MYOS_ERR_AGAIN)`.

---

## 4. Инварианты syscall (критично — не ломать)

### 4.1. `in_syscall` и preempt

`int 0x80` выполняется на **kernel stack LWKT** (TSS.RSP0).  
**Нельзя** вызывать `lwkt_switch()` из syscall — `prev->rsp` станет указателем
внутрь syscall frame → после возврата `rip=0`, GPF `cr2=0`.

| Правило | Где |
|---------|-----|
| `cur->in_syscall = 1` в начале `syscall_dispatch` | `syscall.c` |
| `in_syscall = 0` **после** `lwkt_preempt_check()` | `syscall.c` |
| `lwkt_switch()` — no-op при `in_syscall` | `lwkt.c` |
| `lwkt_preempt_check()` не вызывать из IRQ при `in_syscall` | `interrupt.c` |
| При save RSP: если `in_syscall` или RSP вне стека LWKT — не портить `prev->rsp` | `lwkt_switch` guard |

### 4.2. `lwkt_block()` в syscall

Вместо `lwkt_switch()`:

```c
if (lwkt_in_usersyscall()) {
    lwkt_syscall_wait_edge();  // sti; hlt
    return;
}
```

Иначе `msg_receive` / `token_lock` крутят busy-loop (100% CPU).

### 4.5. Единый retry-путь для блокирующих syscall (7b)

Добавлен helper:

```c
int lwkt_syscall_resched(int64_t retry_ret);
```

- работает только в `lwkt_in_usersyscall()`;
- пишет `current_uthread->user_syscall_ret = retry_ret`;
- делает `lwkt_syscall_wait_edge()` (`sti; hlt`) и возвращает `1`.

Использование:

- `uthread_join` и `proc_mutex_lock` для `MYOS_ERR_AGAIN`;
- `SYS_MSG_RECV`: в syscall runner делается `msg_receive(..., 0)` + retry через `MYOS_ERR_AGAIN`, без block-loop в `msg_receive(..., 1)`.
- `SYS_MSG_PING`: send выполняется один раз (`MYOS_MSG_PING_SEND`), далее только wait на `PONG` через retry.
- После `MYOS_ERR_AGAIN` в IPC-path **не** делаем `lwkt_yield()` в `syscall_post_dispatch` (это нарушает инвариант syscall stack); перенос работы делается через `lwkt_syscall_wait_edge()` + timer preempt + IPC bump.

### 4.3. Клавиатура (`SYS_READ`)

Runner остаётся `RUNNING` в `hlt`-цикле — **kbdd LWKT не получает CPU** через msgport.

**Решение:** в `kbdd_request_char()` при `lwkt_in_usersyscall()`:

1. `kbdd_drain_scancodes()` — IRQ ring → char ring
2. `char_ring_pop()` — отдать символ
3. иначе `lwkt_syscall_wait_edge()`

Msgport-путь (`MSG_KBD_WAIT`) — только для kernel LWKT (не runner).

### 4.4. In-proc yield

При `proc_sched_enqueue` uthread **обязан** стать `UTHREAD_RUNNABLE` (не оставаться `RUNNING`).

`proc_sched_pick_other(p, cur)` — не выбирать текущий uthread.

Если других runnable нет → `lwkt_syscall_wait_edge()`, не busy-loop.

Иначе `myos_yield()` / mutex / join дают **100% CPU** на одном proc.

---

## 5. SMP idle (критично на `SMP=8`)

### Симптом

`htop`: 8 потоков QEMU по ~90% CPU при простое на `MyOS>`.

### Причины (исправлены)

1. **Глобальный idle:** `has_runnable_threads()` по всем CPU → AP idle делал `yield` без `hlt`.
2. **Очередь без проверки state:** `cpu_has_runnable` считал любой узел в очереди, не только `THREAD_READY`.
3. **preempt_check:** считал `BLOCKED`/`TERMINATED` как «есть работа».

### Правильное поведение idle

```c
for (;;) {
    lwkt_preempt_check();
    if (cpu_has_ready_excluding_idle(this_cpu)) {
        lwkt_yield();
        continue;
    }
    cpu->preempt_requested = 0;
    sti; hlt;
}
```

- AP **не** обязан крутиться, если работа только на BSP (shell runner в `hlt`).
- Ожидаемая нагрузка QEMU в простое: **~0–10%** (один vCPU + timer), не 8×90%.

`lwkt_preempt_check`: искать только `t->state == THREAD_READY`; на idle без работы — сбросить `preempt_requested`.

---

## 6. Приоритеты

| LWKT | Приоритет | Кто |
|------|-----------|-----|
| shell runner `p1` | `LWKT_PRIO_HIGH` (2) | exec shell |
| child runner `pN` | `LWKT_PRIO_NORMAL` (8) | exec module |
| uthread in-proc | поле `uthread.priority` | `THREAD_CREATE` arg |

In-proc pick: **меньше число = выше приоритет** (как LWKT).

### 6.1. Жёсткое квантование LWKT

- `LWKT_QUANTUM_TICKS` (по умолчанию 3 тика LAPIC/PIT, ~30 ms при 100 Hz).
- По исчерпанию кванта поток получает `quantum_force`, и `dequeue_thread()` не выбирает его fallback-веткой.
- Это ломает starvation, когда `p1` (prio 2) бесконечно вытесняет `msgd` (prio 8) на SMP.

---

## 7. Чеклист регрессии proc-runner

После изменений в `uthread.c`, `lwkt.c`, `syscall.c`, `kbdd.c`, `proc_mutex.c`:

- [ ] `make run SMP=8` — на `MyOS>` **htop ~0–10%**, не сотни процентов
- [ ] Ввод с клавиатуры: `help`, `ps`, `threads`, `uthreads`
- [ ] `exec threads.elf` — workers, `join: … counter=10`, `main done`, `Process 2 exited`
- [ ] После exit: shell отвечает, `ps` — один процесс
- [ ] `threads` — `p1` runner, без лишних `uN` на user uthread
- [ ] `exec threads.elf` на **8 CPU** без GPF / зависания
- [ ] `capdiag stress 1000` — `fail=0`, shell остаётся интерактивным
- [ ] `capdiag grantstress 1000` — `fail=0`, `uthreads/cpus` после теста стабильны

---

## 8. Известные ограничения

- **Msgport из syscall**: для блокирующего recv обязателен retry-протокол (`MYOS_ERR_AGAIN`) в userland-обёртках; прямой single-shot `SYS_MSG_RECV(block=1)` в приложении без retry может зависнуть по ожиданиям API.
- **Preempt runner в user mode** без сохранения uthread state — отключён в `lwkt_preempt_check`.
- **Максимум:** `MAX_THREADS` LWKT, `MAX_PROCS` proc, `STACK_SIZE` 8 KiB на LWKT.

### 8.1. Capability IPC диагностика (MVP)

- Добавлены syscall: `CAP_CREATE_PORT`, `CAP_SEND`, `CAP_RECV`, `CAP_GRANT`, `CAP_CLOSE`, `GETPID`.
- Shell-диагностика:
  - `capdiag` — одиночный проход с проверкой data-path и negative-path.
  - `capdiag stress N` — циклический self-cap тест (create/send/recv/close).
  - `capdiag grantstress N` — циклический grant-path (create/grant/send/recv/close).
- Критичный инвариант: каждый раунд обязан закрывать созданные cap-слоты (`capclose`), иначе после `MYOS_CAP_MAX` раундов будет `create rc=-1` (исчерпание таблицы), что не является SMP race.

---

## 9. Ссылки

- [ARCHITECTURE_AUDIT.md](ARCHITECTURE_AUDIT.md) — полный аудит, история багов, инварианты
- DragonFly (референс, не копировать): `sys/kern/usched_dfly.c`, token sleep
- Локально: `/home/redbeard/dsk_250/DragonFlyBSD` (если доступен)
