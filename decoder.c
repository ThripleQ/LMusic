// decoder.c — 通用音频解码（基于 FFmpeg）
// 编译时链接: -lavformat -lavcodec -lavutil -lswresample

#include "decoder.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libswresample/swresample.h>

int read_audio_tags(const char *path,
    char *title, int title_len,
    char *artist, int artist_len,
    char *album, int album_len,
    int *duration_sec)
{
    AVFormatContext *fmt_ctx = NULL;
    if (avformat_open_input(&fmt_ctx, path, NULL, NULL) < 0)
        return -1;
    if (avformat_find_stream_info(fmt_ctx, NULL) < 0) {
        avformat_close_input(&fmt_ctx);
        return -1;
    }

    // 读格式层元数据
    AVDictionaryEntry *tag = NULL;
    if (title) title[0] = '\0';
    if (artist) artist[0] = '\0';
    if (album) album[0] = '\0';
    while ((tag = av_dict_get(fmt_ctx->metadata, "", tag, AV_DICT_IGNORE_SUFFIX))) {
        if (title && strcasecmp(tag->key, "title") == 0)
            strncpy(title, tag->value, title_len - 1);
        else if (artist && strcasecmp(tag->key, "artist") == 0)
            strncpy(artist, tag->value, artist_len - 1);
        else if (album && strcasecmp(tag->key, "album") == 0)
            strncpy(album, tag->value, album_len - 1);
    }

    // 如果格式层没有，尝试从音频流 metadata 读取
    if (title && !title[0]) {
        for (unsigned i = 0; i < fmt_ctx->nb_streams; i++) {
            AVDictionaryEntry *st = NULL;
            while ((st = av_dict_get(fmt_ctx->streams[i]->metadata, "", st, AV_DICT_IGNORE_SUFFIX))) {
                if (strcasecmp(st->key, "title") == 0) {
                    strncpy(title, st->value, title_len - 1);
                    break;
                }
            }
            if (title[0]) break;
        }
    }

    // 总时长
    if (duration_sec) {
        if (fmt_ctx->duration > 0)
            *duration_sec = (int)(fmt_ctx->duration / AV_TIME_BASE);
        else
            *duration_sec = 0;
    }

    avformat_close_input(&fmt_ctx);
    return 0;
}

int decode_audio(const char *path, uint8_t **out_buf, size_t *out_size, AudioInfo *out_info) {
    *out_buf  = NULL;
    *out_size = 0;
    memset(out_info, 0, sizeof(*out_info));

    int ret = -1;

    // ── 1. 打开输入文件 ──
    AVFormatContext *fmt_ctx = NULL;
    if (avformat_open_input(&fmt_ctx, path, NULL, NULL) < 0) {
        fprintf(stderr, "无法打开文件: %s\n", path);
        return -1;
    }

    if (avformat_find_stream_info(fmt_ctx, NULL) < 0) {
        fprintf(stderr, "无法读取流信息\n");
        goto cleanup_fmt;
    }

    // ── 2. 找到音频流 ──
    const AVCodec *codec = NULL;
    int stream_idx = av_find_best_stream(fmt_ctx, AVMEDIA_TYPE_AUDIO, -1, -1, &codec, 0);
    if (stream_idx < 0) {
        fprintf(stderr, "未找到音频流\n");
        goto cleanup_fmt;
    }

    AVStream *stream    = fmt_ctx->streams[stream_idx];
    AVCodecParameters *codecpar = stream->codecpar;

    // ── 3. 打开解码器 ──
    AVCodecContext *codec_ctx = avcodec_alloc_context3(codec);
    if (!codec_ctx) { fprintf(stderr, "分配解码器上下文失败\n"); goto cleanup_fmt; }

    if (avcodec_parameters_to_context(codec_ctx, codecpar) < 0) {
        fprintf(stderr, "复制解码参数失败\n"); goto cleanup_codec;
    }

    if (avcodec_open2(codec_ctx, codec, NULL) < 0) {
        fprintf(stderr, "打开解码器失败\n"); goto cleanup_codec;
    }

    // ── 4. 设置重采样器（统一输出 16-bit PCM）──
    SwrContext *swr = NULL;
    AVChannelLayout out_ch_layout = codec_ctx->ch_layout;
    swr_alloc_set_opts2(&swr,
        &out_ch_layout, AV_SAMPLE_FMT_S16, codec_ctx->sample_rate,   // 输出
        &codec_ctx->ch_layout, codec_ctx->sample_fmt, codec_ctx->sample_rate, // 输入
        0, NULL);
    if (!swr || swr_init(swr) < 0) {
        fprintf(stderr, "初始化重采样器失败\n");
        if (swr) swr_free(&swr);
        goto cleanup_codec;
    }

    // ── 5. 解码循环 ──
    AVPacket *pkt = av_packet_alloc();
    AVFrame  *frm = av_frame_alloc();
    if (!pkt || !frm) { fprintf(stderr, "分配包/帧失败\n"); goto cleanup_all; }

    // 动态缓冲区
    size_t buf_capacity = 65536;
    *out_buf = malloc(buf_capacity);
    if (!*out_buf) { fprintf(stderr, "分配缓冲区失败\n"); goto cleanup_all; }
    *out_size = 0;

    while (av_read_frame(fmt_ctx, pkt) >= 0) {
        if (pkt->stream_index != stream_idx) {
            av_packet_unref(pkt);
            continue;
        }

        if (avcodec_send_packet(codec_ctx, pkt) < 0) {
            av_packet_unref(pkt);
            continue;
        }

        while (avcodec_receive_frame(codec_ctx, frm) >= 0) {
            // 重采样到 S16
            int out_samples = swr_get_out_samples(swr, frm->nb_samples);
            if (out_samples <= 0) continue;

            size_t needed = *out_size + out_samples * frm->ch_layout.nb_channels * 2;
            if (needed > buf_capacity) {
                buf_capacity = needed + 65536;
                uint8_t *new_buf = realloc(*out_buf, buf_capacity);
                if (!new_buf) { fprintf(stderr, "扩展缓冲区失败\n"); goto cleanup_all; }
                *out_buf = new_buf;
            }

            uint8_t *out_ptr = *out_buf + *out_size;
            int converted = swr_convert(swr, &out_ptr, out_samples,
                (const uint8_t **)frm->extended_data, frm->nb_samples);
            if (converted > 0) {
                *out_size += converted * frm->ch_layout.nb_channels * 2;
            }
        }
        av_packet_unref(pkt);
    }

    // ── 刷新解码器 ──
    avcodec_send_packet(codec_ctx, NULL);
    while (avcodec_receive_frame(codec_ctx, frm) >= 0) {
        int out_samples = swr_get_out_samples(swr, frm->nb_samples);
        if (out_samples <= 0) continue;

        size_t needed = *out_size + out_samples * frm->ch_layout.nb_channels * 2;
        if (needed > buf_capacity) {
            buf_capacity = needed + 65536;
            uint8_t *new_buf = realloc(*out_buf, buf_capacity);
            if (!new_buf) { fprintf(stderr, "扩展缓冲区失败\n"); goto cleanup_all; }
            *out_buf = new_buf;
        }

        uint8_t *out_ptr = *out_buf + *out_size;
        int converted = swr_convert(swr, &out_ptr, out_samples,
            (const uint8_t **)frm->extended_data, frm->nb_samples);
        if (converted > 0) {
            *out_size += converted * frm->ch_layout.nb_channels * 2;
        }
    }

    // ── 6. 填充输出信息 ──
    out_info->num_channels   = codec_ctx->ch_layout.nb_channels;
    out_info->sample_rate    = codec_ctx->sample_rate;
    out_info->bits_per_sample = 16;  // 统一输出 16-bit
    out_info->block_align    = out_info->num_channels * 2;
    // total_samples ≈ *out_size / block_align
    out_info->total_samples  = *out_size / out_info->block_align;

    ret = 0;
    av_frame_free(&frm);
    av_packet_free(&pkt);
    swr_free(&swr);
    avcodec_free_context(&codec_ctx);
    avformat_close_input(&fmt_ctx);
    return ret;

cleanup_all:
    av_frame_free(&frm);
    av_packet_free(&pkt);
    swr_free(&swr);
    free(*out_buf); *out_buf = NULL;
cleanup_codec:
    avcodec_free_context(&codec_ctx);
cleanup_fmt:
    avformat_close_input(&fmt_ctx);
    return -1;
}

// =============================================================
// 流式解码
// =============================================================

struct StreamDecoderPriv {
    AVFormatContext *fmt_ctx;
    AVCodecContext  *codec_ctx;
    SwrContext      *swr;
    AVPacket        *pkt;
    AVFrame         *frm;
    int              stream_idx;
    int              frame_bytes;  // 每帧字节数 = channels * 2
    int              channels;     // 输出声道数
    bool             eof;
};

int stream_open(StreamDecoder *sd, const char *path, AudioInfo *info) {
    av_log_set_level(AV_LOG_ERROR);
    struct StreamDecoderPriv *p = calloc(1, sizeof(struct StreamDecoderPriv));
    if (!p) return -1;
    sd->priv = p;

    if (avformat_open_input(&p->fmt_ctx, path, NULL, NULL) < 0) goto fail;
    p->fmt_ctx->max_analyze_duration = 30 * AV_TIME_BASE;
    if (avformat_find_stream_info(p->fmt_ctx, NULL) < 0) goto fail;

    const AVCodec *codec = NULL;
    p->stream_idx = av_find_best_stream(p->fmt_ctx, AVMEDIA_TYPE_AUDIO, -1,-1,&codec,0);
    if (p->stream_idx < 0) goto fail;

    AVCodecParameters *par = p->fmt_ctx->streams[p->stream_idx]->codecpar;
    p->codec_ctx = avcodec_alloc_context3(codec);
    if (!p->codec_ctx) goto fail;
    if (avcodec_parameters_to_context(p->codec_ctx, par) < 0) goto fail;
    if (avcodec_open2(p->codec_ctx, codec, NULL) < 0) goto fail;

    // 重采样：输出 S16，保持原始声道数和采样率
    AVChannelLayout out_layout = p->codec_ctx->ch_layout;
    p->channels = out_layout.nb_channels;
    p->frame_bytes = p->channels * 2;

    swr_alloc_set_opts2(&p->swr,
        &out_layout, AV_SAMPLE_FMT_S16, p->codec_ctx->sample_rate,
        &p->codec_ctx->ch_layout, p->codec_ctx->sample_fmt, p->codec_ctx->sample_rate,
        0, NULL);
    if (!p->swr || swr_init(p->swr) < 0) goto fail;

    p->pkt = av_packet_alloc();
    p->frm = av_frame_alloc();
    if (!p->pkt || !p->frm) goto fail;

    info->sample_rate    = p->codec_ctx->sample_rate;
    info->num_channels   = p->channels;
    info->bits_per_sample = 16;
    info->block_align    = p->frame_bytes;
    // 从容器元数据获取真实总帧数
    info->total_samples  = 0;
    if (p->fmt_ctx->duration > 0) {
        info->total_samples = (int64_t)(p->fmt_ctx->duration * info->sample_rate / AV_TIME_BASE);
    }
    return 0;

fail:
    stream_close(sd);
    return -1;
}

// 辅助：确保缓冲区能容纳 needed 字节
static int realloc_buf(uint8_t **buf, size_t *cap, size_t needed) {
    size_t new_cap = *cap ? *cap * 2 : 65536;
    while (new_cap < needed) new_cap *= 2;
    uint8_t *new_buf = realloc(*buf, new_cap);
    if (!new_buf) return -1;
 *buf = new_buf; *cap = new_cap;
 return 0;
}

int stream_decode(StreamDecoder *sd, uint8_t **buf, size_t *cap, long long *total_frames) {
    struct StreamDecoderPriv *p = sd->priv;
    if (!p || p->eof) return 0;

    int total_new = 0;  // 本调用解码的新帧数

    while (1) {
        // 从解码器收帧
        int ret = avcodec_receive_frame(p->codec_ctx, p->frm);

        if (ret == AVERROR(EAGAIN)) {
            // 需要更多输入数据
            int r = av_read_frame(p->fmt_ctx, p->pkt);
            if (r < 0) {
                // 文件读完，排空解码器
                avcodec_send_packet(p->codec_ctx, NULL);
                continue;
            }
            if (p->pkt->stream_index == p->stream_idx)
                avcodec_send_packet(p->codec_ctx, p->pkt);
            av_packet_unref(p->pkt);
            continue;
        } else if (ret == AVERROR_EOF) {
            // 彻底收完（包含排空后）
            // 刷新 swr 残留
            uint8_t *flush = NULL;
            int n = swr_convert(p->swr, &flush, 0, NULL, 0);
            if (n > 0) {
                // 有残留，写入 buffer
                size_t cur_bytes = (*total_frames) * p->frame_bytes;
                size_t needed = cur_bytes + (size_t)n * p->frame_bytes;
                if (needed > *cap || (realloc_buf(buf, cap, needed) != 0)) return -1;
                uint8_t *out = *buf + cur_bytes;
                int converted = swr_convert(p->swr, &out, n, NULL, 0);
                if (converted > 0) {
                    *total_frames += converted;
                    total_new += converted;
                }
            }
            p->eof = true;
            return total_new;
        } else if (ret < 0) {
            return total_new > 0 ? total_new : -1;
        }

        // ret >= 0：拿到一帧，重采样到 S16
        int out_samples = swr_get_out_samples(p->swr, p->frm->nb_samples);
        if (out_samples <= 0) { av_frame_unref(p->frm); continue; }

        size_t cur_bytes = (*total_frames) * p->frame_bytes;
        size_t needed = cur_bytes + (size_t)out_samples * p->frame_bytes;
        if (needed > *cap || (realloc_buf(buf, cap, needed) != 0)) return -1;

        uint8_t *out = *buf + cur_bytes;
        int n = swr_convert(p->swr, &out, out_samples,
            (const uint8_t **)p->frm->extended_data, p->frm->nb_samples);
        av_frame_unref(p->frm);

        if (n > 0) {
            *total_frames += n;
            total_new += n;
        }

        // 如果已经解了足够多，先返回一批
        if (total_new >= 4096) break;
    }

    return total_new;
}

int stream_seek(StreamDecoder *sd, long long target_frame, int sample_rate) {
    struct StreamDecoderPriv *p = sd->priv;
    if (!p || !p->fmt_ctx) return -1;

    int64_t ts = av_rescale_q(
        target_frame,
        (AVRational){1, sample_rate},
        p->fmt_ctx->streams[p->stream_idx]->time_base
    );

    if (av_seek_frame(p->fmt_ctx, p->stream_idx, ts, AVSEEK_FLAG_BACKWARD) < 0)
        return -1;

    avcodec_flush_buffers(p->codec_ctx);
    swr_close(p->swr);
    swr_init(p->swr);
    p->eof = false;
    return 0;
}

void stream_close(StreamDecoder *sd) {
    struct StreamDecoderPriv *p = sd->priv;
    if (!p) return;
    av_frame_free(&p->frm);
    av_packet_free(&p->pkt);
    swr_free(&p->swr);
    avcodec_free_context(&p->codec_ctx);
    avformat_close_input(&p->fmt_ctx);
    free(p);
    sd->priv = NULL;
}
