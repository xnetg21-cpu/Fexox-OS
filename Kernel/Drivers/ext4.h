#pragma once

#include <stdint.h>
#include "disk.h"

#define EXT4_SUPER_MAGIC 0xEF53
#define EXT4_ROOT_INODE 2
#define EXT4_NAME_LEN_MAX 255
#define EXT4_NDIR_BLOCKS 12
#define EXT4_N_BLOCKS 15
#define EXT4_EXTENTS_FL 0x80000
#define EXT4_JOURNAL_INO 8

#define EXT4_GOOD_OLD_INODE_SIZE 128

#define EXT4_FEATURE_COMPAT_DIR_INDEX      0x0001
#define EXT4_FEATURE_COMPAT_IMAGIC_INODES  0x0002
#define EXT4_FEATURE_COMPAT_HAS_JOURNAL    0x0004
#define EXT4_FEATURE_COMPAT_EXT_ATTR       0x0008
#define EXT4_FEATURE_COMPAT_RESIZE_INO     0x0010
#define EXT4_FEATURE_COMPAT_DIR_DATA       0x0020

#define EXT4_FEATURE_INCOMPAT_FILETYPE     0x0002
#define EXT4_FEATURE_INCOMPAT_EXTENTS      0x0040
#define EXT4_FEATURE_INCOMPAT_64BIT        0x0080
#define EXT4_FEATURE_INCOMPAT_METADATA_CSUM 0x0200

#define EXT4_FEATURE_RO_COMPAT_SPARSE_SUPER 0x0001
#define EXT4_FEATURE_RO_COMPAT_LARGE_FILE    0x0002
#define EXT4_FEATURE_RO_COMPAT_BTREE_DIR     0x0004

#define EXT4_INODE_MODE_REG 0x8000
#define EXT4_INODE_MODE_DIR 0x4000
#define EXT4_INODE_MODE_SYMLINK 0xA000

#define EXT4_FILE_TYPE_UNKNOWN 0
#define EXT4_FILE_TYPE_REG_FILE 1
#define EXT4_FILE_TYPE_DIR 2
#define EXT4_FILE_TYPE_SYMLINK 7

#define EXT4_JOURNAL_HEADER_MAGIC 0xC03B3998
#define EXT4_JOURNAL_COMMIT_BLOCK 0x01

#pragma pack(push, 1)

typedef struct {
    uint32_t s_inodes_count;
    uint32_t s_blocks_count_lo;
    uint32_t s_r_blocks_count_lo;
    uint32_t s_free_blocks_count_lo;
    uint32_t s_free_inodes_count;
    uint32_t s_first_data_block;
    uint32_t s_log_block_size;
    uint32_t s_log_cluster_size;
    uint32_t s_blocks_per_group;
    uint32_t s_clusters_per_group;
    uint32_t s_inodes_per_group;
    uint32_t s_mtime;
    uint32_t s_wtime;
    uint16_t s_mnt_count;
    uint16_t s_max_mnt_count;
    uint16_t s_magic;
    uint16_t s_state;
    uint16_t s_errors;
    uint16_t s_minor_rev_level;
    uint32_t s_lastcheck;
    uint32_t s_checkinterval;
    uint32_t s_creator_os;
    uint32_t s_rev_level;
    uint16_t s_def_resuid;
    uint16_t s_def_resgid;
    uint32_t s_first_ino;
    uint16_t s_inode_size;
    uint16_t s_block_group_nr;
    uint32_t s_feature_compat;
    uint32_t s_feature_incompat;
    uint32_t s_feature_ro_compat;
    uint8_t  s_uuid[16];
    char     s_volume_name[16];
    char     s_last_mounted[64];
    uint32_t s_algo_bitmap;
    uint8_t  s_prealloc_blocks;
    uint8_t  s_prealloc_dir_blocks;
    uint16_t s_padding;
    uint8_t  s_journal_uuid[16];
    uint32_t s_journal_inum;
    uint32_t s_journal_dev;
    uint32_t s_last_orphan;
    uint32_t s_hash_seed[4];
    uint8_t  s_def_hash_version;
    uint8_t  s_jnl_backup_type;
    uint16_t s_desc_size;
    uint32_t s_default_mount_opts;
    uint32_t s_first_meta_bg;
    uint32_t s_mkfs_time;
    uint32_t s_jnl_blocks[17];
    uint32_t s_blocks_count_hi;
    uint32_t s_r_blocks_count_hi;
    uint32_t s_free_blocks_count_hi;
    uint16_t s_min_extra_isize;
    uint16_t s_want_extra_isize;
    uint32_t s_flags;
    uint16_t s_raid_stride;
    uint16_t s_mmp_interval;
    uint64_t s_mmp_block;
    uint32_t s_raid_stripe_width;
    uint8_t  s_log_groups_per_flex;
    uint8_t  s_reserved_char_pad;
    uint16_t s_reserved_word_pad;
    uint32_t s_reserved[152];
} ext4_superblock_t;

typedef struct {
    uint32_t bg_block_bitmap_lo;
    uint32_t bg_inode_bitmap_lo;
    uint32_t bg_inode_table_lo;
    uint16_t bg_free_blocks_count_lo;
    uint16_t bg_free_inodes_count_lo;
    uint16_t bg_used_dirs_count_lo;
    uint16_t bg_flags;
    uint32_t bg_exclude_bitmap_lo;
    uint16_t bg_block_bitmap_csum_lo;
    uint16_t bg_inode_bitmap_csum_lo;
    uint16_t bg_itable_unused_lo;
    uint16_t bg_checksum;
    uint32_t bg_block_bitmap_hi;
    uint32_t bg_inode_bitmap_hi;
    uint32_t bg_inode_table_hi;
    uint16_t bg_free_blocks_count_hi;
    uint16_t bg_free_inodes_count_hi;
    uint16_t bg_used_dirs_count_hi;
    uint16_t bg_itable_unused_hi;
    uint32_t bg_exclude_bitmap_hi;
    uint16_t bg_block_bitmap_csum_hi;
    uint16_t bg_inode_bitmap_csum_hi;
    uint32_t bg_reserved;
} ext4_group_desc_t;

typedef struct {
    uint16_t i_mode;
    uint16_t i_uid;
    uint32_t i_size_lo;
    uint32_t i_atime;
    uint32_t i_ctime;
    uint32_t i_mtime;
    uint32_t i_dtime;
    uint16_t i_gid;
    uint16_t i_links_count;
    uint32_t i_blocks_lo;
    uint32_t i_flags;
    uint32_t i_osd1;
    uint32_t i_block[EXT4_N_BLOCKS];
    uint32_t i_generation;
    uint32_t i_file_acl_lo;
    uint32_t i_size_high;
    uint32_t i_obso_faddr;
    uint32_t i_osd2[3];
} ext4_inode_t;

typedef struct {
    uint32_t inode;
    uint16_t rec_len;
    uint8_t  name_len;
    uint8_t  file_type;
    char     name[EXT4_NAME_LEN_MAX];
} ext4_dir_entry_t;

typedef struct {
    uint16_t eh_magic;
    uint16_t eh_entries;
    uint16_t eh_max;
    uint16_t eh_depth;
    uint32_t eh_generation;
} ext4_extent_header_t;

typedef struct {
    uint32_t ei_block;
    uint32_t ei_leaf_lo;
    uint16_t ei_leaf_hi;
    uint16_t ei_unused;
} ext4_extent_idx_t;

typedef struct {
    uint32_t ee_block;
    uint16_t ee_len;
    uint16_t ee_start_hi;
    uint32_t ee_start_lo;
} ext4_extent_t;

typedef struct {
    uint32_t journal_magic;
    uint32_t journal_blocktype;
    uint32_t journal_sequence;
} ext4_journal_header_t;

#pragma pack(pop)

typedef struct {
    uint32_t device_id;
    uint64_t start_lba;
    ext4_superblock_t superblock;
    ext4_group_desc_t *group_descriptors;
    uint32_t block_size;
    uint32_t blocks_per_group;
    uint32_t inodes_per_group;
    uint32_t inode_size;
    uint32_t inode_table_blocks;
    uint32_t group_count;
    uint32_t first_data_block;
    uint32_t journal_block;
    uint32_t journal_sequence;
    uint8_t  journal_active;
} ext4_fs_t;

uint32_t ext4_mount(uint32_t device_id, ext4_fs_t *fs);
uint32_t ext4_unmount(ext4_fs_t *fs);
uint32_t ext4_read_superblock(ext4_fs_t *fs);
uint32_t ext4_sync(ext4_fs_t *fs);

uint32_t ext4_resolve_path(ext4_fs_t *fs, const char *path, uint32_t *inode_number);
uint32_t ext4_read_file(ext4_fs_t *fs, const char *path, void *buffer, uint64_t offset, uint64_t count, uint64_t *read_bytes);
uint32_t ext4_write_file(ext4_fs_t *fs, const char *path, const void *buffer, uint64_t offset, uint64_t count, uint64_t *written_bytes);
uint32_t ext4_create_file(ext4_fs_t *fs, const char *path, uint16_t mode);
uint32_t ext4_make_directory(ext4_fs_t *fs, const char *path);
uint32_t ext4_remove_entry(ext4_fs_t *fs, const char *path);
uint32_t ext4_list_directory(ext4_fs_t *fs, const char *path, void (*callback)(const char *, uint32_t, void *), void *context);

uint32_t ext4_read_inode(ext4_fs_t *fs, uint32_t inode_number, ext4_inode_t *inode);
uint32_t ext4_write_inode(ext4_fs_t *fs, uint32_t inode_number, ext4_inode_t *inode);

uint32_t ext4_read_directory(ext4_fs_t *fs, ext4_inode_t *directory, uint32_t (*entry_callback)(ext4_dir_entry_t *, void *), void *context);
uint32_t ext4_get_file_type(ext4_inode_t *inode);

uint64_t ext4_get_inode_size(ext4_inode_t *inode);

uint32_t ext4_allocate_inode(ext4_fs_t *fs, uint16_t mode, uint32_t *allocated_inode);
uint32_t ext4_allocate_blocks(ext4_fs_t *fs, uint32_t count, uint32_t *first_block);
uint32_t ext4_free_blocks(ext4_fs_t *fs, uint32_t first_block, uint32_t count);

uint32_t ext4_enable_journal(ext4_fs_t *fs);
uint32_t ext4_journal_start(ext4_fs_t *fs);
uint32_t ext4_journal_commit(ext4_fs_t *fs);

uint32_t ext4_is_directory(ext4_inode_t *inode);
uint32_t ext4_is_regular_file(ext4_inode_t *inode);
uint32_t ext4_is_symlink(ext4_inode_t *inode);
