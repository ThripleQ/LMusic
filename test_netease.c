// test_netease.c — netease 模块功能测试
// gcc test_netease.c netease.c -o test_netease
#include "netease.h"
#include <stdio.h>

int main() {
    Song results[30];
    
    printf("=== 搜索: 周杰伦 ===\n");
    int n = netease_search("周杰伦", results, 5);
    printf("找到 %d 首:\n", n);
    for (int i = 0; i < n; i++)
        printf("  [%s] %s - %s (%ds)\n", results[i].id, results[i].artist, results[i].title, results[i].duration_sec);
    
    if (n > 0) {
        printf("\n=== 获取 URL: %s ===\n", results[0].id);
        char url[512];
        if (netease_song_url(results[0].id, url, sizeof(url)) == 0)
            printf("URL: %s\n", url);
        else
            printf("获取 URL 失败\n");
    }
    
    printf("\n=== 日推 ===\n");
    n = netease_recommend_songs(results, 3);
    printf("日推 %d 首\n", n);
    for (int i = 0; i < n && i < 3; i++)
        printf("  %s - %s\n", results[i].artist, results[i].title);
    
    printf("\n=== 歌单: 3778678 (热歌榜) ===\n");
    n = netease_playlist_detail("3778678", results, 5);
    printf("歌单 %d 首:\n", n);
    for (int i = 0; i < n && i < 5; i++)
        printf("  [%s] %s - %s\n", results[i].id, results[i].artist, results[i].title);
    
    return 0;
}
