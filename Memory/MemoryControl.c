// ============================
// ADVANCED MEMORY CONTROL SYSTEM
// ============================

#include "MemoryControl.h"
#include <string.h>

#define PAGE_SIZE 0x1000ULL
#define MAX_MEMORY_REGIONS 128
#define MAX_HEAP_BLOCKS 4096
#define MAX_PHYSICAL_PAGES ((64ULL * 1024ULL * 1024ULL * 1024ULL) / PAGE_SIZE)
#define PHYS_BITMAP_WORDS (MAX_PHYSICAL_PAGES / 64ULL)

#define MEM_REGION_FREE     1
#define MEM_REGION_RESERVED 2
#define MEM_REGION_ACPI     3
#define MEM_REGION_NVS      4
#define MEM_REGION_BAD      5

#define MEM_FREE            0
#define MEM_ALLOCATED       1

typedef struct {
    uint64_t base;
    uint64_t size;
    uint32_t type;
} memory_region_t;

typedef struct {
    uint64_t addr;
    uint64_t size;
    uint32_t flags;
    uint32_t owner_pid;
    uint32_t alloc_count;
    uint64_t created_time;
    uint64_t last_access;
} heap_block_t;

static memory_region_t memory_regions[MAX_MEMORY_REGIONS];
static uint32_t memory_region_count = 0;
static heap_block_t heap_blocks[MAX_HEAP_BLOCKS];
static uint32_t heap_block_count = 0;
static uint64_t physical_page_bitmap[PHYS_BITMAP_WORDS];
static uint64_t total_physical_pages = 0;
static uint64_t total_memory_bytes = 0;
static volatile uint32_t memory_lock = 0;
static memory_stats_t mem_stats = {0};

static inline void acquire_memory_lock(void) {
    while (__sync_lock_test_and_set(&memory_lock, 1)) {
        __asm__ volatile ("pause");
    }
}

static inline void release_memory_lock(void) {
    __sync_lock_release(&memory_lock);
}

static inline uint64_t round_up(uint64_t value, uint64_t align) {
    if (align == 0) {
        return value;
    }
    return (value + align - 1) & ~(align - 1);
}

uint64_t round_up_to_page(uint64_t value) {
    return round_up(value, PAGE_SIZE);
}

uint64_t round_down_to_page(uint64_t value) {
    return value & ~(PAGE_SIZE - 1);
}

static inline uint64_t bytes_to_pages(uint64_t bytes) {
    return (bytes + PAGE_SIZE - 1) / PAGE_SIZE;
}

static inline uint64_t pages_to_bytes(uint64_t pages) {
    return pages * PAGE_SIZE;
}

static inline void set_page_bit(uint64_t page) {
    if (page >= total_physical_pages) {
        return;
    }
    physical_page_bitmap[page >> 6] |= 1ULL << (page & 63ULL);
}

static inline void clear_page_bit(uint64_t page) {
    if (page >= total_physical_pages) {
        return;
    }
    physical_page_bitmap[page >> 6] &= ~(1ULL << (page & 63ULL));
}

static inline int test_page_bit(uint64_t page) {
    if (page >= total_physical_pages) {
        return 1;
    }
    return (physical_page_bitmap[page >> 6] >> (page & 63ULL)) & 1ULL;
}

static void add_memory_region(uint64_t base, uint64_t size, uint32_t type) {
    if (memory_region_count >= MAX_MEMORY_REGIONS || size == 0) {
        return;
    }

    memory_regions[memory_region_count].base = base;
    memory_regions[memory_region_count].size = size;
    memory_regions[memory_region_count].type = type;
    memory_region_count++;
}

static uint64_t find_contiguous_free_pages(uint32_t count, uint32_t align_order) {
    if (count == 0 || total_physical_pages == 0) {
        return UINT64_MAX;
    }

    uint64_t align_pages = 1ULL << align_order;
    uint64_t max_page = total_physical_pages;
    uint64_t page = 0;

    while (page + count <= max_page) {
        if (align_pages > 1) {
            page = (page + align_pages - 1) & ~(align_pages - 1);
        }

        uint64_t end = page + count;
        if (end > max_page) {
            break;
        }

        uint64_t probe = page;
        while (probe < end && !test_page_bit(probe)) {
            probe++;
        }

        if (probe == end) {
            return page;
        }

        page = probe + 1;
    }

    return UINT64_MAX;
}

static void reserve_physical_pages(uint64_t page_base, uint32_t count) {
    for (uint32_t i = 0; i < count; i++) {
        if (!test_page_bit(page_base + i)) {
            set_page_bit(page_base + i);
        }
    }
}

static void release_physical_pages(uint64_t page_base, uint32_t count) {
    for (uint32_t i = 0; i < count; i++) {
        clear_page_bit(page_base + i);
    }
}

void init_memory_system(uint64_t total_memory_bytes_input) {
    acquire_memory_lock();

    uint64_t max_supported = 64ULL * 1024ULL * 1024ULL * 1024ULL;
    if (total_memory_bytes_input == 0) {
        total_memory_bytes_input = 4ULL * 1024ULL * 1024ULL * 1024ULL;
    }
    if (total_memory_bytes_input > max_supported) {
        total_memory_bytes_input = max_supported;
    }

    total_memory_bytes = round_down_to_page(total_memory_bytes_input);
    total_physical_pages = bytes_to_pages(total_memory_bytes);
    memset(physical_page_bitmap, 0, sizeof(physical_page_bitmap));
    memset(memory_regions, 0, sizeof(memory_regions));
    memset(heap_blocks, 0, sizeof(heap_blocks));

    memory_region_count = 0;
    heap_block_count = 0;

    add_memory_region(0, 0x100000ULL, MEM_REGION_RESERVED);
    add_memory_region(0x100000ULL, total_memory_bytes - 0x100000ULL, MEM_REGION_FREE);

    uint64_t reserved_pages = bytes_to_pages(0x100000ULL);
    reserve_physical_pages(0, reserved_pages);

    mem_stats.total_memory = total_memory_bytes;
    mem_stats.used_memory = 0;
    mem_stats.free_memory = total_memory_bytes - 0x100000ULL;
    mem_stats.total_pages = total_physical_pages;
    mem_stats.used_pages = reserved_pages;
    mem_stats.free_pages = total_physical_pages - reserved_pages;
    mem_stats.fragmentation_percent = 0;
    mem_stats.total_allocations = 0;
    mem_stats.total_deallocations = 0;
    mem_stats.allocation_errors = 0;
    mem_stats.page_allocations = 0;
    mem_stats.page_deallocations = 0;

    release_memory_lock();
}

uint64_t allocate_physical_pages(uint32_t pid, uint32_t page_count, uint32_t align_order) {
    if (page_count == 0 || page_count > total_physical_pages) {
        return 0;
    }

    acquire_memory_lock();

    uint64_t base_page = find_contiguous_free_pages(page_count, align_order);
    if (base_page == UINT64_MAX) {
        mem_stats.allocation_errors++;
        release_memory_lock();
        return 0;
    }

    reserve_physical_pages(base_page, page_count);
    uint64_t bytes = pages_to_bytes(page_count);
    mem_stats.used_pages += page_count;
    mem_stats.free_pages -= page_count;
    mem_stats.used_memory += bytes;
    mem_stats.free_memory -= bytes;
    mem_stats.page_allocations += page_count;

    release_memory_lock();
    return base_page * PAGE_SIZE;
}

uint64_t allocate_page(uint32_t pid) {
    return allocate_physical_pages(pid, 1, 0);
}

void free_page(uint64_t phys_addr) {
    if (phys_addr % PAGE_SIZE != 0) {
        return;
    }

    uint64_t page = phys_addr / PAGE_SIZE;
    if (page >= total_physical_pages) {
        return;
    }

    acquire_memory_lock();
    if (!test_page_bit(page)) {
        release_memory_lock();
        return;
    }

    clear_page_bit(page);
    mem_stats.used_pages--;
    mem_stats.free_pages++;
    mem_stats.used_memory -= PAGE_SIZE;
    mem_stats.free_memory += PAGE_SIZE;
    mem_stats.page_deallocations++;
    release_memory_lock();
}

uint64_t malloc(uint32_t pid, uint64_t size) {
    if (size == 0) {
        return 0;
    }

    uint64_t page_count = bytes_to_pages(size);
    uint64_t address = allocate_physical_pages(pid, (uint32_t)page_count, 0);
    if (address == 0) {
        return 0;
    }

    acquire_memory_lock();
    if (heap_block_count < MAX_HEAP_BLOCKS) {
        heap_blocks[heap_block_count].addr = address;
        heap_blocks[heap_block_count].size = pages_to_bytes(page_count);
        heap_blocks[heap_block_count].flags = MEM_ALLOCATED;
        heap_blocks[heap_block_count].owner_pid = pid;
        heap_blocks[heap_block_count].alloc_count = 1;
        heap_blocks[heap_block_count].created_time = 0;
        heap_blocks[heap_block_count].last_access = 0;
        heap_block_count++;
    }
    release_memory_lock();
    return address;
}

void free_memory_block(uint64_t addr) {
    if (addr == 0) {
        return;
    }

    acquire_memory_lock();
    for (uint32_t i = 0; i < heap_block_count; i++) {
        if (heap_blocks[i].addr == addr && heap_blocks[i].flags == MEM_ALLOCATED) {
            uint64_t page_count = bytes_to_pages(heap_blocks[i].size);
            uint64_t page_base = addr / PAGE_SIZE;
            release_physical_pages(page_base, (uint32_t)page_count);

            mem_stats.used_memory -= heap_blocks[i].size;
            mem_stats.free_memory += heap_blocks[i].size;
            mem_stats.total_deallocations++;

            heap_blocks[i].flags = MEM_FREE;
            heap_blocks[i].owner_pid = 0;
            heap_blocks[i].alloc_count = 0;
            heap_blocks[i].size = 0;
            heap_blocks[i].addr = 0;
            heap_blocks[i].created_time = 0;
            heap_blocks[i].last_access = 0;

            // Compact heap block array
            for (uint32_t j = i; j + 1 < heap_block_count; j++) {
                heap_blocks[j] = heap_blocks[j + 1];
            }
            if (heap_block_count > 0) {
                heap_block_count--;
            }
            release_memory_lock();
            return;
        }
    }
    mem_stats.allocation_errors++;
    release_memory_lock();
}

void kfree(uint64_t addr) {
    free_memory_block(addr);
}

void reserve_physical_region(uint64_t base_address, uint64_t size) {
    if (size == 0) {
        return;
    }

    uint64_t region_start = round_down_to_page(base_address);
    uint64_t region_end = round_up(base_address + size, PAGE_SIZE);
    if (region_end > total_memory_bytes) {
        region_end = total_memory_bytes;
    }

    uint64_t page_base = region_start / PAGE_SIZE;
    uint32_t page_count = (uint32_t)bytes_to_pages(region_end - region_start);

    acquire_memory_lock();
    add_memory_region(region_start, region_end - region_start, MEM_REGION_RESERVED);
    reserve_physical_pages(page_base, page_count);
    mem_stats.used_pages += page_count;
    mem_stats.free_pages -= page_count;
    mem_stats.used_memory += pages_to_bytes(page_count);
    mem_stats.free_memory -= pages_to_bytes(page_count);
    release_memory_lock();
}

uint64_t get_total_physical_memory(void) {
    return total_memory_bytes;
}

uint64_t get_used_physical_memory(void) {
    return mem_stats.used_memory;
}

uint64_t get_free_physical_memory(void) {
    return mem_stats.free_memory;
}

uint64_t get_total_pages(void) {
    return total_physical_pages;
}

uint64_t get_free_pages(void) {
    return mem_stats.free_pages;
}

static void calculate_fragmentation(void) {
    acquire_memory_lock();
    if (heap_block_count == 0) {
        mem_stats.fragmentation_percent = 0;
        release_memory_lock();
        return;
    }

    uint64_t free_blocks = 0;
    for (uint32_t i = 0; i < heap_block_count; i++) {
        if (heap_blocks[i].flags == MEM_FREE) {
            free_blocks++;
        }
    }

    mem_stats.fragmentation_percent = (uint32_t)((free_blocks * 100ULL) / heap_block_count);
    release_memory_lock();
}

memory_stats_t* get_memory_stats(void) {
    calculate_fragmentation();
    return &mem_stats;
}

void print_memory_stats(void) {
    memory_stats_t* stats = get_memory_stats();
    (void)stats;
}

void cleanup_process_memory(uint32_t pid) {
    acquire_memory_lock();

    for (uint32_t i = 0; i < heap_block_count;) {
        if (heap_blocks[i].owner_pid == pid && heap_blocks[i].flags == MEM_ALLOCATED) {
            uint64_t page_count = bytes_to_pages(heap_blocks[i].size);
            uint64_t page_base = heap_blocks[i].addr / PAGE_SIZE;
            release_physical_pages(page_base, (uint32_t)page_count);
            mem_stats.used_memory -= heap_blocks[i].size;
            mem_stats.free_memory += heap_blocks[i].size;
            mem_stats.total_deallocations++;

            for (uint32_t j = i; j + 1 < heap_block_count; j++) {
                heap_blocks[j] = heap_blocks[j + 1];
            }
            heap_block_count--;
            continue;
        }
        i++;
    }

    release_memory_lock();
}
