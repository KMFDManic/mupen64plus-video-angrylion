#include <string.h>

#include "z64.h"
#include "rdp.h"
#include "vi.h"
#include "api/libretro.h"

extern retro_log_printf_t log_cb;

onetime onetimewarnings;

UINT8* rdram_8;
UINT16* rdram_16;
UINT32 plim;
UINT32 idxlim16;
UINT32 idxlim32;
UINT8 hidden_bits[0x400000];

UINT32 gamma_table[0x100];
UINT32 gamma_dither_table[0x4000];
INT32 vi_restore_table[0x400];
INT32 oldvstart = 1337;
int32_t* PreScale;

int overlay = 0;

static UINT32 tvfadeoutstate[625];
static UINT32 brightness = 0;
static UINT32 prevwasblank = 0;

STRICTINLINE static void video_filter16(
    int* r, int* g, int* b, UINT32 fboffset, UINT32 num, UINT32 hres,
    UINT32 centercvg);
STRICTINLINE static void video_filter32(
    int* endr, int* endg, int* endb, UINT32 fboffset, UINT32 num, UINT32 hres,
    UINT32 centercvg);
STRICTINLINE static void divot_filter(
    CCVG* final, CCVG centercolor, CCVG leftcolor, CCVG rightcolor);
STRICTINLINE static void restore_filter16(
    int* r, int* g, int* b, UINT32 fboffset, UINT32 num, UINT32 hres);
STRICTINLINE static void restore_filter32(
    int* r, int* g, int* b, UINT32 fboffset, UINT32 num, UINT32 hres);
static void gamma_filters(unsigned char* argb, int gamma_and_dither);
static void adjust_brightness(unsigned char* argb, int brightcoeff);
STRICTINLINE static void vi_vl_lerp(CCVG* up, CCVG down, UINT32 frac);
STRICTINLINE static void video_max_optimized(UINT32* Pixels, UINT32* penumin, UINT32* penumax, int numofels);

STRICTINLINE static void vi_fetch_filter16(
    CCVG* res, UINT32 fboffset, UINT32 cur_x, UINT32 fsaa, UINT32 dither_filter,
    UINT32 vres);
STRICTINLINE static void vi_fetch_filter32(
    CCVG* res, UINT32 fboffset, UINT32 cur_x, UINT32 fsaa, UINT32 dither_filter,
    UINT32 vres);

static void do_frame_buffer_proper(
    UINT32 prescale_ptr, int hres, int vres, int x_start, int vitype,
    int linecount);
static void do_frame_buffer_raw(
    UINT32 prescale_ptr, int hres, int vres, int x_start, int vitype,
    int linecount);
static void (*do_frame_buffer[2])(UINT32, int, int, int, int, int) = {
    do_frame_buffer_raw, do_frame_buffer_proper
};

static void (*vi_fetch_filter_ptr)(
    CCVG*, UINT32, UINT32, UINT32, UINT32, UINT32);
static void (*vi_fetch_filter_func[2])(
    CCVG*, UINT32, UINT32, UINT32, UINT32, UINT32) = {
    vi_fetch_filter16, vi_fetch_filter32
};

void rdp_update(void)
{
    UINT32 prescale_ptr;
    UINT32 pix;
    UINT8 cur_cvg;
    int hres, vres;
    int h_start, v_start;
    int x_start;
    int h_end;
    int two_lines, line_shifter, line_count;
    int hrightblank;
    int vactivelines;
    int validh;
    int serration_pulses;
    int validinterlace;
    int lowerfield;
    register int i, j;
    extern uint32_t *blitter_buf;
    extern uint32_t *blitter_buf_lock;
    const int x_add = *GET_GFX_INFO(VI_X_SCALE_REG) & 0x00000FFF;
    const int v_sync = *GET_GFX_INFO(VI_V_SYNC_REG) & 0x000003FF;
    const int ispal  = (v_sync > 550);
    const int x1 = (*GET_GFX_INFO(VI_H_START_REG) >> 16) & 0x03FF;
    const int y1 = (*GET_GFX_INFO(VI_V_START_REG) >> 16) & 0x03FF;
    const int x2 = (*GET_GFX_INFO(VI_H_START_REG) >>  0) & 0x03FF;
    const int y2 = (*GET_GFX_INFO(VI_V_START_REG) >>  0) & 0x03FF;
    const int delta_x = x2 - x1;
    const int delta_y = y2 - y1;
    const int vitype = *GET_GFX_INFO(VI_STATUS_REG) & 0x00000003;
    const int pixel_size = sizeof(INT32);

/*
 * initial value (angrylion)
 */
    serration_pulses  = !!(*GET_GFX_INFO(VI_STATUS_REG) & 0x00000040);
    serration_pulses &= (y1 != oldvstart);
    two_lines = serration_pulses ^ 0;

    validinterlace = (vitype & 2) && serration_pulses;
    if (!validinterlace)
       internal_vi_v_current_line = 0;
    lowerfield = validinterlace && !(internal_vi_v_current_line & 1);
    if (validinterlace)
       internal_vi_v_current_line ^= 1;

    line_count = pitchindwords << serration_pulses;
    line_shifter = serration_pulses ^ 1;

    hres = delta_x;
    vres = delta_y;
    h_start = x1 - (ispal ? 128 : 108);
    v_start = y1 - (ispal ?  47 :  37);
    x_start = (*gfx_info.VI_X_SCALE_REG >> 16) & 0x00000FFF;

    if (h_start < 0)
    {
        x_start -= x_add * h_start;
        h_start  = 0;
    }
    oldvstart = y1;
    v_start >>= 1;
    v_start  &= -(v_start >= 0);
    vres >>= 1;

    if (hres > PRESCALE_WIDTH - h_start)
        hres = PRESCALE_WIDTH - h_start;
    if (vres > PRESCALE_HEIGHT - v_start)
        vres = PRESCALE_HEIGHT - v_start;
    h_end = hres + h_start;

    hrightblank = PRESCALE_WIDTH - h_end;
    vactivelines = v_sync - (ispal ? 47 : 37);
    if (vactivelines > PRESCALE_HEIGHT)
    {
       if (log_cb)
          log_cb(RETRO_LOG_WARN, "VI_V_SYNC_REG too big\n");
        return;
    }
    if (vactivelines < 0)
    {
       if (log_cb)
          log_cb(RETRO_LOG_WARN, "vactivelines lesser than 0\n");
        return;
    }
    vactivelines >>= line_shifter;
    validh = (hres >= 0 && h_start >= 0 && h_start < PRESCALE_WIDTH);
    pix = 0;
    cur_cvg = 0;
    if (hres <= 0 || vres <= 0 || (!(vitype & 2) && prevwasblank)) /* early return. */
        return;

    if (blitter_buf_lock)
       PreScale = (uint32_t*)blitter_buf_lock;
    else
       PreScale = (uint32_t*)blitter_buf;

    if (vitype >> 1 == 0)
    {
        zerobuf(tvfadeoutstate, pixel_size*PRESCALE_HEIGHT);
        for (i = 0; i < PRESCALE_HEIGHT; i++)
            zerobuf(&PreScale[i * pitchindwords], pixel_size*PRESCALE_WIDTH);
        prevwasblank = 1;
        goto no_frame_buffer;
    }
#undef RENDER_CVG_BITS16
#undef RENDER_CVG_BITS32
#undef RENDER_MIN_CVG_ONLY
#undef RENDER_MAX_CVG_ONLY

#undef MONITOR_Z
#undef BW_ZBUFFER
#undef ZBUFF_AS_16B_IATEXTURE

#ifdef MONITOR_Z
    frame_buffer = zb_address;
#endif

    prevwasblank = 0;
    if (h_start > 0 && h_start < PRESCALE_WIDTH)
        for (i = 0; i < vactivelines; i++)
            zerobuf(&PreScale[i*pitchindwords], pixel_size*h_start);

    if (h_end >= 0 && h_end < PRESCALE_WIDTH)
        for (i = 0; i < vactivelines; i++)
            zerobuf(&PreScale[i*pitchindwords + h_end], pixel_size*hrightblank);

    for (i = 0; i < (v_start << two_lines) + lowerfield; i++)
    {
        tvfadeoutstate[i] >>= 1;
        if (~tvfadeoutstate[i] & validh)
            zerobuf(&PreScale[i*pitchindwords + h_start], pixel_size*hres);
    }

    if (serration_pulses == 0)
        for (j = 0; j < vres; j++)
            tvfadeoutstate[i++] = 2;
    else
        for (j = 0; j < vres; j++)
        {
            tvfadeoutstate[i] = 2;
            ++i;
            tvfadeoutstate[i] >>= 1;
            if (~tvfadeoutstate[i] & validh)
                zerobuf(&PreScale[i*pitchindwords + h_start], pixel_size*hres);
            ++i;
        }

    while (i < vactivelines)
    {
        tvfadeoutstate[i] >>= 1;
        if (~tvfadeoutstate[i] & validh)
            zerobuf(&PreScale[i*pitchindwords + h_start], pixel_size*hres);
        ++i;
    }

    prescale_ptr =
        (v_start * line_count) + h_start + (lowerfield ? pitchindwords : 0);
    do_frame_buffer[overlay](
        prescale_ptr, hres, vres, x_start, vitype, line_count);
no_frame_buffer:

    __src.bottom = (ispal ? 576 : 480) >> line_shifter; /* visible lines */

    if (line_shifter != 0) /* 240p non-interlaced VI DAC mode */
    {
        register signed int cur_line;

        cur_line = 240 - 1;
        while (cur_line >= 0)
        {
            memcpy(
                &PreScale[2*PRESCALE_WIDTH*cur_line + PRESCALE_WIDTH],
                &PreScale[1*PRESCALE_WIDTH*cur_line],
                4 * PRESCALE_WIDTH
            );
            memcpy(
                &PreScale[2*PRESCALE_WIDTH*cur_line + 0],
                &PreScale[1*PRESCALE_WIDTH*cur_line],
                4 * PRESCALE_WIDTH
            );
            --cur_line;
        }
    }
    return;
}

static void do_frame_buffer_proper(
    UINT32 prescale_ptr, int hres, int vres, int x_start, int vitype,
    int linecount)
{
    CCVG viaa_array[2048];
    CCVG divot_array[2048];
    CCVG *viaa_cache, *viaa_cache_next, *divot_cache, *divot_cache_next;
    CCVG *tempccvgptr;
    CCVG color, nextcolor, scancolor, scannextcolor;
    UINT32 * scanline;
    UINT32 pixels = 0, nextpixels = 0;
    UINT32 prevy = 0;
    UINT32 y_start = (vi_y_scale >> 16) & 0x0FFF;
	UINT32 frame_buffer = vi_origin & 0x00FFFFFF;
    signed int cache_marker_init;
    int line_x = 0, next_line_x = 0, prev_line_x = 0, far_line_x = 0;
    int prev_scan_x = 0, scan_x = 0, next_scan_x = 0, far_scan_x = 0;
    int prev_x = 0, cur_x = 0, next_x = 0, far_x = 0;
    int cache_marker = 0, cache_next_marker = 0, divot_cache_marker = 0, divot_cache_next_marker = 0;
    int xfrac = 0, yfrac = 0;
    int slowbright;
    int lerping = 0;
    int vi_width_low = vi_width & 0xFFF;
    const int x_add = *GET_GFX_INFO(VI_X_SCALE_REG) & 0x00000FFF;
    UINT32 y_add = vi_y_scale & 0xfff;
    register int i, j;
    const int gamma_dither     = !!(*GET_GFX_INFO(VI_STATUS_REG) & 0x00000004);
    const int gamma            = !!(*GET_GFX_INFO(VI_STATUS_REG) & 0x00000008);
    const int divot            = !!(*GET_GFX_INFO(VI_STATUS_REG) & 0x00000010);
    const int clock_enable     = !!(*GET_GFX_INFO(VI_STATUS_REG) & 0x00000020);
    const int extralines       =  !(*GET_GFX_INFO(VI_STATUS_REG) & 0x00000100);
    const int fsaa             =  !(*GET_GFX_INFO(VI_STATUS_REG) & 0x00000200);
    const int dither_filter    = !!(*GET_GFX_INFO(VI_STATUS_REG) & 0x00010000);
    const int gamma_and_dither = (gamma << 1) | gamma_dither;
    const int lerp_en          = fsaa | extralines;

    if (frame_buffer == 0)
        return;

    if (clock_enable)
        DisplayError(
            "rdp_update: vbus_clock_enable bit set in VI_CONTROL_REG "\
            "register. Never run this code on your N64! It's rumored that "\
            "turning this bit on will result in permanent damage to the "\
            "hardware! Emulation will now continue.");

    viaa_cache = &viaa_array[0];
    viaa_cache_next = &viaa_array[1024];
    divot_cache = &divot_array[0];
    divot_cache_next = &divot_array[1024];

    cache_marker_init  = (x_start >> 10) - 2;
    cache_marker_init |= -(cache_marker_init < 0);

    slowbright = 0;
#if 0
    if (GetAsyncKeyState(0x91))
        brightness = ++brightness & 0xF;
    slowbright = brightness >> 1;
#endif
    pixels = 0;

    for (j = 0; j < vres; j++)
    {
        x_start = (vi_x_scale >> 16) & 0x0FFF;

        if ((y_start >> 10) == (prevy + 1) && j)
        {
            cache_marker = cache_next_marker;
            cache_next_marker = cache_marker_init;

            tempccvgptr = viaa_cache;
            viaa_cache = viaa_cache_next;
            viaa_cache_next = tempccvgptr;
            if (divot == 0)
                {/* do nothing and branch */}
            else
            {
                divot_cache_marker = divot_cache_next_marker;
                divot_cache_next_marker = cache_marker_init;
                tempccvgptr = divot_cache;
                divot_cache = divot_cache_next;
                divot_cache_next = tempccvgptr;
            }
        }
        else if ((y_start >> 10) != prevy || !j)
        {
            cache_marker = cache_next_marker = cache_marker_init;
            if (divot == 0)
                {/* do nothing and branch */}
            else
                divot_cache_marker
              = divot_cache_next_marker
              = cache_marker_init;
        }

        scanline = &PreScale[prescale_ptr];
        prescale_ptr += linecount;

        prevy = y_start >> 10;
        yfrac = (y_start >> 5) & 0x1f;
        pixels = vi_width_low * prevy;
        nextpixels = pixels + vi_width_low;

        for (i = 0; i < hres; i++)
        {
            unsigned char argb[4];

            line_x = x_start >> 10;
            prev_line_x = line_x - 1;
            next_line_x = line_x + 1;
            far_line_x = line_x + 2;

            cur_x = pixels + line_x;
            prev_x = pixels + prev_line_x;
            next_x = pixels + next_line_x;
            far_x = pixels + far_line_x;

            scan_x = nextpixels + line_x;
            prev_scan_x = nextpixels + prev_line_x;
            next_scan_x = nextpixels + next_line_x;
            far_scan_x = nextpixels + far_line_x;

            xfrac = (x_start >> 5) & 0x1f;
            lerping = lerp_en & (xfrac || yfrac);

            if (prev_line_x > cache_marker)
            {
                vi_fetch_filter_func[vitype & 1](
                    &viaa_cache[prev_line_x], frame_buffer, prev_x, fsaa,
                    dither_filter, vres);
                vi_fetch_filter_func[vitype & 1](
                    &viaa_cache[line_x], frame_buffer, cur_x, fsaa,
                    dither_filter, vres);
                vi_fetch_filter_func[vitype & 1](
                    &viaa_cache[next_line_x], frame_buffer, next_x, fsaa,
                    dither_filter, vres);
                cache_marker = next_line_x;
            }
            else if (line_x > cache_marker)
            {
                vi_fetch_filter_func[vitype & 1](
                    &viaa_cache[line_x], frame_buffer, cur_x, fsaa,
                    dither_filter, vres);
                vi_fetch_filter_func[vitype & 1](
                    &viaa_cache[next_line_x], frame_buffer, next_x, fsaa,
                    dither_filter, vres);
                cache_marker = next_line_x;
            }
            else if (next_line_x > cache_marker)
            {
                vi_fetch_filter_func[vitype & 1](
                    &viaa_cache[next_line_x], frame_buffer, next_x, fsaa,
                    dither_filter, vres);
                cache_marker = next_line_x;
            }

            if (prev_line_x > cache_next_marker)
            {
                vi_fetch_filter_func[vitype & 1](
                    &viaa_cache_next[prev_line_x], frame_buffer, prev_scan_x,
                    fsaa, dither_filter, vres);
                vi_fetch_filter_func[vitype & 1](
                    &viaa_cache_next[line_x], frame_buffer, scan_x, fsaa,
                    dither_filter, vres);
                vi_fetch_filter_func[vitype & 1](
                    &viaa_cache_next[next_line_x], frame_buffer, next_scan_x,
                    fsaa, dither_filter, vres);
                cache_next_marker = next_line_x;
            }
            else if (line_x > cache_next_marker)
            {
                vi_fetch_filter_func[vitype & 1](
                    &viaa_cache_next[line_x], frame_buffer, scan_x, fsaa,
                    dither_filter, vres);
                vi_fetch_filter_func[vitype & 1](
                    &viaa_cache_next[next_line_x], frame_buffer, next_scan_x,
                    fsaa, dither_filter, vres);
                cache_next_marker = next_line_x;
            }
            else if (next_line_x > cache_next_marker)
            {
                vi_fetch_filter_func[vitype & 1](
                    &viaa_cache_next[next_line_x], frame_buffer, next_scan_x,
                    fsaa, dither_filter, vres);
                cache_next_marker = next_line_x;
            }

            if (divot == 0)
                color = viaa_cache[line_x];
            else
            {
                if (far_line_x > cache_marker)
                {
                    vi_fetch_filter_func[vitype & 1](
                        &viaa_cache[far_line_x], frame_buffer, far_x, fsaa,
                        dither_filter, vres);
                    cache_marker = far_line_x;
                }

                if (far_line_x > cache_next_marker)
                {
                    vi_fetch_filter_func[vitype & 1](
                        &viaa_cache_next[far_line_x], frame_buffer, far_scan_x,
                        fsaa, dither_filter, vres);
                    cache_next_marker = far_line_x;
                }

                if (line_x > divot_cache_marker)
                {
                    divot_filter(
                        &divot_cache[line_x], viaa_cache[line_x],
                        viaa_cache[prev_line_x], viaa_cache[next_line_x]);
                    divot_filter(
                        &divot_cache[next_line_x], viaa_cache[next_line_x],
                        viaa_cache[line_x], viaa_cache[far_line_x]);
                    divot_cache_marker = next_line_x;
                }
                else if (next_line_x > divot_cache_marker)
                {
                    divot_filter(
                        &divot_cache[next_line_x], viaa_cache[next_line_x],
                        viaa_cache[line_x], viaa_cache[far_line_x]);
                    divot_cache_marker = next_line_x;
                }

                if (line_x > divot_cache_next_marker)
                {
                    divot_filter(
                        &divot_cache_next[line_x], viaa_cache_next[line_x],
                        viaa_cache_next[prev_line_x],
                        viaa_cache_next[next_line_x]);
                    divot_filter(
                        &divot_cache_next[next_line_x],
                        viaa_cache_next[next_line_x], viaa_cache_next[line_x],
                        viaa_cache_next[far_line_x]);
                    divot_cache_next_marker = next_line_x;
                }
                else if (next_line_x > divot_cache_next_marker)
                {
                    divot_filter(
                        &divot_cache_next[next_line_x],
                        viaa_cache_next[next_line_x], viaa_cache_next[line_x],
                        viaa_cache_next[far_line_x]);
                    divot_cache_next_marker = next_line_x;
                }
                color = divot_cache[line_x];
            }

            if (lerping)
            {
                if (divot == 0)
                { /* branch unlikely */
                    nextcolor = viaa_cache[next_line_x];
                    scancolor = viaa_cache_next[line_x];
                    scannextcolor = viaa_cache_next[next_line_x];
                }
                else
                {
                    nextcolor = divot_cache[next_line_x];
                    scancolor = divot_cache_next[line_x];
                    scannextcolor = divot_cache_next[next_line_x];
                }
                if (yfrac == 0)
                    {}
                else
                {
                    vi_vl_lerp(&color, scancolor, yfrac);
                    vi_vl_lerp(&nextcolor, scannextcolor, yfrac);
                }
                if (xfrac == 0)
                    {}
                else
                    vi_vl_lerp(&color, nextcolor, xfrac);
            }
            argb[1 ^ BYTE_ADDR_XOR] = color.r;
            argb[2 ^ BYTE_ADDR_XOR] = color.g;
            argb[3 ^ BYTE_ADDR_XOR] = color.b;

            gamma_filters(argb, gamma_and_dither);
#ifdef BW_ZBUFFER
            UINT32 tempz = RREADIDX16((frame_buffer >> 1) + cur_x);

            pix = tempz;
            argb[1 ^ 3] = argb[2 ^ 3] = argb[3 ^ 3] = pix >> 8;
#endif
#ifdef ZBUFF_AS_16B_IATEXTURE
            argb[1 ^ 3] = argb[2 ^ 3] = argb[3 ^ 3] =
                (unsigned char)(pix >> 8)*(unsigned char)(pix >> 0) >> 8;
#endif
#ifdef RENDER_CVG_BITS16
            argb[1 ^ 3] = argb[2 ^ 3] = argb[3 ^ 3] = cur_cvg << 5;
#endif
#ifdef RENDER_CVG_BITS32
            argb[1 ^ 3] = argb[2 ^ 3] = argb[3 ^ 3] = cur_cvg << 5;
#endif
#ifdef RENDER_MIN_CVG_ONLY
            if (!cur_cvg)
                argb[1 ^ 3] = argb[2 ^ 3] = argb[3 ^ 3] = 0x00;
            else
                argb[1 ^ 3] = argb[2 ^ 3] = argb[3 ^ 3] = 0xFF;
#endif
#ifdef RENDER_MAX_CVG_ONLY
            if (cur_cvg != 7)
                argb[1 ^ 3] = argb[2 ^ 3] = argb[3 ^ 3] = 0x00;
            else
                argb[1 ^ 3] = argb[2 ^ 3] = argb[3 ^ 3] = 0xFF;
#endif
            x_start += x_add;
            scanline[i] = *(INT32 *)(argb);
            if (slowbright == 0)
                continue;
            adjust_brightness(argb, slowbright);
            scanline[i] = *(INT32 *)(argb);
        }
        y_start += y_add;
    }
}
static void do_frame_buffer_raw(
    UINT32 prescale_ptr, int hres, int vres, int x_start, int vitype,
    int linecount)
{
    UINT32 * scanline;
    int pixels;
    int prevy, y_start;
    int cur_x, line_x;
    register int i;
    const int frame_buffer = *GET_GFX_INFO(VI_ORIGIN_REG) & 0x00FFFFFF;
    const int VI_width = *GET_GFX_INFO(VI_WIDTH_REG) & 0x00000FFF;
    const int x_add = *GET_GFX_INFO(VI_X_SCALE_REG) & 0x00000FFF;
    const int y_add = *GET_GFX_INFO(VI_Y_SCALE_REG) & 0x00000FFF;

    if (frame_buffer == 0)
        return;
    y_start = *GET_GFX_INFO(VI_Y_SCALE_REG)>>16 & 0x0FFF;

    if (vitype & 1) /* 32-bit RGBA (branch unlikely) */
    {
        while (--vres >= 0)
        {
            x_start = *GET_GFX_INFO(VI_X_SCALE_REG)>>16 & 0x0FFF;
            scanline = &PreScale[prescale_ptr];
            prescale_ptr += linecount;

            prevy = y_start >> 10;
            pixels = VI_width * prevy;

            for (i = 0; i < hres; i++)
            {
                unsigned long pix;
                unsigned long addr;
                unsigned char argb[4];

                line_x = x_start >> 10;
                cur_x = pixels + line_x;

                x_start += x_add;
                addr = frame_buffer + 4*cur_x;
                if (plim - addr < 0)
                    continue;
                pix = *(INT32 *)(DRAM + addr);
                argb[1 ^ BYTE_ADDR_XOR] = (unsigned char)(pix >> 24);
                argb[2 ^ BYTE_ADDR_XOR] = (unsigned char)(pix >> 16);
                argb[3 ^ BYTE_ADDR_XOR] = (unsigned char)(pix >>  8);
                argb[0 ^ BYTE_ADDR_XOR] = (unsigned char)(pix >>  0);
                scanline[i] = *(INT32 *)(argb);
            }
            y_start += y_add;
        }
    }
    else /* 16-bit RRRRR GGGGG BBBBB A */
    {
        while (--vres >= 0)
        {
            x_start = *GET_GFX_INFO(VI_X_SCALE_REG)>>16 & 0x0FFF;
            scanline = &PreScale[prescale_ptr];
            prescale_ptr += linecount;

            prevy = y_start >> 10;
            pixels = VI_width * prevy;

            for (i = 0; i < hres; i++)
            {
                unsigned short pix;
                unsigned long addr;
                unsigned char argb[4];

                line_x = x_start >> 10;
                cur_x = pixels + line_x;

                x_start += x_add;
                addr = frame_buffer + 2*cur_x;
                if (plim - addr < 0)
                    continue;
                addr = addr ^ (WORD_ADDR_XOR << 1);
                pix = *(INT16 *)(DRAM + addr);
                argb[1 ^ BYTE_ADDR_XOR] = (unsigned char)(pix >> 8);
                argb[2 ^ BYTE_ADDR_XOR] = (unsigned char)(pix >> 3) & ~7;
                argb[3 ^ BYTE_ADDR_XOR] = (unsigned char)(pix & ~1) << 2;
                scanline[i] = *(INT32 *)(argb);
            }
            y_start += y_add;
        }
    }
    return;
}

STRICTINLINE static void vi_fetch_filter16(
    CCVG* res, UINT32 fboffset, UINT32 cur_x, UINT32 fsaa, UINT32 dither_filter,
    UINT32 vres)
{
    int r, g, b;
    UINT32 pix, hval;
    UINT32 cur_cvg;
    UINT32 idx = (fboffset >> 1) + cur_x;
    UINT32 fbw = vi_width & 0xfff;

    if (fsaa)
    {
       PAIRREAD16(pix, hval, idx); 
       cur_cvg = ((pix & 1) << 2) | hval;
    }
    else
    {
       RREADIDX16(pix, idx);
       cur_cvg = 7;
    }
    r = GET_HI(pix);
    g = GET_MED(pix);
    b = GET_LOW(pix);

    if (cur_cvg == 7)
    {
        if (dither_filter)
            restore_filter16(&r, &g, &b, fboffset, cur_x, fbw);
    }
    else
    {
        video_filter16(&r, &g, &b, fboffset, cur_x, fbw, cur_cvg);
    }

    res -> r = r;
    res -> g = g;
    res -> b = b;
    res -> cvg = cur_cvg;
    return;
}

STRICTINLINE static void vi_fetch_filter32(
    CCVG* res, UINT32 fboffset, UINT32 cur_x, UINT32 fsaa, UINT32 dither_filter,
    UINT32 vres)
{
    int r, g, b;
    UINT32 cur_cvg;
    UINT32 fbw = vi_width & 0xfff;
    UINT32 pix, addr = (fboffset >> 2) + cur_x;
    RREADIDX32(pix, addr);

    if (fsaa)
        cur_cvg = (pix >> 5) & 7;
    else
        cur_cvg = 7;

    r = (pix >> 24) & 0xff;
    g = (pix >> 16) & 0xff;
    b = (pix >> 8) & 0xff;

    if (cur_cvg == 7)
    {
        if (dither_filter)
            restore_filter32(&r, &g, &b, fboffset, cur_x, fbw);
    }
    else
    {
        video_filter32(&r, &g, &b, fboffset, cur_x, fbw, cur_cvg);
    }

    res -> r = r;
    res -> g = g;
    res -> b = b;
    res -> cvg = cur_cvg;
    return;
}

STRICTINLINE static void video_filter16(
    int* endr, int* endg, int* endb, UINT32 fboffset, UINT32 num, UINT32 hres,
    UINT32 centercvg)
{
    UINT32 penumaxr, penumaxg, penumaxb, penuminr, penuming, penuminb;
    UINT16 pix;
    UINT32 numoffull = 1;
    UINT32 hidval;
    UINT32 r, g, b; 
    UINT32 backr[7], backg[7], backb[7];
    UINT32 colr, colg, colb;

    UINT32 idx = (fboffset >> 1) + num;
    UINT32 leftup = idx - hres - 1;
    UINT32 rightup = idx - hres + 1;
    UINT32 toleft = idx - 2;
    UINT32 toright = idx + 2;
    UINT32 leftdown = idx + hres - 1;
    UINT32 rightdown = idx + hres + 1;
    UINT32 coeff = 7 - centercvg;

    r = *endr;
    g = *endg;
    b = *endb;

    backr[0] = r;
    backg[0] = g;
    backb[0] = b;

    VI_ANDER(leftup);
    VI_ANDER(rightup);
    VI_ANDER(toleft);
    VI_ANDER(toright);
    VI_ANDER(leftdown);
    VI_ANDER(rightdown);

    video_max_optimized(backr, &penuminr, &penumaxr, numoffull);
	video_max_optimized(backg, &penuming, &penumaxg, numoffull);
	video_max_optimized(backb, &penuminb, &penumaxb, numoffull);

    colr = penuminr + penumaxr - (r << 1);
    colg = penuming + penumaxg - (g << 1);
    colb = penuminb + penumaxb - (b << 1);

    colr = (((colr * coeff) + 4) >> 3) + r;
    colg = (((colg * coeff) + 4) >> 3) + g;
    colb = (((colb * coeff) + 4) >> 3) + b;

    *endr = colr & 0xFF;
    *endg = colg & 0xFF;
    *endb = colb & 0xFF;
    return;
}

STRICTINLINE static void video_filter32(
    int* endr, int* endg, int* endb, UINT32 fboffset, UINT32 num, UINT32 hres,
    UINT32 centercvg)
{
    UINT32 penumaxr, penumaxg, penumaxb, penuminr, penuming, penuminb;
    UINT32 numoffull = 1;
    UINT32 pix = 0, pixcvg = 0;
    UINT32 r, g, b; 
    UINT32 backr[7], backg[7], backb[7];
    UINT32 colr, colg, colb;

    UINT32 idx = (fboffset >> 2) + num;
    UINT32 leftup = idx - hres - 1;
    UINT32 rightup = idx - hres + 1;
    UINT32 toleft = idx - 2;
    UINT32 toright = idx + 2;
    UINT32 leftdown = idx + hres - 1;
    UINT32 rightdown = idx + hres + 1;
    UINT32 coeff = 7 - centercvg;

    r = *endr;
    g = *endg;
    b = *endb;

    backr[0] = r;
    backg[0] = g;
    backb[0] = b;

    VI_ANDER32(leftup);
    VI_ANDER32(rightup);
    VI_ANDER32(toleft);
    VI_ANDER32(toright);
    VI_ANDER32(leftdown);
    VI_ANDER32(rightdown);

    video_max_optimized(backr, &penuminr, &penumaxr, numoffull);
    video_max_optimized(backg, &penuming, &penumaxg, numoffull);
    video_max_optimized(backb, &penuminb, &penumaxb, numoffull);

    colr = penuminr + penumaxr - (r << 1);
    colg = penuming + penumaxg - (g << 1);
    colb = penuminb + penumaxb - (b << 1);

    colr = (((colr * coeff) + 4) >> 3) + r;
    colg = (((colg * coeff) + 4) >> 3) + g;
    colb = (((colb * coeff) + 4) >> 3) + b;

    *endr = colr & 0xFF;
    *endg = colg & 0xFF;
    *endb = colb & 0xFF;
    return;
}

STRICTINLINE static void divot_filter(
    CCVG* final, CCVG centercolor, CCVG leftcolor, CCVG rightcolor)
{
    UINT32 leftr, leftg, leftb;
    UINT32 rightr, rightg, rightb;
    UINT32 centerr, centerg, centerb;

    *final = centercolor;
    if ((centercolor.cvg & leftcolor.cvg & rightcolor.cvg) == 7)
        return;

    leftr = leftcolor.r;    
    leftg = leftcolor.g;    
    leftb = leftcolor.b;
    rightr = rightcolor.r;    
    rightg = rightcolor.g;    
    rightb = rightcolor.b;
    centerr = centercolor.r;
    centerg = centercolor.g;
    centerb = centercolor.b;

    if ((leftr >= centerr && rightr >= leftr) || (leftr >= rightr && centerr >= leftr))
        final -> r = leftr;
    else if ((rightr >= centerr && leftr >= rightr) || (rightr >= leftr && centerr >= rightr))
        final -> r = rightr;

    if ((leftg >= centerg && rightg >= leftg) || (leftg >= rightg && centerg >= leftg))
        final -> g = leftg;
    else if ((rightg >= centerg && leftg >= rightg) || (rightg >= leftg && centerg >= rightg))
        final -> g = rightg;

    if ((leftb >= centerb && rightb >= leftb) || (leftb >= rightb && centerb >= leftb))
        final -> b = leftb;
    else if ((rightb >= centerb && leftb >= rightb) || (rightb >= leftb && centerb >= rightb))
        final -> b = rightb;
    return;
}

STRICTINLINE static void restore_filter16(
    int* r, int* g, int* b, UINT32 fboffset, UINT32 num, UINT32 hres)
{
    UINT32 tempr, tempg, tempb;
    UINT16 pix;
    UINT32 addr;

    UINT32 idx = (fboffset >> 1) + num;
    UINT32 leftuppix = idx - hres - 1;
    UINT32 leftdownpix = idx + hres - 1;
    UINT32 toleftpix = idx - 1;
    UINT32 maxpix = idx + hres + 1;

    INT32 rend = *r;
	INT32 gend = *g;
	INT32 bend = *b;
	const INT32* redptr = &vi_restore_table[(rend << 2) & 0x3e0];
	const INT32* greenptr = &vi_restore_table[(gend << 2) & 0x3e0];
	const INT32* blueptr = &vi_restore_table[(bend << 2) & 0x3e0];

    if (maxpix <= idxlim16 && leftuppix <= idxlim16)
	{
		VI_COMPARE_OPT(leftuppix);
		VI_COMPARE_OPT(leftuppix + 1);
		VI_COMPARE_OPT(leftuppix + 2);
		VI_COMPARE_OPT(leftdownpix);
		VI_COMPARE_OPT(leftdownpix + 1);
		VI_COMPARE_OPT(maxpix);
		VI_COMPARE_OPT(toleftpix);
		VI_COMPARE_OPT(toleftpix + 2);
	}
	else
	{
		VI_COMPARE(leftuppix);
		VI_COMPARE(leftuppix + 1);
		VI_COMPARE(leftuppix + 2);
		VI_COMPARE(leftdownpix);
		VI_COMPARE(leftdownpix + 1);
		VI_COMPARE(maxpix);
		VI_COMPARE(toleftpix);
		VI_COMPARE(toleftpix + 2);
	}

    *r = rend;
    *g = gend;
    *b = bend;
    return;
}

STRICTINLINE static void restore_filter32(
    int* r, int* g, int* b, UINT32 fboffset, UINT32 num, UINT32 hres)
{
    UINT32 tempr, tempg, tempb;
    UINT32 pix, addr;

    UINT32 idx = (fboffset >> 2) + num;
    UINT32 leftuppix = idx - hres - 1;
    UINT32 leftdownpix = idx + hres - 1;
    UINT32 toleftpix = idx - 1;
    UINT32 maxpix = idx + hres + 1;

    INT32 rend = *r;
    INT32 gend = *g;
    INT32 bend = *b;
    const INT32* redptr = &vi_restore_table[(rend << 2) & 0x3e0];
    const INT32* greenptr = &vi_restore_table[(gend << 2) & 0x3e0];
    const INT32* blueptr = &vi_restore_table[(bend << 2) & 0x3e0];


    if (maxpix <= idxlim32 && leftuppix <= idxlim32)
	{
		VI_COMPARE32_OPT(leftuppix);
		VI_COMPARE32_OPT(leftuppix + 1);
		VI_COMPARE32_OPT(leftuppix + 2);
		VI_COMPARE32_OPT(leftdownpix);
		VI_COMPARE32_OPT(leftdownpix + 1);
		VI_COMPARE32_OPT(maxpix);
		VI_COMPARE32_OPT(toleftpix);
		VI_COMPARE32_OPT(toleftpix + 2);
	}
	else
	{
		VI_COMPARE32(leftuppix);
		VI_COMPARE32(leftuppix + 1);
		VI_COMPARE32(leftuppix + 2);
		VI_COMPARE32(leftdownpix);
		VI_COMPARE32(leftdownpix + 1);
		VI_COMPARE32(maxpix);
		VI_COMPARE32(toleftpix);
		VI_COMPARE32(toleftpix + 2);
	}


    *r = rend;
    *g = gend;
    *b = bend;
    return;
}

static void gamma_filters(unsigned char* argb, int gamma_and_dither)
{
    int cdith, dith;
    int r, g, b;

    r = argb[1 ^ BYTE_ADDR_XOR];
    g = argb[2 ^ BYTE_ADDR_XOR];
    b = argb[3 ^ BYTE_ADDR_XOR];
    switch(gamma_and_dither)
    {
        case 0:
            return;
            break;
        case 1:
            cdith = irand();
            dith = cdith & 1;
            if (r < 255)
                r += dith;
            dith = (cdith >> 1) & 1;
            if (g < 255)
                g += dith;
            dith = (cdith >> 2) & 1;
            if (b < 255)
                b += dith;
            break;
        case 2:
            r = gamma_table[r];
            g = gamma_table[g];
            b = gamma_table[b];
            break;
        case 3:
            cdith = irand();
            dith = cdith & 0x3f;
            r = gamma_dither_table[(r << 6) | dith];
            dith = (cdith >> 6) & 0x3f;
            g = gamma_dither_table[(g << 6) | dith];
            dith = ((cdith >> 9) & 0x38) | (cdith & 7);
            b = gamma_dither_table[(b << 6) | dith];
            break;
    }
    argb[1 ^ BYTE_ADDR_XOR] = (unsigned char)(r);
    argb[2 ^ BYTE_ADDR_XOR] = (unsigned char)(g);
    argb[3 ^ BYTE_ADDR_XOR] = (unsigned char)(b);
    return;
}

static void adjust_brightness(unsigned char* argb, int brightcoeff)
{
    int r, g, b;

    r = argb[1 ^ BYTE_ADDR_XOR];
    g = argb[2 ^ BYTE_ADDR_XOR];
    b = argb[3 ^ BYTE_ADDR_XOR];
    brightcoeff &= 7;
    switch (brightcoeff)
    {
        case 0:    
            break;
        case 1: 
        case 2:
        case 3:
            r += (r >> (4 - brightcoeff));
            g += (g >> (4 - brightcoeff));
            b += (b >> (4 - brightcoeff));
            if (r > 0xFF)
                r = 0xFF;
            if (g > 0xFF)
                g = 0xFF;
            if (b > 0xFF)
                b = 0xFF;
            break;
        case 4:
        case 5:
        case 6:
        case 7:
            r = (r + 1) << (brightcoeff - 3);
            g = (g + 1) << (brightcoeff - 3);
            b = (b + 1) << (brightcoeff - 3);
            if (r > 0xFF)
                r = 0xFF;
            if (g > 0xFF)
                g = 0xFF;
            if (b > 0xFF)
                b = 0xFF;
            break;
    }
    argb[1 ^ BYTE_ADDR_XOR] = (unsigned char)(r);
    argb[2 ^ BYTE_ADDR_XOR] = (unsigned char)(g);
    argb[3 ^ BYTE_ADDR_XOR] = (unsigned char)(b);
    return;
}

STRICTINLINE static void vi_vl_lerp(CCVG* up, CCVG down, UINT32 frac)
{
    UINT32 r0, g0, b0;

    if (frac == 0)
        return;

    r0 = up -> r;
    g0 = up -> g;
    b0 = up -> b;

    up -> r = (((frac*(down.r - r0) + 16) >> 5) + r0) & 0xFF;
    up -> g = (((frac*(down.g - g0) + 16) >> 5) + g0) & 0xFF;
    up -> b = (((frac*(down.b - b0) + 16) >> 5) + b0) & 0xFF;
    return;
}

STRICTINLINE void video_max_optimized(UINT32* pixels, UINT32* penumin, UINT32* penumax, int numofels)
{
	int i;
	UINT32 max, min;
	int posmax = 0, posmin = 0;
	UINT32 curpenmax = pixels[0], curpenmin = pixels[0];

	for (i = 1; i < numofels; i++)
	{
	    if (pixels[i] > pixels[posmax])
		{
			curpenmax = pixels[posmax];
			posmax = i;			
		}
		else if (pixels[i] < pixels[posmin])
		{
			curpenmin = pixels[posmin];
			posmin = i;
		}
	}
	max = pixels[posmax];
	min = pixels[posmin];
	if (curpenmax != max)
	{
		for (i = posmax + 1; i < numofels; i++)
		{
			if (pixels[i] > curpenmax)
				curpenmax = pixels[i];
		}
	}
	if (curpenmin != min)
	{
		for (i = posmin + 1; i < numofels; i++)
		{
			if (pixels[i] < curpenmin)
				curpenmin = pixels[i];
		}
	}
	*penumax = curpenmax;
	*penumin = curpenmin;
}

NOINLINE void DisplayError(char * error)
{
    //MessageBox(NULL, error, NULL, MB_ICONERROR);
    return;
}

NOINLINE void zerobuf(void * memory, size_t length)
{
    memset(memory, 0, length);
}
