/* The library as Mixtape sees it: stock's song database read a page at a time, Q2 Pod's play
 * counts, hearts, song lengths and folder markers, and the profiles the scan and the desktop
 * indexer leave on the card. */
#include "mixtape.h"
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

/* The device's libsqlite3 (3.19), linked as the Q2 has it: only what is used. */
typedef struct sqlite3 sqlite3;
typedef struct sqlite3_stmt sqlite3_stmt;
int sqlite3_open_v2(const char *, sqlite3 **, int, const char *);
int sqlite3_close(sqlite3 *);
int sqlite3_busy_timeout(sqlite3 *, int);
int sqlite3_prepare_v2(sqlite3 *, const char *, int, sqlite3_stmt **, const char **);
int sqlite3_bind_int64(sqlite3_stmt *, int, long long);
int sqlite3_bind_text(sqlite3_stmt *, int, const char *, int, void (*)(void *));
int sqlite3_step(sqlite3_stmt *);
const unsigned char *sqlite3_column_text(sqlite3_stmt *, int);
long long sqlite3_column_int64(sqlite3_stmt *, int);
int sqlite3_finalize(sqlite3_stmt *);

/* Q2 Pod's own files on /mnt/data (docs/internals.md: Play counts, Ratings, Song durations,
 * Library scope). */
#define PLAYS_FILE "/mnt/data/ringnav-plays"     /* 512 {key, count}, newest first */
#define RATINGS_FILE "/mnt/data/ringnav-ratings" /* listen keys, low 3 bits the hearts */
#define ALBUM_RATINGS_FILE "/mnt/data/ringnav-album-ratings" /* album_ident, the same */
#define DUR_FILE "/mnt/data/ringnav-durations-v1"            /* "Q2DUR2\0", then rec by key */
#define SCOPE_FILE "/mnt/data/q2-library-scope"              /* "0/path" hidden, "1/path" shown */
#define PLAYS_SLOTS 512
#define RATE_SLOTS 4096
#define DB_PAGE 500

lib_t lib;

const char *R(const char *path) {
    static char buf[8][640];
    static unsigned k;
    const char *root = getenv("MIXTAPE_ROOT");
    char *b = buf[k++ & 7];
    snprintf(b, sizeof *buf, "%s%s", root ? root : "", path);
    return b;
}

long long file_stamp(const char *path) {
    struct stat s;
    return stat(path, &s) ? 0 : (long long)s.st_mtime * 1000003 + s.st_size;
}

/* ---- Keys --------------------------------------------------------------------------------- */

static unsigned bytes32(unsigned h, const void *s, unsigned n) {
    for (unsigned i = 0; i < n; ++i) h = (h ^ ((const unsigned char *)s)[i]) * 16777619u;
    return h;
}
/* Q2 Pod's fnv: FNV-1a 32, then a step more, so "ab"+"c" and "a"+"bc" differ. */
unsigned fnv32(unsigned h, const char *s) { return bytes32(h, s, s ? strlen(s) : 0) * 16777619u; }

static unsigned long long fnv64(unsigned long long h, const char *s) {
    for (; *s; ++s) h = (h ^ (unsigned char)*s) * 0x100000001b3ull;
    return h;
}
/* A profile's key: FNV-1a 64 of the path on the card (a computer sees the same), with a CUE
 * track's bounds after it, since an image's tracks share one file. */
unsigned long long prof_key(const char *path) {
    if (!strncmp(path, CARD "/", sizeof CARD)) path += sizeof CARD;
    return fnv64(0xcbf29ce484222325ull, path);
}
/* Q2 Pod's listen_key, which keys its play counts and hearts: the path's fnv, a CUE track's
 * start mixed in. Never 0. */
unsigned listen_key(const char *path, int cue) {
    unsigned key = fnv32(2166136261u, path);
    if (cue) key = bytes32(key, &cue, sizeof cue); /* the int's own bytes: both little-endian */
    return key | !key;
}

/* Q2 Pod's album_dir: path's folder, a disc folder (CD2, Disc 1) as its parent. */
static unsigned album_dir(const char *p) {
    const char *e = strrchr(p, '/');
    unsigned n = e ? (unsigned)(e - p) : 0, i = n;
    while (i && p[i - 1] >= '0' && p[i - 1] <= '9') --i;
    if (i == n) return n;
    i -= i && (p[i - 1] == ' ' || p[i - 1] == '_' || p[i - 1] == '-');
    if (i >= 3 && p[i - 3] == '/' && !strncasecmp(p + i - 2, "cd", 2)) return i - 3;
    if (i >= 5 && p[i - 5] == '/' && !strncasecmp(p + i - 4, "dis", 3) &&
        ((p[i - 1] | 32) == 'c' || (p[i - 1] | 32) == 'k'))
        return i - 5;
    return n;
}
static unsigned fnv_lower(unsigned h, const char *s) {
    for (; s && *s; ++s) {
        unsigned char c = (unsigned char)*s;
        h = (h ^ (c + ((unsigned)(c - 'A') < 26u) * 32)) * 16777619u;
    }
    return h * 16777619u;
}
/* Q2 Pod's album_ident, which keys album hearts: the name and album artist, else the name and
 * the folder (disc folders as one), ASCII case aside but the folder's. Never 0. */
unsigned album_ident(const char *x, const char *y, const char *p) {
    unsigned h = fnv_lower(2166136261u, x);
    h = x && *x && y && *y ? fnv_lower(h, y) : bytes32(h, p, album_dir(p)) * 16777619u;
    return h | !h;
}

/* s lowercased (ASCII) with only its letters, digits and other scripts' bytes, FNV-hashed, so
 * "The Beatles" and "Beatles", "AC/DC" and "ACDC" agree, as Last.fm's and ListenBrainz's names
 * meet the tags. A title stops at "(", "[" or " - " (a version: "Song (Live)", "Song - 2011
 * Remaster"), an artist at " feat" or " ft.". 0 for none. */
unsigned norm_hash(const char *s, int title) {
    if (!s) return 0;
    while (*s == ' ') ++s;
    if (!strncasecmp(s, "the ", 4)) s += 4;
    unsigned h = 2166136261u, any = 0;
    for (const char *p = s; *p; ++p) {
        unsigned char c = (unsigned char)*p;
        if (title ? c == '(' || c == '[' || !strncmp(p, " - ", 3)
                  : !strncasecmp(p, " feat", 5) || !strncasecmp(p, " ft.", 4))
            break;
        c += ((unsigned)(c - 'A') < 26u) * 32;
        if (c < 0x80 && !((unsigned)(c - 'a') < 26u || (unsigned)(c - '0') < 10u)) continue;
        h = (h ^ c) * 16777619u, any = 1;
    }
    return any ? h | !h : 0;
}

/* s's first n bytes lowercased (ASCII), FNV-hashed; 0 for none. */
unsigned lower_hash(const char *s, unsigned n) {
    unsigned h = 2166136261u;
    for (unsigned i = 0; s && i < n && s[i]; ++i) {
        unsigned char c = (unsigned char)s[i];
        h = (h ^ ((unsigned)(c - 'A') < 26u ? c + 32u : c)) * 16777619u;
    }
    return s && n && *s ? h | !h : 0;
}

/* A genre tag's first four names (split at ; / , |, spaces trimmed), hashed. */
void genres(const char *s, unsigned *g) {
    for (int k = 0; s && *s && k < 4;) {
        while (*s == ' ') ++s;
        unsigned n = 0;
        while (s[n] && s[n] != ';' && s[n] != '/' && s[n] != ',' && s[n] != '|') ++n;
        unsigned e = n;
        while (e && s[e - 1] == ' ') --e;
        if (e) g[k++] = lower_hash(s, e);
        s += n + !!s[n];
    }
}

/* ---- Profiles ----------------------------------------------------------------------------- */

static const prof_t *sorting;
static int by_key_then_place(const void *a, const void *b) {
    unsigned x = *(const unsigned *)a, y = *(const unsigned *)b;
    unsigned long long kx = sorting[x].key, ky = sorting[y].key;
    return kx != ky ? (kx > ky) - (kx < ky) : (x > y) - (x < y);
}

/* The profiles file mapped (the page cache holds it, not the heap) and indexed by key, each key's
 * last record its profile. A bad header is no file. Returns the songs it has. */
int profs_open(profs_t *p) {
    memset(p, 0, sizeof *p);
    int fd = open(R(PROF_FILE), O_RDONLY);
    struct stat s;
    if (fd < 0) return 0;
    if (fstat(fd, &s) || s.st_size < (long long)(sizeof(prof_head) + sizeof(prof_t))) {
        close(fd);
        return 0;
    }
    void *m = mmap(0, (size_t)s.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (m == MAP_FAILED) return 0;
    const prof_head *h = m;
    int n = (int)((s.st_size - (long long)sizeof *h) / (long long)sizeof(prof_t)), k = 0;
    if (memcmp(h->magic, PROF_MAGIC, 4) || h->version != PROF_VERSION ||
        h->record != sizeof(prof_t) || !(p->idx = malloc((size_t)n * sizeof *p->idx))) {
        munmap(m, (size_t)s.st_size);
        return 0;
    }
    p->map = m, p->len = (size_t)s.st_size, p->tab = (const prof_t *)(h + 1);
    for (int i = 0; i < n; ++i) p->idx[i] = (unsigned)i;
    sorting = p->tab;
    qsort(p->idx, (size_t)n, sizeof *p->idx, by_key_then_place);
    for (int i = 0; i < n; ++i) /* a key's last record wins */
        if (i + 1 == n || p->tab[p->idx[i + 1]].key != p->tab[p->idx[i]].key)
            p->idx[k++] = p->idx[i];
    return p->n = k;
}

void profs_close(profs_t *p) {
    if (p->map) munmap(p->map, p->len);
    free(p->idx);
    memset(p, 0, sizeof *p);
}

const prof_t *profs_find(const profs_t *p, unsigned long long key) {
    int lo = 0, hi = p->n;
    while (lo < hi) {
        int mid = lo + (hi - lo) / 2;
        if (p->tab[p->idx[mid]].key < key)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo < p->n && p->tab[p->idx[lo]].key == key ? &p->tab[p->idx[lo]] : 0;
}

/* ---- The database ------------------------------------------------------------------------- */

void *db_open(void) {
    sqlite3 *db = 0;
    if (sqlite3_open_v2(R(DB_FILE), &db, 1 /* READONLY */, 0)) {
        sqlite3_close(db);
        return 0;
    }
    sqlite3_busy_timeout(db, 2000);
    return db;
}
void db_close(void *db) { sqlite3_close(db); }

const char *col_s(void *st, int i) {
    const char *s = (const char *)sqlite3_column_text(st, i);
    return s ? s : "";
}
int col_i(void *st, int i) { return (int)sqlite3_column_int64(st, i); }

/* Each row of "select <cols> from <table> where id > ?" a page at a time, its first column the
 * id: a long read never keeps Q2 Pod from writing the database (a heart) for more than a page,
 * as one statement over 50,000 songs would. row returns nonzero to stop. The rows; -1 when the
 * database refused. */
int db_page(void *db, const char *sql, unsigned *last, int (*row)(void *, void *), void *ctx) {
    int total = 0, got, rc;
    do {
        sqlite3_stmt *st;
        if (sqlite3_prepare_v2(db, sql, -1, &st, 0)) return -1;
        sqlite3_bind_int64(st, 1, *last);
        for (got = 0; (rc = sqlite3_step(st)) == 100;) {
            *last = (unsigned)sqlite3_column_int64(st, 0);
            ++got;
            if (row(ctx, st)) {
                rc = 101, got = 0;
                break;
            }
        }
        sqlite3_finalize(st);
        if (rc != 101) return -1;
        total += got;
    } while (got == DB_PAGE);
    return total;
}

/* sql's one number; 0 when it has none. */
int db_int(void *db, const char *sql) {
    sqlite3_stmt *st;
    int n = 0;
    if (sqlite3_prepare_v2(db, sql, -1, &st, 0)) return 0;
    if (sqlite3_step(st) == 100) n = col_i(st, 0);
    sqlite3_finalize(st);
    return n;
}

/* The id of the first song at path (a CUE image's first track); 0 for none. */
unsigned song_id(void *db, const char *path) {
    sqlite3_stmt *st;
    unsigned id = 0;
    if (!db || sqlite3_prepare_v2(
                   db, "select id from songtable where fileurl=? order by id limit 1", -1, &st, 0))
        return 0;
    sqlite3_bind_text(st, 1, path, -1, (void (*)(void *))-1); /* SQLITE_TRANSIENT */
    if (sqlite3_step(st) == 100) id = (unsigned)col_i(st, 0);
    sqlite3_finalize(st);
    return id;
}

/* What a song shows: its title (the file's name without one) and artist, and its path. 0 when it
 * has gone. */
int song_text(void *db, unsigned id, char *path, unsigned np, char *title, unsigned nt,
              char *artist, unsigned na) {
    sqlite3_stmt *st;
    int ok = 0;
    if (!db || sqlite3_prepare_v2(
                   db, "select fileurl,name,songer,filename from songtable where id=?", -1, &st, 0))
        return 0;
    sqlite3_bind_int64(st, 1, id);
    if (sqlite3_step(st) == 100) {
        const char *t = col_s(st, 1), *f = col_s(st, 3), *dot = strrchr(f, '.');
        snprintf(path, np, "%s", col_s(st, 0));
        if (*t && strcmp(t, "(null)"))
            snprintf(title, nt, "%s", t);
        else
            snprintf(title, nt, "%.*s", dot ? (int)(dot - f) : (int)strlen(f), f);
        snprintf(artist, na, "%s", strcmp(col_s(st, 2), "(null)") ? col_s(st, 2) : "");
        ok = 1;
    }
    sqlite3_finalize(st);
    return ok;
}

/* ---- Folder markers ----------------------------------------------------------------------- */

/* Q2 Pod's library scope: a folder with a database.ignore file is out of the Library, one with
 * database.unignore back in, the closest marker deciding. A library update writes them down. */
static struct {
    char *text;
    struct {
        const char *path;
        unsigned len;
        int shown;
    } r[64];
    int n;
} scope;
void scope_load(void) {
    free(scope.text);
    memset(&scope, 0, sizeof scope);
    FILE *f = fopen(R(SCOPE_FILE), "rb");
    if (!f) return;
    long n = (fseek(f, 0, SEEK_END), ftell(f));
    rewind(f);
    if (n > 0 && n < (1 << 17) && (scope.text = calloc(1, (size_t)n + 1)) &&
        fread(scope.text, 1, (size_t)n, f) == (size_t)n)
        for (char *s = scope.text, *e; (e = strchr(s, '\n')) && scope.n < 64; s = e + 1) {
            *e = 0;
            if ((*s == '0' || *s == '1') && s[1] == '/')
                scope.r[scope.n].path = s + 1, scope.r[scope.n].len = (unsigned)(e - s - 1),
                scope.r[scope.n++].shown = *s == '1';
        }
    fclose(f);
}
/* Whether the Library shows path. ponytail: the first 64 markers. */
int music(const char *path) {
    unsigned best = 0;
    int shown = 1;
    for (int i = 0; i < scope.n; ++i)
        if (scope.r[i].len > best && !strncmp(path, scope.r[i].path, scope.r[i].len) &&
            path[scope.r[i].len] == '/')
            best = scope.r[i].len, shown = scope.r[i].shown;
    return shown;
}

/* ---- Loading ------------------------------------------------------------------------------ */

typedef struct {
    group_t *v;
    int n, cap, *slot;
    unsigned mask;
} groups_t;
static groups_t albums, artists;
static struct {
    const unsigned char *map;
    size_t len;
    unsigned n;
} dur;

static unsigned pool_add(const char *s) {
    unsigned n = (unsigned)strlen(s) + 1;
    if (lib.used + n > lib.cap) {
        unsigned cap = lib.cap ? lib.cap * 2 : 1 << 16;
        while (cap < lib.used + n) cap *= 2;
        char *p = realloc(lib.pool, cap);
        if (!p) return 0; /* offset 0 is "" */
        lib.pool = p, lib.cap = cap;
    }
    memcpy(lib.pool + lib.used, s, n);
    return (lib.used += n) - n;
}
const char *lib_str(unsigned off) { return lib.pool && off < lib.used ? lib.pool + off : ""; }

static void group_add(groups_t *g, unsigned h, const char *name, const char *artist) {
    if (!h || !*name) return;
    if (2 * (unsigned)g->n >= g->mask) { /* the table at most half full */
        unsigned mask = g->mask ? 2 * g->mask + 1 : 1023;
        int *slot = malloc((mask + 1) * sizeof *slot);
        if (!slot) return;
        memset(slot, -1, (mask + 1) * sizeof *slot);
        for (int i = 0; i < g->n; ++i) {
            unsigned j = g->v[i].hash & mask;
            while (slot[j] >= 0) j = (j + 1) & mask;
            slot[j] = i;
        }
        free(g->slot);
        g->slot = slot, g->mask = mask;
    }
    unsigned j = h & g->mask;
    for (; g->slot[j] >= 0; j = (j + 1) & g->mask)
        if (g->v[g->slot[j]].hash == h) return;
    if (g->n == g->cap) {
        int cap = g->cap ? 2 * g->cap : 1024;
        group_t *v = realloc(g->v, (size_t)cap * sizeof *v);
        if (!v) return;
        g->v = v, g->cap = cap;
    }
    g->v[g->n] = (group_t){ h, pool_add(name), pool_add(artist) };
    g->slot[j] = g->n++;
}
static int by_name(const void *a, const void *b) {
    return strcasecmp(lib_str(((const group_t *)a)->name), lib_str(((const group_t *)b)->name));
}

/* Q2 Pod's measured lengths (Scan Song Durations), mapped while the library is read or a scan
 * runs. */
void dur_open(void) {
    int fd = open(R(DUR_FILE), O_RDONLY);
    struct stat s;
    if (fd >= 0 && !fstat(fd, &s) && s.st_size > 8) {
        void *m = mmap(0, (size_t)s.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
        if (m != MAP_FAILED && !memcmp(m, "Q2DUR2", 7))
            dur.map = m, dur.len = (size_t)s.st_size, dur.n = (unsigned)((s.st_size - 8) / 24);
        else if (m != MAP_FAILED)
            munmap(m, (size_t)s.st_size);
    }
    if (fd >= 0) close(fd);
}
void dur_close(void) {
    if (dur.map) munmap((void *)dur.map, dur.len);
    memset(&dur, 0, sizeof dur);
}
/* path's measured length in seconds; 0 unknown. */
int dur_sec(const char *path) {
    unsigned long long key = fnv64(0xcbf29ce484222325ull, path);
    unsigned lo = 0, hi = dur.n;
    while (lo < hi) { /* records: {u64 key, u32 size, mtime, ns, i32 sec} */
        unsigned mid = lo + (hi - lo) / 2;
        unsigned long long k;
        memcpy(&k, dur.map + 8 + 24 * (size_t)mid, 8);
        if (k < key)
            lo = mid + 1;
        else
            hi = mid;
    }
    int sec = 0;
    if (lo < dur.n && !memcmp(dur.map + 8 + 24 * (size_t)lo, &key, 8))
        memcpy(&sec, dur.map + 8 + 24 * (size_t)lo + 20, 4);
    return sec > 0 ? sec : 0;
}

static int cap_songs;
static int lib_row(void *ctx, void *st) {
    const char *path = col_s(st, 1), *album = col_s(st, 2), *aa = col_s(st, 3),
               *artist = col_s(st, 4), *year = col_s(st, 7);
    if (!music(path)) return 0;
    if (lib.n == cap_songs) {
        int cap = cap_songs ? cap_songs * 2 : 4096;
        song_t *v = realloc(lib.v, (size_t)cap * sizeof *v);
        if (!v) return 1;
        lib.v = v, cap_songs = cap;
    }
    if (!strcmp(artist, "(null)")) artist = ""; /* stock's untagged artist */
    song_t *s = &lib.v[lib.n++];
    memset(s, 0, sizeof *s);
    s->id = (unsigned)col_i(st, 0);
    s->lkey = listen_key(path, 0);
    s->pkey = prof_key(path);
    s->artist = norm_hash(artist, 0), s->album_artist = norm_hash(aa, 0);
    s->title = norm_hash(col_s(st, 5), 1);
    s->album = album_ident(album, aa, path);
    genres(col_s(st, 6), s->genre);
    if (year[0] >= '1' && year[0] <= '9' && year[1] >= '0' && year[1] <= '9' && year[2] >= '0' &&
        year[2] <= '9' && year[3] >= '0' && year[3] <= '9')
        s->year = (short)((year[0] - '0') * 1000 + (year[1] - '0') * 100 + (year[2] - '0') * 10 +
                          year[3] - '0');
    int sec = dur_sec(path);
    s->sec = (short)(sec < 32767 ? sec : 32767);
    s->recency = -1;
    group_add(&albums, s->album, album, *aa ? aa : artist);
    group_add(&artists, s->artist, artist, "");
    return 0;
}

static unsigned *words(const char *file, unsigned slots, unsigned *n) {
    FILE *f = fopen(R(file), "rb");
    unsigned *w = f ? calloc(slots, sizeof *w) : 0;
    *n = w ? (unsigned)fread(w, sizeof *w, slots, f) : 0;
    if (f) fclose(f);
    return w;
}
static int by_unsigned(const void *a, const void *b) {
    unsigned x = *(const unsigned *)a, y = *(const unsigned *)b;
    return (x > y) - (x < y);
}
/* The hearts kept for key in a sorted table of words (the key with its low three bits the
 * hearts): 0 for none, 6 and 7 too. */
static int hearts_of(const unsigned *w, unsigned n, unsigned key) {
    unsigned lo = 0, hi = n, k = key & ~7u;
    while (lo < hi) {
        unsigned mid = lo + (hi - lo) / 2;
        if ((w[mid] & ~7u) < k)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo < n && (w[lo] & ~7u) == k && (w[lo] & 7) <= 5 ? (int)(w[lo] & 7) : 0;
}
typedef struct {
    unsigned *v, n, cap;
} keys_t;
static int fav_row(void *ctx, void *st) {
    keys_t *k = ctx;
    if (k->n == k->cap) {
        unsigned cap = k->cap ? 2 * k->cap : 256, *v = realloc(k->v, cap * sizeof *v);
        if (!v) return 1;
        k->v = v, k->cap = cap;
    }
    k->v[k->n++] = listen_key(col_s(st, 1), col_i(st, 2));
    return 0;
}
static int has(const unsigned *v, unsigned n, unsigned key) {
    return n && bsearch(&key, v, n, sizeof *v, by_unsigned);
}

/* What changes as the user listens: play counts, hearts (My Fav is five), album hearts and the
 * profiles heard since. */
static void refresh(void *db) {
    struct {
        unsigned key, n;
    } plays[PLAYS_SLOTS] = { { 0 } };
    FILE *f = fopen(R(PLAYS_FILE), "rb");
    if (f) {
        if (fread(plays, sizeof plays, 1, f) != 1) memset(plays, 0, sizeof plays);
        fclose(f);
    }
    unsigned np = 0, pk[PLAYS_SLOTS], nr, na, last = 0;
    for (int i = 0; i < PLAYS_SLOTS; ++i)
        if (plays[i].key && plays[i].n) pk[np++] = plays[i].key;
    qsort(pk, np, sizeof *pk, by_unsigned);
    unsigned *rates = words(RATINGS_FILE, RATE_SLOTS, &nr),
             *arates = words(ALBUM_RATINGS_FILE, RATE_SLOTS, &na);
    if (rates) qsort(rates, nr, sizeof *rates, by_unsigned);
    if (arates) qsort(arates, na, sizeof *arates, by_unsigned);
    keys_t fav = { 0 };
    db_page(db, "select id,fileurl,time from myfav where id>? order by id limit 500", &last,
            fav_row, &fav);
    if (fav.n) qsort(fav.v, fav.n, sizeof *fav.v, by_unsigned);
    profs_close(&lib.profs);
    profs_open(&lib.profs);
    for (int i = 0; i < lib.n; ++i) {
        song_t *s = &lib.v[i];
        s->plays = 0, s->recency = -1;
        if (has(pk, np, s->lkey))
            for (int j = 0; j < PLAYS_SLOTS; ++j)
                if (plays[j].key == s->lkey) {
                    s->plays = (unsigned short)(plays[j].n < 65535 ? plays[j].n : 65535);
                    s->recency = (short)j;
                    break;
                }
        s->hearts = (unsigned char)(has(fav.v, fav.n, s->lkey) ? 5 : hearts_of(rates, nr, s->lkey));
        s->album_hearts = (unsigned char)hearts_of(arates, na, s->album);
        const prof_t *p = profs_find(&lib.profs, s->pkey);
        s->prof = p && p->status == MX_OK ? p : 0;
        if (s->sec <= 0 && p && p->sec) s->sec = (short)(p->sec < 32767 ? p->sec : 32767);
    }
    free(rates);
    free(arates);
    free(fav.v);
}

void lib_free(void) {
    profs_close(&lib.profs);
    free(lib.v);
    free(lib.albums);
    free(lib.artists);
    free(lib.pool);
    int gen = lib.gen;
    memset(&lib, 0, sizeof lib);
    lib.gen = gen + 1;
    cap_songs = 0;
}

/* The library, read again when the database has changed since; and with fresh, what the user
 * has played, hearted and had heard since. 0 when there is none (no card or no database). */
int lib_load(int fresh) {
    long long stamp = file_stamp(R(DB_FILE));
    void *db = stamp ? db_open() : 0;
    if (!db) return 0;
    if (stamp != lib.stamp || !lib.n) {
        lib_free();
        scope_load();
        dur_open();
        unsigned last = 0;
        pool_add("");
        int got =
            db_page(db,
                    "select id,fileurl,album,albumsonger,songer,name,style,year from songtable"
                    " where id>? and " MUSIC_SQL " order by id limit 500",
                    &last, lib_row, 0);
        dur_close();
        lib.albums = albums.v, lib.nalbums = albums.n;
        lib.artists = artists.v, lib.nartists = artists.n;
        free(albums.slot);
        free(artists.slot);
        memset(&albums, 0, sizeof albums);
        memset(&artists, 0, sizeof artists);
        qsort(lib.albums, (size_t)lib.nalbums, sizeof *lib.albums, by_name);
        qsort(lib.artists, (size_t)lib.nartists, sizeof *lib.artists, by_name);
        if (got < 0) lib_free(); /* a library update under it: next time */
        lib.stamp = got < 0 ? 0 : stamp;
        fresh = 1;
    }
    if (fresh && lib.n) refresh(db);
    db_close(db);
    return lib.n;
}

/* songtable's id in lib.v, which is in id order; -1 when not there. */
int lib_find(unsigned id) {
    int lo = 0, hi = lib.n;
    while (lo < hi) {
        int mid = lo + (hi - lo) / 2;
        if (lib.v[mid].id < id)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo < lib.n && lib.v[lo].id == id ? lo : -1;
}
