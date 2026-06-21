// netease.c — 网易云音乐 API 实现
// 通过 popen() 调用 netease-cli Go 桥接，解析 JSON

#include "netease.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <ctype.h>

// ── 轻量 JSON 提取 ────────────────────────────────────────
// 不依赖第三方 JSON 库，只提取我们需要的字段

// 从 JSON 字符串中提取 "key":"value" 或 "key":123 中的 value
// 返回拷贝到 dst，需要调用者 free。
static char *json_str(const char *json, const char *key) {
    char search[128];
    snprintf(search, sizeof(search), "\"%s\"", key);
    const char *p = strstr(json, search);
    if (!p) return NULL;
    p += strlen(search);
    while (*p && (*p == ':' || *p == ' ' || *p == '\t' || *p == '\n')) p++;
    if (*p == '"') {
        // 字符串值（解码 \uXXXX 转义）
        p++;
        // 先计算解码后的长度
        size_t cap = 512;
        char *out = malloc(cap);
        if (!out) return NULL;
        size_t w = 0;
        while (*p && *p != '"' && w < cap - 1) {
            if (*p == '\\' && *(p+1) == 'u' && isxdigit(*(p+2)) && isxdigit(*(p+3)) && isxdigit(*(p+4)) && isxdigit(*(p+5))) {
                // \uXXXX → 单个字符
                char hex[5] = {*(p+2), *(p+3), *(p+4), *(p+5), '\0'};
                unsigned long cp = strtoul(hex, NULL, 16);
                if (cp < 0x80) {
                    out[w++] = (char)cp;
                } else if (cp < 0x800) {
                    out[w++] = (char)(0xC0 | (cp >> 6));
                    out[w++] = (char)(0x80 | (cp & 0x3F));
                } else {
                    out[w++] = (char)(0xE0 | (cp >> 12));
                    out[w++] = (char)(0x80 | ((cp >> 6) & 0x3F));
                    out[w++] = (char)(0x80 | (cp & 0x3F));
                }
                p += 6;
            } else {
                out[w++] = *p++;
            }
        }
        out[w] = '\0';
        return out;
    } else if (isdigit(*p) || *p == '-') {
        // 数字值
        const char *end = p;
        while (isdigit(*end) || *end == '.') end++;
        size_t len = end - p;
        char *out = malloc(len + 1);
        if (!out) return NULL;
        memcpy(out, p, len);
        out[len] = '\0';
        return out;
    }
    return NULL;
}

// 从 JSON 提取 "key":123  中的整数
static long long json_int(const char *json, const char *key) {
    char search[128];
    snprintf(search, sizeof(search), "\"%s\"", key);
    const char *p = strstr(json, search);
    if (!p) return 0;
    p += strlen(search);
    while (*p && (*p == ':' || *p == ' ' || *p == '\t' || *p == '\n')) p++;
    return atoll(p);
}

// 在 JSON 字符串中找到下一个 "key" 对应的对象 { ... } 的起始位置
// 返回指向 { 的指针，或 NULL
static const char *json_obj_start(const char *json, const char *key) {
    char search[128];
    snprintf(search, sizeof(search), "\"%s\"", key);
    const char *p = strstr(json, search);
    if (!p) return NULL;
    p += strlen(search);
    while (*p && *p != '{' && *p != '[') p++;
    return (*p == '{' || *p == '[') ? p : NULL;
}

// 找到匹配的闭合括号。从 open 位置开始（open 指向 { 或 [）
// 返回指向闭合括号的指针
static const char *json_match(const char *open) {
    char open_ch = *open;
    char close_ch = (open_ch == '{') ? '}' : ']';
    int depth = 1;
    const char *p = open + 1;
    while (*p && depth > 0) {
        if (*p == '"') { p++; while (*p && *p != '"') { if (*p == '\\') p++; p++; } }
        else if (*p == open_ch) depth++;
        else if (*p == close_ch) depth--;
        p++;
    }
    return p; // 指向闭合括号后一位
}

// ── CLI 调用 ───────────────────────────────────────────────

// 执行 netease-cli 命令，返回 JSON 字符串。调用者 free。
static char *run_cli(const char *fmt, ...) {
    char cmd[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(cmd, sizeof(cmd), fmt, ap);
    va_end(ap);

    FILE *fp = popen(cmd, "r");
    if (!fp) return NULL;

    size_t cap = 4096, len = 0;
    char *buf = malloc(cap);
    if (!buf) { pclose(fp); return NULL; }

    while (!feof(fp)) {
        if (len + 1024 >= cap) {
            cap *= 2;
            char *tmp = realloc(buf, cap);
            if (!tmp) { free(buf); pclose(fp); return NULL; }
            buf = tmp;
        }
        len += fread(buf + len, 1, cap - len - 1, fp);
    }
    buf[len] = '\0';
    pclose(fp);
    return buf;
}

// 从 song_json 中提取歌曲名（跳过 ar 数组里的 name）
// 策略：找到 "ar" 数组结束位置，之后第一个 "name" 就是歌名
static char *song_title(const char *song_json) {
    const char *ar = strstr(song_json, "\"ar\"");
    if (!ar) {
        // 无 ar 字段，直接用第二个 "name"（跳过 al 的）
        ar = strstr(song_json, "\"name\"");
        if (ar) ar = strstr(ar + 6, "\"name\"");
        if (!ar) return NULL;
    } else {
        // 找到 ar 数组的匹配 ]
        while (*ar && *ar != '[') ar++;
        if (*ar != '[') return NULL;
        const char *ar_end = json_match(ar);
        if (!ar_end) return NULL;
        // 从 ar 数组之后找 "name"
        ar = strstr(ar_end, "\"name\"");
        if (!ar) return NULL;
    }
    ar += 6;
    while (*ar && (*ar == ':' || *ar == ' ')) ar++;
    if (*ar != '"') return NULL;
    ar++;
    const char *end = strchr(ar, '"');
    if (!end) return NULL;
    size_t len = end - ar;
    char *out = malloc(len + 1);
    memcpy(out, ar, len);
    out[len] = '\0';
    return out;
}

// 从 song_json 中提取歌手名（ar 数组第一个对象的 name）
static char *artist_name(const char *song_json) {
    const char *ar = strstr(song_json, "\"ar\"");
    if (!ar) return NULL;
    // 在 ar 的 JSON 片段中找 "name"
    const char *name = strstr(ar, "\"name\"");
    if (!name) return NULL;
    name += 6;
    while (*name && (*name == ':' || *name == ' ')) name++;
    if (*name == '"') {
        name++;
        const char *end = strchr(name, '"');
        if (!end) return NULL;
        size_t len = end - name;
        char *out = malloc(len + 1);
        memcpy(out, name, len);
        out[len] = '\0';
        return out;
    }
    return NULL;
}

// 从 song_json 中提取专辑名（al 对象的 name）
static char *album_name(const char *song_json) {
    const char *al = strstr(song_json, "\"al\"");
    if (!al) return NULL;
    // al 对象里的 name 就是第一个
    const char *name = strstr(al, "\"name\"");
    if (!name) return NULL;
    name += 6;
    while (*name && (*name == ':' || *name == ' ')) name++;
    if (*name == '"') {
        name++;
        const char *end = strchr(name, '"');
        if (!end) return NULL;
        size_t len = end - name;
        char *out = malloc(len + 1);
        memcpy(out, name, len);
        out[len] = '\0';
        return out;
    }
    return NULL;
}

// ── 歌曲解析 ──────────────────────────────────────────────

static int parse_song(const char *song_json, Song *s) {
    s->source = SRC_NETEASE;

    // 取歌曲 ID：跳过 al、ar 和 privilege 里嵌套的 id
    const char *id_start = song_json;
    for (int skip = 0; skip < 2; skip++) {
        static const char *skip_keys[] = {"al", "ar", "privilege"};
        const char *key = skip_keys[skip];
        char key_q[32]; snprintf(key_q, sizeof(key_q), "\"%s\"", key);
        const char *found = strstr(id_start, key_q);
        if (found) {
            const char *open = found + strlen(key_q);
            while (*open && *open != '{' && *open != '[') open++;
            if (*open == '{' || *open == '[') {
                const char *close = json_match(open);
                if (close) id_start = close;
            }
        }
    }
    char *id = json_str(id_start, "id");
    if (!id) return -1;
    snprintf(s->id, sizeof(s->id), "%s", id);
    free(id);

    char *title = song_title(song_json);
    snprintf(s->title, sizeof(s->title), "%s", title ? title : "");
    free(title);

    char *artist = artist_name(song_json);
    snprintf(s->artist, sizeof(s->artist), "%s", artist ? artist : "");
    free(artist);

    char *album = album_name(song_json);
    snprintf(s->album, sizeof(s->album), "%s", album ? album : "");
    free(album);

    s->duration_sec = (int)(json_int(song_json, "dt") / 1000);
    snprintf(s->aux_label, sizeof(s->aux_label), "网易云");
    return 0;
}

// ── 公开 API ──────────────────────────────────────────────

int netease_search(const char *keyword, Song *results, int max) {
    char *json = run_cli("%s search %s", NETEASE_CLI, keyword);
    if (!json) return 0;

    int count = 0;
    const char *songs = json_obj_start(json, "songs");
    if (songs && *songs == '[') {
        const char *p = songs + 1;
        while (*p && count < max) {
            while (*p && *p != '{' && *p != ']') p++;
            if (*p == ']') break;
            const char *end = json_match(p);
            if (end - p > 10 && parse_song(p, &results[count]) == 0)
                count++;
            p = end;
        }
    }
    free(json);
    return count;
}

int netease_song_url(const char *id, char *url, int url_len) {
    char *json = run_cli("%s song-url %s", NETEASE_CLI, id);
    if (!json) return -1;

    int ret = -1;
    // data 是数组 [{id:..., url:"..."}]
    // 找 "url":" 防止命中 urlSource
    const char *u = strstr(json, "\"url\":\"");
    if (u) {
        u += 7;
        const char *end = strchr(u, '"');
        if (end) {
            size_t len = end - u;
            if (len > 0 && len < (size_t)url_len) {
                memcpy(url, u, len);
                url[len] = '\0';
                ret = 0;
            }
        }
    }
    free(json);
    return ret;
}

int netease_playlist_detail(const char *id, Song *results, int max) {
    char *json = run_cli("%s playlist %s", NETEASE_CLI, id);
    if (!json) return 0;

    int count = 0;
    // 歌单详情: result.playlist.tracks[]
    // 有时是 playlist.tracks[]
    const char *tracks = NULL;

    // 尝试 playlist.tracks
    const char *pl = json_obj_start(json, "playlist");
    if (pl && *pl == '{')
        tracks = json_obj_start(pl, "tracks");

    // 如果上面找不到，尝试 songs
    if (!tracks)
        tracks = json_obj_start(json, "songs");

    if (tracks && *tracks == '[') {
        const char *p = tracks + 1;
        while (*p && count < max) {
            while (*p && *p != '{' && *p != ']') p++;
            if (*p == ']') break;
            const char *end = json_match(p);
            if (end - p > 10 && parse_song(p, &results[count]) == 0)
                count++;
            p = end;
        }
    }
    free(json);
    return count;
}

int netease_recommend_songs(Song *results, int max) {
    char *json = run_cli("%s recommend-songs", NETEASE_CLI);
    if (!json) return 0;

    int count = 0;
    // recommend-songs: data.dailySongs[]
    const char *data = json_obj_start(json, "data");
    if (data && *data == '{') {
        const char *daily = json_obj_start(data, "dailySongs");
        if (daily && *daily == '[') {
            const char *p = daily + 1;
            while (*p && count < max) {
                while (*p && *p != '{' && *p != ']') p++;
                if (*p == ']') break;
                const char *end = json_match(p);
                if (end - p > 10 && parse_song(p, &results[count]) == 0)
                    count++;
                p = end;
            }
        }
    }
    free(json);
    return count;
}

int netease_user_playlist(const char *uid, Song *results, int max) {
    char *json = run_cli("%s user-playlist %s", NETEASE_CLI, uid);
    if (!json) return 0;

    int count = 0;
    // user-playlist: playlist[]
    const char *pl = json_obj_start(json, "playlist");
    if (pl && *pl == '[') {
        const char *p = pl + 1;
        while (*p && count < max) {
            while (*p && *p != '{' && *p != ']') p++;
            if (*p == ']') break;
            const char *end = json_match(p);

            // 这里返回的是歌单元数据，不是歌曲。用 name + id 填
            if (end - p > 10) {
                Song *s = &results[count];
                s->source = SRC_NETEASE;
                char *pid = json_str(p, "id");
                if (pid) {
                    snprintf(s->id, sizeof(s->id), "%s", pid);
                    free(pid);
                }
                char *pname = json_str(p, "name");
                if (pname) {
                    snprintf(s->title, sizeof(s->title), "%s", pname);
                    free(pname);
                }
                s->artist[0] = '\0';
                s->album[0] = '\0';
                s->duration_sec = 0;
                snprintf(s->aux_label, sizeof(s->aux_label), "歌单");
                count++;
            }
            p = end;
        }
    }
    free(json);
    return count;
}

int netease_liked_songs(Song *results, int max) {
    char *json = run_cli("%s liked 2>/dev/null", NETEASE_CLI);
    if (!json) return 0;
    // 输出格式与 search 一致: {code, result:{songs:[...]}}
    int count = 0;
    const char *songs = json_obj_start(json, "songs");
    if (songs && *songs == '[') {
        const char *p = songs + 1;
        while (*p && count < max) {
            while (*p && *p != '{' && *p != ']') p++;
            if (*p == ']') break;
            const char *end = json_match(p);
            if (end - p > 10 && parse_song(p, &results[count]) == 0)
                count++;
            p = end;
        }
    }
    free(json);
    return count;
}

int netease_login_cellphone(const char *phone, const char *password) {
    char *json = run_cli("%s login-cellphone %s %s", NETEASE_CLI, phone, password);
    if (!json) return -1;
    long long code = json_int(json, "code");
    free(json);
    return (code == 200) ? 0 : -1;
}

int netease_qr_get_key(char *url, int url_len, char *unikey, int key_len) {
    // 清除 cookie 文件，防止旧会话干扰
    char cp[512];
    const char *home = getenv("HOME");
    if (home) {
        snprintf(cp, sizeof(cp), "%s/.cache/lmusic/cookies.txt", home);
        remove(cp);
    }
    remove("cookie.txt");
    char *json = run_cli("%s qr-key", NETEASE_CLI);
    if (!json) return -1;
    char *u = json_str(json, "url");
    char *k = json_str(json, "unikey");
    if (u && k) {
        snprintf(url, url_len, "%s", u);
        snprintf(unikey, key_len, "%s", k);
        free(u); free(k); free(json);
        return 0;
    }
    free(u); free(k); free(json);
    return -1;
}

int netease_qr_check(const char *unikey) {
    char *json = run_cli("%s qr-check %s 2>/dev/null", NETEASE_CLI, unikey);
    if (!json) return -1;
    long long code = json_int(json, "code");
    free(json);
    // 803 = 授权成功, 800 = 二维码过期, 801 = 等待扫码, 802 = 等待确认
    if (code == 803) return 1;
    if (code == 800) return -1;
    return 0;
}

int netease_login_status(void) {
    // 简单检查 cookie.txt 是否存在
    FILE *f = fopen("netease-cli/cookie.txt", "r");
    if (f) { fclose(f); return 1; }
    return 0;
}
