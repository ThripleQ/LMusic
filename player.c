// ================================================================
// simple-player - 终端本地播放器（修复版）
// ================================================================

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <pthread.h>
#include <strings.h>
#include <unistd.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <alsa/asoundlib.h>
#include <ncurses.h>
#include <signal.h>
#include <locale.h>

#include "decoder.h"

//#define DEBUG
#ifdef DEBUG
#define DBG(fmt, ...) fprintf(stderr, "[DBG] " fmt "\n", ##__VA_ARGS__)
#else
#define DBG(fmt, ...) ((void)0)
#endif

// ── 播放器状态 ──────────────────────────────────────────────
typedef enum { STOPPED, PLAYING, PAUSED } PlayState;

typedef struct {
 atomic_int command;
 atomic_llong seek_frame;

 atomic_int state;
 atomic_llong cur_frame;
 atomic_llong playback_frame;
 atomic_llong total_frames;
 atomic_llong frame_offset;
 atomic_llong total_duration_frames;

 char cur_song[256];
 atomic_int sample_rate;
 atomic_int num_channels;
 atomic_int bits_per_sample;

 char pending_path[512];
 atomic_bool quit;
} PlayerState;

static PlayerState g_state = { .seek_frame = -1 };
static uint8_t *g_audio_buf = NULL;
static size_t g_audio_cap = 0;
static int g_block_align = 4;

static pthread_mutex_t g_data_mutex = PTHREAD_MUTEX_INITIALIZER;

static StreamDecoder *g_stream_dec = NULL;
static bool g_decode_done = false;
static long long g_stream_local_total = 0;
static int tail_stall = 0;  // ALSA tail-drain timeout counter

// 静默 ALSA 运行时错误
static void noop_alsa_err(const char *f, int l, const char *fn, int e, const char *fmt, ...) {}


// ── Song 抽象 ─────────────────────────────────────────────
typedef enum { SRC_LOCAL, SRC_NETEASE } SongSource;

typedef struct {
 SongSource source;
 char id[512];
 char title[256];
 char artist[128];
 char album[128];
 int duration_sec;
 char aux_label[64];  // 来源目录名
} Song;

#define MAX_SONGS 1024
static Song playlist[MAX_SONGS];
static atomic_int song_count = 0;
static int dir_counts[64];
static int active_panel = 0;
static int quitting = 0;  // 退出确认标志
static volatile sig_atomic_t sigint_caught = 0;

static void handle_sigint(int sig) { sigint_caught = 1; }
static int song_sel = 0;  // 右面板选中的歌曲在过滤列表中的序号  // 每个目录的歌曲数
static atomic_int play_index = -1;
static atomic_int loop_mode = 0;

// ── 目录浏览 ──
static char dirs[64][512];
static int dir_count = 0;

// ── 扫描本地音频文件 ──────────────────────────────────────
static const char *audio_exts[] = {
 ".wav",".mp3",".flac",".ogg",".aac",".m4a",".wma",".opus",NULL
};

static int has_audio_ext(const char *name) {
 size_t len = strlen(name);
 for (int i = 0; audio_exts[i]; i++) {
 size_t ext_len = strlen(audio_exts[i]);
 if (len > ext_len && strcasecmp(name + len - ext_len, audio_exts[i]) == 0)
 return 1;
 }
 return 0;
}

static void title_from_path(const char *path, char *title, int max_len) {
 const char *base = strrchr(path, '/');
 base = base ? base + 1 : path;
 const char *dot = strrchr(base, '.');
 if (dot) {
 int len = (int)(dot - base);
 if (len >= max_len) len = max_len - 1;
 strncpy(title, base, len);
 title[len] = '\0';
 } else {
 strncpy(title, base, max_len - 1);
 title[max_len - 1] = '\0';
 }
}

static int scan_songs_at(const char *dir, int start) {
 DIR *d = opendir(dir);
 if (!d) return 0;
 struct dirent *ent;
 int count = start;
 char full_path[512];
 while ((ent = readdir(d)) && count < MAX_SONGS) {
 if (has_audio_ext(ent->d_name)) {
 snprintf(full_path, sizeof(full_path), "%s/%s", dir, ent->d_name);
 playlist[count].source = SRC_LOCAL;
 strncpy(playlist[count].id, full_path, sizeof(playlist[count].id)-1);
 // 读标签元数据
 char tag_title[256] = "", tag_artist[128] = "", tag_album[128] = "";
 int tag_dur = 0;
 if (read_audio_tags(full_path, tag_title, sizeof(tag_title),
 tag_artist, sizeof(tag_artist),
 tag_album, sizeof(tag_album), &tag_dur) == 0 && tag_title[0]) {
 strncpy(playlist[count].title, tag_title, sizeof(playlist[count].title)-1);
 strncpy(playlist[count].artist, tag_artist, sizeof(playlist[count].artist)-1);
 strncpy(playlist[count].album, tag_album, sizeof(playlist[count].album)-1);
 // 来源目录名
 const char *pdir = strrchr(dir, '/');
 pdir = pdir ? pdir + 1 : dir;
 strncpy(playlist[count].aux_label, pdir, sizeof(playlist[count].aux_label)-1);
 playlist[count].duration_sec = tag_dur;
 } else {
 // 无标签则用文件名
 title_from_path(full_path, playlist[count].title, sizeof(playlist[count].title));
 playlist[count].artist[0] = '\0';
 playlist[count].album[0] = '\0';
 // 来源目录名
 const char *pdir = strrchr(dir, '/');
 pdir = pdir ? pdir + 1 : dir;
 strncpy(playlist[count].aux_label, pdir, sizeof(playlist[count].aux_label)-1);
 playlist[count].duration_sec = 0;
 }
 count++;
 }
 }
 closedir(d);
 return count - start;
}

// ── 流式打开文件（支持从指定帧开始播放）───────────────────
static int start_stream_at(const char *path, long long start_frame) {
 AudioInfo info;

 if (g_stream_dec) { stream_close(g_stream_dec); free(g_stream_dec); g_stream_dec = NULL; }
 if (g_audio_buf) { free(g_audio_buf); g_audio_buf = NULL; g_audio_cap = 0; }
 g_decode_done = false;

 StreamDecoder *dec = malloc(sizeof(StreamDecoder));
 if (!dec) return -1;

 if (stream_open(dec, path, &info) != 0) { free(dec); return -1; }
 g_block_align = info.block_align;

 if (start_frame > 0) {
 if (stream_seek(dec, start_frame, info.sample_rate) != 0) {
 stream_close(dec); free(dec);
 return -1;
 }
 }

 long long total_fr = 0;
 int ret = stream_decode(dec, &g_audio_buf, &g_audio_cap, &total_fr);
 if (ret <= 0) {
 stream_close(dec); free(dec);
 return -1;
 }

 g_stream_dec = dec;
 g_stream_local_total = total_fr;

 pthread_mutex_lock(&g_data_mutex);
 strncpy(g_state.cur_song, path, sizeof(g_state.cur_song)-1);
 g_state.cur_song[sizeof(g_state.cur_song)-1] = '\0';
 pthread_mutex_unlock(&g_data_mutex);

 atomic_store(&g_state.total_duration_frames, info.total_samples > 0
 ? (long long)info.total_samples : 0LL);
 atomic_store(&g_state.frame_offset, start_frame);
 atomic_store(&g_state.total_frames, start_frame + total_fr);
 atomic_store(&g_state.cur_frame, 0LL);
 atomic_store(&g_state.playback_frame, 0LL);
 atomic_store(&g_state.sample_rate, (int)info.sample_rate);
 atomic_store(&g_state.num_channels, (int)info.num_channels);
 atomic_store(&g_state.bits_per_sample, (int)info.bits_per_sample);

 return 0;
}

// ── 播放线程 ─────────────────────────────────────────────
static void *playback_thread(void *arg) {
 (void)arg;
 snd_pcm_t *pcm = NULL;

 while (!atomic_load(&g_state.quit)) {
 int cmd = atomic_exchange(&g_state.command, 0);

 // ── 下一首 / 上一首 ──
 if (cmd == 5 || cmd == 6) {
 if (atomic_load(&song_count) == 0) continue;
 int next = (cmd == 5)
 ? (atomic_load(&play_index) + 1) % atomic_load(&song_count)
 : (atomic_load(&play_index) - 1 + atomic_load(&song_count)) % atomic_load(&song_count);
 atomic_store(&play_index, next);
 strncpy(g_state.pending_path, playlist[next].id,
 sizeof(g_state.pending_path)-1);
 atomic_store(&g_state.seek_frame, -1);
 }

 // ── 开始播放 ──
 if (cmd == 1 || cmd == 5 || cmd == 6) {
 tail_stall = 0;
 if (pcm) { snd_pcm_drop(pcm); snd_pcm_close(pcm); pcm = NULL; }

 long long seek_to = atomic_exchange(&g_state.seek_frame, -1);
 if (seek_to < 0) seek_to = 0;

 if (start_stream_at(g_state.pending_path, seek_to) != 0) {
 atomic_store(&g_state.state, STOPPED);
 continue;
 }

 size_t total_fr = (size_t)atomic_load(&g_state.total_frames);
 if (total_fr == 0) { atomic_store(&g_state.state, STOPPED); continue; }

 if (snd_pcm_open(&pcm, "default", SND_PCM_STREAM_PLAYBACK, SND_PCM_NONBLOCK) < 0) {
 pcm = NULL; atomic_store(&g_state.state, STOPPED); continue;
 }

 snd_pcm_hw_params_t *hw;
 snd_pcm_hw_params_alloca(&hw);
 snd_pcm_hw_params_any(pcm, hw);
 snd_pcm_hw_params_set_access(pcm, hw, SND_PCM_ACCESS_RW_INTERLEAVED);
 snd_pcm_hw_params_set_format(pcm, hw, SND_PCM_FORMAT_S16_LE);
 snd_pcm_hw_params_set_channels(pcm, hw, (int)atomic_load(&g_state.num_channels));
 unsigned int rate = (unsigned int)atomic_load(&g_state.sample_rate);
 snd_pcm_hw_params_set_rate_near(pcm, hw, &rate, 0);
 if (snd_pcm_hw_params(pcm, hw) < 0) {
 snd_pcm_close(pcm); pcm = NULL;
 atomic_store(&g_state.state, STOPPED); continue;
 }

 atomic_store(&g_state.state, PLAYING);
 continue;
 }

 // ── 停止 ──
 if (cmd == 4) {
 if (pcm) { snd_pcm_drop(pcm); snd_pcm_close(pcm); pcm = NULL; }
 if (g_stream_dec) { stream_close(g_stream_dec); free(g_stream_dec); g_stream_dec = NULL; }
 g_decode_done = false;
 g_stream_local_total = 0;
 atomic_store(&g_state.state, STOPPED);
 atomic_store(&g_state.cur_frame, 0LL);
 atomic_store(&g_state.playback_frame, 0LL);
 atomic_store(&g_state.frame_offset, 0LL);
 atomic_store(&g_state.total_duration_frames, 0LL);
 continue;
 }

 // ── 播放循环 ──
 if (pcm) {
 int st = atomic_load(&g_state.state);

 // 暂停
 if (cmd == 2 && st == PLAYING) {
 snd_pcm_sframes_t delay = 0;
 if (snd_pcm_delay(pcm, &delay) == 0) {
 long long cur = atomic_load(&g_state.cur_frame);
 long long real = cur - delay;
 if (real >= 0) {
 atomic_store(&g_state.cur_frame, real);
 atomic_store(&g_state.playback_frame, real);
 }
 }
 snd_pcm_drop(pcm);
 atomic_store(&g_state.state, PAUSED);
 continue;
 }

 // 恢复
 if (cmd == 3 && st == PAUSED) {
 snd_pcm_prepare(pcm);
 atomic_store(&g_state.state, PLAYING);
 continue;
 }

 if (st == PLAYING) {
 long long total_fr = atomic_load(&g_state.total_frames);
 long long cf = atomic_load(&g_state.cur_frame);
 long long frame_off = atomic_load(&g_state.frame_offset);
 long long global_cf = frame_off + cf;

 // ── 流式续流（仅在解码未完成时）──
 if (!g_decode_done && g_stream_dec) {
 snd_pcm_sframes_t delay = 0;
 snd_pcm_delay(pcm, &delay);
 long long buffered = (delay > 0) ? delay : 0;
 if (cf + buffered >= g_stream_local_total - 1024) {
 long long new_local = g_stream_local_total;
 size_t cap = g_audio_cap;
 int ret = stream_decode(g_stream_dec, &g_audio_buf, &cap, &new_local);
 if (ret > 0) {
 g_audio_cap = cap;
 g_stream_local_total = new_local;
 total_fr = frame_off + new_local;
 atomic_store(&g_state.total_frames, total_fr);
 } else if (ret == 0) {
 DBG("decode done! total_fr=%lld local=%lld", frame_off + new_local, new_local);
 g_decode_done = true;
 g_stream_local_total = new_local;
 total_fr = frame_off + new_local;
 atomic_store(&g_state.total_frames, total_fr);
 } else {
 snd_pcm_drop(pcm); snd_pcm_close(pcm); pcm = NULL;
 atomic_store(&g_state.state, STOPPED);
 continue;
 }
 }
 }

 // ── 进度刷新（始终执行）──
 snd_pcm_sframes_t delay = 0;
 if (snd_pcm_delay(pcm, &delay) == 0) {
 long long real = cf - delay;
 long long old = atomic_load(&g_state.playback_frame);
 if (real > old)
 atomic_store(&g_state.playback_frame, real);
 }

 // ── ① 解码完成但还有数据没写 → 集中写完 ──
 DBG("branch1: g_decode_done=%d cf=%lld local=%lld", g_decode_done, cf, g_stream_local_total);
 if (g_decode_done && cf < g_stream_local_total) {
 size_t rem = (size_t)(g_stream_local_total - cf);
 size_t chunk = rem > 1024 ? 1024 : rem;
 uint8_t *pos = g_audio_buf + (size_t)cf * g_block_align;
 snd_pcm_sframes_t n = snd_pcm_writei(pcm, pos, chunk);
 if (n == -EAGAIN) {
 usleep(5000);
 } else if (n < 0) {
 n = snd_pcm_recover(pcm, n, 0);
 if (n < 0) {
 snd_pcm_close(pcm); pcm = NULL;
 atomic_store(&g_state.state, STOPPED);
 }
 } else if (n > 0) {
 atomic_fetch_add(&g_state.cur_frame, (long long)n);
 cf += n;
 }
 usleep(5000);
 continue;
 }

 // ── ② 解码完成且数据全写完 → 等待硬件排空 ──
 DBG("branch2: entering tail-wait");
 if (g_decode_done && cf >= g_stream_local_total) {
 snd_pcm_sframes_t rem = 0;
 if (snd_pcm_delay(pcm, &rem) == 0) {
 if (rem == 0 || (++tail_stall > 100 && rem < 2000) || tail_stall > 500) {
 tail_stall = 0;
 DBG("branch2a: delay=%ld → cleanup (stall=%d)", rem, tail_stall);
 // ★ 真正结束：拉满进度，清理，切歌
 long long total_dur = atomic_load(&g_state.total_duration_frames);
 if (total_dur == 0) total_dur = total_fr;
 if (total_dur > frame_off)
 atomic_store(&g_state.playback_frame, total_dur - frame_off);
 else
 atomic_store(&g_state.playback_frame, cf);

 snd_pcm_drop(pcm);
 snd_pcm_close(pcm); pcm = NULL;
 if (g_stream_dec) { stream_close(g_stream_dec); free(g_stream_dec); g_stream_dec = NULL; }
 g_decode_done = false;
 g_stream_local_total = 0;
 atomic_store(&g_state.frame_offset, 0LL);
 atomic_store(&g_state.total_duration_frames, 0LL);

 if (atomic_load(&song_count) > 0 && atomic_load(&loop_mode) > 0) {
 atomic_store(&g_state.state, STOPPED);
 int next = (atomic_load(&loop_mode) == 2)
 ? (atomic_load(&play_index) + 1) % atomic_load(&song_count)
 : atomic_load(&play_index);
 if (next >= 0) {
 atomic_store(&play_index, next);
 strncpy(g_state.pending_path, playlist[next].id,
 sizeof(g_state.pending_path)-1);
 atomic_store(&g_state.seek_frame, -1);
 atomic_store(&g_state.command, 1);
 continue;
 }
 }
 atomic_store(&g_state.state, STOPPED);
 continue;
 } else {
 // 硬件还有数据，更新进度并等待
 DBG("branch2b: delay=%ld waiting (stall=%d)", rem, tail_stall);
 long long real = cf - rem;
 long long old = atomic_load(&g_state.playback_frame);
 if (real > old)
 atomic_store(&g_state.playback_frame, real);
 usleep(5000);
 continue;
 }
 } else {
 DBG("branch2c: delay FAILED, force close");
 // delay 失败，强制结束
 long long total_dur = atomic_load(&g_state.total_duration_frames);
 if (total_dur == 0) total_dur = total_fr;
 atomic_store(&g_state.playback_frame, total_dur - frame_off);
 snd_pcm_drop(pcm);
 snd_pcm_close(pcm); pcm = NULL;
 if (g_stream_dec) { stream_close(g_stream_dec); free(g_stream_dec); g_stream_dec = NULL; }
 g_decode_done = false;
 g_stream_local_total = 0;
 atomic_store(&g_state.frame_offset, 0LL);
 atomic_store(&g_state.total_duration_frames, 0LL);

 if (atomic_load(&song_count) > 0 && atomic_load(&loop_mode) > 0) {
 atomic_store(&g_state.state, STOPPED);
 int next = (atomic_load(&loop_mode) == 2)
 ? (atomic_load(&play_index) + 1) % atomic_load(&song_count)
 : atomic_load(&play_index);
 if (next >= 0) {
 atomic_store(&play_index, next);
 strncpy(g_state.pending_path, playlist[next].id,
 sizeof(g_state.pending_path)-1);
 atomic_store(&g_state.seek_frame, -1);
 atomic_store(&g_state.command, 1);
 continue;
 }
 }
 atomic_store(&g_state.state, STOPPED);
 continue;
 }
 }

 // ── ③ 正常播放（解码未完成）──
 DBG("branch3: normal write, cf=%lld local=%lld", cf, g_stream_local_total);
 if (!g_decode_done) {
 size_t remaining = (total_fr > global_cf) ? (size_t)(total_fr - global_cf) : 0;
 if (remaining > 0) {
 size_t chunk = remaining > 1024 ? 1024 : remaining;
 uint8_t *pos = g_audio_buf + (size_t)cf * g_block_align;

 snd_pcm_sframes_t n = snd_pcm_writei(pcm, pos, chunk);
 if (n == -EAGAIN) {
 usleep(5000);
 } else if (n < 0) {
 n = snd_pcm_recover(pcm, n, 0);
 if (n < 0) {
 snd_pcm_close(pcm); pcm = NULL;
 atomic_store(&g_state.state, STOPPED);
 }
 } else if (n > 0) {
 atomic_fetch_add(&g_state.cur_frame, (long long)n);
 }
 }
 }
 }

 }

 usleep(5000);
 }

 if (pcm) { snd_pcm_drop(pcm); snd_pcm_close(pcm); pcm = NULL; }
 return NULL;
}

// ── ncurses 界面 ──────────────────────────────────────────
static int format_time(char *buf, size_t sz, snd_pcm_uframes_t frames, int rate) {
 if (rate <= 0) return snprintf(buf, sz, "--:--");
 int sec = frames / rate;
 return snprintf(buf, sz, "%02d:%02d", sec / 60, sec % 60);
}

static void draw_ui(WINDOW *win, int selected, int col_w) {
 werase(win);

 int state = atomic_load(&g_state.state);
 int pi = atomic_load(&play_index);
 int song_n = atomic_load(&song_count);

 // ── 标题栏（合并目录/目录名）──
 wattron(win, COLOR_PAIR(1));
 mvwhline(win, 0, 0, ' ' , col_w);
 char title_line[256];
 snprintf(title_line, sizeof(title_line), "♪ 目录 |");
 if (selected >= 0 && selected < dir_count) {
 const char *dname = strrchr(dirs[selected], '/');
 dname = dname ? dname + 1 : dirs[selected];
 strncat(title_line, dname, sizeof(title_line)-strlen(title_line)-1);
 }
 strncat(title_line, "  |  LMusic", sizeof(title_line)-strlen(title_line)-1);
 mvwprintw(win, 0, 2, "%-*s", col_w-4, title_line);
 wattroff(win, COLOR_PAIR(1));

 // ── 左面板：目录列表 ── // ── 左面板：目录列表 ──
 int rows = getmaxy(win);
 int left_w = 20; if (left_w > col_w/3) left_w = col_w/3;
 int right_w = col_w - left_w - 1;
 int list_rows = rows - 5;



 // 左面板列表
 int dir_top = 0; // 简单：不滚动
 for (int d = 0; d < dir_count && d < list_rows; d++) {
 const char *dname = strrchr(dirs[d], '/');
 dname = dname ? dname + 1 : dirs[d];
 char marker = ' ';
 if (d == pi) marker = '>';  // 当前播放歌曲所在目录
 if (active_panel == 0 && d == selected) {
 wattron(win, COLOR_PAIR(2) | A_BOLD);
 mvwprintw(win, 2+d, 2, "%c %s", marker, dname);
 wattroff(win, COLOR_PAIR(2) | A_BOLD);
 } else {
 int has_songs = 0;
 for (int i = 0; i < song_n; i++) {
 if (strcmp(playlist[i].aux_label, dname) == 0) { has_songs = 1; break; }
 }
 wattron(win, has_songs ? A_NORMAL : A_DIM);
 mvwprintw(win, 2+d, 2, "%c %s", marker, dname);
 wattroff(win, has_songs ? A_NORMAL : A_DIM);
 }
 }

 // ── 分隔线 ──
 for (int r = 1; r < rows - 4; r++)
 mvwaddch(win, r, left_w, '|');

 // ── 右面板：歌曲列表 ──
 // 右面板：始终显示选中目录的歌曲
 if (selected >= 0 && selected < dir_count) {
 if (selected >= 0 && selected < dir_count) {
 const char *dname = strrchr(dirs[selected], '/');
 dname = dname ? dname + 1 : dirs[selected];


 // 右面板标题


 int song_idx = 0;
 for (int i = 0; i < song_n && song_idx < list_rows; i++) {
 // 只显示属于当前目录的
 const char *sdir = strrchr(dirs[selected], '/');
 sdir = sdir ? sdir + 1 : dirs[selected];
 if (strcmp(playlist[i].aux_label, sdir) != 0 &&
 (playlist[i].aux_label[0] && strcmp(playlist[i].aux_label, sdir) != 0))
 continue;

 char line[320];
 if (playlist[i].artist[0])
 snprintf(line, sizeof(line), "%s - %s", playlist[i].artist, playlist[i].title);
 else
 strncpy(line, playlist[i].title, sizeof(line)-1);

 char dur_str[16] = "";
 if (playlist[i].duration_sec > 0)
 snprintf(dur_str, sizeof(dur_str), "%d:%02d", playlist[i].duration_sec/60, playlist[i].duration_sec%60);

 char marker = ' ';
 if (active_panel == 1 && song_idx == song_sel) marker = '>';
 else if (i == pi) marker = '>';

 int line_row = 2 + song_idx;

 if (active_panel == 1 && song_idx == song_sel) {
 wattron(win, COLOR_PAIR(2) | A_BOLD);
 mvwprintw(win, line_row, left_w + 2, "%c %s", marker, line);
 wattroff(win, COLOR_PAIR(2) | A_BOLD);
 } else if (i == pi) {
 mvwprintw(win, line_row, left_w + 2, "%c %s", marker, line);
 } else {
 mvwprintw(win, line_row, left_w + 2, "  %s", line);
 }
 if (dur_str[0])
 mvwprintw(win, line_row, col_w - (int)strlen(dur_str) - 2, "%s", dur_str);
 song_idx++;
 }
 } else {
 mvwprintw(win, 1, left_w + 2, "请选择目录");
 }
 }

 // ── 进度条 ──
 mvwhline(win, rows-4, 0, '-', col_w);
 int progress_row = rows - 3;
 long long frame_off = atomic_load(&g_state.frame_offset);
 snd_pcm_uframes_t cur_f = (snd_pcm_uframes_t)(frame_off + atomic_load(&g_state.playback_frame));
 long long total_dur = atomic_load(&g_state.total_duration_frames);
 snd_pcm_uframes_t total_f = (snd_pcm_uframes_t)(total_dur > 0 ? total_dur : atomic_load(&g_state.total_frames));
 int rate = atomic_load(&g_state.sample_rate);
 char cur_t[16], tot_t[16];
 format_time(cur_t, 16, cur_f, rate);
 format_time(tot_t, 16, total_f, rate);
 int bar_w = col_w - 22;
 if (bar_w > 0) {
 int filled = total_f > 0 ? (int)(cur_f * bar_w / total_f) : 0;
 if (filled > bar_w) filled = bar_w;
 mvwprintw(win, progress_row, 2, "%s", cur_t);
 mvwprintw(win, progress_row, 2+8, " ");
 // 进度条：已播放用 █（实心块），未播放用 ░（浅色点）
 char progress[512];
 int pw = 0;
 for (int i = 0; i < bar_w && pw < (int)sizeof(progress)-4; i++)
 pw += snprintf(progress + pw, sizeof(progress) - pw,
 i < filled ? "\xe2\x96\x88" : "\xe2\x96\x91");
 mvwprintw(win, progress_row, 2+9, "%s", progress);
 mvwprintw(win, progress_row, 2+10+bar_w, " %s", tot_t);
 }

 // ── 底部状态栏 ──
 int bot_row = rows - 1;
 char song_name[256] = "(无)";
 if (pi >= 0 && pi < song_n) strncpy(song_name, playlist[pi].title, sizeof(song_name)-1);
 const char *loop_str = "";
 if (atomic_load(&loop_mode) == 1) loop_str = " [单曲]";
 else if (atomic_load(&loop_mode) == 2) loop_str = " [列表]";
 char artist_str[128] = "";
 if (pi >= 0 && pi < song_n && playlist[pi].artist[0])
 snprintf(artist_str, sizeof(artist_str), " %s -", playlist[pi].artist);

 wattron(win, COLOR_PAIR(3));
 mvwhline(win, bot_row, 0, ' ', col_w);
 if (quitting) {
 mvwprintw(win, bot_row, 2, "确认退出？再按 q 或 Ctrl+C 退出，其他键取消");
 } else if (song_name[0] && strcmp(song_name, "(无)") != 0) {
 mvwprintw(win, bot_row, 2, "%s%s %s%s", state==PLAYING?"▶":state==PAUSED?"⏸":"⏹", artist_str, song_name, loop_str);
 mvwprintw(win, bot_row, col_w-30, "%dHz %dch", rate, atomic_load(&g_state.num_channels));
 } else {
 mvwprintw(win, bot_row, 2, "Tab切换面板 ↑↓选择 q退出 Ctrl+R刷新");
 }
 wattroff(win, COLOR_PAIR(3));

 wmove(win, 2, active_panel == 0 ? 2 : left_w + 2);
 wrefresh(win);
}

// ── 主函数 ────────────────────────────────────────────────

// ── 媒体库缓存（纯文本格式）────────────────────────────────
#define CACHE_DIR  ".cache/simple-player"
#define CACHE_FILE "library.db"

static void cache_path(char *buf, size_t sz) {
    const char *home = getenv("HOME");
    if (!home) home = ".";
    snprintf(buf, sz, "%s/%s/%s", home, CACHE_DIR, CACHE_FILE);
}

// 保存 Song 数组到缓存。返回 0 成功，-1 失败
static int save_library(void) {
    char path[512];
    cache_path(path, sizeof(path));
    // 确保目录存在
    char dir[512];
    snprintf(dir, sizeof(dir), "%s/%s", getenv("HOME") ? getenv("HOME") : ".", CACHE_DIR);
    mkdir(dir, 0755);
    FILE *f = fopen(path, "w");
    if (!f) return -1;
    int n = atomic_load(&song_count);
    for (int i = 0; i < n; i++) {
        Song *s = &playlist[i];
        fprintf(f, "%s\n%s\n%s\n%s\n%s\n%d\n\n",
            s->id, s->title, s->artist, s->album, s->aux_label, s->duration_sec);
    }
    fclose(f);
    return 0;
}

// 从缓存加载 Song 数组。返回加载的歌曲数
static int load_library(void) {
    char path[512];
    cache_path(path, sizeof(path));
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    int count = 0;
    char buf[512];
    while (count < MAX_SONGS && fgets(buf, sizeof(buf), f)) {
        // 跳过空行
        if (buf[0] == '\n') continue;
        // 去换行
        size_t len = strlen(buf);
        while (len > 0 && (buf[len-1] == '\n' || buf[len-1] == '\r'))
            buf[--len] = '\0';
        
        Song *s = &playlist[count];
        s->source = SRC_LOCAL;
        // id (path)
        strncpy(s->id, buf, sizeof(s->id)-1);
        // title
        if (!fgets(buf, sizeof(buf), f)) break;
        len = strlen(buf);
        while (len > 0 && (buf[len-1] == '\n' || buf[len-1] == '\r')) buf[--len] = '\0';
        strncpy(s->title, buf, sizeof(s->title)-1);
        // artist
        if (!fgets(buf, sizeof(buf), f)) break;
        len = strlen(buf);
        while (len > 0 && (buf[len-1] == '\n' || buf[len-1] == '\r')) buf[--len] = '\0';
        strncpy(s->artist, buf, sizeof(s->artist)-1);
        // album
        if (!fgets(buf, sizeof(buf), f)) break;
        len = strlen(buf);
        while (len > 0 && (buf[len-1] == '\n' || buf[len-1] == '\r')) buf[--len] = '\0';
        strncpy(s->album, buf, sizeof(s->album)-1);
        // aux_label
        if (!fgets(buf, sizeof(buf), f)) break;
        len = strlen(buf);
        while (len > 0 && (buf[len-1] == '\n' || buf[len-1] == '\r')) buf[--len] = '\0';
        strncpy(s->aux_label, buf, sizeof(s->aux_label)-1);
        // duration_sec
        if (!fgets(buf, sizeof(buf), f)) break;
        s->duration_sec = atoi(buf);
        // skip blank line separator
        fgets(buf, sizeof(buf), f);
        count++;
    }
    fclose(f);
    return count;
}

// ── 配置文件 ──────────────────────────────────────────────
#define CONFIG_DIR  ".config/simple-player"
#define CONFIG_FILE "dirs"

static void config_path(char *buf, size_t sz) {
    const char *home = getenv("HOME");
    if (!home) home = ".";
    snprintf(buf, sz, "%s/%s/%s", home, CONFIG_DIR, CONFIG_FILE);
}

static void read_dirs(void) {
    char path[512];
    config_path(path, sizeof(path));
    FILE *f = fopen(path, "r");
    if (!f) return;
    dir_count = 0;
    while (dir_count < 64 && fgets(dirs[dir_count], 512, f)) {
        size_t len = strlen(dirs[dir_count]);
        while (len > 0 && (dirs[dir_count][len-1] == '\n' || dirs[dir_count][len-1] == '\r'))
            dirs[dir_count][--len] = '\0';
        if (dirs[dir_count][0]) dir_count++;
    }
    fclose(f);
}

static void save_dirs(void) {
    char path[512];
    config_path(path, sizeof(path));
    char dir[512];
    snprintf(dir, sizeof(dir), "%s/%s", getenv("HOME") ? getenv("HOME") : ".", CONFIG_DIR);
    mkdir(dir, 0755);
    FILE *f = fopen(path, "w");
    if (!f) return;
    for (int i = 0; i < dir_count; i++)
        fprintf(f, "%s\n", dirs[i]);
    fclose(f);
}

int main(int argc, char *argv[]) {
 setlocale(LC_ALL, "");

    // 收集目录列表：命令行参数追加到配置文件，否则读配置
    dir_count = 0;
    read_dirs();  // 先读已有的
    if (argc > 1) {
        for (int i = 1; i < argc && dir_count < 64; i++) {
            // 跳过已存在的目录
            int dup = 0;
            for (int j = 0; j < dir_count; j++) {
                if (strcmp(dirs[j], argv[i]) == 0) { dup = 1; break; }
            }
            if (!dup)
                strncpy(dirs[dir_count++], argv[i], 511);
        }
        save_dirs();  // 保存合并后的列表
    }
    if (dir_count == 0) {
        strncpy(dirs[0], ".", 511);
        dir_count = 1;
    }

    // 尝试从缓存加载
    int loaded = load_library();
    if (loaded > 0) {
        atomic_store(&song_count, loaded);
    } else {
        // 缓存不存在 → 扫描所有目录
        atomic_store(&song_count, 0);
        for (int d = 0; d < dir_count; d++) {
            int start = atomic_load(&song_count);
            int n = scan_songs_at(dirs[d], start);
            dir_counts[d] = n;
            if (n > 0)
                atomic_store(&song_count, start + n);
        }
        // 扫描完建缓存
        if (atomic_load(&song_count) > 0)
            save_library();
    }
    // 默认选中第一个有歌曲的目录
    int init_dir = 0;
    for (int d = 0; d < dir_count; d++) {
        const char *dname = strrchr(dirs[d], '/');
        dname = dname ? dname + 1 : dirs[d];
        int has_songs = 0;
        for (int i = 0; i < atomic_load(&song_count); i++) {
            if (strcmp(playlist[i].aux_label, dname) == 0) {
                has_songs = 1;
                break;
            }
        }
        if (has_songs) { init_dir = d; break; }
    }

    if (atomic_load(&song_count) == 0) {
        fprintf(stderr, "提示: 没有找到音频文件\n");
    }

 snd_lib_error_set_handler(noop_alsa_err);

 atomic_store(&g_state.state, STOPPED);
 atomic_store(&g_state.quit, false);
 pthread_t pt;
 pthread_create(&pt, NULL, playback_thread, NULL);

 initscr();
 signal(SIGINT, handle_sigint);
 start_color();
 init_pair(1, COLOR_WHITE, COLOR_BLUE);   // 标题栏：白字蓝底
 init_pair(2, COLOR_WHITE, COLOR_CYAN);   // 选中行：白字青底
 init_pair(3, COLOR_WHITE, COLOR_BLUE);   // 底部栏：白字蓝底
 cbreak();
 noecho();
 keypad(stdscr, TRUE);
 curs_set(0);
 timeout(30);

 int selected = init_dir, running = 1, col_w;

 while (running) {
 if (sigint_caught) {
 if (quitting) { running = 0; break; }
 quitting = 1;
 sigint_caught = 0;
 }
 col_w = getmaxx(stdscr);
 if (getmaxy(stdscr) < 10 || col_w < 40) {
 mvprintw(0,0,"终端太小，至少需要 40x10");
 refresh();
 goto input;
 }
 draw_ui(stdscr, selected, col_w);

input:
 int ch = getch();
 switch (ch) {
 case 'q': case 'Q':
 if (!quitting) {
 quitting = 1;
 } else {
 running = 0;
 }
 break;
 case ERR: break;   // 超时无按键
 default:
 if (quitting) quitting = 0;
 break;

 case KEY_UP:
 if (active_panel == 0) {
 // 左面板：切换目录
 if (dir_count == 0) break;
 selected = (selected - 1 + dir_count) % dir_count;
 song_sel = 0;
 } else {
 // 右面板：从选中目录过滤歌曲
 if (dir_count == 0) break;
 const char *dname = strrchr(dirs[selected], '/');
 dname = dname ? dname + 1 : dirs[selected];
 int cnt = 0;
 for (int i = 0; i < atomic_load(&song_count); i++)
 if (strcmp(playlist[i].aux_label, dname) == 0) cnt++;
 if (cnt == 0) break;
 song_sel = (song_sel - 1 + cnt) % cnt;
 }
 break;
 case KEY_DOWN:
 if (active_panel == 0) {
 if (dir_count == 0) break;
 selected = (selected + 1) % dir_count;
 song_sel = 0;
 } else {
 if (dir_count == 0) break;
 const char *dname = strrchr(dirs[selected], '/');
 dname = dname ? dname + 1 : dirs[selected];
 int cnt = 0;
 for (int i = 0; i < atomic_load(&song_count); i++)
 if (strcmp(playlist[i].aux_label, dname) == 0) cnt++;
 if (cnt == 0) break;
 song_sel = (song_sel + 1) % cnt;
 }
 break;

 case '\t':
 active_panel = !active_panel;
 break;

 case '\n': case '\r':
 if (active_panel == 0 || dir_count == 0) break;
 // 在右面板：播放选中的歌曲
 {
 const char *dname = strrchr(dirs[selected], '/');
 dname = dname ? dname + 1 : dirs[selected];
 int cnt = 0;
 int target = -1;
 for (int i = 0; i < atomic_load(&song_count); i++) {
 if (strcmp(playlist[i].aux_label, dname) == 0) {
 if (cnt == song_sel) { target = i; break; }
 cnt++;
 }
 }
 if (target < 0) break;
 atomic_store(&play_index, target);
 strncpy(g_state.pending_path, playlist[target].id,
 sizeof(g_state.pending_path)-1);
 atomic_store(&g_state.command, 1);
 }
 break;

 case ' ':
 {
 int st = atomic_load(&g_state.state);
 if (st == PLAYING) atomic_store(&g_state.command, 2);
 else if (st == PAUSED) atomic_store(&g_state.command, 3);
 else if (st == STOPPED) {
 pthread_mutex_lock(&g_data_mutex);
 int has_song = (g_state.cur_song[0] != '\0');
 if (has_song) {
 strncpy(g_state.pending_path, g_state.cur_song,
 sizeof(g_state.pending_path)-1);
 if (atomic_load(&play_index) < 0) {
 for (int i = 0; i < atomic_load(&song_count); i++) {
 if (strcmp(playlist[i].id, g_state.cur_song) == 0) {
 atomic_store(&play_index, i);
 break;
 }
 }
 }
 }
 pthread_mutex_unlock(&g_data_mutex);
 if (has_song) atomic_store(&g_state.command, 1);
 }
 }
 break;

 case 's': case 'S':
 atomic_store(&g_state.command, 4);
 break;

 case 'n': case 'N':
 if (atomic_load(&song_count) > 0) atomic_store(&g_state.command, 5);
 break;
 case 'p': case 'P':
 if (atomic_load(&song_count) > 0) atomic_store(&g_state.command, 6);
 break;

 case 18:  // Ctrl+R 刷新缓存
 {
 char cp[512];
 cache_path(cp, sizeof(cp));
 remove(cp);
 }
 atomic_store(&song_count, 0);
 for (int d = 0; d < dir_count; d++) {
 int start = atomic_load(&song_count);
 int n = scan_songs_at(dirs[d], start);
 dir_counts[d] = n;
 if (n > 0)
 atomic_store(&song_count, start + n);
 }
 if (atomic_load(&song_count) > 0) save_library();
 song_sel = 0;
 selected = 0;
 // 重新定位第一个有歌曲的目录
 for (int d = 0; d < dir_count; d++) {
 const char *dname = strrchr(dirs[d], '/');
 dname = dname ? dname + 1 : dirs[d];
 for (int i = 0; i < atomic_load(&song_count); i++) {
 if (strcmp(playlist[i].aux_label, dname) == 0) {
 selected = d; d = dir_count; break;
 }
 }
 }
 break;

 case 'r': case 'R':
 atomic_store(&loop_mode, (atomic_load(&loop_mode) + 1) % 3);
 break;

 case 'd': case 'D':
 if (active_panel == 1) {
 // 删歌仅在右面板可用
 if (dir_count == 0) break;
 const char *dname = strrchr(dirs[selected], '/');
 dname = dname ? dname + 1 : dirs[selected];
 int cnt = 0, target = -1;
 for (int i = 0; i < atomic_load(&song_count); i++) {
 if (strcmp(playlist[i].aux_label, dname) == 0) {
 if (cnt == song_sel) { target = i; break; }
 cnt++;
 }
 }
 if (target < 0 || target == atomic_load(&play_index)) break;
 for (int i = target; i < atomic_load(&song_count) - 1; i++)
 playlist[i] = playlist[i + 1];
 atomic_fetch_sub(&song_count, 1);
 if (atomic_load(&play_index) > target) atomic_store(&play_index, atomic_load(&play_index) - 1);
 }
 break;

 case KEY_LEFT:
 case KEY_RIGHT:
 {
 // 在左面板时切换面板
 if (active_panel == 0) {
 active_panel = 1;
 break;
 }
 int st = atomic_load(&g_state.state);
 if (st != PLAYING && st != PAUSED) break;
 long long rate = atomic_load(&g_state.sample_rate);
 if (rate <= 0) break;
 long long step = rate * 5;
 long long foff = atomic_load(&g_state.frame_offset);
 long long local_play = atomic_load(&g_state.playback_frame);
 long long cur_global = foff + local_play;
 long long target = (ch == KEY_LEFT) ? cur_global - step : cur_global + step;
 if (target < 0) target = 0;
 long long total_dur = atomic_load(&g_state.total_duration_frames);
 long long total = total_dur > 0 ? total_dur : atomic_load(&g_state.total_frames);
 if (total > 0 && target >= total) {
 atomic_store(&g_state.command, 4);
 break;
 }
 atomic_store(&g_state.seek_frame, target);
 pthread_mutex_lock(&g_data_mutex);
 if (g_state.cur_song[0]) {
 strncpy(g_state.pending_path, g_state.cur_song,
 sizeof(g_state.pending_path)-1);
 }
 pthread_mutex_unlock(&g_data_mutex);
 atomic_store(&g_state.command, 1);
 }
 break;

 case KEY_RESIZE: break;
 }
 }

 atomic_store(&g_state.quit, true);
 atomic_store(&g_state.command, 4);
 pthread_join(pt, NULL);

 if (g_audio_buf) free(g_audio_buf);
 endwin();
 printf("再见 ♪\n");
 return 0;
}
