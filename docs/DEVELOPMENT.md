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
│  uthread.c   — связка LWKT ↔ процесс ↔ user entry           │
│  msgport.c   — сообщения только между потоками ядра         │
└─────────────────────────────────────────────────────────────┘
```

**Загрузка:** Limine → `kernel.c` → `exec_start_shell()` → `lwkt_bootstrap_first()`.

**Модель процессов:** у каждого `proc` свой CR3; виртуальная карта одинакова у всех
(`0x400000` text, `0x500000+` heap, `0x700000` stack), физические страницы изолированы.

---

## 2. Слои и границы ответственности

### Userland → ядро

- Только **системные вызовы** (`int 0x80`), номера и layout — в `myos_abi.h` / `user/myos.h`.
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
  → proc_create(cr3, …)
  → uthread_spawn_in_proc()
  → lwkt_create() + enqueue

user: SYS_EXIT
  → uthread_exit()
  → uthread_cleanup() → proc_on_uthread_exit() → proc_destroy (при последнем uthread)
  → lwkt_thread_exit() → lwkt_yield()   // без возврата в userland
```

Инварианты:

- `SYS_EXIT` не возвращается в syscall stub (`__builtin_unreachable`).
- `SYS_READ` блокируется через `lwkt_block()` на **syscall stack**, не через jump на user stack.
- После exit LWKT-слот: `id = 0`, `msgport_clear_slot()` в `lwkt_thread_exit()`.
- Shell (`EXEC_FLAG_SHELL`, `proc->is_shell`) не убивается через `proc_kill_children()`.

---

## 6. Правила для syscall-обработчиков

| Syscall | Поведение |
|---------|-----------|
| `SYS_WRITE`, `SYS_PS`, `SYS_THREADS`, `SYS_ALLOC`, `SYS_FREE`, `SYS_EXEC` | Обработать и вернуться через `iretq` |
| `SYS_READ` | `keyboard_set_reader` → `lwkt_block()` (цикл на syscall stack) |
| `SYS_YIELD` | `lwkt_yield()` на syscall stack |
| `SYS_EXIT` | `uthread_exit()` — без return |
| `SYS_MSG_*` | Как READ/WRITE; при block — `lwkt_block()` внутри msgport |

**Запрещено:**

- `syscall_return` / произвольные переходы между user stack и kernel stack.
- Двойной вызов post-syscall логики после `SYS_EXIT` или `SYS_READ`.
- Прямой `vmm_switch()` в syscall без учёта того, какой поток станет текущим.

---

## 7. Добавление нового syscall

1. Константа в `myos_abi.h` и обёртка в `user/myos.h`.
2. Ветка в `syscall_dispatch()` в `syscall.c`.
3. Копирование аргументов из userland в kernel buffer (`copy_from_user`).
4. Выбор модели завершения: быстрый return / block / exit (таблица выше).
5. Если syscall глубокий (много локальных переменных, циклы, I/O) — учитывать размер
   **syscall stack** (`STACK_SIZE` = 4 KiB на LWKT-поток, стек в `TSS.RSP0`).

---

## 8. Добавление нового userland-бинарника

1. Исходник в `user/`, linker script `user/linker.ld` (база `0x400000`).
2. Цель в `user/Makefile`, embed в ядро (`hello_embed.o` / `shell_embed.o`) или модуль Limine.
3. Регистрация в `exec_spawn_module()` (`exec.c`) по суффиксу имени.
4. Запуск из shell: `exec name.elf` → `SYS_EXEC`.

Проверки после добавления — см. раздел 10.

---

## 9. Ключевые файлы

| Файл | Назначение |
|------|------------|
| `myos_abi.h` | VA layout, номера syscall |
| `isr.asm` | `int 0x80`, `user_enter_asm`, `switch_context` |
| `syscall.c` | Dispatch и copy_from_user |
| `lwkt.c` | Планировщик, CR3, TSS, deferred CR3 destroy |
| `uthread.c` | User/kernel uthread, trampoline |
| `proc.c` | Таблица процессов, destroy, kill |
| `vmm.c` | Aspace, map/unmap, active CR3 checks |
| `exec.c` / `elf.c` | Загрузка ELF в aspace процесса |
| `user.c` | `user_enter`, heap в aspace текущего proc |
| `gdt.c` | GDT, TSS, `tss_set_rsp0()` |

---

## 10. Чеклист регрессии

Перед merge / после нетривиальных изменений в schedule, syscall, vmm, exec:

- [ ] `exec hello.elf` **дважды подряд** — оба раза вывод и `Process N exited`.
- [ ] Между двумя `exec`: `ps`, `threads` — без GPF/page fault.
- [ ] После exit дочернего: `ps` показывает только shell; `threads` — shell + idle.
- [ ] Shell остаётся с тем же CR3 в `ps` до и после дочернего процесса.
- [ ] `read` / prompt: ввод команды после exit дочернего работает.
- [ ] (При изменении msgport) слоты mbox очищаются в `lwkt_thread_exit` / `lwkt_destroy`.

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
- Новые блокирующие syscall — по образцу `SYS_READ` (`lwkt_block` на syscall stack).
- Любое копирование из user VA — с активным CR3 **того процесса**, который вызвал syscall
  (`proc_current()->cr3` уже выбран через `lwkt_apply_cr3`).
- Для SMP в будущем: TSS и CR3 per-CPU / per-thread, те же инварианты на каждом ядре.

---

*Последнее обновление: контекст после исправления TSS.RSP0 и deferred CR3 destroy.*
