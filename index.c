#include "index.h"
#include "object.h"
#include "pes.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

/* ------------------------------------------------------------------ */
/*  index_load                                                          */
/* ------------------------------------------------------------------ */
int index_load(Index *idx) {
    memset(idx, 0, sizeof(*idx));

    char index_path[512];
    snprintf(index_path, sizeof(index_path), ".pes/index");

    FILE *f = fopen(index_path, "r");
    if (!f) {
        if (errno == ENOENT) return 0;
        perror("index_load: fopen");
        return -1;
    }

    char line[2048];
    while (fgets(line, sizeof(line), f)) {
        if (line[0] == '\n' || line[0] == '\0') continue;
        if (idx->count >= MAX_INDEX_ENTRIES) {
            fprintf(stderr, "index_load: too many entries\n");
            break;
        }

        IndexEntry *e = &idx->entries[idx->count];
        unsigned int mode;
        char hash[HEX_SIZE + 1];
        long mtime;
        size_t size;
        char path[512];

        int matched = sscanf(line, "%o %64s %ld %zu %511s",
                             &mode, hash, &mtime, &size, path);
        if (matched != 5) {
            fprintf(stderr, "index_load: malformed line, skipping: %s", line);
            continue;
        }

        e->mode  = mode;
        e->mtime = (time_t)mtime;
        e->size  = size;
        strncpy(e->hash, hash, HEX_SIZE);
        e->hash[HEX_SIZE] = '\0';
        strncpy(e->path, path, sizeof(e->path) - 1);
        e->path[sizeof(e->path) - 1] = '\0';

        idx->count++;
    }

    fclose(f);
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Comparator for qsort                                               */
/* ------------------------------------------------------------------ */
static int entry_cmp(const void *a, const void *b) {
    const IndexEntry *ea = (const IndexEntry *)a;
    const IndexEntry *eb = (const IndexEntry *)b;
    return strcmp(ea->path, eb->path);
}

/* ------------------------------------------------------------------ */
/*  index_save — sorted, atomic, fsync-safe write                      */
/* ------------------------------------------------------------------ */
int index_save(const Index *idx) {
    char index_path[512];
    char tmp_path[512];
    snprintf(index_path, sizeof(index_path), ".pes/index");
    snprintf(tmp_path,   sizeof(tmp_path),   ".pes/index.tmp");

    Index sorted = *idx;
    qsort(sorted.entries, sorted.count, sizeof(IndexEntry), entry_cmp);

    int fd = open(tmp_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        perror("index_save: open tmp");
        return -1;
    }

    FILE *f = fdopen(fd, "w");
    if (!f) {
        perror("index_save: fdopen");
        close(fd);
        return -1;
    }

    for (int i = 0; i < sorted.count; i++) {
        const IndexEntry *e = &sorted.entries[i];
        fprintf(f, "%o %s %ld %zu %s\n",
                e->mode,
                e->hash,
                (long)e->mtime,
                e->size,
                e->path);
    }

    fflush(f);

    if (fsync(fd) != 0) {
        perror("index_save: fsync");
        fclose(f);
        return -1;
    }

    fclose(f);

    if (rename(tmp_path, index_path) != 0) {
        perror("index_save: rename");
        return -1;
    }

    return 0;
}

/* ------------------------------------------------------------------ */
/*  index_add                                                           */
/*  Reads filepath from disk, writes a blob object to the store,      */
/*  then inserts or updates its entry in the in-memory Index.         */
/* ------------------------------------------------------------------ */
int index_add(Index *idx, const char *filepath) {
    /* ----- 1. Read the file into memory ----------------------------- */
    FILE *f = fopen(filepath, "rb");
    if (!f) {
        fprintf(stderr, "index_add: cannot open '%s': %s\n",
                filepath, strerror(errno));
        return -1;
    }

    /* Determine size */
    if (fseek(f, 0, SEEK_END) != 0) { perror("index_add: fseek"); fclose(f); return -1; }
    long file_size = ftell(f);
    if (file_size < 0)               { perror("index_add: ftell");  fclose(f); return -1; }
    rewind(f);

    uint8_t *data = malloc((size_t)file_size);
    if (!data) { fprintf(stderr, "index_add: out of memory\n"); fclose(f); return -1; }

    size_t nread = fread(data, 1, (size_t)file_size, f);
    fclose(f);

    if ((long)nread != file_size) {
        fprintf(stderr, "index_add: short read on '%s'\n", filepath);
        free(data);
        return -1;
    }

    /* ----- 2. Write blob object to the object store ----------------- */
    char hash[HEX_SIZE + 1];
    if (object_write(OBJ_BLOB, data, (size_t)file_size, hash) != 0) {
        fprintf(stderr, "index_add: object_write failed for '%s'\n", filepath);
        free(data);
        return -1;
    }
    free(data);

    /* ----- 3. Stat the file to capture mtime and size -------------- */
    struct stat st;
    if (stat(filepath, &st) != 0) {
        perror("index_add: stat");
        return -1;
    }

    /*
     * Determine the mode.
     * 100755 if executable, 100644 otherwise.
     * We use the same octal values Git uses.
     */
    uint32_t mode = (st.st_mode & S_IXUSR) ? 0100755 : 0100644;

    /* ----- 4. Insert or update the index entry --------------------- */
    IndexEntry *existing = index_find(idx, filepath);
    if (existing) {
        /* Update in place */
        existing->mode  = mode;
        existing->mtime = st.st_mtime;
        existing->size  = (size_t)st.st_size;
        strncpy(existing->hash, hash, HEX_SIZE);
        existing->hash[HEX_SIZE] = '\0';
    } else {
        /* Append a new entry */
        if (idx->count >= MAX_INDEX_ENTRIES) {
            fprintf(stderr, "index_add: index full\n");
            return -1;
        }
        IndexEntry *e = &idx->entries[idx->count];
        e->mode  = mode;
        e->mtime = st.st_mtime;
        e->size  = (size_t)st.st_size;
        strncpy(e->hash, hash, HEX_SIZE);
        e->hash[HEX_SIZE] = '\0';
        strncpy(e->path, filepath, sizeof(e->path) - 1);
        e->path[sizeof(e->path) - 1] = '\0';
        idx->count++;
    }

    return 0;
}
