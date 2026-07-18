# MyOS — sleepable token (учебный пример)

Упрощённый аналог **DragonFly token**: эксклюзивная сериализация с **сном**
при конфликте, а не busy-wait.

Полная реализация в DragonFly включает shared tokens, MP-правила и
привязку к подсистемам. В MyOS — минимальный building block для
длинных/блокирующих путей ядра.

---

## 1. Три примитива MyOS

| Примитив | Когда | SMP |
|----------|-------|-----|
| **spinlock** | Очень коротко (планировщик, страницы, tail IRQ) | `irqsave` где нужно |
| **token** | Сериализация подсистемы/объекта, можно спать | `lwkt_block` + IPI |
| **lwkt_block** | Низкоуровневое ожидание (внутри token/mutex) | уже есть |

---

## 2. API

```c
struct token {
    spinlock_t guard;           /* только метаданные token */
    struct lwkt_thread *holder;
    struct lwkt_thread *waiters;
};

void token_init(struct token *t);
void token_lock(struct token *t);    /* sleep if busy */
int  token_trylock(struct token *t); /* 0 = would block */
void token_unlock(struct token *t);
```

**Не вызывать из IRQ** — в обработчике прерывания нельзя `lwkt_block()`.

---

## 3. Пример: msgport

Каждый mailbox:

```c
struct mailbox {
    struct token lock;              /* эксклюзивный доступ к очереди */
    struct lwkt_thread *read_waiters; /* ждут сообщение (не lost wakeup) */
    struct msg queue[MSG_QUEUE_DEPTH];
    ...
};
```

- `msg_send` — `token_lock` → push → разбудить `read_waiters` → `token_unlock`
- `msg_receive` — `token_lock` → pop или встать в `read_waiters` → `token_unlock` → `lwkt_block`

Раньше: `cli`/`sti` (защищало только текущее CPU). Теперь: token (все CPU).

---

## 4. Сравнение с DragonFly и FreeBSD spinlock

**DragonFly token** — центральная дисциплина ядра; много подсистем на tokens.

**FreeBSD 2000-х (ваш опыт)** — широкое использование spinlock/Mutex на SMP:
contention, hold time в драйверах, priority inversion, «IRQ + spinlock» deadlock.

**MyOS сейчас** — осознанный гибрид:

- spinlock остаётся на **горячих** путях (`queue_lock` / `thread_pool_lock`, `page_lock`);
- token — для **объектов** вроде mailbox, позже VFS/буферы;
- не заменяем всё ядро tokens за один шаг.

---

## 5. `token_shared` (readers / writer)

Многие читатели **или** один писатель. Write-preferring: ожидающий writer
блокирует новых readers (нет голодания writer).

```c
struct token_shared {
    spinlock_t guard;
    int readers;
    struct lwkt_thread *writer;
    struct lwkt_thread *read_waiters;
    struct lwkt_thread *write_waiters;
};

void token_shared_read_lock(struct token_shared *t);
void token_shared_read_unlock(struct token_shared *t);
void token_shared_write_lock(struct token_shared *t);
void token_shared_write_unlock(struct token_shared *t);
```

При boot: `token_shared selftest OK` и `token_shared MP selftest OK`.

Применение: таблица модулей Limine, статистика VFS, read-mostly конфиг.

---

## 6. Дальше

- per-subsystem tokens (vnode, mount) при росте VFS
- msgport по имени, `kbdd` IPC — см. [ROADMAP.md](ROADMAP.md) фазы 4–5

См. [SMP.md](SMP.md), [DEVELOPMENT.md](DEVELOPMENT.md), [ROADMAP.md](ROADMAP.md).
