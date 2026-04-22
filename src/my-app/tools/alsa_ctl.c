/**
 * alsa_ctl - 极简 amixer / tinymix 替代
 *
 * 设备固件里没有 amixer / tinymix，但 /dev/snd/controlC0 一直都在。
 * 这个小工具直接通过 SNDRV_CTL_IOCTL_ELEM_* 系列 ioctl 枚举和设置控件，
 * 用来调 mic / codec 的增益。
 *
 * 用法：
 *   alsa_ctl list
 *       枚举所有控件（numid / 名称 / 类型 / 值范围 / 当前值）
 *   alsa_ctl get <numid>
 *       读一个控件的当前值
 *   alsa_ctl set <numid> <val1> [val2 ...]
 *       按 numid 设置（整数/布尔/枚举：写 long）
 *   alsa_ctl set-name "Control Name" <val1> [val2 ...]
 *       按名字设置
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sound/asound.h>

#define CTL_DEV "/dev/snd/controlC0"

static int open_ctl(void) {
    int fd = open(CTL_DEV, O_RDWR);
    if (fd < 0) {
        fprintf(stderr, "open %s: %s\n", CTL_DEV, strerror(errno));
        exit(1);
    }
    return fd;
}

static const char *type_name(int t) {
    switch (t) {
        case SNDRV_CTL_ELEM_TYPE_BOOLEAN:    return "BOOL";
        case SNDRV_CTL_ELEM_TYPE_INTEGER:    return "INT";
        case SNDRV_CTL_ELEM_TYPE_ENUMERATED: return "ENUM";
        case SNDRV_CTL_ELEM_TYPE_BYTES:      return "BYTES";
        case SNDRV_CTL_ELEM_TYPE_IEC958:     return "IEC958";
        case SNDRV_CTL_ELEM_TYPE_INTEGER64:  return "INT64";
        default:                             return "?";
    }
}

static int list_all(int fd) {
    struct snd_ctl_elem_list list;
    memset(&list, 0, sizeof(list));
    if (ioctl(fd, SNDRV_CTL_IOCTL_ELEM_LIST, &list) < 0) {
        perror("ELEM_LIST count");
        return -1;
    }
    int count = list.count;
    printf("total controls: %d\n", count);
    if (count <= 0) return 0;

    struct snd_ctl_elem_id *ids = calloc(count, sizeof(*ids));
    list.offset  = 0;
    list.space   = count;
    list.pids    = ids;
    if (ioctl(fd, SNDRV_CTL_IOCTL_ELEM_LIST, &list) < 0) {
        perror("ELEM_LIST fetch");
        free(ids); return -1;
    }

    for (unsigned int i = 0; i < list.used; i++) {
        struct snd_ctl_elem_info info;
        memset(&info, 0, sizeof(info));
        info.id = ids[i];
        if (ioctl(fd, SNDRV_CTL_IOCTL_ELEM_INFO, &info) < 0) continue;

        struct snd_ctl_elem_value val;
        memset(&val, 0, sizeof(val));
        val.id = ids[i];
        int have_val = (ioctl(fd, SNDRV_CTL_IOCTL_ELEM_READ, &val) == 0);

        printf("numid=%-4u name='%s' type=%s count=%u",
               ids[i].numid, ids[i].name, type_name(info.type),
               info.count);

        if (info.type == SNDRV_CTL_ELEM_TYPE_INTEGER) {
            printf(" range=[%ld..%ld] step=%ld",
                   info.value.integer.min,
                   info.value.integer.max,
                   info.value.integer.step);
            if (have_val) {
                printf(" cur=");
                for (unsigned int k = 0; k < info.count && k < 128; k++) {
                    printf("%s%ld", k ? "," : "", val.value.integer.value[k]);
                }
            }
        } else if (info.type == SNDRV_CTL_ELEM_TYPE_BOOLEAN) {
            if (have_val) {
                printf(" cur=");
                for (unsigned int k = 0; k < info.count && k < 128; k++) {
                    printf("%s%ld", k ? "," : "", val.value.integer.value[k]);
                }
            }
        } else if (info.type == SNDRV_CTL_ELEM_TYPE_ENUMERATED) {
            printf(" items=%u", info.value.enumerated.items);
            if (have_val) {
                printf(" cur=");
                for (unsigned int k = 0; k < info.count && k < 128; k++) {
                    printf("%s%u", k ? "," : "", val.value.enumerated.item[k]);
                }
            }
            /* 枚举项名 */
            printf(" {");
            for (unsigned int it = 0; it < info.value.enumerated.items; it++) {
                struct snd_ctl_elem_info qi;
                memset(&qi, 0, sizeof(qi));
                qi.id = ids[i];
                qi.value.enumerated.item = it;
                if (ioctl(fd, SNDRV_CTL_IOCTL_ELEM_INFO, &qi) == 0) {
                    printf("%s%u:%s", it ? "," : "", it,
                           qi.value.enumerated.name);
                }
            }
            printf("}");
        }
        printf("\n");
    }

    free(ids);
    return 0;
}

static int find_by_numid(int fd, unsigned int numid,
                         struct snd_ctl_elem_id *out_id,
                         struct snd_ctl_elem_info *out_info) {
    struct snd_ctl_elem_info info;
    memset(&info, 0, sizeof(info));
    info.id.numid = numid;
    if (ioctl(fd, SNDRV_CTL_IOCTL_ELEM_INFO, &info) < 0) {
        fprintf(stderr, "numid=%u INFO: %s\n", numid, strerror(errno));
        return -1;
    }
    *out_id   = info.id;
    *out_info = info;
    return 0;
}

static int find_by_name(int fd, const char *name,
                        struct snd_ctl_elem_id *out_id,
                        struct snd_ctl_elem_info *out_info) {
    struct snd_ctl_elem_info info;
    memset(&info, 0, sizeof(info));
    info.id.iface = SNDRV_CTL_ELEM_IFACE_MIXER;
    snprintf((char *)info.id.name, sizeof(info.id.name), "%s", name);
    if (ioctl(fd, SNDRV_CTL_IOCTL_ELEM_INFO, &info) < 0) {
        fprintf(stderr, "name='%s' INFO: %s\n", name, strerror(errno));
        return -1;
    }
    *out_id   = info.id;
    *out_info = info;
    return 0;
}

static int do_get(int fd, unsigned int numid) {
    struct snd_ctl_elem_id   id;
    struct snd_ctl_elem_info info;
    if (find_by_numid(fd, numid, &id, &info) != 0) return -1;
    struct snd_ctl_elem_value val;
    memset(&val, 0, sizeof(val));
    val.id = id;
    if (ioctl(fd, SNDRV_CTL_IOCTL_ELEM_READ, &val) < 0) {
        perror("ELEM_READ"); return -1;
    }
    printf("name='%s' type=%s count=%u value=",
           id.name, type_name(info.type), info.count);
    for (unsigned int k = 0; k < info.count && k < 128; k++) {
        if (info.type == SNDRV_CTL_ELEM_TYPE_ENUMERATED)
            printf("%s%u", k ? "," : "", val.value.enumerated.item[k]);
        else
            printf("%s%ld", k ? "," : "", val.value.integer.value[k]);
    }
    printf("\n");
    return 0;
}

static int do_set(int fd, struct snd_ctl_elem_id id,
                  struct snd_ctl_elem_info info,
                  int argc, char **argv) {
    struct snd_ctl_elem_value val;
    memset(&val, 0, sizeof(val));
    val.id = id;
    unsigned int n = info.count;
    for (unsigned int k = 0; k < n && k < 128; k++) {
        long v = (k < (unsigned)argc) ? strtol(argv[k], NULL, 0)
                                      : strtol(argv[argc - 1], NULL, 0);
        if (info.type == SNDRV_CTL_ELEM_TYPE_ENUMERATED) {
            val.value.enumerated.item[k] = (unsigned int)v;
        } else {
            val.value.integer.value[k] = v;
        }
    }
    if (ioctl(fd, SNDRV_CTL_IOCTL_ELEM_WRITE, &val) < 0) {
        perror("ELEM_WRITE"); return -1;
    }
    printf("OK: name='%s' set.\n", id.name);
    /* 回读一次确认 */
    memset(&val, 0, sizeof(val));
    val.id = id;
    if (ioctl(fd, SNDRV_CTL_IOCTL_ELEM_READ, &val) == 0) {
        printf("now: ");
        for (unsigned int k = 0; k < n && k < 128; k++) {
            if (info.type == SNDRV_CTL_ELEM_TYPE_ENUMERATED)
                printf("%s%u", k ? "," : "", val.value.enumerated.item[k]);
            else
                printf("%s%ld", k ? "," : "", val.value.integer.value[k]);
        }
        printf("\n");
    }
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr,
            "usage:\n"
            "  %s list\n"
            "  %s get <numid>\n"
            "  %s set <numid> <val1> [val2 ...]\n"
            "  %s set-name \"Control Name\" <val1> [val2 ...]\n",
            argv[0], argv[0], argv[0], argv[0]);
        return 1;
    }

    int fd = open_ctl();

    if (strcmp(argv[1], "list") == 0) {
        int rc = list_all(fd);
        close(fd); return rc;
    }
    if (strcmp(argv[1], "get") == 0 && argc == 3) {
        int rc = do_get(fd, (unsigned)strtoul(argv[2], NULL, 0));
        close(fd); return rc;
    }
    if (strcmp(argv[1], "set") == 0 && argc >= 4) {
        struct snd_ctl_elem_id id;
        struct snd_ctl_elem_info info;
        if (find_by_numid(fd, (unsigned)strtoul(argv[2], NULL, 0), &id, &info) != 0) {
            close(fd); return 1;
        }
        int rc = do_set(fd, id, info, argc - 3, argv + 3);
        close(fd); return rc;
    }
    if (strcmp(argv[1], "set-name") == 0 && argc >= 4) {
        struct snd_ctl_elem_id id;
        struct snd_ctl_elem_info info;
        if (find_by_name(fd, argv[2], &id, &info) != 0) {
            close(fd); return 1;
        }
        int rc = do_set(fd, id, info, argc - 3, argv + 3);
        close(fd); return rc;
    }

    fprintf(stderr, "bad args\n");
    close(fd);
    return 1;
}
