/* The app: Mixtape's pages over Q2SPOT/1 (Q2 Pod's docs/librespot.md) on its socket, one request a
 * connection. A mix is crafted when its page opens and kept until another is, so the page's
 * 2 s refresh, a strike or a re-roll see the same one; Play hands it to Q2 Pod's Stream mode as
 * an .m3u, Save as Playlist to its Library update. Last.fm and ListenBrainz are asked on a
 * worker thread while the page says so; listening is a child process (scan.c). */
#include "mixtape.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define SOCK "/tmp/q2-mixtape.browse.sock"
#define CAPS "/tmp/q2-addon.caps" /* the firmware's card-app words */
#define STATE "/tmp/q2-mixtape.state"
#define PLAY "/tmp/q2-mixtape.play"
#define PLAYING "/tmp/q2-mixtape.playing" /* the queue's song playing, from Q2 Pod */
#define LIBRARY "/tmp/q2-mixtape.library"
#define SCAN_FILE "/tmp/q2-mixtape.scan"
#define QUEUE APP_DIR "/play.m3u"
#define ACCOUNTS APP_DIR "/accounts"
#define PLAYLISTS CARD "/_explaylist_data"
#define REPLY_MAX (64 << 10) /* Q2SPOT/1's limits */
#define REQ_MAX (8 << 10)
#define ROWS 50
#define FIND_ROWS 24 /* recommendations not on the card, listed after the mix */
#define IDLE_S 600   /* the library let go after this long without a request */

static volatile sig_atomic_t quit;
static void on_term(int sig) {
    (void)sig;
    quit = 1;
}

/* ---- Replies ------------------------------------------------------------------------------ */

typedef struct {
    char rows[REPLY_MAX];
    int n, count;
    char status[16], msg[128], cursor[24];
} rep_t;

/* s escaped into o (room bytes), cut at max bytes on a character's boundary; its length, -1 when
 * it doesn't fit. */
static int esc(char *o, int room, const char *s, int max) {
    int k = 0, n = (int)strlen(s);
    if (n > max) {
        n = max;
        while (n && ((unsigned char)s[n] & 0xc0) == 0x80) --n;
    }
    for (int i = 0; i < n; ++i) {
        unsigned char c = (unsigned char)s[i];
        int e = c == '%' || c == '\t' || c == '\n' || c == '\r';
        if (k + 3 >= room) return -1;
        if (e)
            k += sprintf(o + k, "%%%02X", c);
        else
            o[k++] = (char)c;
    }
    return k;
}
static void row6(rep_t *r, const char *kind, const char *uri, const char *title, const char *sub,
                 const char *ctx, const char *position) {
    const char *f[] = { kind, uri, title, sub, ctx, position };
    char line[4096];
    int k = 0;
    for (int i = 0; i < 6; ++i) {
        int n = esc(line + k, (int)sizeof line - k - 2, f[i], i == 2 || i == 3 ? 255 : 1024);
        if (n < 0) return;
        k += n;
        line[k++] = i < 5 ? '\t' : '\n';
    }
    if (r->count == ROWS || r->n + k > REPLY_MAX - 512) return; /* the header's room */
    memcpy(r->rows + r->n, line, (size_t)k);
    r->n += k, r->count++;
}
static void row(rep_t *r, const char *kind, const char *uri, const char *title, const char *sub,
                const char *ctx) {
    row6(r, kind, uri, title, sub, ctx, "");
}
static void info(rep_t *r, const char *title, const char *sub) {
    row(r, "info", "", title, sub, "");
}

static int has_cap(const char *word) {
    char caps[1024] = " ";
    FILE *f = fopen(R(CAPS), "rb");
    size_t n = f ? fread(caps + 1, 1, sizeof caps - 3, f) : 0;
    if (f) fclose(f);
    caps[n + 1] = ' ', caps[n + 2] = 0;
    for (char *c = caps; *c; ++c)
        if (*c == '\n' || *c == '\r' || *c == '\t') *c = ' ';
    char want[40];
    snprintf(want, sizeof want, " %s ", word);
    return !!strstr(caps, want);
}

/* path written whole: a temp beside it, then renamed over it. 1 when it was. */
static int put_file(const char *path, const char *data, size_t n) {
    char tmp[700];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    int ok = f && fwrite(data, 1, n, f) == n;
    if (f && (fflush(f) || fsync(fileno(f)))) ok = 0;
    if (f && fclose(f)) ok = 0;
    if (ok && !rename(tmp, path)) return 1;
    unlink(tmp);
    return 0;
}

static const char *commas(int n, char *buf) {
    if (n >= 1000000)
        sprintf(buf, "%d,%03d,%03d", n / 1000000, n / 1000 % 1000, n % 1000);
    else if (n >= 1000)
        sprintf(buf, "%d,%03d", n / 1000, n % 1000);
    else
        sprintf(buf, "%d", n);
    return buf;
}

/* ---- Accounts ----------------------------------------------------------------------------- */

static void account(int lb, char *out, unsigned size) {
    char line[160];
    const char *key = lb ? "listenbrainz=" : "lastfm=";
    FILE *f = fopen(R(ACCOUNTS), "rb");
    out[0] = 0;
    while (f && fgets(line, sizeof line, f))
        if (!strncmp(line, key, strlen(key))) {
            line[strcspn(line, "\r\n")] = 0;
            snprintf(out, size, "%s", line + strlen(key));
        }
    if (f) fclose(f);
}
static int account_set(int lb, const char *name) {
    char other[80], text[200], clean[80];
    unsigned k = 0;
    for (const char *s = name; *s && k + 1 < sizeof clean; ++s)
        if ((unsigned char)*s >= ' ' && (k || *s != ' ')) clean[k++] = *s;
    while (k && clean[k - 1] == ' ') --k;
    clean[k] = 0;
    account(!lb, other, sizeof other);
    int n = snprintf(text, sizeof text, "lastfm=%s\nlistenbrainz=%s\n", lb ? other : clean,
                     lb ? clean : other);
    mkdir(R(APP_DIR), 0755);
    return put_file(R(ACCOUNTS), text, (size_t)n);
}

/* ---- Listening ---------------------------------------------------------------------------- */

static pid_t scanner;
static void hold(int on) { put_file(R(STATE), on ? "hold=1\n" : "hold=0\n", 7); }
static void reap(void) {
    if (scanner > 0 && waitpid(scanner, 0, WNOHANG) == scanner) scanner = 0, hold(0);
}
static void scan_stop(void) {
    if (scanner <= 0) return;
    kill(scanner, SIGTERM);
    for (int i = 0; i < 40 && waitpid(scanner, 0, WNOHANG) != scanner; ++i) usleep(50000);
    if (waitpid(scanner, 0, WNOHANG) == 0) kill(scanner, SIGKILL), waitpid(scanner, 0, 0);
    scanner = 0;
    hold(0);
}
static void scan_start(void) {
    if (scanner > 0) return;
    unlink(R(SCAN_FILE));
    pid_t pid = fork();
    if (!pid) {
        execl("/proc/self/exe", "q2-mixtape", "scan", (char *)0);
        _exit(1);
    }
    if (pid > 0) scanner = pid, hold(1); /* standby would take the card from under it */
}
typedef struct {
    char phase[8];
    int done, total;
    long left;
} scan_t;
static int scan_read(scan_t *s) {
    FILE *f = fopen(R(SCAN_FILE), "rb");
    memset(s, 0, sizeof *s);
    int ok = f && fscanf(f, "%7s %d %d %ld", s->phase, &s->done, &s->total, &s->left) == 4;
    if (f) fclose(f);
    return ok;
}

/* ---- Recommendations, on the worker ------------------------------------------------------- */

static struct {
    pthread_t t;
    volatile int running, done;
    int lb;
    char key[160], user[80], api[64]; /* key: the mix's and the username, what was asked */
    net_t res;
} nw;
static void *net_worker(void *unused) {
    (void)unused;
    if (nw.lb)
        net_listenbrainz(nw.user, &nw.res);
    else
        net_lastfm(nw.user, nw.api, &nw.res);
    __sync_synchronize(); /* the result written before done says so */
    nw.done = 1;
    return 0;
}
static void net_take(net_t *out) {
    if (nw.running) pthread_join(nw.t, 0);
    free(out->v);
    *out = nw.res;
    memset(&nw.res, 0, sizeof nw.res);
    nw.running = nw.done = 0, nw.key[0] = 0;
}

/* ---- Mixes -------------------------------------------------------------------------------- */

static struct {
    char key[48];   /* the mix op's argument: "most", "album 1a2b3c4d", "song 812", "lastfm" */
    char name[128]; /* what it is made from: "Most Played", an album's name */
    int kind, gen, libgen, len, seeded, pending;
    int list[MX_MAX];
    unsigned char struck[MX_MAX];
    struct {
        char path[1024], title[128], artist[128];
        int sec;
    } row[MX_MAX];
    char said[120]; /* why there is no mix */
    net_t net;      /* the recommendations it is made from */
} mx;

static const char *const COUNTS[] = { "most", "forgotten", "never", "deep" };
static const char *const COUNT_NAMES[] = { "Most Played", "Forgotten Favourites", "Never Played",
                                           "Deep Cuts" };
static const char *const COUNT_EMPTY[] = {
    "Play some music first: this mix is made from play counts",
    "Nothing forgotten yet: songs played or loved a while ago",
    "Every song has a play counted",
    "Play some music first: this mix is made from play counts",
};

static void rows_fetch(void) {
    void *db = db_open();
    for (int i = 0; i < mx.len; ++i) {
        const song_t *s = &lib.v[mx.list[i]];
        if (!song_text(db, s->id, mx.row[i].path, sizeof mx.row[i].path, mx.row[i].title,
                       sizeof mx.row[i].title, mx.row[i].artist, sizeof mx.row[i].artist))
            mx.row[i].path[0] = 0, snprintf(mx.row[i].title, sizeof mx.row[i].title, "Song gone");
        mx.row[i].sec = s->sec;
    }
    if (db) db_close(db);
}

static const group_t *group_of(const group_t *g, int n, unsigned hash) {
    for (int i = 0; i < n; ++i)
        if (g[i].hash == hash) return &g[i];
    return 0;
}

/* The mix for arg made anew into mx: Last.fm's and ListenBrainz's asked for first, unless again
 * (a re-roll's), which makes it from what they said last time. */
static void craft(const char *arg, int again) {
    unsigned h = 0;
    int kind = -1, lb = !strcmp(arg, "listenbrainz");
    for (int i = 0; i < 4; ++i)
        if (!strcmp(arg, COUNTS[i])) kind = i;
    if (sscanf(arg, "album %x", &h) == 1 || sscanf(arg, "artist %x", &h) == 1 ||
        sscanf(arg, "song %u", &h) == 1)
        kind = MIX_SOUND;
    if (lb || !strcmp(arg, "lastfm")) kind = lb ? MIX_LB : MIX_LASTFM;
    snprintf(mx.key, sizeof mx.key, "%s", arg);
    mx.kind = kind, mx.len = mx.seeded = mx.pending = 0, mx.said[0] = mx.name[0] = 0, ++mx.gen;
    memset(mx.struck, 0, sizeof mx.struck);
    if (kind < 0) {
        snprintf(mx.said, sizeof mx.said, "Mixtape doesn't know this mix");
        return;
    }
    if (kind >= MIX_LASTFM && !again) {
        char user[80], want[160];
        account(lb, user, sizeof user);
        snprintf(want, sizeof want, "%s %s", arg, user);
        if (nw.done && strcmp(nw.key, want)) net_take(&mx.net); /* another question's answer */
        if (!nw.done) {
            mx.pending = 1;
            if (nw.running) return; /* another's question, then this one's */
            nw.lb = lb;
            snprintf(nw.user, sizeof nw.user, "%s", user);
            snprintf(nw.api, sizeof nw.api, "%s", lb ? "" : lastfm_key());
            snprintf(nw.key, sizeof nw.key, "%s", want);
            nw.running = !pthread_create(&nw.t, 0, net_worker, 0);
            if (!nw.running) mx.pending = 0, snprintf(mx.said, sizeof mx.said, "Not enough memory");
            return;
        }
        net_take(&mx.net);
    }
    if (!lib_load(1)) {
        snprintf(mx.said, sizeof mx.said, "No Library yet: run Update Library");
        return;
    }
    mx.libgen = lib.gen;
    song_t *v = lib.v;
    int n = lib.n, seeds = 0;
    if (kind < MIX_SOUND) {
        mx_counts(v, n, kind);
        snprintf(mx.name, sizeof mx.name, "%s", COUNT_NAMES[kind]);
    } else if (kind == MIX_SOUND) {
        int album = arg[1] == 'l', song = arg[0] == 's', at = song ? lib_find(h) : -1;
        for (int i = 0; i < n; ++i)
            seeds += v[i].seed =
                (unsigned char)(song    ? i == at
                                : album ? v[i].album == h
                                        : v[i].artist == h || v[i].album_artist == h);
        const group_t *g = album  ? group_of(lib.albums, lib.nalbums, h)
                           : song ? 0
                                  : group_of(lib.artists, lib.nartists, h);
        if (song && at >= 0) {
            char path[1024], artist[128];
            void *db = db_open();
            if (!song_text(db, h, path, sizeof path, mx.name, sizeof mx.name, artist,
                           sizeof artist))
                mx.name[0] = 0;
            if (db) db_close(db);
        } else if (g)
            snprintf(mx.name, sizeof mx.name, "%s", lib_str(g->name));
        if (!seeds) {
            snprintf(mx.said, sizeof mx.said,
                     song    ? "That song isn't in the Library now"
                     : album ? "That album isn't in the Library now"
                             : "That artist isn't in the Library now");
            return;
        }
        mx_rank(v, n);
        mx.seeded = !song;
    } else {
        snprintf(mx.name, sizeof mx.name, "%s",
                 lb ? "ListenBrainz Picks" : "Last.fm Similar Artists");
        mx_items(v, n, mx.net.v, mx.net.n, kind);
    }
    mx.len = mx_craft(v, n, mx.list, mx.seeded);
    if (mx.len < (kind >= MIX_LASTFM ? 1 : MX_MIN)) {
        mx.len = 0;
        if (kind < MIX_SOUND)
            snprintf(mx.said, sizeof mx.said, "%s", COUNT_EMPTY[kind]);
        else if (kind == MIX_SOUND)
            snprintf(mx.said, sizeof mx.said, "Not enough related music");
        else
            snprintf(mx.said, sizeof mx.said, "%s",
                     mx.net.said[0] ? mx.net.said
                     : lb           ? "None of its picks are on your card"
                                    : "None of those artists are on your card");
    }
    rows_fetch();
}

static void time_text(char *o, int sec) {
    if (sec > 0)
        sprintf(o, "%d:%02d", sec / 60, sec % 60);
    else
        o[0] = 0;
}

/* The mix's page: Play, its songs (Centre strikes one), Re-roll and Save, then what Last.fm or
 * ListenBrainz recommends that isn't on the card. */
static void mix_rows(rep_t *r) {
    char ctx[32], sub[300], t[16];
    int kept = 0, struck = 0, total = 0;
    snprintf(ctx, sizeof ctx, "%d", mx.gen);
    for (int i = 0; i < mx.len; ++i)
        if (mx.struck[i])
            ++struck;
        else
            ++kept, total += mx.row[i].sec > 0 ? mx.row[i].sec : MX_GUESS_S;
    if (mx.len) {
        int k = mx.kind == MIX_SOUND ? snprintf(sub, sizeof sub, "%s%s \xc2\xb7 ",
                                                mx.seeded ? "From " : "Like ", mx.name)
                                     : 0;
        snprintf(sub + k, sizeof sub - (size_t)k, "%d song%s \xc2\xb7 %d min", kept,
                 kept == 1 ? "" : "s", (total + 30) / 60);
        row(r, "action", "play", "Play Mixtape", sub, ctx);
    } else
        info(r, mx.said, "");
    for (int i = 0; i < mx.len; ++i) {
        char c[32];
        snprintf(c, sizeof c, "%d %d", mx.gen, i);
        time_text(t, mx.row[i].sec);
        if (mx.struck[i])
            snprintf(sub, sizeof sub, "Struck \xc2\xb7 Re-roll replaces it");
        else
            snprintf(sub, sizeof sub, "%s%s%s", mx.row[i].artist,
                     *mx.row[i].artist && *t ? " \xc2\xb7 " : "", t);
        row(r, "action", "strike", mx.row[i].title, sub, c);
    }
    if (mx.len) {
        row(r, "action", "reroll", "Re-roll",
            struck                          ? "New songs for the struck ones"
            : mx.seeded && mx.key[1] == 'l' ? "New songs around the album's own"
            : mx.seeded                     ? "New songs around the artist's own"
                                            : "New songs",
            ctx);
        row(r, "action", "save", "Save as Playlist", "Adds it to Library \xe2\x86\x92 Playlists",
            ctx);
    }
    /* the best not found, in order */
    for (int k = 0; k < FIND_ROWS && mx.kind >= MIX_LASTFM; ++k) {
        int best = -1;
        for (int i = 0; i < mx.net.n; ++i) {
            const item_t *it = &mx.net.v[i];
            if (!it->matched && !it->top && it->name[0] &&
                (best < 0 || it->score > mx.net.v[best].score))
                best = i;
        }
        if (best < 0) break;
        item_t *it = &mx.net.v[best];
        snprintf(sub, sizeof sub, "%s%sTo find", it->sub, it->sub[0] ? " \xc2\xb7 " : "");
        info(r, it->name, sub);
        it->matched = 2; /* listed: put back below */
    }
    for (int i = 0; i < mx.net.n; ++i)
        if (mx.net.v[i].matched == 2) mx.net.v[i].matched = 0;
    snprintf(r->msg, sizeof r->msg, "refresh"); /* a strike's mark moves at once */
}

static void page_mix(rep_t *r, const char *arg) {
    if (strcmp(mx.key, arg) || (mx.pending && nw.done)) craft(arg, 0);
    if (mx.pending) {
        info(r,
             mx.kind == MIX_LB ? "Asking ListenBrainz\xe2\x80\xa6" : "Asking Last.fm\xe2\x80\xa6",
             "");
        snprintf(r->msg, sizeof r->msg, "refresh");
        return;
    }
    mix_rows(r);
}

/* An action's mix: ctx's gen is the mix's own. */
static int ours(const char *ctx) { return atoi(ctx) == mx.gen && mx.len && !mx.pending; }

static int queue_text(char **out) {
    size_t cap = 4 + (size_t)mx.len * (sizeof mx.row[0].path + 1), n = 3;
    char *t = malloc(cap);
    if (!t) return -1;
    memcpy(t, "\xef\xbb\xbf", 3); /* UTF-8, as stock's playlist import reads it (no sniffing) */
    int songs = 0;
    for (int i = 0; i < mx.len; ++i)
        if (!mx.struck[i] && mx.row[i].path[0])
            n += (size_t)sprintf(t + n, "%s\n", mx.row[i].path), ++songs;
    *out = t;
    return (int)n * !!songs;
}

static void act_play(rep_t *r, const char *ctx) {
    char *t = 0, line[256];
    int n = ours(ctx) ? queue_text(&t) : -2;
    if (n < 0)
        snprintf(r->msg, sizeof r->msg, n == -2 ? "This Mixtape has changed" : "Not enough memory");
    else if (!n)
        snprintf(r->msg, sizeof r->msg, "Every song is struck");
    else if (!has_cap("playlist"))
        snprintf(r->msg, sizeof r->msg, "Update Q2 Pod to play a Mixtape");
    else if (mkdir(R(APP_DIR), 0755), !put_file(R(QUEUE), t, (size_t)n))
        snprintf(r->msg, sizeof r->msg, "Can't write to the card");
    else {
        static unsigned seq;
        int k = snprintf(line, sizeof line, "%lx%u %s\n", (unsigned long)time(0), ++seq, QUEUE);
        if (!put_file(R(PLAY), line, (size_t)k))
            snprintf(r->msg, sizeof r->msg, "Can't start the Mixtape");
    }
    free(t);
}

static void act_strike(rep_t *r, const char *ctx) {
    int gen, i;
    if (sscanf(ctx, "%d %d", &gen, &i) == 2 && gen == mx.gen && i >= 0 && i < mx.len)
        mx.struck[i] ^= 1;
    else
        snprintf(r->msg, sizeof r->msg, "This Mixtape has changed");
}

static void act_reroll(rep_t *r, const char *ctx) {
    if (!ours(ctx)) {
        snprintf(r->msg, sizeof r->msg, "This Mixtape has changed");
        return;
    }
    char key[sizeof mx.key];
    snprintf(key, sizeof key, "%s", mx.key);
    if (lib_load(0) && lib.gen == mx.libgen && !mx.pending) {
        int len = mx_reroll(lib.v, lib.n, mx.list, mx.len, mx.struck);
        if (len) {
            mx.len = len, ++mx.gen;
            rows_fetch();
            return;
        }
    }
    craft(key, 1); /* the library changed under it, or nothing else fit: a new one */
}

/* A playlist's name from the mix's: "<name> Mixtape <date>", at most 63 bytes (stock's limit),
 * none of FAT's forbidden characters, and a number after it when the file is there already. */
static void playlist_name(char *out, unsigned size, char *file, unsigned fsize) {
    char base[64], date[16];
    time_t now = time(0);
    strftime(date, sizeof date, "%Y-%m-%d", localtime(&now));
    const char *s = mx.name + strspn(mx.name, ". "); /* a leading dot hides a file */
    unsigned len = (unsigned)strlen(s), room = 63 - 9 - (unsigned)strlen(date) - 3, k;
    for (k = len < room ? len : room; k && k < len && ((unsigned char)s[k] & 0xc0) == 0x80;) --k;
    for (unsigned i = 0; i < k; ++i) /* " Mixtape ", the date and " 99" fit after it */
        base[i] = strchr("/\\:*?\"<>|", s[i]) || (unsigned char)s[i] < ' ' ? '-' : s[i];
    while (k && (base[k - 1] == ' ' || base[k - 1] == '.')) --k;
    base[k] = 0;
    for (int i = 1; i < 100; ++i) {
        if (i == 1)
            snprintf(out, size, "%s Mixtape %.10s", base, date);
        else
            snprintf(out, size, "%s Mixtape %.10s %d", base, date, i);
        snprintf(file, fsize, PLAYLISTS "/%s.m3u", out);
        if (access(R(file), 0)) return;
    }
}

static void act_save(rep_t *r, const char *ctx) {
    char *t = 0, name[100], file[200], line[256];
    int n = ours(ctx) ? queue_text(&t) : -2;
    if (n < 0) {
        snprintf(r->msg, sizeof r->msg, n == -2 ? "This Mixtape has changed" : "Not enough memory");
        return;
    }
    playlist_name(name, sizeof name, file, sizeof file);
    mkdir(R(PLAYLISTS), 0755);
    if (!n)
        snprintf(r->msg, sizeof r->msg, "Every song is struck");
    else if (!put_file(R(file), t, (size_t)n))
        snprintf(r->msg, sizeof r->msg, "Can't write to the card");
    else if (!has_cap("library"))
        snprintf(r->msg, sizeof r->msg,
                 "Saved: Import Playlists adds \xe2\x80\x9c%.60s\xe2\x80\x9d", name);
    else {
        static unsigned seq;
        int k = snprintf(line, sizeof line, "%lx%u\nadd %s\n", (unsigned long)time(0), ++seq, file);
        if (put_file(R(LIBRARY), line, (size_t)k))
            snprintf(r->msg, sizeof r->msg,
                     "\xe2\x80\x9c%.60s\xe2\x80\x9d joins Playlists once nothing plays", name);
        else
            snprintf(r->msg, sizeof r->msg, "Can't ask Q2 Pod to add it");
    }
    free(t);
}

/* ---- Pages -------------------------------------------------------------------------------- */

static void page_menu(rep_t *r) {
    scan_t s;
    char a[16], b[16], sub[96] = "How each song sounds, for the sound mixes";
    int scanning = scanner > 0 && scan_read(&s);
    if (scanning && !strcmp(s.phase, "listen"))
        snprintf(sub, sizeof sub, "Listening: %s of %s", commas(s.done, a), commas(s.total, b));
    else if (scanning)
        snprintf(sub, sizeof sub, "Checking your songs\xe2\x80\xa6");
    row(r, "list", "mix", "Most Played", "Your most played songs", "most");
    row(r, "list", "mix", "Forgotten Favourites", "Loved or played a lot, but not lately",
        "forgotten");
    row(r, "list", "mix", "Never Played", "Songs with no play counted", "never");
    row(r, "list", "mix", "Deep Cuts", "Unplayed songs by the artists you play most", "deep");
    row(r, "list", "albums", "From an Album", "Songs that sound like it", "");
    row(r, "list", "artists", "From an Artist", "Songs that sound like theirs", "");
    row(r, "list", "lastfm", "Last.fm", "Its similar artists, from your card", "");
    row(r, "list", "listenbrainz", "ListenBrainz", "Its picks for you, from your card", "");
    row(r, "list", "scan", "Listen to Your Music", sub, "");
}

static void page_groups(rep_t *r, int artists, const char *cursor) {
    if (!lib_load(0)) {
        info(r, "No Library yet: run Update Library", "");
        return;
    }
    int off = atoi(cursor), n = artists ? lib.nartists : lib.nalbums;
    const group_t *g = artists ? lib.artists : lib.albums;
    if (!off) row(r, "search", "", "Search", "", "");
    if (!n) info(r, artists ? "No artists are tagged" : "No albums are tagged", "");
    int i = off;
    for (char c[24]; i < n && r->count < ROWS; ++i) {
        snprintf(c, sizeof c, "%s %08x", artists ? "artist" : "album", g[i].hash);
        row(r, "list", "mix", lib_str(g[i].name), lib_str(g[i].artist), c);
    }
    if (i < n)
        snprintf(r->cursor, sizeof r->cursor, "%d", i), snprintf(r->msg, sizeof r->msg, "pages");
}

static void page_search(rep_t *r, const char *text) {
    if (!lib_load(0)) {
        info(r, "No Library yet: run Update Library", "");
        return;
    }
    char c[24];
    for (int i = 0; i < lib.nartists && r->count < ROWS; ++i)
        if (strcasestr(lib_str(lib.artists[i].name), text))
            snprintf(c, sizeof c, "artist %08x", lib.artists[i].hash),
                row(r, "list", "mix", lib_str(lib.artists[i].name), "Artist", c);
    for (int i = 0; i < lib.nalbums && r->count < ROWS; ++i)
        if (strcasestr(lib_str(lib.albums[i].name), text) ||
            strcasestr(lib_str(lib.albums[i].artist), text))
            snprintf(c, sizeof c, "album %08x", lib.albums[i].hash),
                row(r, "list", "mix", lib_str(lib.albums[i].name), lib_str(lib.albums[i].artist),
                    c);
    if (!r->count) info(r, "No album or artist has that name", "");
}

static void page_scan(rep_t *r) {
    scan_t s;
    char a[16], b[16], sub[96];
    int ok = scan_read(&s);
    reap();
    if (scanner > 0) {
        int listen = ok && !strcmp(s.phase, "listen");
        snprintf(sub, sizeof sub, "%s of %s", commas(s.done, a), commas(s.total, b));
        const char *title = listen ? "Listening" : "Checking your songs";
        if (has_cap("progress")) {
            char pml[16];
            snprintf(pml, sizeof pml, "%d", s.total ? (int)(1000LL * s.done / s.total) : 0);
            row6(r, "progress", "", title, s.total ? sub : "", "", pml);
        } else
            info(r, title, s.total ? sub : "");
        if (listen && s.left >= 0) {
            long m = (s.left + 59) / 60;
            if (m >= 60)
                snprintf(sub, sizeof sub, "About %ld h %ld min left", m / 60, m % 60);
            else
                snprintf(sub, sizeof sub, "About %ld min left", m);
            info(r, sub, "Keep the Q2 charging for a big library");
        }
        row(r, "action", "scanstop", "Stop Listening", "What's been heard is kept", "");
        snprintf(r->msg, sizeof r->msg, "refresh");
        return;
    }
    int heard = 0, n = lib_load(1);
    for (int i = 0; i < n; ++i) heard += lib.v[i].prof != 0;
    snprintf(sub, sizeof sub, "%s of %s", commas(heard, a), commas(n, b));
    info(r, "Songs heard", sub);
    static const char *const ENDS[] = { 0, "Stopped: the card went away",
                                        "Stopped: this Q2 has no ffmpeg",
                                        "Stopped: the Library was updating", 0 };
    if (ok && !strcmp(s.phase, "end") && s.left > 0 && s.left < 4 && ENDS[s.left])
        info(r, ENDS[s.left], "");
    if (n)
        row(r, "action", "scanstart", heard < n ? "Listen Now" : "Listen for New Songs",
            "30 s from the middle of each song not heard yet", "");
    else
        info(r, "No Library yet: run Update Library", "");
    info(r, "A computer is quicker", "See github.com/DiamondBond/q2-mixtape");
}

static void page_account(rep_t *r, int lb) {
    char user[80];
    account(lb, user, sizeof user);
    if (has_cap("field"))
        row(r, "field", lb ? "lbuser" : "fmuser", "Username:", user[0] ? user : "Not set", "");
    else
        info(r, "Update Q2 Pod to enter a username", "");
    if (!lb && !*lastfm_key())
        info(r, "This Mixtape has no Last.fm API key",
             "Put yours under [LASTFM] API_KEY in .scrobble.ini");
    else if (user[0])
        row(r, "list", "mix", lb ? "Recommendations" : "Similar Artists",
            lb ? "Its picks for you, from your card" : "Artists like your top ones, from your card",
            lb ? "listenbrainz" : "lastfm");
}

/* Now Playing's ⋯ while a Mixtape plays (q2app's more=): mixes like the song playing, its album
 * and its artist. */
static void page_more(rep_t *r) {
    char path[1024] = "", c[24], t[300];
    FILE *f = fopen(R(PLAYING), "rb");
    if (f && fgets(path, sizeof path, f)) path[strcspn(path, "\r\n")] = 0;
    if (f) fclose(f);
    void *db = path[0] && lib_load(0) ? db_open() : 0;
    unsigned id = song_id(db, path);
    int at = id ? lib_find(id) : -1;
    char title[128] = "", artist[128] = "", p[1024];
    if (at >= 0) song_text(db, id, p, sizeof p, title, sizeof title, artist, sizeof artist);
    if (db) db_close(db);
    if (at < 0) {
        info(r, "Nothing from the Library is playing", "");
        return;
    }
    const song_t *s = &lib.v[at];
    const group_t *al = group_of(lib.albums, lib.nalbums, s->album),
                  *ar = group_of(lib.artists, lib.nartists, s->artist);
    snprintf(c, sizeof c, "song %u", id);
    snprintf(t, sizeof t, "Like \xe2\x80\x9c%s\xe2\x80\x9d", title);
    row(r, "list", "mix", t, "Songs that sound like this one", c);
    if (al) {
        snprintf(c, sizeof c, "album %08x", al->hash),
            snprintf(t, sizeof t, "From %s", lib_str(al->name));
        row(r, "list", "mix", t, "Songs that sound like its album", c);
    }
    if (ar) {
        snprintf(c, sizeof c, "artist %08x", ar->hash),
            snprintf(t, sizeof t, "From %s", lib_str(ar->name));
        row(r, "list", "mix", t, "Songs that sound like the artist's", c);
    }
}

static void handle(char **a, int n, rep_t *r) {
    const char *op = n > 1 ? a[1] : "", *x = n > 2 ? a[2] : "";
    snprintf(r->status, sizeof r->status, "ok");
    if (!strcmp(op, "menu"))
        page_menu(r);
    else if (!strcmp(op, "mix"))
        page_mix(r, x);
    else if (!strcmp(op, "albums") || !strcmp(op, "artists"))
        page_groups(r, op[1] == 'r', x);
    else if (!strcmp(op, "search"))
        page_search(r, x);
    else if (!strcmp(op, "scan"))
        page_scan(r);
    else if (!strcmp(op, "lastfm") || !strcmp(op, "listenbrainz"))
        page_account(r, op[1] == 'i');
    else if (!strcmp(op, "fmuser") || !strcmp(op, "lbuser")) {
        if (!account_set(op[0] == 'l', x)) {
            snprintf(r->status, sizeof r->status, "error");
            snprintf(r->msg, sizeof r->msg, "Can't write to the card");
        } else {
            if (!strcmp(mx.key, op[0] == 'l' ? "listenbrainz" : "lastfm"))
                mx.key[0] = 0; /* the last user's mix: asked again when opened */
            page_account(r, op[0] == 'l');
        }
    } else if (!strcmp(op, "more"))
        page_more(r);
    else if (!strcmp(op, "play"))
        act_play(r, x);
    else if (!strcmp(op, "strike"))
        act_strike(r, x);
    else if (!strcmp(op, "reroll"))
        act_reroll(r, x);
    else if (!strcmp(op, "save"))
        act_save(r, x);
    else if (!strcmp(op, "scanstart")) {
        if (lib_load(0)) scan_start();
    } else if (!strcmp(op, "scanstop"))
        scan_stop();
    else
        snprintf(r->status, sizeof r->status, "unsupported");
}

/* ---- The socket --------------------------------------------------------------------------- */

static int unescape(char *s) {
    char *o = s;
    for (; *s; ++o) {
        if (*s != '%') {
            *o = *s++;
            continue;
        }
        unsigned v;
        if (sscanf(s + 1, "%2x", &v) != 1 || !s[1] || !s[2] || !v) return 0;
        *o = (char)v, s += 3;
    }
    *o = 0;
    return 1;
}

static void serve_one(int fd) {
    static char req[REQ_MAX + 1];
    static rep_t r;
    struct timeval tv = { 5, 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    int n = 0;
    for (int got; n < REQ_MAX && (got = (int)read(fd, req + n, (size_t)(REQ_MAX - n))) > 0;)
        n += got;
    req[n] = 0;
    r.n = r.count = 0, r.status[0] = r.msg[0] = r.cursor[0] = 0;
    char *a[16], *line = !strncmp(req, "Q2SPOT/1\n", 9) ? req + 9 : 0;
    int k = 0;
    if (line) {
        line[strcspn(line, "\n")] = 0;
        for (char *s = line; s && k < 16;) a[k++] = strsep(&s, "\t");
        for (int i = 0; i < k; ++i)
            if (!unescape(a[i])) k = 0;
    }
    if (k >= 2)
        handle(a, k, &r);
    else
        snprintf(r.status, sizeof r.status, "bad");
    char head[512], m[300], cu[64], id[32] = "0";
    if (k) snprintf(id, sizeof id, "%.20s", a[0]);
    int mn = esc(m, sizeof m, r.msg, 120), cn = esc(cu, sizeof cu, r.cursor, 24);
    m[mn < 0 ? 0 : mn] = 0, cu[cn < 0 ? 0 : cn] = 0;
    int h = snprintf(head, sizeof head, "Q2SPOT/1\n%s\t%s\t%s\t%s\n", id, r.status, m, cu);
    if (write(fd, head, (size_t)h) == h && r.n) {
        for (int at = 0, w; at < r.n; at += w)
            if ((w = (int)write(fd, r.rows + at, (size_t)(r.n - at))) <= 0) break;
    }
    close(fd);
}

int serve(void) {
    struct sigaction sa = { 0 };
    sa.sa_handler = on_term;
    sigaction(SIGTERM, &sa, 0);
    sigaction(SIGINT, &sa, 0);
    signal(SIGPIPE, SIG_IGN);
    struct sockaddr_un addr = { AF_UNIX, "" };
    snprintf(addr.sun_path, sizeof addr.sun_path, "%s", R(SOCK));
    unlink(addr.sun_path);
    int s = socket(AF_UNIX, SOCK_STREAM, 0);
    if (s < 0 || bind(s, (struct sockaddr *)&addr, sizeof addr) || listen(s, 4)) return 1;
    hold(0);
    time_t last = time(0);
    while (!quit) {
        struct pollfd p = { s, POLLIN, 0 };
        int k = poll(&p, 1, 1000);
        reap();
        if (k > 0) {
            int fd = accept(s, 0, 0);
            if (fd >= 0) serve_one(fd), last = time(0);
        }
        /* ponytail: the library (about 4 MB at 50,000 songs) is let go when idle, the app kept
         * running for the next page; a re-roll after that crafts a new mix. */
        if (lib.n && time(0) - last > IDLE_S && !mx.pending) lib_free();
    }
    scan_stop();
    close(s);
    unlink(addr.sun_path);
    unlink(R(STATE));
    return 0;
}
