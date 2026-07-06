# MyOS — многопоточность и демо `threads.elf`

Подробный разбор userland-программы `user/threads.c`, связанных syscall'ов и механизмов ядра.

См. также:

- [USERLAND.md](USERLAND.md) — общий гайд по userland и API
- [DEVELOPMENT.md](DEVELOPMENT.md) — архитектура ядра, LWKT, инварианты CR3/TSS
- [PROC_RUNNER.md](PROC_RUNNER.md) — **proc-runner**: 1 LWKT на proc, in-proc sched, фиксы syscall/SMP idle

---

## 1. Краткий ответ: есть ли вытеснение uthread?

**Да.** User-потоки выполняются на **одном LWKT-runner** на процесс (`pN`). Вытеснение между **процессами** и kernel LWKT — на уровне **LWKT**; между **uthread одного proc** — **in-proc scheduler** (`uthread_yield`, join, mutex).

| Механизм | Где | Что делает |
|----------|-----|------------|
| **Вытесняющий** | PIT ~100 Hz (`timer_interrupt_handler`) | `lwkt_preempt_request()` → `lwkt_preempt_check()` (не из IRQ при syscall runner) |
| **Вытесняющий** | Возврат из syscall | `lwkt_preempt_check()` после `in_syscall=0` |
| **Кооперативный** | `SYS_YIELD` / `myos_yield()` | `uthread_yield()` — переключение uthread внутри proc |
| **Блокировка** | `SYS_READ`, join, mutex | `lwkt_syscall_wait_edge()` или in-proc yield + `MYOS_ERR_AGAIN` |

Preempt **не** прерывает runner посреди syscall (инвариант `in_syscall` — см. [PROC_RUNNER.md](PROC_RUNNER.md) §4).

### Ограничения текущей модели

- In-proc квантования нет — fair switch через yield, mutex, join.
- Preempt runner только между syscall / в user mode (с ограничениями §4 PROC_RUNNER).
- SMP: per-CPU LWKT queues; idle AP в `hlt` при пустой локальной очереди.
- `myos_yield()` переключает uthread внутри proc, не отдаёт CPU другому процессу (для этого нужен блокирующий syscall или preempt LWKT).

**Практический вывод для `threads.elf`:** mutex на `counter` нужен именно потому, что другой uthread может быть вытеснен **после** инкремента, но **до** unlock — или наоборот. Без `MYOS_MUTEX_DATA` возможны гонки.

---

## 2. Три слоя исполнения (proc-runner)

```
┌─────────────────────────────────────────────────────────┐
│  proc (PID, CR3, run_queue, mutex[8], uthread list)    │
│    ├─ uthread main                                      │
│    ├─ uthread worker1    ── in-proc sched ──┐           │
│    └─ uthread worker2                       │           │
│              ▲                              │           │
│         proc_sched_pick                     │           │
│              │                              │           │
│         LWKT runner p{N}  (один на proc) ◄──┘           │
└─────────────────────────────────────────────────────────┘
```

| Слой | Структура | Роль |
|------|-----------|------|
| **proc** | `struct proc` | Контейнер: CR3, куча, mutex'ы, `run_queue`, `runner` |
| **uthread** | `struct uthread` | Логический поток userland; `uthread_id` = TID |
| **LWKT runner** | `struct lwkt_thread` | Один на proc: kernel stack, `proc_runner_entry` |

Команды shell:

| Команда | Показывает |
|---------|------------|
| `ps` | proc (PID, CR3, число uthread) |
| `uthreads` | uthread (TID, PID, #InProc, state) |
| `threads` | LWKT (`p1`, `p2`, … runners + kernel threads) |

---

## 3. Карта памяти `threads.elf` (PID 2)

```
0x0040_0000  .text / .rodata / .data / .bss  — код ELF (общий для всех uthread)
0x0050_0000  counter (страница кучи, myos_alloc_page)
0x006F_F000  стек main      (первая страница под STACK_TOP)
0x006E_F000  стек worker 1  (вторая страница, при spawn)
0x006D_F000  стек worker 2  (третья страница)
```

Все uthread разделяют **один CR3** → одинаковые VA для кучи и глобалов. Стеки **разные** (отдельная страница на uthread, растут вниз).

---

## 4. Разбор `user/threads.c`

### 4.1. Заголовки и счётчик

```c
#include "myos_thread.h"   /* spawn, join, MYOS_MUTEX_* */
#include "myos_util.h"       /* myos_write_str, myos_write_dec */

static volatile unsigned long *counter;
```

`counter` — указатель на страницу кучи. `volatile` не заменяет mutex, но мешает компилятору оптимизировать обращения к памяти, которую меняют другие uthread.

### 4.2. Mutex CONSOLE (id = 1)

```c
static void log_worker_line(unsigned long id, const char *msg) {
    myos_mutex_lock(MYOS_MUTEX_CONSOLE);
    /* ... вывод одной строки ... */
    myos_mutex_unlock(MYOS_MUTEX_CONSOLE);
}
```

Без этого mutex'а два worker'а при вытеснении могли бы чередовать символы в `SYS_WRITE` → «мусор» в консоли. Mutex сериализует **целые строки**.

### 4.3. Worker

```c
void worker(uint64_t id) {   /* arg приходит в rdi (SysV ABI) */
    log_worker_line(id, "start");

    for (int i = 0; i < 5; i++) {
        myos_mutex_lock(MYOS_MUTEX_DATA);
        (*counter)++;
        myos_mutex_unlock(MYOS_MUTEX_DATA);
        myos_yield();        /* кооперативная уступка; таймер тоже вытеснит */
    }

    log_worker_line(id, "done");
    myos_exit((int)id);      /* ОБЯЗАТЕЛЬНО: return из worker = crash */
}
```

- **`myos_exit` в worker** — uthread завершается; proc живёт, пока не вышел последний uthread.
- **`myos_yield`** — ускоряет чередование; без него переключение всё равно произойдёт по таймеру (~10 ms).
- **5 итераций × 2 worker = 10** → ожидаемый `counter=10` в выводе `join`.

### 4.4. Main

```c
counter = (volatile unsigned long *)myos_alloc_page();
long t1 = myos_thread_spawn(worker, 1);                    /* prio NORMAL (8) */
long t2 = myos_thread_spawn_prio(worker, 2, MYOS_THREAD_PRIO_HIGH); /* prio 2 */
long c1 = myos_thread_join(t1);
long c2 = myos_thread_join(t2);
```

| Вызов | Возвращает | Ядро |
|-------|------------|------|
| `myos_thread_spawn(fn, arg)` | LWKT id (>0) или <0 | `SYS_THREAD_CREATE`, prio NORMAL |
| `myos_thread_spawn_prio(fn, arg, prio)` | то же | prio из аргумента (0..15) |
| `myos_thread_join(lwkt_id)` | exit code worker'а | `SYS_THREAD_JOIN` |

**Почему на экране `worker 2 start` раньше `worker 1 start`:** у t2 приоритет `HIGH` (2), у t1 — `NORMAL` (8). Меньшее число = выше приоритет в LWKT.

`join` блокирует main-uthread до exit worker'а. Exit code = аргумент `myos_exit` → `join: t1=1 t2=2`.

`return 0` из `main` → `crt0` → `SYS_EXIT` для main-uthread. К этому моменту worker'ы уже завершены.

---

## 5. Цепочки syscall → ядро

### 5.1. `SYS_THREAD_CREATE` (14)

```
myos_thread_create(entry, arg, prio)
  → int 0x80
  → syscall_dispatch
  → uthread_create_in_proc(proc_current(), entry, arg, prio)
       → user_stack_alloc()     /* страница стека в [STACK_BASE, STACK_TOP) */
       → uthread_spawn_in_proc()
            → lwkt_create("u2.N", uthread_user_trampoline, uthread, prio)
            → proc_attach_uthread()
```

Трамплин user-uthread:

```c
uthread_user_trampoline(u)
  → user_enter(u->user_rip, u->user_rsp, u->user_arg, &saved_kernel_rsp)
  → iretq в ring 3 на worker(arg)
  → (после exit из ring 3) uthread_exit()
```

`user_enter_asm` кладёт `arg` в `rdi` перед `iretq`.

### 5.2. `SYS_MUTEX_LOCK` / `UNLOCK` (16 / 17)

Файл: `kernel/proc/proc_mutex.c`

```
proc_mutex_lock(id)
  → proc_mutex_lock_slot(p->mutexes, id)
       spin_lock(guard)
       if (!locked) { locked=1; return; }
       enqueue в waiters → lwkt_block()
```

- **`spinlock_t guard`** — защита полей mutex (SMP-ready).
- **`lwkt_block()`** — uthread/LWKT переходит в `THREAD_BLOCKED`, планировщик выбирает другой поток.
- **`unlock`** — снимает lock, `lwkt_unblock()` первого из очереди `waiters` (`lwkt_thread.wait_next`).

Mutex'ы **per-proc** (8 слотов, `MYOS_PROC_MUTEX_MAX`).

### 5.3. `SYS_THREAD_JOIN` (15)

```
uthread_join(lwkt_id)
  → пока worker жив: join_waiter = self; lwkt_block()
  → при exit worker: exit_code сохраняется, joiner будится
  → return exit_code
```

Запись exit code: в `SYS_EXIT` → `uthread->exit_code = code` → `uthread_record_join()` перед cleanup.

### 5.4. `SYS_EXIT` (0)

```
uthread_exit()
  → uthread_record_join + wakeup join_waiter
  → user_stack_free (если был user_stack_base)
  → proc_on_uthread_exit (detach; proc_destroy если последний uthread)
  → lwkt_thread_exit → lwkt_yield
```

---

## 6. Планирование и вытеснение (детали)

### 6.1. Таймер

```c
/* kernel/arch/x86_64/interrupt.c */
void timer_interrupt_handler(void) {
    timer_ticks++;
    lwkt_preempt_request();
}
/* после pic_eoi(IRQ_TIMER): */
lwkt_preempt_check();
```

Частота: `TICK_FREQUENCY = 100` Hz (PIT channel 0).

### 6.2. `lwkt_preempt_check`

```c
/* kernel/sched/lwkt.c */
if (preempt_requested && есть другой готовый LWKT)
    lwkt_switch();
```

`lwkt_switch()`:

1. Текущий LWKT → `THREAD_READY`, в run queue
2. Выбор следующего по приоритету
3. **`lwkt_apply_cr3(next)`** — CR3 proc следующего user-потока
4. **`lwkt_apply_tss(next)`** — TSS.RSP0 = вершина kernel stack LWKT
5. `switch_context` — переключение стека ядра

Инвариант: при каждом switch на user-capable LWKT **CR3 и TSS вместе**.

### 6.3. Кооперация vs вытеснение в демо

| Действие в `threads.elf` | Тип переключения |
|--------------------------|------------------|
| `myos_yield()` | кооперативный |
| Таймер во время цикла worker | вытесняющий |
| `myos_mutex_lock` при занятом lock | блокировка (`lwkt_block`) |
| `myos_thread_join` | блокировка до exit worker |

---

## 7. Интерпретация `uthreads` после exit

Пример после завершения `threads.elf`:

```
Slot  PID  #InProc  LWKT  Type    State
  0    -    -        1    kernel  blocked   msgd
  1    1    1        2    user    running   shell.elf
```

- Worker'ы proc 2 **исчезли** — слоты uthread/LWKT освобождены.
- Shell (PID 1) — единственный user-proc.
- msgd — kernel LWKT, не привязан к user proc.

---

## 8. Соглашения по mutex id

| id | Константа | Назначение |
|----|-----------|------------|
| 0 | `MYOS_MUTEX_DATA` | общие данные, счётчики |
| 1 | `MYOS_MUTEX_CONSOLE` | сериализация вывода |
| 2–7 | — | свободны для приложения |

Определены в `user/myos_thread.h` и `include/myos_abi.h` (`MYOS_PROC_MUTEX_MAX = 8`).

---

## 9. Типичные ошибки

| Ошибка | Симптом |
|--------|---------|
| `return` из worker вместо `myos_exit` | GPF / page fault |
| Shared данные без mutex при нескольких uthread | неверный `counter`, гонки |
| Большой массив на стеке uthread | stack overflow (~4 KiB) |
| `join` на чужой LWKT id | отрицательный код ошибки |
| Двойной `unlock` | `-2` от `proc_mutex_unlock` |

---

## 10. Запуск и проверка

```bash
make && make run
```

В shell:

```
exec threads.elf
uthreads
ps
```

Ожидаемый вывод программы:

```
threads.elf: spawning 2 workers (mutex 0=data, 1=console)
  worker 2 start          ← выше приоритет
  worker 1 start
  worker 2 done
  worker 1 done
join: t1=1 t2=2 counter=10
main done

Process 2 (threads.elf) exited
```

---

## 11. Связанные файлы

| Файл | Содержание |
|------|------------|
| `user/threads.c` | демо-программа |
| `user/myos_thread.h` | обёртки spawn/join, константы mutex |
| `user/myos_util.h` | вывод без libc |
| `include/myos_abi.h` | номера syscall, VA layout |
| `kernel/sched/uthread.c` | uthread pool, spawn, join, exit |
| `kernel/proc/proc_mutex.c` | blocking mutex + wait queue |
| `kernel/sched/lwkt.c` | планировщик, preempt, switch |
| `kernel/arch/x86_64/interrupt.c` | PIT, preempt на IRQ |
| `kernel/syscall/syscall.c` | dispatch threading syscall'ов |

---

*Последнее обновление: после multi-mutex и spinlock (Этап 1 подготовки к SMP).*
