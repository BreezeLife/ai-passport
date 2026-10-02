#pragma once
#include "dino_model.h"
typedef struct {
    const char *name, *latin, *period, *diet, *trait;
    const char *facts[2];
    const char *question, *options[2], *hint;
    unsigned char correct;
} dino_entry_t;
extern const dino_entry_t dino_catalog[DINO_COUNT];
