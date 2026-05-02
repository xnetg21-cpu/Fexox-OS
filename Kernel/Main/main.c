#include <stdint.h>
#include <string.h>

// ============================
// SYSTEM CONSTANTS
// ============================

#define IDT_SIZE 256
#define MAX_TASKS 64
#define MAX_SEMAPHORES 128
#define MAX_MUTEXES 128
#define MAX_NAMED_PIPES 64
#define MAX_MESSAGE_QUEUES 64
#define MAX_BARRIERS 32

// Task states
#define TASK_STATE_READY 0
#define TASK_STATE_RUNNING 1
#define TASK_STATE_BLOCKED 2
#define TASK_STATE_WAITING_SEMAPHORE 3
#define TASK_STATE_WAITING_MUTEX 4
#define TASK_STATE_WAITING_IPC 5
#define TASK_STATE_TERMINATED 6
#define TASK_STATE_SUSPENDED 7

// ============================
// PORT I/O
// ============================

static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    __asm__ volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

// ============================
// PAGING & MEMORY
// ============================

uint64_t PML4[512] __attribute__((aligned(4096)));
uint64_t PDP[512]  __attribute__((aligned(4096)));
uint64_t PD[512]   __attribute__((aligned(4096)));

void init_paging() {
    PML4[0] = (uint64_t)PDP | 0x3;
    PDP[0]  = (uint64_t)PD  | 0x3;

    for (int i = 0; i < 512; i++) {
        PD[i] = (i * 0x200000) | 0x83;
    }

    __asm__ volatile ("mov %0, %%cr3" : : "r"(PML4));

    uint64_t cr0, cr4;
    __asm__ volatile ("mov %%cr4, %0" : "=r"(cr4));
    cr4 |= (1 << 5);
    __asm__ volatile ("mov %0, %%cr4" : : "r"(cr4));

    __asm__ volatile ("mov %%cr0, %0" : "=r"(cr0));
    cr0 |= (1 << 31);
    __asm__ volatile ("mov %0, %%cr0" : : "r"(cr0));
}

// ============================
// IDT SETUP
// ============================

struct IDTEntry {
    uint16_t offset_low;
    uint16_t selector;
    uint8_t  ist;
    uint8_t  type_attr;
    uint16_t offset_mid;
    uint32_t offset_high;
    uint32_t zero;
} __attribute__((packed));

struct IDTPtr {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

struct IDTEntry idt[IDT_SIZE];
struct IDTPtr idt_ptr;

// Внешние обработчики из context_switch.asm
extern void timer_irq_handler();
extern void page_fault_handler();
extern void general_protection_fault_handler();

void set_idt_gate(int n, uint64_t handler, uint8_t flags) {
    idt[n].offset_low  = handler & 0xFFFF;
    idt[n].selector    = 0x08;           // Code segment selector
    idt[n].ist         = 0;              // Interrupt Stack Table (0 = use RSP0)
    idt[n].type_attr   = flags;          // Type and flags
    idt[n].offset_mid  = (handler >> 16) & 0xFFFF;
    idt[n].offset_high = (handler >> 32);
    idt[n].zero        = 0;
}

void load_idt() {
    idt_ptr.limit = sizeof(idt) - 1;
    idt_ptr.base  = (uint64_t)&idt;
    __asm__ volatile ("lidt %0" : : "m"(idt_ptr));
}

// ============================
// PIC REMAP
// ============================

void pic_remap() {
    outb(0x20, 0x11);
    outb(0xA0, 0x11);
    outb(0x21, 0x20);
    outb(0xA1, 0x28);
    outb(0x21, 0x04);
    outb(0xA1, 0x02);
    outb(0x21, 0x01);
    outb(0xA1, 0x01);
    outb(0x21, 0x0);
    outb(0xA1, 0x0);
}

// ============================
// PIT TIMER
// ============================

volatile uint64_t ticks = 0;

void pit_init() {
    uint32_t freq = 100;
    uint32_t divisor = 1193180 / freq;
    outb(0x43, 0x36);
    outb(0x40, divisor & 0xFF);
    outb(0x40, (divisor >> 8) & 0xFF);
}

// ============================
// SPINLOCK (для синхронизации)
// ============================

typedef volatile uint32_t spinlock_t;

static inline void acquire_spinlock(spinlock_t* lock) {
    while (__sync_lock_test_and_set(lock, 1)) {
        __asm__ volatile ("pause");
    }
}

static inline void release_spinlock(spinlock_t* lock) {
    __sync_lock_release(lock);
}

// ============================
// SEMAPHORE
// ============================

typedef struct {
    uint32_t semaphore_id;
    int32_t count;
    uint32_t waiting_tasks[MAX_TASKS];
    uint32_t waiting_count;
    spinlock_t lock;
} semaphore_t;

semaphore_t semaphores[MAX_SEMAPHORES];
uint32_t semaphore_count = 0;

uint32_t create_semaphore(int32_t initial_count) {
    if (semaphore_count >= MAX_SEMAPHORES) return -1;
    
    uint32_t sem_id = semaphore_count;
    semaphores[sem_id].semaphore_id = sem_id;
    semaphores[sem_id].count = initial_count;
    semaphores[sem_id].waiting_count = 0;
    semaphores[sem_id].lock = 0;
    semaphore_count++;
    
    return sem_id;
}

int32_t wait_semaphore(uint32_t sem_id, uint32_t pid) {
    if (sem_id >= semaphore_count) return -1;
    
    semaphore_t* sem = &semaphores[sem_id];
    
    acquire_spinlock(&sem->lock);
    
    if (sem->count > 0) {
        sem->count--;
        release_spinlock(&sem->lock);
        return 0;
    }
    
    if (sem->waiting_count < MAX_TASKS) {
        sem->waiting_tasks[sem->waiting_count++] = pid;
        release_spinlock(&sem->lock);
        return 0;
    }
    
    release_spinlock(&sem->lock);
    return -1;
}

int32_t signal_semaphore(uint32_t sem_id) {
    if (sem_id >= semaphore_count) return -1;
    
    semaphore_t* sem = &semaphores[sem_id];
    
    acquire_spinlock(&sem->lock);
    
    if (sem->waiting_count > 0) {
        sem->waiting_count--;
    } else {
        sem->count++;
    }
    
    release_spinlock(&sem->lock);
    return 0;
}

// ============================
// MUTEX
// ============================

typedef struct {
    uint32_t mutex_id;
    uint32_t owner_pid;
    uint32_t lock_count;
    uint32_t waiting_tasks[MAX_TASKS];
    uint32_t waiting_count;
    spinlock_t lock;
} mutex_t;

mutex_t mutexes[MAX_MUTEXES];
uint32_t mutex_count = 0;

uint32_t create_mutex() {
    if (mutex_count >= MAX_MUTEXES) return -1;
    
    uint32_t mutex_id = mutex_count;
    mutexes[mutex_id].mutex_id = mutex_id;
    mutexes[mutex_id].owner_pid = (uint32_t)-1;
    mutexes[mutex_id].lock_count = 0;
    mutexes[mutex_id].waiting_count = 0;
    mutexes[mutex_id].lock = 0;
    mutex_count++;
    
    return mutex_id;
}

int32_t lock_mutex(uint32_t mutex_id, uint32_t pid) {
    if (mutex_id >= mutex_count) return -1;
    
    mutex_t* mtx = &mutexes[mutex_id];
    
    acquire_spinlock(&mtx->lock);
    
    if (mtx->owner_pid == (uint32_t)-1 || mtx->owner_pid == pid) {
        mtx->owner_pid = pid;
        mtx->lock_count++;
        release_spinlock(&mtx->lock);
        return 0;
    }
    
    if (mtx->waiting_count < MAX_TASKS) {
        mtx->waiting_tasks[mtx->waiting_count++] = pid;
        release_spinlock(&mtx->lock);
        return 1;
    }
    
    release_spinlock(&mtx->lock);
    return -1;
}

int32_t unlock_mutex(uint32_t mutex_id, uint32_t pid) {
    if (mutex_id >= mutex_count) return -1;
    
    mutex_t* mtx = &mutexes[mutex_id];
    
    acquire_spinlock(&mtx->lock);
    
    if (mtx->owner_pid != pid) {
        release_spinlock(&mtx->lock);
        return -1;
    }
    
    mtx->lock_count--;
    
    if (mtx->lock_count == 0) {
        if (mtx->waiting_count > 0) {
            mtx->owner_pid = mtx->waiting_tasks[0];
            mtx->lock_count = 1;
            
            for (uint32_t i = 0; i < mtx->waiting_count - 1; i++) {
                mtx->waiting_tasks[i] = mtx->waiting_tasks[i + 1];
            }
            mtx->waiting_count--;
        } else {
            mtx->owner_pid = (uint32_t)-1;
        }
    }
    
    release_spinlock(&mtx->lock);
    return 0;
}

// ============================
// NAMED PIPE
// ============================

#define PIPE_BUFFER_SIZE 4096

typedef struct {
    uint32_t pipe_id;
    char name[64];
    uint8_t buffer[PIPE_BUFFER_SIZE];
    uint32_t read_pos;
    uint32_t write_pos;
    uint32_t reader_pid;
    uint32_t writer_pid;
    spinlock_t lock;
} named_pipe_t;

named_pipe_t named_pipes[MAX_NAMED_PIPES];
uint32_t pipe_count = 0;

uint32_t create_named_pipe(const char* name) {
    if (pipe_count >= MAX_NAMED_PIPES) return -1;
    
    uint32_t pipe_id = pipe_count;
    named_pipe_t* pipe = &named_pipes[pipe_id];
    
    pipe->pipe_id = pipe_id;
    for (int i = 0; i < 64 && name[i]; i++) {
        pipe->name[i] = name[i];
    }
    pipe->read_pos = 0;
    pipe->write_pos = 0;
    pipe->reader_pid = (uint32_t)-1;
    pipe->writer_pid = (uint32_t)-1;
    pipe->lock = 0;
    pipe_count++;
    
    return pipe_id;
}

int32_t write_to_pipe(uint32_t pipe_id, uint8_t* data, uint32_t size) {
    if (pipe_id >= pipe_count || size == 0) return -1;
    
    named_pipe_t* pipe = &named_pipes[pipe_id];
    
    acquire_spinlock(&pipe->lock);
    
    uint32_t available = PIPE_BUFFER_SIZE - (pipe->write_pos - pipe->read_pos);
    if (available < size) size = available;
    
    uint32_t pos = pipe->write_pos % PIPE_BUFFER_SIZE;
    
    if (pos + size <= PIPE_BUFFER_SIZE) {
        for (uint32_t i = 0; i < size; i++) {
            pipe->buffer[pos + i] = data[i];
        }
    } else {
        uint32_t first_part = PIPE_BUFFER_SIZE - pos;
        for (uint32_t i = 0; i < first_part; i++) {
            pipe->buffer[pos + i] = data[i];
        }
        for (uint32_t i = 0; i < size - first_part; i++) {
            pipe->buffer[i] = data[first_part + i];
        }
    }
    
    pipe->write_pos += size;
    
    release_spinlock(&pipe->lock);
    return size;
}

int32_t read_from_pipe(uint32_t pipe_id, uint8_t* buffer, uint32_t size) {
    if (pipe_id >= pipe_count) return -1;
    
    named_pipe_t* pipe = &named_pipes[pipe_id];
    
    acquire_spinlock(&pipe->lock);
    
    uint32_t available = pipe->write_pos - pipe->read_pos;
    if (available == 0) {
        release_spinlock(&pipe->lock);
        return 0;
    }
    
    if (size > available) size = available;
    
    uint32_t pos = pipe->read_pos % PIPE_BUFFER_SIZE;
    
    if (pos + size <= PIPE_BUFFER_SIZE) {
        for (uint32_t i = 0; i < size; i++) {
            buffer[i] = pipe->buffer[pos + i];
        }
    } else {
        uint32_t first_part = PIPE_BUFFER_SIZE - pos;
        for (uint32_t i = 0; i < first_part; i++) {
            buffer[i] = pipe->buffer[pos + i];
        }
        for (uint32_t i = 0; i < size - first_part; i++) {
            buffer[first_part + i] = pipe->buffer[i];
        }
    }
    
    pipe->read_pos += size;
    
    release_spinlock(&pipe->lock);
    return size;
}

// ============================
// MESSAGE QUEUE
// ============================

#define MAX_QUEUE_MESSAGES 256

typedef struct {
    uint32_t sender_pid;
    uint32_t receiver_pid;
    uint64_t timestamp;
    uint32_t msg_type;
    uint32_t data_size;
    uint8_t data[256];
} queue_message_t;

typedef struct {
    uint32_t queue_id;
    char name[64];
    queue_message_t messages[MAX_QUEUE_MESSAGES];
    uint32_t head;
    uint32_t tail;
    uint32_t count;
    spinlock_t lock;
} message_queue_t;

message_queue_t msg_queues[MAX_MESSAGE_QUEUES];
uint32_t queue_count = 0;

uint32_t create_message_queue(const char* name) {
    if (queue_count >= MAX_MESSAGE_QUEUES) return -1;
    
    uint32_t queue_id = queue_count;
    message_queue_t* queue = &msg_queues[queue_id];
    
    queue->queue_id = queue_id;
    for (int i = 0; i < 64 && name[i]; i++) {
        queue->name[i] = name[i];
    }
    queue->head = 0;
    queue->tail = 0;
    queue->count = 0;
    queue->lock = 0;
    queue_count++;
    
    return queue_id;
}

int32_t send_to_queue(uint32_t queue_id, uint32_t from_pid, uint32_t to_pid,
                      uint32_t msg_type, uint8_t* data, uint32_t size) {
    if (queue_id >= queue_count || size > 256) return -1;
    
    message_queue_t* queue = &msg_queues[queue_id];
    
    acquire_spinlock(&queue->lock);
    
    if (queue->count >= MAX_QUEUE_MESSAGES) {
        release_spinlock(&queue->lock);
        return -1;
    }
    
    queue_message_t* msg = &queue->messages[queue->tail];
    msg->sender_pid = from_pid;
    msg->receiver_pid = to_pid;
    msg->timestamp = ticks;
    msg->msg_type = msg_type;
    msg->data_size = size;
    
    for (uint32_t i = 0; i < size; i++) {
        msg->data[i] = data[i];
    }
    
    queue->tail = (queue->tail + 1) % MAX_QUEUE_MESSAGES;
    queue->count++;
    
    release_spinlock(&queue->lock);
    return 0;
}

int32_t receive_from_queue(uint32_t queue_id, queue_message_t* out_msg) {
    if (queue_id >= queue_count) return -1;
    
    message_queue_t* queue = &msg_queues[queue_id];
    
    acquire_spinlock(&queue->lock);
    
    if (queue->count == 0) {
        release_spinlock(&queue->lock);
        return -1;
    }
    
    *out_msg = queue->messages[queue->head];
    queue->head = (queue->head + 1) % MAX_QUEUE_MESSAGES;
    queue->count--;
    
    release_spinlock(&queue->lock);
    return 0;
}

// ============================
// BARRIER SYNCHRONIZATION
// ============================

typedef struct {
    uint32_t barrier_id;
    uint32_t total_tasks;
    uint32_t arrived_tasks;
    uint32_t waiting_tasks[MAX_TASKS];
    spinlock_t lock;
} barrier_t;

barrier_t barriers[MAX_BARRIERS];
uint32_t barrier_count = 0;

uint32_t create_barrier(uint32_t num_tasks) {
    if (barrier_count >= MAX_BARRIERS) return -1;
    
    uint32_t barrier_id = barrier_count;
    barrier_t* barrier = &barriers[barrier_id];
    
    barrier->barrier_id = barrier_id;
    barrier->total_tasks = num_tasks;
    barrier->arrived_tasks = 0;
    barrier->lock = 0;
    barrier_count++;
    
    return barrier_id;
}

int32_t wait_barrier(uint32_t barrier_id, uint32_t pid) {
    if (barrier_id >= barrier_count) return -1;
    
    barrier_t* barrier = &barriers[barrier_id];
    
    acquire_spinlock(&barrier->lock);
    
    barrier->arrived_tasks++;
    barrier->waiting_tasks[barrier->arrived_tasks - 1] = pid;
    
    if (barrier->arrived_tasks >= barrier->total_tasks) {
        barrier->arrived_tasks = 0;
        release_spinlock(&barrier->lock);
        return 0;
    }
    


// ============================
// TASK & CONTEXT SWITCHING
// ============================

typedef struct {
    uint64_t rax, rbx, rcx, rdx;
    uint64_t rsi, rdi, rbp;
    uint64_t r8, r9, r10, r11;
    uint64_t r12, r13, r14, r15;
    uint64_t rsp, rip;
    uint64_t rflags;
} registers_t;

typedef struct {
    uint32_t pid;
    uint32_t state;
    registers_t registers;
    uint64_t stack_base;
    uint32_t priority;
    uint64_t created_time;
    uint32_t cpu_ticks_used;
    uint8_t reserved_semaphores[MAX_SEMAPHORES / 8];
    uint8_t reserved_mutexes[MAX_MUTEXES / 8];
} task_t;

task_t task_table[MAX_TASKS];
uint32_t task_count = 0;
uint32_t current_pid = 0;

extern void context_switch(registers_t* old_regs, registers_t* new_regs);

// ============================
// TASK MANAGEMENT
// ============================

uint32_t create_task(uint64_t entry_point, uint32_t priority) {
    if (task_count >= MAX_TASKS) return -1;
    
    uint32_t pid = task_count;
    task_t* new_task = &task_table[task_count];
    task_count++;
    
    new_task->pid = pid;
    new_task->state = TASK_STATE_READY;
    new_task->priority = priority;
    new_task->created_time = ticks;
    new_task->cpu_ticks_used = 0;
    
    new_task->stack_base = 0x100000 + (pid * 0x2000);
    new_task->registers.rsp = new_task->stack_base + 0x2000;
    new_task->registers.rip = entry_point;
    new_task->registers.rflags = 0x200;
    
    for (int i = 0; i < MAX_SEMAPHORES / 8; i++) {
        new_task->reserved_semaphores[i] = 0;
    }
    for (int i = 0; i < MAX_MUTEXES / 8; i++) {
        new_task->reserved_mutexes[i] = 0;
    }
    
    return pid;
}

void terminate_task(uint32_t pid) {
    if (pid < task_count) {
        task_table[pid].state = TASK_STATE_TERMINATED;
    }
}

// ============================
// SCHEDULER
// ============================

uint32_t scheduler_counter = 0;
uint32_t schedule() {
    uint32_t best_pid = current_pid;
    uint32_t best_priority = 0;
    
    for (uint32_t i = 0; i < task_count; i++) {
        if (task_table[i].state == TASK_STATE_READY || 
            task_table[i].state == TASK_STATE_RUNNING) {
            if (task_table[i].priority > best_priority) {
                best_priority = task_table[i].priority;
                best_pid = i;
            }
        }
    }
    
    scheduler_counter++;
    if (scheduler_counter % 5 == 0 && best_priority > 0) {
        for (uint32_t i = best_pid + 1; i < task_count; i++) {
            if (task_table[i].state == TASK_STATE_READY && 
                task_table[i].priority == best_priority) {
                best_pid = i;
                break;
            }
        }
    }
    
    if (best_pid != current_pid) {
        task_t* old_task = &task_table[current_pid];
        task_t* new_task = &task_table[best_pid];
        
        old_task->state = TASK_STATE_READY;
        new_task->state = TASK_STATE_RUNNING;
        
        context_switch(&old_task->registers, &new_task->registers);
        current_pid = best_pid;
    }
    
    return best_pid;
}

// ============================
// IRQ HANDLER
// ============================

void irq_handler() {
    ticks++;
    
    task_t* current = &task_table[current_pid];
    current->cpu_ticks_used++;

    if (ticks % 10 == 0) {
        schedule();
    }

    outb(0x20, 0x20);
}

// ============================
// TEST TASKS
// ============================

void task_idle() {
    while (1) {
        __asm__ volatile ("hlt");
    }
}

void task_producer() {
    uint32_t queue_id = 0;
    uint32_t count = 0;
    
    while (1) {
        uint8_t data[16];
        data[0] = count++;
        send_to_queue(queue_id, 1, 2, 0, data, 16);
        
        for (volatile int i = 0; i < 500000; i++);
    }
}

void task_consumer() {
    uint32_t queue_id = 0;
    queue_message_t msg;
    
    while (1) {
        if (receive_from_queue(queue_id, &msg) == 0) {
            // Получили сообщение
        }
        
        for (volatile int i = 0; i < 500000; i++);
    }
}

void task_sync_test() {
    uint32_t barrier_id = 0;
    
    while (1) {
        int result = wait_barrier(barrier_id, current_pid);
        
        for (volatile int i = 0; i < 500000; i++);
    }
}

// ============================
// KERNEL MAIN
// ============================

void kernel_main() {
    // Инициализируем защиту памяти
    init_paging();
    
    // Переводим PIC в режим 8086 compat
    pic_remap();
    
    // Инициализируем IDT с обработчиками исключений
    // Сначала очищаем IDT
    for (int i = 0; i < IDT_SIZE; i++) {
        idt[i].type_attr = 0;
    }
    
    // Инициализируем обработчики исключений (Exception handlers)
    // #PF - Page Fault (INT 14)
    set_idt_gate(14, (uint64_t)page_fault_handler, 0x8E);
    
    // #GP - General Protection Fault (INT 13)
    set_idt_gate(13, (uint64_t)general_protection_fault_handler, 0x8E);
    
    // IRQ0 - Timer interrupt (INT 32)
    set_idt_gate(32, (uint64_t)timer_irq_handler, 0x8E);
    
    // Загружаем IDT в процессор
    load_idt();
    
    // Инициализируем таймер
    pit_init();
    
    // Создаем очередь сообщений
    create_message_queue("main_queue");
    
    // Создаем барьер для синхронизации (3 задачи)
    create_barrier(3);
    
    // Создаем семафор
    create_semaphore(1);
    
    // Создаем мьютекс
    create_mutex();
    
    // Создаем именованный pipe
    create_named_pipe("system_pipe");
    
    // Создаем задачи (PID 0 = idle, остальные = worker)
    create_task((uint64_t)task_idle, 1);
    task_table[0].state = TASK_STATE_RUNNING;
    current_pid = 0;
    
    create_task((uint64_t)task_producer, 128);
    create_task((uint64_t)task_consumer, 128);
    create_task((uint64_t)task_sync_test, 128);
    
    // Разрешаем прерывания
    __asm__ volatile ("sti");
    
    // Основной цикл ядра
    while (1) {
        __asm__ volatile ("hlt");
    }
}