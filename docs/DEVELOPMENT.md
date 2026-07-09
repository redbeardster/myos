# MyOS — руководство для разработки

Документ описывает архитектуру, инварианты и практики, которых нужно придерживаться при
расширении ядра, syscall API и userland. Цель — не повторять класс ошибок вроде GPF после
повторного `exec`, когда команд и бинарников станет больше.

---

## 1. Обзор архитектуры

```
┌─────────────────────────────────────────────────────────────┐
│  Userland (ring 3): shell.elf, hello.elf, …                 │
│  Единственный вход в ядро: int 0x80 (см. myos_abi.h)        │
└───────────────────────────┬─────────────────────────────────┘
                            │ syscall trap
┌───────────────────────────▼─────────────────────────────────┐
│  syscall.c — dispatch, copy_from_user, блокировки             │
└───────────────────────────┬─────────────────────────────────┘
                            │
     ┌──────────────────────┼──────────────────────┐
     ▼                      ▼                      ▼
  exec/elf              proc/vmm/user           keyboard
     │                      │                      │
     └──────────────────────┼──────────────────────┘
                            ▼
┌─────────────────────────────────────────────────────────────┐
│  LWKT (lwkt.c) — планировщик, context switch                │
│  uthread.c   — proc-runner, in-proc sched, user entry       │
│  msgport.c   — сообщения только между потоками ядра         │
└─────────────────────────────────────────────────────────────┘
```

**Загрузка:** Limine → `kernel/main.c` → `exec_start_shell()` → `lwkt_bootstrap_first()`.

**Proc-runner (фаза 7):** один LWKT-runner на user-процесс, uthread планируются внутри proc.
Подробно: [PROC_RUNNER.md](PROC_RUNNER.md).

### Структура каталогов

```
os/
├── Makefile              # сборка → build/myos.iso
├── limine.conf
├── include/              # заголовки ядра и ABI (myos_abi.h, limine.h, …)
├── kernel/
│   ├── main.c            # kmain
│   ├── arch/x86_64/      # GDT, IDT, ISR, linker.ld
│   ├── drivers/          # framebuffer console, keyboard
│   ├── mm/               # physical memory, VMM
│   ├── sched/            # LWKT, uthread, msgport
│   ├── proc/             # proc, exec, ELF loader
│   └── syscall/          # syscall dispatch, user helpers
├── user/                 # shell.elf, hello.elf, crt0, programs.mk
├── docs/                 # DEVELOPMENT.md, USERLAND.md
├── tools/                # вспомогательные скрипты (QEMU monitor)
└── build/                # артефакты сборки (в .gitignore)
```

**Модель процессов:** у каждого `proc` свой CR3; виртуальная карта одинакова у всех
(`0x400000` text, `0x500000+` heap, `0x700000` stack), физические страницы изолированы.

---

## 2. Слои и границы ответственности

### Userland → ядро

- Только **системные вызовы** (`int 0x80`), номера и layout — в `include/myos_abi.h` / `user/myos.h`.
- Userland **не** обращается к msgport, LWKT и внутренним структурам ядра напрямую.
- Все указатели из userland копируются через `copy_from_user` / `copy_user_string` в `syscall.c`.

### Ядро → ядро (LWKT)

- `lwkt_yield`, `lwkt_block`, `lwkt_unblock` — внутри ядра.
- `msgport` (`SYS_MSG_SEND` / `SYS_MSG_RECV`) — упрощённая очередь между LWKT-потоками;
  syscall лишь **обёртка**, сам msgport не виден из ring 3.

### Ориентир: DragonFlyBSD

| DragonFly | MyOS |
|-----------|------|
| `syscall` → `syscall2()` → `sysent[]` | `int 0x80` → `syscall_dispatch()` |
| `lwkt_sendmsg` между kernel threads | `msgport` между LWKT |
| Userland всегда через trap | То же |

Сообщение ядру **без** trap невозможно. Message passing не заменяет syscall.

---

## 3. Контракт переключения контекста (критично)

При schedule на поток, который может выполнять syscalls из ring 3, обновляется **всё**:

| Состояние | Где хранится | Когда обновлять |
|-----------|--------------|-----------------|
| Page tables | `CR3` (CPU) | `lwkt_apply_cr3()` в `lwkt_switch()` |
| Syscall/interrupt stack | `TSS.RSP0` | `lwkt_apply_tss()` в `lwkt_switch()` |
| Cooperative stack | `lwkt_thread->rsp` | `switch_context()` |
| Текущий поток | `current_thread` | `lwkt_switch()` |

**Единая точка:** `lwkt_switch()` (и `lwkt_bootstrap_first()` для первого потока).

### Два разных «стека ядра»

```
Cooperative (lwkt_thread->rsp)
  • switch_context, thread_bootstrap
  • lwkt_block / lwkt_yield с syscall stack, если поток заблокирован в syscall

Syscall / interrupt (TSS.RSP0)
  • int 0x80 из ring 3
  • IRQ, пока выполняется код обработчика на этом стеке
```

`user_enter()` выставляет TSS при входе в ring 3, но **после возврата другого процесса**
планировщик обязан восстановить TSS снова — иначе следующий user-поток будет использовать
чужой kernel stack.

### Реальный инцидент (GPF на втором exec)

1. Shell установил `TSS.RSP0` на свой стек.
2. Hello при `user_enter()` переключил TSS на свой стек.
3. Hello завершился; CR3 вернулся к shell, но **TSS остался на стеке hello**.
4. Shell делал syscalls (`ps`, `threads`, второй `exec`) на мёртвом стеке hello.
5. Глубокий `elf_load` переполнил стек → порча `struct lwkt_thread` → GPF #13.

**Правило:** при любом новом пути, который меняет текущий user-поток без `lwkt_switch()`,
нужно вручную вызвать и CR3, и TSS — лучше не добавлять такие пути, а идти через планировщик.

---

## 4. Память и адресные пространства

### Создание процесса

1. `vmm_aspace_create()` — новый PML4, верхняя половина скопирована с kernel.
2. `elf_load()` / `vmm_map()` — только в **указанный** `cr3`, не в активный (если это другой процесс).
3. `invlpg` (`vmm_flush`) — только если `vmm_cr3_active(cr3)` (см. `vmm.c`).

### Уничтожение процесса

- `proc_destroy()` → `proc_destroy_aspace()`.
- Если уничтожаемый CR3 **сейчас в CPU** — отложить в `pending_cr3_destroy` текущего LWKT-потока.
- Фактическое `vmm_aspace_destroy()` — в `lwkt_switch()` **после** `lwkt_apply_cr3(next)`.

Нельзя освобождать PML4, пока регистр CR3 на него указывает.

### User heap

- `SYS_ALLOC` / `SYS_FREE` → `user_page_alloc()` / `user_free_page()` в `user.c`.
- Рост кучи: `proc->heap_next` от `MYOS_USER_HEAP_START`.

---

## 5. Жизненный цикл процесса и потока

```
exec_spawn_elf()
  → proc_create(cr3, …)          # sched_mode = PROC_SCHED_KSE (default)
  → uthread_spawn_in_proc()      # main uthread + lwkt_create_user(user_kse_entry)
  → [legacy] proc_start_runner() # только при PROC_SCHED_RUNNER (schedmode 0)

user: SYS_EXIT
  → uthread_exit()
  → uthread_cleanup() → proc_on_uthread_exit() → proc_destroy (при последнем uthread)
  → KSE LWKT: lwkt_thread_exit() → lwkt_yield()
```

Инварианты:

- `SYS_EXIT` не возвращается в syscall stub (`__builtin_unreachable`).
- Syscall выполняется на **kernel stack KSE LWKT** (или runner LWKT в legacy mode); из syscall **нельзя** `lwkt_switch()` —
  см. `in_syscall`, `lwkt_syscall_wait_edge()` в [PROC_RUNNER.md](PROC_RUNNER.md) §4.
- `SYS_READ` в user proc: poll клавиатуры + `lwkt_syscall_wait_edge()`, не msgport block.
- После exit LWKT-слот: `id = 0`, `msgport_clear_slot()` в `lwkt_thread_exit()`.
- Shell (`EXEC_FLAG_SHELL`, `proc->is_shell`) не убивается через `proc_kill_children()`.

### Многопоточность userland (uthread)

- Один proc — несколько uthread; у каждого свой user stack (`user_stack_alloc`).
- `SYS_THREAD_CREATE` / `JOIN` / mutex — см. [THREADS_DEMO.md](THREADS_DEMO.md).
- **Default (KSE):** каждый uthread — отдельный kernel LWKT (`user_kse_entry`); SMP scheduler выбирает uthread напрямую.
- **Legacy (`schedmode 0`):** in-proc scheduler (`proc_sched_pick`, `run_queue`); один runner LWKT на proc.
- `uthread_yield` из syscall переключает KSE LWKT (или uthread внутри proc в runner mode); при отсутствии peer — `hlt`.

### Вытесняющая многозадачность (LWKT)

| Источник | Файл | Поведение |
|----------|------|-----------|
| PIT IRQ ~100 Hz | `interrupt.c` | `lwkt_preempt_request()` → `lwkt_preempt_check()` после EOI |
| Выход из syscall | `syscall.c` | `lwkt_preempt_check()` в конце `syscall_dispatch` |
| `SYS_YIELD` | `lwkt.c` | немедленный `lwkt_switch()` (кооперативно) |
| `lwkt_block` | `lwkt.c` | блокировка kernel LWKT; в syscall runner → `lwkt_syscall_wait_edge()` |
| `uthread_yield` | `uthread.c` | переключение uthread внутри proc из syscall |

Ограничения:

- Preempt только в точках вызова `lwkt_preempt_check` (не произвольная инструкция внутри длинного syscall handler).
- Preempt **не** из IRQ во время syscall runner (`in_syscall`).
- 16 уровней приоритета LWKT; in-proc uthread — поле `priority`.
- SMP: per-CPU run queue, work steal, idle `hlt` при пустой **локальной** очереди — см. [PROC_RUNNER.md](PROC_RUNNER.md) §5.

---

## 6. Правила для syscall-обработчиков

| Syscall | Поведение |
|---------|-----------|
| `SYS_WRITE`, `SYS_PS`, `SYS_THREADS`, `SYS_ALLOC`, `SYS_FREE`, `SYS_EXEC` | Обработать и вернуться через `iretq` |
| `SYS_READ` | user proc: `kbdd_request_char` (poll + `lwkt_syscall_wait_edge`) |
| `SYS_YIELD` | user proc: `uthread_yield()`; kernel LWKT: `lwkt_yield()` |
| `SYS_EXIT` | `uthread_exit()` — без return |
| `SYS_THREAD_*`, `SYS_MUTEX_*` | in-proc yield + `MYOS_ERR_AGAIN`; userland цикл в `myos.h` |
| `SYS_MSG_*` | kernel LWKT: `lwkt_block()`; runner в syscall — см. [PROC_RUNNER.md](PROC_RUNNER.md) §8 |

**Запрещено:**

- `lwkt_switch()` / `lwkt_block()` → switch из syscall runner (`in_syscall`).
- `syscall_return` / произвольные переходы между user stack и kernel stack.
- Двойной вызов post-syscall логики после `SYS_EXIT` или `SYS_READ`.
- Прямой `vmm_switch()` в syscall без учёта того, какой поток станет текущим.
- Оставлять uthread в `RUNNING` после enqueue (ломает in-proc sched → 100% CPU).

---

## 6.1. Kill strict semantics (summary)

Целевой контракт:

- `kill <pid>` успешен только если PID фактически исчез из `proc_table`.
- `killall <name>` возвращает число реально уничтоженных процессов.

Краткая цепочка:

```text
SYS_KILL / SYS_KILLALL_NAME
  -> proc_kill / proc_kill_name
  -> proc_kill_request
       -> proc_destroy(remote) -> runner->pending_kill=1 + IPI/unblock
  -> proc_kill_wait_dead (bounded wait until PID absent)
```

Критичные инварианты:

- Нельзя разрушать `cpu->current` чужого CPU напрямую.
- Для single-uthread процессов (`spin.elf`) при `pending_kill` нужен форсированный возврат в runner loop (`uthread_yield` -> `uthread_return_to_runner`), иначе kill может "висеть" в `hlt`-цикле.
- В `syscall_post_dispatch` нельзя преждевременно очищать `pending_kill` до перехода в runner.

Детальная схема с кодом и моделью памяти: [PROC_RUNNER.md](PROC_RUNNER.md) §8.2.

---

## 6.2. Scheduling mode (KSE default)

По умолчанию все user uthread — kernel-schedulable (KSE):

- `PROC_SCHED_KSE` (default): каждый uthread — отдельный LWKT, планируется SMP scheduler.
- `PROC_SCHED_RUNNER` (legacy/debug): один runner LWKT на proc, in-proc `run_queue`.

Syscall:

- `SYS_PROC_SET_SCHED_MODE`, `SYS_PROC_GET_SCHED_MODE`
- `SYS_THREAD_CREATE_EX` (совместимость; флаги игнорируются в KSE mode)

Userland:

- `myos_proc_set_sched_mode(0)` — legacy runner
- shell: `schedmode [0|1]`
- `ksebench`, `ksebench compare` — stress/benchmark для KSE

Детали: [PROC_RUNNER.md](PROC_RUNNER.md) §10.

---

## 7. Добавление нового syscall

1. Константа в `include/myos_abi.h` и обёртка в `user/myos.h`.
2. Ветка в `syscall_dispatch()` в `kernel/syscall/syscall.c`.
3. Копирование аргументов из userland в kernel buffer (`copy_from_user`).
4. Выбор модели завершения: быстрый return / block / exit (таблица выше).
5. Если syscall глубокий (много локальных переменных, циклы, I/O) — учитывать размер
   **syscall stack** (`STACK_SIZE` = 4 KiB на LWKT-поток, стек в `TSS.RSP0`).

---

## 8. Добавление нового userland-бинарника

**Полное руководство:** [USERLAND.md](USERLAND.md)

Кратко:

1. `cp user/template.c user/foo.c`, отредактировать.
2. Добавить `foo` в `PROGRAMS` в `user/programs.mk`.
3. `make && make run` → в shell: `exec foo.elf`.

Регистрация в ядре (`exec`, embed, ISO, `limine.conf`) выполняется **автоматически**
через `tools/gen_user_embeds.sh` и `tools/gen_limine_conf.sh`.

Проверки после добавления — см. раздел 10 и чеклист в USERLAND.md.

---

## 9. Ключевые файлы

| Файл | Назначение |
|------|------------|
| `include/myos_abi.h` | VA layout, номера syscall |
| `kernel/arch/x86_64/isr.asm` | `int 0x80`, `user_enter_asm`, `switch_context` |
| `kernel/syscall/syscall.c` | Dispatch и copy_from_user |
| `kernel/sched/lwkt.c` | Планировщик, CR3, TSS, deferred CR3 destroy, **preempt** |
| `kernel/sched/uthread.c` | proc-runner, in-proc sched, trampoline |
| `kernel/proc/proc.c` | Таблица процессов, destroy, kill |
| `kernel/mm/vmm.c` | Aspace, map/unmap, active CR3 checks |
| `kernel/proc/exec.c` / `kernel/proc/elf.c` | Загрузка ELF в aspace процесса |
| `kernel/syscall/user.c` | `user_enter`, heap в aspace текущего proc |
| `kernel/arch/x86_64/gdt.c` | GDT, TSS, `tss_set_rsp0()` |
| `kernel/main.c` | Точка входа `kmain` |

---

## 10. Чеклист регрессии

Перед merge / после нетривиальных изменений в schedule, syscall, vmm, exec:

- [ ] `exec hello.elf` **дважды подряд** — оба раза вывод и `Process N exited`.
- [ ] Между двумя `exec`: `ps`, `threads` — без GPF/page fault.
- [ ] После exit дочернего: `ps` показывает только shell; `threads` — shell + idle.
- [ ] Shell остаётся с тем же CR3 в `ps` до и после дочернего процесса.
- [ ] `read` / prompt: ввод команды после exit дочернего работает.
- [ ] (При изменении msgport) слоты mbox очищаются в `lwkt_thread_exit` / `lwkt_destroy`.
- [ ] `make run SMP=8` — в простое на `MyOS>` нагрузка QEMU ~0–10% (не 8×90%).
- [ ] `exec threads.elf` — counter=10, shell жив; см. [PROC_RUNNER.md](PROC_RUNNER.md) §7.

Команды в shell:

```
exec hello.elf
ps
threads
exec hello.elf
ps
threads
```

---

## 11. Debug-идеи (опционально)

В debug-сборке в начале `syscall_dispatch()`:

```c
struct lwkt_thread *t = lwkt_curthread();
if (t && t->uthread && t->uthread->type == UTHREAD_USER) {
    uint64_t top = ((uint64_t)(uintptr_t)t->stack + STACK_SIZE) & ~0xFULL;
    /* assert read_tss_rsp0() == top; */
    /* assert vmm_cr3_active(t->user_cr3); */
}
```

Нарушение инварианта всплывёт на первом syscall после switch, а не на «тяжёлом» `exec`.

---

## 12. Планируемые направления

При добавлении `fork`, `wait`, файловой системы, сети:

- Сохранять разделение: user trap → syscall → (опционально) lwkt message внутри ядра.
- Новые блокирующие syscall — по образцу `SYS_READ` / proc-runner ([PROC_RUNNER.md](PROC_RUNNER.md) §4).
- Любое копирование из user VA — с активным CR3 **того процесса**, который вызвал syscall
  (`proc_current()->cr3` уже выбран через `lwkt_apply_cr3`).
- Для SMP: см. [SMP.md](SMP.md) — AP boot, per-CPU TSS/GS, LAPIC timer, `sched_lock`.
- Mutex proc: `proc_mutex.c`, до `MYOS_PROC_MUTEX_MAX` слотов, wait queue через `lwkt_thread.wait_next`.

---

*Последнее обновление: proc-runner (фаза 7), SMP idle fix, syscall invariants — см. [PROC_RUNNER.md](PROC_RUNNER.md).*
