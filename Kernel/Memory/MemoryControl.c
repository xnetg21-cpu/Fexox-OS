// ============================
// ADVANCED MEMORY CONTROL SYSTEM
// ============================

#include <stdint.h>
#include <string.h>

#define PAGE_SIZE 4096
#define MAX_MEMORY_REGIONS 512
#define MAX_PAGE_BLOCKS 4096
#define MAX_HEAP_BLOCKS 2048
#define MEMORY_ALIGNMENT 16

// Флаги памяти
#define MEM_FREE 0
#define MEM_ALLOCATED 1
#define MEM_RESERVED 2
#define MEM_CACHE 3
#define MEM_SWAPPED 4

// Структура блока памяти для heap allocation
typedef struct {
    uint64_t addr;
    uint64_t size;
    uint32_t flags;
    uint32_t owner_pid;
    uint32_t alloc_count;      // Количество аллокаций в этом блоке
    uint64_t created_time;
    uint64_t last_access;
} heap_block_t;

// Структура страницы в памяти
typedef struct {
    uint64_t phys_addr;        // Физический адрес
    uint64_t virt_addr;        // Виртуальный адрес
    uint32_t owner_pid;
    uint32_t flags;
    uint32_t access_count;     // Для отслеживания кэша
    uint64_t mapped_at;
} page_frame_t;

// Статистика памяти
typedef struct {
    uint64_t total_memory;
    uint64_t used_memory;
    uint64_t free_memory;
    uint64_t cached_memory;
    uint32_t total_pages;
    uint32_t used_pages;
    uint32_t free_pages;
    uint32_t fragmentation_percent;
    uint64_t total_allocations;
    uint64_t total_deallocations;
    uint64_t allocation_errors;
} memory_stats_t;

// Глобальные структуры
heap_block_t heap_blocks[MAX_HEAP_BLOCKS];
page_frame_t page_frames[MAX_PAGE_BLOCKS];
uint32_t heap_block_count = 0;
uint32_t page_frame_count = 0;

memory_stats_t mem_stats = {0};

volatile uint32_t memory_lock = 0;

// ============================
// MEMORY LOCKING (Simple Spinlock)
// ============================

static inline void acquire_memory_lock() {
    while (__sync_lock_test_and_set(&memory_lock, 1)) {
        __asm__ volatile ("pause");
    }
}

static inline void release_memory_lock() {
    __sync_lock_release(&memory_lock);
}

// ============================
// MEMORY INITIALIZATION
// ============================

void init_memory_system() {
    // Инициализируем heap
    for (uint32_t i = 0; i < MAX_HEAP_BLOCKS; i++) {
        heap_blocks[i].addr = 0;
        heap_blocks[i].size = 0;
        heap_blocks[i].flags = MEM_FREE;
        heap_blocks[i].owner_pid = 0;
    }
    
    // Инициализируем таблицу страниц
    for (uint32_t i = 0; i < MAX_PAGE_BLOCKS; i++) {
        page_frames[i].phys_addr = 0;
        page_frames[i].virt_addr = 0;
        page_frames[i].owner_pid = 0;
        page_frames[i].flags = MEM_FREE;
    }
    
    // Зарезервированная память: 0x0 - 0x100000 (ядро)
    heap_blocks[0].addr = 0x0;
    heap_blocks[0].size = 0x100000;
    heap_blocks[0].flags = MEM_RESERVED;
    heap_blocks[0].owner_pid = 0;
    heap_block_count = 1;
    
    // Heap: 0x100000 - 0x10000000 (256 MB)
    heap_blocks[1].addr = 0x100000;
    heap_blocks[1].size = 0x10000000 - 0x100000;
    heap_blocks[1].flags = MEM_FREE;
    heap_blocks[1].owner_pid = 0;
    heap_block_count = 2;
    
    // Кэш: 0x10000000 - 0x20000000 (256 MB)
    heap_blocks[2].addr = 0x10000000;
    heap_blocks[2].size = 0x10000000;
    heap_blocks[2].flags = MEM_CACHE;
    heap_blocks[2].owner_pid = 0;
    heap_block_count = 3;
    
    mem_stats.total_memory = 0x80000000;
    mem_stats.free_memory = mem_stats.total_memory;
    mem_stats.total_pages = MAX_PAGE_BLOCKS;
    mem_stats.free_pages = MAX_PAGE_BLOCKS;
}

// ============================
// HEAP ALLOCATION
// ============================

uint64_t malloc(uint32_t pid, uint64_t size) {
    if (size == 0 || size > 0x10000000) return 0;
    
    acquire_memory_lock();
    
    // Выравниваем размер
    uint64_t aligned_size = (size + MEMORY_ALIGNMENT - 1) & ~(MEMORY_ALIGNMENT - 1);
    
    // First-fit allocation
    for (uint32_t i = 0; i < heap_block_count; i++) {
        if (heap_blocks[i].flags == MEM_FREE && 
            heap_blocks[i].size >= aligned_size) {
            
            uint64_t allocated_addr = heap_blocks[i].addr;
            
            // Если блок больше - разбиваем его
            if (heap_blocks[i].size > aligned_size) {
                if (heap_block_count < MAX_HEAP_BLOCKS - 1) {
                    // Сдвигаем остальные блоки
                    for (uint32_t j = heap_block_count; j > i + 1; j--) {
                        heap_blocks[j] = heap_blocks[j - 1];
                    }
                    
                    // Создаем новый блок для остатка
                    heap_blocks[i + 1].addr = allocated_addr + aligned_size;
                    heap_blocks[i + 1].size = heap_blocks[i].size - aligned_size;
                    heap_blocks[i + 1].flags = MEM_FREE;
                    heap_blocks[i + 1].owner_pid = 0;
                    heap_block_count++;
                }
            }
            
            // Помечаем блок как выделенный
            heap_blocks[i].addr = allocated_addr;
            heap_blocks[i].size = aligned_size;
            heap_blocks[i].flags = MEM_ALLOCATED;
            heap_blocks[i].owner_pid = pid;
            heap_blocks[i].alloc_count = 1;
            heap_blocks[i].created_time = 0;
            heap_blocks[i].last_access = 0;
            
            mem_stats.used_memory += aligned_size;
            mem_stats.free_memory -= aligned_size;
            mem_stats.total_allocations++;
            
            release_memory_lock();
            return allocated_addr;
        }
    }
    
    mem_stats.allocation_errors++;
    release_memory_lock();
    return 0;  // Allocation failed
}

void free_memory_block(uint64_t addr) {
    if (addr == 0) return;
    
    acquire_memory_lock();
    
    // Находим блок
    uint32_t target_idx = -1;
    for (uint32_t i = 0; i < heap_block_count; i++) {
        if (heap_blocks[i].addr == addr && heap_blocks[i].flags == MEM_ALLOCATED) {
            target_idx = i;
            break;
        }
    }
    
    if (target_idx == -1) {
        release_memory_lock();
        return;  // Block not found
    }
    
    uint64_t freed_size = heap_blocks[target_idx].size;
    heap_blocks[target_idx].flags = MEM_FREE;
    heap_blocks[target_idx].owner_pid = 0;
    
    mem_stats.used_memory -= freed_size;
    mem_stats.free_memory += freed_size;
    mem_stats.total_deallocations++;
    
    // Объединяем соседние свободные блоки (Coalescing)
    for (uint32_t i = 0; i < heap_block_count - 1; i++) {
        if (heap_blocks[i].flags == MEM_FREE && 
            heap_blocks[i + 1].flags == MEM_FREE &&
            heap_blocks[i].addr + heap_blocks[i].size == heap_blocks[i + 1].addr) {
            
            heap_blocks[i].size += heap_blocks[i + 1].size;
            
            // Удаляем следующий блок
            for (uint32_t j = i + 1; j < heap_block_count - 1; j++) {
                heap_blocks[j] = heap_blocks[j + 1];
            }
            heap_block_count--;
            i--;
        }
    }
    
    release_memory_lock();
}

// ============================
// PAGE MANAGEMENT
// ============================

uint64_t allocate_page(uint32_t pid) {
    acquire_memory_lock();
    
    for (uint32_t i = 0; i < page_frame_count; i++) {
        if (page_frames[i].flags == MEM_FREE) {
            page_frames[i].phys_addr = i * PAGE_SIZE + 0x1000000;
            page_frames[i].owner_pid = pid;
            page_frames[i].flags = MEM_ALLOCATED;
            page_frames[i].access_count = 0;
            
            mem_stats.used_pages++;
            mem_stats.free_pages--;
            
            release_memory_lock();
            return page_frames[i].phys_addr;
        }
    }
    
    if (page_frame_count < MAX_PAGE_BLOCKS) {
        uint32_t idx = page_frame_count++;
        page_frames[idx].phys_addr = idx * PAGE_SIZE + 0x1000000;
        page_frames[idx].owner_pid = pid;
        page_frames[idx].flags = MEM_ALLOCATED;
        page_frames[idx].access_count = 0;
        
        mem_stats.used_pages++;
        mem_stats.free_pages--;
        
        release_memory_lock();
        return page_frames[idx].phys_addr;
    }
    
    release_memory_lock();
    return 0;  // No pages available
}

void free_page(uint64_t phys_addr) {
    acquire_memory_lock();
    
    for (uint32_t i = 0; i < page_frame_count; i++) {
        if (page_frames[i].phys_addr == phys_addr) {
            page_frames[i].flags = MEM_FREE;
            page_frames[i].owner_pid = 0;
            page_frames[i].access_count = 0;
            
            mem_stats.used_pages--;
            mem_stats.free_pages++;
            
            release_memory_lock();
            return;
        }
    }
    
    release_memory_lock();
}

// ============================
// MEMORY STATISTICS & DEBUGGING
// ============================

void calculate_fragmentation() {
    acquire_memory_lock();
    
    uint32_t free_blocks = 0;
    for (uint32_t i = 0; i < heap_block_count; i++) {
        if (heap_blocks[i].flags == MEM_FREE) free_blocks++;
    }
    
    mem_stats.fragmentation_percent = (free_blocks * 100) / heap_block_count;
    
    release_memory_lock();
}

memory_stats_t* get_memory_stats() {
    calculate_fragmentation();
    return &mem_stats;
}

void print_memory_stats() {
    memory_stats_t* stats = get_memory_stats();
    
    // В настоящей ОС здесь был бы вывод на экран
    // Для отладки используйте breakpoint или логирование
}

void defragment_heap() {
    acquire_memory_lock();
    
    // Simple defragmentation: переместить все выделенные блоки в начало
    heap_block_t temp[MAX_HEAP_BLOCKS];
    uint32_t allocated_count = 0;
    uint32_t free_size = 0;
    
    // Собираем выделенные блоки
    for (uint32_t i = 0; i < heap_block_count; i++) {
        if (heap_blocks[i].flags == MEM_ALLOCATED) {
            temp[allocated_count++] = heap_blocks[i];
        } else {
            free_size += heap_blocks[i].size;
        }
    }
    
    // Копируем выделенные блоки обратно
    uint32_t new_count = allocated_count;
    for (uint32_t i = 0; i < allocated_count; i++) {
        heap_blocks[i] = temp[i];
    }
    
    // Добавляем один большой свободный блок в конце
    if (new_count < MAX_HEAP_BLOCKS) {
        heap_blocks[new_count].addr = heap_blocks[new_count - 1].addr + 
                                       heap_blocks[new_count - 1].size;
        heap_blocks[new_count].size = free_size;
        heap_blocks[new_count].flags = MEM_FREE;
        new_count++;
    }
    
    heap_block_count = new_count;
    release_memory_lock();
}

// ============================
// PROCESS MEMORY CLEANUP
// ============================

void cleanup_process_memory(uint32_t pid) {
    acquire_memory_lock();
    
    // Освобождаем всю память процесса
    for (uint32_t i = 0; i < heap_block_count; i++) {
        if (heap_blocks[i].owner_pid == pid && heap_blocks[i].flags == MEM_ALLOCATED) {
            mem_stats.used_memory -= heap_blocks[i].size;
            mem_stats.free_memory += heap_blocks[i].size;
            heap_blocks[i].flags = MEM_FREE;
            heap_blocks[i].owner_pid = 0;
        }
    }
    
    // Освобождаем страницы процесса
    for (uint32_t i = 0; i < page_frame_count; i++) {
        if (page_frames[i].owner_pid == pid) {
            page_frames[i].flags = MEM_FREE;
            page_frames[i].owner_pid = 0;
            mem_stats.used_pages--;
            mem_stats.free_pages++;
        }
    }
    
    release_memory_lock();
}
