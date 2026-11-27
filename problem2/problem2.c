/*
 * CS3103 Operating Systems - Problem 2 Starter Code
 * Mini File System Simulator
 *
 * This file intentionally provides only a framework.
 * Students must implement the file-system operations marked TODO.
 */

/* Build on Linux: gcc -std=c11 -Wall -Wextra problem2.c -o minifs */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <ctype.h>
#include <errno.h>
#include <unistd.h>

#define DISK_FILE       "virtual_disk.bin"
#define DISK_SIZE       (1024 * 1024)
#define BLOCK_SIZE      256
#define NUM_BLOCKS      (DISK_SIZE / BLOCK_SIZE)
#define MAX_ENTRIES     128
#define MAX_NAME_LEN    31
/* Initial storage choice, not an assignment limit; extend as needed. */
#define MAX_PATH_LEN    (MAX_ENTRIES * (MAX_NAME_LEN + 1) + 1)

#define TYPE_FILE       1
#define TYPE_DIR        2

#define FS_MAGIC        UINT32_C(0x3153464d)
#define FS_VERSION      1
#define FAT_START       1
#define FAT_BLOCKS      (NUM_BLOCKS * 4 / BLOCK_SIZE)
#define ENTRY_BYTES     64
#define ENTRY_START     (FAT_START + FAT_BLOCKS)
#define ENTRY_BLOCKS    (MAX_ENTRIES * ENTRY_BYTES / BLOCK_SIZE)
#define DATA_START      (ENTRY_START + ENTRY_BLOCKS)
#define DATA_CAPACITY   ((NUM_BLOCKS - DATA_START) * BLOCK_SIZE)
#define NO_BLOCK        (-1)
#define FAT_FREE        (-1)
#define FAT_END         (-2)
#define FAT_RESERVED    (-3)

/*
 * A minimal metadata definition is provided as a starting point.
 * You may modify or extend it to support your design.
 *
 * Example metadata table for /courses/os/report.txt:
 *   entry ID    name          parent_id
 *       0       /                 0       (root is its own parent)
 *       1       courses           0
 *       2       os                1
 *       3       report.txt        2
 * Each ID is an index in your metadata table. To resolve this path,
 * start at root and find each name among the current directory's children.
 * Persist name and parent_id in virtual_disk.bin to retain the hierarchy.
 * This is one possible representation; an equivalent design is allowed.
 */
typedef struct {
    char name[MAX_NAME_LEN + 1]; /* One name, not the full path. */
    int parent_id;              /* Parent directory's metadata-table index. */
    uint32_t size;
    uint8_t type;
    int32_t first_block; 
    uint8_t used;       
} file_entry_t;

/*
 * Example superblock. Students may modify or extend this structure.
 */
typedef struct {
    uint32_t magic;
    uint32_t block_size;
    uint32_t total_blocks;
    uint32_t version;
    uint32_t fat_start;
    uint32_t fat_blocks;
    uint32_t entries_start;
    uint32_t entries_blocks;
    uint32_t data_start;
    uint32_t entry_bytes;
    uint32_t max_entries;
} superblock_t;

/* File-system state used while the simulator is running. */
typedef struct {
    FILE *disk;
    superblock_t superblock;
    char cwd[MAX_PATH_LEN]; /* May be replaced by a current-directory ID. */
    file_entry_t entries[MAX_ENTRIES]; 
    int32_t fat[NUM_BLOCKS];        
    int cwd_id;                      
    int ready;                      
} filesystem_t;

/* ---------- Utility functions provided in the starter ---------- */

static void trim_newline(char *s)
{
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r')) {
        s[n - 1] = '\0';
        --n;
    }
}

static char *skip_spaces(char *s)
{
    while (*s != '\0' && isspace((unsigned char)*s)) {
        ++s;
    }
    return s;
}

static char *next_token(char **cursor)
{
    char *start = skip_spaces(*cursor);
    char *end;

    if (*start == '\0') {
        *cursor = start;
        return NULL;
    }

    end = start;
    while (*end != '\0' && !isspace((unsigned char)*end)) {
        ++end;
    }

    if (*end != '\0') {
        *end = '\0';
        *cursor = end + 1;
    } else {
        *cursor = end;
    }

    return start;
}

/* Display the assignment's commands at startup; no help command is required. */
static void print_commands(void)
{
    printf("Commands:\n");
    printf("  format\n");
    printf("  create <path>\n");
    printf("  write <path> \"<content>\"\n");
    printf("  append <path> \"<content>\"\n");
    printf("  read <path>\n");
    printf("  delete <path>\n");
    printf("  mkdir <path>\n");
    printf("  rmdir <path>\n");
    printf("  cd <path>\n");
    printf("  pwd\n");
    printf("  ls\n");
    printf("  stat <path>\n");
    printf("  df\n");
    printf("  exit\n");
}

/*
 * Purpose: Report a filesystem error.
 * Input: message is the error description.
 * Return: -1; writes the message to stderr.
 */
static int fs_error(const char *message)
{
    fprintf(stderr, "Error: %s\n", message);
    return -1;
}

/*
 * Purpose: Check whether filesystem commands can use the current disk.
 * Input: fs is the current filesystem state.
 * Return: 0 when ready, or -1 after reporting an error.
 */
static int require_ready(const filesystem_t *fs)
{
    return fs->ready ? 0 : fs_error("No usable file system. Run format first.");
}

/*
 * Purpose: Encode a 32-bit unsigned value in little-endian order.
 * Inputs: p points to four writable bytes; value is the number to store.
 * Effects: Writes four bytes to p; returns no value.
 */
static void put_u32(unsigned char *p, uint32_t value)
{
    for (unsigned int i = 0; i < 4; ++i) p[i] = (unsigned char)(value >> (8 * i));
}

/*
 * Purpose: Decode a little-endian 32-bit unsigned value.
 * Input: p points to four readable bytes.
 * Return: The decoded unsigned value.
 */
static uint32_t get_u32(const unsigned char *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 |
           (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

/*
 * Purpose: Decode a little-endian 32-bit signed value, including FAT markers.
 * Input: p points to four readable bytes.
 * Return: The decoded signed value.
 */
static int32_t get_i32(const unsigned char *p)
{
    uint32_t value = get_u32(p);
    return value <= INT32_MAX ? (int32_t)value : -1 - (int32_t)(UINT32_MAX - value);
}

/*
 * Purpose: Calculate how many data blocks a file needs.
 * Input: size is the file size in bytes.
 * Return: The block count rounded up; zero for an empty file.
 */
static uint32_t blocks_for(uint32_t size)
{
    return size / BLOCK_SIZE + (size % BLOCK_SIZE != 0);
}

/*
 * Purpose: Validate a single file or directory name.
 * Input: name is stored in a buffer of at least MAX_NAME_LEN + 1 bytes.
 * Return: 1 for a valid component, or 0 for an invalid name.
 */
static int valid_name(const char *name)
{
    const char *end = memchr(name, '\0', MAX_NAME_LEN + 1);
    if (end == NULL || end == name || strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
        return 0;
    for (const char *p = name; p < end; ++p)
        if (*p == '/' || iscntrl((unsigned char)*p)) return 0;
    return 1;
}

/*
 * Purpose: Find an existing child with a given parent and name.
 * Inputs: fs is the state; parent is the directory ID; name is the component.
 * Return: The matching metadata ID, or -1 if no child matches.
 */
static int find_child(const filesystem_t *fs, int parent, const char *name)
{
    for (int i = 1; i < MAX_ENTRIES; ++i)
        if (fs->entries[i].used && fs->entries[i].parent_id == parent &&
            strcmp(fs->entries[i].name, name) == 0) return i;
    return -1;
}

/*
 * Purpose: Count the direct children of a directory.
 * Inputs: fs is the state; id is the directory metadata ID.
 * Return: The number of used child records, excluding root itself.
 */
static int child_count(const filesystem_t *fs, int id)
{
    int count = 0;
    for (int i = 1; i < MAX_ENTRIES; ++i)
        if (fs->entries[i].used && fs->entries[i].parent_id == id) ++count;
    return count;
}

/*
 * Purpose: Serialize the superblock, FAT and metadata records.
 * Inputs: fs supplies the state; image is a writable DISK_SIZE-byte buffer.
 * Effects: Rewrites the reserved metadata region; returns no value.
 */
static void encode_metadata(const filesystem_t *fs, unsigned char *image)
{
    const superblock_t *s = &fs->superblock;
    const uint32_t fields[] = {s->magic, s->block_size, s->total_blocks,
        s->version, s->fat_start, s->fat_blocks, s->entries_start,
        s->entries_blocks, s->data_start, s->entry_bytes, s->max_entries};
    memset(image, 0, DATA_START * BLOCK_SIZE);
    for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); ++i)
        put_u32(image + 4 * i, fields[i]);
    for (int i = 0; i < NUM_BLOCKS; ++i)
        put_u32(image + FAT_START * BLOCK_SIZE + 4 * i, (uint32_t)fs->fat[i]);
    for (int i = 0; i < MAX_ENTRIES; ++i) {
        const file_entry_t *e = &fs->entries[i];
        unsigned char *p = image + ENTRY_START * BLOCK_SIZE + ENTRY_BYTES * i;
        memcpy(p, e->name, MAX_NAME_LEN + 1);
        put_u32(p + 32, (uint32_t)e->parent_id);
        put_u32(p + 36, e->size);
        p[40] = e->type;
        p[41] = e->used;
        put_u32(p + 44, (uint32_t)e->first_block);
    }
}

/*
 * Purpose: Decode the superblock, FAT and metadata records from a disk image.
 * Inputs: fs receives the decoded state; image contains DISK_SIZE bytes.
 * Effects: Updates metadata in fs without validating it; returns no value.
 */
static void decode_metadata(filesystem_t *fs, const unsigned char *image)
{
    superblock_t *s = &fs->superblock;
    s->magic = get_u32(image);
    s->block_size = get_u32(image + 4);
    s->total_blocks = get_u32(image + 8);
    s->version = get_u32(image + 12);
    s->fat_start = get_u32(image + 16);
    s->fat_blocks = get_u32(image + 20);
    s->entries_start = get_u32(image + 24);
    s->entries_blocks = get_u32(image + 28);
    s->data_start = get_u32(image + 32);
    s->entry_bytes = get_u32(image + 36);
    s->max_entries = get_u32(image + 40);
    for (int i = 0; i < NUM_BLOCKS; ++i)
        fs->fat[i] = get_i32(image + FAT_START * BLOCK_SIZE + 4 * i);
    for (int i = 0; i < MAX_ENTRIES; ++i) {
        file_entry_t *e = &fs->entries[i];
        const unsigned char *p = image + ENTRY_START * BLOCK_SIZE + ENTRY_BYTES * i;
        memcpy(e->name, p, MAX_NAME_LEN + 1);
        e->parent_id = get_i32(p + 32);
        e->size = get_u32(p + 36);
        e->type = p[40];
        e->used = p[41];
        e->first_block = get_i32(p + 44);
    }
}

/*
 * Purpose: Check the disk layout, directory tree and FAT ownership rules.
 * Input: fs contains decoded metadata and allocation state.
 * Return: 1 if the state is consistent, or 0 if an invariant is violated.
 */
static int valid_state(const filesystem_t *fs)
{
    const superblock_t *s = &fs->superblock;
    unsigned char owned[NUM_BLOCKS] = {0};
    if (s->magic != FS_MAGIC || s->version != FS_VERSION ||
        s->block_size != BLOCK_SIZE || s->total_blocks != NUM_BLOCKS ||
        s->fat_start != FAT_START || s->fat_blocks != FAT_BLOCKS ||
        s->entries_start != ENTRY_START || s->entries_blocks != ENTRY_BLOCKS ||
        s->data_start != DATA_START || s->entry_bytes != ENTRY_BYTES ||
        s->max_entries != MAX_ENTRIES) return 0;
    for (int b = 0; b < DATA_START; ++b)
        if (fs->fat[b] != FAT_RESERVED) return 0;
    if (fs->entries[0].used != 1 || fs->entries[0].type != TYPE_DIR ||
        fs->entries[0].parent_id != 0 || memcmp(fs->entries[0].name, "/\0", 2) != 0)
        return 0;
    for (int i = 0; i < MAX_ENTRIES; ++i) {
        const file_entry_t *e = &fs->entries[i];
        if (e->used == 0) continue;
        if (e->used != 1 || (e->type != TYPE_FILE && e->type != TYPE_DIR)) return 0;
        if (i != 0 && !valid_name(e->name)) return 0;
        if (e->parent_id < 0 || e->parent_id >= MAX_ENTRIES ||
            !fs->entries[e->parent_id].used || fs->entries[e->parent_id].type != TYPE_DIR)
            return 0;
        int ancestor = i, steps = 0;
        while (ancestor != 0 && steps++ < MAX_ENTRIES) {
            ancestor = fs->entries[ancestor].parent_id;
            if (ancestor < 0 || ancestor >= MAX_ENTRIES ||
                !fs->entries[ancestor].used || fs->entries[ancestor].type != TYPE_DIR) return 0;
        }
        if (ancestor != 0) return 0;
        for (int j = 1; j < i; ++j)
            if (fs->entries[j].used && fs->entries[j].parent_id == e->parent_id &&
                strcmp(fs->entries[j].name, e->name) == 0) return 0;
        if (e->type == TYPE_DIR) {
            if (e->size != 0 || e->first_block != NO_BLOCK) return 0;
            continue;
        }
        if (e->size > DATA_CAPACITY) return 0;
        int b = e->first_block;
        uint32_t count = blocks_for(e->size);
        if (count == 0) { if (b != NO_BLOCK) return 0; continue; }
        for (uint32_t n = 0; n < count; ++n) {
            if (b < DATA_START || b >= NUM_BLOCKS || owned[b]) return 0;
            owned[b] = 1;
            b = fs->fat[b];
        }
        if (b != FAT_END) return 0;
    }
    for (int b = DATA_START; b < NUM_BLOCKS; ++b)
        if (!owned[b] && fs->fat[b] != FAT_FREE) return 0;
    return 1;
}

/*
 * Purpose: Read the complete virtual disk into an allocated memory buffer.
 * Input: fs supplies the open disk stream.
 * Return: A DISK_SIZE-byte buffer the caller must free, or NULL on error.
 */
static unsigned char *read_image(filesystem_t *fs)
{
    unsigned char *image = malloc(DISK_SIZE);
    if (image == NULL) { fs_error("Out of host memory."); return NULL; }
    if (fs->disk == NULL || fseek(fs->disk, 0, SEEK_SET) != 0 ||
        fread(image, 1, DISK_SIZE, fs->disk) != DISK_SIZE) {
        free(image);
        fs_error("Cannot read virtual disk.");
        return NULL;
    }
    return image;
}

/*
 * Purpose: Write and synchronize a complete disk image.
 * Inputs: disk is the open stream; image contains DISK_SIZE bytes.
 * Return: 0 on success or -1 on I/O failure; writes and resizes the disk.
 */
static int write_image(FILE *disk, const unsigned char *image)
{
    clearerr(disk);
    if (fseek(disk, 0, SEEK_SET) != 0 || fwrite(image, 1, DISK_SIZE, disk) != DISK_SIZE ||
        fflush(disk) != 0 || ftruncate(fileno(disk), DISK_SIZE) != 0 || fsync(fileno(disk)) != 0)
        return -1;
    return 0;
}

/*
 * Purpose: Save a candidate image before adopting its runtime state.
 * Inputs: fs is current state; next is candidate state; image is owned memory.
 * Return: 0 on success or -1 on failure; frees image on every path.
 * Effects: Writes the disk and attempts backup restoration on write failure.
 */
static int commit_state(filesystem_t *fs, filesystem_t *next, unsigned char *image)
{
    unsigned char *backup = NULL;
    if (fs->ready) {
        backup = read_image(fs);
        if (backup == NULL) { free(image); return -1; }
    }
    if (fs->disk == NULL) fs->disk = fopen(DISK_FILE, "w+b");
    if (fs->disk == NULL) {
        free(backup); free(image);
        return fs_error("Cannot create virtual disk.");
    }
    next->disk = fs->disk;
    encode_metadata(next, image);
    if (write_image(fs->disk, image) != 0) {
        fs_error("Disk write failed.");
        if (backup != NULL && write_image(fs->disk, backup) != 0) {
            fs->ready = 0;
            fs_error("Disk restoration failed; file system disabled.");
        }
        free(backup); free(image);
        return -1;
    }
    *fs = *next;
    free(backup); free(image);
    return 0;
}

/*
 * Purpose: Resolve an absolute or relative path, including dot components.
 * Inputs: fs and path; parent_mode selects the object (0) or its parent (1).
 * Outputs: id receives the resolved ID; name receives the final name in mode 1.
 * Return: 0 on success or -1 on error; name may be NULL in mode 0.
 */
static int resolve_path(const filesystem_t *fs, const char *path, int parent_mode,
                        int *id, char *name)
{
    if (require_ready(fs) != 0) return -1;
    if (path == NULL || *path == '\0') return fs_error("Empty path.");
    int current = path[0] == '/' ? 0 : fs->cwd_id;
    const char *p = path;
    while (*p == '/') ++p;
    if (*p == '\0') {
        if (parent_mode) return fs_error("Root already exists.");
        *id = current;
        return 0;
    }
    while (*p != '\0') {
        const char *start = p;
        char component[MAX_NAME_LEN + 1];
        while (*p != '\0' && *p != '/') ++p;
        size_t length = (size_t)(p - start);
        if (length > MAX_NAME_LEN) return fs_error("Name exceeds 31 bytes.");
        memcpy(component, start, length); component[length] = '\0';
        int trailing_slash = *p == '/';
        while (*p == '/') ++p;
        if (fs->entries[current].type != TYPE_DIR) return fs_error("Intermediate object is not a directory.");
        if (parent_mode && *p == '\0') {
            if (!valid_name(component)) return fs_error("Invalid name.");
            strcpy(name, component); *id = current;
            return 0;
        }
        if (strcmp(component, "..") == 0) current = fs->entries[current].parent_id;
        else if (strcmp(component, ".") != 0) {
            if (!valid_name(component)) return fs_error("Invalid name.");
            int child = find_child(fs, current, component);
            if (child < 0) return fs_error("Path does not exist.");
            current = child;
        }
        if (*p == '\0' && trailing_slash && fs->entries[current].type != TYPE_DIR)
            return fs_error("Trailing slash requires a directory.");
    }
    *id = current;
    return 0;
}

/*
 * Purpose: Reconstruct and set the absolute current directory path.
 * Inputs: fs is the state to update; id is a valid directory metadata ID.
 * Return: 0 after updating cwd and cwd_id, or -1 if reconstruction fails.
 */
static int set_cwd(filesystem_t *fs, int id)
{
    int ancestors[MAX_ENTRIES], count = 0, current = id;
    char path[MAX_PATH_LEN];
    size_t length = 0;
    while (current != 0) {
        if (count >= MAX_ENTRIES) return fs_error("Directory cycle detected.");
        ancestors[count++] = current;
        current = fs->entries[current].parent_id;
    }
    if (count == 0) path[length++] = '/';
    while (count > 0) {
        const char *name = fs->entries[ancestors[--count]].name;
        size_t n = strlen(name);
        if (length + 1 + n >= sizeof(path)) return fs_error("Cannot represent current path.");
        path[length++] = '/';
        memcpy(path + length, name, n); length += n;
    }
    path[length] = '\0';
    strcpy(fs->cwd, path); fs->cwd_id = id;
    return 0;
}

/*
 * Purpose: Validate and create an empty file or directory record.
 * Inputs: fs is the state; path names the new entry; type selects file or directory.
 * Return: 0 after committing the new record, or -1 on validation or I/O error.
 */
static int create_entry(filesystem_t *fs, const char *path, uint8_t type)
{
    int parent, slot = -1;
    char name[MAX_NAME_LEN + 1];
    if (resolve_path(fs, path, 1, &parent, name) != 0) return -1;
    if (type == TYPE_FILE && path[strlen(path) - 1] == '/')
        return fs_error("Regular-file creation path cannot end in '/'.");
    if (find_child(fs, parent, name) >= 0) return fs_error("Name already exists in this directory.");
    for (int i = 1; i < MAX_ENTRIES; ++i)
        if (!fs->entries[i].used) { slot = i; break; }
    if (slot < 0) return fs_error("All 128 metadata slots are occupied.");
    unsigned char *image = read_image(fs);
    if (image == NULL) return -1;
    filesystem_t next = *fs;
    file_entry_t *entry = &next.entries[slot];
    memset(entry, 0, sizeof(*entry));
    strcpy(entry->name, name);
    entry->parent_id = parent; entry->type = type;
    entry->first_block = NO_BLOCK; entry->used = 1;
    return commit_state(fs, &next, image);
}

/*
 * Purpose: Resolve a path and require that it names a regular file.
 * Inputs: fs is the state; path is the target; id receives its metadata ID.
 * Return: 0 for a regular file, or -1 on a path or type error.
 */
static int resolve_file(const filesystem_t *fs, const char *path, int *id)
{
    if (resolve_path(fs, path, 0, id, NULL) != 0) return -1;
    if (fs->entries[*id].type != TYPE_FILE) return fs_error("Operation requires a regular file.");
    return 0;
}

/*
 * Purpose: Replace file bytes and adjust its FAT chain for growth or shrinkage.
 * Inputs: fs is the state; id is the file; content supplies length bytes.
 * Return: 0 after committing, or -1 on failure; checks free space before mutation.
 */
static int replace_contents(filesystem_t *fs, int id, const unsigned char *content, uint32_t length)
{
    uint32_t old_count = blocks_for(fs->entries[id].size), new_count = blocks_for(length);
    int chain[NUM_BLOCKS], block = fs->entries[id].first_block;
    uint32_t have = old_count;
    for (uint32_t i = 0; i < old_count; ++i) { chain[i] = block; block = fs->fat[block]; }
    for (int b = DATA_START; b < NUM_BLOCKS && have < new_count; ++b)
        if (fs->fat[b] == FAT_FREE) chain[have++] = b;
    if (have < new_count) return fs_error("Insufficient space; file is unchanged.");
    unsigned char *image = read_image(fs);
    if (image == NULL) return -1;
    filesystem_t next = *fs;
    for (uint32_t i = 0; i < new_count; ++i) {
        uint32_t n = length - i * BLOCK_SIZE;
        if (n > BLOCK_SIZE) n = BLOCK_SIZE;
        unsigned char *dest = image + chain[i] * BLOCK_SIZE;
        memset(dest, 0, BLOCK_SIZE);
        memcpy(dest, content + i * BLOCK_SIZE, n);
        next.fat[chain[i]] = i + 1 < new_count ? chain[i + 1] : FAT_END;
    }
    for (uint32_t i = new_count; i < old_count; ++i) {
        next.fat[chain[i]] = FAT_FREE;
        memset(image + chain[i] * BLOCK_SIZE, 0, BLOCK_SIZE);
    }
    next.entries[id].size = length;
    next.entries[id].first_block = new_count == 0 ? NO_BLOCK : chain[0];
    return commit_state(fs, &next, image);
}

/*
 * Purpose: Reconstruct a file by following its FAT chain.
 * Inputs: fs is the state; id is the file; buffer must hold its recorded size.
 * Return: 0 on success or -1 on read failure; fills buffer without adding a NUL.
 */
static int copy_contents(filesystem_t *fs, int id, unsigned char *buffer)
{
    uint32_t remaining = fs->entries[id].size, offset = 0;
    int block = fs->entries[id].first_block;
    while (remaining > 0) {
        uint32_t n = remaining < BLOCK_SIZE ? remaining : BLOCK_SIZE;
        if (fseek(fs->disk, (long)block * BLOCK_SIZE, SEEK_SET) != 0 ||
            fread(buffer + offset, 1, n, fs->disk) != n) return fs_error("Cannot read file data.");
        offset += n; remaining -= n; block = fs->fat[block];
    }
    return 0;
}

/* ---------- Disk setup ---------- */

/*
 * Purpose: Open an existing disk and validate its persisted filesystem state.
 * Input: fs is the initialized runtime state to populate.
 * Return: 0 to continue, or -1 on a fatal open/read error.
 * Effects: Valid images become ready; missing or invalid images require format.
 */
static int open_virtual_disk(filesystem_t *fs)
{
    fs->disk = fopen(DISK_FILE, "r+b");

    if (fs->disk == NULL) {
        /* It is valid for the disk not to exist before 'format'. */
        if (errno != ENOENT) {
            perror(DISK_FILE);
            return -1;
        }
        return 0;
    }

    if (fseek(fs->disk, 0, SEEK_END) != 0 || ftell(fs->disk) != DISK_SIZE) {
        fs_error("Invalid disk size. Use format to initialize it.");
        return 0;
    }
    unsigned char *image = read_image(fs);
    if (image == NULL) return -1;
    filesystem_t loaded = *fs;
    decode_metadata(&loaded, image);
    free(image);
    if (!valid_state(&loaded)) {
        fs_error("Invalid disk metadata. Use format to initialize it.");
        return 0;
    }
    loaded.ready = 1;
    loaded.cwd_id = 0;
    strcpy(loaded.cwd, "/");
    *fs = loaded;
    return 0;
}

static void close_virtual_disk(filesystem_t *fs)
{
    if (fs->disk != NULL) {
        fclose(fs->disk);
        fs->disk = NULL;
    }
}

/* ---------- Operations students must implement ---------- */

/*
 * Purpose: Initialize a blank disk with reserved metadata and an empty root.
 * Input: fs is the current filesystem state.
 * Return: 0 after committing and resetting cwd to root, or -1 on failure.
 */
static int fs_format(filesystem_t *fs)
{   
    filesystem_t next;
    memset(&next, 0, sizeof(next));
    next.disk = fs->disk;
    next.superblock = (superblock_t){FS_MAGIC, BLOCK_SIZE, NUM_BLOCKS, FS_VERSION,
        FAT_START, FAT_BLOCKS, ENTRY_START, ENTRY_BLOCKS, DATA_START, ENTRY_BYTES, MAX_ENTRIES};
    for (int i = 0; i < MAX_ENTRIES; ++i) next.entries[i].first_block = NO_BLOCK;
    for (int b = 0; b < NUM_BLOCKS; ++b) next.fat[b] = b < DATA_START ? FAT_RESERVED : FAT_FREE;
    strcpy(next.entries[0].name, "/");
    next.entries[0].type = TYPE_DIR;
    next.entries[0].used = 1;
    next.entries[0].parent_id = 0;
    strcpy(next.cwd, "/");
    next.cwd_id = 0;
    next.ready = 1;
    unsigned char *image = calloc(1, DISK_SIZE);
    if (image == NULL) return fs_error("Out of host memory.");
    return commit_state(fs, &next, image);
}

/*
 * Purpose: Create an empty regular file without allocating data blocks.
 * Inputs: fs is the state; path names the new file.
 * Return: 0 after committing the record, or -1 on failure.
 */
static int fs_create(filesystem_t *fs, const char *path)
{    
    return create_entry(fs, path, TYPE_FILE);
}

/*
 * Purpose: Replace all content of an existing regular file.
 * Inputs: fs is the state; path names the file; content is a NUL-terminated string.
 * Return: 0 after committing the replacement, or -1 on failure.
 */
static int fs_write(filesystem_t *fs, const char *path, const char *content)
{
    int id;
    if (resolve_file(fs, path, &id) != 0) return -1;
    size_t length = strlen(content);
    if (length > DATA_CAPACITY) return fs_error("Content exceeds total data capacity.");
    return replace_contents(fs, id, (const unsigned char *)content, (uint32_t)length);
}

/*
 * Purpose: Append text to a regular file while preserving its existing bytes.
 * Inputs: fs is the state; path names the file; content is a NUL-terminated string.
 * Return: 0 on success, including an empty append, or -1 on failure.
 */
static int fs_append(filesystem_t *fs, const char *path, const char *content)
{
    int id;
    if (resolve_file(fs, path, &id) != 0) return -1;
    size_t extra = strlen(content);
    uint32_t old_size = fs->entries[id].size;
    if (extra > DATA_CAPACITY - old_size) return fs_error("Append exceeds total data capacity.");
    if (extra == 0) return 0;
    uint32_t length = old_size + (uint32_t)extra;
    unsigned char *combined = malloc(length);
    if (combined == NULL) return fs_error("Out of host memory.");
    if (copy_contents(fs, id, combined) != 0) { free(combined); return -1; }
    memcpy(combined + old_size, content, extra);
    int result = replace_contents(fs, id, combined, length);
    free(combined);
    return result;
}

/*
 * Purpose: Read a regular file and display its complete contents.
 * Inputs: fs is the state; path names the file.
 * Return: 0 on success or -1 on failure; writes content and a newline to stdout.
 */
static int fs_read(filesystem_t *fs, const char *path)
{
    int id;
    if (resolve_file(fs, path, &id) != 0) return -1;
    uint32_t size = fs->entries[id].size;
    unsigned char *content = malloc(size == 0 ? 1 : size);
    if (content == NULL) return fs_error("Out of host memory.");
    if (copy_contents(fs, id, content) != 0) { free(content); return -1; }
    int ok = fwrite(content, 1, size, stdout) == size;
    free(content);
    if (!ok || putchar('\n') == EOF) return fs_error("Cannot display file content.");
    return 0;
}

/*
 * Purpose: Delete a regular file and release its metadata and data blocks.
 * Inputs: fs is the state; path names the file to remove.
 * Return: 0 after committing the deletion, or -1 on failure.
 */
static int fs_delete(filesystem_t *fs, const char *path)
{
    int id;
    if (resolve_file(fs, path, &id) != 0) return -1;
    unsigned char *image = read_image(fs);
    if (image == NULL) return -1;
    filesystem_t next = *fs;
    int block = next.entries[id].first_block;
    while (block >= DATA_START) {
        int following = next.fat[block];
        next.fat[block] = FAT_FREE;
        memset(image + block * BLOCK_SIZE, 0, BLOCK_SIZE);
        block = following;
    }
    memset(&next.entries[id], 0, sizeof(next.entries[id]));
    next.entries[id].first_block = NO_BLOCK;
    return commit_state(fs, &next, image);
}

/*
 * Purpose: Create an empty directory using a metadata slot only.
 * Inputs: fs is the state; path names the new directory.
 * Return: 0 after committing the record, or -1 on failure.
 */
static int fs_mkdir(filesystem_t *fs, const char *path)
{
    return create_entry(fs, path, TYPE_DIR);
}

/*
 * Purpose: Remove an empty directory while protecting root and nonempty entries.
 * Inputs: fs is the state; path names the directory to remove.
 * Return: 0 after committing, or -1 on failure; moves cwd to its parent if needed.
 */
static int fs_rmdir(filesystem_t *fs, const char *path)
{
    int id;
    if (resolve_path(fs, path, 0, &id, NULL) != 0) return -1;
    if (fs->entries[id].type != TYPE_DIR) return fs_error("rmdir requires a directory.");
    if (id == 0) return fs_error("Cannot remove root.");
    if (child_count(fs, id) != 0) return fs_error("Directory is not empty.");
    unsigned char *image = read_image(fs);
    if (image == NULL) return -1;
    filesystem_t next = *fs;
    int parent = next.entries[id].parent_id;
    memset(&next.entries[id], 0, sizeof(next.entries[id]));
    next.entries[id].first_block = NO_BLOCK;
    if (next.cwd_id == id && set_cwd(&next, parent) != 0) { free(image); return -1; }
    return commit_state(fs, &next, image);
}

/*
 * Purpose: Change the current working directory.
 * Inputs: fs is the state; path must resolve to a directory.
 * Return: 0 after updating cwd and cwd_id, or -1 on failure; does not write the disk.
 */
static int fs_cd(filesystem_t *fs, const char *path)
{
    int id;
    if (resolve_path(fs, path, 0, &id, NULL) != 0) return -1;
    if (fs->entries[id].type != TYPE_DIR) return fs_error("cd requires a directory.");
    return set_cwd(fs, id);
}

/* Print the current path; keep this representation consistent in fs_cd(). */
/*
 * Purpose: Display the absolute current directory path.
 * Input: fs supplies the current path and readiness flag.
 * Effects: Prints cwd when ready, otherwise reports an error; returns no value.
 */
static void fs_pwd(const filesystem_t *fs)
{
    if (require_ready(fs) != 0) return;
    printf("%s\n", fs->cwd);
}

/*
 * Purpose: List the current directory direct children with types and file sizes.
 * Input: fs supplies the metadata table and current directory ID.
 * Return: 0 after printing the listing, or -1 if the filesystem is not ready.
 */
static int fs_ls(filesystem_t *fs)
{
    if (require_ready(fs) != 0) return -1;
    for (int i = 1; i < MAX_ENTRIES; ++i) {
        const file_entry_t *e = &fs->entries[i];
        if (!e->used || e->parent_id != fs->cwd_id) continue;
        if (e->type == TYPE_FILE) printf("file %s %u bytes\n", e->name, (unsigned)e->size);
        else printf("directory %s\n", e->name);
    }
    return 0;
}

/*
 * Purpose: Show file size and block count, or a directory direct-child count.
 * Inputs: fs is the state; path names the object to inspect.
 * Return: 0 after printing the information, or -1 on a resolution error.
 */
static int fs_stat(filesystem_t *fs, const char *path)
{
    int id;
    if (resolve_path(fs, path, 0, &id, NULL) != 0) return -1;
    const file_entry_t *e = &fs->entries[id];
    if (e->type == TYPE_FILE)
        printf("name=%s type=file size=%u blocks=%u\n", e->name, (unsigned)e->size,
               (unsigned)blocks_for(e->size));
    else printf("name=%s type=directory children=%d\n", e->name, child_count(fs, id));
    return 0;
}

/*
 * Purpose: Report total, used, free and reserved blocks and the block size.
 * Input: fs supplies the FAT and readiness flag.
 * Return: 0 after printing disk usage, or -1 if the filesystem is not ready.
 */
static int fs_df(filesystem_t *fs)
{
    if (require_ready(fs) != 0) return -1;
    int free_blocks = 0;
    for (int b = DATA_START; b < NUM_BLOCKS; ++b)
        if (fs->fat[b] == FAT_FREE) ++free_blocks;
    printf("total=%d used=%d free=%d metadata=%d block_size=%d\n",
           NUM_BLOCKS, NUM_BLOCKS - free_blocks, free_blocks, DATA_START, BLOCK_SIZE);
    return 0;
}

/* ---------- Command interpreter ---------- */

static int require_arg(const char *cmd, const char *arg)
{
    if (arg == NULL || *arg == '\0') {
        fprintf(stderr, "Usage error: %s requires an argument.\n", cmd);
        return 0;
    }
    return 1;
}

/* Reject extra arguments before calling an operation. */
static int no_extra_args(const char *cmd, char *cursor)
{
    if (*skip_spaces(cursor) != '\0') {
        fprintf(stderr, "Usage error: unexpected arguments for %s.\n", cmd);
        return 0;
    }
    return 1;
}

/* Remove only the enclosing quotes; the returned content may be empty. */
/*
 * Purpose: Validate quoted command text and remove its enclosing quotes.
 * Inputs: cmd names the command; cursor points to writable remaining input.
 * Return: A pointer into the modified input, or NULL on invalid quoting.
 */
static char *quoted_content(const char *cmd, char *cursor)
{
    char *content = skip_spaces(cursor);
    size_t length = strlen(content);
    while (length > 0 && isspace((unsigned char)content[length - 1])) {
        --length;
    }
    if (length < 2 || content[0] != '"' || content[length - 1] != '"') {
        fprintf(stderr, "Usage: %s <path> \"<content>\"\n", cmd);
        return NULL;
    }
    for (size_t i = 1; i + 1 < length; ++i) {
        if (content[i] == '"') {
            fprintf(stderr, "Usage error: unexpected quote in %s content.\n", cmd);
            return NULL;
        }
    }
    content[length - 1] = '\0';
    return content + 1;
}

static void execute_command(filesystem_t *fs, char *line, int *running)
{
    char *cursor = line;
    char *cmd = next_token(&cursor);
    char *arg;

    if (cmd == NULL) {
        return;
    }

    if (strcmp(cmd, "format") == 0) {
        if (no_extra_args(cmd, cursor)) fs_format(fs);
    } else if (strcmp(cmd, "create") == 0) {
        arg = next_token(&cursor);
        if (require_arg(cmd, arg) && no_extra_args(cmd, cursor)) fs_create(fs, arg);
    } else if (strcmp(cmd, "write") == 0) {
        arg = next_token(&cursor);
        if (require_arg(cmd, arg)) {
            char *content = quoted_content(cmd, cursor);
            if (content != NULL) fs_write(fs, arg, content);
        }
    } else if (strcmp(cmd, "append") == 0) {
        arg = next_token(&cursor);
        if (require_arg(cmd, arg)) {
            char *content = quoted_content(cmd, cursor);
            if (content != NULL) fs_append(fs, arg, content);
        }
    } else if (strcmp(cmd, "read") == 0) {
        arg = next_token(&cursor);
        if (require_arg(cmd, arg) && no_extra_args(cmd, cursor)) fs_read(fs, arg);
    } else if (strcmp(cmd, "delete") == 0) {
        arg = next_token(&cursor);
        if (require_arg(cmd, arg) && no_extra_args(cmd, cursor)) fs_delete(fs, arg);
    } else if (strcmp(cmd, "mkdir") == 0) {
        arg = next_token(&cursor);
        if (require_arg(cmd, arg) && no_extra_args(cmd, cursor)) fs_mkdir(fs, arg);
    } else if (strcmp(cmd, "rmdir") == 0) {
        arg = next_token(&cursor);
        if (require_arg(cmd, arg) && no_extra_args(cmd, cursor)) fs_rmdir(fs, arg);
    } else if (strcmp(cmd, "cd") == 0) {
        arg = next_token(&cursor);
        if (require_arg(cmd, arg) && no_extra_args(cmd, cursor)) fs_cd(fs, arg);
    } else if (strcmp(cmd, "pwd") == 0) {
        if (no_extra_args(cmd, cursor)) fs_pwd(fs);
    } else if (strcmp(cmd, "ls") == 0) {
        if (no_extra_args(cmd, cursor)) fs_ls(fs);
    } else if (strcmp(cmd, "stat") == 0) {
        arg = next_token(&cursor);
        if (require_arg(cmd, arg) && no_extra_args(cmd, cursor)) fs_stat(fs, arg);
    } else if (strcmp(cmd, "df") == 0) {
        if (no_extra_args(cmd, cursor)) fs_df(fs);
    } else if (strcmp(cmd, "exit") == 0) {
        if (no_extra_args(cmd, cursor)) *running = 0;
    } else {
        fprintf(stderr, "Unknown command: %s\n", cmd);
    }
}

/*
 * Purpose: Initialize the simulator and process commands until exit or EOF.
 * Inputs: None; reads command lines from stdin.
 * Return: EXIT_SUCCESS after normal shutdown, or EXIT_FAILURE on startup/input error.
 * Effects: Dispatches commands and releases the input buffer and disk stream.
 */
int main(void)
{
    filesystem_t fs;
    char *line = NULL;
    size_t line_capacity = 0;
    int running = 1;

    memset(&fs, 0, sizeof(fs));
    strcpy(fs.cwd, "/");

    if (open_virtual_disk(&fs) != 0) {
        fprintf(stderr, "Failed to initialize the virtual disk.\n");
        return EXIT_FAILURE;
    }

    printf("Mini File System Simulator\n");
    printf("Student starter: file-system operations are TODOs.\n");
    print_commands();

    while (running) {
        printf("minifs> ");
        fflush(stdout);

        /* Read a whole command; no assignment-specific line limit is imposed. */
        ssize_t line_length = getline(&line, &line_capacity, stdin);
        if (line_length == -1) {
            if (ferror(stdin) || !feof(stdin)) {
                perror("command input");
                free(line);
                close_virtual_disk(&fs);
                return EXIT_FAILURE;
            }
            putchar('\n');
            break;
        }

        if (memchr(line, '\0', (size_t)line_length) != NULL) {
            fs_error("NUL bytes are not supported in command text.");
            continue;
        }
        trim_newline(line);
        execute_command(&fs, line, &running);
    }

    free(line);
    close_virtual_disk(&fs);
    return EXIT_SUCCESS;
}
