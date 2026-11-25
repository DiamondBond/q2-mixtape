/* q2-mixtape: the app's server, or with "scan" its listening child, or with "covers" its covers'
 * child (scan.c). */
#include "mixtape.h"
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1], "scan")) return scan_main(argc > 2 ? atoi(argv[2]) : 0);
    if (argc > 1 && !strcmp(argv[1], "covers")) return covers_main();
    return serve();
}
