/* Mixtape, a Q2 Pod card app (README.md): mixes of the music on the card, made from what the user
 * plays and hearts, from how each song sounds (its MFCCs, heard by the scan or the desktop
 * indexer) and from Last.fm's and ListenBrainz's recommendations. */
#ifndef MIXTAPE_H
#define MIXTAPE_H
#include <stddef.h>

/* The sound profile: 30 s from a song's middle at 22.05 kHz mono, 13 MFCCs' mean and spread.
 * desktop/mixtape_index.py has the same constants and must change with them. */
#define MX_RATE 22050
#define MX_CLIP_S 30
#define MX_N 1024 /* FFT frame, hop half of it */
#define MX_MELS 26
#define MX_CEPS 13
#define MX_DIMS (2 * MX_CEPS)

#define MX_MAX 20        /* songs in a mix */
#define MX_MIN 6         /* fewer and there is no mix (Last.fm's and ListenBrainz's take any) */
#define MX_TARGET_S 2700 /* about 45 minutes */
#define MX_GUESS_S 240   /* a song of unknown length */
#define MX_SHORT_S 60    /* intros and interludes stay out */
#define MX_TOP 8         /* a slot draws from this many best */
#define MX_SHORT(s) ((s)->sec > 0 && (s)->sec < MX_SHORT_S) /* sec <= 0: unknown */

/* The profiles file, CARD/.mixtape/profiles: a header, then records appended as songs are heard,
 * by the Q2's scan or the desktop indexer; a song's last record is its profile. A record is keyed
 * by the song's path on the card (less "/mnt/mmc/") and its size, never its time: FAT keeps local
 * time, so a computer and the Q2 can read one file's time hours apart. */
#define PROF_MAGIC "MXT2"
#define PROF_VERSION 1 /* bump when the features change: every song is heard again */
enum { MX_OK = 1, MX_FAILED };
typedef struct {
    unsigned long long key; /* prof_key */
    unsigned size;          /* the file's size when heard */
    unsigned short sec;     /* its length, 0 unknown */
    unsigned char status, pad;
    float f[MX_DIMS];
} prof_t; /* 120 bytes, little-endian, as mixtape_index.py's RECORD */
typedef struct {
    char magic[4];
    unsigned version, record, zero;
} prof_head;
typedef struct {
    void *map;
    size_t len;
    const prof_t *tab;
    unsigned *idx; /* tab's latest record of each key, by key */
    int n;
} profs_t;

/* A library song, as compact as a 50,000-song library on the Q2 needs: hashes, not strings. */
enum { MX_USED = 1, MX_REJECTED = 2, MX_ASIDE = 4 };
typedef struct {
    unsigned long long pkey;                               /* its profile's key */
    unsigned id, lkey;                                     /* songtable's id; Q2 Pod's listen key */
    unsigned artist, album_artist, album, title, genre[4]; /* hashes (norm_hash, album_ident) */
    const prof_t *prof;                                    /* heard, or 0 */
    short year, sec, recency; /* recency: its slot in Q2 Pod's play counts, 0 newest; -1 none */
    unsigned short plays;
    unsigned char hearts, album_hearts, seed, flags;
    float score;
} song_t;

/* An album or an artist: a hash and its names, offsets into lib.pool. */
typedef struct {
    unsigned hash, name, artist;
} group_t;

typedef struct {
    song_t *v; /* by id */
    int n, gen;
    group_t *albums, *artists; /* by name */
    int nalbums, nartists;
    char *pool;
    unsigned used, cap;
    long long stamp; /* the database's size and time when read */
    profs_t profs;
} lib_t;
extern lib_t lib;

/* A recommendation from Last.fm (an artist) or ListenBrainz (a recording). */
typedef struct {
    unsigned title, artist[4]; /* norm_hash: 0 none */
    float score;
    unsigned char top, matched; /* top: one of the user's own top artists */
    char name[96], sub[96];
} item_t;
typedef struct {
    item_t *v;
    int n;
    char said[120]; /* why there are none, or "" */
} net_t;

enum { MIX_MOST, MIX_FORGOTTEN, MIX_NEVER, MIX_DEEP, MIX_SOUND, MIX_LASTFM, MIX_LB };

/* Paths: under MIXTAPE_ROOT on a computer (the checks), as they are on the Q2 otherwise. */
const char *R(const char *path);
#define CARD "/mnt/mmc"
#define APP_DIR CARD "/.mixtape"
#define PROF_FILE APP_DIR "/profiles"
#define DB_FILE "/mnt/data/database.db"
/* The Library's music, as Q2 Pod's MUSIC_WHERE: no podcasts, audiobooks or .m4b books. And no
 * CUE image's tracks (a start or an end set): a playlist's line names a file, so the image's first
 * track would play instead. */
#define MUSIC_SQL                                                                                  \
    "fileurl not like '/mnt/mmc/audiobooks/%' and fileurl not like '/mnt/mmc/podcasts/%' and "     \
    "fileurl not like '%.m4b' and ifnull(time, 0) <= 0 and ifnull(time_end, 0) <= 0"

/* lib.c */
unsigned fnv32(unsigned h, const char *s);
unsigned long long prof_key(const char *path);
unsigned listen_key(const char *path, int cue);
unsigned album_ident(const char *album, const char *album_artist, const char *path);
unsigned norm_hash(const char *s, int title);
unsigned lower_hash(const char *s, unsigned n);
void genres(const char *s, unsigned *g);
int profs_open(profs_t *p);
void profs_close(profs_t *p);
const prof_t *profs_find(const profs_t *p, unsigned long long key);
void *db_open(void);
void db_close(void *db);
int db_page(void *db, const char *sql, unsigned *last, int (*row)(void *, void *), void *ctx);
void scope_load(void);
int music(const char *path);
int db_int(void *db, const char *sql);
unsigned song_id(void *db, const char *path);
const char *col_s(void *st, int i);
int col_i(void *st, int i);
int lib_load(int fresh);
void lib_free(void);
int lib_find(unsigned id);
const char *lib_str(unsigned off);
int song_text(void *db, unsigned id, char *path, unsigned np, char *title, unsigned nt,
              char *artist, unsigned na);
long long file_stamp(const char *path);
void dur_open(void);
void dur_close(void);
int dur_sec(const char *path);

/* mix.c */
unsigned rnd(unsigned n);
int mx_features(const short *pcm, int n, float *out);
void mx_rank(song_t *v, int n);
void mx_counts(song_t *v, int n, int kind);
void mx_items(song_t *v, int n, item_t *items, int ni, int kind);
int mx_craft(song_t *v, int n, int *list, int seeded);
int mx_reroll(song_t *v, int n, int *list, int len, unsigned char *struck);

/* scan.c */
int scan_main(void);
int analyse(const char *path, int len, prof_t *p, short *pcm);

/* net.c */
typedef void (*json_cb)(void *ctx, int depth, char **keys, const char *value);
int json_walk(char *js, json_cb cb, void *ctx);
const char *lastfm_key(void);
void net_lastfm(const char *user, const char *key, net_t *out);
void net_listenbrainz(const char *user, net_t *out);

/* app.c */
int serve(void);

#endif
