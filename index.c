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
    /* Zero out the struct so we start with an empty index */
    memset(idx, 0, sizeof(*idx));

    /* Build the path to .pes/index */
    char index_path[512];
    snprintf(index_path, sizeof(index_path), ".pes/index");

    FILE *f = fopen(index_path, "r");
    if (!f) {
        /* A missing index file is NOT an error — it just means empty */
        if (errno == ENOENT) {
            return 0;
        }
        perror("index_load: fopen");
        return -1;
    }

    fclose(f);
    return 0;  /* parsing added in next commit */
}

/* ------------------------------------------------------------------ */
/*  index_save  (stub — implemented in later commit)                   */
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
