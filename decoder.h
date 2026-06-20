// decoder.h — 通用音频解码接口
// 支持：MP3, FLAC, OGG, AAC, WMA, Opus, WAV 等

#ifndef DECODER_H
#define DECODER_H

#include <stdint.h>
#include <stddef.h>

typedef struct {
    uint16_t num_channels;    // 声道数
    uint32_t sample_rate;     // 采样率 (Hz)
    uint16_t bits_per_sample; // 位深 (16)
    uint16_t block_align;     // = num_channels * bits_per_sample / 8
    size_t   total_samples;   // 总采样点数（每声道）
} AudioInfo;

// 读取音频文件标签元数据
// 返回 0 成功，-1 失败（文件不存在或无法读取）
int read_audio_tags(const char *path,
    char *title, int title_len,
    char *artist, int artist_len,
    char *album, int album_len,
    int *duration_sec);

// 解码音频文件为 16-bit PCM（全部解码到内存）
int decode_audio(const char *path, uint8_t **buf, size_t *size, AudioInfo *info);

// ── 流式解码接口 ──────────────────────────────────────────

typedef struct {
    void *priv; // 内部实现，不对外暴露
} StreamDecoder;

// 打开文件，准备流式解码
// 返回 0 成功，-1 失败
int stream_open(StreamDecoder *sd, const char *path, AudioInfo *info);

// 向缓冲区追加解码数据。
// *buf: 缓冲区指针（可能被 realloc）
// *cap: 缓冲区容量（realloc 后更新）
// *total_frames: 累计已解码帧数（此调用后更新）
// 返回本次解码的新帧数，0 = EOF，-1 = 错误
int stream_decode(StreamDecoder *sd, uint8_t **buf, size_t *cap, long long *total_frames);

// 跳转到目标帧位置（帧号从歌曲开头算）
// target_frame: 目标帧号
// sample_rate: 采样率，用于帧→时间戳转换
// 返回 0 成功，-1 失败
int stream_seek(StreamDecoder *sd, long long target_frame, int sample_rate);

// 关闭并清理
void stream_close(StreamDecoder *sd);

#endif // DECODER_H
