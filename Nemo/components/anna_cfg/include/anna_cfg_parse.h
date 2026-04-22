#pragma once
#include <stddef.h>
#include "anna_cfg.h"

#ifdef __cplusplus
extern "C" {
#endif

int anna_cfg_parse_json(const char *js, size_t len);
int anna_cfg_parse_json_into(const char *js, size_t len, anna_cfg_t *out_cfg);

#ifdef __cplusplus
}
#endif
