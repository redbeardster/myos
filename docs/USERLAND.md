# MyOS — разработка userland-программ

Руководство по написанию, сборке и запуску программ для MyOS (ring 3, freestanding ELF).

См. также: [DEVELOPMENT.md](DEVELOPMENT.md) — архитектура ядра и инварианты.

---

## 1. Кратко

| | |
|---|---|
| Язык | C (рекомендуется), ассемблер для crt0 |
| libc | **нет** — `user/myos.h`, `myos_util.h`, `myos_thread.h` |
| ABI | **свой** (`include/myos_abi.h`), не Linux |
| Формат | статический ELF64, база `0x400000` |
| Запуск | `exec имя.elf` из shell или embed в ядро |

---

## 2. Память процесса

Все процессы используют **одинаковую** виртуальную карту; физические страницы изолированы (CR3):

```
0x0040_0000  .text / .rodata / .data / .bss  (ELF)
0x0050_0000+ куча (myos_alloc_page, растёт вверх)
0x0060_0000  нижняя граница стеков uthread
0x0070_0000  верх стеков (по одной странице на uthread, растут вниз)
```

Ограничения:

- стек **каждого** uthread ~4 KiB — не кладите большие массивы на стек;
- куча — по одной странице (4 KiB) за вызов `myos_alloc_page()`;
- нет `mmap`, `sbrk`, `brk`.
- максимум ~256 uthread-стеков на процесс (1 MiB между `STACK_BASE` и `STACK_TOP`).

---

## 3. Системные вызовы

Вызов: `int 0x80`

| Регистр | Назначение |
|---------|------------|
| `rax` | номер syscall |
| `rdi`, `rsi`, `rdx`, `rcx` | аргументы |
| `rax` (выход) | результат; отрицательное = ошибка |

### Таблица syscall

| № | Имя | Обёртка | Описание |
|---|-----|---------|----------|
| 0 | `MYOS_SYS_EXIT` | `myos_exit(code)` | завершить процесс |
| 1 | `MYOS_SYS_WRITE` | `myos_write(fd, buf, len)` | вывод: fd 1 или 2 → консоль |
| 2 | `MYOS_SYS_YIELD` | `myos_yield()` | уступить CPU |
| 5 | `MYOS_SYS_ALLOC` | `myos_alloc_page()` | страница кучи |
| 6 | `MYOS_SYS_FREE` | `myos_free_page(p)` | освободить страницу |
| 7 | `MYOS_SYS_READ` | `myos_read_char()` | один символ (клавиатура) |
| 8 | `MYOS_SYS_EXEC` | `myos_exec(path)` | запустить ELF |
| 9 | `MYOS_SYS_PS` | `myos_ps()` | список процессов → консоль |
| 10 | `MYOS_SYS_THREADS` | `myos_threads()` | список LWKT → консоль |
| 11 | `MYOS_SYS_UTHREADS` | `myos_uthreads()` | список uthread → консоль |
| 14 | `MYOS_SYS_THREAD_CREATE` | `myos_thread_create(entry, arg, prio)` | новый uthread в текущем proc |
| 15 | `MYOS_SYS_THREAD_JOIN` | `myos_thread_join(lwkt_id)` | ждать uthread; возврат = exit code |
| 16 | `MYOS_SYS_MUTEX_LOCK` | `myos_mutex_lock(id)` | mutex процесса по номеру (0..7) |
| 17 | `MYOS_SYS_MUTEX_UNLOCK` | `myos_mutex_unlock(id)` | отпустить mutex |

Идентификаторы mutex (соглашение в `myos_thread.h`):

| id | Имя | Типичное использование |
|----|-----|------------------------|
| 0 | `MYOS_MUTEX_DATA` | общие данные / счётчики |
| 1 | `MYOS_MUTEX_CONSOLE` | сериализация вывода |
| 2–7 | — | свободны для приложения |

`MYOS_PROC_MUTEX_MAX = 8` — лимит на процесс. Очередь ожидания на каждый mutex (несколько uthread могут ждать один lock).

Приоритеты для `THREAD_CREATE` (`MYOS_PRIO_*` в `myos_abi.h`): `HIGH=2`, `NORMAL=8`, `LOW=15` (0 = наивысший в LWKT).

Номера 3–4 (`MSG_SEND`/`MSG_RECV`) в ядре есть, в `myos.h` пока не обёрнуты.

---

## 4. Минимальная программа

```c
#include "myos.h"

int main(void) {
    const char msg[] = "Hello!\n";
    myos_write(1, msg, sizeof(msg) - 1);
    return 0;
}
```

Точка входа — `_start` в `user/crt0.asm`: вызывает `main()`, затем `SYS_EXIT`.
`argc` / `argv` **не передаются**.

---

## 5. Добавление новой программы (3 шага)

### Шаг 1 — исходник

```bash
cp user/template.c user/cat.c
# отредактировать user/cat.c
```

Шаблон: `user/template.c`.

### Шаг 2 — список программ

В `user/programs.mk` добавить имя **без** `.elf`:

```makefile
PROGRAMS = shell hello cat
```

Этого достаточно для:

- сборки `user/cat.elf` (правила в `user/Makefile`);
- вшивания blob в `kernel.elf` (автоматически);
- копирования на ISO;
- регистрации в `exec` (таблица генерируется);
- строк `module_path` в `limine.conf` (генерируется из `limine.conf.in`).

**Редактировать `kernel/proc/exec.c` и корневой `Makefile` вручную не нужно.**

### Шаг 3 — сборка и запуск

```bash
cd ~/os
make          # пересоберёт userland, ядро, limine.conf, ISO
make run
```

В shell:

```
exec cat.elf
```

---

## 6. Типы программ

### Shell (`shell.elf`)

- Загружается при старте (`EXEC_FLAG_SHELL`).
- Единственный долгоживущий userland-процесс.
- Не убивается `proc_kill_children()`.
- Запускает дочерние через `myos_exec()`.

### Дочерние (`hello.elf`, `threads.elf`, …)

- Создаются через `exec` из shell.
- Свой CR3, своя куча; один или несколько uthread.
- При `exit` / возврате из `main` **uthread** завершается; процесс уничтожается, когда выходит **последний** uthread.
- Родитель **не ждёт** завершения дочернего proc (`wait` для exec пока нет).
- Внутри одного proc: `myos_thread_join()` ждёт sibling-uthread.

---

## 7. Многопоточность (uthreads)

### Модель

```
exec foo.elf
  └─ proc (один CR3, общая куча и глобальные переменные)
       ├─ uthread main   — стек ~4 KiB
       ├─ uthread worker — свой стек
       └─ uthread worker — свой стек
```

Все uthread одного proc разделяют адресное пространство. Стеки изолированы.

### Заголовки

| Файл | Назначение |
|------|------------|
| `user/myos.h` | syscall-обёртки |
| `user/myos_util.h` | `myos_write_str`, `myos_write_dec`, `myos_strlen` |
| `user/myos_thread.h` | `myos_thread_spawn`, `join`, `mutex`, приоритеты |

### Минимальный worker

```c
#include "myos_thread.h"
#include "myos_util.h"

void worker(uint64_t arg) {
    myos_write_dec(arg);
    myos_write_char('\n');
    myos_exit(0);          /* обязательно — return из worker = crash */
}

int main(void) {
    long t = myos_thread_spawn(worker, 42);
    if (t < 0) return 1;
    myos_thread_join(t);   /* дождаться завершения */
    return 0;
}
```

### Правила

| Правило | Почему |
|---------|--------|
| Worker вызывает `myos_exit()` | на стеке нет return address |
| `main` может `return` | `crt0` вызовет `SYS_EXIT` |
| Shared data — через кучу или `.bss` | общий CR3 |
| Защита shared data — `myos_mutex_lock(id)` | до 8 mutex на proc; **нужен из-за вытеснения по таймеру** |
| Уступка CPU — `myos_yield()` | кооперация; таймер (~100 Hz) тоже вытесняет |
| Отладка — `uthreads` в shell | таблица uthread |

**Вытеснение:** отдельного планировщика uthread нет — user-потоки бегут на LWKT, а LWKT вытесняется таймером PIT и при выходе из syscall. Подробно: [THREADS_DEMO.md](THREADS_DEMO.md) §1 и §6.

### Пример с mutex и join

См. `user/threads.c` — два worker'а инкрементируют счётчик под mutex; `main` делает `join`.

**Полный разбор демо и ядра:** [THREADS_DEMO.md](THREADS_DEMO.md).

Запуск: `exec threads.elf`, затем `uthreads`.

### Создание своей программы

```bash
cp user/template.c user/mine.c
# раскомментировать блок multithreading или скопировать threads.c
# добавить mine в user/programs.mk
make && make run
```

---

## 8. Сборка userland

### Флаги компиляции

```
-m64 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector
-fno-pic -fno-pie -mno-red-zone
```

### Файлы

| Файл | Назначение |
|------|------------|
| `user/crt0.asm` | `_start`, вызов `main`, `SYS_EXIT` |
| `user/linker.ld` | база `0x400000` |
| `user/myos.h` | обёртки syscall |
| `user/myos_util.h` | строки, числа без libc |
| `user/myos_thread.h` | uthread spawn / join / mutex |
| `user/programs.mk` | **список всех программ** |
| `include/myos_abi.h` | номера syscall, VA layout |

### Команды

```bash
make -C user          # только userland
make -C user clean
```

---

## 9. Как программа попадает в систему

```
user/foo.c
    → user/foo.elf          (gcc + ld + crt0)
    → build/foo_embed.o     (ld -r -b binary)
    → build/user_embeds.o   (таблица имён, gen script)
    → build/kernel.elf
    → build/iso_root/boot/foo.elf
    → build/myos.iso
```

При `exec foo.elf` ядро ищет ELF:

1. модули Limine (с диска ISO);
2. таблицу встроенных blob (`user_embed_lookup`).

Оба источника ведут к одному и тому же `foo.elf` после `make`.

---

## 10. Полезные приёмы без libc

### Вывод строки

```c
#include "myos_util.h"
myos_write_str("hello\n");
```

Или вручную (если не хотите util):

```c
static void write_str(const char *s) {
    unsigned long n = 0;
    while (s[n]) n++;
    myos_write(1, s, n);
}
```

### Форматирование числа (упрощённо)

См. `user/shell.c` — там есть разбор команд и вывод PID.

### Буфер на куче

```c
char *buf = (char *)myos_alloc_page();
if (!buf) { /* OOM */ }
/* ... */
myos_free_page(buf);
```

---

## 11. Отладка

| Метод | Когда |
|-------|--------|
| `myos_write(1, …)` | printf-замена |
| `ps` / `threads` / `uthreads` в shell | живые proc и uthread |
| `myos_ps()` / `myos_threads()` / `myos_uthreads()` | то же из кода |
| Пересборка `make` | после каждого изменения user **и** kernel embed |

Типичные ошибки:

| Симптом | Причина |
|---------|---------|
| `exec failed` | имя не в `programs.mk` или не пересобран ISO |
| GPF / page fault | большой стековый массив, битый указатель |
| Тишина после exec | программа завершилась без вывода |

---

## 12. Чеклист новой программы

- [ ] `user/foo.c` с `#include "myos.h"` и `int main(void)`
- [ ] `foo` добавлен в `PROGRAMS` в `user/programs.mk`
- [ ] `make` без ошибок
- [ ] `exec foo.elf` — первый раз OK
- [ ] `exec foo.elf` — **второй раз** подряд OK (PID переиспользуется)
- [ ] `ps` / `threads` / `uthreads` между exec — без сбоев
- [ ] (multithread) worker'ы завершаются через `myos_exit`, не `return`

---

## 13. Дальнейшее развитие

| Улучшение | Зачем |
|-----------|--------|
| `user/libmyos.a` | линковать util как библиотеку |
| `argc`/`argv` в crt0 | аргументы командной строки |
| per-mutex / futex | futex поверх spinlock (после SMP) |
| `wait` syscall | родитель узнаёт о завершении дочернего proc |

---

## 14. Ссылки в репозитории

```
user/template.c       — стартовый шаблон (+ закомментированный threading)
user/threads.c        — демо: spawn, join, mutex
docs/THREADS_DEMO.md  — полный разбор threads.elf и планирования
user/hello.c          — пример: вывод, куча, yield
user/shell.c          — интерактивная оболочка
user/myos_util.h      — утилиты вывода
user/myos_thread.h    — обёртки uthread
user/programs.mk      — список программ (главный конфиг)
tools/gen_user_embeds.sh
tools/gen_limine_conf.sh
docs/DEVELOPMENT.md   — ядро и планировщик
```
