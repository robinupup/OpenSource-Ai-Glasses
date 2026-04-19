#ifndef MYAPP_BASE64_H
#define MYAPP_BASE64_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 返回 base64 编码长度（含终止 NUL）。 */
size_t base64_encoded_len(size_t binary_len);

/**
 * 把 binary 编码成 base64。out 需要至少 base64_encoded_len(len) 字节。
 * 写入一个 '\0' 终止符；返回 写入的有效字符数（不含 '\0'）。
 */
size_t base64_encode(const unsigned char *src, size_t len, char *out);

/**
 * 读取 path 指向的文件内容，并 base64 编码为 malloc 的字符串。
 * 调用方 free()。失败返回 NULL。
 */
char *base64_encode_file(const char *path);

#ifdef __cplusplus
}
#endif

#endif
