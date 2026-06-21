// song.h — Song 抽象，player.c 和 netease.c 共享

#ifndef SONG_H
#define SONG_H

typedef enum { SRC_LOCAL, SRC_NETEASE } SongSource;

typedef struct {
    SongSource source;
    char id[512];
    char title[256];
    char artist[128];
    char album[128];
    int  duration_sec;
    char aux_label[64];  // 来源目录名 / 歌单名
} Song;

#define MAX_SONGS 1024

#endif // SONG_H
