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
#include <time.h>
#include <wchar.h>
#include <errno.h>
#include <fcntl.h>
#include <alsa/asoundlib.h>
#include <ncurses.h>
#include <signal.h>
#include <locale.h>

#include "decoder.h"
#include "song.h"
#include "netease.h"

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
 atomic_llong peak_total_frames;  // 跨 seek 稳定的最大已解码帧数

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

// 静默 ALSA 运行时错误
static void noop_alsa_err(const char *f, int l, const char *fn, int e, const char *fmt, ...) {}


// ── 播放器状态 ──────────────────────────────────────────────
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

// ── 网易云状态 ──
static int netease_mode = 0;           // 当前选中目录是否为网易云
static int netease_vdir_idx = -1;      // 网易云虚拟目录索引
static int netease_submode = 0;        // 0=菜单 1=搜索结果 2=歌单 3=日推
static char netease_search_buf[256];   // 搜索关键词
static Song ne_playlist[MAX_SONGS];    // 网易云独立歌曲列表
static int ne_count = 0;               // 网易云歌曲数

// ── 扫码登录状态机 ──
static int qr_logging_in = 0;          // 1=登录中
static char qr_unikey[128];            // 扫码 key
static char qr_url[512];               // 二维码 URL
static int qr_next_check = 0;          // 下次轮询时间

// ── 列表滚动 ──
static int dir_scroll = 0;            // 左面板滚动偏移
static int song_scroll = 0;           // 右面板滚动偏移（在过滤后列表中的位置）
static int help_dismissed = 0;

// ── 加载动画 ──
static int loading = 0;
static int loading_done = 0;
static int loading_filled = 0;
static int loading_frame = 0;
static char loading_msg[64];
static char loading_buf[65536];
static int loading_len = 0;
static int loading_fd = -1;
static FILE *loading_fp = NULL;        // 帮助文字是否已消失

// ── 目录浏览 ──
static char dirs[64][512];
static int dir_count = 0;

// ── 跑马灯 ──
static int scroll_pos = 0;           // 滚动偏移（列）
static int scroll_idx = -1;          // 当前滚动的歌曲索引
static int scroll_timer = 0;         // 帧计数器

// 返回 UTF-8 字符串的终端显示宽度（列）
static int display_width(const char *s) {
    wchar_t wcs[512];
    int n = mbstowcs(wcs, s, 512);
    if (n <= 0) return (int)strlen(s);
    return wcswidth(wcs, n);
}

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
 // total_frames 只增不减：防止 seek 时变小导致总时长跳变
 long long new_total = start_frame + total_fr;
 long long old_total = atomic_load(&g_state.total_frames);
 if (new_total > old_total)
 atomic_store(&g_state.total_frames, new_total);
 // peak_total_frames：新歌归零，跨 seek 只增不减，UI 用
 if (start_frame == 0)
 atomic_store(&g_state.peak_total_frames, 0LL);
 long long peak = atomic_load(&g_state.peak_total_frames);
 if (new_total > peak)
 atomic_store(&g_state.peak_total_frames, new_total);
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

 // ── ② 解码完成且数据全写完 → 阻塞排空 ──
 if (g_decode_done && cf >= g_stream_local_total) {
 // 阻塞排空：唯一可靠的方式，等硬件播完所有数据
 snd_pcm_drain(pcm);

 // 用实际解码总帧数设置最终进度（不依赖容器 metadata）
 long long final_relative = total_fr - frame_off;
 if (final_relative < 0) final_relative = 0;
 atomic_store(&g_state.playback_frame, final_relative);

 // 清理
 snd_pcm_close(pcm); pcm = NULL;
 if (g_stream_dec) { stream_close(g_stream_dec); free(g_stream_dec); g_stream_dec = NULL; }
 g_decode_done = false;
 g_stream_local_total = 0;
 atomic_store(&g_state.frame_offset, 0LL);
 atomic_store(&g_state.total_duration_frames, 0LL);

 // 循环模式处理
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

 // 非循环模式：停止并重置进度
 atomic_store(&g_state.state, STOPPED);
 atomic_store(&g_state.playback_frame, 0LL);
 atomic_store(&g_state.cur_frame, 0LL);
 atomic_store(&g_state.total_frames, 0LL);
 continue;
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

// ── 网易云数据加载 ────────────────────────────────────

// 加载网易云菜单（虚拟歌曲列表）
static void load_netease_menu(void) {
    ne_count = 5;
    for (int i = 0; i < 5; i++) {
        ne_playlist[i].source = SRC_NETEASE;
        ne_playlist[i].artist[0] = '\0';
        ne_playlist[i].album[0] = '\0';
        ne_playlist[i].duration_sec = 0;
        snprintf(ne_playlist[i].aux_label, sizeof(ne_playlist[i].aux_label), "网易云");
    }
    snprintf(ne_playlist[0].id, sizeof(ne_playlist[0].id), "__search__");
    snprintf(ne_playlist[0].title, sizeof(ne_playlist[0].title), "🔍 搜索");
    snprintf(ne_playlist[1].title, sizeof(ne_playlist[1].title), "❤ 红心歌单");
    snprintf(ne_playlist[2].id, sizeof(ne_playlist[2].id), "__daily__");
    snprintf(ne_playlist[2].title, sizeof(ne_playlist[2].title), "每日推荐");
    snprintf(ne_playlist[3].id, sizeof(ne_playlist[3].id), "__hot__");
    snprintf(ne_playlist[3].title, sizeof(ne_playlist[3].title), "热歌榜");
    ne_count = 5;
    snprintf(ne_playlist[4].id, sizeof(ne_playlist[4].id), "__playlists__");
    snprintf(ne_playlist[4].title, sizeof(ne_playlist[4].title), "收藏歌单");
    snprintf(ne_playlist[4].aux_label, sizeof(ne_playlist[4].aux_label), "网易云");
    netease_submode = 0;
}

// 加载搜索结果
static void load_netease_search(const char *kw) {
    Song results[MAX_SONGS];
    int n = netease_search(kw, results, MAX_SONGS);
    if (n == 0) {
        // 无结果，保持菜单
        load_netease_menu();
        return;
    }
    ne_count = n;
    for (int i = 0; i < n; i++)
        ne_playlist[i] = results[i];
    netease_submode = 1;
    netease_mode = 1;
    song_sel = 0;
}

// 加载歌单
static void load_netease_playlist(const char *id) {
    Song results[MAX_SONGS];
    int n = netease_playlist_detail(id, results, MAX_SONGS);
    if (n == 0) return;
    ne_count = n;
    for (int i = 0; i < n; i++)
        ne_playlist[i] = results[i];
    netease_submode = 2;
    netease_mode = 1;
    song_sel = 0;
}

// 加载每日推荐
static void load_netease_daily(void) {
    Song results[MAX_SONGS];
    int n = netease_recommend_songs(results, MAX_SONGS);
    if (n == 0) return;
    ne_count = n;
    for (int i = 0; i < n; i++)
        ne_playlist[i] = results[i];
    netease_submode = 3;
    netease_mode = 1;
    song_sel = 0;
}

// 加载红心歌单
static void load_netease_liked(void) {
    Song results[MAX_SONGS];
    int n = netease_liked_songs(results, MAX_SONGS);
    if (n == 0) return;
    ne_count = n;
    for (int i = 0; i < n; i++)
        ne_playlist[i] = results[i];
    netease_submode = 4;
    netease_mode = 1;
    song_sel = 0;
}

// ── 异步加载 ──

static void start_loading(const char *cmd, const char *msg) {
    if (loading) return;
    loading = 1; loading_done = 0; loading_filled = 0; loading_frame = 0;
    loading_len = 0; strncpy(loading_msg, msg, sizeof(loading_msg)-1);
    loading_fp = popen(cmd, "r");
    if (!loading_fp) { loading = 0; return; }
    loading_fd = fileno(loading_fp);
    int fl = fcntl(loading_fd, F_GETFL, 0);
    fcntl(loading_fd, F_SETFL, fl | O_NONBLOCK);
}

static void process_loading_result(void) {
    loading_buf[loading_len] = 0;
    int n = 0;
    if (netease_submode == 1 || netease_submode == 2)
        n = netease_parse_search(loading_buf, ne_playlist, MAX_SONGS);
    else if (netease_submode == 3)
        n = netease_parse_daily(loading_buf, ne_playlist, MAX_SONGS);
    else if (netease_submode == 4)
        n = netease_parse_playlist(loading_buf, ne_playlist, MAX_SONGS);
    else if (netease_submode == 5)
        n = netease_parse_playlists(loading_buf, ne_playlist, MAX_SONGS);
    else if (netease_submode == 6)
        n = netease_parse_search(loading_buf, ne_playlist, MAX_SONGS);
    if (n > 0) {
        ne_count = n; song_sel = 0; netease_mode = 1;
    } else {
        load_netease_menu();
    }
}

static void draw_ui(WINDOW *win, int selected, int col_w) {
 werase(win);

 // 当前播放列表（本地 or 网易云）
 Song *cur_list = netease_mode ? ne_playlist : playlist;
 int cur_total = netease_mode ? ne_count : atomic_load(&song_count);

 int state = atomic_load(&g_state.state);
 int pi = atomic_load(&play_index);

 // ── 尺寸计算 ──
 int rows = getmaxy(win);
 int left_w = 20; if (left_w > col_w/3) left_w = col_w/3;
 int right_w = col_w - left_w - 1;
 int list_rows = rows - 4;

 // ── 标题栏 ──
 wattron(win, COLOR_PAIR(1));
 mvwhline(win, 0, 0, ' ', col_w);
 mvwprintw(win, 0, 2, "Browser");
 mvwaddstr(win, 0, left_w, "│");
 if (selected >= 0 && selected < dir_count) {
  const char *dname = strrchr(dirs[selected], '/');
  dname = dname ? dname + 1 : dirs[selected];
  mvwprintw(win, 0, left_w + 2, "%s", dname);
 }
 // 循环模式（固定 6 列宽，"LMusic" 左边）
 const char *title_loop = "      ";
 if (atomic_load(&loop_mode) == 1) title_loop = "[单曲]";
 else if (atomic_load(&loop_mode) == 2) title_loop = "[列表]";
 mvwprintw(win, 0, col_w - 15, "%s", title_loop);
 mvwprintw(win, 0, col_w - 8, "LMusic");
 wattroff(win, COLOR_PAIR(1));



 // 左面板列表
 for (int d = dir_scroll; d < dir_count && d < dir_scroll + list_rows; d++) {
 const char *dname = strrchr(dirs[d], '/');
 dname = dname ? dname + 1 : dirs[d];
 char marker = ' ';
 if (d == pi) marker = '>';  // 当前播放歌曲所在目录
 if (active_panel == 0 && d == selected && d == netease_vdir_idx) marker = '>';
 int drow = 2 + (d - dir_scroll);
 if (active_panel == 0 && d == selected) {
 // 选中行：清行 + 左右红竖线 + 选中色
 if (d == netease_vdir_idx) {
  wattron(win, COLOR_PAIR(6) | A_BOLD);
 } else {
  wattron(win, COLOR_PAIR(2) | A_BOLD);
 }
 mvwhline(win, drow, 1, ' ', left_w - 1);
 mvwaddstr(win, drow, 0, "▐");
 mvwprintw(win, drow, 2, "%c %s", marker, dname);
 mvwaddstr(win, drow, left_w - 1, "▌");
 wattroff(win, COLOR_PAIR(6) | A_BOLD);
 wattroff(win, COLOR_PAIR(2) | A_BOLD);
 } else if (d == netease_vdir_idx) {
 wattron(win, COLOR_PAIR(6) | A_BOLD);
 mvwprintw(win, drow, 2, "%c %s", marker, dname);
 wattroff(win, COLOR_PAIR(6) | A_BOLD);
 } else {
 int has_songs = 0;
 int local_n = atomic_load(&song_count);
 for (int i = 0; i < local_n; i++) {
 if (strcmp(playlist[i].aux_label, dname) == 0) { has_songs = 1; break; }
 }
 wattron(win, has_songs ? A_NORMAL : A_DIM);
 mvwprintw(win, drow, 2, "%c %s", marker, dname);
 wattroff(win, has_songs ? A_NORMAL : A_DIM);
 }
 }

 // ── 分隔线 ──
 wattron(win, COLOR_PAIR(4));
 for (int r = 1; r < rows - 2; r++)
  mvwaddstr(win, r, left_w, "│");
 wattroff(win, COLOR_PAIR(4));

 // ── 右面板：歌曲列表 ──
 // 右面板：始终显示选中目录的歌曲
 if (selected >= 0 && selected < dir_count) {
 if (selected >= 0 && selected < dir_count) {
 const char *dname = strrchr(dirs[selected], '/');
 dname = dname ? dname + 1 : dirs[selected];


 // 右面板标题


 int song_idx = 0;

 // 右面板目录名
 const char *sdir = strrchr(dirs[selected], '/');
 sdir = sdir ? sdir + 1 : dirs[selected];

 // 更新滚动状态（在循环外，避免被后续迭代覆盖）
 if (active_panel != 1) {
  scroll_pos = 0; scroll_timer = 0; scroll_idx = -1;
 } else {
  int cnt = 0;
  int new_idx = -1;
  for (int t = 0; t < cur_total && cnt <= song_sel; t++) {
   if (strcmp(cur_list[t].aux_label, sdir) == 0) {
    if (cnt == song_sel) { new_idx = t; break; }
    cnt++;
   }
  }
  if (new_idx != scroll_idx) { scroll_pos = 0; scroll_timer = 0; scroll_idx = new_idx; }
 }

 int matched = 0;
 for (int i = 0; i < cur_total && song_idx < list_rows; i++) {
 // 只显示属于当前目录的
 if (strcmp(cur_list[i].aux_label, sdir) != 0)
  continue;
 matched++;
 // 跳过已滚动出视野的
 if (matched <= song_scroll) continue;

 char line[320];
 if (cur_list[i].artist[0])
 snprintf(line, sizeof(line), "%s - %s", cur_list[i].artist, cur_list[i].title);
 else
 strncpy(line, cur_list[i].title, sizeof(line)-1);

 char dur_str[16] = "";
 if (cur_list[i].duration_sec > 0)
 snprintf(dur_str, sizeof(dur_str), "%d:%02d", cur_list[i].duration_sec/60, cur_list[i].duration_sec%60);

 char marker = ' ';
 if (active_panel == 1 && song_idx + song_scroll == song_sel) marker = '>';
 else if (i == pi) marker = '>';

 int line_row = 2 + song_idx;
 int dur_w = dur_str[0] ? (int)strlen(dur_str) + 2 : 0;
 int max_w = col_w - (left_w + 2) - dur_w - 3;
 int line_w = display_width(line);
 int is_sel = (active_panel == 1 && song_idx + song_scroll == song_sel);

 // 构建显示字符串（跑马灯 or 截断）
 char disp[320];
 int trim = 0;
 if (is_sel && line_w > max_w) {
  // 跑马灯：按列偏移截取
  scroll_timer++;
  if (scroll_timer >= 3) {  // 每 3 帧 ~90ms 走 1 列
   scroll_timer = 0;
   scroll_pos++;
   // 末尾留 4 列缓冲后回 0
   if (scroll_pos > line_w + 4) scroll_pos = 0;
  }
  // 用宽字符截取
  wchar_t wcs[512];
  int nw = mbstowcs(wcs, line, 512);
  if (nw > 0) {
   int col = 0, wpos = 0;
   for (int ci = 0; ci < nw; ci++) {
    int cw = wcwidth(wcs[ci]);
    if (cw < 1) cw = 1;
    if (col + cw <= scroll_pos) { col += cw; continue; }
    if (col >= scroll_pos + max_w) break;
    // 写字符
    char mb[8];
    int mb_len = wctomb(mb, wcs[ci]);
    if (mb_len > 0) { memcpy(disp + trim, mb, mb_len); trim += mb_len; }
    col += cw;
   }
  }
  disp[trim] = '\0';
  trim = 0;
 } else if (line_w > max_w) {
  // 未选中：截断
  wchar_t wcs[512];
  int nw = mbstowcs(wcs, line, 512);
  if (nw > 0) {
   int col = 0;
   for (int ci = 0; ci < nw; ci++) {
    int cw = wcwidth(wcs[ci]);
    if (cw < 1) cw = 1;
    if (col + cw > max_w - 3) break;
    char mb[8];
    int mb_len = wctomb(mb, wcs[ci]);
    if (mb_len > 0) { memcpy(disp + trim, mb, mb_len); trim += mb_len; }
    col += cw;
   }
   memcpy(disp + trim, "...", 4);
   trim += 3;
  } else {
   trim = snprintf(disp, sizeof(disp), "%.*s...", max_w - 3 < 0 ? 0 : max_w - 3, line);
  }
 } else {
  trim = snprintf(disp, sizeof(disp), "%s", line);
 }

 if (is_sel) {
 wattron(win, COLOR_PAIR(2) | A_BOLD);
 mvwprintw(win, line_row, left_w + 2, "%c %s", marker, disp);
 wattroff(win, COLOR_PAIR(2) | A_BOLD);
 } else if (i == pi) {
 mvwprintw(win, line_row, left_w + 2, "%c %s", marker, disp);
 } else {
 mvwprintw(win, line_row, left_w + 2, "  %s", disp);
 }
 if (dur_str[0])
 mvwprintw(win, line_row, col_w - (int)strlen(dur_str) - 2, "%s", dur_str);
 song_idx++;
 }
 } else {
 mvwprintw(win, 1, left_w + 2, "请选择目录");
 }
 }

 // ── cmus 式双行底部 ──
 int info_row = rows - 2;
 int bar_row  = rows - 1;
 long long frame_off = atomic_load(&g_state.frame_offset);
 snd_pcm_uframes_t cur_f = (snd_pcm_uframes_t)(frame_off + atomic_load(&g_state.playback_frame));
 long long meta_dur = atomic_load(&g_state.total_duration_frames);
 long long peak_fr = atomic_load(&g_state.peak_total_frames);
 long long decoded_fr = atomic_load(&g_state.total_frames);
 long long best = meta_dur;
 if (best == 0) best = peak_fr;
 if (best == 0) best = decoded_fr;
 snd_pcm_uframes_t total_f = (snd_pcm_uframes_t)best;
 int rate = atomic_load(&g_state.sample_rate);
 char cur_t[16], tot_t[16];
 format_time(cur_t, 16, cur_f, rate);
 format_time(tot_t, 16, total_f, rate);

 if (qr_logging_in) {
  // 扫码登录——ncurses 内渲染二维码
  char cmd[1280];
  snprintf(cmd, sizeof(cmd), "netease-cli qr-render '%s' 2>/dev/null", qr_url);
  FILE *fp = popen(cmd, "r");
  if (fp) {
   // 清屏并居中显示
   for (int r = 1; r < rows - 3; r++) mvwhline(win, r, 0, ' ', col_w);
   int qr_y = (rows - 3) / 2 - 10;
   if (qr_y < 2) qr_y = 2;
   char qr_line[256];
   int ln = 0;
   while (ln < 30 && fgets(qr_line, sizeof(qr_line), fp)) {
    size_t len = strlen(qr_line);
    while (len > 0 && qr_line[len-1] == '\n') qr_line[--len] = '\0';
    int x = (col_w - (int)len) / 2;
    if (x < 0) x = 0;
    mvwaddstr(win, qr_y + ln, x, qr_line);
    ln++;
   }
   pclose(fp);
  }
 } else if (loading) {
  wattron(win, COLOR_PAIR(3));
  mvwhline(win, info_row, 0, ' ', col_w);
  int bar_w = col_w - 4;
  if (bar_w < 4) bar_w = 4;
  int filled = (loading_filled * bar_w + 10) / 20;
  int lx = 2;
  for (int i = 0; i < bar_w; i++, lx++) {
   if (i < filled) {
    wattron(win, COLOR_PAIR(7));
    mvwaddstr(win, info_row, lx, "━");
    wattroff(win, COLOR_PAIR(7));
   } else {
    mvwaddstr(win, info_row, lx, "━");
   }
  }
  wattroff(win, COLOR_PAIR(3));
 } else if (pi >= 0 && pi < cur_total) {
  // ── 信息行（白字蓝底，无状态图标）──
  wattron(win, COLOR_PAIR(3));
    mvwhline(win, info_row, 0, ' ', col_w);
  if (quitting) {
   mvwprintw(win, info_row, 2, "确认退出？再按 q 或 Ctrl+C 退出，其他键取消");
  } else {
   // 左：文件夹名  右：歌曲名
   mvwprintw(win, info_row, 2, "%s", cur_list[pi].aux_label);
   mvwaddstr(win, info_row, left_w, "│");
   if (cur_list[pi].artist[0])
    mvwprintw(win, info_row, left_w + 2, "%s - %s", cur_list[pi].artist, cur_list[pi].title);
   else
    mvwprintw(win, info_row, left_w + 2, "%s", cur_list[pi].title);
  }
  wattroff(win, COLOR_PAIR(3));

  // ── 进度条行（黑字白底）──
  const char *icon = state==PLAYING?"▶":state==PAUSED?"⏸":"⏹";
  const char *loop_str = "      ";
  if (atomic_load(&loop_mode) == 1) loop_str = "[单曲]";
  else if (atomic_load(&loop_mode) == 2) loop_str = "[列表]";
  // 右侧附加信息（循环模式在前，字号固定防跳动）
  char extra[64];
  snprintf(extra, sizeof(extra), "%s %dkHz %dbit  ", loop_str, rate / 1000, atomic_load(&g_state.bits_per_sample));

  wattron(win, COLOR_PAIR(5));
  mvwhline(win, bar_row, 0, ' ', col_w);
  char bar_line[512];
  int bl = 0;
  bl += snprintf(bar_line + bl, sizeof(bar_line) - bl, " %s %s / %s ", icon, cur_t, tot_t);
  // 进度条宽度 = 剩余空间 - 右侧信息 - 边距
  // 右侧固定 20 列（loop 6 + kHz 6 + bit 6 + 尾空 2），防跳动
  int bar_w = col_w - bl - 20 - 3;
  if (bar_w > 0) {
   int filled = total_f > 0 ? (int)(cur_f * bar_w / total_f) : 0;
   if (filled > bar_w) filled = bar_w;
   for (int i = 0; i < bar_w && bl < (int)sizeof(bar_line)-4; i++)
    bl += snprintf(bar_line + bl, sizeof(bar_line) - bl, "%s", i < filled ? "━" : " ");
  }
  bl += snprintf(bar_line + bl, sizeof(bar_line) - bl, " │ %s", extra);
  mvwaddstr(win, bar_row, 0, bar_line);
  wattroff(win, COLOR_PAIR(5));
 } else if (!help_dismissed) {
  wattron(win, COLOR_PAIR(3));
  mvwhline(win, info_row, 0, ' ', col_w);
  const char *hlp_loop = "";
  if (atomic_load(&loop_mode) == 1) hlp_loop = " [单曲]";
  else if (atomic_load(&loop_mode) == 2) hlp_loop = " [列表]";
  mvwprintw(win, info_row, 2, "Tab切换面板 ↑↓选择 Enter播放 q退出 Ctrl+R刷新%s", hlp_loop);
  wattroff(win, COLOR_PAIR(3));
  // 进度条常驻：即使帮助未消除也显示
  const char *loop_str = "      ";
  if (atomic_load(&loop_mode) == 1) loop_str = "[单曲]";
  else if (atomic_load(&loop_mode) == 2) loop_str = "[列表]";
  char extra[64];
  snprintf(extra, sizeof(extra), "%s %dkHz %dbit  ", loop_str, rate / 1000, atomic_load(&g_state.bits_per_sample));
  wattron(win, COLOR_PAIR(5));
  mvwhline(win, bar_row, 0, ' ', col_w);
  char bar_line[512];
  int bl = snprintf(bar_line, sizeof(bar_line), " \u23f9 00:00 / 00:00 ");
  int bar_w2 = col_w - bl - 20 - 3;
  for (int i = 0; i < bar_w2 && bl < (int)sizeof(bar_line)-4; i++)
   bl += snprintf(bar_line + bl, sizeof(bar_line) - bl, " ");
  bl += snprintf(bar_line + bl, sizeof(bar_line) - bl, " \u2502 %s", extra);
  mvwaddstr(win, bar_row, 0, bar_line);
  wattroff(win, COLOR_PAIR(5));
 } else {
  // 无播放时：显示当前选中目录/歌曲 + 静默进度条（与播放进度条格式一致）
  const char *sdname = strrchr(dirs[selected], '/');
  sdname = sdname ? sdname + 1 : dirs[selected];
  Song *slist = netease_mode ? ne_playlist : playlist;
  int stotal = netease_mode ? ne_count : atomic_load(&song_count);
  int scnt = 0, seltarget = -1;
  for (int t = 0; t < stotal; t++) {
   if (strcmp(slist[t].aux_label, sdname) == 0) {
    if (scnt == song_sel) { seltarget = t; break; }
    scnt++;
   }
  }
  wattron(win, COLOR_PAIR(3));
    mvwhline(win, info_row, 0, ' ', col_w);
  if (quitting) {
   mvwprintw(win, info_row, 2, "确认退出？再按 q 或 Ctrl+C 退出，其他键取消");
  } else {
   mvwprintw(win, info_row, 2, "%s", sdname);
   mvwaddstr(win, info_row, left_w, "│");
   if (seltarget >= 0) {
    if (slist[seltarget].artist[0])
     mvwprintw(win, info_row, left_w + 2, "%s - %s", slist[seltarget].artist, slist[seltarget].title);
    else
     mvwprintw(win, info_row, left_w + 2, "%s", slist[seltarget].title);
   }
  }
  wattroff(win, COLOR_PAIR(3));
  // 静默进度条（与播放时完全一致）
  const char *loop_str = "      ";
  if (atomic_load(&loop_mode) == 1) loop_str = "[单曲]";
  else if (atomic_load(&loop_mode) == 2) loop_str = "[列表]";
  char extra[64];
  snprintf(extra, sizeof(extra), "%s %dkHz %dbit  ", loop_str, rate / 1000, atomic_load(&g_state.bits_per_sample));
  wattron(win, COLOR_PAIR(5));
  mvwhline(win, bar_row, 0, ' ', col_w);
  char bar_line[512];
  int bl = snprintf(bar_line, sizeof(bar_line), " \u23f9 00:00 / 00:00 ");
  int bar_w = col_w - bl - 20 - 3;
  for (int i = 0; i < bar_w && bl < (int)sizeof(bar_line)-4; i++)
   bl += snprintf(bar_line + bl, sizeof(bar_line) - bl, " ");
  bl += snprintf(bar_line + bl, sizeof(bar_line) - bl, " \u2502 %s", extra);
  mvwaddstr(win, bar_row, 0, bar_line);
  wattroff(win, COLOR_PAIR(5));
 }


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
    // 默认选中网易云首页
    int init_dir = 0;
    netease_vdir_idx = 0;
    for (int i = dir_count; i > 0; i--)
        memcpy(dirs[i], dirs[i-1], sizeof(dirs[0]));
    strncpy(dirs[0], "网易云", 511);
    dir_count++;

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
 init_pair(4, COLOR_CYAN, COLOR_BLACK);   // 分隔线：青色细线
 init_pair(5, COLOR_BLACK, COLOR_WHITE);   // 进度条：黑字白底
 init_pair(6, COLOR_RED, COLOR_BLACK);
 init_pair(7, COLOR_RED, COLOR_BLUE);     // 首页：亮红色
 cbreak();
 noecho();
 keypad(stdscr, TRUE);
 curs_set(0);
 timeout(30);
 set_escdelay(0);

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
 // 网易云扫码登录轮询
 if (qr_logging_in) {
  int now = time(NULL);
  if (qr_next_check == 0) {
   qr_next_check = now + 1;  // 首次延迟 1s, 先让 draw_ui 渲染二维码
  } else if (now >= qr_next_check) {
   int r = netease_qr_check(qr_unikey);
   if (r == 1) { qr_logging_in = 0; }
   else if (r == -1) { qr_logging_in = 0; }
   else { qr_next_check = now + 2; }
  }
 }

 // ── 加载动画轮询 ──
 if (loading) {
  loading_frame++;
  if (!loading_done && loading_fp) {
   while (1) {
    ssize_t r = read(loading_fd, loading_buf + loading_len, sizeof(loading_buf) - loading_len - 1);
    if (r > 0) loading_len += r;
    else if (r == 0 || (r < 0 && errno != EAGAIN)) {
     loading_buf[loading_len] = 0; pclose(loading_fp); loading_fp = NULL; loading_fd = -1;
     loading_done = 1; break;
    } else break;
   }
   if (loading_frame % 6 == 0 && loading_filled < 20) loading_filled++;
   } else if (loading_done) {
    // CLI完成 -> 冲刺：每帧1格快速填满
    if (loading_filled++ >= 20) { loading = 0; loading_done = 0; process_loading_result(); }
   }
 }

 // 网易云目录切换：选中时自动加载菜单
 if (selected == netease_vdir_idx && !netease_mode) {
  load_netease_menu();
  netease_mode = 1;
 } else if (selected != netease_vdir_idx && netease_mode) {
  netease_mode = 0;
 }
 draw_ui(stdscr, selected, col_w);

input:
 int ch = getch();
 if (ch != ERR && ch != 'q' && ch != 'Q') quitting = 0;
 if (ch != ERR) help_dismissed = 1;
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
 if (qr_logging_in) { qr_logging_in = 0; break; }
 if (quitting) quitting = 0;
 break;

 case KEY_UP:
 if (active_panel == 0) {
 if (dir_count == 0) break;
 if (selected > 0) selected--;
 song_sel = 0;
 song_scroll = 0;
 if (selected < dir_scroll) dir_scroll = selected;
 } else {
 if (dir_count == 0) break;
 const char *dname = strrchr(dirs[selected], '/');
 dname = dname ? dname + 1 : dirs[selected];
 Song *songs = netease_mode ? ne_playlist : playlist;
 int total = netease_mode ? ne_count : atomic_load(&song_count);
 int cnt = 0;
 for (int i = 0; i < total; i++)
  if (strcmp(songs[i].aux_label, dname) == 0) cnt++;
 if (cnt == 0) break;
 if (song_sel > 0) song_sel--;
 if (song_sel < song_scroll) song_scroll = song_sel;
 }
 break;
 case KEY_DOWN:
 if (active_panel == 0) {
 if (dir_count == 0) break;
 if (selected < dir_count - 1) selected++;
 song_sel = 0;
 song_scroll = 0;
 int lr = getmaxy(stdscr) - 4;
 if (selected >= dir_scroll + lr) dir_scroll = selected - lr + 1;
 } else {
 if (dir_count == 0) break;
 const char *dname = strrchr(dirs[selected], '/');
 dname = dname ? dname + 1 : dirs[selected];
 Song *songs = netease_mode ? ne_playlist : playlist;
 int total = netease_mode ? ne_count : atomic_load(&song_count);
 int cnt = 0;
 for (int i = 0; i < total; i++)
  if (strcmp(songs[i].aux_label, dname) == 0) cnt++;
 if (cnt == 0) break;
 if (song_sel < cnt - 1) song_sel++;
 int lr2 = getmaxy(stdscr) - 4;
 if (song_sel >= song_scroll + lr2) { song_scroll = song_sel - lr2 + 1; fprintf(stderr, "[SCROLL] sel=%d scroll=%d lr2=%d\n", song_sel, song_scroll, lr2); }
 }
 break;

 case '\t':
 active_panel = !active_panel;
 break;

 case '\n': case '\r':
 if (active_panel == 0 || dir_count == 0) break;

 // ── 网易云特殊处理 ──
 if (selected == netease_vdir_idx && netease_submode == 0) {
  // 菜单模式 → 异步加载（start_loading 触发）
  if (song_sel == 0) {
   // 搜索
   timeout(-1); echo(); curs_set(1);
   int sr = getmaxy(stdscr);
   mvwhline(stdscr, sr - 2, 0, ' ', col_w);
   mvwprintw(stdscr, sr - 2, 2, "搜索: ");
   wgetnstr(stdscr, netease_search_buf, sizeof(netease_search_buf)-1);
   noecho(); curs_set(0); timeout(30);
   if (netease_search_buf[0]) {
    netease_submode = 1;
    char cmd[1024];
    snprintf(cmd, sizeof(cmd), "netease-cli search %s 2>/dev/null", netease_search_buf);
    start_loading(cmd, "\u250f 搜索中...");
   }
  } else if (song_sel == 1) {
   netease_submode = 2;
   start_loading("netease-cli liked 2>/dev/null", "\u2764 加载红心...");
  } else if (song_sel == 2) {
   netease_submode = 3;
   start_loading("netease-cli recommend-songs 2>/dev/null", "\u2601 加载推荐...");
  } else if (song_sel == 3) {
   netease_submode = 4;
   start_loading("netease-cli playlist 3778678 2>/dev/null", "\u266a 加载热歌榜...");
  } else if (song_sel == 4) {
   netease_submode = 5;
   start_loading("netease-cli playlists 2>/dev/null", "💿 加载收藏歌单...");
  }
  break;
 }

 // ── 播放网易云歌曲 ──
 if (selected == netease_vdir_idx && netease_submode > 0) {
  // 歌单列表模式（submode 5）：选中歌单 → 加载曲目
  if (netease_submode == 5) {
   int cnt = 0, target = -1;
   for (int i = 0; i < ne_count; i++) {
    if (strcmp(ne_playlist[i].aux_label, "网易云") == 0) {
     if (cnt == song_sel) { target = i; break; }
     cnt++;
    }
   }
   if (target >= 0 && ne_playlist[target].id[0]) {
    netease_submode = 6;
    char cmd[1024];
    snprintf(cmd, sizeof(cmd), "netease-cli playlist-tracks %s 2>/dev/null", ne_playlist[target].id);
    start_loading(cmd, "︧e 加载歌单曲目...");
   }
   break;
  }
  int cnt = 0, target = -1;
  for (int i = 0; i < ne_count; i++) {
   if (strcmp(ne_playlist[i].aux_label, "网易云") == 0) {
    if (cnt == song_sel) { target = i; break; }
    cnt++;
   }
  }
  if (target < 0) break;
  char url[512];
  if (netease_song_url(ne_playlist[target].id, url, sizeof(url)) != 0) {
   mvwhline(stdscr, getmaxy(stdscr)-1, 0, ' ', col_w);
   mvwprintw(stdscr, getmaxy(stdscr)-2, 2, "⚠ 需要登录网易云，按 l 扫码登录");
   wrefresh(stdscr);
   break;
  }
  strncpy(g_state.pending_path, url, sizeof(g_state.pending_path)-1);
  atomic_store(&play_index, target);
  atomic_store(&g_state.seek_frame, -1);
  atomic_store(&g_state.command, 1);
  break;
 }

 // ── 本地歌曲播放 ──
 {
 const char *dname = strrchr(dirs[selected], '/');
 dname = dname ? dname + 1 : dirs[selected];
 int total = netease_mode ? ne_count : atomic_load(&song_count);
 Song *songs = netease_mode ? ne_playlist : playlist;
 int cnt = 0;
 int target = -1;
 for (int i = 0; i < total; i++) {
 if (strcmp(songs[i].aux_label, dname) == 0) {
 if (cnt == song_sel) { target = i; break; }
 cnt++;
 }
 }
 if (target < 0) break;
 atomic_store(&play_index, target);
 strncpy(g_state.pending_path, songs[target].id,
 sizeof(g_state.pending_path)-1);
 atomic_store(&g_state.seek_frame, -1);
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
 if (has_song) {
 atomic_store(&g_state.seek_frame, -1);
 atomic_store(&g_state.command, 1);
 }
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
 // 先停播，防止播放线程访问正在重建的 playlist
 atomic_store(&g_state.command, 4);
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

 case 27:  // ESC → 网易云返回菜单
 if (netease_mode && netease_submode > 0) {
  load_netease_menu();
  netease_mode = 1;
  song_sel = 0;
 }
 break;

 case 'r': case 'R':

 case 'l': case 'L':
  if (!qr_logging_in) {
   remove("cookie.txt");
   char cookie_path[512];
   snprintf(cookie_path, sizeof(cookie_path), "%s/.cache/lmusic/cookies.txt", getenv("HOME"));
   remove(cookie_path);
   if (netease_qr_get_key(qr_url, sizeof(qr_url), qr_unikey, sizeof(qr_unikey)) == 0) {
    qr_logging_in = 1;
    qr_next_check = 0;
   }
  }
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
 // 调整 song_sel 防止越界（删最后一项时）
 int new_cnt = 0;
 for (int i = 0; i < atomic_load(&song_count); i++) {
 if (strcmp(playlist[i].aux_label, dname) == 0) new_cnt++;
 }
 if (new_cnt > 0 && song_sel >= new_cnt)
 song_sel = new_cnt - 1;
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
