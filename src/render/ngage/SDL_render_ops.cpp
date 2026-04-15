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

    // Pre-calculate LUT offsets to reduce addressing calculations.
    const TUint8 *lut_r = colorLUT;
    const TUint8 *lut_g = colorLUT + 256;
    const TUint8 *lut_b = colorLUT + 512;

    // Process 4 pixels at a time (loop unrolling).
    for (int y = 0; y < height; ++y) {
        const TInt rowOffset = y * pitchPixels;
        int x = 0;

        // Unrolled loop: process 4 pixels at once with optimized bit manipulation.
        for (; x < width - 3; x += 4) {
            // Load 4 pixels at once.
            TUint16 p0 = src_pixels[rowOffset + x];
            TUint16 p1 = src_pixels[rowOffset + x + 1];
            TUint16 p2 = src_pixels[rowOffset + x + 2];
            TUint16 p3 = src_pixels[rowOffset + x + 3];

            // Pixel 0: Extract and modulate RGB4444 components.
            // RGB4444 format: RRRR GGGG BBBB xxxx
            TUint8 r0 = lut_r[(p0 >> 8) & 0xF0];  // Extract R (bits 12-15), shift to byte position
            TUint8 g0 = lut_g[(p0 >> 3) & 0xF8];  // Extract G (bits 6-9), scale to 8-bit
            TUint8 b0 = lut_b[(p0 << 3) & 0xF8];  // Extract B (bits 0-3), scale to 8-bit
            dst_pixels[rowOffset + x] = ((r0 & 0xF0) << 8) | ((g0 & 0xF0) << 3) | ((b0 & 0xF0) >> 1);

            // Pixel 1
            TUint8 r1 = lut_r[(p1 >> 8) & 0xF0];
            TUint8 g1 = lut_g[(p1 >> 3) & 0xF8];
            TUint8 b1 = lut_b[(p1 << 3) & 0xF8];
            dst_pixels[rowOffset + x + 1] = ((r1 & 0xF0) << 8) | ((g1 & 0xF0) << 3) | ((b1 & 0xF0) >> 1);

            // Pixel 2
            TUint8 r2 = lut_r[(p2 >> 8) & 0xF0];
            TUint8 g2 = lut_g[(p2 >> 3) & 0xF8];
            TUint8 b2 = lut_b[(p2 << 3) & 0xF8];
            dst_pixels[rowOffset + x + 2] = ((r2 & 0xF0) << 8) | ((g2 & 0xF0) << 3) | ((b2 & 0xF0) >> 1);

            // Pixel 3
            TUint8 r3 = lut_r[(p3 >> 8) & 0xF0];
            TUint8 g3 = lut_g[(p3 >> 3) & 0xF8];
            TUint8 b3 = lut_b[(p3 << 3) & 0xF8];
            dst_pixels[rowOffset + x + 3] = ((r3 & 0xF0) << 8) | ((g3 & 0xF0) << 3) | ((b3 & 0xF0) >> 1);
        }

        // Handle remaining pixels.
        for (; x < width; ++x) {
            TUint16 pixel = src_pixels[rowOffset + x];
            TUint8 r = lut_r[(pixel >> 8) & 0xF0];
            TUint8 g = lut_g[(pixel >> 3) & 0xF8];
            TUint8 b = lut_b[(pixel << 3) & 0xF8];
            dst_pixels[rowOffset + x] = ((r & 0xF0) << 8) | ((g & 0xF0) << 3) | ((b & 0xF0) >> 1);
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

    // Pre-calculate width/height bounds for horizontal/vertical flipping.
    const int width_m1 = width - 1;
    const int height_m1 = height - 1;

    for (int y = 0; y < height; ++y) {
        // Calculate destination row offset once per row.
        const TInt dstRowOffset = y * pitchPixels;

        // Calculate source Y coordinate once per row.
        const int src_y = flipVertical ? (height_m1 - y) : y;
        const TInt srcRowOffset = src_y * pitchPixels;

        int x = 0;

        // Unrolled loop: process 4 pixels at once.
        for (; x < width - 3; x += 4) {
            if (flipHorizontal) {
                dst_pixels[dstRowOffset + x] = src_pixels[srcRowOffset + (width_m1 - x)];
                dst_pixels[dstRowOffset + x + 1] = src_pixels[srcRowOffset + (width_m1 - x - 1)];
                dst_pixels[dstRowOffset + x + 2] = src_pixels[srcRowOffset + (width_m1 - x - 2)];
                dst_pixels[dstRowOffset + x + 3] = src_pixels[srcRowOffset + (width_m1 - x - 3)];
            } else {
                dst_pixels[dstRowOffset + x] = src_pixels[srcRowOffset + x];
                dst_pixels[dstRowOffset + x + 1] = src_pixels[srcRowOffset + x + 1];
                dst_pixels[dstRowOffset + x + 2] = src_pixels[srcRowOffset + x + 2];
                dst_pixels[dstRowOffset + x + 3] = src_pixels[srcRowOffset + x + 3];
            }
        }

        // Handle remaining pixels.
        for (; x < width; ++x) {
            const int src_x = flipHorizontal ? (width_m1 - x) : x;
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
    const TFixed dx_cos = cos_angle;
    const TFixed dx_sin = -sin_angle;

    for (int y = 0; y < height; ++y) {
        // Calculate destination row offset once per row.
        const TInt dstRowOffset = y * pitchPixels;

        // Calculate starting position for this row.
        // For y, rotation transforms: x' = x*cos - y*sin, y' = x*sin + y*cos
        // At x=0: x' = -y*sin, y' = y*cos (relative to center)
        const TFixed translated_y = Int2Fix(y) - center_y;
        const TFixed row_start_x = center_x - FixMul(translated_y, sin_angle);
        const TFixed row_start_y = center_y + FixMul(translated_y, cos_angle);

        // Start at x=0 position.
        TFixed src_x = row_start_x;
        TFixed src_y = row_start_y;

        int x = 0;

        // Unrolled loop: process 4 pixels at once.
        for (; x < width - 3; x += 4) {
            // Pixel 0
            int final_x0 = Fix2Int(src_x);
            int final_y0 = Fix2Int(src_y);
            src_x += dx_cos;
            src_y += dx_sin;

            // Pixel 1
            int final_x1 = Fix2Int(src_x);
            int final_y1 = Fix2Int(src_y);
            src_x += dx_cos;
            src_y += dx_sin;

            // Pixel 2
            int final_x2 = Fix2Int(src_x);
            int final_y2 = Fix2Int(src_y);
            src_x += dx_cos;
            src_y += dx_sin;

            // Pixel 3
            int final_x3 = Fix2Int(src_x);
            int final_y3 = Fix2Int(src_y);
            src_x += dx_cos;
            src_y += dx_sin;

            // Write all 4 pixels with bounds checking.
            dst_pixels[dstRowOffset + x] = (final_x0 >= 0 && final_x0 < width && final_y0 >= 0 && final_y0 < height) ?
                src_pixels[final_y0 * pitchPixels + final_x0] : 0;
            dst_pixels[dstRowOffset + x + 1] = (final_x1 >= 0 && final_x1 < width && final_y1 >= 0 && final_y1 < height) ?
                src_pixels[final_y1 * pitchPixels + final_x1] : 0;
            dst_pixels[dstRowOffset + x + 2] = (final_x2 >= 0 && final_x2 < width && final_y2 >= 0 && final_y2 < height) ?
                src_pixels[final_y2 * pitchPixels + final_x2] : 0;
            dst_pixels[dstRowOffset + x + 3] = (final_x3 >= 0 && final_x3 < width && final_y3 >= 0 && final_y3 < height) ?
                src_pixels[final_y3 * pitchPixels + final_x3] : 0;
        }

        // Handle remaining pixels.
        for (; x < width; ++x) {
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

    // Pre-calculate inverse scale factors to use FixMul instead of FixDiv.
    // This is MUCH faster on N-Gage hardware (no division per pixel!).
    TFixed inv_scale_x = FixDiv(Int2Fix(1), scale_x);
    TFixed inv_scale_y = FixDiv(Int2Fix(1), scale_y);

    // Pre-calculate center offset to reduce operations per pixel.
    TFixed center_x_fixed = center_x;
    TFixed center_y_fixed = center_y;

    for (int y = 0; y < height; ++y) {
        // Calculate destination row offset once per row.
        TInt dstRowOffset = y * pitchPixels;

        // Use inverse scale factor (multiply instead of divide).
        TFixed translated_y = Int2Fix(y) - center_y_fixed;
        TFixed scaled_y = FixMul(translated_y, inv_scale_y);
        int final_y = Fix2Int(scaled_y + center_y_fixed);

        // Check if this row is within bounds.
        bool rowInBounds = (final_y >= 0 && final_y < height);
        TInt srcRowOffset = final_y * pitchPixels;

        // Incremental DDA for X: pre-calculate starting position and increment.
        TFixed src_x_start = FixMul(-center_x_fixed, inv_scale_x) + center_x_fixed;
        TFixed src_x = src_x_start;

        int x = 0;

        // Unrolled loop: process 4 pixels at once.
        for (; x < width - 3; x += 4) {
            // Process 4 pixels using incremental approach.
            int final_x0 = Fix2Int(src_x);
            src_x += inv_scale_x;
            int final_x1 = Fix2Int(src_x);
            src_x += inv_scale_x;
            int final_x2 = Fix2Int(src_x);
            src_x += inv_scale_x;
            int final_x3 = Fix2Int(src_x);
            src_x += inv_scale_x;

            // Write all 4 pixels with bounds checking.
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
            int final_x = Fix2Int(src_x);
            src_x += inv_scale_x;

            if (rowInBounds && final_x >= 0 && final_x < width) {
                dst_pixels[dstRowOffset + x] = src_pixels[srcRowOffset + final_x];
            } else {
                dst_pixels[dstRowOffset + x] = 0;
            }
        }
    }
}
