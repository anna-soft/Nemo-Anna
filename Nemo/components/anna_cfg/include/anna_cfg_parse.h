#pragma once
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

int anna_cfg_parse_json(const char *js, size_t len);

#ifdef __cplusplus
}
#endif