#include "wut.h"

#include <assert.h> // assert
#include <errno.h> // errno
#include <stdbool.h> // bool
#include <stddef.h> // NULL
#include <stdint.h> // uintptr_t
#include <stdio.h> // perror
#include <stdlib.h> // reallocarray
#include <sys/mman.h> // mmap, munmap
#include <sys/signal.h> // SIGSTKSZ
#include <sys/queue.h> // TAILQ_*
#include <ucontext.h> // getcontext, makecontext, setcontext, swapcontext
#include <valgrind/valgrind.h> // VALGRIND_STACK_REGISTER

static void die(const char* message) {
    int err = errno;
    perror(message);
    exit(err);
}

static char* new_stack(void) {
    char* stack = mmap(
        NULL,
        SIGSTKSZ,
        PROT_READ | PROT_WRITE | PROT_EXEC,
        MAP_ANONYMOUS | MAP_PRIVATE,
        -1,
        0
    );
    if (stack == MAP_FAILED) {
        die("mmap stack failed");
    }
    VALGRIND_STACK_REGISTER(stack, stack + SIGSTKSZ);
    return stack;
}

static void delete_stack(char* stack) {
    if (munmap(stack, SIGSTKSZ) == -1) {
        die("munmap stack failed");
    }
}

enum wut_state {
    WUT_STATE_UNUSED = 0,
    WUT_STATE_RUNNING,
    WUT_STATE_READY,
    WUT_STATE_BLOCKED,
    WUT_STATE_ZOMBIE,
};

struct wut_tcb {
    int id;
    ucontext_t ctx;
    char* stack;
    int exit_status;
    enum wut_state state;
    int waiting_for;
    int joiner;
    int queued;
    TAILQ_ENTRY(wut_tcb) ready_link;
};

TAILQ_HEAD(ready_queue, wut_tcb);

static struct ready_queue ready_threads;
static struct wut_tcb** tcb_table = NULL;
static size_t tcb_table_len = 0;
static struct wut_tcb* current_thread = NULL;
static int initialized = 0;

static struct wut_tcb* get_tcb(int id) {
    if (id < 0 || (size_t)id >= tcb_table_len) {
        return NULL;
    }
    return tcb_table[id];
}

static void ensure_capacity(size_t count) {
    if (count <= tcb_table_len) {
        return;
    }
    struct wut_tcb** new_table = reallocarray(tcb_table, count, sizeof(*new_table));
    if (!new_table) {
        die("reallocarray failed");
    }
    for (size_t i = tcb_table_len; i < count; ++i) {
        new_table[i] = NULL;
    }
    tcb_table = new_table;
    tcb_table_len = count;
}

static struct wut_tcb* allocate_tcb(int id) {
    struct wut_tcb* tcb = calloc(1, sizeof(*tcb));
    if (!tcb) {
        die("calloc tcb failed");
    }
    tcb->id = id;
    tcb->stack = NULL;
    tcb->exit_status = 0;
    tcb->state = WUT_STATE_UNUSED;
    tcb->waiting_for = -1;
    tcb->joiner = -1;
    tcb->queued = 0;
    return tcb;
}

static void enqueue_thread(struct wut_tcb* tcb) {
    if (!tcb || tcb->queued) {
        return;
    }
    TAILQ_INSERT_TAIL(&ready_threads, tcb, ready_link);
    tcb->queued = 1;
    tcb->state = WUT_STATE_READY;
}

static struct wut_tcb* dequeue_thread(void) {
    struct wut_tcb* next = TAILQ_FIRST(&ready_threads);
    if (!next) {
        return NULL;
    }
    TAILQ_REMOVE(&ready_threads, next, ready_link);
    next->queued = 0;
    next->state = WUT_STATE_RUNNING;
    return next;
}

static int switch_to_next(void) {
    struct wut_tcb* next = dequeue_thread();
    if (!next) {
        return -1;
    }
    struct wut_tcb* prev = current_thread;
    current_thread = next;
    if (swapcontext(&prev->ctx, &next->ctx) == -1) {
        die("swapcontext failed");
    }
    current_thread = prev;
    prev->state = WUT_STATE_RUNNING;
    return 0;
}

static void cleanup_tcb(struct wut_tcb* tcb) {
    if (!tcb) {
        return;
    }
    if (tcb->stack) {
        delete_stack(tcb->stack);
        tcb->stack = NULL;
    }
    int id = tcb->id;
    if (id >= 0 && (size_t)id < tcb_table_len && tcb_table[id] == tcb) {
        tcb_table[id] = NULL;
    }
    free(tcb);
}

static void wake_joiner(struct wut_tcb* target) {
    if (!target || target->joiner == -1) {
        return;
    }
    struct wut_tcb* joiner = get_tcb(target->joiner);
    if (!joiner) {
        return;
    }
    joiner->waiting_for = -1;
    if (joiner->state == WUT_STATE_BLOCKED) {
        enqueue_thread(joiner);
    }
}

static void thread_trampoline(uintptr_t fn_ptr) {
    void (*run)(void) = (void (*)(void))fn_ptr;
    run();
    wut_exit(0);
}

void wut_init() {
    if (initialized) {
        return;
    }
    TAILQ_INIT(&ready_threads);
    ensure_capacity(1);
    struct wut_tcb* main_tcb = allocate_tcb(0);
    if (getcontext(&main_tcb->ctx) == -1) {
        die("getcontext failed");
    }
    main_tcb->state = WUT_STATE_RUNNING;
    tcb_table[0] = main_tcb;
    current_thread = main_tcb;
    initialized = 1;
}

int wut_id() {
    if (!current_thread) {
        return -1;
    }
    return current_thread->id;
}

int wut_create(void (*run)(void)) {
    if (!initialized || !run) {
        return -1;
    }

    int id = -1;
    for (size_t i = 0; i < tcb_table_len; ++i) {
        if (!tcb_table[i]) {
            id = (int)i;
            break;
        }
    }
    if (id == -1) {
        ensure_capacity(tcb_table_len + 1);
        id = (int)(tcb_table_len - 1);
    }

    struct wut_tcb* tcb = allocate_tcb(id);
    if (getcontext(&tcb->ctx) == -1) {
        die("getcontext failed");
    }
    char* stack = new_stack();
    tcb->stack = stack;
    tcb->ctx.uc_stack.ss_sp = stack;
    tcb->ctx.uc_stack.ss_size = SIGSTKSZ;
    tcb->ctx.uc_stack.ss_flags = 0;
    tcb->ctx.uc_link = NULL;
    makecontext(&tcb->ctx, (void (*)(void))thread_trampoline, 1, (uintptr_t)run);
    tcb->state = WUT_STATE_READY;
    tcb_table[id] = tcb;
    enqueue_thread(tcb);
    return id;
}

int wut_cancel(int id) {
    if (!initialized) {
        return -1;
    }
    if (id < 0 || (size_t)id >= tcb_table_len) {
        return -1;
    }
    if (current_thread && current_thread->id == id) {
        return -1;
    }
    struct wut_tcb* target = tcb_table[id];
    if (!target) {
        return -1;
    }
    if (target->state != WUT_STATE_READY || !target->queued) {
        return -1;
    }

    TAILQ_REMOVE(&ready_threads, target, ready_link);
    target->queued = 0;
    target->state = WUT_STATE_ZOMBIE;
    target->exit_status = 128;
    if (target->stack) {
        delete_stack(target->stack);
        target->stack = NULL;
    }
    wake_joiner(target);
    return 0;
}

int wut_join(int id) {
    if (!initialized) {
        return -1;
    }
    if (!current_thread || id < 0 || (size_t)id >= tcb_table_len) {
        return -1;
    }
    struct wut_tcb* target = tcb_table[id];
    if (!target) {
        return -1;
    }
    if (target->id == current_thread->id) {
        return -1;
    }
    if (target->joiner != -1 && target->joiner != current_thread->id) {
        return -1;
    }
    if (target->waiting_for != -1) {
        return -1;
    }

    if (target->state == WUT_STATE_ZOMBIE) {
        target->joiner = current_thread->id;
        int status = target->exit_status;
        cleanup_tcb(target);
        return status;
    }

    if (target->state != WUT_STATE_READY || !target->queued) {
        return -1;
    }

    target->joiner = current_thread->id;
    current_thread->waiting_for = id;
    current_thread->state = WUT_STATE_BLOCKED;

    if (switch_to_next() == -1) {
        current_thread->state = WUT_STATE_RUNNING;
        current_thread->waiting_for = -1;
        target->joiner = -1;
        return -1;
    }

    current_thread->waiting_for = -1;
    target = get_tcb(id);
    if (!target || target->state != WUT_STATE_ZOMBIE) {
        return -1;
    }
    int status = target->exit_status;
    cleanup_tcb(target);
    return status;
}

int wut_yield() {
    if (!initialized || !current_thread) {
        return -1;
    }
    if (TAILQ_EMPTY(&ready_threads)) {
        return -1;
    }
    enqueue_thread(current_thread);
    if (switch_to_next() == -1) {
        return -1;
    }
    return 0;
}

void wut_exit(int status) {
    if (!initialized || !current_thread) {
        exit(0);
    }
    status &= 0xFF;
    current_thread->exit_status = status;
    current_thread->state = WUT_STATE_ZOMBIE;
    wake_joiner(current_thread);

    if (TAILQ_EMPTY(&ready_threads)) {
        exit(0);
    }

    struct wut_tcb* next = dequeue_thread();
    if (!next) {
        exit(0);
    }
    current_thread = next;
    setcontext(&next->ctx);
}
