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
/*  index_save — atomic write with fsync                               */
/* ------------------------------------------------------------------ */
int index_save(const Index *idx) {
    char index_path[512];
    char tmp_path[512];
    snprintf(index_path, sizeof(index_path), ".pes/index");
    snprintf(tmp_path,   sizeof(tmp_path),   ".pes/index.tmp");

    Index sorted = *idx;
    qsort(sorted.entries, sorted.count, sizeof(IndexEntry), entry_cmp);

    /*
     * Open with O_WRONLY | O_CREAT | O_TRUNC so we can get the
     * file descriptor for fsync() before closing.
     */
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

    /*
     * fsync ensures bytes are on disk before we rename.
     * Without this, a crash between rename and disk flush could
     * leave us with an empty index.
     */
    if (fsync(fd) != 0) {
        perror("index_save: fsync");
        fclose(f);
        return -1;
    }

    fclose(f);   /* also closes fd */

    if (rename(tmp_path, index_path) != 0) {
        perror("index_save: rename");
        return -1;
    }

    return 0;
}

/* ------------------------------------------------------------------ */
/*  index_add   (stub — implemented in next commit)                    */
/* ------------------------------------------------------------------ */
int index_add(Index *idx, const char *filepath) {
    (void)idx;
    (void)filepath;
    return 0;
}
