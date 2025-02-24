/*
 * MACDRV mouse driver
 *
 * Copyright 1998 Ulrich Weigand
 * Copyright 2007 Henri Verbeet
 * Copyright 2011, 2012, 2013 Ken Thomases for CodeWeavers Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#include "config.h"

#define OEMRESOURCE
#include "macdrv.h"
#include "winuser.h"
#include "winreg.h"
#include "wine/server.h"
#include "wine/unicode.h"

WINE_DEFAULT_DEBUG_CHANNEL(cursor);


static CRITICAL_SECTION cursor_cache_section;
static CRITICAL_SECTION_DEBUG critsect_debug =
{
    0, 0, &cursor_cache_section,
    { &critsect_debug.ProcessLocksList, &critsect_debug.ProcessLocksList },
      0, 0, { (DWORD_PTR)(__FILE__ ": cursor_cache_section") }
};
static CRITICAL_SECTION cursor_cache_section = { &critsect_debug, -1, 0, 0, 0, 0 };

static CFMutableDictionaryRef cursor_cache;


struct system_cursors
{
    WORD id;
    CFStringRef name;
};

static const struct system_cursors user32_cursors[] =
{
    { OCR_NORMAL,      CFSTR("arrowCursor") },
    { OCR_IBEAM,       CFSTR("IBeamCursor") },
    { OCR_CROSS,       CFSTR("crosshairCursor") },
    { OCR_SIZEWE,      CFSTR("resizeLeftRightCursor") },
    { OCR_SIZENS,      CFSTR("resizeUpDownCursor") },
    { OCR_NO,          CFSTR("operationNotAllowedCursor") },
    { OCR_HAND,        CFSTR("pointingHandCursor") },
    { 0 }
};

static const struct system_cursors comctl32_cursors[] =
{
    { 102, CFSTR("closedHandCursor") },
    { 104, CFSTR("dragCopyCursor") },
    { 105, CFSTR("arrowCursor") },
    { 106, CFSTR("resizeLeftRightCursor") },
    { 107, CFSTR("resizeLeftRightCursor") },
    { 108, CFSTR("pointingHandCursor") },
    { 135, CFSTR("resizeUpDownCursor") },
    { 0 }
};

static const struct system_cursors ole32_cursors[] =
{
    { 1, CFSTR("operationNotAllowedCursor") },
    { 2, CFSTR("closedHandCursor") },
    { 3, CFSTR("dragCopyCursor") },
    { 4, CFSTR("dragLinkCursor") },
    { 0 }
};

static const struct system_cursors riched20_cursors[] =
{
    { 105, CFSTR("pointingHandCursor") },
    { 109, CFSTR("dragCopyCursor") },
    { 110, CFSTR("closedHandCursor") },
    { 111, CFSTR("operationNotAllowedCursor") },
    { 0 }
};

static const struct
{
    const struct system_cursors *cursors;
    WCHAR name[16];
} module_cursors[] =
{
    { user32_cursors, {'u','s','e','r','3','2','.','d','l','l',0} },
    { comctl32_cursors, {'c','o','m','c','t','l','3','2','.','d','l','l',0} },
    { ole32_cursors, {'o','l','e','3','2','.','d','l','l',0} },
    { riched20_cursors, {'r','i','c','h','e','d','2','0','.','d','l','l',0} }
};

/* The names of NSCursor class methods which return cursor objects. */
static const CFStringRef cocoa_cursor_names[] =
{
    CFSTR("arrowCursor"),
    CFSTR("closedHandCursor"),
    CFSTR("contextualMenuCursor"),
    CFSTR("crosshairCursor"),
    CFSTR("disappearingItemCursor"),
    CFSTR("dragCopyCursor"),
    CFSTR("dragLinkCursor"),
    CFSTR("IBeamCursor"),
    CFSTR("IBeamCursorForVerticalLayout"),
    CFSTR("openHandCursor"),
    CFSTR("operationNotAllowedCursor"),
    CFSTR("pointingHandCursor"),
    CFSTR("resizeDownCursor"),
    CFSTR("resizeLeftCursor"),
    CFSTR("resizeLeftRightCursor"),
    CFSTR("resizeRightCursor"),
    CFSTR("resizeUpCursor"),
    CFSTR("resizeUpDownCursor"),
};


/***********************************************************************
 *              send_mouse_input
 *
 * Update the various window states on a mouse event.
 */
static void send_mouse_input(HWND hwnd, macdrv_window cocoa_window, UINT flags, int x, int y,
                             DWORD mouse_data, BOOL drag, unsigned long time)
{
    INPUT input;
    HWND top_level_hwnd;

    top_level_hwnd = GetAncestor(hwnd, GA_ROOT);

    if ((flags & MOUSEEVENTF_MOVE) && (flags & MOUSEEVENTF_ABSOLUTE) && !drag &&
        cocoa_window != macdrv_thread_data()->capture_window)
    {
        /* update the wine server Z-order */
        SERVER_START_REQ(update_window_zorder)
        {
            req->window      = wine_server_user_handle(top_level_hwnd);
            req->rect.left   = x;
            req->rect.top    = y;
            req->rect.right  = x + 1;
            req->rect.bottom = y + 1;
            wine_server_call(req);
        }
        SERVER_END_REQ;
    }

    input.type              = INPUT_MOUSE;
    input.mi.dx             = x;
    input.mi.dy             = y;
    input.mi.mouseData      = mouse_data;
    input.mi.dwFlags        = flags;
    input.mi.time           = time;
    input.mi.dwExtraInfo    = 0;

    __wine_send_input(top_level_hwnd, &input, NULL);
}


/***********************************************************************
 *              copy_system_cursor_name
 */
CFStringRef copy_system_cursor_name(ICONINFOEXW *info)
{
    static const WCHAR idW[] = {'%','h','u',0};
    const struct system_cursors *cursors;
    unsigned int i;
    CFStringRef cursor_name = NULL;
    HMODULE module;
    HKEY key;
    WCHAR *p, name[MAX_PATH * 2];

    TRACE("info->szModName %s info->szResName %s info->wResID %hu\n", debugstr_w(info->szModName),
          debugstr_w(info->szResName), info->wResID);

    if (!info->szModName[0]) return NULL;

    p = strrchrW(info->szModName, '\\');
    strcpyW(name, p ? p + 1 : info->szModName);
    p = name + strlenW(name);
    *p++ = ',';
    if (info->szResName[0]) strcpyW(p, info->szResName);
    else sprintfW(p, idW, info->wResID);

    /* @@ Wine registry key: HKCU\Software\Wine\Mac Driver\Cursors */
    if (!RegOpenKeyA(HKEY_CURRENT_USER, "Software\\Wine\\Mac Driver\\Cursors", &key))
    {
        WCHAR value[64];
        DWORD size, ret;

        value[0] = 0;
        size = sizeof(value);
        ret = RegQueryValueExW(key, name, NULL, NULL, (BYTE *)value, &size);
        RegCloseKey(key);
        if (!ret)
        {
            if (!value[0])
            {
                TRACE("registry forces standard cursor for %s\n", debugstr_w(name));
                return NULL; /* force standard cursor */
            }

            cursor_name = CFStringCreateWithCharacters(NULL, value, strlenW(value));
            if (!cursor_name)
            {
                WARN("CFStringCreateWithCharacters failed for %s\n", debugstr_w(value));
                return NULL;
            }

            /* Make sure it's one of the appropriate NSCursor class methods. */
            for (i = 0; i < ARRAY_SIZE(cocoa_cursor_names); i++)
                if (CFEqual(cursor_name, cocoa_cursor_names[i]))
                    goto done;

            WARN("%s mapped to invalid Cocoa cursor name %s\n", debugstr_w(name), debugstr_w(value));
            CFRelease(cursor_name);
            return NULL;
        }
    }

    if (info->szResName[0]) goto done;  /* only integer resources are supported here */
    if (!(module = GetModuleHandleW(info->szModName))) goto done;

    for (i = 0; i < ARRAY_SIZE(module_cursors); i++)
        if (GetModuleHandleW(module_cursors[i].name) == module) break;
    if (i == ARRAY_SIZE(module_cursors)) goto done;

    cursors = module_cursors[i].cursors;
    for (i = 0; cursors[i].id; i++)
        if (cursors[i].id == info->wResID)
        {
            cursor_name = CFRetain(cursors[i].name);
            break;
        }

done:
    if (cursor_name)
        TRACE("%s -> %s\n", debugstr_w(name), debugstr_cf(cursor_name));
    else
        WARN("no system cursor found for %s\n", debugstr_w(name));
    return cursor_name;
}

/***********************************************************************
 *              create_monochrome_cursor
 */
CFArrayRef create_monochrome_cursor(HDC hdc, const ICONINFOEXW *icon, int width, int height)
{
    char buffer[FIELD_OFFSET(BITMAPINFO, bmiColors[256])];
    BITMAPINFO *info = (BITMAPINFO *)buffer;
    unsigned int width_bytes = (width + 31) / 32 * 4;
    unsigned long *and_bits = NULL, *xor_bits;
    unsigned long * HOSTPTR data_bits;
    int count, i;
    CGColorSpaceRef colorspace;
    CFMutableDataRef data;
    CGDataProviderRef provider;
    CGImageRef cgimage, cgmask, cgmasked;
    CGPoint hot_spot;
    CFDictionaryRef hot_spot_dict;
    const CFStringRef keys[] = { CFSTR("image"), CFSTR("hotSpot") };
    CFTypeRef values[ARRAY_SIZE(keys)];
    CFDictionaryRef frame;
    CFArrayRef frames;

    TRACE("hdc %p icon->hbmMask %p icon->xHotspot %d icon->yHotspot %d width %d height %d\n",
          hdc, icon->hbmMask, icon->xHotspot, icon->yHotspot, width, height);

    info->bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info->bmiHeader.biWidth = width;
    info->bmiHeader.biHeight = -height * 2;
    info->bmiHeader.biPlanes = 1;
    info->bmiHeader.biBitCount = 1;
    info->bmiHeader.biCompression = BI_RGB;
    info->bmiHeader.biSizeImage = width_bytes * height * 2;
    info->bmiHeader.biXPelsPerMeter = 0;
    info->bmiHeader.biYPelsPerMeter = 0;
    info->bmiHeader.biClrUsed = 0;
    info->bmiHeader.biClrImportant = 0;

    and_bits = HeapAlloc(GetProcessHeap(), 0, info->bmiHeader.biSizeImage);
    if (!and_bits)
    {
        WARN("failed to allocate and_bits\n");
        return NULL;
    }
    xor_bits = (unsigned long*)((char*)and_bits + info->bmiHeader.biSizeImage / 2);

    if (!GetDIBits(hdc, icon->hbmMask, 0, height * 2, and_bits, info, DIB_RGB_COLORS))
    {
        WARN("GetDIBits failed\n");
        HeapFree(GetProcessHeap(), 0, and_bits);
        return NULL;
    }

    /* On Windows, the pixels of a monochrome cursor can have four effects:
       draw black, draw white, leave unchanged (transparent), or invert.  The Mac
       only supports the first three.  It can't do pixels which invert the
       background.  Since the background is usually white, I am arbitrarily
       mapping "invert" to "draw black".  This entails bitwise math between the
       cursor's AND mask and XOR mask:

            AND | XOR | Windows cursor pixel
            --------------------------------
             0  |  0  | black
             0  |  1  | white
             1  |  0  | transparent
             1  |  1  | invert

            AND | XOR | Mac image
            ---------------------
             0  |  0  | black (0)
             0  |  1  | white (1)
             1  |  0  | don't care
             1  |  1  | black (0)

            AND | XOR | Mac mask
            ---------------------------
             0  |  0  | paint (0)
             0  |  1  | paint (0)
             1  |  0  | don't paint (1)
             1  |  1  | paint (0)

       So, Mac image = AND ^ XOR and Mac mask = AND & ~XOR.
      */
    /* Create data for Mac image. */
    data = CFDataCreateMutable(NULL, info->bmiHeader.biSizeImage / 2);
    if (!data)
    {
        WARN("failed to create data\n");
        HeapFree(GetProcessHeap(), 0, and_bits);
        return NULL;
    }

    /* image data = AND mask */
    CFDataAppendBytes(data, (UInt8*)and_bits, info->bmiHeader.biSizeImage / 2);
    /* image data ^= XOR mask */
    data_bits = (unsigned long* HOSTPTR)CFDataGetMutableBytePtr(data);
    count = (info->bmiHeader.biSizeImage / 2) / sizeof(*data_bits);
    for (i = 0; i < count; i++)
        data_bits[i] ^= xor_bits[i];

    colorspace = CGColorSpaceCreateWithName(kCGColorSpaceGenericGrayGamma2_2);
    if (!colorspace)
    {
        WARN("failed to create colorspace\n");
        CFRelease(data);
        HeapFree(GetProcessHeap(), 0, and_bits);
        return NULL;
    }

    provider = CGDataProviderCreateWithCFData(data);
    CFRelease(data);
    if (!provider)
    {
        WARN("failed to create data provider\n");
        CGColorSpaceRelease(colorspace);
        HeapFree(GetProcessHeap(), 0, and_bits);
        return NULL;
    }

    cgimage = CGImageCreate(width, height, 1, 1, width_bytes, colorspace,
                            kCGImageAlphaNone | kCGBitmapByteOrderDefault,
                            provider, NULL, FALSE, kCGRenderingIntentDefault);
    CGDataProviderRelease(provider);
    CGColorSpaceRelease(colorspace);
    if (!cgimage)
    {
        WARN("failed to create image\n");
        HeapFree(GetProcessHeap(), 0, and_bits);
        return NULL;
    }

    /* Create data for mask. */
    data = CFDataCreateMutable(NULL, info->bmiHeader.biSizeImage / 2);
    if (!data)
    {
        WARN("failed to create data\n");
        CGImageRelease(cgimage);
        HeapFree(GetProcessHeap(), 0, and_bits);
        return NULL;
    }

    /* mask data = AND mask */
    CFDataAppendBytes(data, (UInt8*)and_bits, info->bmiHeader.biSizeImage / 2);
    /* mask data &= ~XOR mask */
    data_bits = (unsigned long* HOSTPTR)CFDataGetMutableBytePtr(data);
    for (i = 0; i < count; i++)
        data_bits[i] &= ~xor_bits[i];
    HeapFree(GetProcessHeap(), 0, and_bits);

    provider = CGDataProviderCreateWithCFData(data);
    CFRelease(data);
    if (!provider)
    {
        WARN("failed to create data provider\n");
        CGImageRelease(cgimage);
        return NULL;
    }

    cgmask = CGImageMaskCreate(width, height, 1, 1, width_bytes, provider, NULL, FALSE);
    CGDataProviderRelease(provider);
    if (!cgmask)
    {
        WARN("failed to create mask image\n");
        CGImageRelease(cgimage);
        return NULL;
    }

    cgmasked = CGImageCreateWithMask(cgimage, cgmask);
    CGImageRelease(cgimage);
    CGImageRelease(cgmask);
    if (!cgmasked)
    {
        WARN("failed to create masked image\n");
        return NULL;
    }

    hot_spot = CGPointMake(icon->xHotspot, icon->yHotspot);
    hot_spot_dict = CGPointCreateDictionaryRepresentation(hot_spot);
    if (!hot_spot_dict)
    {
        WARN("failed to create hot spot dictionary\n");
        CGImageRelease(cgmasked);
        return NULL;
    }

    values[0] = cgmasked;
    values[1] = hot_spot_dict;
    frame = CFDictionaryCreate(NULL, (const void* HOSTPTR *)keys, values, ARRAY_SIZE(keys),
                               &kCFCopyStringDictionaryKeyCallBacks,
                               &kCFTypeDictionaryValueCallBacks);
    CFRelease(hot_spot_dict);
    CGImageRelease(cgmasked);
    if (!frame)
    {
        WARN("failed to create frame dictionary\n");
        return NULL;
    }

    frames = CFArrayCreate(NULL, (const void* HOSTPTR *)&frame, 1, &kCFTypeArrayCallBacks);
    CFRelease(frame);
    if (!frames)
    {
        WARN("failed to create frames array\n");
        return NULL;
    }

    return frames;
}


/***********************************************************************
 *              create_cursor_frame
 *
 * Create a frame dictionary for a cursor from a Windows icon.
 * Keys:
 *      "image"     a CGImage for the frame
 *      "duration"  a CFNumber for the frame duration in seconds
 *      "hotSpot"   a CFDictionary encoding a CGPoint for the hot spot
 */
static CFDictionaryRef create_cursor_frame(HDC hdc, const ICONINFOEXW *iinfo, HANDLE icon,
                                           HBITMAP hbmColor, unsigned char *color_bits, int color_size,
                                           HBITMAP hbmMask, unsigned char *mask_bits, int mask_size,
                                           int width, int height, int istep)
{
    DWORD delay_jiffies, num_steps;
    CFMutableDictionaryRef frame;
    CGPoint hot_spot;
    CFDictionaryRef hot_spot_dict;
    double duration;
    CFNumberRef duration_number;
    CGImageRef cgimage;

    TRACE("hdc %p iinfo->xHotspot %d iinfo->yHotspot %d icon %p hbmColor %p color_bits %p color_size %d"
          " hbmMask %p mask_bits %p mask_size %d width %d height %d istep %d\n",
          hdc, iinfo->xHotspot, iinfo->yHotspot, icon, hbmColor, color_bits, color_size,
          hbmMask, mask_bits, mask_size, width, height, istep);

    frame = CFDictionaryCreateMutable(NULL, 0, &kCFCopyStringDictionaryKeyCallBacks,
                                      &kCFTypeDictionaryValueCallBacks);
    if (!frame)
    {
        WARN("failed to allocate dictionary for frame\n");
        return NULL;
    }

    hot_spot = CGPointMake(iinfo->xHotspot, iinfo->yHotspot);
    hot_spot_dict = CGPointCreateDictionaryRepresentation(hot_spot);
    if (!hot_spot_dict)
    {
        WARN("failed to create hot spot dictionary\n");
        CFRelease(frame);
        return NULL;
    }
    CFDictionarySetValue(frame, CFSTR("hotSpot"), hot_spot_dict);
    CFRelease(hot_spot_dict);

    if (GetCursorFrameInfo(icon, 0x0 /* unknown parameter */, istep, &delay_jiffies, &num_steps) != 0)
        duration = delay_jiffies / 60.0; /* convert jiffies (1/60s) to seconds */
    else
    {
        WARN("Failed to retrieve animated cursor frame-rate for frame %d.\n", istep);
        duration = 0.1; /* fallback delay, 100 ms */
    }
    duration_number = CFNumberCreate(NULL, kCFNumberDoubleType, &duration);
    if (!duration_number)
    {
        WARN("failed to create duration number\n");
        CFRelease(frame);
        return NULL;
    }
    CFDictionarySetValue(frame, CFSTR("duration"), duration_number);
    CFRelease(duration_number);

    cgimage = create_cgimage_from_icon_bitmaps(hdc, icon, hbmColor, color_bits, color_size,
                                               hbmMask, mask_bits, mask_size, width, height, istep);
    if (!cgimage)
    {
        CFRelease(frame);
        return NULL;
    }

    CFDictionarySetValue(frame, CFSTR("image"), cgimage);
    CGImageRelease(cgimage);

    return frame;
}


/***********************************************************************
 *              create_color_cursor
 *
 * Create an array of color cursor frames from a Windows cursor.  Each
 * frame is represented in the array by a dictionary.
 * Frame dictionary keys:
 *      "image"     a CGImage for the frame
 *      "duration"  a CFNumber for the frame duration in seconds
 *      "hotSpot"   a CFDictionary encoding a CGPoint for the hot spot
 */
static CFArrayRef create_color_cursor(HDC hdc, const ICONINFOEXW *iinfo, HANDLE icon, int width, int height)
{
    unsigned char *color_bits, *mask_bits;
    HBITMAP hbmColor = 0, hbmMask = 0;
    DWORD nFrames, delay_jiffies, i;
    int color_size, mask_size;
    BITMAPINFO *info = NULL;
    CFMutableArrayRef frames;

    TRACE("hdc %p iinfo %p icon %p width %d height %d\n", hdc, iinfo, icon, width, height);

    /* Retrieve the number of frames to render */
    if (!GetCursorFrameInfo(icon, 0x0 /* unknown parameter */, 0, &delay_jiffies, &nFrames))
    {
        WARN("GetCursorFrameInfo failed\n");
        return NULL;
    }
    if (!(frames = CFArrayCreateMutable(NULL, nFrames, &kCFTypeArrayCallBacks)))
    {
        WARN("failed to allocate frames array\n");
        return NULL;
    }

    /* Allocate all of the resources necessary to obtain a cursor frame */
    if (!(info = HeapAlloc(GetProcessHeap(), 0, FIELD_OFFSET(BITMAPINFO, bmiColors[256])))) goto cleanup;
    info->bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info->bmiHeader.biWidth = width;
    info->bmiHeader.biHeight = -height;
    info->bmiHeader.biPlanes = 1;
    info->bmiHeader.biCompression = BI_RGB;
    info->bmiHeader.biXPelsPerMeter = 0;
    info->bmiHeader.biYPelsPerMeter = 0;
    info->bmiHeader.biClrUsed = 0;
    info->bmiHeader.biClrImportant = 0;
    info->bmiHeader.biBitCount = 32;
    color_size = width * height * 4;
    info->bmiHeader.biSizeImage = color_size;
    hbmColor = CreateDIBSection(hdc, info, DIB_RGB_COLORS, (VOID **) &color_bits, NULL, 0);
    if (!hbmColor)
    {
        WARN("failed to create DIB section for cursor color data\n");
        goto cleanup;
    }
    info->bmiHeader.biBitCount = 1;
    info->bmiColors[0].rgbRed      = 0;
    info->bmiColors[0].rgbGreen    = 0;
    info->bmiColors[0].rgbBlue     = 0;
    info->bmiColors[0].rgbReserved = 0;
    info->bmiColors[1].rgbRed      = 0xff;
    info->bmiColors[1].rgbGreen    = 0xff;
    info->bmiColors[1].rgbBlue     = 0xff;
    info->bmiColors[1].rgbReserved = 0;

    mask_size = ((width + 31) / 32 * 4) * height; /* width_bytes * height */
    info->bmiHeader.biSizeImage = mask_size;
    hbmMask = CreateDIBSection(hdc, info, DIB_RGB_COLORS, (VOID **) &mask_bits, NULL, 0);
    if (!hbmMask)
    {
        WARN("failed to create DIB section for cursor mask data\n");
        goto cleanup;
    }

    /* Create a CFDictionary for each frame of the cursor */
    for (i = 0; i < nFrames; i++)
    {
        CFDictionaryRef frame = create_cursor_frame(hdc, iinfo, icon,
                                                    hbmColor, color_bits, color_size,
                                                    hbmMask, mask_bits, mask_size,
                                                    width, height, i);
        if (!frame) goto cleanup;
        CFArrayAppendValue(frames, frame);
        CFRelease(frame);
    }

cleanup:
    if (CFArrayGetCount(frames) < nFrames)
    {
        CFRelease(frames);
        frames = NULL;
    }
    else
        TRACE("returning cursor with %d frames\n", nFrames);
    /* Cleanup all of the resources used to obtain the frame data */
    if (hbmColor) DeleteObject(hbmColor);
    if (hbmMask) DeleteObject(hbmMask);
    HeapFree(GetProcessHeap(), 0, info);
    return frames;
}


/***********************************************************************
 *              DestroyCursorIcon (MACDRV.@)
 */
void CDECL macdrv_DestroyCursorIcon(HCURSOR cursor)
{
    TRACE("cursor %p\n", cursor);

    EnterCriticalSection(&cursor_cache_section);
    if (cursor_cache)
        CFDictionaryRemoveValue(cursor_cache, cursor);
    LeaveCriticalSection(&cursor_cache_section);
}


/***********************************************************************
 *              ClipCursor (MACDRV.@)
 *
 * Set the cursor clipping rectangle.
 */
BOOL CDECL macdrv_ClipCursor(LPCRECT clip)
{
    CGRect rect;

    TRACE("%s\n", wine_dbgstr_rect(clip));

    if (clip)
    {
        rect = CGRectMake(clip->left, clip->top, max(1, clip->right - clip->left),
                          max(1, clip->bottom - clip->top));
    }
    else
        rect = CGRectInfinite;

    /* FIXME: This needs to be done not just in this process but in all of the
       ones for this WINEPREFIX.  Broadcast a message to do that. */

    return macdrv_clip_cursor(rect);
}


/***********************************************************************
 *              GetCursorPos (MACDRV.@)
 */
BOOL CDECL macdrv_GetCursorPos(LPPOINT pos)
{
    CGPoint pt;
    BOOL ret;

    ret = macdrv_get_cursor_position(&pt);
    if (ret)
    {
        TRACE("pointer at (%g,%g) server pos %d,%d\n", pt.x, pt.y, pos->x, pos->y);
        pos->x = floor(pt.x);
        pos->y = floor(pt.y);
    }
    return ret;
}


/***********************************************************************
 *              SetCapture (MACDRV.@)
 */
 void CDECL macdrv_SetCapture(HWND hwnd, UINT flags)
{
    struct macdrv_thread_data *thread_data = macdrv_thread_data();
    HWND top = GetAncestor(hwnd, GA_ROOT);
    macdrv_window cocoa_window = macdrv_get_cocoa_window(top, FALSE);

    TRACE("hwnd %p top %p/%p flags 0x%08x\n", hwnd, top, cocoa_window, flags);

    if (!thread_data) return;

    thread_data->capture_window = cocoa_window;
    macdrv_set_mouse_capture_window(cocoa_window);
}


/***********************************************************************
 *              SetCursor (MACDRV.@)
 */
void CDECL macdrv_SetCursor(HCURSOR cursor)
{
    CFStringRef cursor_name = NULL;
    CFArrayRef cursor_frames = NULL;

    TRACE("%p\n", cursor);

    if (cursor)
    {
        ICONINFOEXW info;

        EnterCriticalSection(&cursor_cache_section);
        if (cursor_cache)
        {
            CFTypeRef cached_cursor = CFDictionaryGetValue(cursor_cache, cursor);
            if (cached_cursor)
            {
                if (CFGetTypeID(cached_cursor) == CFStringGetTypeID())
                    cursor_name = CFRetain(cached_cursor);
                else
                    cursor_frames = CFRetain(cached_cursor);
            }
        }
        LeaveCriticalSection(&cursor_cache_section);
        if (cursor_name || cursor_frames)
            goto done;

        info.cbSize = sizeof(info);
        if (!GetIconInfoExW(cursor, &info))
        {
            WARN("GetIconInfoExW failed\n");
            return;
        }

        if ((cursor_name = copy_system_cursor_name(&info)))
        {
            DeleteObject(info.hbmColor);
            DeleteObject(info.hbmMask);
        }
        else
        {
            BITMAP bm;
            HDC hdc;

            GetObjectW(info.hbmMask, sizeof(bm), &bm);
            if (!info.hbmColor) bm.bmHeight = max(1, bm.bmHeight / 2);

            /* make sure hotspot is valid */
            if (info.xHotspot >= bm.bmWidth || info.yHotspot >= bm.bmHeight)
            {
                info.xHotspot = bm.bmWidth / 2;
                info.yHotspot = bm.bmHeight / 2;
            }

            hdc = CreateCompatibleDC(0);

            if (info.hbmColor)
            {
                cursor_frames = create_color_cursor(hdc, &info, cursor, bm.bmWidth, bm.bmHeight);
                DeleteObject(info.hbmColor);
            }
            else
                cursor_frames = create_monochrome_cursor(hdc, &info, bm.bmWidth, bm.bmHeight);

            DeleteObject(info.hbmMask);
            DeleteDC(hdc);
        }

        if (cursor_name || cursor_frames)
        {
            EnterCriticalSection(&cursor_cache_section);
            if (!cursor_cache)
                cursor_cache = CFDictionaryCreateMutable(NULL, 0, NULL,
                                                         &kCFTypeDictionaryValueCallBacks);
            CFDictionarySetValue(cursor_cache, cursor,
                                 cursor_name ? (CFTypeRef)cursor_name : (CFTypeRef)cursor_frames);
            LeaveCriticalSection(&cursor_cache_section);
        }
        else
            cursor_name = CFRetain(CFSTR("arrowCursor"));
    }

done:
    TRACE("setting cursor with cursor_name %s cursor_frames %p\n", debugstr_cf(cursor_name), cursor_frames);
    macdrv_set_cursor(cursor_name, cursor_frames);
    if (cursor_name) CFRelease(cursor_name);
    if (cursor_frames) CFRelease(cursor_frames);
}


/***********************************************************************
 *              SetCursorPos (MACDRV.@)
 */
BOOL CDECL macdrv_SetCursorPos(INT x, INT y)
{
    BOOL ret = macdrv_set_cursor_position(CGPointMake(x, y));
    if (ret)
        TRACE("warped to %d,%d\n", x, y);
    else
        ERR("failed to warp to %d,%d\n", x, y);
    return ret;
}


/***********************************************************************
 *              macdrv_mouse_button
 *
 * Handler for MOUSE_BUTTON events.
 */
void macdrv_mouse_button(HWND hwnd, const macdrv_event *event)
{
    UINT flags = 0;
    WORD data = 0;

    TRACE("win %p button %d %s at (%d,%d) time %lu (%lu ticks ago)\n", hwnd, event->mouse_button.button,
          (event->mouse_button.pressed ? "pressed" : "released"),
          event->mouse_button.x, event->mouse_button.y,
          event->mouse_button.time_ms, (GetTickCount() - event->mouse_button.time_ms));

    if (event->mouse_button.pressed)
    {
        switch (event->mouse_button.button)
        {
        case 0: flags |= MOUSEEVENTF_LEFTDOWN; break;
        case 1: flags |= MOUSEEVENTF_RIGHTDOWN; break;
        case 2: flags |= MOUSEEVENTF_MIDDLEDOWN; break;
        default:
            flags |= MOUSEEVENTF_XDOWN;
            data = 1 << (event->mouse_button.button - 3);
            break;
        }
    }
    else
    {
        switch (event->mouse_button.button)
        {
        case 0: flags |= MOUSEEVENTF_LEFTUP; break;
        case 1: flags |= MOUSEEVENTF_RIGHTUP; break;
        case 2: flags |= MOUSEEVENTF_MIDDLEUP; break;
        default:
            flags |= MOUSEEVENTF_XUP;
            data = 1 << (event->mouse_button.button - 3);
            break;
        }
    }

    send_mouse_input(hwnd, event->window, flags | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_MOVE,
                     event->mouse_button.x, event->mouse_button.y,
                     data, FALSE, event->mouse_button.time_ms);
}


/***********************************************************************
 *              macdrv_mouse_moved
 *
 * Handler for MOUSE_MOVED and MOUSE_MOVED_ABSOLUTE events.
 */
void macdrv_mouse_moved(HWND hwnd, const macdrv_event *event)
{
    UINT flags = MOUSEEVENTF_MOVE;

    TRACE("win %p/%p %s (%d,%d) drag %d time %lu (%lu ticks ago)\n", hwnd, event->window,
          (event->type == MOUSE_MOVED) ? "relative" : "absolute",
          event->mouse_moved.x, event->mouse_moved.y, event->mouse_moved.drag,
          event->mouse_moved.time_ms, (GetTickCount() - event->mouse_moved.time_ms));

    if (event->type == MOUSE_MOVED_ABSOLUTE)
        flags |= MOUSEEVENTF_ABSOLUTE;

    send_mouse_input(hwnd, event->window, flags, event->mouse_moved.x, event->mouse_moved.y,
                     0, event->mouse_moved.drag, event->mouse_moved.time_ms);
}


/***********************************************************************
 *              macdrv_mouse_scroll
 *
 * Handler for MOUSE_SCROLL events.
 */
void macdrv_mouse_scroll(HWND hwnd, const macdrv_event *event)
{
    TRACE("win %p/%p scroll (%d,%d) at (%d,%d) time %lu (%lu ticks ago)\n", hwnd,
          event->window, event->mouse_scroll.x_scroll, event->mouse_scroll.y_scroll,
          event->mouse_scroll.x, event->mouse_scroll.y,
          event->mouse_scroll.time_ms, (GetTickCount() - event->mouse_scroll.time_ms));

    if (event->mouse_scroll.y_scroll)
        send_mouse_input(hwnd, event->window, MOUSEEVENTF_WHEEL | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_MOVE,
                         event->mouse_scroll.x, event->mouse_scroll.y,
                         event->mouse_scroll.y_scroll, FALSE, event->mouse_scroll.time_ms);
    if (event->mouse_scroll.x_scroll)
        send_mouse_input(hwnd, event->window, MOUSEEVENTF_HWHEEL | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_MOVE,
                         event->mouse_scroll.x, event->mouse_scroll.y,
                         event->mouse_scroll.x_scroll, FALSE, event->mouse_scroll.time_ms);
}


/***********************************************************************
 *              macdrv_release_capture
 *
 * Handler for RELEASE_CAPTURE events.
 */
void macdrv_release_capture(HWND hwnd, const macdrv_event *event)
{
    struct macdrv_thread_data *thread_data = macdrv_thread_data();
    HWND capture = GetCapture();
    HWND capture_top = GetAncestor(capture, GA_ROOT);

    TRACE("win %p/%p thread_data->capture_window %p GetCapture() %p in %p\n", hwnd,
          event->window, thread_data->capture_window, capture, capture_top);

    if (event->window == thread_data->capture_window && hwnd == capture_top)
    {
        ReleaseCapture();
        if (!PostMessageW(capture, WM_CANCELMODE, 0, 0))
            WARN("failed to post WM_CANCELMODE; error 0x%08x\n", GetLastError());
    }
}
