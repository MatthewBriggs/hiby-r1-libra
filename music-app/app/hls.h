#ifndef HLS_H
#define HLS_H

#define HLS_URL_MAX  1024
#define HLS_QUEUE    8

typedef struct {
    char media_url[HLS_URL_MAX];          /* the chosen variant playlist */
    char queue[HLS_QUEUE][HLS_URL_MAX];   /* segments not yet played */
    int  n_queue;
    long long last_seq;                   /* media sequence of the last taken */
    int  target_ms;                       /* segment length the server declares */
    double last_refresh_mono;             /* CLOCK_MONOTONIC seconds, see hls_next()'s own comment */
} hls_t;

int hls_open(hls_t *h, const char *master_url);
/* Fetch the next segment. Returns its length, or 0 if the playlist has
 * nothing new yet (the caller should wait and ask again). */
int hls_next(hls_t *h, unsigned char *buf, int max);
#endif
