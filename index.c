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

/*
 * index.c — Staging area implementation for PES-VCS.
 *
 * The index lives at .pes/index as a plain-text file.
 * Each line has the form:
 *
 *   <mode(octal)> <sha256-hex> <mtime> <size> <path>
 *
 * e.g.:
 *   100644 a3f2b1... 1699900000 42 src/main.c
 *
 * Atomic saves: we write to .pes/index.tmp, fsync, then rename into place.
 * This guarantees the index is never in a half-written state even if the
 * process is killed mid-write.
 */

/* ------------------------------------------------------------------ */
/*  index_load                                                          */
/* ------------------------------------------------------------------ */
int index_load(Index *idx) {
    if (!idx) return -1;
    memset(idx, 0, sizeof(*idx));

    const char *index_path = ".pes/index";

    FILE *f = fopen(index_path, "r");
    if (!f) {
        /*
         * ENOENT just means no commits yet — treat as an empty index.
         * Any other error (permissions, I/O) is a real failure.
         */
        if (errno == ENOENT) return 0;
        perror("index_load: fopen");
        return -1;
    }

    char line[2048];
    while (fgets(line, sizeof(line), f)) {
        /* Skip blank lines silently */
        size_t len = strlen(line);
        if (len == 0 || line[0] == '\n') continue;

        /* Strip trailing newline for cleaner sscanf matching */
        if (line[len - 1] == '\n') line[len - 1] = '\0';

        if (idx->count >= MAX_INDEX_ENTRIES) {
            fprintf(stderr, "index_load: index is full (%d entries), "
                            "truncating\n", MAX_INDEX_ENTRIES);
            break;
        }

        IndexEntry *e = &idx->entries[idx->count];
        unsigned int mode;
        char hash[HEX_SIZE + 1];
        long mtime;
        size_t sz;
        char path[512];

        int matched = sscanf(line, "%o %64s %ld %zu %511s",
                             &mode, hash, &mtime, &sz, path);
        if (matched != 5) {
            fprintf(stderr, "index_load: skipping malformed line: [%s]\n", line);
            continue;
        }

        e->mode  = (uint32_t)mode;
        e->mtime = (time_t)mtime;
        e->size  = sz;
        memcpy(e->hash, hash, HEX_SIZE);
        e->hash[HEX_SIZE] = '\0';
        strncpy(e->path, path, sizeof(e->path) - 1);
        e->path[sizeof(e->path) - 1] = '\0';

        idx->count++;
    }

    fclose(f);
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Comparator: sort entries alphabetically by path (like Git does)   */
/* ------------------------------------------------------------------ */
static int entry_cmp(const void *a, const void *b) {
    return strcmp(((const IndexEntry *)a)->path,
                  ((const IndexEntry *)b)->path);
}

/* ------------------------------------------------------------------ */
/*  index_save                                                          */
/* ------------------------------------------------------------------ */
int index_save(const Index *idx) {
    if (!idx) return -1;

    const char *index_path = ".pes/index";
    const char *tmp_path   = ".pes/index.tmp";

    /* Sort a local copy — do not mutate the caller's struct */
    Index sorted = *idx;
    qsort(sorted.entries, (size_t)sorted.count, sizeof(IndexEntry), entry_cmp);

    /*
     * Open with O_WRONLY so we can call fsync() on the fd directly.
     * fdopen() wraps the fd in a FILE* for convenient fprintf().
     */
    int fd = open(tmp_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        perror("index_save: open");
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
        if (fprintf(f, "%o %s %ld %zu %s\n",
                    e->mode, e->hash, (long)e->mtime, e->size, e->path) < 0) {
            perror("index_save: fprintf");
            fclose(f);
            return -1;
        }
    }

    if (fflush(f) != 0) {
        perror("index_save: fflush");
        fclose(f);
        return -1;
    }

    /*
     * fsync() flushes the kernel's page cache to physical storage.
     * Without this, rename() could succeed but the data might not
     * survive a sudden power loss.
     */
    if (fsync(fd) != 0) {
        perror("index_save: fsync");
        fclose(f);
        return -1;
    }

    fclose(f);   /* fd is closed here too */

    /* Atomic replace — rename() is POSIX-atomic on the same filesystem */
    if (rename(tmp_path, index_path) != 0) {
        perror("index_save: rename");
        unlink(tmp_path);   /* clean up the orphaned temp file */
        return -1;
    }

    return 0;
}

/* ------------------------------------------------------------------ */
/*  index_add                                                           */
/* ------------------------------------------------------------------ */
int index_add(Index *idx, const char *filepath) {
    if (!idx || !filepath) return -1;

    /* ----- Step 1: Slurp the file into memory --------------------- */
    FILE *f = fopen(filepath, "rb");
    if (!f) {
        fprintf(stderr, "index_add: cannot open '%s': %s\n",
                filepath, strerror(errno));
        return -1;
    }

    if (fseek(f, 0, SEEK_END) != 0) {
        perror("index_add: fseek");
        fclose(f);
        return -1;
    }

    long file_size = ftell(f);
    if (file_size < 0) {
        perror("index_add: ftell");
        fclose(f);
        return -1;
    }
    rewind(f);

    /*
     * Allocate 1 extra byte so we can safely handle a 0-byte file
     * without passing NULL to object_write.
     */
    uint8_t *data = malloc((size_t)file_size + 1);
    if (!data) {
        fprintf(stderr, "index_add: out of memory\n");
        fclose(f);
        return -1;
    }

    size_t nread = fread(data, 1, (size_t)file_size, f);
    fclose(f);

    if ((long)nread != file_size) {
        fprintf(stderr, "index_add: short read on '%s' "
                        "(expected %ld, got %zu)\n",
                filepath, file_size, nread);
        free(data);
        return -1;
    }

    /* ----- Step 2: Store blob in the object store ----------------- */
    char hash[HEX_SIZE + 1];
    if (object_write(OBJ_BLOB, data, (size_t)file_size, hash) != 0) {
        fprintf(stderr, "index_add: object_write failed for '%s'\n", filepath);
        free(data);
        return -1;
    }
    free(data);

    /* ----- Step 3: Stat the file for metadata --------------------- */
    struct stat st;
    if (stat(filepath, &st) != 0) {
        perror("index_add: stat");
        return -1;
    }

    /*
     * Mode encoding mirrors Git:
     *   100755 — regular file, executable
     *   100644 — regular file, not executable
     */
    uint32_t mode = (st.st_mode & S_IXUSR) ? 0100755u : 0100644u;

    /* ----- Step 4: Update or insert the index entry --------------- */
    IndexEntry *existing = index_find(idx, filepath);
    if (existing) {
        existing->mode  = mode;
        existing->mtime = st.st_mtime;
        existing->size  = (size_t)st.st_size;
        memcpy(existing->hash, hash, HEX_SIZE + 1);
    } else {
        if (idx->count >= MAX_INDEX_ENTRIES) {
            fprintf(stderr, "index_add: index is full, cannot add '%s'\n",
                    filepath);
            return -1;
        }
        IndexEntry *e = &idx->entries[idx->count];
        e->mode  = mode;
        e->mtime = st.st_mtime;
        e->size  = (size_t)st.st_size;
        memcpy(e->hash, hash, HEX_SIZE + 1);
        strncpy(e->path, filepath, sizeof(e->path) - 1);
        e->path[sizeof(e->path) - 1] = '\0';
        idx->count++;
    }

    return 0;
}
