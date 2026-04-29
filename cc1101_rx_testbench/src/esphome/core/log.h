#pragma once
#include <cstdio>

#ifndef ESP_LOGE
#define ESP_LOGE(tag, fmt, ...) std::fprintf(stderr, "[E][%s] " fmt "\n", tag, ##__VA_ARGS__)
#endif
#ifndef ESP_LOGW
#define ESP_LOGW(tag, fmt, ...) std::fprintf(stderr, "[W][%s] " fmt "\n", tag, ##__VA_ARGS__)
#endif
#ifndef ESP_LOGI
#define ESP_LOGI(tag, fmt, ...) std::fprintf(stdout, "[I][%s] " fmt "\n", tag, ##__VA_ARGS__)
#endif
#ifndef ESP_LOGD
#define ESP_LOGD(tag, fmt, ...) std::fprintf(stdout, "[D][%s] " fmt "\n", tag, ##__VA_ARGS__)
#endif
#ifndef ESP_LOGV
#define ESP_LOGV(tag, fmt, ...) do {} while (0)
#endif
