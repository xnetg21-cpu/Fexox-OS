#pragma once

#include <stdint.h>

// Memory management for up to 64 GB of physical RAM

typedef struct {
    uint64_t total_memory;
    uint64_t used_memory;
    uint64_t free_memory;
    uint64_t total_pages;
    uint64_t used_pages;
    uint64_t free_pages;
    uint32_t fragmentation_percent;
    uint64_t total_allocations;
    uint64_t total_deallocations;
    uint64_t allocation_errors;
    uint64_t page_allocations;
    uint64_t page_deallocations;
} memory_stats_t;

void init_memory_system(uint64_t total_memory_bytes);

uint64_t malloc(uint32_t pid, uint64_t size);
void kfree(uint64_t addr);
void free_memory_block(uint64_t addr);

uint64_t allocate_page(uint32_t pid);
uint64_t allocate_physical_pages(uint32_t pid, uint32_t page_count, uint32_t align_order);
void free_page(uint64_t phys_addr);

uint64_t get_total_physical_memory(void);
uint64_t get_used_physical_memory(void);
uint64_t get_free_physical_memory(void);
uint64_t get_total_pages(void);
uint64_t get_free_pages(void);
memory_stats_t* get_memory_stats(void);
void print_memory_stats(void);

void reserve_physical_region(uint64_t base_address, uint64_t size);
void cleanup_process_memory(uint32_t pid);

uint64_t round_up_to_page(uint64_t value);
uint64_t round_down_to_page(uint64_t value);
