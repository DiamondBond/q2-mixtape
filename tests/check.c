/* Mixtape's host checks (make check, under ASan and UBSan): the JSON walker, the name matching,
 * the features, a synthetic 50,000-song library through every mix and the variety rules, and the
 * app itself over its socket, listening included. With arguments, it is the parity check's C side
 * (tests/parity.py): "analyse FILE", "features RAW", "key PATH" and
 * "lookup CARD RELPATH". */
#include "mixtape.h"
#include <fcntl.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/wait.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

typedef struct sqlite3 sqlite3;
int sqlite3_open_v2(const char *, sqlite3 **, int, const char *);
int sqlite3_exec(sqlite3 *, const char *, void *, void *, char **);
int sqlite3_close(sqlite3 *);

#define ROOT "build/check-root"
#define SMALL "build/check-small"
#define SONGS 50000
#define CHECK(c) ((c) ? (void)0 : (fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c), abort()))

static double now_s(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + t.tv_nsec / 1e9;
}
static void sh(const char *cmd) { CHECK(!system(cmd)); }
static void put(const char *path, const void *data, size_t n) {
    FILE *f = fopen(path, "wb");
    CHECK(f && fwrite(data, 1, n, f) == n && !fclose(f));
}
static char *slurp(const char *path, size_t *n) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    long k = ftell(f);
    rewind(f);
    char *s = calloc(1, (size_t)k + 1);
    CHECK(s && fread(s, 1, (size_t)k, f) == (size_t)k);
    fclose(f);
    if (n) *n = (size_t)k;
    return s;
}

/* ---- JSON and names ----------------------------------------------------------------------- */

typedef struct {
    char seen[16][160];
    int n, ends;
} seen_t;
static void collect(void *ctx, int depth, char **keys, const char *v) {
    seen_t *s = ctx;
    if (!v) {
        ++s->ends;
        return;
    }
    if (s->n == 16) return;
    int k = 0;
    for (int i = 0; i < depth && i < 8; ++i)
        k += snprintf(s->seen[s->n] + k, 160 - (size_t)k, "%s/", keys[i]);
    snprintf(s->seen[s->n++] + k, 160 - (size_t)k, "=%s", v);
}
static void check_json(void) {
    char a[] = "{\"payload\":{\"mbids\":[{\"recording_mbid\":\"a-1\",\"score\":0.5},{\"recording_"
               "mbid\":\"b-2\"}],"
               "\"count\":2,\"ok\":true,\"none\":null}}";
    seen_t s = { { { 0 } }, 0, 0 };
    CHECK(json_walk(a, collect, &s));
    CHECK(!strcmp(s.seen[0], "payload/mbids/recording_mbid/=a-1"));
    CHECK(!strcmp(s.seen[1], "payload/mbids/score/=0.5"));
    CHECK(!strcmp(s.seen[2], "payload/mbids/recording_mbid/=b-2"));
    CHECK(!strcmp(s.seen[3], "payload/count/=2") && !strcmp(s.seen[4], "payload/ok/=true"));
    CHECK(s.ends == 4); /* two recordings, payload, the whole */
    char b[] = "{\"name\":\"Bj\\u00f6rk \\ud83c\\udfb5 \\\"x\\\"\\n\"}";
    memset(&s, 0, sizeof s);
    CHECK(json_walk(b, collect, &s) &&
          !strcmp(s.seen[0], "name/=Bj\xc3\xb6rk \xf0\x9f\x8e\xb5 \"x\"\n"));
    char bad1[] = "{\"a\":", bad2[] = "{\"a\":\"x}", bad3[] = "[1,2] x",
         bad4[] = "{\"a\":\"\\u12\"}";
    CHECK(!json_walk(bad1, collect, &s) && !json_walk(bad2, collect, &s) &&
          !json_walk(bad3, collect, &s) && !json_walk(bad4, collect, &s));
    char deep[600];
    int k = 0;
    for (int i = 0; i < 20; ++i) k += sprintf(deep + k, "{\"k%d\":", i);
    k += sprintf(deep + k, "\"v\"");
    for (int i = 0; i < 20; ++i) deep[k++] = '}';
    deep[k] = 0;
    memset(&s, 0, sizeof s);
    CHECK(json_walk(deep, collect, &s) && s.n == 0); /* past JSON_DEPTH: walked, not reported */

    char *nest = calloc(1, 200001);
    memset(nest, '[', 32), memset(nest + 32, ']', 32);
    CHECK(json_walk(nest, collect, &s)); /* 32 deep: walked */
    memset(nest, '[', 200000);
    CHECK(!json_walk(nest, collect, &s)); /* deeper: refused, not a stack overflow */
    free(nest);

    CHECK(norm_hash("The Beatles", 0) == norm_hash("beatles", 0));
    CHECK(norm_hash("AC/DC", 0) == norm_hash("acdc", 0));
    CHECK(norm_hash("Bonobo feat. Totally Enormous Extinct Dinosaurs", 0) ==
          norm_hash("Bonobo", 0));
    CHECK(norm_hash("Kerala (Live)", 1) == norm_hash("kerala", 1));
    CHECK(norm_hash("Kerala - 2011 Remaster", 1) == norm_hash("Kerala", 1));
    CHECK(norm_hash("Bj\xc3\xb6rk", 0) != norm_hash("Bjrk", 0) &&
          norm_hash("Kerala", 1) != norm_hash("Cirrus", 1));
    CHECK(!norm_hash("", 0) && !norm_hash("  ", 1) && !norm_hash(0, 0));
}

/* ---- Features ----------------------------------------------------------------------------- */

static void tone(short *pcm, int n, double hz, double noise, unsigned seed) {
    srand(seed);
    for (int i = 0; i < n; ++i) {
        double t = (double)i / MX_RATE,
               v = 0.3 * sin(2 * 3.14159265 * hz * t) + 0.2 * sin(2 * 3.14159265 * hz * 2.01 * t);
        v += noise * ((double)rand() / RAND_MAX - 0.5);
        pcm[i] = (short)(v * 32767);
    }
}
static void check_features(void) {
    int n = MX_CLIP_S * MX_RATE;
    short *pcm = calloc((size_t)n, sizeof *pcm);
    float a[MX_DIMS], b[MX_DIMS], c[MX_DIMS];
    CHECK(!mx_features(pcm, n, a)); /* silence */
    tone(pcm, n, 220, 0.05, 1);
    CHECK(mx_features(pcm, n, a));
    for (int i = 0; i < n; ++i) pcm[i] /= 4; /* 12 dB down: loudness cancels */
    CHECK(mx_features(pcm, n, b));
    for (int k = 0; k < MX_DIMS; ++k) CHECK(fabsf(a[k] - b[k]) < 0.05f);
    tone(pcm, n, 1500, 0.4, 2);
    CHECK(mx_features(pcm, n, c));
    float d = 0;
    for (int k = 0; k < MX_CEPS; ++k) d += (a[k] - c[k]) * (a[k] - c[k]);
    CHECK(d > 1);                     /* another sound, another profile */
    CHECK(!mx_features(pcm, 500, c)); /* under ten frames */
    free(pcm);
}

/* ---- A synthetic library ------------------------------------------------------------------ */

/* Song i: album i / 10 (5,000), artist album % 1,500, genre album % 6, which its sound follows. */
static void song_path(char *o, size_t n, int i) {
    snprintf(o, n, "/mnt/mmc/Music/Artist %d/Album %d/%02d Song %d.flac", i / 10 % 1500, i / 10,
             i % 10, i);
}
static float cluster(int g, int k, unsigned *r) {
    *r = *r * 1103515245u + 12345u;
    return (float)(g * 7 % 11) * (k < MX_CEPS ? 2.0f : 0.5f) * (float)((k * 5 + g) % 7 - 3) +
           ((float)(*r >> 16 & 1023) / 1023.0f - 0.5f) * 1.5f;
}
typedef struct {
    unsigned long long key;
    unsigned size, mtime, ns;
    int sec;
} dur_t;
static int by_key(const void *a, const void *b) {
    unsigned long long x = ((const dur_t *)a)->key, y = ((const dur_t *)b)->key;
    return (x > y) - (x < y);
}
static unsigned long long path_key(const char *path) { /* Q2 Pod's duration key */
    unsigned long long h = 0xcbf29ce484222325ull;
    for (; *path; ++path) h = (h ^ (unsigned char)*path) * 0x100000001b3ull;
    return h;
}
static void put_durations(const char *file, dur_t *d, int n) {
    qsort(d, (size_t)n, sizeof *d, by_key); /* sorted by key, as the firmware keeps it */
    FILE *f = fopen(file, "wb");
    CHECK(f && fwrite("Q2DUR2\0\0", 8, 1, f) == 1 &&
          fwrite(d, sizeof *d, (size_t)n, f) == (size_t)n && !fclose(f));
}
static void make_library(void) {
    char cmd[512], path[256];
    sh("rm -rf " ROOT " && mkdir -p " ROOT "/mnt/data " ROOT "/mnt/mmc/.mixtape " ROOT
       "/mnt/mmc/Music " ROOT "/tmp");
    sqlite3 *db;
    CHECK(!sqlite3_open_v2(ROOT "/mnt/data/database.db", &db, 6, 0));
    static const char *schema = "create table %s(id INTEGER PRIMARY KEY, filename VARCHAR(512), "
                                "fileurl VARCHAR(1024), album VARCHAR(512),"
                                "songer VARCHAR(256), style VARCHAR(128), name VARCHAR(512), "
                                "composer VARCHAR(256), albumsonger VARCHAR(256),"
                                "year INTEGER, track INTEGER, cd_num INTEGER, time INTEGER, "
                                "time_end INTEGER, time_create INTEGER,"
                                "Initial INTEGER, bitdepth INTEGER, samprate INTEGER);";
    snprintf(cmd, sizeof cmd, schema, "songtable");
    CHECK(!sqlite3_exec(db, cmd, 0, 0, 0));
    snprintf(cmd, sizeof cmd, schema, "myfav");
    CHECK(!sqlite3_exec(db, cmd, 0, 0, 0));
    CHECK(!sqlite3_exec(
        db,
        "begin; with recursive n(i) as (select 0 union all select i+1 from n where i<49999) "
        "insert into "
        "songtable(id,filename,fileurl,album,songer,style,name,albumsonger,year,track,time,time_"
        "end,time_create) "
        "select i+1,printf('%02d Song %d.flac',i%10,i),"
        "printf('/mnt/mmc/Music/Artist %d/Album %d/%02d Song %d.flac',i/10%1500,i/10,i%10,i),"
        "'Album '||(i/10),'Artist '||(i/10%1500),"
        "case i/10%6 when 0 then 'Rock' when 1 then 'Jazz; Soul' when 2 then 'Electronic' when 3 "
        "then 'Folk' "
        "when 4 then 'Classical' else 'Hip-Hop' end,'Song "
        "'||i,'',(1960+i/10%60)||'-05-10',i%10+1,0,0,i "
        "from n; commit;",
        0, 0, 0));
    /* out of the Library's music: podcasts, a book; hidden by a marker; a CUE image's tracks */
    CHECK(!sqlite3_exec(
        db,
        "insert into songtable(id,fileurl,album,songer,name) values"
        "(60001,'/mnt/mmc/podcasts/Show/1.mp3','Show','Host','Ep 1'),"
        "(60002,'/mnt/mmc/Books/A.m4b','A','Writer','A'),"
        "(60003,'/mnt/mmc/Ringtones/Bell.mp3','Bells','Phone','Bell');"
        "insert into songtable(id,fileurl,album,songer,name,time,time_end) values"
        "(60004,'/mnt/mmc/Live/Gig.flac','Gig','Band','One',0,300),"
        "(60005,'/mnt/mmc/Live/Gig.flac','Gig','Band','Two',300,600);"
        "insert into myfav(id,fileurl,time) select id,fileurl,time from songtable where id<=100;",
        0, 0, 0));
    sqlite3_close(db);
    put(ROOT "/mnt/data/q2-library-scope", "0/mnt/mmc/Ringtones\n", 20);

    /* Q2 Pod's play counts: songs 101-612 played, the counts falling, newest first */
    struct {
        unsigned key, n;
    } plays[512];
    for (int i = 0; i < 512; ++i) {
        song_path(path, sizeof path, 100 + i);
        plays[i].key = listen_key(path), plays[i].n = (unsigned)(600 - i);
    }
    put(ROOT "/mnt/data/ringnav-plays", plays, sizeof plays);
    unsigned rates[4096] = { 0 }, arates[4096] = { 0 };
    for (int i = 0; i < 300; ++i) {
        song_path(path, sizeof path, 2000 + i);
        rates[i] = (listen_key(path) & ~7u) | (unsigned)(i % 4 + 1);
    }
    put(ROOT "/mnt/data/ringnav-ratings", rates, sizeof rates);
    for (int a = 0; a < 50; ++a) {
        char album[32];
        song_path(path, sizeof path, a * 10);
        snprintf(album, sizeof album, "Album %d", a);
        arates[a] = (album_ident(album, "", path) & ~7u) | 5;
    }
    put(ROOT "/mnt/data/ringnav-album-ratings", arates, sizeof arates);

    /* lengths, as Scan Song Durations leaves them: every 37th song an interlude, every 101st long
     */
    dur_t *d = calloc(SONGS, sizeof *d);
    for (int i = 0; i < SONGS; ++i) {
        song_path(path, sizeof path, i);
        d[i].key = path_key(path), d[i].sec = !(i % 37) ? 40 : i % 101 == 50 ? 900 : 150 + i % 200;
    }
    put_durations(ROOT "/mnt/data/ringnav-durations-v1", d, SONGS);
    free(d);

    /* the songs' files, empty: a mix queues only what is on the card */
    for (int i = 0; i < SONGS; ++i) {
        char full[320], *album, *artist;
        song_path(path, sizeof path, i);
        snprintf(full, sizeof full, ROOT "%s", path);
        if (!(i % 10)) {
            *(album = strrchr(full, '/')) = 0, *(artist = strrchr(full, '/')) = 0;
            mkdir(full, 0755), *artist = '/', mkdir(full, 0755), *album = '/';
        }
        int fd = open(full, O_CREAT | O_WRONLY, 0644);
        CHECK(fd >= 0 && !close(fd));
    }

    /* profiles for 40,000 songs, near their genre's sound, and one record a cut write left */
    FILE *f = fopen(ROOT PROF_FILE, "wb");
    prof_head h = { "", PROF_VERSION, sizeof(prof_t), 0 };
    memcpy(h.magic, PROF_MAGIC, 4);
    CHECK(f && fwrite(&h, sizeof h, 1, f) == 1);
    unsigned r = 7;
    for (int i = 0; i < 40000; ++i) {
        prof_t p = { 0 };
        song_path(path, sizeof path, i);
        p.key = prof_key(path), p.size = 1000, p.status = MX_OK;
        for (int k = 0; k < MX_DIMS; ++k) p.f[k] = cluster(i / 10 % 6, k, &r);
        CHECK(fwrite(&p, sizeof p, 1, f) == 1);
    }
    CHECK(fwrite(&h, 7, 1, f) == 1 && !fclose(f));
}

static int same_genre(int a, int b) { return lib.v[a].genre[0] == lib.v[b].genre[0]; }

static int played_artist(unsigned artist) {
    for (int i = 0; i < lib.n; ++i)
        if (lib.v[i].artist == artist && lib.v[i].plays) return 1;
    return 0;
}

/* The variety rules every mix keeps; a timed one (target > 0) at most a fifth over target seconds,
 * and short of it by less than half a song, most of the synthetic songs being 150 to 349 s. */
/* Songs heard along one dimension, each its own artist and album: seed 0 at 0, 1 near at 1, 2 a
 * favourite at 1.1, 3 a dislike at 0.5, the rest at i - 2. A favourite breaks near ties only (at
 * its full lift, 2 would win), a dislike counts in full; a play-count mix in sound order. */
static void check_choosing(void) {
    enum { N = 24 };
    static song_t v[N];
    static prof_t p[N];
    for (int i = 0; i < N; ++i) {
        p[i].status = MX_OK, p[i].f[0] = i < 4 ? (float[]){ 0, 1, 1.1f, 0.5f }[i] : (float)(i - 2);
        v[i].prof = &p[i], v[i].sec = 200, v[i].artist = v[i].album = (unsigned)i + 1;
    }
    v[0].seed = 1, v[2].hearts = 5, v[3].hearts = 1;
    mx_rank(v, N);
    CHECK(v[1].score > v[2].score && v[2].score > v[3].score && v[3].score > 0);
    v[7].prof = 0; /* not heard: last */
    int list[] = { 4, 9, 5, 8, 7, 6 }, want[] = { 4, 5, 6, 8, 9, 7 };
    mx_flow(v, N, list, 6);
    for (int i = 0; i < 6; ++i) CHECK(list[i] == want[i]);
    v[12].artist = v[14].artist; /* 14 left after 12, its artist's: first */
    int pair[] = { 13, 12, 14 }, pair_want[] = { 14, 13, 12 };
    mx_flow(v, N, pair, 3);
    for (int i = 0; i < 3; ++i) CHECK(pair[i] == pair_want[i]);
    for (int i = 0; i < N; ++i) v[i].prof = 0; /* nothing heard: the order drawn */
    int none[] = { 9, 4, 6 };
    mx_flow(v, N, none, 3);
    CHECK(none[0] == 9 && none[1] == 4 && none[2] == 6);
}

static void rules(const int *list, int len, int seeded, int target) {
    int total = 0;
    for (int i = 0; i < len; ++i) {
        const song_t *s = &lib.v[list[i]];
        CHECK(!MX_SHORT(s) && s->score > 0);
        if (i)
            CHECK(s->artist != lib.v[list[i - 1]].artist || (s->seed && lib.v[list[i - 1]].seed));
        int artist = 0, album = 0;
        for (int j = 0; j < len; ++j) {
            const song_t *o = &lib.v[list[j]];
            CHECK(j == i || list[j] != list[i]);
            if (j != i && !o->seed && !s->seed)
                artist += o->artist == s->artist, album += o->album == s->album;
        }
        CHECK(artist <= 1 && !album);
        total += MX_SECS(s->sec);
    }
    CHECK(target <= 0 || len == MX_MAX || (total > target - 175 && total <= target + target / 5));
    CHECK(!seeded || lib.v[list[0]].seed);
}

static void check_library(void) {
    make_library();
    setenv("MIXTAPE_ROOT", ROOT, 1);
    double t = now_s();
    int n = lib_load(1);
    double load = now_s() - t;
    CHECK(n == SONGS); /* no podcast, book, ringtone or CUE image's track */
    CHECK(lib.nalbums == 5000 && lib.nartists == 1500 && lib_find(60004) < 0);
    CHECK(lib.profs.n == 40000);
    int heard = 0, i0 = lib_find(1);
    for (int i = 0; i < n; ++i) heard += lib.v[i].prof != 0;
    CHECK(heard == 40000 && i0 == 0);
    CHECK(lib.v[i0].hearts == 5 && lib.v[i0].sec == 40 &&
          lib.v[i0].year == 1960); /* My Fav; an interlude */
    CHECK(lib.v[lib_find(101)].plays == 600 && lib.v[lib_find(101)].recency == 0);
    CHECK(lib.v[lib_find(2001)].hearts == 1 && lib.v[lib_find(2002)].hearts == 2);
    CHECK(lib.v[lib_find(11)].album_hearts == 5 && !lib.v[lib_find(1001)].album_hearts);
    printf("library: %d songs read in %.2f s, %zu bytes a song (%.1f MB), %d albums, %d artists\n",
           n, load, sizeof(song_t), (double)n * sizeof(song_t) / 1e6, lib.nalbums, lib.nartists);
    t = now_s();
    CHECK(lib_load(1) == n && lib.gen == 1); /* cached: only what the user changes is read again */
    printf("library: read again in %.3f s\n", now_s() - t);

    int list[MX_MAX];
    unsigned char struck[MX_MAX] = { 0 };
    for (int kind = MIX_MOST; kind <= MIX_DEEP; ++kind) {
        t = now_s();
        mx_counts(lib.v, n, kind);
        int len = mx_craft(lib.v, n, list, 0, 2700), drawn[MX_MAX];
        memcpy(drawn, list, sizeof drawn);
        mx_flow(lib.v, n, list, len); /* in sound order: the same songs, the rules kept */
        for (int i = 0, in; i < len; ++i) {
            for (int j = in = 0; j < len; ++j) in |= drawn[j] == list[i];
            CHECK(in);
        }
        CHECK(len >= MX_MIN);
        rules(list, len, 0, 2700);
        for (int i = 0; i < len; ++i) {
            const song_t *s = &lib.v[list[i]];
            if (kind == MIX_MOST) CHECK(s->plays >= 100); /* the 512 counted, the most first */
            if (kind == MIX_FORGOTTEN)
                CHECK((s->plays >= 3 || s->hearts >= 4) && (s->recency < 0 || s->recency >= 100));
            if (kind == MIX_NEVER || kind == MIX_DEEP) CHECK(!s->plays);
            if (kind == MIX_DEEP) CHECK(played_artist(s->artist));
        }
        printf("mix %d: %d songs in %.3f s\n", kind, len, now_s() - t);
    }
    /* Shuffle: any song but a disliked one, a new draw each time, at every length */
    static const int TARGETS[] = { 300, 600, 1200, 2700, 3600 };
    for (int k = 0; k < 5; ++k) {
        int a[MX_MAX], na, nb, same = 0, target = TARGETS[k];
        mx_counts(lib.v, n, MIX_SHUFFLE);
        na = mx_craft(lib.v, n, a, 0, target);
        rules(a, na, 0, target);
        mx_counts(lib.v, n, MIX_SHUFFLE);
        nb = mx_craft(lib.v, n, list, 0, target);
        rules(list, nb, 0, target);
        for (int i = 0; i < na; ++i) {
            CHECK(lib.v[a[i]].hearts != 1);
            for (int j = 0; j < nb; ++j) same += a[i] == list[j];
        }
        CHECK(na >= 1 && same * 2 < na);
        unsigned char one[MX_MAX] = { 0 };
        one[0] = 1;
        nb = mx_reroll(lib.v, n, list, nb, one, target); /* struck: the length kept */
        rules(list, nb, 0, target);
        printf("shuffle of %d min: %d songs\n", target / 60, na);
    }

    /* an album: its best-loved track first, two more of its own a third and two thirds in, the rest
     * mostly of its sound */
    t = now_s();
    unsigned album = lib.v[lib_find(12346)].album;
    for (int i = 0; i < n; ++i) lib.v[i].seed = lib.v[i].album == album;
    mx_rank(lib.v, n);
    int len = mx_craft(lib.v, n, list, 1, 2700), near = 0, seed = lib_find(12346);
    rules(list, len, 1, 2700);
    CHECK(lib.v[list[3]].seed && lib.v[list[7]].seed && len >= 10); /* 45 min: about 11 songs */
    for (int i = 0; i < len; ++i) near += same_genre(list[i], seed);
    CHECK(near >= len * 3 / 4);
    printf("album mix: %d songs, %d of its genre's sound, in %.3f s\n", len, near, now_s() - t);
    for (int i = 0; i < len; ++i) struck[i] = i % 3 == 1;
    int gone[MX_MAX], ng = 0;
    for (int i = 0; i < len; ++i)
        if (struck[i]) gone[ng++] = list[i];
    int len2 = mx_reroll(lib.v, n, list, len, struck, 2700);
    CHECK(len2 >= len - 1);
    for (int i = 0; i < len2; ++i)
        for (int j = 0; j < ng; ++j) CHECK(list[i] != gone[j]);
    rules(list, len2, 1, 2700);
    int before[MX_MAX], nb = len2;
    memcpy(before, list, sizeof before);
    /* none struck: all but the album's own anew */
    len2 = mx_reroll(lib.v, n, list, len2, struck, 2700);
    CHECK(len2 >= nb - 1);
    for (int i = 0; i < len2; ++i)
        for (int j = 0; j < nb; ++j) CHECK(lib.v[list[i]].seed || list[i] != before[j]);
    rules(list, len2, 1, 2700);

    /* an artist (all 3 of its albums); and a song, never placed itself */
    unsigned artist = lib.v[seed].artist;
    for (int i = 0; i < n; ++i) lib.v[i].seed = lib.v[i].artist == artist;
    mx_rank(lib.v, n);
    len = mx_craft(lib.v, n, list, 1, 2700);
    rules(list, len, 1, 2700);
    for (int i = 0; i < n; ++i) lib.v[i].seed = i == seed;
    mx_rank(lib.v, n);
    len = mx_craft(lib.v, n, list, 0, 2700);
    rules(list, len, 0, 2700);
    for (int i = 0; i < len; ++i) CHECK(list[i] != seed);

    /* ListenBrainz: two of its picks on the card (one by its second credited artist), one not */
    item_t it[3] = { { 0 } };
    it[0].title = norm_hash("Song 778 (Live)", 1), it[0].artist[0] = norm_hash("Artist 77", 0),
    it[0].score = 1;
    it[1].title = norm_hash("Song 31415", 1), it[1].artist[0] = norm_hash("Somebody", 0),
    it[1].artist[1] = norm_hash("artist 141", 0), it[1].score = 0.9f;
    it[2].title = norm_hash("Nowhere", 1), it[2].artist[0] = norm_hash("Nobody", 0),
    it[2].score = 0.8f;
    mx_items(lib.v, n, it, 3, MIX_LB);
    len = mx_craft(lib.v, n, list, 0, 2700);
    CHECK(len == 2);
    int matched = 0;
    for (int i = 0; i < 3; ++i) matched += it[i].matched;
    CHECK(matched == 2);
    /* Last.fm: three similar artists, one of them a top artist of the user's own */
    item_t fm[3] = { { 0 } };
    for (int i = 0; i < 3; ++i) {
        char name[32];
        snprintf(name, sizeof name, "The Artist %d", 200 + i);
        fm[i].artist[0] = norm_hash(name, 0), fm[i].score = 1, fm[i].top = i == 2;
    }
    mx_items(lib.v, n, fm, 3, MIX_LASTFM);
    len = mx_craft(lib.v, n, list, 0, 2700);
    CHECK(len >= 4 && len <= 4 * 2);
    for (int i = 0; i < len; ++i)
        CHECK(lib.v[list[i]].artist == fm[0].artist[0] || lib.v[list[i]].artist == fm[1].artist[0]);
    CHECK(fm[0].matched && fm[1].matched && !fm[2].matched);

    struct rusage u;
    getrusage(RUSAGE_SELF, &u);
    printf("library checks: peak RSS %.1f MB (ASan's own included)\n", u.ru_maxrss / 1024.0);
    lib_free();
}

/* ---- Last.fm and ListenBrainz, from a server of the checks' own -------------------------- */

static const char *const ANSWERS[][3] = {
    /* what the request has, the status, the body */
    { "user.gettopartists&limit=20&user=rj", "200",
      "{\"topartists\":{\"artist\":[{\"name\":\"Artist "
      "300\",\"playcount\":\"50\"},{\"name\":\"Artist 301\"}],"
      "\"@attr\":{\"user\":\"rj\"}}}" },
    { "user.gettopartists", "404", "{\"error\":6,\"message\":\"User not found\",\"links\":[]}" },
    { "artist=Artist%20300", "200",
      "{\"similarartists\":{\"artist\":[{\"name\":\"Artist "
      "302\",\"match\":\"0.9\"},{\"match\":\"0.5\",\"name\":"
      "\"The Artist 303\"},{\"name\":\"Nobody "
      "Here\",\"match\":\"0.8\",\"image\":[{\"#text\":\"\"}]}]}}" },
    { "artist=Artist%20301", "200",
      "{\"similarartists\":{\"artist\":[{\"name\":\"Artist 300\",\"match\":\"1\"}]}}" },
    { "/user/someone/", "200",
      "{\"payload\":{\"mbids\":[{\"recording_mbid\":\"m1\",\"score\":1},{\"recording_mbid\":\"m2\"}"
      ","
      "{\"recording_mbid\":\"m3\"}],\"user_name\":\"someone\"}}" },
    { "/user/ghost/", "404", "{\"code\":404,\"error\":\"Cannot find user: ghost\"}" },
    { "/user/new/", "204", "" },
    { "recording_mbids=m1,m2,m3", "200",
      "{\"m2\":{\"artist\":{\"name\":\"Nobody\",\"artists\":[{\"name\":\"Nobody\"}]},\"recording\":"
      "{\"name\":"
      "\"Nowhere\"}},\"m1\":{\"artist\":{\"name\":\"Somebody & Artist "
      "77\",\"artists\":[{\"name\":\"Somebody\"},"
      "{\"name\":\"Artist 77\"}]},\"recording\":{\"name\":\"Song 778 (Live)\",\"length\":200000}},"
      "\"m3\":{\"recording\":{\"name\":\"Nobody's\"}}}" },
};
static pid_t web;
static void web_start(void) {
    struct sockaddr_in a = { AF_INET, 0, { htonl(INADDR_LOOPBACK) }, { 0 } };
    socklen_t n = sizeof a;
    int s = socket(AF_INET, SOCK_STREAM, 0);
    CHECK(s >= 0 && !bind(s, (struct sockaddr *)&a, sizeof a) && !listen(s, 8) &&
          !getsockname(s, (struct sockaddr *)&a, &n));
    char url[64];
    snprintf(url, sizeof url, "http://127.0.0.1:%d", ntohs(a.sin_port));
    setenv("MIXTAPE_NET", url, 1);
    if ((web = fork())) {
        close(s);
        return;
    }
    prctl(PR_SET_PDEATHSIG, SIGKILL);
    for (int fd; (fd = accept(s, 0, 0)) >= 0; close(fd)) {
        char req[8192], head[160];
        int k = 0;
        for (int got; k < 8191 && !strstr(req, "\r\n\r\n") &&
                      (got = (int)read(fd, req + k, 8191 - (size_t)k)) > 0;)
            k += got, req[k] = 0;
        *strchr(req, '\n') = 0; /* the request line */
        const char *const *at = 0;
        for (unsigned i = 0; !at && i < sizeof ANSWERS / sizeof *ANSWERS; ++i)
            if (strstr(req, ANSWERS[i][0])) at = ANSWERS[i];
        const char *code = at ? at[1] : "404", *body = at ? at[2] : "{}";
        int h = snprintf(head, sizeof head,
                         "HTTP/1.1 %s X\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n", code,
                         strlen(body));
        if (write(fd, head, (size_t)h) != h || write(fd, body, strlen(body)) < 0) continue;
    }
    _exit(0);
}

static void check_net(void) {
    net_t o;
    net_lastfm("rj", "k", &o);
    CHECK(!o.said[0] && o.n == 5);
    const item_t *first = 0;
    for (int i = 0; i < o.n; ++i)
        if (!first || o.v[i].score > first->score) first = &o.v[i];
    CHECK(!strcmp(first->name, "Artist 302") && first->score > 0.89f && !first->top);
    for (int i = 0; i < o.n; ++i) /* the user's own two, marked */
        CHECK(o.v[i].top == (o.v[i].artist[0] == norm_hash("Artist 300", 0) ||
                             o.v[i].artist[0] == norm_hash("Artist 301", 0)));
    free(o.v);
    net_lastfm("ghost", "k", &o);
    CHECK(!strcmp(o.said, "Last.fm: User not found"));
    free(o.v);
    net_lastfm("rj", "", &o);
    CHECK(!strcmp(o.said, "This Mixtape has no Last.fm API key"));
    free(o.v);

    net_listenbrainz("someone", &o);
    CHECK(!o.said[0] && o.n == 2 && !strcmp(o.v[0].name, "Song 778 (Live)") &&
          !strcmp(o.v[0].sub, "Somebody & Artist 77"));
    CHECK(o.v[0].title == norm_hash("song 778", 1) &&
          o.v[0].artist[2] == norm_hash("Artist 77", 0) && o.v[0].score > o.v[1].score);
    CHECK(!strcmp(o.v[1].name, "Nowhere")); /* m3 named no artist: dropped */
    free(o.v);
    net_listenbrainz("ghost", &o);
    CHECK(!strcmp(o.said, "ListenBrainz has no user ghost"));
    free(o.v);
    net_listenbrainz("new", &o);
    CHECK(!strcmp(o.said, "ListenBrainz has no recommendations for you yet"));
    free(o.v);
    char url[64];
    snprintf(url, sizeof url, "%s", getenv("MIXTAPE_NET"));
    setenv("MIXTAPE_NET", "http://127.0.0.1:1", 1);
    net_listenbrainz("someone", &o);
    CHECK(!strcmp(o.said, "Can't reach ListenBrainz"));
    free(o.v);
    setenv("MIXTAPE_NET", url, 1);
}

/* ---- The app, over its socket ------------------------------------------------------------- */

typedef struct {
    char *buf, *head[4], *f[60][6];
    int rows;
} reply_t;
static void unesc(char *s) {
    char *o = s;
    for (unsigned v; *s; ++o)
        if (*s == '%' && sscanf(s + 1, "%2x", &v) == 1)
            *o = (char)v, s += 3;
        else
            *o = *s++;
    *o = 0;
}
static void ask(const char *root, reply_t *r, const char *line) {
    char req[2048];
    struct sockaddr_un a = { AF_UNIX, "" };
    snprintf(a.sun_path, sizeof a.sun_path, "%s/tmp/q2-mixtape.browse.sock", root);
    int fd = -1;
    for (int i = 0; i < 100; ++i) {
        fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (!connect(fd, (struct sockaddr *)&a, sizeof a)) break;
        close(fd), fd = -1, usleep(50000);
    }
    CHECK(fd >= 0);
    int n = snprintf(req, sizeof req, "Q2SPOT/1\n7\t%s\n", line);
    CHECK(write(fd, req, (size_t)n) == n);
    shutdown(fd, SHUT_WR);
    free(r->buf);
    r->buf = calloc(1, 70000);
    struct timeval tv = { 5, 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    int got, k = 0;
    while ((got = (int)read(fd, r->buf + k, 69999 - (size_t)k)) > 0) k += got;
    CHECK(!got); /* closed: Q2 Pod reads a reply until the close */
    close(fd);
    CHECK(!strncmp(r->buf, "Q2SPOT/1\n", 9));
    char *s = r->buf + 9, *e;
    r->rows = -1;
    for (; (e = strchr(s, '\n')); s = e + 1) {
        *e = 0;
        char **out = r->rows < 0 ? r->head : r->f[r->rows];
        int k = 0, want = r->rows < 0 ? 4 : 6;
        for (char *c = s; k < want; ++k) {
            out[k] = c;
            c = strchr(c, '\t');
            if (c)
                *c++ = 0;
            else
                c = "";
            unesc(out[k]);
        }
        CHECK(k == want);
        CHECK(++r->rows <= 50);
    }
    CHECK(!strcmp(r->head[0], "7"));
}
static int find(reply_t *r, const char *title) {
    for (int i = 0; i < r->rows; ++i)
        if (!strcmp(r->f[i][2], title)) return i;
    return -1;
}

static pid_t app_start(const char *root, const char *caps) {
    char path[256];
    snprintf(path, sizeof path, "%s/tmp/q2-addon.caps", root);
    put(path, caps, strlen(caps));
    pid_t pid = fork();
    if (!pid) {
        prctl(PR_SET_PDEATHSIG, SIGKILL); /* a failed check leaves no app behind */
        setenv("MIXTAPE_ROOT", root, 1);
        execl("build/host/q2-mixtape", "q2-mixtape", (char *)0);
        _exit(127);
    }
    return pid;
}
static void app_end(pid_t pid) {
    int status;
    kill(pid, SIGTERM);
    CHECK(waitpid(pid, &status, 0) == pid && WIFEXITED(status) && !WEXITSTATUS(status));
}

static void check_app(void) {
    reply_t r = { 0 };
    char path[512], c[64];
    pid_t pid = app_start(ROOT, "playlist progress pages library goto field more fav\n");
    ask(ROOT, &r, "menu");
    CHECK(!strcmp(r.head[1], "ok") && r.rows == 11 && !strcmp(r.f[0][1], "mix") &&
          !strcmp(r.f[0][4], "shuffle") && !strcmp(r.f[10][3], "45 minutes"));
    ask(ROOT, &r, "nothing");
    CHECK(!strcmp(r.head[1], "unsupported"));

    /* no database for a moment (an Update Library): said, and tried again at the next open */
    CHECK(!rename(ROOT "/mnt/data/database.db", ROOT "/mnt/data/away.db"));
    ask(ROOT, &r, "mix\tmost");
    CHECK(r.rows == 1 && !strcmp(r.f[0][2], "No Library yet: run Update Library") && !r.head[2][0]);
    CHECK(!rename(ROOT "/mnt/data/away.db", ROOT "/mnt/data/database.db"));
    double t = now_s();
    ask(ROOT, &r, "mix\tmost");
    printf("app: Most Played crafted in %.2f s, the library read included\n", now_s() - t);
    CHECK(!strcmp(r.head[2], "refresh") && !strcmp(r.f[0][2], "Play Mixtape") &&
          r.rows >= MX_MIN + 3);
    CHECK(find(&r, "Re-roll") == r.rows - 2 && find(&r, "Save as Playlist") == r.rows - 1);
    char gen[16], first[128], ctx[32];
    snprintf(gen, sizeof gen, "%s", r.f[0][4]);
    snprintf(first, sizeof first, "%s", r.f[1][2]);
    snprintf(ctx, sizeof ctx, "%s", r.f[1][4]);
    CHECK(strstr(r.f[1][3], "Artist ") && strstr(r.f[1][3], ":"));
    ask(ROOT, &r, "mix\tmost"); /* the refresh: the same mix */
    CHECK(!strcmp(r.f[1][2], first) && !strcmp(r.f[0][4], gen));
    snprintf(c, sizeof c, "strike\t%s", ctx);
    ask(ROOT, &r, c);
    CHECK(!strcmp(r.head[1], "ok") && !r.head[2][0]);
    ask(ROOT, &r, "mix\tmost");
    CHECK(!strcmp(r.f[1][3], "Struck \xc2\xb7 Re-roll replaces it") &&
          !strcmp(r.f[r.rows - 2][3], "New songs for the struck ones"));
    char kept_titles[MX_MAX][64];
    int nkept = 0;
    for (int i = 2; i < r.rows - 2; ++i) snprintf(kept_titles[nkept++], 64, "%s", r.f[i][2]);
    /* a heart in stock's database since: the library read again, the mix kept */
    sqlite3 *db;
    CHECK(!sqlite3_open_v2(ROOT "/mnt/data/database.db", &db, 2, 0) &&
          !sqlite3_exec(db, "insert into myfav(id,fileurl,time) values(99999,'/x',0)", 0, 0, 0));
    sqlite3_close(db);
    struct timeval later[2] = { { time(0) + 5, 0 }, { time(0) + 5, 0 } };
    CHECK(!utimes(ROOT "/mnt/data/database.db", later));
    snprintf(c, sizeof c, "reroll\t%s", gen);
    ask(ROOT, &r, c);
    ask(ROOT, &r, "mix\tmost");
    CHECK(find(&r, first) < 0 && strcmp(r.f[0][4], gen));
    for (int i = 0; i < nkept; ++i) CHECK(find(&r, kept_titles[i]) > 0);
    /* one of its songs gone from the library since (its id another): a new mix, every song there */
    int moved;
    CHECK(sscanf(r.f[2][2], "Song %d", &moved) == 1);
    snprintf(c, sizeof c, "update songtable set id=%d where id=%d", 200000 + moved, moved + 1);
    CHECK(!sqlite3_open_v2(ROOT "/mnt/data/database.db", &db, 2, 0) &&
          !sqlite3_exec(db, c, 0, 0, 0));
    sqlite3_close(db);
    later[0].tv_sec = later[1].tv_sec = time(0) + 10;
    CHECK(!utimes(ROOT "/mnt/data/database.db", later));
    char g2[16];
    snprintf(g2, sizeof g2, "%s", r.f[0][4]);
    snprintf(c, sizeof c, "strike\t%s 1", g2);
    ask(ROOT, &r, c);
    snprintf(c, sizeof c, "reroll\t%s", g2);
    ask(ROOT, &r, c);
    ask(ROOT, &r, "mix\tmost");
    CHECK(find(&r, "Play Mixtape") == 0 && strcmp(r.f[0][4], g2) && find(&r, "Song gone") < 0);
    snprintf(c, sizeof c, "update songtable set id=%d where id=%d", moved + 1, 200000 + moved);
    CHECK(!sqlite3_open_v2(ROOT "/mnt/data/database.db", &db, 2, 0) &&
          !sqlite3_exec(db, c, 0, 0, 0));
    sqlite3_close(db);
    later[0].tv_sec = later[1].tv_sec = time(0) + 15;
    CHECK(!utimes(ROOT "/mnt/data/database.db", later));
    snprintf(c, sizeof c, "play\t%s", gen); /* the old mix's */
    ask(ROOT, &r, c);
    CHECK(!strcmp(r.head[2], "This Mixtape has changed"));
    ask(ROOT, &r, "mix\tmost");
    snprintf(gen, sizeof gen, "%s", r.f[0][4]);
    int songs = r.rows - 3, gone;
    char gone_path[256], gone_full[320];
    CHECK(sscanf(r.f[1][2], "Song %d", &gone) == 1); /* its file deleted since: never queued */
    song_path(gone_path, sizeof gone_path, gone);
    snprintf(gone_full, sizeof gone_full, ROOT "%s", gone_path);
    CHECK(!unlink(gone_full));
    snprintf(c, sizeof c, "play\t%s", gen);
    ask(ROOT, &r, c);
    CHECK(!strcmp(r.head[1], "ok") && !r.head[2][0]);
    char *play = slurp(ROOT "/tmp/q2-mixtape.play", 0), *m3u;
    size_t len;
    CHECK(play && strstr(play, " /mnt/mmc/.mixtape/play.m3u\n"));
    CHECK((m3u = slurp(ROOT "/mnt/mmc/.mixtape/play.m3u", &len)) &&
          !memcmp(m3u, "\xef\xbb\xbf/mnt/mmc/Music/", 17));
    int lines = 0;
    for (char *s = m3u; *s; ++s) lines += *s == '\n';
    CHECK(lines == songs - 1 && !strstr(m3u, gone_path));
    free(play);
    free(m3u);
    char kept[32];
    snprintf(kept, sizeof kept, "%d songs \xc2\xb7 ", songs - 1);
    ask(ROOT, &r, "mix\tmost");
    CHECK(!strcmp(r.f[1][3], "Not on the card") && strstr(r.f[0][3], kept));
    int fd = open(gone_full, O_CREAT | O_WRONLY, 0644);
    CHECK(fd >= 0 && !close(fd));
    snprintf(c, sizeof c, "save\t%s", gen);
    ask(ROOT, &r, c);
    CHECK(strstr(r.head[2], "Most Played Mixtape 20") && strstr(r.head[2], "joins Playlists"));
    char *lib = slurp(ROOT "/tmp/q2-mixtape.library", 0),
         *add = lib ? strstr(lib, "\nadd /mnt/mmc/_explaylist_data/Most Played Mixtape ") : 0;
    CHECK(add && strstr(add, ".m3u\n"));
    snprintf(path, sizeof path, ROOT "%.*s", (int)(strstr(add, ".m3u\n") + 4 - add - 5), add + 5);
    CHECK(!access(path, 0));
    ask(ROOT, &r, c); /* the same name again: a number after it */
    CHECK(strstr(r.head[2], " 2\xe2\x80\x9d"));
    free(lib);
    /* Q2 Pod hasn't taken the first (something plays): the second carries its add */
    char seq[32], done[64];
    int adds = 0;
    lib = slurp(ROOT "/tmp/q2-mixtape.library", 0);
    for (char *a = lib; (a = strstr(a, "\nadd ")); ++a) ++adds;
    CHECK(adds == 2 && sscanf(lib, "%31s", seq) == 1);
    free(lib);
    snprintf(done, sizeof done, "%s ok\n", seq);
    put(ROOT "/tmp/q2-mixtape.library.done", done, strlen(done));
    ask(ROOT, &r, c); /* answered: the third carries none */
    lib = slurp(ROOT "/tmp/q2-mixtape.library", 0), adds = 0;
    for (char *a = lib; (a = strstr(a, "\nadd ")); ++a) ++adds;
    CHECK(adds == 1 && strstr(lib, " 3.m3u\n"));
    free(lib);

    ask(ROOT, &r, "albums");
    CHECK(!strcmp(r.f[0][0], "search") && r.rows == 50 && !strcmp(r.head[2], "pages") &&
          !strcmp(r.head[3], "49"));
    CHECK(!strcmp(r.f[1][2], "Album 0") && !strcmp(r.f[1][3], "Artist 0") &&
          !strncmp(r.f[1][4], "album ", 6));
    ask(ROOT, &r, "albums\t4990");
    CHECK(r.rows == 10 && !r.head[3][0] && strcmp(r.f[0][0], "search"));
    ask(ROOT, &r, "artists");
    CHECK(!strcmp(r.f[1][2], "Artist 0") && !strncmp(r.f[1][4], "artist ", 7));
    ask(ROOT, &r, "search\tartist 1499");
    CHECK(r.rows >= 4 && !strcmp(r.f[0][2], "Artist 1499") && !strcmp(r.f[0][3], "Artist"));
    snprintf(c, sizeof c, "mix\t%s", r.f[1][4]);
    ask(ROOT, &r, c);
    CHECK(!strncmp(r.f[0][3], "From Album 1499", 15));
    ask(ROOT, &r, "search\tArtist 1"); /* 611 artists match, and their albums: half the page */
    CHECK(r.rows == 50 && !strcmp(r.f[24][3], "Artist") && strcmp(r.f[25][3], "Artist"));
    ask(ROOT, &r, "search\tno such thing");
    CHECK(r.rows == 1 && !strcmp(r.f[0][0], "info"));

    put(ROOT "/tmp/q2-mixtape.playing", "/mnt/mmc/Music/Artist 5/Album 5/03 Song 53.flac\n", 49);
    ask(ROOT, &r, "more");
    CHECK(r.rows == 3 && !strcmp(r.f[0][2], "Like \xe2\x80\x9cSong 53\xe2\x80\x9d") &&
          !strcmp(r.f[1][2], "From Album 5") && !strcmp(r.f[2][2], "From Artist 5"));
    snprintf(c, sizeof c, "mix\t%s", r.f[0][4]);
    ask(ROOT, &r, c);
    CHECK(!strncmp(r.f[0][3], "Like Song 53", 12) && find(&r, "Song 53") < 0);

    ask(ROOT, &r, "lastfm");
    CHECK(!strcmp(r.f[0][0], "field") && !strcmp(r.f[0][3], "Not set") &&
          !strcmp(r.f[1][2], "This Mixtape has no Last.fm API key"));
    put(ROOT "/mnt/mmc/.scrobble.ini",
        "[LISTENBRAINZ]\nTOKEN=x\n[LASTFM]\n API_KEY = 0123abcd \nUSER=x\n", 60);
    ask(ROOT, &r, "fmuser\t  rj ");
    CHECK(!strcmp(r.f[0][3], "rj") && !strcmp(r.f[1][2], "Similar Artists") &&
          !strcmp(r.f[1][4], "lastfm"));
    ask(ROOT, &r, "lbuser\tsomeone");
    CHECK(!strcmp(r.f[0][3], "someone") && !strcmp(r.f[1][4], "listenbrainz"));
    char *acc = slurp(ROOT "/mnt/mmc/.mixtape/accounts", 0);
    CHECK(acc && !strcmp(acc, "lastfm=rj\nlistenbrainz=someone\n"));
    free(acc);
    ask(ROOT, &r, "mix\tlistenbrainz");
    CHECK(r.rows == 1 && !strcmp(r.f[0][2], "Asking ListenBrainz\xe2\x80\xa6") &&
          !strcmp(r.head[2], "refresh"));
    for (int i = 0; i < 100 && find(&r, "Play Mixtape") < 0; ++i)
        usleep(50000), ask(ROOT, &r, "mix\tlistenbrainz");
    CHECK(r.rows == 5 && !strcmp(r.f[1][2], "Song 778") && !strcmp(r.f[4][2], "Nowhere") &&
          !strcmp(r.f[4][3], "Nobody \xc2\xb7 To find"));
    ask(ROOT, &r, "mix\tlastfm");
    for (int i = 0; i < 100 && r.rows == 1; ++i) usleep(50000), ask(ROOT, &r, "mix\tlastfm");
    CHECK(find(&r, "Play Mixtape") == 0 && r.rows >= 5 + 1 &&
          !strcmp(r.f[r.rows - 1][2], "Nobody Here"));
    for (int i = 1; i < r.rows - 3; ++i)
        CHECK(strstr(r.f[i][3], "Artist 302") || strstr(r.f[i][3], "Artist 303"));

    /* Mix Length: kept on the card, and the next mix made to it */
    ask(ROOT, &r, "lengths");
    CHECK(r.rows == 7 && !strcmp(r.f[5][2], "45 minutes") && !strcmp(r.f[5][3], "Chosen") &&
          !r.f[3][3][0] && !r.head[2][0]);
    ask(ROOT, &r, "length\t20");
    CHECK(!strcmp(r.head[1], "ok") && !r.head[2][0]);
    ask(ROOT, &r, "lengths");
    CHECK(!strcmp(r.f[3][3], "Chosen") && !r.f[5][3][0]);
    char *lf = slurp(ROOT "/mnt/mmc/.mixtape/length", 0);
    CHECK(lf && !strcmp(lf, "20\n"));
    free(lf);
    ask(ROOT, &r, "length\t7"); /* not a length offered */
    ask(ROOT, &r, "mix\tshuffle");
    int min = 0;
    CHECK(sscanf(strstr(r.f[0][3], "\xc2\xb7 ") + 3, "%d min", &min) == 1 && min >= 17 &&
          min <= 24);
    char was[MX_MAX][64]; /* Re-roll, nothing struck: a new shuffle */
    int nwas = 0, again = 0;
    for (int i = 1; i < r.rows - 2; ++i) snprintf(was[nwas++], 64, "%s", r.f[i][2]);
    snprintf(c, sizeof c, "reroll\t%s", r.f[0][4]);
    ask(ROOT, &r, c);
    ask(ROOT, &r, "mix\tshuffle");
    for (int i = 0; i < nwas; ++i) again += find(&r, was[i]) > 0;
    CHECK(find(&r, "Play Mixtape") == 0 && again <= 1);

    ask(ROOT, &r, "scan");
    CHECK(!strcmp(r.f[0][2], "Songs heard") && !strcmp(r.f[0][3], "40,000 of 50,000") &&
          find(&r, "Listen Now") >= 0 && find(&r, "Heard songs make the sound mixes") == 1);
    app_end(pid);
    CHECK(access(ROOT "/tmp/q2-mixtape.play", 0)); /* stopped: never played again as new */

    /* older firmware: no words, no playing or saving through it */
    pid = app_start(ROOT, "");
    ask(ROOT, &r, "lengths");
    CHECK(!strcmp(r.f[3][3], "Chosen")); /* read back from the card */
    ask(ROOT, &r, "mix\tnever");
    snprintf(c, sizeof c, "play\t%s", r.f[0][4]);
    ask(ROOT, &r, c);
    CHECK(!strcmp(r.head[2], "Update Q2 Pod to play a Mixtape"));
    ask(ROOT, &r, "mix\tnever");
    snprintf(c, sizeof c, "save\t%s", r.f[0][4]);
    ask(ROOT, &r, c);
    CHECK(strstr(r.head[2], "Saved: Import Playlists adds"));
    ask(ROOT, &r, "lastfm");
    CHECK(!strcmp(r.f[0][0], "info"));
    app_end(pid);
    free(r.buf);
}

/* Listening for real: three songs ffmpeg makes, heard by the app's own scan child, then nothing
 * to hear, then a changed one heard again. */
static void check_scan(void) {
    reply_t r = { 0 };
    sh("rm -rf " SMALL " && mkdir -p " SMALL "/mnt/data " SMALL "/mnt/mmc/Music " SMALL "/tmp");
    static const char *const songs[] = { "sine=frequency=220:duration=40",
                                         "sine=frequency=880:duration=20",
                                         "anoisesrc=d=50:c=pink:a=0.3" };
    char cmd[512];
    for (int i = 0; i < 3; ++i) {
        snprintf(cmd, sizeof cmd,
                 "ffmpeg -v quiet -f lavfi -i %s -ac 2 -ar 44100 -y " SMALL
                 "/mnt/mmc/Music/%d.flac",
                 songs[i], i);
        sh(cmd);
    }
    sqlite3 *db;
    CHECK(!sqlite3_open_v2(SMALL "/mnt/data/database.db", &db, 6, 0));
    CHECK(!sqlite3_exec(db,
                        "create table songtable(id INTEGER PRIMARY KEY, filename, fileurl, album, "
                        "songer, style, name, composer,"
                        "albumsonger, year, track, cd_num, time, time_end, time_create);"
                        "insert into songtable(id,fileurl,album,songer,name,time,time_end) values"
                        "(1,'/mnt/mmc/Music/0.flac','A','X','Low',0,0),(2,'/mnt/mmc/Music/"
                        "1.flac','A','X','High',0,0),"
                        "(3,'/mnt/mmc/Music/2.flac','B','Y','Noise',0,0),(4,'/mnt/mmc/Music/"
                        "gone.flac','B','Y','Gone',0,0),"
                        "(5,'/mnt/mmc/Music/1.flac','A','X','A CUE track',10,20);",
                        0, 0, 0));
    sqlite3_close(db);
    dur_t d[1] = { { path_key("/mnt/mmc/Music/0.flac"), 0, 0, 0, 35 } }; /* measured: not asked */
    put_durations(SMALL "/mnt/data/ringnav-durations-v1", d, 1);
    for (int round = 0; round < 3; ++round) {
        pid_t pid = app_start(SMALL, "progress\n");
        ask(SMALL, &r, "scanstart");
        double t = now_s();
        do {
            usleep(100000);
            ask(SMALL, &r, "scan");
            CHECK(now_s() - t < 60);
        } while (find(&r, "Songs heard") < 0);
        char *state = slurp(SMALL "/tmp/q2-mixtape.state", 0);
        CHECK(state && !strcmp(state, "hold=0\n"));
        free(state);
        CHECK(!strcmp(r.f[0][3], "3 of 4"));
        char *s = slurp(SMALL "/tmp/q2-mixtape.scan", 0);
        printf("scan %d: %s", round, s);
        CHECK(!strcmp(s, round == 0 ? "end 3 3 0\n" : "end 1 1 0\n") ||
              (round == 1 && !strcmp(s, "end 0 0 0\n")));
        free(s);
        profs_t p;
        setenv("MIXTAPE_ROOT", SMALL, 1);
        CHECK(profs_open(&p) == 3);
        const prof_t *a = profs_find(&p, prof_key("/mnt/mmc/Music/0.flac")),
                     *b = profs_find(&p, prof_key("/mnt/mmc/Music/2.flac"));
        CHECK(a && a->status == MX_OK && a->sec == 35 && b && b->status == MX_OK && b->sec == 50);
        profs_close(&p);
        app_end(pid);
        if (round == 1) /* a changed file: heard again */
            sh("ffmpeg -v quiet -f lavfi -i sine=frequency=330:duration=40 -ac 1 -y " SMALL
               "/mnt/mmc/Music/0.flac");
    }
    /* A song that never ends (a FIFO nobody writes): Listen Now's reply still closes at once (ask
     * checks), the app stays while it listens, Stop Listening kills its ffmpeg and keeps what was
     * heard, and the app then ends by itself, nothing asked */
    sh("rm -f " SMALL "/mnt/mmc/.mixtape/profiles && mkfifo " SMALL "/mnt/mmc/Music/stuck.flac");
    CHECK(!sqlite3_open_v2(SMALL "/mnt/data/database.db", &db, 2, 0));
    CHECK(!sqlite3_exec(db,
                        "insert into songtable(id,fileurl,name,time,time_end) values"
                        "(6,'/mnt/mmc/Music/stuck.flac','Stuck',0,0)",
                        0, 0, 0));
    sqlite3_close(db);
    setenv("MIXTAPE_IDLE_S", "1", 1);
    pid_t pid = app_start(SMALL, "");
    unsetenv("MIXTAPE_IDLE_S");
    double t = now_s();
    ask(SMALL, &r, "scanstart");
    CHECK(now_s() - t < 2);
    char *at = 0;
    for (; (!at || strncmp(at, "listen 3 4 ", 11)) && now_s() - t < 30; usleep(100000))
        free(at), at = slurp(SMALL "/tmp/q2-mixtape.scan", 0);
    CHECK(at && !strncmp(at, "listen 3 4 ", 11));
    free(at);
    sleep(3);
    CHECK(!waitpid(pid, 0, WNOHANG)); /* listening: not idle */
    ask(SMALL, &r, "scanstop");
    int status = -1;
    for (t = now_s(); waitpid(pid, &status, WNOHANG) != pid && now_s() - t < 10;) usleep(100000);
    CHECK(WIFEXITED(status) && !WEXITSTATUS(status));
    at = slurp(SMALL "/tmp/q2-mixtape.scan", 0);
    CHECK(at && !strcmp(at, "end 3 4 4\n"));
    free(at);
    profs_t p;
    CHECK(profs_open(&p) == 3);
    profs_close(&p);

    /* the same song past ffmpeg's time: left for next time, the rest of the scan going on */
    sh("ffmpeg -v quiet -f lavfi -i sine=frequency=440:duration=25 -ac 2 -ar 44100 -y " SMALL
       "/mnt/mmc/Music/1.flac"); /* changed: to hear again, before the stuck one */
    setenv("MIXTAPE_FFMPEG_MS", "3000", 1);
    pid = app_start(SMALL, "");
    unsetenv("MIXTAPE_FFMPEG_MS");
    ask(SMALL, &r, "scanstart");
    for (t = now_s(); find(&r, "Songs heard") < 0 && now_s() - t < 20; usleep(100000))
        ask(SMALL, &r, "scan");
    at = slurp(SMALL "/tmp/q2-mixtape.scan", 0);
    CHECK(at && !strcmp(at, "end 1 2 0\n") && profs_open(&p) == 3);
    free(at);
    profs_close(&p);
    app_end(pid);

    /* a newer Mixtape's file: kept as it is, and the page says why nothing was heard */
    size_t n;
    char *was = slurp(SMALL PROF_FILE, &n), *now;
    unsigned v = PROF_VERSION + 1;
    memcpy(was + 4, &v, 4);
    put(SMALL PROF_FILE, was, n);
    pid = app_start(SMALL, "");
    ask(SMALL, &r, "scanstart");
    for (t = now_s(); find(&r, "Songs heard") < 0 && now_s() - t < 10; usleep(100000))
        ask(SMALL, &r, "scan");
    CHECK(find(&r, "Stopped: update Mixtape first") >= 0);
    CHECK((now = slurp(SMALL PROF_FILE, 0)) && !memcmp(now, was, n));
    app_end(pid);
    free(was);
    free(now);
    free(r.buf);
}

int main(int argc, char **argv) {
    if (argc == 3 && !strcmp(argv[1], "analyse")) {
        prof_t p = { 0 };
        short *pcm = malloc(MX_CLIP_S * MX_RATE * sizeof *pcm);
        CHECK(!analyse(argv[2], 0, &p, pcm));
        printf("%d %d", p.sec, p.status);
        for (int k = 0; k < MX_DIMS; ++k) printf(" %.9g", p.f[k]);
        printf("\n");
        free(pcm);
        return 0;
    }
    if (argc == 3 && !strcmp(argv[1], "features")) {
        size_t n;
        char *raw = slurp(argv[2], &n);
        float f[MX_DIMS];
        int ok = raw && mx_features((short *)raw, (int)(n / 2), f);
        printf("%d", ok);
        for (int k = 0; ok && k < MX_DIMS; ++k) printf(" %.9g", f[k]);
        printf("\n");
        free(raw);
        return 0;
    }
    if (argc == 3 && !strcmp(argv[1], "key")) {
        printf("%llu\n", prof_key(argv[2]));
        return 0;
    }
    if (argc == 4 && !strcmp(argv[1], "lookup")) {
        char path[1024];
        profs_t p;
        setenv("MIXTAPE_ROOT", argv[2], 1);
        snprintf(path, sizeof path, "/mnt/mmc/%s", argv[3]);
        int n = profs_open(&p);
        const prof_t *f = profs_find(&p, prof_key(path));
        printf("%d %d %d %u", n, f ? f->sec : -1, f ? f->status : -1, f ? f->size : 0);
        for (int k = 0; f && k < MX_DIMS; ++k) printf(" %.9g", f->f[k]);
        printf("\n");
        profs_close(&p);
        return 0;
    }
    setvbuf(stdout, 0, _IONBF, 0);
    check_json();
    check_features();
    check_choosing();
    check_library();
    web_start();
    check_net();
    check_app();
    check_scan();
    printf("check: all passed\n");
    return 0;
}
