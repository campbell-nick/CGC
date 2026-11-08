#include "pack.h"
#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv)
{
    if (argc != 3) {
        fprintf(stderr, "usage: packtest <packfile> <gitdir>\n");
        return 2;
    }

    Buf pack;
    buf_init(&pack);
    if (read_file(argv[1], &pack) != 0) {
        fprintf(stderr, "packtest: cannot read %s\n", argv[1]);
        return 1;
    }

    PackStats stats;
    memset(&stats, 0, sizeof(stats));
    int rc = pack_unpack(pack.data, pack.len, argv[2], &stats, NULL, NULL);
    buf_free(&pack);
    if (rc != 0) {
        fprintf(stderr, "packtest: unpack failed\n");
        return 1;
    }

    printf("%u %u\n", stats.objects, stats.deltas);
    return 0;
}
