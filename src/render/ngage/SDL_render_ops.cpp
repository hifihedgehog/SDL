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

void ApplyColorMod(void *dest, void *source, int pitch, int width, int height, SDL_FColor color)
{
    TUint16 *src_pixels = static_cast<TUint16 *>(source);
    TUint16 *dst_pixels = static_cast<TUint16 *>(dest);

    TFixed rf = Real2Fix(color.r);
    TFixed gf = Real2Fix(color.g);
    TFixed bf = Real2Fix(color.b);

    // Pre-calculate pitch in pixels to avoid repeated division.
    const TInt pitchPixels = pitch >> 1;

    for (int y = 0; y < height; ++y) {
        // Calculate row offset once per row.
        TInt rowOffset = y * pitchPixels;

        for (int x = 0; x < width; ++x) {
            TUint16 pixel = src_pixels[rowOffset + x];
            TUint8 r = (pixel & 0xF800) >> 8;
            TUint8 g = (pixel & 0x07E0) >> 3;
            TUint8 b = (pixel & 0x001F) << 3;
            r = FixMul(r, rf);
            g = FixMul(g, gf);
            b = FixMul(b, bf);
            dst_pixels[rowOffset + x] = (r << 8) | (g << 3) | (b >> 3);
        }
    }
}

void ApplyFlip(void *dest, void *source, int pitch, int width, int height, SDL_FlipMode flip)
{
    TUint16 *src_pixels = static_cast<TUint16 *>(source);
    TUint16 *dst_pixels = static_cast<TUint16 *>(dest);

    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            int src_x = x;
            int src_y = y;

            if (flip & SDL_FLIP_HORIZONTAL) {
                src_x = width - 1 - x;
            }

            if (flip & SDL_FLIP_VERTICAL) {
                src_y = height - 1 - y;
            }

            dst_pixels[y * pitch / 2 + x] = src_pixels[src_y * pitch / 2 + src_x];
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

    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            // Translate point to origin.
            TFixed translated_x = Int2Fix(x) - center_x;
            TFixed translated_y = Int2Fix(y) - center_y;

            // Scale point.
            TFixed scaled_x = FixDiv(translated_x, scale_x);
            TFixed scaled_y = FixDiv(translated_y, scale_y);

            // Translate point back.
            int final_x = Fix2Int(scaled_x + center_x);
            int final_y = Fix2Int(scaled_y + center_y);

            // Check bounds.
            if (final_x >= 0 && final_x < width && final_y >= 0 && final_y < height) {
                dst_pixels[y * pitch / 2 + x] = src_pixels[final_y * pitch / 2 + final_x];
            } else {
                dst_pixels[y * pitch / 2 + x] = 0;
            }
        }
    }
}
