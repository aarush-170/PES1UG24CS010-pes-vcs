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
/*  Reads .pes/index into an Index struct.                             */
/*  Format per line: <mode> <hash> <mtime> <size> <path>              */
/* ------------------------------------------------------------------ */
int index_load(Index *idx) {
    memset(idx, 0, sizeof(*idx));

    char index_path[512];
    snprintf(index_path, sizeof(index_path), ".pes/index");

    FILE *f = fopen(index_path, "r");
    if (!f) {
        if (errno == ENOENT) {
            return 0;   /* empty index — not an error */
        }
        perror("index_load: fopen");
        return -1;
    }

    char line[2048];
    while (fgets(line, sizeof(line), f)) {
        /* Skip blank lines */
        if (line[0] == '\n' || line[0] == '\0') continue;

        if (idx->count >= MAX_INDEX_ENTRIES) {
            fprintf(stderr, "index_load: too many entries\n");
            break;
        }

        IndexEntry *e = &idx->entries[idx->count];

        /*
         * sscanf into temporary variables first so a malformed line
         * doesn't partially populate an entry.
         */
        unsigned int mode;
        char hash[HEX_SIZE + 1];    /* HEX_SIZE defined in pes.h */
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
/*  index_save  (stub — implemented in next commit)                    */
/* ------------------------------------------------------------------ */
int index_save(const Index *idx) {
    (void)idx;
    return 0;
}

/* ------------------------------------------------------------------ */
/*  index_add   (stub — implemented in later commit)                   */
/* ------------------------------------------------------------------ */
int index_add(Index *idx, const char *filepath) {
    (void)idx;
    (void)filepath;
    return 0;
}
