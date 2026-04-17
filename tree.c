// tree.c — Tree object serialization and construction
//
// PROVIDED functions: get_file_mode, tree_parse, tree_serialize
// TODO functions: tree_from_index
//
// Binary tree format (per entry, concatenated with no separators):
//   "<mode-as-ascii-octal> <name>\0<32-byte-binary-hash>"
//
// Example single entry (conceptual):
//   "100644 hello.txt\0" followed by 32 raw bytes of SHA-256

#include "tree.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>

// ─── Mode Constants ─────────────────────────────────────────────────────────
#define MODE_FILE 0100644
#define MODE_EXEC 0100755
#define MODE_DIR  0040000

// ─── PROVIDED ───────────────────────────────────────────────────────────────

// Determine the object mode for a filesystem path.
uint32_t get_file_mode(const char *path) {
    struct stat st;
    if (lstat(path, &st) != 0) return 0;
    if (S_ISDIR(st.st_mode))      return MODE_DIR;
    if (st.st_mode & S_IXUSR)     return MODE_EXEC;
    return MODE_FILE;
}

// Parse binary tree data into a Tree struct safely.
// Returns 0 on success, -1 on parse error.
int tree_parse(const void *data, size_t len, Tree *tree_out) {
    tree_out->count = 0;
    const uint8_t *ptr = (const uint8_t *)data;
    const uint8_t *end = ptr + len;

    while (ptr < end && tree_out->count < MAX_TREE_ENTRIES) {
        TreeEntry *entry = &tree_out->entries[tree_out->count];

        // 1. Safely find the space character for the mode
        const uint8_t *space = memchr(ptr, ' ', end - ptr);
        if (!space) return -1; // Malformed data

        // Parse mode into an isolated buffer
        char mode_str[16] = {0};
        size_t mode_len = space - ptr;
        if (mode_len >= sizeof(mode_str)) return -1;
        memcpy(mode_str, ptr, mode_len);
        entry->mode = strtol(mode_str, NULL, 8);
        ptr = space + 1; // Skip space

        // 2. Safely find the null terminator for the name
        const uint8_t *null_byte = memchr(ptr, '\0', end - ptr);
        if (!null_byte) return -1; // Malformed data

        size_t name_len = null_byte - ptr;
        if (name_len >= sizeof(entry->name)) return -1;
        memcpy(entry->name, ptr, name_len);
        entry->name[name_len] = '\0'; // Ensure null-terminated
        ptr = null_byte + 1;          // Skip null byte

        // 3. Read the 32-byte binary hash
        if (ptr + HASH_SIZE > end) return -1;
        memcpy(entry->hash.hash, ptr, HASH_SIZE);
        ptr += HASH_SIZE;

        tree_out->count++;
    }
    return 0;
}

// Helper for qsort to ensure consistent tree hashing
static int compare_tree_entries(const void *a, const void *b) {
    return strcmp(((const TreeEntry *)a)->name, ((const TreeEntry *)b)->name);
}

// Serialize a Tree struct into binary format for storage.
// Caller must free(*data_out).
// Returns 0 on success, -1 on error.
int tree_serialize(const Tree *tree, void **data_out, size_t *len_out) {
    // Estimate max size: (6 bytes mode + 1 byte space + 256 bytes name + 1 byte null + 32 bytes hash) per entry
    size_t max_size = tree->count * 296;
    uint8_t *buffer = malloc(max_size);
    if (!buffer) return -1;

    // Create a mutable copy to sort entries (Git requirement)
    Tree sorted_tree = *tree;
    qsort(sorted_tree.entries, sorted_tree.count, sizeof(TreeEntry), compare_tree_entries);

    size_t offset = 0;
    for (int i = 0; i < sorted_tree.count; i++) {
        const TreeEntry *entry = &sorted_tree.entries[i];

        // Write mode and name (%o writes octal correctly for Git standards)
        int written = sprintf((char *)buffer + offset, "%o %s", entry->mode, entry->name);
        offset += written + 1; // +1 to step over the null terminator written by sprintf

        // Write binary hash
        memcpy(buffer + offset, entry->hash.hash, HASH_SIZE);
        offset += HASH_SIZE;
    }

    *data_out = buffer;
    *len_out  = offset;
    return 0;
}

// ─── TODO: Implement these ──────────────────────────────────────────────────

// Forward declaration for the recursive helper
int object_write(ObjectType type, const void *data, size_t len, ObjectID *id_out);

// Internal recursive helper:
// Given a flat list of IndexEntry-style records (path + hash + mode),
// this function groups the entries at the current directory depth,
// recursively writes subtree objects for subdirectories,
// then serialises and writes the current level's tree object.
//
// 'entries'  : array of pointers to IndexEntry
// 'count'    : how many entries in this call
// 'prefix'   : the directory prefix already consumed (e.g. "src/")
//              used to strip leading path components from names
// 'id_out'   : receives the ObjectID of the written tree object

// We need access to IndexEntry; pull in the header.
#include "index.h"

static int write_tree_recursive(IndexEntry **entries, int count,
                                const char *prefix, ObjectID *id_out) {
    Tree tree;
    tree.count = 0;

    int i = 0;
    while (i < count) {
        // Determine the name at this level (strip the prefix)
        const char *rel = entries[i]->path + strlen(prefix);

        // Is there a '/' in the remaining path? → this is a subdirectory
        const char *slash = strchr(rel, '/');

        if (!slash) {
            // ── Blob entry (plain file at this level) ───────────────────────
            TreeEntry *te = &tree.entries[tree.count];
            strncpy(te->name, rel, sizeof(te->name) - 1);
            te->name[sizeof(te->name) - 1] = '\0';
            te->mode = entries[i]->mode;
            te->hash = entries[i]->hash;
            tree.count++;
            i++;
        } else {
            // ── Directory entry (subtree) ────────────────────────────────────
            // Collect all entries that share this subdirectory name
            size_t dir_name_len = (size_t)(slash - rel);
            char dir_name[256];
            if (dir_name_len >= sizeof(dir_name)) return -1;
            memcpy(dir_name, rel, dir_name_len);
            dir_name[dir_name_len] = '\0';

            // Build the new prefix for the recursive call
            char new_prefix[1024];
            snprintf(new_prefix, sizeof(new_prefix), "%s%s/", prefix, dir_name);

            // Find the range of entries that belong to this subdirectory
            int j = i;
            while (j < count) {
                const char *r = entries[j]->path + strlen(prefix);
                const char *s = strchr(r, '/');
                if (!s) break; // no slash → different level
                size_t d = (size_t)(s - r);
                if (d != dir_name_len || strncmp(r, dir_name, d) != 0) break;
                j++;
            }

            // Recursively write the subtree
            ObjectID sub_id;
            if (write_tree_recursive(entries + i, j - i, new_prefix, &sub_id) != 0)
                return -1;

            // Add a tree entry for this subdirectory
            TreeEntry *te = &tree.entries[tree.count];
            strncpy(te->name, dir_name, sizeof(te->name) - 1);
            te->name[sizeof(te->name) - 1] = '\0';
            te->mode = MODE_DIR;
            te->hash = sub_id;
            tree.count++;

            i = j; // skip past all entries in this subdirectory
        }
    }

    // Serialize and write this level's tree object
    void   *tree_data = NULL;
    size_t  tree_len  = 0;
    if (tree_serialize(&tree, &tree_data, &tree_len) != 0) return -1;

    int rc = object_write(OBJ_TREE, tree_data, tree_len, id_out);
    free(tree_data);
    return rc;
}

// Comparator for sorting IndexEntry pointers by path (for deterministic trees)
static int cmp_entry_ptr(const void *a, const void *b) {
    const IndexEntry *ea = *(const IndexEntry **)a;
    const IndexEntry *eb = *(const IndexEntry **)b;
    return strcmp(ea->path, eb->path);
}

// Build a tree hierarchy from the current index and write all tree
// objects to the object store.
//
// Returns 0 on success, -1 on error.
int tree_from_index(ObjectID *id_out) {
    // Load the current index
    Index index;
    index.count = 0;
    if (index_load(&index) != 0 && index.count == 0) {
        // Empty index — write an empty tree
        Tree empty;
        empty.count = 0;
        void  *data = NULL;
        size_t dlen = 0;
        if (tree_serialize(&empty, &data, &dlen) != 0) return -1;
        int rc = object_write(OBJ_TREE, data, dlen, id_out);
        free(data);
        return rc;
    }

    // Build a sorted array of pointers to entries
    IndexEntry **ptrs = malloc((size_t)index.count * sizeof(IndexEntry *));
    if (!ptrs) return -1;
    for (int i = 0; i < index.count; i++) ptrs[i] = &index.entries[i];
    qsort(ptrs, (size_t)index.count, sizeof(IndexEntry *), cmp_entry_ptr);

    // Recursively build the tree starting from the root prefix ""
    int rc = write_tree_recursive(ptrs, index.count, "", id_out);
    free(ptrs);
    return rc;
}
