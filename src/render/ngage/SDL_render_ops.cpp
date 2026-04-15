/*
  Simple DirectMedia Layer
  Copyright (C) 1997-2026 Sam Lantinga <slouken@libsdl.org>

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely, subject to the following restrictions:

  1. The origin of this software must not be misrepresented; you must not
     claim that you wrote the original software. If you use this software
     in a product, an acknowledgment in the product documentation would be
     appreciated but is not required.
  2. Altered source versions must be plainly marked as such, and must not be
     misrepresented as being the original software.
  3. This notice may not be removed or altered from any source distribution.
*/
#include "SDL_internal.h"

#include "SDL_render_ops.hpp"
#include <3dtypes.h>

void ApplyColorMod(void *dest, void *source, int pitch, int width, int height, SDL_FColor color, const TUint8 *colorLUT)
{
    TUint16 *src_pixels = static_cast<TUint16 *>(source);
    TUint16 *dst_pixels = static_cast<TUint16 *>(dest);

    // Pre-calculate pitch in pixels to avoid repeated division.
    const TInt pitchPixels = pitch >> 1;
    const int totalPixels = width * height;

    // Process 4 pixels at a time (loop unrolling).
    int pixelIndex = 0;
    for (int y = 0; y < height; ++y) {
        TInt rowOffset = y * pitchPixels;
        int x = 0;

        // Unrolled loop: process 4 pixels at once.
        for (; x < width - 3; x += 4) {
            // Pixel 0
            TUint16 p0 = src_pixels[rowOffset + x];
            TUint8 r0 = colorLUT[(p0 & 0xF800) >> 8];
            TUint8 g0 = colorLUT[256 + ((p0 & 0x07E0) >> 3)];
            TUint8 b0 = colorLUT[512 + ((p0 & 0x001F) << 3)];
            dst_pixels[rowOffset + x] = (r0 << 8) | (g0 << 3) | (b0 >> 3);

            // Pixel 1
            TUint16 p1 = src_pixels[rowOffset + x + 1];
            TUint8 r1 = colorLUT[(p1 & 0xF800) >> 8];
            TUint8 g1 = colorLUT[256 + ((p1 & 0x07E0) >> 3)];
            TUint8 b1 = colorLUT[512 + ((p1 & 0x001F) << 3)];
            dst_pixels[rowOffset + x + 1] = (r1 << 8) | (g1 << 3) | (b1 >> 3);

            // Pixel 2
            TUint16 p2 = src_pixels[rowOffset + x + 2];
            TUint8 r2 = colorLUT[(p2 & 0xF800) >> 8];
            TUint8 g2 = colorLUT[256 + ((p2 & 0x07E0) >> 3)];
            TUint8 b2 = colorLUT[512 + ((p2 & 0x001F) << 3)];
            dst_pixels[rowOffset + x + 2] = (r2 << 8) | (g2 << 3) | (b2 >> 3);

            // Pixel 3
            TUint16 p3 = src_pixels[rowOffset + x + 3];
            TUint8 r3 = colorLUT[(p3 & 0xF800) >> 8];
            TUint8 g3 = colorLUT[256 + ((p3 & 0x07E0) >> 3)];
            TUint8 b3 = colorLUT[512 + ((p3 & 0x001F) << 3)];
            dst_pixels[rowOffset + x + 3] = (r3 << 8) | (g3 << 3) | (b3 >> 3);
        }

        // Handle remaining pixels.
        for (; x < width; ++x) {
            TUint16 pixel = src_pixels[rowOffset + x];
            TUint8 r = colorLUT[(pixel & 0xF800) >> 8];
            TUint8 g = colorLUT[256 + ((pixel & 0x07E0) >> 3)];
            TUint8 b = colorLUT[512 + ((pixel & 0x001F) << 3)];
            dst_pixels[rowOffset + x] = (r << 8) | (g << 3) | (b >> 3);
        }
    }
}

void ApplyFlip(void *dest, void *source, int pitch, int width, int height, SDL_FlipMode flip)
{
    TUint16 *src_pixels = static_cast<TUint16 *>(source);
    TUint16 *dst_pixels = static_cast<TUint16 *>(dest);

    // Pre-calculate pitch in pixels to avoid repeated division.
    const TInt pitchPixels = pitch >> 1;

    // Pre-calculate flip flags to avoid repeated bitwise operations.
    const bool flipHorizontal = (flip & SDL_FLIP_HORIZONTAL) != 0;
    const bool flipVertical = (flip & SDL_FLIP_VERTICAL) != 0;

    for (int y = 0; y < height; ++y) {
        // Calculate destination row offset once per row.
        TInt dstRowOffset = y * pitchPixels;

        // Calculate source Y coordinate once per row.
        int src_y = flipVertical ? (height - 1 - y) : y;
        TInt srcRowOffset = src_y * pitchPixels;

        int x = 0;

        // Unrolled loop: process 4 pixels at once.
        for (; x < width - 3; x += 4) {
            int src_x0 = flipHorizontal ? (width - 1 - x) : x;
            int src_x1 = flipHorizontal ? (width - 2 - x) : (x + 1);
            int src_x2 = flipHorizontal ? (width - 3 - x) : (x + 2);
            int src_x3 = flipHorizontal ? (width - 4 - x) : (x + 3);

            dst_pixels[dstRowOffset + x] = src_pixels[srcRowOffset + src_x0];
            dst_pixels[dstRowOffset + x + 1] = src_pixels[srcRowOffset + src_x1];
            dst_pixels[dstRowOffset + x + 2] = src_pixels[srcRowOffset + src_x2];
            dst_pixels[dstRowOffset + x + 3] = src_pixels[srcRowOffset + src_x3];
        }

        // Handle remaining pixels.
        for (; x < width; ++x) {
            int src_x = flipHorizontal ? (width - 1 - x) : x;
            dst_pixels[dstRowOffset + x] = src_pixels[srcRowOffset + src_x];
        }
    }
}

void ApplyRotation(void *dest, void *source, int pitch, int width, int height, TFixed center_x, TFixed center_y, TFixed angle)
{
    TUint16 *src_pixels = static_cast<TUint16 *>(source);
    TUint16 *dst_pixels = static_cast<TUint16 *>(dest);

    TFixed cos_angle = 0;
    TFixed sin_angle = 0;

    if (angle != 0) {
        FixSinCos(angle, sin_angle, cos_angle);
    }

    // Pre-calculate pitch in pixels to avoid repeated division.
    const TInt pitchPixels = pitch >> 1;

    // Incremental DDA: Calculate per-pixel increments.
    // As we move right (x+1), the rotated position changes by (cos, -sin).
    TFixed dx_cos = cos_angle;
    TFixed dx_sin = -sin_angle;

    for (int y = 0; y < height; ++y) {
        // Calculate destination row offset once per row.
        TInt dstRowOffset = y * pitchPixels;

        // Calculate starting position for this row.
        TFixed translated_y = Int2Fix(y) - center_y;
        TFixed row_start_x = FixMul(translated_y, sin_angle) + center_x;
        TFixed row_start_y = FixMul(translated_y, cos_angle) + center_y;

        // For first pixel in row, account for x=0 translation.
        TFixed src_x = row_start_x - FixMul(center_x, cos_angle);
        TFixed src_y = row_start_y + FixMul(center_x, sin_angle);

        for (int x = 0; x < width; ++x) {
            // Convert to integer coordinates.
            int final_x = Fix2Int(src_x);
            int final_y = Fix2Int(src_y);

            // Check bounds.
            if (final_x >= 0 && final_x < width && final_y >= 0 && final_y < height) {
                dst_pixels[dstRowOffset + x] = src_pixels[final_y * pitchPixels + final_x];
            } else {
                dst_pixels[dstRowOffset + x] = 0;
            }

            // Incremental step: move to next pixel (just additions, no multiplications!).
            src_x += dx_cos;
            src_y += dx_sin;
        }
    }
}

void ApplyScale(void *dest, void *source, int pitch, int width, int height, TFixed center_x, TFixed center_y, TFixed scale_x, TFixed scale_y)
{
    TUint16 *src_pixels = static_cast<TUint16 *>(source);
    TUint16 *dst_pixels = static_cast<TUint16 *>(dest);

    // Pre-calculate pitch in pixels to avoid repeated division.
    const TInt pitchPixels = pitch >> 1;

    for (int y = 0; y < height; ++y) {
        // Calculate destination row offset once per row.
        TInt dstRowOffset = y * pitchPixels;

        // Pre-calculate translated_y for the entire row.
        TFixed translated_y = Int2Fix(y) - center_y;
        TFixed scaled_y = FixDiv(translated_y, scale_y);
        int final_y = Fix2Int(scaled_y + center_y);

        // Check if this row is within bounds.
        bool rowInBounds = (final_y >= 0 && final_y < height);
        TInt srcRowOffset = final_y * pitchPixels;

        int x = 0;

        // Unrolled loop: process 4 pixels at once.
        for (; x < width - 3; x += 4) {
            // Pixel 0
            TFixed translated_x0 = Int2Fix(x) - center_x;
            TFixed scaled_x0 = FixDiv(translated_x0, scale_x);
            int final_x0 = Fix2Int(scaled_x0 + center_x);

            // Pixel 1
            TFixed translated_x1 = Int2Fix(x + 1) - center_x;
            TFixed scaled_x1 = FixDiv(translated_x1, scale_x);
            int final_x1 = Fix2Int(scaled_x1 + center_x);

            // Pixel 2
            TFixed translated_x2 = Int2Fix(x + 2) - center_x;
            TFixed scaled_x2 = FixDiv(translated_x2, scale_x);
            int final_x2 = Fix2Int(scaled_x2 + center_x);

            // Pixel 3
            TFixed translated_x3 = Int2Fix(x + 3) - center_x;
            TFixed scaled_x3 = FixDiv(translated_x3, scale_x);
            int final_x3 = Fix2Int(scaled_x3 + center_x);

            // Write all 4 pixels
            dst_pixels[dstRowOffset + x] = (rowInBounds && final_x0 >= 0 && final_x0 < width) ?
                src_pixels[srcRowOffset + final_x0] : 0;
            dst_pixels[dstRowOffset + x + 1] = (rowInBounds && final_x1 >= 0 && final_x1 < width) ?
                src_pixels[srcRowOffset + final_x1] : 0;
            dst_pixels[dstRowOffset + x + 2] = (rowInBounds && final_x2 >= 0 && final_x2 < width) ?
                src_pixels[srcRowOffset + final_x2] : 0;
            dst_pixels[dstRowOffset + x + 3] = (rowInBounds && final_x3 >= 0 && final_x3 < width) ?
                src_pixels[srcRowOffset + final_x3] : 0;
        }

        // Handle remaining pixels.
        for (; x < width; ++x) {
            TFixed translated_x = Int2Fix(x) - center_x;
            TFixed scaled_x = FixDiv(translated_x, scale_x);
            int final_x = Fix2Int(scaled_x + center_x);

            if (rowInBounds && final_x >= 0 && final_x < width) {
                dst_pixels[dstRowOffset + x] = src_pixels[srcRowOffset + final_x];
            } else {
                dst_pixels[dstRowOffset + x] = 0;
            }
        }
    }
}
