#include "wifi.h"
#include "event_bus.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <net/if.h>

#define WIFI_CONF_PATH   "/tmp/myapp_wpa_supplicant.conf"
#define WIFI_CTRL_DIR    "/var/run/wpa_supplicant"

static struct {
    char            iface[IFNAMSIZ];
    pthread_t       monitor;
    pthread_mutex_t mutex;
    volatile int    running;
    wifi_status_t   status;
    int             initialized;
} g;

static int run_cmd(const char *fmt, ...) {
    char cmd[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(cmd, sizeof(cmd), fmt, ap);
    va_end(ap);
    return system(cmd);
}

static int get_ip_addr(const char *iface, char *out, size_t n) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return -1;
    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, iface, IFNAMSIZ - 1);
    ifr.ifr_addr.sa_family = AF_INET;
    int rc = ioctl(fd, SIOCGIFADDR, &ifr);
    close(fd);
    if (rc < 0) return -1;
    struct sockaddr_in *sin = (struct sockaddr_in *)&ifr.ifr_addr;
    if (!inet_ntop(AF_INET, &sin->sin_addr, out, n)) return -1;
    return 0;
}

static int wpa_cli_status_value(const char *iface, const char *key, char *out, size_t n) {
    char cmd[256];
    snprintf(cmd, sizeof(cmd),
             "wpa_cli -i %s -p %s status 2>/dev/null | grep '^%s=' | head -1 | cut -d= -f2-",
             iface, WIFI_CTRL_DIR, key);
    FILE *fp = popen(cmd, "r");
    if (!fp) return -1;
    if (!fgets(out, n, fp)) { pclose(fp); out[0] = '\0'; return -1; }
    pclose(fp);
    size_t len = strlen(out);
    if (len && out[len - 1] == '\n') out[len - 1] = '\0';
    return 0;
}

static void publish_state(wifi_state_t state, const char *topic) {
    pthread_mutex_lock(&g.mutex);
    g.status.state = (int)state;
    get_ip_addr(g.iface, g.status.ip, sizeof(g.status.ip));
    wifi_status_t snap = g.status;
    pthread_mutex_unlock(&g.mutex);
    event_bus_publish(topic, &snap, sizeof(snap));
}

static int write_conf(const char *ssid, const char *password) {
    FILE *fp = fopen(WIFI_CONF_PATH, "w");
    if (!fp) return -1;
    fprintf(fp,
            "ctrl_interface=DIR=%s GROUP=0\n"
            "update_config=1\n"
            "network={\n"
            "    ssid=\"%s\"\n"
            "    psk=\"%s\"\n"
            "    key_mgmt=WPA-PSK\n"
            "}\n",
            WIFI_CTRL_DIR, ssid, password);
    fclose(fp);
    return 0;
}

static void *monitor_thread(void *arg) {
    (void)arg;
    wifi_state_t last = WIFI_STATE_IDLE;
    while (g.running) {
        char state[64] = {0};
        wpa_cli_status_value(g.iface, "wpa_state", state, sizeof(state));
        wifi_state_t cur;
        if (strcmp(state, "COMPLETED") == 0)     cur = WIFI_STATE_CONNECTED;
        else if (strcmp(state, "SCANNING") == 0 ||
                 strcmp(state, "AUTHENTICATING") == 0 ||
                 strcmp(state, "ASSOCIATING") == 0 ||
                 strcmp(state, "ASSOCIATED") == 0 ||
                 strcmp(state, "4WAY_HANDSHAKE") == 0 ||
                 strcmp(state, "GROUP_HANDSHAKE") == 0) cur = WIFI_STATE_CONNECTING;
        else if (state[0] == '\0' ||
                 strcmp(state, "INACTIVE") == 0)  cur = WIFI_STATE_IDLE;
        else if (strcmp(state, "DISCONNECTED") == 0) cur = WIFI_STATE_DISCONNECTED;
        else                                      cur = WIFI_STATE_CONNECTING;

        char ssid_buf[64] = {0};
        wpa_cli_status_value(g.iface, "ssid", ssid_buf, sizeof(ssid_buf));
        pthread_mutex_lock(&g.mutex);
        strncpy(g.status.ssid, ssid_buf, sizeof(g.status.ssid) - 1);
        pthread_mutex_unlock(&g.mutex);

        if (cur != last) {
            switch (cur) {
                case WIFI_STATE_CONNECTING:
                    publish_state(cur, "wifi/connecting"); break;
                case WIFI_STATE_CONNECTED:
                    /* 成功后发 DHCP（若未取得 IP） */
                    run_cmd("ip link set %s up >/dev/null 2>&1", g.iface);
                    run_cmd("(udhcpc -i %s -n -q -t 5 >/dev/null 2>&1) || "
                            "(dhclient %s >/dev/null 2>&1)",
                            g.iface, g.iface);
                    publish_state(cur, "wifi/connected"); break;
                case WIFI_STATE_DISCONNECTED:
                    publish_state(cur, "wifi/disconnected"); break;
                default:
                    publish_state(cur, "wifi/status"); break;
            }
            last = cur;
        }
        sleep(1);
    }
    return NULL;
}

int wifi_init(const char *iface) {
    if (g.initialized) return 0;
    memset(&g, 0, sizeof(g));
    pthread_mutex_init(&g.mutex, NULL);
    strncpy(g.iface, iface ? iface : "wlan0", IFNAMSIZ - 1);
    g.status.state = WIFI_STATE_IDLE;
    g.running = 1;
    if (pthread_create(&g.monitor, NULL, monitor_thread, NULL) != 0) {
        pthread_mutex_destroy(&g.mutex);
        return -1;
    }
    g.initialized = 1;
    return 0;
}

int wifi_connect(const char *ssid, const char *password) {
    if (!g.initialized || !ssid || !password) return -1;
    if (write_conf(ssid, password) != 0) return -1;

    /* 停掉可能在运行的旧实例 */
    run_cmd("killall -q wpa_supplicant >/dev/null 2>&1");
    usleep(200000);
    run_cmd("mkdir -p %s", WIFI_CTRL_DIR);
    run_cmd("ip link set %s up >/dev/null 2>&1", g.iface);
    /* 后台启动 wpa_supplicant */
    int rc = run_cmd("wpa_supplicant -B -i %s -c %s -D nl80211,wext >/dev/null 2>&1",
                     g.iface, WIFI_CONF_PATH);
    if (rc != 0) {
        fprintf(stderr, "[wifi] wpa_supplicant start failed (rc=%d)\n", rc);
        publish_state(WIFI_STATE_FAILED, "wifi/failed");
        return -1;
    }
    pthread_mutex_lock(&g.mutex);
    strncpy(g.status.ssid, ssid, sizeof(g.status.ssid) - 1);
    g.status.state = WIFI_STATE_CONNECTING;
    pthread_mutex_unlock(&g.mutex);
    publish_state(WIFI_STATE_CONNECTING, "wifi/connecting");
    return 0;
}

int wifi_wait_connected(int timeout_ms) {
    int elapsed = 0;
    const int step = 200;
    while (timeout_ms < 0 || elapsed < timeout_ms) {
        pthread_mutex_lock(&g.mutex);
        wifi_state_t s = (wifi_state_t)g.status.state;
        pthread_mutex_unlock(&g.mutex);
        if (s == WIFI_STATE_CONNECTED) return 0;
        if (s == WIFI_STATE_FAILED)    return -1;
        usleep(step * 1000);
        elapsed += step;
    }
    return -2;
}

int wifi_disconnect(void) {
    if (!g.initialized) return -1;
    run_cmd("wpa_cli -i %s -p %s disconnect >/dev/null 2>&1", g.iface, WIFI_CTRL_DIR);
    return 0;
}

int wifi_get_status(wifi_status_t *out) {
    if (!out || !g.initialized) return -1;
    pthread_mutex_lock(&g.mutex);
    *out = g.status;
    pthread_mutex_unlock(&g.mutex);
    return 0;
}

void wifi_shutdown(void) {
    if (!g.initialized) return;
    g.running = 0;
    pthread_join(g.monitor, NULL);
    pthread_mutex_destroy(&g.mutex);
    g.initialized = 0;
}
