#include "photo_decode.h"

#include <math.h>
#include <stdlib.h>

#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#endif

namespace {

void* photoMalloc(size_t size) {
#ifdef ESP_PLATFORM
  return heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#else
  return malloc(size);
#endif
}

void* photoRealloc(void* ptr, size_t size) {
#ifdef ESP_PLATFORM
  return heap_caps_realloc(ptr, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#else
  return realloc(ptr, size);
#endif
}

void photoFree(void* ptr) {
#ifdef ESP_PLATFORM
  heap_caps_free(ptr);
#else
  free(ptr);
#endif
}

int clampIndex(int value, int maximum) {
  return value < 0 ? 0 : (value > maximum ? maximum : value);
}

}  // namespace

#define STBI_ONLY_JPEG
#define STBI_NO_STDIO
#define STBI_NO_SIMD
#define STBI_MALLOC(size) photoMalloc(size)
#define STBI_REALLOC(ptr, size) photoRealloc(ptr, size)
#define STBI_FREE(ptr) photoFree(ptr)
#define STB_IMAGE_IMPLEMENTATION
#include "stb/stb_image.h"

const char* photoDecodeError() { return stbi_failure_reason(); }

bool decodePhotoJpeg(const uint8_t* data, size_t size, uint16_t* output,
                     int outputWidth, int outputHeight, int* sourceWidth,
                     int* sourceHeight) {
  if (!data || !output || size == 0 || outputWidth <= 0 || outputHeight <= 0) return false;

  int width = 0;
  int height = 0;
  int channels = 0;
  stbi_uc* pixels = stbi_load_from_memory(data, static_cast<int>(size),
                                          &width, &height, &channels, 3);
  if (!pixels) return false;
  if (sourceWidth) *sourceWidth = width;
  if (sourceHeight) *sourceHeight = height;

  int cropWidth = width;
  int cropHeight = static_cast<int>(roundf(static_cast<float>(width) * outputHeight / outputWidth));
  if (cropHeight > height) {
    cropHeight = height;
    cropWidth = static_cast<int>(roundf(static_cast<float>(height) * outputWidth / outputHeight));
  }
  const int cropX = (width - cropWidth) / 2;
  const int cropY = (height - cropHeight) / 2;

  for (int y = 0; y < outputHeight; ++y) {
    const float sourceY = cropY + (y + 0.5f) * cropHeight / outputHeight - 0.5f;
    const int y0 = clampIndex(static_cast<int>(floorf(sourceY)), height - 1);
    const int y1 = clampIndex(y0 + 1, height - 1);
    const float fy = sourceY < 0 ? 0 : sourceY - floorf(sourceY);
    for (int x = 0; x < outputWidth; ++x) {
      const float sourceX = cropX + (x + 0.5f) * cropWidth / outputWidth - 0.5f;
      const int x0 = clampIndex(static_cast<int>(floorf(sourceX)), width - 1);
      const int x1 = clampIndex(x0 + 1, width - 1);
      const float fx = sourceX < 0 ? 0 : sourceX - floorf(sourceX);
      const size_t p00 = (static_cast<size_t>(y0) * width + x0) * 3;
      const size_t p10 = (static_cast<size_t>(y0) * width + x1) * 3;
      const size_t p01 = (static_cast<size_t>(y1) * width + x0) * 3;
      const size_t p11 = (static_cast<size_t>(y1) * width + x1) * 3;
      uint8_t rgb[3];
      for (int channel = 0; channel < 3; ++channel) {
        const float top = pixels[p00 + channel] * (1 - fx) + pixels[p10 + channel] * fx;
        const float bottom = pixels[p01 + channel] * (1 - fx) + pixels[p11 + channel] * fx;
        rgb[channel] = static_cast<uint8_t>(roundf(top * (1 - fy) + bottom * fy));
      }
      output[y * outputWidth + x] = static_cast<uint16_t>(((rgb[0] & 0xF8) << 8) |
                                                            ((rgb[1] & 0xFC) << 3) | (rgb[2] >> 3));
    }
  }

  stbi_image_free(pixels);
  return true;
}
