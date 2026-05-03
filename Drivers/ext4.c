#include "ext4.h"
#include "../Memory/MemoryControl.h"
#include <string.h>

#define EXT4_SUPERBLOCK_OFFSET 1024ULL
#define EXT4_EXTENT_MAGIC 0xF30A
#define EXT4_DEFAULT_DESC_SIZE 32
#define EXT4_MAX_INLINE_EXTENTS 3

static uint32_t ext4_sector_count_for_blocks(ext4_fs_t *fs, uint32_t block_count) {
    return (block_count * fs->block_size) / 512U;
}

static uint32_t ext4_sector_offset_for_block(ext4_fs_t *fs, uint64_t block_number) {
    return (uint32_t)(fs->start_lba + (block_number * (fs->block_size / 512ULL)));
}

static uint32_t ext4_read_blocks(ext4_fs_t *fs, uint64_t block_number, uint32_t count, void *buffer) {
    if (!fs || !buffer || count == 0) {
        return 1;
    }

    unified_disk_request_t request;
    request.device_id = fs->device_id;
    request.lba = ext4_sector_offset_for_block(fs, block_number);
    request.block_count = ext4_sector_count_for_blocks(fs, count);
    request.buffer = buffer;
    request.timeout_ms = 5000;
    request.status = 0;
    return disk_read(&request);
}

static uint32_t ext4_write_blocks(ext4_fs_t *fs, uint64_t block_number, uint32_t count, const void *buffer) {
    if (!fs || !buffer || count == 0) {
        return 1;
    }

    unified_disk_request_t request;
    request.device_id = fs->device_id;
    request.lba = ext4_sector_offset_for_block(fs, block_number);
    request.block_count = ext4_sector_count_for_blocks(fs, count);
    request.buffer = (void *)buffer;
    request.timeout_ms = 5000;
    request.status = 0;
    return disk_write(&request);
}

static uint64_t ext4_superblock_block(ext4_fs_t *fs) {
    return fs->block_size == 1024 ? 1 : 0;
}

static uint64_t ext4_group_descriptor_block(ext4_fs_t *fs) {
    return fs->block_size == 1024 ? 2 : 1;
}

static uint32_t ext4_read_superblock(ext4_fs_t *fs) {
    if (!fs) {
        return 1;
    }

    uint8_t buffer[2048];
    uint32_t read_blocks = 2;
    unified_disk_request_t request;
    request.device_id = fs->device_id;
    request.lba = fs->start_lba;
    request.block_count = (read_blocks * 1024) / 512;
    request.buffer = buffer;
    request.timeout_ms = 5000;
    request.status = 0;
    if (disk_read(&request) != 0) {
        return 2;
    }

    uint8_t *sb_ptr = buffer + EXT4_SUPERBLOCK_OFFSET;
    memcpy(&fs->superblock, sb_ptr, sizeof(ext4_superblock_t));

    if (fs->superblock.s_magic != EXT4_SUPER_MAGIC) {
        return 3;
    }

    fs->block_size = 1024U << fs->superblock.s_log_block_size;
    fs->blocks_per_group = fs->superblock.s_blocks_per_group;
    fs->inodes_per_group = fs->superblock.s_inodes_per_group;
    fs->inode_size = fs->superblock.s_inode_size ? fs->superblock.s_inode_size : EXT4_GOOD_OLD_INODE_SIZE;
    fs->first_data_block = fs->superblock.s_first_data_block;
    fs->group_count = (fs->superblock.s_blocks_count_lo + fs->blocks_per_group - 1) / fs->blocks_per_group;

    if (fs->group_count == 0) {
        return 4;
    }

    uint32_t desc_size = fs->superblock.s_desc_size ? fs->superblock.s_desc_size : EXT4_DEFAULT_DESC_SIZE;
    uint64_t descriptor_bytes = (uint64_t)fs->group_count * desc_size;
    uint32_t descriptor_blocks = (descriptor_bytes + fs->block_size - 1) / fs->block_size;

    fs->group_descriptors = (ext4_group_desc_t *)malloc(0, descriptor_blocks * fs->block_size);
    if (!fs->group_descriptors) {
        return 5;
    }

    uint8_t *desc_buffer = (uint8_t *)malloc(0, descriptor_blocks * fs->block_size);
    if (!desc_buffer) {
        kfree((uint64_t)fs->group_descriptors);
        fs->group_descriptors = NULL;
        return 6;
    }

    if (ext4_read_blocks(fs, ext4_group_descriptor_block(fs), descriptor_blocks, desc_buffer) != 0) {
        kfree((uint64_t)fs->group_descriptors);
        kfree((uint64_t)desc_buffer);
        fs->group_descriptors = NULL;
        return 7;
    }

    for (uint32_t i = 0; i < fs->group_count; i++) {
        uint8_t *entry_ptr = desc_buffer + (uint64_t)i * desc_size;
        memcpy(&fs->group_descriptors[i], entry_ptr, sizeof(ext4_group_desc_t));
    }

    kfree((uint64_t)desc_buffer);

    if (fs->superblock.s_feature_compat & EXT4_FEATURE_COMPAT_HAS_JOURNAL) {
        ext4_enable_journal(fs);
    } else {
        fs->journal_active = 0;
    }

    return 0;
}

static uint32_t ext4_write_superblock(ext4_fs_t *fs) {
    if (!fs) {
        return 1;
    }

    uint8_t *buffer = (uint8_t *)malloc(0, fs->block_size);
    if (!buffer) {
        return 2;
    }

    memset(buffer, 0, fs->block_size);
    memcpy(buffer + EXT4_SUPERBLOCK_OFFSET, &fs->superblock, sizeof(ext4_superblock_t));
    uint32_t result = ext4_write_blocks(fs, ext4_superblock_block(fs), 1, buffer);
    kfree((uint64_t)buffer);
    return result;
}

static uint32_t ext4_write_group_descriptors(ext4_fs_t *fs) {
    if (!fs || !fs->group_descriptors) {
        return 1;
    }

    uint32_t desc_size = fs->superblock.s_desc_size ? fs->superblock.s_desc_size : EXT4_DEFAULT_DESC_SIZE;
    uint64_t descriptor_bytes = (uint64_t)fs->group_count * desc_size;
    uint32_t descriptor_blocks = (descriptor_bytes + fs->block_size - 1) / fs->block_size;

    uint8_t *buffer = (uint8_t *)malloc(0, descriptor_blocks * fs->block_size);
    if (!buffer) {
        return 2;
    }

    for (uint32_t i = 0; i < fs->group_count; i++) {
        memcpy(buffer + (uint64_t)i * desc_size, &fs->group_descriptors[i], sizeof(ext4_group_desc_t));
    }

    uint32_t result = ext4_write_blocks(fs, ext4_group_descriptor_block(fs), descriptor_blocks, buffer);
    kfree((uint64_t)buffer);
    return result;
}

static uint64_t ext4_inode_table_block(ext4_fs_t *fs, uint32_t group) {
    ext4_group_desc_t *desc = &fs->group_descriptors[group];
    uint64_t lo = desc->bg_inode_table_lo;
    uint64_t hi = desc->bg_inode_table_hi;
    return (hi << 32) | lo;
}

static uint64_t ext4_inode_size(ext4_fs_t *fs) {
    return fs->inode_size;
}

static uint32_t ext4_read_inode(ext4_fs_t *fs, uint32_t inode_number, ext4_inode_t *inode) {
    if (!fs || !inode || inode_number == 0 || inode_number > fs->superblock.s_inodes_count) {
        return 1;
    }

    uint32_t group = (inode_number - 1) / fs->inodes_per_group;
    uint32_t index = (inode_number - 1) % fs->inodes_per_group;
    uint64_t inode_table = ext4_inode_table_block(fs, group);
    uint64_t inode_offset = index * fs->inode_size;
    uint64_t block_index = inode_table + (inode_offset / fs->block_size);
    uint32_t block_offset = inode_offset % fs->block_size;

    uint8_t *buffer = (uint8_t *)malloc(0, fs->block_size);
    if (!buffer) {
        return 2;
    }

    if (ext4_read_blocks(fs, block_index, 1, buffer) != 0) {
        kfree((uint64_t)buffer);
        return 3;
    }

    memcpy(inode, buffer + block_offset, sizeof(ext4_inode_t));
    kfree((uint64_t)buffer);
    return 0;
}

static uint32_t ext4_write_inode(ext4_fs_t *fs, uint32_t inode_number, ext4_inode_t *inode) {
    if (!fs || !inode || inode_number == 0 || inode_number > fs->superblock.s_inodes_count) {
        return 1;
    }

    uint32_t group = (inode_number - 1) / fs->inodes_per_group;
    uint32_t index = (inode_number - 1) % fs->inodes_per_group;
    uint64_t inode_table = ext4_inode_table_block(fs, group);
    uint64_t inode_offset = index * fs->inode_size;
    uint64_t block_index = inode_table + (inode_offset / fs->block_size);
    uint32_t block_offset = inode_offset % fs->block_size;

    uint8_t *buffer = (uint8_t *)malloc(0, fs->block_size);
    if (!buffer) {
        return 2;
    }

    if (ext4_read_blocks(fs, block_index, 1, buffer) != 0) {
        kfree((uint64_t)buffer);
        return 3;
    }

    memcpy(buffer + block_offset, inode, sizeof(ext4_inode_t));
    uint32_t result = ext4_write_blocks(fs, block_index, 1, buffer);
    kfree((uint64_t)buffer);
    return result;
}

static uint64_t ext4_inode_physical_size(ext4_inode_t *inode) {
    uint64_t size = inode->i_size_lo;
    size |= ((uint64_t)inode->i_size_high << 32);
    return size;
}

uint64_t ext4_get_inode_size(ext4_inode_t *inode) {
    return ext4_inode_physical_size(inode);
}

static uint32_t ext4_is_directory(ext4_inode_t *inode) {
    return (inode->i_mode & EXT4_INODE_MODE_DIR) == EXT4_INODE_MODE_DIR;
}

static uint32_t ext4_is_regular_file(ext4_inode_t *inode) {
    return (inode->i_mode & EXT4_INODE_MODE_REG) == EXT4_INODE_MODE_REG;
}

static uint32_t ext4_is_symlink(ext4_inode_t *inode) {
    return (inode->i_mode & EXT4_INODE_MODE_SYMLINK) == EXT4_INODE_MODE_SYMLINK;
}

uint32_t ext4_get_file_type(ext4_inode_t *inode) {
    if (ext4_is_directory(inode)) {
        return EXT4_FILE_TYPE_DIR;
    }
    if (ext4_is_regular_file(inode)) {
        return EXT4_FILE_TYPE_REG_FILE;
    }
    if (ext4_is_symlink(inode)) {
        return EXT4_FILE_TYPE_SYMLINK;
    }
    return EXT4_FILE_TYPE_UNKNOWN;
}

static uint32_t ext4_read_bitmap_block(ext4_fs_t *fs, uint32_t group, uint8_t *buffer) {
    if (!fs || !buffer || group >= fs->group_count) {
        return 1;
    }

    uint64_t block_bitmap = fs->group_descriptors[group].bg_block_bitmap_lo;
    block_bitmap |= ((uint64_t)fs->group_descriptors[group].bg_block_bitmap_hi << 32);
    return ext4_read_blocks(fs, block_bitmap, 1, buffer);
}

static uint32_t ext4_write_bitmap_block(ext4_fs_t *fs, uint32_t group, uint8_t *buffer) {
    if (!fs || !buffer || group >= fs->group_count) {
        return 1;
    }

    uint64_t block_bitmap = fs->group_descriptors[group].bg_block_bitmap_lo;
    block_bitmap |= ((uint64_t)fs->group_descriptors[group].bg_block_bitmap_hi << 32);
    return ext4_write_blocks(fs, block_bitmap, 1, buffer);
}

static uint32_t ext4_read_inode_bitmap_block(ext4_fs_t *fs, uint32_t group, uint8_t *buffer) {
    if (!fs || !buffer || group >= fs->group_count) {
        return 1;
    }

    uint64_t inode_bitmap = fs->group_descriptors[group].bg_inode_bitmap_lo;
    inode_bitmap |= ((uint64_t)fs->group_descriptors[group].bg_inode_bitmap_hi << 32);
    return ext4_read_blocks(fs, inode_bitmap, 1, buffer);
}

static uint32_t ext4_write_inode_bitmap_block(ext4_fs_t *fs, uint32_t group, uint8_t *buffer) {
    if (!fs || !buffer || group >= fs->group_count) {
        return 1;
    }

    uint64_t inode_bitmap = fs->group_descriptors[group].bg_inode_bitmap_lo;
    inode_bitmap |= ((uint64_t)fs->group_descriptors[group].bg_inode_bitmap_hi << 32);
    return ext4_write_blocks(fs, inode_bitmap, 1, buffer);
}

static int ext4_find_free_bit(uint8_t *bitmap, uint32_t bits, uint32_t *index) {
    for (uint32_t i = 0; i < bits; i++) {
        uint32_t byte_index = i >> 3;
        uint8_t mask = 1U << (i & 7U);
        if (!(bitmap[byte_index] & mask)) {
            *index = i;
            return 0;
        }
    }
    return 1;
}

static void ext4_set_bitmap_bit(uint8_t *bitmap, uint32_t index) {
    bitmap[index >> 3] |= 1U << (index & 7U);
}

static void ext4_clear_bitmap_bit(uint8_t *bitmap, uint32_t index) {
    bitmap[index >> 3] &= ~(1U << (index & 7U));
}

static uint32_t ext4_update_group_counts(ext4_fs_t *fs, uint32_t group, int32_t block_delta, int32_t inode_delta) {
    if (group >= fs->group_count) {
        return 1;
    }
    ext4_group_desc_t *desc = &fs->group_descriptors[group];
    desc->bg_free_blocks_count_lo = (uint16_t)(desc->bg_free_blocks_count_lo + block_delta);
    desc->bg_free_inodes_count_lo = (uint16_t)(desc->bg_free_inodes_count_lo + inode_delta);
    fs->superblock.s_free_blocks_count_lo += block_delta;
    fs->superblock.s_free_inodes_count += inode_delta;
    if (block_delta < 0) {
        fs->superblock.s_free_blocks_count_hi -= 1;
    }
    return 0;
}

uint32_t ext4_allocate_inode(ext4_fs_t *fs, uint16_t mode, uint32_t *allocated_inode) {
    if (!fs || !allocated_inode) {
        return 1;
    }

    uint32_t bitmap_blocks = fs->block_size;
    uint8_t *bitmap = (uint8_t *)malloc(0, bitmap_blocks);
    if (!bitmap) {
        return 2;
    }

    for (uint32_t group = 0; group < fs->group_count; group++) {
        if (fs->group_descriptors[group].bg_free_inodes_count_lo == 0) {
            continue;
        }

        if (ext4_read_inode_bitmap_block(fs, group, bitmap) != 0) {
            continue;
        }

        uint32_t free_index;
        uint32_t bits = fs->inodes_per_group;
        if (ext4_find_free_bit(bitmap, bits, &free_index) == 0) {
            ext4_set_bitmap_bit(bitmap, free_index);
            if (ext4_write_inode_bitmap_block(fs, group, bitmap) != 0) {
                kfree((uint64_t)bitmap);
                return 3;
            }

            uint32_t inode_number = group * fs->inodes_per_group + free_index + 1;
            ext4_group_desc_t *desc = &fs->group_descriptors[group];
            desc->bg_free_inodes_count_lo -= 1;
            fs->superblock.s_free_inodes_count -= 1;

            ext4_inode_t new_inode;
            memset(&new_inode, 0, sizeof(new_inode));
            new_inode.i_mode = mode;
            new_inode.i_links_count = 1;
            new_inode.i_blocks_lo = 0;
            ext4_write_inode(fs, inode_number, &new_inode);

            *allocated_inode = inode_number;
            kfree((uint64_t)bitmap);
            return 0;
        }
    }

    kfree((uint64_t)bitmap);
    return 4;
}

uint32_t ext4_alloc_group_block(ext4_fs_t *fs, uint32_t group, uint32_t *block_index) {
    if (!fs || !block_index || group >= fs->group_count) {
        return 1;
    }

    uint32_t bits = fs->blocks_per_group;
    uint32_t bitmap_bytes = fs->block_size;
    uint8_t *bitmap = (uint8_t *)malloc(0, bitmap_bytes);
    if (!bitmap) {
        return 2;
    }

    if (ext4_read_bitmap_block(fs, group, bitmap) != 0) {
        kfree((uint64_t)bitmap);
        return 3;
    }

    uint32_t free_bit;
    if (ext4_find_free_bit(bitmap, bits, &free_bit) != 0) {
        kfree((uint64_t)bitmap);
        return 4;
    }

    ext4_set_bitmap_bit(bitmap, free_bit);
    if (ext4_write_bitmap_block(fs, group, bitmap) != 0) {
        kfree((uint64_t)bitmap);
        return 5;
    }

    ext4_group_desc_t *desc = &fs->group_descriptors[group];
    desc->bg_free_blocks_count_lo -= 1;
    fs->superblock.s_free_blocks_count_lo -= 1;
    fs->superblock.s_free_blocks_count_hi -= (desc->bg_free_blocks_count_lo == 0) ? 1 : 0;

    uint32_t first_block = fs->first_data_block + group * fs->blocks_per_group + free_bit;
    *block_index = first_block;

    kfree((uint64_t)bitmap);
    return 0;
}

uint32_t ext4_allocate_blocks(ext4_fs_t *fs, uint32_t count, uint32_t *first_block) {
    if (!fs || !first_block || count == 0) {
        return 1;
    }

    uint32_t contiguous_blocks = count;
    uint32_t candidate = 0;
    uint32_t best_group = UINT32_MAX;
    uint32_t found_block = 0;
    uint8_t *bitmap = (uint8_t *)malloc(0, fs->block_size);
    if (!bitmap) {
        return 2;
    }

    for (uint32_t group = 0; group < fs->group_count; group++) {
        if (fs->group_descriptors[group].bg_free_blocks_count_lo < count) {
            continue;
        }

        if (ext4_read_bitmap_block(fs, group, bitmap) != 0) {
            continue;
        }

        uint32_t group_base = fs->first_data_block + group * fs->blocks_per_group;
        uint32_t consecutive = 0;
        for (uint32_t bit = 0; bit < fs->blocks_per_group; bit++) {
            uint32_t byte_index = bit >> 3;
            uint8_t mask = 1U << (bit & 7U);
            if (!(bitmap[byte_index] & mask)) {
                if (consecutive == 0) {
                    candidate = bit;
                }
                consecutive++;
                if (consecutive == count) {
                    found_block = group_base + candidate;
                    best_group = group;
                    break;
                }
            } else {
                consecutive = 0;
            }
        }

        if (best_group != UINT32_MAX) {
            break;
        }
    }

    if (best_group == UINT32_MAX) {
        kfree((uint64_t)bitmap);
        return 3;
    }

    uint32_t group_start = found_block - (fs->first_data_block + best_group * fs->blocks_per_group);
    for (uint32_t i = 0; i < count; i++) {
        ext4_set_bitmap_bit(bitmap, group_start + i);
    }

    if (ext4_write_bitmap_block(fs, best_group, bitmap) != 0) {
        kfree((uint64_t)bitmap);
        return 4;
    }

    ext4_group_desc_t *desc = &fs->group_descriptors[best_group];
    desc->bg_free_blocks_count_lo -= count;
    fs->superblock.s_free_blocks_count_lo -= count;
    kfree((uint64_t)bitmap);
    *first_block = found_block;
    return 0;
}

uint32_t ext4_free_blocks(ext4_fs_t *fs, uint32_t first_block, uint32_t count) {
    if (!fs || count == 0) {
        return 1;
    }

    uint32_t relative = first_block - fs->first_data_block;
    uint32_t group = relative / fs->blocks_per_group;
    uint32_t index = relative % fs->blocks_per_group;
    if (group >= fs->group_count) {
        return 2;
    }

    uint8_t *bitmap = (uint8_t *)malloc(0, fs->block_size);
    if (!bitmap) {
        return 3;
    }

    if (ext4_read_bitmap_block(fs, group, bitmap) != 0) {
        kfree((uint64_t)bitmap);
        return 4;
    }

    for (uint32_t i = 0; i < count; i++) {
        ext4_clear_bitmap_bit(bitmap, index + i);
    }

    if (ext4_write_bitmap_block(fs, group, bitmap) != 0) {
        kfree((uint64_t)bitmap);
        return 5;
    }

    ext4_group_desc_t *desc = &fs->group_descriptors[group];
    desc->bg_free_blocks_count_lo += count;
    fs->superblock.s_free_blocks_count_lo += count;
    kfree((uint64_t)bitmap);
    return 0;
}

static uint32_t ext4_read_extent_block(ext4_fs_t *fs, uint64_t block, uint8_t *buffer) {
    return ext4_read_blocks(fs, block, 1, buffer);
}

static uint64_t ext4_extent_get_start(ext4_extent_t *extent) {
    uint64_t start_lo = extent->ee_start_lo;
    uint64_t start_hi = extent->ee_start_hi;
    return (start_hi << 32) | start_lo;
}

static uint32_t ext4_find_extent_in_leaf(ext4_extent_header_t *header, uint32_t logical_block, ext4_extent_t **out_extent) {
    ext4_extent_t *extent = (ext4_extent_t *)((uint8_t *)header + sizeof(ext4_extent_header_t));
    uint32_t count = header->eh_entries;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t start = extent[i].ee_block;
        uint32_t len = extent[i].ee_len;
        if (logical_block >= start && logical_block < start + len) {
            *out_extent = &extent[i];
            return 0;
        }
        if (logical_block < start) {
            return 1;
        }
    }
    return 2;
}

static uint32_t ext4_lookup_extent(ext4_fs_t *fs, ext4_inode_t *inode, uint32_t logical_block, uint64_t *physical_block) {
    if (!fs || !inode || !physical_block) {
        return 1;
    }

    ext4_extent_header_t *header = (ext4_extent_header_t *)inode->i_block;
    if (header->eh_magic != EXT4_EXTENT_MAGIC) {
        return 2;
    }

    uint8_t *node_buffer = (uint8_t *)malloc(0, fs->block_size);
    if (!node_buffer) {
        return 3;
    }

    uint64_t current_block = 0;
    ext4_extent_header_t *current_header = header;
    uint32_t depth = header->eh_depth;

    while (depth > 0) {
        ext4_extent_idx_t *index = (ext4_extent_idx_t *)((uint8_t *)current_header + sizeof(ext4_extent_header_t));
        uint32_t entries = current_header->eh_entries;
        uint32_t next_block = 0;
        for (uint32_t i = 0; i < entries; i++) {
            uint32_t start = index[i].ei_block;
            if (logical_block < start) {
                break;
            }
            next_block = index[i].ei_leaf_lo | ((uint64_t)index[i].ei_leaf_hi << 32);
            if (i + 1 == entries || logical_block < index[i + 1].ei_block) {
                break;
            }
        }

        if (next_block == 0) {
            kfree((uint64_t)node_buffer);
            return 4;
        }

        if (ext4_read_extent_block(fs, next_block, node_buffer) != 0) {
            kfree((uint64_t)node_buffer);
            return 5;
        }

        current_header = (ext4_extent_header_t *)node_buffer;
        depth = current_header->eh_depth;
    }

    ext4_extent_t *found_extent;
    uint32_t result = ext4_find_extent_in_leaf(current_header, logical_block, &found_extent);
    if (result != 0) {
        kfree((uint64_t)node_buffer);
        return 6;
    }

    uint64_t start = ext4_extent_get_start(found_extent);
    *physical_block = start + (logical_block - found_extent->ee_block);
    kfree((uint64_t)node_buffer);
    return 0;
}

static uint32_t ext4_read_block_pointer(ext4_fs_t *fs, uint32_t block, uint32_t index, uint32_t *pointer) {
    uint8_t *buffer = (uint8_t *)malloc(0, fs->block_size);
    if (!buffer) {
        return 1;
    }

    if (ext4_read_blocks(fs, block, 1, buffer) != 0) {
        kfree((uint64_t)buffer);
        return 2;
    }

    uint32_t *entries = (uint32_t *)buffer;
    *pointer = entries[index];
    kfree((uint64_t)buffer);
    return 0;
}

static uint32_t ext4_map_logical_block(ext4_fs_t *fs, ext4_inode_t *inode, uint32_t logical_block, uint64_t *physical_block) {
    if (!fs || !inode || !physical_block) {
        return 1;
    }

    if (inode->i_flags & EXT4_EXTENTS_FL) {
        return ext4_lookup_extent(fs, inode, logical_block, physical_block);
    }

    if (logical_block < EXT4_NDIR_BLOCKS) {
        *physical_block = inode->i_block[logical_block];
        return *physical_block ? 0 : 2;
    }

    uint32_t blocks_per_block = fs->block_size / sizeof(uint32_t);
    uint32_t pointer_offset = logical_block - EXT4_NDIR_BLOCKS;
    uint32_t single_threshold = blocks_per_block;
    uint32_t double_threshold = blocks_per_block * blocks_per_block + single_threshold;
    uint32_t pointer_block;

    if (pointer_offset < single_threshold) {
        pointer_block = inode->i_block[EXT4_NDIR_BLOCKS];
        if (pointer_block == 0) {
            return 2;
        }
        return ext4_read_block_pointer(fs, pointer_block, pointer_offset, (uint32_t *)physical_block);
    }

    if (pointer_offset < double_threshold) {
        uint32_t first_index = pointer_offset / blocks_per_block;
        uint32_t second_index = pointer_offset % blocks_per_block;
        if (inode->i_block[EXT4_NDIR_BLOCKS + 1] == 0) {
            return 2;
        }
        if (ext4_read_block_pointer(fs, inode->i_block[EXT4_NDIR_BLOCKS + 1], first_index, &pointer_block) != 0) {
            return 3;
        }
        return ext4_read_block_pointer(fs, pointer_block, second_index, (uint32_t *)physical_block);
    }

    if (pointer_offset < double_threshold + blocks_per_block * blocks_per_block * blocks_per_block) {
        uint32_t first_index = (pointer_offset - double_threshold) / (blocks_per_block * blocks_per_block);
        uint32_t second_index = ((pointer_offset - double_threshold) / blocks_per_block) % blocks_per_block;
        uint32_t third_index = (pointer_offset - double_threshold) % blocks_per_block;
        if (inode->i_block[EXT4_NDIR_BLOCKS + 2] == 0) {
            return 2;
        }
        if (ext4_read_block_pointer(fs, inode->i_block[EXT4_NDIR_BLOCKS + 2], first_index, &pointer_block) != 0) {
            return 3;
        }
        if (ext4_read_block_pointer(fs, pointer_block, second_index, &pointer_block) != 0) {
            return 4;
        }
        return ext4_read_block_pointer(fs, pointer_block, third_index, (uint32_t *)physical_block);
    }

    return 5;
}

static uint32_t ext4_read_inode_range(ext4_fs_t *fs, ext4_inode_t *inode, uint64_t offset, uint64_t count, void *buffer, uint64_t *read_bytes) {
    if (!fs || !inode || !buffer || !read_bytes) {
        return 1;
    }

    uint64_t file_size = ext4_inode_physical_size(inode);
    if (offset >= file_size) {
        *read_bytes = 0;
        return 0;
    }

    if (offset + count > file_size) {
        count = file_size - offset;
    }

    uint64_t block_offset = offset / fs->block_size;
    uint32_t inner_offset = offset % fs->block_size;
    uint64_t remaining = count;
    uint8_t *out_ptr = (uint8_t *)buffer;
    uint8_t *block_buffer = (uint8_t *)malloc(0, fs->block_size);
    if (!block_buffer) {
        return 2;
    }

    while (remaining > 0) {
        uint64_t phys_block;
        if (ext4_map_logical_block(fs, inode, (uint32_t)block_offset, &phys_block) != 0) {
            kfree((uint64_t)block_buffer);
            return 3;
        }

        if (ext4_read_blocks(fs, phys_block, 1, block_buffer) != 0) {
            kfree((uint64_t)block_buffer);
            return 4;
        }

        uint64_t chunk = fs->block_size - inner_offset;
        if (chunk > remaining) {
            chunk = remaining;
        }

        memcpy(out_ptr, block_buffer + inner_offset, (size_t)chunk);
        out_ptr += chunk;
        remaining -= chunk;
        block_offset++;
        inner_offset = 0;
    }

    *read_bytes = count;
    kfree((uint64_t)block_buffer);
    return 0;
}

static uint32_t ext4_write_inode_range(ext4_fs_t *fs, ext4_inode_t *inode, uint64_t offset, uint64_t count, const void *buffer, uint64_t *written_bytes) {
    if (!fs || !inode || !buffer || !written_bytes) {
        return 1;
    }

    uint64_t end_offset = offset + count;
    uint64_t block_offset = offset / fs->block_size;
    uint32_t inner_offset = offset % fs->block_size;
    uint64_t remaining = count;
    const uint8_t *src_ptr = (const uint8_t *)buffer;
    uint8_t *write_buffer = (uint8_t *)malloc(0, fs->block_size);
    if (!write_buffer) {
        return 2;
    }

    while (remaining > 0) {
        uint64_t phys_block;
        if (ext4_map_logical_block(fs, inode, (uint32_t)block_offset, &phys_block) != 0) {
            uint32_t allocated;
            if (ext4_allocate_blocks(fs, 1, &allocated) != 0) {
                kfree((uint64_t)write_buffer);
                return 3;
            }
            phys_block = allocated;
            if (block_offset < EXT4_NDIR_BLOCKS && !(inode->i_flags & EXT4_EXTENTS_FL)) {
                inode->i_block[block_offset] = (uint32_t)phys_block;
            } else {
                if (!(inode->i_flags & EXT4_EXTENTS_FL)) {
                    inode->i_flags |= EXT4_EXTENTS_FL;
                    memset(inode->i_block, 0, sizeof(inode->i_block));
                    ext4_extent_header_t *new_header = (ext4_extent_header_t *)inode->i_block;
                    new_header->eh_magic = EXT4_EXTENT_MAGIC;
                    new_header->eh_entries = 0;
                    new_header->eh_max = (fs->block_size - sizeof(ext4_extent_header_t) - sizeof(uint32_t)) / sizeof(ext4_extent_t);
                    new_header->eh_depth = 0;
                    new_header->eh_generation = 1;
                }
                ext4_extent_header_t *header = (ext4_extent_header_t *)inode->i_block;
                ext4_extent_t *extent = (ext4_extent_t *)((uint8_t *)header + sizeof(ext4_extent_header_t));
                if (header->eh_entries < header->eh_max) {
                    ext4_extent_t *entry = &extent[header->eh_entries];
                    entry->ee_block = (uint32_t)block_offset;
                    entry->ee_len = 1;
                    entry->ee_start_lo = (uint32_t)phys_block;
                    entry->ee_start_hi = (uint16_t)(phys_block >> 32);
                    header->eh_entries++;
                } else {
                    kfree((uint64_t)write_buffer);
                    return 4;
                }
            }
        }

        uint64_t chunk = fs->block_size - inner_offset;
        if (chunk > remaining) {
            chunk = remaining;
        }

        if (inner_offset != 0 || chunk != fs->block_size) {
            if (ext4_read_blocks(fs, phys_block, 1, write_buffer) != 0) {
                kfree((uint64_t)write_buffer);
                return 5;
            }
        }

        memcpy(write_buffer + inner_offset, src_ptr, (size_t)chunk);
        if (ext4_write_blocks(fs, phys_block, 1, write_buffer) != 0) {
            kfree((uint64_t)write_buffer);
            return 6;
        }

        src_ptr += chunk;
        remaining -= chunk;
        block_offset++;
        inner_offset = 0;
    }

    if (end_offset > ext4_inode_physical_size(inode)) {
        inode->i_size_lo = (uint32_t)end_offset;
        inode->i_size_high = (uint32_t)(end_offset >> 32);
    }

    uint32_t block_count = (uint32_t)bytes_to_pages(end_offset);
    inode->i_blocks_lo = block_count * (fs->block_size / 512);
    *written_bytes = count;
    kfree((uint64_t)write_buffer);
    return 0;
}

static uint32_t ext4_find_dir_entry(ext4_fs_t *fs, ext4_inode_t *directory, const char *name, uint32_t *inode_number) {
    if (!fs || !directory || !name || !inode_number) {
        return 1;
    }

    uint64_t dir_size = ext4_inode_physical_size(directory);
    uint64_t offset = 0;
    uint8_t *block_buffer = (uint8_t *)malloc(0, fs->block_size);
    if (!block_buffer) {
        return 2;
    }

    while (offset < dir_size) {
        uint64_t phys_block;
        if (ext4_map_logical_block(fs, directory, (uint32_t)(offset / fs->block_size), &phys_block) != 0) {
            kfree((uint64_t)block_buffer);
            return 3;
        }
        if (ext4_read_blocks(fs, phys_block, 1, block_buffer) != 0) {
            kfree((uint64_t)block_buffer);
            return 4;
        }

        uint32_t block_offset = 0;
        while (block_offset < fs->block_size) {
            ext4_dir_entry_t *entry = (ext4_dir_entry_t *)(block_buffer + block_offset);
            if (entry->inode != 0 && entry->name_len != 0) {
                char entry_name[EXT4_NAME_LEN_MAX + 1];
                memcpy(entry_name, entry->name, entry->name_len);
                entry_name[entry->name_len] = '\0';
                if (strcmp(entry_name, name) == 0) {
                    *inode_number = entry->inode;
                    kfree((uint64_t)block_buffer);
                    return 0;
                }
            }
            if (entry->rec_len == 0) {
                break;
            }
            block_offset += entry->rec_len;
        }
        offset += fs->block_size;
    }

    kfree((uint64_t)block_buffer);
    return 5;
}

static uint32_t ext4_insert_dir_entry(ext4_fs_t *fs, ext4_inode_t *directory, uint32_t dir_inode_number, uint32_t inode_number, const char *name, uint8_t file_type) {
    if (!fs || !directory || !name) {
        return 1;
    }

    uint64_t dir_size = ext4_inode_physical_size(directory);
    uint64_t offset = 0;
    uint8_t *block_buffer = (uint8_t *)malloc(0, fs->block_size);
    if (!block_buffer) {
        return 2;
    }

    uint32_t name_len = (uint32_t)strlen(name);
    uint16_t entry_size = (uint16_t)((8 + name_len + 3) & ~3);

    if (dir_size == 0) {
        uint32_t allocated;
        if (ext4_allocate_blocks(fs, 1, &allocated) != 0) {
            kfree((uint64_t)block_buffer);
            return 3;
        }
        if (directory->i_flags & EXT4_EXTENTS_FL) {
            ext4_extent_header_t *header = (ext4_extent_header_t *)directory->i_block;
            header->eh_magic = EXT4_EXTENT_MAGIC;
            header->eh_entries = 1;
            header->eh_max = (fs->block_size - sizeof(ext4_extent_header_t) - sizeof(uint32_t)) / sizeof(ext4_extent_t);
            header->eh_depth = 0;
            header->eh_generation = 1;
            ext4_extent_t *extent = (ext4_extent_t *)((uint8_t *)header + sizeof(ext4_extent_header_t));
            extent->ee_block = 0;
            extent->ee_len = 1;
            extent->ee_start_lo = (uint32_t)allocated;
            extent->ee_start_hi = (uint16_t)(allocated >> 32);
        } else {
            directory->i_block[0] = allocated;
        }
        directory->i_size_lo = fs->block_size;
        dir_size = fs->block_size;
    }

    while (offset < dir_size) {
        uint64_t phys_block;
        if (ext4_map_logical_block(fs, directory, (uint32_t)(offset / fs->block_size), &phys_block) != 0) {
            uint32_t allocated;
            if (ext4_allocate_blocks(fs, 1, &allocated) != 0) {
                kfree((uint64_t)block_buffer);
                return 3;
            }
            phys_block = allocated;
            if (ext4_is_directory(directory)) {
                if (directory->i_flags & EXT4_EXTENTS_FL) {
                    ext4_extent_header_t *header = (ext4_extent_header_t *)directory->i_block;
                    ext4_extent_t *extent = (ext4_extent_t *)((uint8_t *)header + sizeof(ext4_extent_header_t));
                    if (header->eh_entries < header->eh_max) {
                        extent[header->eh_entries].ee_block = (uint32_t)(offset / fs->block_size);
                        extent[header->eh_entries].ee_len = 1;
                        extent[header->eh_entries].ee_start_lo = (uint32_t)phys_block;
                        extent[header->eh_entries].ee_start_hi = (uint16_t)(phys_block >> 32);
                        header->eh_entries++;
                    }
                } else {
                    directory->i_block[offset / fs->block_size] = (uint32_t)phys_block;
                }
                directory->i_size_lo += fs->block_size;
            }
        }

        if (ext4_read_blocks(fs, phys_block, 1, block_buffer) != 0) {
            kfree((uint64_t)block_buffer);
            return 4;
        }

        uint32_t block_offset = 0;
        while (block_offset < fs->block_size) {
            ext4_dir_entry_t *entry = (ext4_dir_entry_t *)(block_buffer + block_offset);
            if (entry->rec_len == 0) {
                break;
            }
            uint16_t actual_size = (uint16_t)((8 + entry->name_len + 3) & ~3);
            uint16_t free_space = entry->rec_len - actual_size;
            if (entry->inode && free_space >= entry_size) {
                entry->rec_len = actual_size;
                uint16_t next_offset = block_offset + actual_size;
                ext4_dir_entry_t *new_entry = (ext4_dir_entry_t *)(block_buffer + next_offset);
                new_entry->inode = inode_number;
                new_entry->rec_len = free_space;
                new_entry->name_len = (uint8_t)name_len;
                new_entry->file_type = file_type;
                memcpy(new_entry->name, name, name_len);
                if (ext4_write_blocks(fs, phys_block, 1, block_buffer) != 0) {
                    kfree((uint64_t)block_buffer);
                    return 5;
                }
                kfree((uint64_t)block_buffer);
                return 0;
            }
            block_offset += entry->rec_len;
        }

        if (fs->block_size - block_offset >= entry_size) {
            ext4_dir_entry_t *entry = (ext4_dir_entry_t *)(block_buffer + block_offset);
            entry->inode = inode_number;
            entry->rec_len = fs->block_size - block_offset;
            entry->name_len = (uint8_t)name_len;
            entry->file_type = file_type;
            memcpy(entry->name, name, name_len);
            if (ext4_write_blocks(fs, phys_block, 1, block_buffer) != 0) {
                kfree((uint64_t)block_buffer);
                return 6;
            }
            kfree((uint64_t)block_buffer);
            return 0;
        }

        offset += fs->block_size;
    }

    kfree((uint64_t)block_buffer);
    return 7;
}

static uint32_t ext4_remove_dir_entry(ext4_fs_t *fs, ext4_inode_t *directory, const char *name) {
    if (!fs || !directory || !name) {
        return 1;
    }

    uint64_t dir_size = ext4_inode_physical_size(directory);
    uint64_t offset = 0;
    uint8_t *block_buffer = (uint8_t *)malloc(0, fs->block_size);
    if (!block_buffer) {
        return 2;
    }

    while (offset < dir_size) {
        uint64_t phys_block;
        if (ext4_map_logical_block(fs, directory, (uint32_t)(offset / fs->block_size), &phys_block) != 0) {
            kfree((uint64_t)block_buffer);
            return 3;
        }
        if (ext4_read_blocks(fs, phys_block, 1, block_buffer) != 0) {
            kfree((uint64_t)block_buffer);
            return 4;
        }

        uint32_t block_offset = 0;
        uint32_t prev_offset = 0;
        ext4_dir_entry_t *prev_entry = NULL;

        while (block_offset < fs->block_size) {
            ext4_dir_entry_t *entry = (ext4_dir_entry_t *)(block_buffer + block_offset);
            if (entry->rec_len == 0) {
                break;
            }
            char entry_name[EXT4_NAME_LEN_MAX + 1];
            memcpy(entry_name, entry->name, entry->name_len);
            entry_name[entry->name_len] = '\0';
            if (entry->inode && strcmp(entry_name, name) == 0) {
                if (prev_entry) {
                    prev_entry->rec_len += entry->rec_len;
                } else {
                    entry->inode = 0;
                }
                if (ext4_write_blocks(fs, phys_block, 1, block_buffer) != 0) {
                    kfree((uint64_t)block_buffer);
                    return 5;
                }
                kfree((uint64_t)block_buffer);
                return 0;
            }
            prev_entry = entry;
            prev_offset = block_offset;
            block_offset += entry->rec_len;
        }
        offset += fs->block_size;
    }

    kfree((uint64_t)block_buffer);
    return 6;
}

static uint32_t ext4_read_directory(ext4_fs_t *fs, ext4_inode_t *directory, uint32_t (*entry_callback)(ext4_dir_entry_t *, void *), void *context) {
    if (!fs || !directory || !entry_callback) {
        return 1;
    }

    uint64_t dir_size = ext4_inode_physical_size(directory);
    uint64_t offset = 0;
    uint8_t *block_buffer = (uint8_t *)malloc(0, fs->block_size);
    if (!block_buffer) {
        return 2;
    }

    while (offset < dir_size) {
        uint64_t phys_block;
        if (ext4_map_logical_block(fs, directory, (uint32_t)(offset / fs->block_size), &phys_block) != 0) {
            kfree((uint64_t)block_buffer);
            return 3;
        }
        if (ext4_read_blocks(fs, phys_block, 1, block_buffer) != 0) {
            kfree((uint64_t)block_buffer);
            return 4;
        }

        uint32_t block_offset = 0;
        while (block_offset < fs->block_size) {
            ext4_dir_entry_t *entry = (ext4_dir_entry_t *)(block_buffer + block_offset);
            if (entry->inode == 0 || entry->rec_len == 0) {
                break;
            }
            if (entry_callback(entry, context)) {
                kfree((uint64_t)block_buffer);
                return 0;
            }
            block_offset += entry->rec_len;
        }
        offset += fs->block_size;
    }

    kfree((uint64_t)block_buffer);
    return 0;
}

static uint32_t ext4_tokenize_path(const char *path, char components[][EXT4_NAME_LEN_MAX + 1], uint32_t *count) {
    if (!path || !count) {
        return 1;
    }

    uint32_t index = 0;
    const char *start = path;
    while (*start == '/') {
        start++;
    }

    while (*start != '\0' && index < 64) {
        const char *end = start;
        while (*end != '/' && *end != '\0') {
            end++;
        }
        uint32_t len = (uint32_t)(end - start);
        if (len > EXT4_NAME_LEN_MAX) {
            return 2;
        }
        memcpy(components[index], start, len);
        components[index][len] = '\0';
        index++;
        while (*end == '/') {
            end++;
        }
        start = end;
    }

    *count = index;
    return 0;
}

uint32_t ext4_resolve_path(ext4_fs_t *fs, const char *path, uint32_t *inode_number) {
    if (!fs || !path || !inode_number) {
        return 1;
    }
    if (path[0] == '\0') {
        return 2;
    }

    char components[64][EXT4_NAME_LEN_MAX + 1];
    uint32_t component_count = 0;
    if (ext4_tokenize_path(path, components, &component_count) != 0) {
        return 3;
    }

    uint32_t current_inode = EXT4_ROOT_INODE;
    ext4_inode_t inode;

    for (uint32_t i = 0; i < component_count; i++) {
        if (ext4_read_inode(fs, current_inode, &inode) != 0) {
            return 4;
        }
        if (!ext4_is_directory(&inode)) {
            return 5;
        }

        if (ext4_find_dir_entry(fs, &inode, components[i], &current_inode) != 0) {
            return 6;
        }
    }

    *inode_number = current_inode;
    return 0;
}

typedef struct {
    void (*user_callback)(const char *, uint32_t, void *);
    void *user_context;
} ext4_list_context_t;

static uint32_t ext4_list_directory_callback(ext4_dir_entry_t *entry, void *ctx) {
    ext4_list_context_t *context = (ext4_list_context_t *)ctx;
    if (!entry || !context || !context->user_callback) {
        return 1;
    }

    char name[EXT4_NAME_LEN_MAX + 1];
    memcpy(name, entry->name, entry->name_len);
    name[entry->name_len] = '\0';
    context->user_callback(name, entry->inode, context->user_context);
    return 0;
}

uint32_t ext4_list_directory(ext4_fs_t *fs, const char *path, void (*callback)(const char *, uint32_t, void *), void *context) {
    if (!fs || !path || !callback) {
        return 1;
    }

    uint32_t inode_number;
    if (ext4_resolve_path(fs, path, &inode_number) != 0) {
        return 2;
    }

    ext4_inode_t inode;
    if (ext4_read_inode(fs, inode_number, &inode) != 0) {
        return 3;
    }

    if (!ext4_is_directory(&inode)) {
        return 4;
    }

    ext4_list_context_t list_context;
    list_context.user_callback = callback;
    list_context.user_context = context;

    return ext4_read_directory(fs, &inode, ext4_list_directory_callback, &list_context);
}

uint32_t ext4_read_file(ext4_fs_t *fs, const char *path, void *buffer, uint64_t offset, uint64_t count, uint64_t *read_bytes) {
    if (!fs || !path || !buffer || !read_bytes) {
        return 1;
    }

    uint32_t inode_number;
    if (ext4_resolve_path(fs, path, &inode_number) != 0) {
        return 2;
    }

    ext4_inode_t inode;
    if (ext4_read_inode(fs, inode_number, &inode) != 0) {
        return 3;
    }

    if (!ext4_is_regular_file(&inode) && !ext4_is_symlink(&inode)) {
        return 4;
    }

    return ext4_read_inode_range(fs, &inode, offset, count, buffer, read_bytes);
}

uint32_t ext4_write_file(ext4_fs_t *fs, const char *path, const void *buffer, uint64_t offset, uint64_t count, uint64_t *written_bytes) {
    if (!fs || !path || !buffer || !written_bytes) {
        return 1;
    }

    uint32_t inode_number;
    if (ext4_resolve_path(fs, path, &inode_number) != 0) {
        return 2;
    }

    ext4_inode_t inode;
    if (ext4_read_inode(fs, inode_number, &inode) != 0) {
        return 3;
    }

    if (!ext4_is_regular_file(&inode)) {
        return 4;
    }

    uint32_t result = ext4_write_inode_range(fs, &inode, offset, count, buffer, written_bytes);
    if (result != 0) {
        return result;
    }

    return ext4_write_inode(fs, inode_number, &inode);
}

uint32_t ext4_create_file(ext4_fs_t *fs, const char *path, uint16_t mode) {
    if (!fs || !path || mode == 0) {
        return 1;
    }

    char components[64][EXT4_NAME_LEN_MAX + 1];
    uint32_t component_count;
    if (ext4_tokenize_path(path, components, &component_count) != 0 || component_count == 0) {
        return 2;
    }

    char parent_path[1024];
    uint32_t offset = 0;
    for (uint32_t i = 0; i < component_count - 1; i++) {
        if (i > 0) {
            parent_path[offset++] = '/';
        }
        uint32_t len = (uint32_t)strlen(components[i]);
        memcpy(parent_path + offset, components[i], len);
        offset += len;
    }
    parent_path[offset] = '\0';

    uint32_t parent_inode_number;
    if (component_count > 1) {
        if (ext4_resolve_path(fs, parent_path, &parent_inode_number) != 0) {
            return 3;
        }
    } else {
        parent_inode_number = EXT4_ROOT_INODE;
    }

    ext4_inode_t parent_inode;
    if (ext4_read_inode(fs, parent_inode_number, &parent_inode) != 0) {
        return 4;
    }
    if (!ext4_is_directory(&parent_inode)) {
        return 5;
    }

    uint32_t existing;
    if (ext4_find_dir_entry(fs, &parent_inode, components[component_count - 1], &existing) == 0) {
        return 6;
    }

    uint32_t new_inode;
    if (ext4_allocate_inode(fs, mode | EXT4_INODE_MODE_REG, &new_inode) != 0) {
        return 7;
    }

    if (ext4_insert_dir_entry(fs, &parent_inode, parent_inode_number, new_inode, components[component_count - 1], EXT4_FILE_TYPE_REG_FILE) != 0) {
        return 8;
    }

    return ext4_write_inode(fs, parent_inode_number, &parent_inode);
}

uint32_t ext4_make_directory(ext4_fs_t *fs, const char *path) {
    if (!fs || !path) {
        return 1;
    }

    char components[64][EXT4_NAME_LEN_MAX + 1];
    uint32_t component_count;
    if (ext4_tokenize_path(path, components, &component_count) != 0 || component_count == 0) {
        return 2;
    }

    char parent_path[1024];
    uint32_t offset = 0;
    for (uint32_t i = 0; i < component_count - 1; i++) {
        if (i > 0) {
            parent_path[offset++] = '/';
        }
        uint32_t len = (uint32_t)strlen(components[i]);
        memcpy(parent_path + offset, components[i], len);
        offset += len;
    }
    parent_path[offset] = '\0';

    uint32_t parent_inode_number;
    if (component_count > 1) {
        if (ext4_resolve_path(fs, parent_path, &parent_inode_number) != 0) {
            return 3;
        }
    } else {
        parent_inode_number = EXT4_ROOT_INODE;
    }

    ext4_inode_t parent_inode;
    if (ext4_read_inode(fs, parent_inode_number, &parent_inode) != 0) {
        return 4;
    }
    if (!ext4_is_directory(&parent_inode)) {
        return 5;
    }

    uint32_t existing;
    if (ext4_find_dir_entry(fs, &parent_inode, components[component_count - 1], &existing) == 0) {
        return 6;
    }

    uint32_t new_inode;
    if (ext4_allocate_inode(fs, EXT4_INODE_MODE_DIR, &new_inode) != 0) {
        return 7;
    }

    ext4_inode_t new_dir_inode;
    memset(&new_dir_inode, 0, sizeof(new_dir_inode));
    new_dir_inode.i_mode = EXT4_INODE_MODE_DIR;
    new_dir_inode.i_links_count = 2;
    ext4_write_inode(fs, new_inode, &new_dir_inode);

    if (ext4_insert_dir_entry(fs, &new_dir_inode, new_inode, new_inode, ".", EXT4_FILE_TYPE_DIR) != 0) {
        return 8;
    }
    if (ext4_insert_dir_entry(fs, &new_dir_inode, new_inode, parent_inode_number, "..", EXT4_FILE_TYPE_DIR) != 0) {
        return 9;
    }

    if (ext4_insert_dir_entry(fs, &parent_inode, parent_inode_number, new_inode, components[component_count - 1], EXT4_FILE_TYPE_DIR) != 0) {
        return 10;
    }

    parent_inode.i_links_count++;
    ext4_write_inode(fs, parent_inode_number, &parent_inode);
    return ext4_write_inode(fs, new_inode, &new_dir_inode);
}

uint32_t ext4_remove_entry(ext4_fs_t *fs, const char *path) {
    if (!fs || !path) {
        return 1;
    }

    char components[64][EXT4_NAME_LEN_MAX + 1];
    uint32_t component_count;
    if (ext4_tokenize_path(path, components, &component_count) != 0 || component_count == 0) {
        return 2;
    }

    char parent_path[1024];
    uint32_t offset = 0;
    for (uint32_t i = 0; i < component_count - 1; i++) {
        if (i > 0) {
            parent_path[offset++] = '/';
        }
        uint32_t len = (uint32_t)strlen(components[i]);
        memcpy(parent_path + offset, components[i], len);
        offset += len;
    }
    parent_path[offset] = '\0';

    uint32_t parent_inode_number;
    if (component_count > 1) {
        if (ext4_resolve_path(fs, parent_path, &parent_inode_number) != 0) {
            return 3;
        }
    } else {
        parent_inode_number = EXT4_ROOT_INODE;
    }

    ext4_inode_t parent_inode;
    if (ext4_read_inode(fs, parent_inode_number, &parent_inode) != 0) {
        return 4;
    }
    if (!ext4_is_directory(&parent_inode)) {
        return 5;
    }

    uint32_t target_inode;
    if (ext4_find_dir_entry(fs, &parent_inode, components[component_count - 1], &target_inode) != 0) {
        return 6;
    }

    ext4_inode_t target_inode_struct;
    if (ext4_read_inode(fs, target_inode, &target_inode_struct) != 0) {
        return 7;
    }

    if (ext4_is_directory(&target_inode_struct)) {
        if (ext4_inode_physical_size(&target_inode_struct) > fs->block_size) {
            return 8;
        }
    }

    if (ext4_remove_dir_entry(fs, &parent_inode, components[component_count - 1]) != 0) {
        return 9;
    }

    target_inode_struct.i_links_count--;
    if (target_inode_struct.i_links_count == 0) {
        if (target_inode_struct.i_flags & EXT4_EXTENTS_FL) {
            ext4_extent_header_t *header = (ext4_extent_header_t *)target_inode_struct.i_block;
            ext4_extent_t *extent = (ext4_extent_t *)((uint8_t *)header + sizeof(ext4_extent_header_t));
            for (uint32_t i = 0; i < header->eh_entries; i++) {
                uint64_t start = ext4_extent_get_start(&extent[i]);
                ext4_free_blocks(fs, (uint32_t)start, extent[i].ee_len);
            }
        } else {
            for (uint32_t i = 0; i < EXT4_NDIR_BLOCKS; i++) {
                if (target_inode_struct.i_block[i] != 0) {
                    ext4_free_blocks(fs, target_inode_struct.i_block[i], 1);
                }
            }
        }
    }

    ext4_write_inode(fs, target_inode, &target_inode_struct);
    return ext4_write_inode(fs, parent_inode_number, &parent_inode);
}

uint32_t ext4_enable_journal(ext4_fs_t *fs) {
    if (!fs) {
        return 1;
    }

    if (!(fs->superblock.s_feature_compat & EXT4_FEATURE_COMPAT_HAS_JOURNAL)) {
        return 2;
    }

    fs->journal_active = 1;
    fs->journal_sequence = 0;
    fs->journal_block = fs->superblock.s_jnl_blocks[0];
    if (fs->journal_block == 0) {
        fs->journal_active = 0;
        return 3;
    }
    return 0;
}

uint32_t ext4_journal_start(ext4_fs_t *fs) {
    if (!fs || !fs->journal_active) {
        return 1;
    }

    fs->journal_sequence++;
    return 0;
}

uint32_t ext4_journal_commit(ext4_fs_t *fs) {
    if (!fs || !fs->journal_active) {
        return 1;
    }

    ext4_journal_header_t header;
    header.journal_magic = EXT4_JOURNAL_HEADER_MAGIC;
    header.journal_blocktype = EXT4_JOURNAL_COMMIT_BLOCK;
    header.journal_sequence = fs->journal_sequence;

    return ext4_write_blocks(fs, fs->journal_block, 1, &header);
}

uint32_t ext4_sync(ext4_fs_t *fs) {
    if (!fs) {
        return 1;
    }

    if (fs->journal_active) {
        ext4_journal_start(fs);
    }

    uint32_t result = ext4_write_superblock(fs);
    if (result != 0) {
        return result;
    }

    result = ext4_write_group_descriptors(fs);
    if (result != 0) {
        return result;
    }

    if (fs->journal_active) {
        return ext4_journal_commit(fs);
    }
    return 0;
}

uint32_t ext4_mount(uint32_t device_id, ext4_fs_t *fs) {
    if (!fs) {
        return 1;
    }

    memset(fs, 0, sizeof(ext4_fs_t));
    fs->device_id = device_id;
    fs->start_lba = 0;

    if (ext4_read_superblock(fs) != 0) {
        return 2;
    }

    return 0;
}

uint32_t ext4_unmount(ext4_fs_t *fs) {
    if (!fs) {
        return 1;
    }

    if (fs->group_descriptors) {
        kfree((uint64_t)fs->group_descriptors);
        fs->group_descriptors = NULL;
    }

    memset(fs, 0, sizeof(ext4_fs_t));
    return 0;
}
