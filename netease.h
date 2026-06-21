// netease.h — 网易云音乐 API 接口
// 通过 netease-cli Go 桥接调用，不依赖 Node.js

#ifndef NETEASE_H
#define NETEASE_H

#include "song.h"

#define NETEASE_CLI "netease-cli"

// ── 歌曲数据接口 ──────────────────────────────────────────

// 搜索歌曲，返回结果数。results 需预分配 MAX_NETEASE_SONGS 空间
int netease_search(const char *keyword, Song *results, int max);

// 获取歌曲播放直链 URL（MP3），返回 0 成功
int netease_song_url(const char *id, char *url, int url_len);

// 获取歌单详情（歌曲列表），返回结果数
int netease_playlist_detail(const char *id, Song *results, int max);

// 每日推荐歌曲，返回结果数
int netease_recommend_songs(Song *results, int max);

// 用户歌单列表（返回歌单 meta，不是歌曲），返回结果数
int netease_user_playlist(const char *uid, Song *results, int max);

// ── 登录 ──────────────────────────────────────────────────

// 用户红心歌单（需要登录），返回结果数
int netease_liked_songs(Song *results, int max);

// 手机号 + 密码登录。返回 0 成功
int netease_login_cellphone(const char *phone, const char *password);

// 扫码登录：step1 获取 key + URL，返回 0 成功
int netease_qr_get_key(char *url, int url_len, char *unikey, int key_len);

// 扫码登录：step2 轮询确认，返回 1=成功 0=等待 -1=失败
int netease_qr_check(const char *unikey);

// 检查登录状态
int netease_login_status(void);

#endif // NETEASE_H

// 从原始 JSON 解析搜索/歌单结果（用于异步加载）
int netease_parse_search(const char *json, Song *results, int max);
int netease_parse_daily(const char *json, Song *results, int max);
int netease_parse_playlist(const char *json, Song *results, int max);
int netease_parse_playlists(const char *json, Song *results, int max);
// 从原始 JSON 解析搜索/歌单结果（用于异步加载）
int netease_parse_search(const char *json, Song *results, int max);
int netease_parse_daily(const char *json, Song *results, int max);
int netease_parse_playlist(const char *json, Song *results, int max);
int netease_parse_playlists(const char *json, Song *results, int max);
