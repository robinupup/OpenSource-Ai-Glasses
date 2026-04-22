/**
 * my-app core - 运行时配置加载
 *
 * 配置文件格式（key=value，每行一对，# 为注释）：
 *   # WiFi
 *   wifi_ssid=CU_Ahta
 *   wifi_password=xd39ad5z
 *   wifi_iface=wlan0
 *   # 算法服务（四个功能共用同一台服务器）
 *   server_host=192.168.1.100
 *   scene_words_port=8002         # 场景单词 (/scene_words)
 *   photo_search_port=8003        # 拍照搜题 (/photo_search)
 *   immersive_english_port=8004   # 拟境英语 (/immersive_english)
 *   english_practice_port=8005    # 英语对练 (/，全双工)
 *
 * 文件路径：/oem/etc/myapp.conf
 *   （不存在或字段缺失时回落到默认值，程序不会失败）
 *   老 key（vlm_port/homework_port/translate_port/english_port）仍兼容解析。
 */
#ifndef MYAPP_CONFIG_H
#define MYAPP_CONFIG_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    /* --- WiFi --- */
    char wifi_ssid[64];
    char wifi_password[64];
    char wifi_iface[16];    /* 默认 wlan0 */

    /* --- 算法服务（四个功能共用一台服务器） --- */
    char server_host[64];           /* e.g. 192.168.1.100 */
    int  scene_words_port;          /* 场景单词  (/scene_words)         默认 8002 */
    int  photo_search_port;         /* 拍照搜题  (/photo_search)        默认 8003 */
    int  immersive_english_port;    /* 拟境英语  (/immersive_english)   默认 8004 */
    int  english_practice_port;     /* 英语对练  (/，全双工)            默认 8005 */
} myapp_config_t;

/** 读取 /oem/etc/myapp.conf；不存在或字段缺失时回落默认值。永不失败。 */
void myapp_config_load(myapp_config_t *cfg);

/** 组装 ws URL：ws://{host}:{port}{path}；返回长度 */
int  myapp_config_build_url(char *buf, int buflen,
                             const char *host, int port, const char *path);

#ifdef __cplusplus
}
#endif

#endif
