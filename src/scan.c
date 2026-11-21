/* Listening, the app's child process (q2-mixtape scan, nice 19): every Library song whose file
 * has no profile yet, or changed size since, is heard through the Q2's ffmpeg, and its profile
 * appended to the card's file at once, so nothing is lost when it stops. Its progress goes to
 * SCAN_FILE for the page; it ends on a TERM, killing its ffmpeg. */
#include "mixtape.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define COMPACT_SLACK 1000 /* stale records the file may carry before it is written afresh */
#ifdef HOST
#define FFMPEG "ffmpeg"
#else
#define FFMPEG "/usr/bin/ffmpeg" /* the Q2's own, 4.2 */
#endif
/* ffmpeg's time for one song; a song it is still on (a damaged file, a card that stalls) is left
 * for next time. Not marked unheard: at nice 19 under playback a slow song isn't a bad one. */
#define FFMPEG_MS 120000

static volatile sig_atomic_t quit;
static volatile pid_t child;
static void on_term(int sig) {
    (void)sig;
    quit = 1;
    if (child > 0) kill(child, SIGKILL);
}

static long long now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (long long)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

/* ffmpeg with args, its stdout (out) or stderr read into buf; the bytes read, -1 when it was
 * killed (by a TERM here, for memory, or past FFMPEG_MS) and -2 when there is no ffmpeg. One that
 * crashed on a file counts as done: what it gave is all there is. */
static int run(char **args, int out, char *buf, int size) {
    int fd[2], n = 0, status = 0;
    long long end = now_ms() + FFMPEG_MS;
#ifdef HOST /* the checks wait seconds, not minutes */
    if (getenv("MIXTAPE_FFMPEG_MS")) end = now_ms() + atoi(getenv("MIXTAPE_FFMPEG_MS"));
#endif
    if (pipe(fd)) return -1;
    pid_t pid = fork();
    if (!pid) {
        int null = open("/dev/null", O_RDWR);
        dup2(null, 0), dup2(out ? fd[1] : null, 1), dup2(out ? null : fd[1], 2);
        close(fd[0]), close(fd[1]);
        execvp(FFMPEG, args);
        _exit(127);
    }
    close(fd[1]);
    child = pid;
    if (quit && pid > 0) kill(pid, SIGKILL); /* a TERM before child was set */
    for (int got; pid > 0 && n < size;) {
        struct pollfd p = { fd[0], POLLIN, 0 };
        long long left = end - now_ms();
        if (left <= 0 || !(got = poll(&p, 1, (int)left))) {
            kill(pid, SIGKILL);
            break;
        }
        if (got > 0 && !(got = (int)read(fd[0], buf + n, (size_t)(size - n)))) break;
        if (got > 0)
            n += got;
        else if (errno != EINTR)
            break;
    }
    close(fd[0]);
    while (pid > 0 && waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    child = 0;
    if (pid <= 0 || (WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL)) return -1;
    return WEXITSTATUS(status) == 127 ? -2 : n;
}

/* path's length in whole seconds from ffmpeg's "Duration: 00:03:21.45"; 0 unknown.
 * ponytail: the first 16 KB of its report only, which tags of lyrics can fill before the length;
 * then the clip is the first 30 s. Asked only of songs Q2 Pod hasn't measured. */
static int probe(const char *path) {
    char buf[16384], *args[] = { "ffmpeg", "-nostdin", "-hide_banner", "-i", (char *)path, 0 };
    int n = run(args, 0, buf, sizeof buf - 1), h, m, s;
    if (n < 0) return n;
    buf[n] = 0;
    const char *d = strstr(buf, "Duration: ");
    return d && sscanf(d + 10, "%d:%d:%d", &h, &m, &s) == 3 ? h * 3600 + m * 60 + s : 0;
}

/* A song's profile into p: 30 s from its middle (len seconds long; 0 asks ffmpeg), decoded to
 * 22.05 kHz mono into pcm (MX_CLIP_S * MX_RATE samples). -1 when killed, -2 when there is no
 * ffmpeg; otherwise p->status says whether it could be heard. The same as
 * desktop/mixtape_index.py's analyse(). */
int analyse(const char *path, int len, prof_t *p, short *pcm) {
    if (len <= 0 && (len = probe(path)) < 0) return len;
    int at = len > MX_CLIP_S ? (len - MX_CLIP_S) / 2 : 0;
    char ss[16], dur[16];
    snprintf(ss, sizeof ss, "%d", at);
    snprintf(dur, sizeof dur, "%d", len > 0 && len < MX_CLIP_S ? len : MX_CLIP_S);
    char *args[] = { "ffmpeg", "-nostdin", "-v",         "quiet", "-ss",   ss,    "-t",
                     dur,      "-i",       (char *)path, "-map",  "0:a:0", "-ac", "1",
                     "-ar",    "22050",    "-f",         "s16le", "-",     0 };
    int n = run(args, 1, (char *)pcm, MX_CLIP_S * MX_RATE * 2);
    if (n < 0) return n;
    n /= 2;
    p->sec = (unsigned short)(len > 0 && len < 65536 ? len : 0);
    p->status = n >= MX_RATE && mx_features(pcm, n, p->f) ? MX_OK : MX_FAILED;
    return 0;
}

static void say(const char *phase, int done, int total, long left) {
    char line[96];
    int n = snprintf(line, sizeof line, "%s %d %d %ld\n", phase, done, total, left);
    put_file(R(SCAN_FILE), line, (size_t)n);
}

/* The songs to hear. The database is read a page into memory before the card is looked at, so
 * stock's writes (a heart, a play) never wait on the card, as they give up after 500 ms. */
typedef struct {
    unsigned *todo, id[DB_PAGE];
    char *path[DB_PAGE];
    int n, cap, got, done, total;
} check_t;
static int check_row(void *ctx, void *st) {
    check_t *c = ctx;
    const char *path = col_s(st, 1);
    if (quit) return 1;
    if (!music(path)) return 0;
    if (!(c->path[c->got] = strdup(path))) return 1;
    c->id[c->got++] = (unsigned)col_i(st, 0);
    return c->got == DB_PAGE;
}
static void check_page(check_t *c, const profs_t *profs) {
    for (int i = 0; i < c->got; ++i) {
        struct stat s;
        const prof_t *p = 0;
        if (!(++c->done % 200)) say("check", c->done, c->total, -1);
        if (!quit && !stat(R(c->path[i]), &s) && /* gone: nothing to hear */
            !((p = profs_find(profs, prof_key(c->path[i]))) && p->size == (unsigned)s.st_size)) {
            if (c->n == c->cap) {
                int cap = c->cap ? 2 * c->cap : 1024;
                unsigned *v = realloc(c->todo, (size_t)cap * sizeof *v);
                if (v) c->todo = v, c->cap = cap;
            }
            if (c->n < c->cap) c->todo[c->n++] = c->id[i];
        }
        free(c->path[i]);
    }
}
static int song_row(void *ctx, void *st) {
    snprintf(ctx, 1024, "%s", col_s(st, 1));
    return 1;
}

/* The profiles file opened to append: a missing or foreign one (an older version) started anew,
 * a record a cut write left dropped. -1 when the card refuses, -2 when a newer Mixtape (or its
 * indexer) wrote it: that one is kept. */
static int append_open(void) {
    int fd = open(R(PROF_FILE), O_RDWR | O_CREAT, 0644);
    prof_head h = { "", PROF_VERSION, sizeof(prof_t), 0 }, got;
    struct stat s;
    memcpy(h.magic, PROF_MAGIC, 4);
    if (fd < 0 || fstat(fd, &s)) return -1;
    long long n = s.st_size < (long long)sizeof h
                      ? 0
                      : (s.st_size - (long long)sizeof h) / (long long)sizeof(prof_t);
    if (!n || pread(fd, &got, sizeof got, 0) != sizeof got || memcmp(&got, &h, sizeof h)) {
        if (n && !memcmp(got.magic, PROF_MAGIC, 4) && got.version > PROF_VERSION) {
            close(fd);
            return -2;
        }
        n = 0;
    }
    if (ftruncate(fd, n ? (off_t)(sizeof h + n * sizeof(prof_t)) : 0) ||
        (!n && pwrite(fd, &h, sizeof h, 0) != sizeof h) || lseek(fd, 0, SEEK_END) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

/* The file written afresh, each song's last record alone, once it holds COMPACT_SLACK more. */
static void compact(void) {
    profs_t p;
    int n = profs_open(&p);
    long long all = p.map ? (long long)(p.len - sizeof(prof_head)) / (long long)sizeof(prof_t) : 0;
    char tmp[640];
    snprintf(tmp, sizeof tmp, "%s.tmp", R(PROF_FILE));
    FILE *f = all - n > COMPACT_SLACK ? fopen(tmp, "wb") : 0;
    int ok = f && fwrite(p.map, sizeof(prof_head), 1, f) == 1;
    for (int i = 0; ok && i < n; ++i) ok = fwrite(&p.tab[p.idx[i]], sizeof(prof_t), 1, f) == 1;
    if (f && (fflush(f) || fsync(fileno(f)))) ok = 0;
    if (f && fclose(f)) ok = 0;
    profs_close(&p);
    if (f && !(ok && !rename(tmp, R(PROF_FILE)))) unlink(tmp);
}

/* songs: the Library's, for the progress. */
int scan_main(int songs) {
    struct sigaction sa = { 0 };
    sa.sa_handler = on_term;
    sigaction(SIGTERM, &sa, 0);
    sigaction(SIGINT, &sa, 0);
    prctl(PR_SET_PDEATHSIG, SIGTERM); /* the app gone, so is this */
    setpriority(PRIO_PROCESS, 0, 19); /* and its ffmpegs: the player and the UI come first */
    say("check", 0, 0, -1);
    void *db = db_open();
    if (!db) {
        say("end", 0, 0, END_LIBRARY);
        return 0;
    }
    if (mkdir(R(APP_DIR), 0755) && errno != EEXIST) {
        db_close(db);
        say("end", 0, 0, END_CARD);
        return 0;
    }
    scope_load(); /* the folder markers, as the Library has them */
    profs_t profs;
    profs_open(&profs);
    static check_t c;
    c.total = songs;
    unsigned last = 0;
    int got;
    do {
        c.got = 0;
        got = db_page(db,
                      "select id,fileurl from songtable where id>? and " MUSIC_SQL
                      " order by id limit 500",
                      &last, check_row, &c);
        check_page(&c, &profs);
    } while (got >= 0 && c.got == DB_PAGE && !quit);
    profs_close(&profs);
    int fd = got >= 0 && !quit ? append_open() : -1, heard = 0;
    int code = quit       ? END_TERM
               : got < 0  ? END_LIBRARY
               : fd == -2 ? END_NEWER
               : fd < 0   ? END_CARD
                          : END_OK;
    short *pcm = fd >= 0 ? malloc(MX_CLIP_S * MX_RATE * sizeof *pcm) : 0;
    long long began = now_ms();
    say("listen", 0, c.n, -1);
    if (pcm) dur_open(); /* Q2 Pod's lengths: ffmpeg is asked only for the rest */
    for (int i = 0; pcm && i < c.n && !quit; ++i) {
        char path[1024];
        prof_t p = { 0 };
        unsigned at = c.todo[i] - 1;
        struct stat st;
        if (db_page(db, "select id,fileurl from songtable where id>? order by id limit 1", &at,
                    song_row, path) < 0) {
            code = END_LIBRARY; /* an Update Library under it */
            break;
        }
        if (at != c.todo[i] || stat(R(path), &st)) continue; /* gone since */
        int r = analyse(R(path), dur_sec(path), &p, pcm);
        if (r == -2) {
            code = END_FFMPEG;
            break;
        }
        if (r < 0) continue; /* killed (a TERM ends the loop): heard next time */
        if (p.status == MX_FAILED && access(R(APP_DIR), 0)) { /* the card went: heard next time */
            code = END_CARD;
            break;
        }
        p.key = prof_key(path), p.size = (unsigned)st.st_size;
        if (write(fd, &p, sizeof p) != sizeof p) {
            code = END_CARD;
            break;
        }
        ++heard;
        long ms = (long)(now_ms() - began);
        say("listen", heard, c.n, heard >= 3 ? ms / heard * (c.n - heard) / 1000 : -1);
    }
    if (quit && code == END_OK) code = END_TERM;
    free(pcm);
    free(c.todo);
    db_close(db);
    dur_close();
    if (fd >= 0) close(fd);
    if (code == END_OK) compact(); /* not on a TERM: USB storage gives the app 3 s */
    say("end", heard, c.n, code);
    return 0;
}
