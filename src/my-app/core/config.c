#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#define CONFIG_PATH "/oem/etc/myapp.conf"

static char *trim(char *s) {
    if (!s) return s;
    while (*s && isspace((unsigned char)*s)) s++;
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) e--;
    *e = '\0';
    return s;
}

void myapp_config_load(myapp_config_t *cfg) {
    if (!cfg) return;
    /* 默认值（与 scripts/myapp.conf.sample 保持一致） */
    snprintf(cfg->wifi_ssid,     sizeof(cfg->wifi_ssid),     "%s", "CU_Ahta");
    snprintf(cfg->wifi_password, sizeof(cfg->wifi_password), "%s", "xd39ad5z");
    snprintf(cfg->wifi_iface,    sizeof(cfg->wifi_iface),    "%s", "wlan0");
    snprintf(cfg->server_host,   sizeof(cfg->server_host),   "%s", "192.168.1.7");
    cfg->scene_words_port       = 8002;
    cfg->photo_search_port      = 8003;
    cfg->immersive_english_port = 8004;
    cfg->english_practice_port  = 8005;

    FILE *fp = fopen(CONFIG_PATH, "r");
    if (!fp) {
        printf("[config] %s not found, use defaults: "
               "wifi=%s iface=%s host=%s scene=%d photo=%d immer=%d eng=%d\n",
               CONFIG_PATH, cfg->wifi_ssid, cfg->wifi_iface,
               cfg->server_host,
               cfg->scene_words_port, cfg->photo_search_port,
               cfg->immersive_english_port, cfg->english_practice_port);
        return;
    }

    char line[256];
    while (fgets(line, sizeof(line), fp)) {
        char *s = trim(line);
        if (!*s || *s == '#') continue;
        char *eq = strchr(s, '=');
        if (!eq) continue;
        *eq = '\0';
        char *key = trim(s);
        char *val = trim(eq + 1);
        if (!*key || !*val) continue;

        if (strcmp(key, "wifi_ssid") == 0) {
            snprintf(cfg->wifi_ssid, sizeof(cfg->wifi_ssid), "%s", val);
        } else if (strcmp(key, "wifi_password") == 0) {
            snprintf(cfg->wifi_password, sizeof(cfg->wifi_password), "%s", val);
        } else if (strcmp(key, "wifi_iface") == 0) {
            snprintf(cfg->wifi_iface, sizeof(cfg->wifi_iface), "%s", val);
        } else if (strcmp(key, "server_host") == 0) {
            snprintf(cfg->server_host, sizeof(cfg->server_host), "%s", val);
        } else if (strcmp(key, "scene_words_port") == 0 ||
                   strcmp(key, "vlm_port") == 0) {          /* 老 key 兼容 */
            cfg->scene_words_port = atoi(val);
        } else if (strcmp(key, "photo_search_port") == 0 ||
                   strcmp(key, "homework_port") == 0) {     /* 老 key 兼容 */
            cfg->photo_search_port = atoi(val);
        } else if (strcmp(key, "immersive_english_port") == 0 ||
                   strcmp(key, "translate_port") == 0) {    /* 老 key 兼容 */
            cfg->immersive_english_port = atoi(val);
        } else if (strcmp(key, "english_practice_port") == 0 ||
                   strcmp(key, "english_port") == 0) {      /* 老 key 兼容 */
            cfg->english_practice_port = atoi(val);
        }
    }
    fclose(fp);
    printf("[config] loaded: wifi=%s iface=%s host=%s scene=%d photo=%d immer=%d eng=%d\n",
           cfg->wifi_ssid, cfg->wifi_iface,
           cfg->server_host,
           cfg->scene_words_port, cfg->photo_search_port,
           cfg->immersive_english_port, cfg->english_practice_port);
}

int myapp_config_build_url(char *buf, int buflen,
                             const char *host, int port, const char *path) {
    if (!buf || buflen <= 0 || !host || !path) return -1;
    return snprintf(buf, buflen, "ws://%s:%d%s", host, port, path);
}
