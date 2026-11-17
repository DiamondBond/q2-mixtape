/* q2-mixtape: the app's server, or with "scan" its listening child (scan.c). */
#include "mixtape.h"
#include <string.h>

int main(int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1], "scan")) return scan_main();
    return serve();
}
