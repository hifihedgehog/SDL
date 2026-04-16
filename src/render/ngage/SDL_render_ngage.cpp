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
#ifdef __cplusplus
extern "C" {
#endif

#include "../../events/SDL_keyboard_c.h"
#include "../SDL_sysrender.h"
#include "SDL_internal.h"
#include "SDL_render_ngage_c.h"

#ifdef __cplusplus
}
#endif

#ifdef SDL_VIDEO_RENDER_NGAGE

#include "SDL_render_ngage_c.hpp"
#include "SDL_render_ops.hpp"

const TUint32 WindowClientHandle = 0x571D0A;

extern CRenderer *gRenderer;

#ifdef __cplusplus
extern "C" {
#endif

void NGAGE_Clear(const Uint32 color)
{
    gRenderer->Clear(color);
}

bool NGAGE_Copy(SDL_Renderer *renderer, SDL_Texture *texture, SDL_Rect *srcrect, SDL_Rect *dstrect)
{
    return gRenderer->Copy(renderer, texture, srcrect, dstrect);
}

bool NGAGE_CopyEx(SDL_Renderer *renderer, SDL_Texture *texture, NGAGE_CopyExData *copydata)
{
    return gRenderer->CopyEx(renderer, texture, copydata);
}

bool NGAGE_CreateTextureData(NGAGE_TextureData *data, const int width, const int height)
{
    return gRenderer->CreateTextureData(data, width, height);
}

void NGAGE_DestroyTextureData(NGAGE_TextureData *data)
{
    if (data) {
        delete data->bitmap;
        data->bitmap = NULL;

        // Free cardinal rotation cache.
        for (int i = 0; i < 4; i++) {
            if (data->cardinalRotations[i]) {
                delete data->cardinalRotations[i];
                data->cardinalRotations[i] = NULL;
            }
        }
    }
}

void *NGAGE_GetBitmapDataAddress(NGAGE_TextureData *data)
{
    if (data) {
        return data->bitmap->DataAddress();
    }
    return NULL;
}

int NGAGE_GetBitmapPitch(NGAGE_TextureData *data)
{
    if (data) {
        return data->cachedPitch;
    }
    return 0;
}

int NGAGE_GetBitmapWidth(NGAGE_TextureData *data)
{
    if (data) {
        return data->cachedWidth;
    }
    return 0;
}

int NGAGE_GetBitmapHeight(NGAGE_TextureData *data)
{
    if (data) {
        return data->cachedHeight;
    }
    return 0;
}

void NGAGE_DrawLines(NGAGE_Vertex *verts, const int count)
{
    gRenderer->DrawLines(verts, count);
}

void NGAGE_DrawPoints(NGAGE_Vertex *verts, const int count)
{
    gRenderer->DrawPoints(verts, count);
}

void NGAGE_FillRects(NGAGE_Vertex *verts, const int count)
{
    gRenderer->FillRects(verts, count);
}

void NGAGE_Flip()
{
    gRenderer->Flip();
}

void NGAGE_SetClipRect(const SDL_Rect *rect)
{
    gRenderer->SetClipRect(rect->x, rect->y, rect->w, rect->h);
}

void NGAGE_SetDrawColor(const Uint32 color)
{
    gRenderer->SetDrawColor(color);
}

void NGAGE_PumpEventsInternal()
{
    gRenderer->PumpEvents();
}

void NGAGE_SuspendScreenSaverInternal(bool suspend)
{
    gRenderer->SuspendScreenSaver(suspend);
}

#ifdef __cplusplus
}
#endif

// Pre-calculated fixed-point angle constants for cardinal rotation checks.
// These avoid repeated Real2Fix conversions in CopyEx hot path.
static const TFixed kAngleZero = 0;
static const TFixed kAnglePi_2 = Real2Fix(M_PI / 2.0);        // 90 degrees
static const TFixed kAnglePi = Real2Fix(M_PI);                // 180 degrees
static const TFixed kAnglePi3_2 = Real2Fix(3.0 * M_PI / 2.0); // 270 degrees
static const TFixed kAnglePi2 = Real2Fix(2.0 * M_PI);         // 360 degrees
static const TFixed kAngleTolerance = 100;                    // Tolerance for angle comparison

CRenderer *CRenderer::NewL()
{
    CRenderer *self = new (ELeave) CRenderer();
    CleanupStack::PushL(self);
    self->ConstructL();
    CleanupStack::Pop(self);
    return self;
}

CRenderer::CRenderer() : iRenderer(0), iDirectScreen(0), iScreenGc(0), iWsSession(), iWsWindowGroup(), iWsWindowGroupID(0), iWsWindow(), iWsScreen(0), iWsEventStatus(), iWsEvent(), iShowFPS(EFalse), iFPS(0), iFont(0), iWorkBuffer1(0), iWorkBuffer2(0), iWorkBufferSize(0), iTempRenderBitmap(0), iTempRenderBitmapWidth(0), iTempRenderBitmapHeight(0), iLastColorR(-1), iLastColorG(-1), iLastColorB(-1), iLinePointsBuffer(0), iLinePointsBufferCapacity(0), iLastDrawColor(0)
{
}

CRenderer::~CRenderer()
{
    delete iRenderer;
    iRenderer = 0;

    // Free work buffers.
    SDL_free(iWorkBuffer1);
    SDL_free(iWorkBuffer2);
    iWorkBuffer1 = 0;
    iWorkBuffer2 = 0;
    iWorkBufferSize = 0;

    // Free temp render bitmap.
    delete iTempRenderBitmap;
    iTempRenderBitmap = 0;
    iTempRenderBitmapWidth = 0;
    iTempRenderBitmapHeight = 0;

    // Free line points buffer.
    delete[] iLinePointsBuffer;
    iLinePointsBuffer = 0;
    iLinePointsBufferCapacity = 0;
}

void CRenderer::ConstructL()
{
    TInt error = KErrNone;

    error = iWsSession.Connect();
    if (error != KErrNone) {
        SDL_Log("Failed to connect to window server: %d", error);
        User::Leave(error);
    }

    iWsScreen = new (ELeave) CWsScreenDevice(iWsSession);
    error = iWsScreen->Construct();
    if (error != KErrNone) {
        SDL_Log("Failed to construct screen device: %d", error);
        User::Leave(error);
    }

    iWsWindowGroup = RWindowGroup(iWsSession);
    error = iWsWindowGroup.Construct(WindowClientHandle);
    if (error != KErrNone) {
        SDL_Log("Failed to construct window group: %d", error);
        User::Leave(error);
    }
    iWsWindowGroup.SetOrdinalPosition(0);

    RProcess thisProcess;
    TParse exeName;
    exeName.Set(thisProcess.FileName(), NULL, NULL);
    TBuf<32> winGroupName;
    winGroupName.Append(0);
    winGroupName.Append(0);
    winGroupName.Append(0); // UID
    winGroupName.Append(0);
    winGroupName.Append(exeName.Name()); // Caption
    winGroupName.Append(0);
    winGroupName.Append(0); // DOC name
    iWsWindowGroup.SetName(winGroupName);

    iWsWindow = RWindow(iWsSession);
    error = iWsWindow.Construct(iWsWindowGroup, WindowClientHandle - 1);
    if (error != KErrNone) {
        SDL_Log("Failed to construct window: %d", error);
        User::Leave(error);
    }
    iWsWindow.SetBackgroundColor(KRgbWhite);
    iWsWindow.SetRequiredDisplayMode(EColor4K);
    iWsWindow.Activate();
    iWsWindow.SetSize(iWsScreen->SizeInPixels());
    iWsWindow.SetVisible(ETrue);

    iWsWindowGroupID = iWsWindowGroup.Identifier();

    TRAPD(errc, iRenderer = iRenderer->NewL());
    if (errc != KErrNone) {
        SDL_Log("Failed to create renderer: %d", errc);
        return;
    }

    iDirectScreen = CDirectScreenAccess::NewL(
        iWsSession,
        *(iWsScreen),
        iWsWindow, *this);

    // Select font.
    TFontSpec fontSpec(_L("LatinBold12"), 12);
    TInt errd = iWsScreen->GetNearestFontInTwips((CFont *&)iFont, fontSpec);
    if (errd != KErrNone) {
        SDL_Log("Failed to get font: %d", errd);
        return;
    }

    // Activate events.
    iWsEventStatus = KRequestPending;
    iWsSession.EventReady(&iWsEventStatus);

    DisableKeyBlocking();

    iIsFocused = ETrue;
    iShowFPS = EFalse;
    iSuspendScreenSaver = EFalse;

    if (!iDirectScreen->IsActive()) {
        TRAPD(err, iDirectScreen->StartL());
        if (KErrNone != err) {
            return;
        }
        iDirectScreen->ScreenDevice()->SetAutoUpdate(ETrue);
    }
}

void CRenderer::Restart(RDirectScreenAccess::TTerminationReasons aReason)
{
    if (!iDirectScreen->IsActive()) {
        TRAPD(err, iDirectScreen->StartL());
        if (KErrNone != err) {
            return;
        }
        iDirectScreen->ScreenDevice()->SetAutoUpdate(ETrue);
    }
}

void CRenderer::AbortNow(RDirectScreenAccess::TTerminationReasons aReason)
{
    if (iDirectScreen->IsActive()) {
        iDirectScreen->Cancel();
    }
}

void CRenderer::Clear(TUint32 iColor)
{
    if (iRenderer && iRenderer->Gc()) {
        iRenderer->Gc()->SetBrushColor(iColor);
        iRenderer->Gc()->Clear();
    }
}

bool CRenderer::EnsureWorkBufferCapacity(TInt aRequiredSize)
{
    if (aRequiredSize <= iWorkBufferSize) {
        return true;
    }

    // Free old buffers.
    SDL_free(iWorkBuffer1);
    SDL_free(iWorkBuffer2);

    // Allocate new buffers.
    iWorkBuffer1 = SDL_calloc(1, aRequiredSize);
    if (!iWorkBuffer1) {
        iWorkBuffer2 = 0;
        iWorkBufferSize = 0;
        return false;
    }

    iWorkBuffer2 = SDL_calloc(1, aRequiredSize);
    if (!iWorkBuffer2) {
        SDL_free(iWorkBuffer1);
        iWorkBuffer1 = 0;
        iWorkBufferSize = 0;
        return false;
    }

    iWorkBufferSize = aRequiredSize;
    return true;
}

bool CRenderer::EnsureLinePointsCapacity(TInt aRequiredCount)
{
    if (aRequiredCount <= iLinePointsBufferCapacity) {
        return true;
    }

    // Free old buffer.
    delete[] iLinePointsBuffer;

    // Allocate new buffer.
    iLinePointsBuffer = new TPoint[aRequiredCount];
    if (!iLinePointsBuffer) {
        iLinePointsBufferCapacity = 0;
        return false;
    }

    iLinePointsBufferCapacity = aRequiredCount;
    return true;
}

bool CRenderer::EnsureTempBitmapCapacity(TInt aWidth, TInt aHeight)
{
    if (iTempRenderBitmap &&
        iTempRenderBitmapWidth >= aWidth &&
        iTempRenderBitmapHeight >= aHeight) {
        return true;
    }

    // Delete old bitmap.
    delete iTempRenderBitmap;
    iTempRenderBitmap = 0;

    // Create new bitmap.
    iTempRenderBitmap = new CFbsBitmap();
    if (!iTempRenderBitmap) {
        iTempRenderBitmapWidth = 0;
        iTempRenderBitmapHeight = 0;
        return false;
    }

    TInt error = iTempRenderBitmap->Create(TSize(aWidth, aHeight), EColor4K);
    if (error != KErrNone) {
        delete iTempRenderBitmap;
        iTempRenderBitmap = 0;
        iTempRenderBitmapWidth = 0;
        iTempRenderBitmapHeight = 0;
        return false;
    }

    iTempRenderBitmapWidth = aWidth;
    iTempRenderBitmapHeight = aHeight;
    return true;
}

void CRenderer::BuildColorModLUT(TFixed rf, TFixed gf, TFixed bf)
{
    // Build lookup tables for R, G, B channels.
    for (int i = 0; i < 256; i++) {
        TFixed val = i << 16;                                                   // Convert to fixed-point
        iColorModLUT[i] = (TUint8)SDL_min(Fix2Int(FixMul(val, rf)), 255);       // R
        iColorModLUT[i + 256] = (TUint8)SDL_min(Fix2Int(FixMul(val, gf)), 255); // G
        iColorModLUT[i + 512] = (TUint8)SDL_min(Fix2Int(FixMul(val, bf)), 255); // B
    }

    // Remember the last color to avoid rebuilding unnecessarily.
    iLastColorR = rf;
    iLastColorG = gf;
    iLastColorB = bf;
}

CFbsBitmap *CRenderer::GetCardinalRotation(NGAGE_TextureData *aTextureData, TInt aAngleIndex)
{
    // Check if already cached.
    if (aTextureData->cardinalRotations[aAngleIndex]) {
        return aTextureData->cardinalRotations[aAngleIndex];
    }

    // Create rotated bitmap.
    CFbsBitmap *rotated = new CFbsBitmap();
    if (!rotated) {
        return NULL;
    }

    TInt w = aTextureData->cachedWidth;
    TInt h = aTextureData->cachedHeight;
    TSize size(w, h);

    // For 90 and 270 degree rotations, swap width/height.
    if (aAngleIndex == 1 || aAngleIndex == 3) {
        size = TSize(h, w);
    }

    TInt error = rotated->Create(size, EColor4K);
    if (error != KErrNone) {
        delete rotated;
        return NULL;
    }

    // Rotate the bitmap data.
    TUint16 *src = (TUint16 *)aTextureData->cachedDataAddress;
    TUint16 *dst = (TUint16 *)rotated->DataAddress();
    TInt srcPitch = aTextureData->cachedPitch >> 1;
    TInt dstPitch = rotated->ScanLineLength(size.iWidth, rotated->DisplayMode()) >> 1;

    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            TUint16 pixel = src[y * srcPitch + x];
            int dstX = 0;
            int dstY = 0;

            switch (aAngleIndex) {
            case 0: // 0 degrees
                dstX = x;
                dstY = y;
                break;
            case 1: // 90 degrees
                dstX = h - 1 - y;
                dstY = x;
                break;
            case 2: // 180 degrees
                dstX = w - 1 - x;
                dstY = h - 1 - y;
                break;
            case 3: // 270 degrees
                dstX = y;
                dstY = w - 1 - x;
                break;
            default:
                // Should never happen, but initialize to avoid warnings
                dstX = x;
                dstY = y;
                break;
            }

            dst[dstY * dstPitch + dstX] = pixel;
        }
    }

    aTextureData->cardinalRotations[aAngleIndex] = rotated;
    return rotated;
}

#ifdef __cplusplus
extern "C" {
#endif

Uint32 NGAGE_ConvertColor(float r, float g, float b, float a, float color_scale)
{
    TFixed ff = 255 << 16; // 255.f

    TFixed scalef = Real2Fix(color_scale);
    TFixed rf = Real2Fix(r);
    TFixed gf = Real2Fix(g);
    TFixed bf = Real2Fix(b);
    TFixed af = Real2Fix(a);

    rf = SDL_clamp(FixMul(rf, scalef), 0, ff);
    gf = SDL_clamp(FixMul(gf, scalef), 0, ff);
    bf = SDL_clamp(FixMul(bf, scalef), 0, ff);
    af = SDL_clamp(af, 0, ff);

    rf = FixMul(rf, ff) >> 16;
    gf = FixMul(gf, ff) >> 16;
    bf = FixMul(bf, ff) >> 16;
    af = FixMul(af, ff) >> 16;

    return (af << 24) | (bf << 16) | (gf << 8) | rf;
}

#ifdef __cplusplus
}
#endif

bool CRenderer::Copy(SDL_Renderer *renderer, SDL_Texture *texture, const SDL_Rect *srcrect, const SDL_Rect *dstrect)
{
    if (!texture) {
        return false;
    }

    NGAGE_TextureData *phdata = (NGAGE_TextureData *)texture->internal;
    if (!phdata || !phdata->bitmap) {
        return false;
    }

    SDL_FColor *c = &texture->color;

    // Get render scale once.
    float sx;
    float sy;
    SDL_GetRenderScale(renderer, &sx, &sy);

    // Fast path 1: No transformations needed; direct BitBlt.
    if (c->a == 1.f && c->r == 1.f && c->g == 1.f && c->b == 1.f && sx == 1.f && sy == 1.f) {
        TRect aSource(TPoint(srcrect->x, srcrect->y), TSize(srcrect->w, srcrect->h));
        TPoint aDest(dstrect->x, dstrect->y);
        iRenderer->Gc()->BitBlt(aDest, phdata->bitmap, aSource);
        return true;
    }

    // Slow path: Transformations needed.
    int w = phdata->cachedWidth;
    int h = phdata->cachedHeight;
    int pitch = phdata->cachedPitch;
    void *source = phdata->cachedDataAddress;
    void *dest;

    if (!source) {
        return false;
    }

    // Ensure work buffers have sufficient capacity.
    TInt bufferSize = pitch * h;
    if (!EnsureWorkBufferCapacity(bufferSize)) {
        return false;
    }

    dest = iWorkBuffer1;

    if (c->a != 1.f || c->r != 1.f || c->g != 1.f || c->b != 1.f) {
        TFixed rf = Real2Fix(c->r);
        TFixed gf = Real2Fix(c->g);
        TFixed bf = Real2Fix(c->b);

        // Build LUT if color changed.
        if (rf != iLastColorR || gf != iLastColorG || bf != iLastColorB) {
            BuildColorModLUT(rf, gf, bf);
        }

        ApplyColorMod(dest, source, pitch, w, h, texture->color, iColorModLUT);
        source = dest;
        dest = (dest == iWorkBuffer1) ? iWorkBuffer2 : iWorkBuffer1;
    }

    if (sx != 1.f || sy != 1.f) {
        TFixed scale_x = Real2Fix(sx);
        TFixed scale_y = Real2Fix(sy);
        TFixed center_x = Int2Fix(w / 2);
        TFixed center_y = Int2Fix(h / 2);

        ApplyScale(dest, source, pitch, w, h, center_x, center_y, scale_x, scale_y);
        source = dest;
    }

    // Use temp bitmap to avoid destroying source texture.
    if (!EnsureTempBitmapCapacity(w, h)) {
        return false;
    }

    // Copy transformed data to temp bitmap.
    Mem::Copy(iTempRenderBitmap->DataAddress(), source, pitch * h);

    // Render from temp bitmap, preserving original texture.
    TRect aSource(TPoint(srcrect->x, srcrect->y), TSize(srcrect->w, srcrect->h));
    TPoint aDest(dstrect->x, dstrect->y);
    iRenderer->Gc()->BitBlt(aDest, iTempRenderBitmap, aSource);

    return true;
}

bool CRenderer::CopyEx(SDL_Renderer *renderer, SDL_Texture *texture, const NGAGE_CopyExData *copydata)
{
    NGAGE_TextureData *phdata = (NGAGE_TextureData *)texture->internal;
    if (!phdata || !phdata->bitmap) {
        return false;
    }

    SDL_FColor *c = &texture->color;

    // Pre-calculate common checks.
    const bool isIdentityScale = (copydata->scale_x == Int2Fix(1) && copydata->scale_y == Int2Fix(1));
    const bool isNoRotation = (copydata->angle == 0);
    const bool isNoFlip = (!copydata->flip);
    const bool isNoColorMod = (c->a == 1.f && c->r == 1.f && c->g == 1.f && c->b == 1.f);

    // Fast path 1: No transformations needed; direct BitBlt.
    if (isNoFlip && isIdentityScale && isNoRotation && isNoColorMod) {
        TRect aSource(TPoint(copydata->srcrect.x, copydata->srcrect.y), TSize(copydata->srcrect.w, copydata->srcrect.h));
        TPoint aDest(copydata->dstrect.x, copydata->dstrect.y);
        iRenderer->Gc()->BitBlt(aDest, phdata->bitmap, aSource);
        return true;
    }

    // Fast path 2: Check for cardinal rotation cache opportunity (0°, 90°, 180°, 270°).
    if (isNoFlip && isIdentityScale && isNoColorMod && !isNoRotation) {
        TFixed angle = copydata->angle;
        TInt angleIndex = -1;

        // Check cardinal angles with tolerance - optimized for early exit.
        if (SDL_abs(angle - kAngleZero) < kAngleTolerance) {
            angleIndex = 0; // 0°
        } else if (SDL_abs(angle - kAnglePi_2) < kAngleTolerance) {
            angleIndex = 1; // 90°
        } else if (SDL_abs(angle - kAnglePi) < kAngleTolerance) {
            angleIndex = 2; // 180°
        } else if (SDL_abs(angle - kAnglePi3_2) < kAngleTolerance) {
            angleIndex = 3; // 270°
        } else if (SDL_abs(angle - kAnglePi2) < kAngleTolerance) {
            angleIndex = 0; // 360° = 0°
        }

        if (angleIndex >= 0) {
            CFbsBitmap *cached = GetCardinalRotation(phdata, angleIndex);
            if (cached) {
                TRect aSource(TPoint(copydata->srcrect.x, copydata->srcrect.y), TSize(copydata->srcrect.w, copydata->srcrect.h));
                TPoint aDest(copydata->dstrect.x, copydata->dstrect.y);
                iRenderer->Gc()->BitBlt(aDest, cached, aSource);
                return true;
            }
        }
    }

    // Slow path: Transformations needed.
    int w = phdata->cachedWidth;
    int h = phdata->cachedHeight;
    int pitch = phdata->cachedPitch;
    void *source = phdata->cachedDataAddress;
    void *dest;

    if (!source) {
        return false;
    }

    // Ensure work buffers have sufficient capacity.
    TInt bufferSize = pitch * h;
    if (!EnsureWorkBufferCapacity(bufferSize)) {
        return false;
    }

    dest = iWorkBuffer1;

    if (copydata->flip) {
        ApplyFlip(dest, source, pitch, w, h, copydata->flip);
        source = dest;
        dest = (dest == iWorkBuffer1) ? iWorkBuffer2 : iWorkBuffer1;
    }

    if (!isIdentityScale) {
        ApplyScale(dest, source, pitch, w, h, copydata->center.x, copydata->center.y, copydata->scale_x, copydata->scale_y);
        source = dest;
        dest = (dest == iWorkBuffer1) ? iWorkBuffer2 : iWorkBuffer1;
    }

    if (copydata->angle) {
        ApplyRotation(dest, source, pitch, w, h, copydata->center.x, copydata->center.y, copydata->angle);
        source = dest;
        dest = (dest == iWorkBuffer1) ? iWorkBuffer2 : iWorkBuffer1;
    }

    if (!isNoColorMod) {
        TFixed rf = Real2Fix(c->r);
        TFixed gf = Real2Fix(c->g);
        TFixed bf = Real2Fix(c->b);

        // Build LUT if color changed.
        if (rf != iLastColorR || gf != iLastColorG || bf != iLastColorB) {
            BuildColorModLUT(rf, gf, bf);
        }

        ApplyColorMod(dest, source, pitch, w, h, texture->color, iColorModLUT);
        source = dest;
    }

    // Use temp bitmap to avoid destroying source texture.
    if (!EnsureTempBitmapCapacity(w, h)) {
        return false;
    }

    // Copy transformed data to temp bitmap.
    Mem::Copy(iTempRenderBitmap->DataAddress(), source, pitch * h);

    // Render from temp bitmap, preserving original texture.
    TRect aSource(TPoint(copydata->srcrect.x, copydata->srcrect.y), TSize(copydata->srcrect.w, copydata->srcrect.h));
    TPoint aDest(copydata->dstrect.x, copydata->dstrect.y);
    iRenderer->Gc()->BitBlt(aDest, iTempRenderBitmap, aSource);

    return true;
}

bool CRenderer::CreateTextureData(NGAGE_TextureData *aTextureData, const TInt aWidth, const TInt aHeight)
{
    if (!aTextureData) {
        return false;
    }

    aTextureData->bitmap = new CFbsBitmap();
    if (!aTextureData->bitmap) {
        return false;
    }

    TInt error = aTextureData->bitmap->Create(TSize(aWidth, aHeight), EColor4K);
    if (error != KErrNone) {
        delete aTextureData->bitmap;
        aTextureData->bitmap = NULL;
        return false;
    }

    // Cache texture properties to avoid repeated API calls.
    TSize bitmapSize = aTextureData->bitmap->SizeInPixels();
    aTextureData->cachedWidth = bitmapSize.iWidth;
    aTextureData->cachedHeight = bitmapSize.iHeight;
    aTextureData->cachedPitch = aTextureData->bitmap->ScanLineLength(aWidth, aTextureData->bitmap->DisplayMode());
    aTextureData->cachedDataAddress = aTextureData->bitmap->DataAddress();

    // Initialize cardinal rotation cache to NULL.
    for (int i = 0; i < 4; i++) {
        aTextureData->cardinalRotations[i] = NULL;
    }

    // Initialize dirty tracking.
    aTextureData->isDirty = true; // New textures start dirty.
    aTextureData->dirtyRect.x = 0;
    aTextureData->dirtyRect.y = 0;
    aTextureData->dirtyRect.w = aWidth;
    aTextureData->dirtyRect.h = aHeight;

    return true;
}

void CRenderer::DrawLines(NGAGE_Vertex *aVerts, const TInt aCount)
{
    if (iRenderer && iRenderer->Gc()) {
        // Ensure reusable buffer has sufficient capacity.
        if (!EnsureLinePointsCapacity(aCount)) {
            return;
        }

        // Fill points from vertex data.
        for (TInt i = 0; i < aCount; i++) {
            iLinePointsBuffer[i] = TPoint(aVerts[i].x, aVerts[i].y);
        }

        // Pack color once - all vertices use the same color in polyline.
        Uint8 ca = aVerts->color.a;
        Uint8 cr = aVerts->color.r;
        Uint8 cg = aVerts->color.g;
        Uint8 cb = aVerts->color.b;
        TUint32 aColor = (ca << 24) | (cb << 16) | (cg << 8) | cr;

        iRenderer->Gc()->SetPenColor(aColor);
        iRenderer->Gc()->DrawPolyLineNoEndPoint(iLinePointsBuffer, aCount);
    }
}

void CRenderer::DrawPoints(NGAGE_Vertex *aVerts, const TInt aCount)
{
    if (iRenderer && iRenderer->Gc()) {
        // Batch points by color to minimize SetPenColor calls.
        TUint32 currentColor = 0;
        bool colorSet = false;

        for (TInt i = 0; i < aCount; i++, aVerts++) {
            Uint8 ca = aVerts->color.a;
            Uint8 cr = aVerts->color.r;
            Uint8 cg = aVerts->color.g;
            Uint8 cb = aVerts->color.b;
            TUint32 aColor = (ca << 24) | (cb << 16) | (cg << 8) | cr;

            // Only set pen color when it changes.
            if (!colorSet || aColor != currentColor) {
                iRenderer->Gc()->SetPenColor(aColor);
                currentColor = aColor;
                colorSet = true;
            }

            iRenderer->Gc()->Plot(TPoint(aVerts->x, aVerts->y));
        }
    }
}

void CRenderer::FillRects(NGAGE_Vertex *aVerts, const TInt aCount)
{
    if (iRenderer && iRenderer->Gc()) {
        // Batch rectangles by color to minimize SetPenColor/SetBrushColor calls.
        TUint32 currentColor = 0;
        bool colorSet = false;

        // Process rectangles (each rect uses 2 vertices: position and size).
        for (TInt i = 0; i < aCount; i += 2) {
            TPoint pos(aVerts[i].x, aVerts[i].y);
            TSize size(aVerts[i + 1].x, aVerts[i + 1].y);
            TRect rect(pos, size);

            Uint8 ca = aVerts[i].color.a;
            Uint8 cr = aVerts[i].color.r;
            Uint8 cg = aVerts[i].color.g;
            Uint8 cb = aVerts[i].color.b;
            TUint32 aColor = (ca << 24) | (cb << 16) | (cg << 8) | cr;

            // Only set colors when they change.
            if (!colorSet || aColor != currentColor) {
                iRenderer->Gc()->SetPenColor(aColor);
                iRenderer->Gc()->SetBrushColor(aColor);
                currentColor = aColor;
                colorSet = true;
            }

            iRenderer->Gc()->DrawRect(rect);
        }
    }
}

void CRenderer::Flip()
{
    if (!iRenderer) {
        SDL_Log("iRenderer is NULL.");
        return;
    }

    if (!iIsFocused) {
        return;
    }

    if (iShowFPS && iRenderer->Gc()) {
        UpdateFPS();

        iRenderer->Gc()->UseFont(iFont);

        TBuf<64> info;

        iRenderer->Gc()->SetPenStyle(CGraphicsContext::ESolidPen);
        iRenderer->Gc()->SetBrushStyle(CGraphicsContext::ESolidBrush);
        iRenderer->Gc()->SetBrushColor(KRgbBlack);
        iRenderer->Gc()->SetPenColor(KRgbCyan);

        // Draw FPS background and text.
        TRect aTextRect(TPoint(3, 203 - iFont->HeightInPixels()), TSize(45, iFont->HeightInPixels() + 2));
        iRenderer->Gc()->DrawRect(aTextRect);

        info.Format(_L("FPS: %d"), iFPS);
        iRenderer->Gc()->DrawText(info, TPoint(5, 203));

        iRenderer->Gc()->DiscardFont();
    }

    iRenderer->Flip(iDirectScreen);

    // Keep the backlight on when screen saver is suspended.
    if (iSuspendScreenSaver) {
        User::ResetInactivityTime();
    }

    // Yield to other threads and active objects briefly.
    User::After(0);
}

void CRenderer::SetDrawColor(TUint32 iColor)
{
    if (iRenderer && iRenderer->Gc()) {
        // Skip redundant calls if color hasn't changed.
        if (iColor == iLastDrawColor) {
            return;
        }

        iRenderer->Gc()->SetPenColor(iColor);
        iRenderer->Gc()->SetBrushColor(iColor);
        iRenderer->Gc()->SetBrushStyle(CGraphicsContext::ESolidBrush);

        TRAPD(err, iRenderer->SetCurrentColor(iColor));
        if (err != KErrNone) {
            return;
        }

        iLastDrawColor = iColor;
    }
}

void CRenderer::SetClipRect(TInt aX, TInt aY, TInt aWidth, TInt aHeight)
{
    if (iRenderer && iRenderer->Gc()) {
        TRect viewportRect(aX, aY, aX + aWidth, aY + aHeight);
        iRenderer->Gc()->SetClippingRect(viewportRect);
    }
}

void CRenderer::UpdateFPS()
{
    static TTime lastTime;
    static TInt frameCount = 0;
    static TBool initialized = EFalse;
    TTime currentTime;
    const TUint KOneSecond = 1000000; // 1s in microseconds.

    currentTime.HomeTime();

    if (!initialized) {
        lastTime = currentTime;
        initialized = ETrue;
    }

    ++frameCount;

    TTimeIntervalMicroSeconds timeDiff = currentTime.MicroSecondsFrom(lastTime);

    if (timeDiff.Int64() >= KOneSecond) {
        // Calculate FPS.
        iFPS = frameCount;

        // Reset frame count and last time.
        frameCount = 0;
        lastTime = currentTime;
    }
}

void CRenderer::SuspendScreenSaver(TBool aSuspend)
{
    iSuspendScreenSaver = aSuspend;
}

static SDL_Scancode ConvertScancode(int key)
{
    SDL_Keycode keycode;

    switch (key) {
    case EStdKeyBackspace: // Clear key
        keycode = SDLK_BACKSPACE;
        break;
    case 0x31: // 1
        keycode = SDLK_1;
        break;
    case 0x32: // 2
        keycode = SDLK_2;
        break;
    case 0x33: // 3
        keycode = SDLK_3;
        break;
    case 0x34: // 4
        keycode = SDLK_4;
        break;
    case 0x35: // 5
        keycode = SDLK_5;
        break;
    case 0x36: // 6
        keycode = SDLK_6;
        break;
    case 0x37: // 7
        keycode = SDLK_7;
        break;
    case 0x38: // 8
        keycode = SDLK_8;
        break;
    case 0x39: // 9
        keycode = SDLK_9;
        break;
    case 0x30: // 0
        keycode = SDLK_0;
        break;
    case 0x2a: // Asterisk
        keycode = SDLK_ASTERISK;
        break;
    case EStdKeyHash: // Hash
        keycode = SDLK_HASH;
        break;
    case EStdKeyDevice0: // Left softkey
        keycode = SDLK_SOFTLEFT;
        break;
    case EStdKeyDevice1: // Right softkey
        keycode = SDLK_SOFTRIGHT;
        break;
    case EStdKeyApplication0: // Call softkey
        keycode = SDLK_CALL;
        break;
    case EStdKeyApplication1: // End call softkey
        keycode = SDLK_ENDCALL;
        break;
    case EStdKeyDevice3: // Middle softkey
        keycode = SDLK_SELECT;
        break;
    case EStdKeyUpArrow: // Up arrow
        keycode = SDLK_UP;
        break;
    case EStdKeyDownArrow: // Down arrow
        keycode = SDLK_DOWN;
        break;
    case EStdKeyLeftArrow: // Left arrow
        keycode = SDLK_LEFT;
        break;
    case EStdKeyRightArrow: // Right arrow
        keycode = SDLK_RIGHT;
        break;
    default:
        keycode = SDLK_UNKNOWN;
        break;
    }

    return SDL_GetScancodeFromKey(keycode, NULL);
}

void CRenderer::HandleEvent(const TWsEvent &aWsEvent)
{
    Uint64 timestamp;

    switch (aWsEvent.Type()) {
    case EEventKeyDown: /* Key events */
        timestamp = SDL_GetPerformanceCounter();
        SDL_SendKeyboardKey(timestamp, 1, aWsEvent.Key()->iCode, ConvertScancode(aWsEvent.Key()->iScanCode), true);

        if (aWsEvent.Key()->iScanCode == EStdKeyHash) {
            if (iShowFPS) {
                iShowFPS = EFalse;
            } else {
                iShowFPS = ETrue;
            }
        }

        break;
    case EEventKeyUp: /* Key events */
        timestamp = SDL_GetPerformanceCounter();
        SDL_SendKeyboardKey(timestamp, 1, aWsEvent.Key()->iCode, ConvertScancode(aWsEvent.Key()->iScanCode), false);
        break;

    case EEventFocusGained:
        DisableKeyBlocking();
        if (!iDirectScreen->IsActive()) {
            TRAPD(err, iDirectScreen->StartL());
            if (KErrNone != err) {
                return;
            }
            iDirectScreen->ScreenDevice()->SetAutoUpdate(ETrue);
            iIsFocused = ETrue;
        }
        Flip();
        break;
    case EEventFocusLost:
    {
        if (iDirectScreen->IsActive()) {
            iDirectScreen->Cancel();
        }

        iIsFocused = EFalse;
        break;
    }
    default:
        break;
    }
}

void CRenderer::DisableKeyBlocking()
{
    TRawEvent aEvent;

    aEvent.Set((TRawEvent::TType) /*EDisableKeyBlock*/ 51);
    iWsSession.SimulateRawEvent(aEvent);
}

void CRenderer::PumpEvents()
{
    while (iWsEventStatus != KRequestPending) {
        iWsSession.GetEvent(iWsEvent);
        HandleEvent(iWsEvent);
        iWsEventStatus = KRequestPending;
        iWsSession.EventReady(&iWsEventStatus);
    }
}

#endif // SDL_VIDEO_RENDER_NGAGE
