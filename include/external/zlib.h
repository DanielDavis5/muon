#ifndef MUON_EXTERNAL_ZLIB_H
#define MUON_EXTERNAL_ZLIB_H

#include <stdbool.h>
#include <stdint.h>

bool muon_zlib_extract(uint8_t *data, uint64_t len, const char *destdir);
#endif