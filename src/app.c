/* The app: Mixtape's pages over Q2SPOT/1 (Q2 Pod's docs/librespot.md) on its socket, one request a
 * connection. A mix is crafted when its page opens and kept until another is, so the page's
 * 2 s refresh, a strike or a re-roll see the same one; Play hands it to Q2 Pod's Stream mode as
 * an .m3u, Save as Playlist to its Library update. Last.fm and ListenBrainz are asked on a
 * worker thread while the page says so; listening is a child process (scan.c). */
#include "mixtape.h"
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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
#define QUEUE APP_DIR "/play.m3u"
#define ACCOUNTS APP_DIR "/accounts"
#define LENGTH APP_DIR "/length" /* the mixes' length in minutes */
#define PLAYLISTS CARD "/_explaylist_data"
#define REPLY_MAX (64 << 10) /* Q2SPOT/1's limits */
#define REQ_MAX (8 << 10)
#define ROWS 50
#define SONG_ROWS 20 /* a search's songs, after its artists and albums */
#define FIND_ROWS 24 /* recommendations not on the card, listed after the mix */
/* No request this long and nothing running: the app ends, so Smart Wi-Fi can let the radio
 * sleep. The next page starts it again. */
#define IDLE_S 600

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
    char songs[16];
    if (scanner > 0) return;
    snprintf(songs, sizeof songs, "%d", lib.n); /* for its count, the database left alone */
    unlink(R(SCAN_FILE));
    pid_t pid = fork();
    if (!pid) {
        /* none of the app's: the page's connection would stay open until the scan ends */
        for (int fd = 3; fd < 1024; ++fd) close(fd);
        execl("/proc/self/exe", "q2-mixtape", "scan", songs, (char *)0);
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
    int list[MX_MAX];    /* into lib.v as read at libgen */
    unsigned id[MX_MAX]; /* the songs' ids, which outlast a read */
    unsigned char struck[MX_MAX];
    struct {
        char path[1024], title[128], artist[128];
        int sec;
    } row[MX_MAX];
    char said[120]; /* why there is no mix */
    net_t net;      /* the recommendations it is made from */
} mx;

static const char *const COUNTS[] = { "most", "forgotten", "never", "deep", "shuffle" };
static const char *const COUNT_NAMES[] = { "Most Played", "Forgotten Favourites", "Never Played",
                                           "Deep Cuts", "Shuffle" };
static const char *const COUNT_EMPTY[] = {
    "Play some music first: this mix is made from play counts",
    "Nothing forgotten yet: songs played or loved a while ago",
    "Every song has a play counted",
    "Play some music first: this mix is made from play counts",
    "Not enough songs in the Library",
};

/* How long a mix is, in minutes: one of LENGTHS, kept in LENGTH. */
static const int LENGTHS[] = { 5, 10, 15, 20, 30, 45, 60 };
static int length_min = 45;
static int length_ok(int m) {
    for (unsigned i = 0; i < sizeof LENGTHS / sizeof *LENGTHS; ++i)
        if (LENGTHS[i] == m) return 1;
    return 0;
}
static void length_read(void) {
    FILE *f = fopen(R(LENGTH), "rb");
    int m;
    if (f && fscanf(f, "%d", &m) == 1 && length_ok(m)) length_min = m;
    if (f) fclose(f);
}

static void rows_fetch(void) {
    void *db = db_open();
    for (int i = 0; i < mx.len; ++i) {
        const song_t *s = &lib.v[mx.list[i]];
        mx.id[i] = s->id;
        if (!song_text(db, s->id, mx.row[i].path, sizeof mx.row[i].path, mx.row[i].title,
                       sizeof mx.row[i].title, mx.row[i].artist, sizeof mx.row[i].artist))
            mx.row[i].path[0] = 0, snprintf(mx.row[i].title, sizeof mx.row[i].title, "Song gone");
        else if (access(R(mx.row[i].path), 0))
            mx.row[i].path[0] = 0; /* not on the card: shown, never queued */
        mx.row[i].sec = s->sec;
    }
    if (db) db_close(db);
}

static const group_t *group_of(const group_t *g, int n, unsigned hash) {
    for (int i = 0; i < n; ++i)
        if (g[i].hash == hash) return &g[i];
    return 0;
}

static int kind_of(const char *arg, unsigned *h) {
    for (int i = 0; i < MIX_SOUND; ++i)
        if (!strcmp(arg, COUNTS[i])) return i;
    if (sscanf(arg, "album %x", h) == 1 || sscanf(arg, "artist %x", h) == 1 ||
        sscanf(arg, "song %u", h) == 1)
        return MIX_SOUND;
    return !strcmp(arg, "listenbrainz") ? MIX_LB : !strcmp(arg, "lastfm") ? MIX_LASTFM : -1;
}

/* The library scored for mx.key's mix, and mx.name and mx.seeded set: 0, with mx.said, when it has
 * nothing to make it from. */
static int score(void) {
    unsigned h = 0;
    const char *arg = mx.key;
    int kind = kind_of(arg, &h), n = lib.n, seeds = 0;
    song_t *v = lib.v;
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
            return 0;
        }
        mx_rank(v, n);
        mx.seeded = !song;
    } else {
        snprintf(mx.name, sizeof mx.name, "%s",
                 kind == MIX_LB ? "ListenBrainz Picks" : "Last.fm Similar Artists");
        mx_items(v, n, mx.net.v, mx.net.n, kind);
    }
    mx.libgen = lib.gen;
    return 1;
}

/* The mix's songs found again in a library read since it was made, scored afresh for a re-roll.
 * 0 when one has gone. ponytail: songs struck before the read may be offered again. */
static int relink(void) {
    if (!score()) return 0;
    for (int i = 0; i < mx.len; ++i)
        if ((mx.list[i] = lib_find(mx.id[i])) < 0) return 0;
    for (int i = 0; i < mx.len; ++i) lib.v[mx.list[i]].flags = MX_USED;
    return 1;
}

/* The mix for arg made anew into mx: Last.fm's and ListenBrainz's asked for first, unless again
 * (a re-roll's), which makes it from what they said last time. */
static void craft(const char *arg, int again) {
    unsigned h = 0;
    int kind = kind_of(arg, &h), lb = kind == MIX_LB;
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
    if (!score()) return;
    song_t *v = lib.v;
    int n = lib.n;
    int target = length_min * 60, secs = 0;
    mx.len = mx_craft(v, n, mx.list, mx.seeded, target);
    if (kind < MIX_SHUFFLE) mx_flow(v, n, mx.list, mx.len);
    for (int i = 0; i < mx.len; ++i) secs += MX_SECS(v[mx.list[i]].sec);
    if (!mx.len || (kind < MIX_LASTFM && mx.len < MX_MIN && 2 * secs < target)) {
        int any = 0, fits = 0; /* songs for it, and whether any is short enough */
        for (int i = 0; i < n; ++i)
            if (v[i].score > 0 && !MX_SHORT(&v[i]))
                any = 1, fits |= MX_SECS(v[i].sec) <= target + target / 5;
        mx.len = 0;
        if (any && !fits)
            snprintf(mx.said, sizeof mx.said,
                     "Its songs are all longer than %d minutes: choose a longer Mix Length",
                     length_min);
        else if (kind < MIX_SOUND)
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

/* The mix's page: Play, its songs (Centre strikes one), Re-roll and Save, then what Last.fm or
 * ListenBrainz recommends that isn't on the card. */
static void mix_rows(rep_t *r) {
    char ctx[32], sub[300], t[16];
    int kept = 0, struck = 0, total = 0;
    snprintf(ctx, sizeof ctx, "%d", mx.gen);
    for (int i = 0; i < mx.len; ++i)
        if (mx.struck[i])
            ++struck;
        else if (mx.row[i].path[0])
            ++kept, total += MX_SECS(mx.row[i].sec);
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
        t[0] = 0;
        if (mx.row[i].sec > 0)
            snprintf(t, sizeof t, "%d:%02d", mx.row[i].sec / 60, mx.row[i].sec % 60);
        if (mx.struck[i])
            snprintf(sub, sizeof sub, "Struck \xc2\xb7 Re-roll replaces it");
        else if (!mx.row[i].path[0])
            snprintf(sub, sizeof sub, "Not on the card");
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
    for (int k = 0; k < FIND_ROWS && r->count < ROWS && mx.kind >= MIX_LASTFM; ++k) {
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
    if (mx.len) snprintf(r->msg, sizeof r->msg, "refresh"); /* a strike's mark moves at once */
}

/* A mix that couldn't be made (no link yet, a library update) is tried again at each open. */
static void page_mix(rep_t *r, const char *arg) {
    if (strcmp(mx.key, arg) || (mx.pending && nw.done) || (!mx.len && !mx.pending)) craft(arg, 0);
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

/* How much of s fits in max bytes, cut on a character's boundary. */
static int cut(const char *s, int max) {
    int n = (int)strlen(s);
    if (n <= max) return n;
    while (max && ((unsigned char)s[max] & 0xc0) == 0x80) --max;
    return max;
}

/* A request's stamp for Q2 Pod, new each time: the time and a count. */
static int stamp(char *o, size_t room) {
    static unsigned seq;
    return snprintf(o, room, "%lx%u", (unsigned long)time(0), ++seq);
}

/* ctx's mix as an .m3u into *out (freed by the caller): its length, or 0 with r's message saying
 * why there is none. */
static int queue_text(rep_t *r, const char *ctx, char **out) {
    size_t n = 3;
    char *t = *out = ours(ctx) ? malloc(4 + (size_t)mx.len * (sizeof mx.row[0].path + 1)) : 0;
    if (!t) {
        snprintf(r->msg, sizeof r->msg,
                 ours(ctx) ? "Not enough memory" : "This Mixtape has changed");
        return 0;
    }
    memcpy(t, "\xef\xbb\xbf", 3); /* UTF-8, as stock's playlist import reads it (no sniffing) */
    for (int i = 0; i < mx.len; ++i) {
        char *p = mx.row[i].path;
        if (p[0] && access(R(p), 0)) p[0] = 0; /* gone since: Q2 Pod's queue would wait on it */
        if (!mx.struck[i] && p[0]) n += (size_t)sprintf(t + n, "%s\n", p);
    }
    if (n == 3) snprintf(r->msg, sizeof r->msg, "Every song is struck or not on the card");
    return n == 3 ? 0 : (int)n;
}

static void act_play(rep_t *r, const char *ctx) {
    char *t, line[256];
    int n = queue_text(r, ctx, &t);
    if (!n) {
        free(t);
        return;
    }
    if (!has_cap("playlist"))
        snprintf(r->msg, sizeof r->msg, "Update Q2 Pod to play a Mixtape");
    else if (mkdir(R(APP_DIR), 0755), !put_file(R(QUEUE), t, (size_t)n))
        snprintf(r->msg, sizeof r->msg, "Can't write to the card");
    else {
        int k = stamp(line, sizeof line);
        k += snprintf(line + k, sizeof line - (size_t)k, " %s\n", QUEUE);
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
    int none = !memchr(mx.struck, 1, (size_t)mx.len), fresh = mx.kind == MIX_SHUFFLE && none;
    if (!fresh && lib_load(0) && !mx.pending && (lib.gen == mx.libgen || relink())) {
        int len = mx_reroll(lib.v, lib.n, mx.list, mx.len, mx.struck, length_min * 60);
        if (len) {
            if (none && mx.kind < MIX_SHUFFLE) /* all anew; struck ones keep the order */
                mx_flow(lib.v, lib.n, mx.list, len);
            mx.len = len, ++mx.gen;
            rows_fetch();
            return;
        }
    }
    craft(key, 1); /* a new shuffle, a song gone from the library, or nothing else fit */
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

/* The adds of the request Q2 Pod hasn't answered yet into out: it takes a request only when
 * none is running (and runs it once nothing plays), so a newer file would replace one it hasn't
 * taken. An add it already ran only says "exists" again. */
static size_t carried(char *out, size_t room) {
    char done[32] = "", old[8192];
    size_t n = 0, k;
    FILE *f = fopen(R(LIBRARY ".done"), "rb");
    if (f && fscanf(f, "%31s", done) != 1) done[0] = 0;
    if (f) fclose(f);
    f = fopen(R(LIBRARY), "rb");
    k = f ? fread(old, 1, sizeof old - 1, f) : 0;
    if (f) fclose(f);
    old[k] = 0;
    char *l = strchr(old, '\n'), *e;
    if (!l || (*l = 0, !strcmp(old, done))) return 0; /* none, or answered */
    for (++l; (e = strchr(l, '\n')); l = e + 1)
        if (!strncmp(l, "add ", 4) && n + (size_t)(e - l) + 1 < room)
            memcpy(out + n, l, (size_t)(e - l) + 1), n += (size_t)(e - l) + 1;
    return n;
}

static void act_save(rep_t *r, const char *ctx) {
    char *t, name[100], file[200], line[8704];
    int n = queue_text(r, ctx, &t);
    if (!n) {
        free(t);
        return;
    }
    playlist_name(name, sizeof name, file, sizeof file);
    mkdir(R(PLAYLISTS), 0755);
    if (!put_file(R(file), t, (size_t)n))
        snprintf(r->msg, sizeof r->msg, "Can't write to the card");
    else if (!has_cap("library"))
        snprintf(r->msg, sizeof r->msg,
                 "Saved: Import Playlists adds \xe2\x80\x9c%.60s\xe2\x80\x9d", name);
    else {
        size_t k = (size_t)stamp(line, sizeof line);
        line[k++] = '\n';
        k += carried(line + k, sizeof line - k - sizeof file - 8);
        k += (size_t)snprintf(line + k, sizeof line - k, "add %s\n", file);
        if (put_file(R(LIBRARY), line, k))
            snprintf(r->msg, sizeof r->msg,
                     "\xe2\x80\x9c%.*s\xe2\x80\x9d joins Playlists once nothing plays and you've "
                     "left the Library",
                     cut(name, 44), name);
        else
            snprintf(r->msg, sizeof r->msg, "Can't ask Q2 Pod to add it");
    }
    free(t);
}

/* ---- Pages -------------------------------------------------------------------------------- */

static void page_menu(rep_t *r) {
    scan_t s;
    char a[16], b[16], sub[96] = "How each song sounds, for the sound mixes", len[24];
    int scanning = scanner > 0 && scan_read(&s);
    if (scanning && !strcmp(s.phase, "listen"))
        snprintf(sub, sizeof sub, "Listening: %s of %s", commas(s.done, a), commas(s.total, b));
    else if (scanning)
        snprintf(sub, sizeof sub, "Checking your songs\xe2\x80\xa6");
    row(r, "list", "mix", "Shuffle", "Any songs from your Library", "shuffle");
    row(r, "list", "played", "Most Played", "Pick a song to mix from", "");
    row(r, "list", "mix", "Forgotten Favourites", "Loved or played a lot, but not lately",
        "forgotten");
    row(r, "list", "mix", "Never Played", "Songs with no play counted", "never");
    row(r, "list", "mix", "Deep Cuts", "Unplayed songs by the artists you play most", "deep");
    row(r, "list", "albums", "From an Album", "Songs that sound like it", "");
    row(r, "list", "artists", "From an Artist", "Songs that sound like theirs", "");
    row(r, "list", "songs", "From a Song", "Songs that sound like it", "");
    row(r, "list", "lastfm", "Last.fm", "Its similar artists, from your card", "");
    row(r, "list", "listenbrainz", "ListenBrainz", "Its picks for you, from your card", "");
    row(r, "list", "scan", "Listen to Your Music", sub, "");
    snprintf(len, sizeof len, "%d minutes", length_min);
    row(r, "list", "lengths", "Mix Length", len, "");
}

/* Mix Length's page: a row a length, the one chosen marked. */
static void page_lengths(rep_t *r) {
    for (unsigned i = 0; i < sizeof LENGTHS / sizeof *LENGTHS; ++i) {
        char t[24], c[8];
        snprintf(t, sizeof t, "%d minutes", LENGTHS[i]);
        snprintf(c, sizeof c, "%d", LENGTHS[i]);
        row(r, "action", "length", t, LENGTHS[i] == length_min ? "Chosen" : "", c);
    } /* asked again after each choice, so the mark moves at once */
}

static void act_length(rep_t *r, const char *ctx) {
    int m = atoi(ctx);
    char t[8];
    if (!length_ok(m) || m == length_min) return;
    length_min = m, mx.key[0] = 0; /* every mix made again at its next open */
    int k = snprintf(t, sizeof t, "%d\n", m);
    mkdir(R(APP_DIR), 0755);
    if (!put_file(R(LENGTH), t, (size_t)k))
        snprintf(r->msg, sizeof r->msg, "Can't write to the card");
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

/* A song's row: its title, its artist (and note) as the subtitle, a press making a mix like it. */
static void song_row(rep_t *r, void *db, const song_t *s, const char *note) {
    char path[1024], title[128], artist[128], sub[300], c[24];
    if (!song_text(db, s->id, path, sizeof path, title, sizeof title, artist, sizeof artist))
        return;
    snprintf(sub, sizeof sub, "%s%s%s", artist, artist[0] && note[0] ? ", " : "", note);
    snprintf(c, sizeof c, "song %u", s->id);
    row(r, "list", "mix", title, sub, c);
}

/* From a Song: every song of the Library, a window of ROWS from the cursor (an index into lib.v,
 * which is in id order), Search first. ponytail: in the Library's order, not by title; Search
 * finds one by name. */
static void page_songs(rep_t *r, const char *cursor) {
    if (!lib_load(0)) {
        info(r, "No Library yet: run Update Library", "");
        return;
    }
    int off = atoi(cursor), i = off;
    void *db = db_open();
    if (!off) row(r, "search", "", "Search", "", "");
    for (; i < lib.n && r->count < ROWS; ++i) song_row(r, db, &lib.v[i], "");
    if (db) db_close(db);
    if (i < lib.n)
        snprintf(r->cursor, sizeof r->cursor, "%d", i), snprintf(r->msg, sizeof r->msg, "pages");
}

static int by_plays(const void *a, const void *b) {
    const song_t *x = &lib.v[*(const int *)a], *y = &lib.v[*(const int *)b];
    return x->plays != y->plays ? (int)y->plays - (int)x->plays : x->recency - y->recency;
}

/* Most Played: the songs with a count, the most played first (the newest first among equals),
 * a window of ROWS from the cursor. Q2 Pod keeps 512 counts. */
static void page_played(rep_t *r, const char *cursor) {
    if (!lib_load(1)) {
        info(r, "No Library yet: run Update Library", "");
        return;
    }
    int off = atoi(cursor), n = 0, *o = malloc((size_t)(lib.n + 1) * sizeof *o);
    if (!o) return;
    for (int i = 0; i < lib.n; ++i)
        if (lib.v[i].plays) o[n++] = i;
    qsort(o, (size_t)n, sizeof *o, by_plays);
    if (!n) info(r, "Play some music first", "This list is made from play counts");
    void *db = db_open();
    int i = off;
    for (char note[24]; i < n && r->count < ROWS; ++i)
        snprintf(note, sizeof note, "%u plays", lib.v[o[i]].plays),
            song_row(r, db, &lib.v[o[i]], note);
    if (db) db_close(db);
    free(o);
    if (i < n)
        snprintf(r->cursor, sizeof r->cursor, "%d", i), snprintf(r->msg, sizeof r->msg, "pages");
}

typedef struct {
    rep_t *r;
    const char *text;
} find_t;
/* A song whose title has the text, listed while the rows last (db_page's row callback). */
static int song_found(void *ctx, void *st) {
    find_t *f = ctx;
    int at = lib_find((unsigned)col_i(st, 0));
    const char *title = col_s(st, 1);
    if (at >= 0 &&
        strcasestr(title[0] && strcmp(title, "(null)") ? title : col_s(st, 3), f->text)) {
        char c[24], artist[128];
        snprintf(c, sizeof c, "song %u", lib.v[at].id);
        snprintf(artist, sizeof artist, "%s", strcmp(col_s(st, 2), "(null)") ? col_s(st, 2) : "");
        row(f->r, "list", "mix", title[0] && strcmp(title, "(null)") ? title : col_s(st, 3), artist,
            c);
    }
    return f->r->count >= ROWS;
}

static void page_search(rep_t *r, const char *text) {
    if (!lib_load(0)) {
        info(r, "No Library yet: run Update Library", "");
        return;
    }
    char c[24];
    int albums = 0, room = ROWS - SONG_ROWS; /* artists leave albums half of their room */
    for (int i = 0; i < lib.nalbums && albums < room / 2; ++i)
        albums += strcasestr(lib_str(lib.albums[i].name), text) ||
                  strcasestr(lib_str(lib.albums[i].artist), text);
    for (int i = 0; i < lib.nartists && r->count < room - albums; ++i)
        if (strcasestr(lib_str(lib.artists[i].name), text))
            snprintf(c, sizeof c, "artist %08x", lib.artists[i].hash),
                row(r, "list", "mix", lib_str(lib.artists[i].name), "Artist", c);
    for (int i = 0; i < lib.nalbums && r->count < room; ++i)
        if (strcasestr(lib_str(lib.albums[i].name), text) ||
            strcasestr(lib_str(lib.albums[i].artist), text))
            snprintf(c, sizeof c, "album %08x", lib.albums[i].hash),
                row(r, "list", "mix", lib_str(lib.albums[i].name), lib_str(lib.albums[i].artist),
                    c);
    void *db = db_open();
    unsigned last = 0;
    find_t f = { r, text };
    if (db)
        db_page(db,
                "select id,name,songer,filename from songtable where id>? and " MUSIC_SQL
                " order by id limit 500",
                &last, song_found, &f),
            db_close(db);
    if (!r->count) info(r, "Nothing has that name", "");
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
    for (int i = 0; i < n; ++i) heard += profs_find(&lib.profs, lib.v[i].pkey) != 0; /* or tried */
    snprintf(sub, sizeof sub, "%s of %s", commas(heard, a), commas(n, b));
    info(r, "Songs heard", sub);
    info(r, "Heard songs make the sound mixes", "From an Album, From an Artist, More Like This");
    static const char *const ENDS[] = { [END_CARD] = "Stopped: the card went away",
                                        [END_FFMPEG] = "Stopped: this Q2 has no ffmpeg",
                                        [END_LIBRARY] = "Stopped: the Library was updating",
                                        [END_NEWER] = "Stopped: update Mixtape first" };
    if (ok && !strcmp(s.phase, "end") && s.left > 0 && s.left <= END_NEWER && ENDS[s.left])
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
    int at = -1;
    if (path[0] && lib_load(0)) { /* by its key in memory: songtable has no index on paths */
        unsigned long long key = prof_key(path);
        for (int i = 0; i < lib.n && at < 0; ++i)
            if (lib.v[i].pkey == key) at = i;
    }
    unsigned id = at >= 0 ? lib.v[at].id : 0;
    char title[128] = "", artist[128] = "", p[1024];
    void *db = at >= 0 ? db_open() : 0;
    if (db)
        song_text(db, id, p, sizeof p, title, sizeof title, artist, sizeof artist), db_close(db);
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
    else if (!strcmp(op, "songs"))
        page_songs(r, x);
    else if (!strcmp(op, "played"))
        page_played(r, x);
    else if (!strcmp(op, "search"))
        page_search(r, x);
    else if (!strcmp(op, "scan"))
        page_scan(r);
    else if (!strcmp(op, "lengths"))
        page_lengths(r);
    else if (!strcmp(op, "length"))
        act_length(r, x);
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
    length_read();
    time_t last = time(0);
    int idle = IDLE_S;
#ifdef HOST /* the checks wait seconds, not minutes */
    if (getenv("MIXTAPE_IDLE_S")) idle = atoi(getenv("MIXTAPE_IDLE_S"));
#endif
    while (!quit) {
        struct pollfd p = { s, POLLIN, 0 };
        int k = poll(&p, 1, 1000);
        reap();
        if (k > 0) {
            int fd = accept(s, 0, 0);
            if (fd >= 0) serve_one(fd), last = time(0);
        }
        if (time(0) - last > idle && scanner <= 0 && !(nw.running && !nw.done)) break;
    }
    scan_stop();
    close(s);
    unlink(addr.sun_path);
    unlink(R(STATE));
    /* Stopped, not idle, where the firmware doesn't list playkept: Q2 Pod started again (back from
     * Rockbox) would take a .play left behind for a new one and play the old mix. Idle, or with
     * playkept, it stays, as Q2 Pod writes .playing (More Like This) only while it is there.
     * ponytail: without playkept, after a stop that Q2 Pod outlives (USB storage, Stop Streaming
     * Services) More Like This keeps the song of the stop, and Q2 Pod restarted by a crash still
     * plays the old mix; firmware with playkept fixes both. */
    if (quit && !has_cap("playkept")) unlink(R(PLAY));
    return 0;
}
