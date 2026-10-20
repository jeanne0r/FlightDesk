#pragma once

#include <stddef.h>
#include <stdint.h>

bool decodePhotoJpeg(const uint8_t* data, size_t size, uint16_t* output,
                     int outputWidth, int outputHeight, int* sourceWidth,
                     int* sourceHeight);
const char* photoDecodeError();
