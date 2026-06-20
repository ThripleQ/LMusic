// test_decode.c — 测试解码器
// gcc test_decode.c decoder.c -lavformat -lavcodec -lavutil -lswresample -o test_decode

#include <stdio.h>
#include <stdlib.h>
#include "decoder.h"

int main(int argc, char *argv[]) {
    if (argc != 2) {
        fprintf(stderr, "用法: %s <音频文件>\n", argv[0]);
        return 1;
    }

    uint8_t *buf = NULL;
    size_t size = 0;
    AudioInfo info;

    if (decode_audio(argv[1], &buf, &size, &info) != 0) {
        fprintf(stderr, "解码失败\n");
        return 1;
    }

    printf("✅ 解码成功: %s\n", argv[1]);
    printf("   采样率: %u Hz\n", info.sample_rate);
    printf("   声道数: %u\n", info.num_channels);
    printf("   位深:   %u bit\n", info.bits_per_sample);
    printf("   数据大小: %zu 字节 (%.1f秒)\n", size,
           (double)size / info.block_align / info.sample_rate);

    free(buf);
    return 0;
}
