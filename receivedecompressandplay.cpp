/*
 *  Copyright (c) 2010 The WebM project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

/*
 * This example illustrates using VP8 in a packet loss scenario by xmitting
 * video over UDP with Forward Error Correction,  Packet Resend, and
 * some Unique VP8 functionality.
 *
 */

#include "tctypes.h"
#include "vpx_network.h"
#include <stdio.h>
#include <ctype.h>  //for tolower
#include <string.h>


extern "C"
{
#include "rtp.h"
#include "vpx/vpx_decoder.h"
#include "vpx/vp8dx.h"
}

typedef struct
{
    unsigned int seq;
    unsigned short arrival;
    unsigned int retry;
    unsigned short age;
    unsigned int received;
    unsigned int given_up;
} SKIPS;

#define SS 256
#define SSM (SS-1)
#define PS 2048
#define PSM  (PS-1)
#define MAX_NUMERATOR 16
#define HRE(y) if(FAILED(hr=y)) {vpxlog_dbg(ERRORS,#y##":%x\n",hr);};

unsigned short first_seq_ever = 0;
unsigned int lag_In_milli_seconds = 0;
unsigned int first_time_stamp_ever = 0;
unsigned int time_of_first_display = 0;
int given_up = 0;
int givenup_skip = 0;
int display_width  = 640;
int display_height = 480;
int capture_frame_rate = 30;
int video_bitrate = 300;
int fec_numerator = 6;
int fec_denominator = 5;
int skip_timeout = 800;
int retry_interval = 50;
unsigned short retry_count = 12;
int drop_simulation = 0;
unsigned short send_port = 1408;
unsigned short recv_port = 1407;
unsigned int quit = 0;
int signalquit = 1;

unsigned char compressed_video_buffer[400000];
unsigned char output_video_buffer[1280*1024*3];
tc8 one_packet[8000];

#ifdef WINDOWS

#include "stdafx.h"
#include <conio.h>
#include <mmsystem.h>
#include <atlbase.h>    // ATL CComPtr
#include <ddraw.h>
CComPtr<IDirectDraw7> direct_draw;
DDCAPS caps;
CComPtr<IDirectDrawSurface7> primary_surface, overlay_surface;
CComPtr<IDirectDrawClipper> clipper;
DDOVERLAYFX overlay_fx;
DWORD overlay_flags;
DDSURFACEDESC2 ddsd;
HANDLE thread;
DWORD thread_id;
HWND hwnd;
MSG msg;
WNDCLASS wc;
RECT client_rect;
LRESULT APIENTRY main_wnd_proc(HWND hwnd, UINT msg, UINT parm1, LONG parm2)
{
    INPUT_RECORD ir;
    HANDLE console_input;
    unsigned int count;

    switch (msg)
    {
    case WM_DESTROY:
        console_input = GetStdHandle(STD_INPUT_HANDLE);
        ir.EventType = KEY_EVENT;
        ir.Event.KeyEvent.uChar.AsciiChar = 'q';
        WriteConsoleInput(console_input,  &ir, 1, (LPDWORD) &count);
        PostQuitMessage(0);
        break;
    case WM_MOVE:
        GetWindowRect(hwnd, &client_rect);
        DefWindowProc(hwnd, msg, parm1, parm2);
        break;
    }

    return DefWindowProc(hwnd, msg, parm1, parm2);

}
char app_name[] = "ReceiveDecompressAndPlay";
void display_win_main(void *dummy)
{
    wc.style = CS_BYTEALIGNWINDOW;
    wc.lpfnWndProc = main_wnd_proc;
    wc.cbClsExtra = 0;
    wc.cbWndExtra = 0;
    wc.hInstance = 0;
    wc.hbrBackground = (HBRUSH) GetStockObject(WHITE_BRUSH);
    wc.lpszMenuName = NULL;
    wc.lpszClassName = app_name;
    RegisterClass(&wc);

    hwnd =  CreateWindow(app_name, app_name, WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX,
                         0, 0, display_width + 9, display_height + 30, NULL, NULL, 0, NULL);

    if (hwnd == NULL)
        ExitThread(-1);

    while (GetMessage(&(msg), NULL, 0, 0))
    {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

}
void setup_surface(void)
{
    HRESULT hr;
    thread = CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE) display_win_main, (LPVOID) NULL, 0, &thread_id);

    HRE(DirectDrawCreateEx(0, (void **)&direct_draw, IID_IDirectDraw7, 0));
    ZeroMemory(&caps, sizeof(caps));
    caps.dwSize = sizeof(caps);
    HRE(direct_draw->GetCaps(&caps, 0));
    HRE(direct_draw->SetCooperativeLevel(0, DDSCL_NORMAL));

    // Create the primary surface
    DDSURFACEDESC2 ddsd;
    ZeroMemory(&ddsd, sizeof(ddsd));
    ddsd.dwSize = sizeof(ddsd);
    ddsd.dwFlags = DDSD_CAPS;
    ddsd.ddsCaps.dwCaps = DDSCAPS_PRIMARYSURFACE;
    HRE(direct_draw->CreateSurface(&ddsd, &primary_surface, 0));

    direct_draw->CreateClipper(0, &clipper, NULL);
    clipper->SetHWnd(0, hwnd);
    primary_surface->SetClipper(clipper);

    // Setup the overlay surface's attributes in the surface descriptor
    ZeroMemory(&ddsd, sizeof(ddsd));
    ddsd.dwSize = sizeof(ddsd);
    ddsd.dwFlags = DDSD_CAPS | DDSD_HEIGHT | DDSD_WIDTH | DDPF_YUV | DDSD_PIXELFORMAT;
    ddsd.ddsCaps.dwCaps = DDSCAPS_OFFSCREENPLAIN | DDSCAPS_VIDEOMEMORY;
    ddsd.dwWidth = display_width;
    ddsd.dwHeight = display_height;
    ddsd.ddpfPixelFormat.dwSize = sizeof(DDPIXELFORMAT);
    ddsd.ddpfPixelFormat.dwFlags  = DDPF_FOURCC | DDPF_YUV;
    ddsd.ddpfPixelFormat.dwFourCC = MAKEFOURCC('Y', 'V', '1', '2');

    // Attempt to create the surface with theses settings
    HRE(direct_draw->CreateSurface(&ddsd, &overlay_surface, 0));

}
#define INIT_DXSTRUCT(dxs) { ZeroMemory(&dxs, sizeof(dxs)); dxs.dwSize = sizeof(dxs); }

int show_frame(vpx_image_t *img)
{
    DDSURFACEDESC2 ddsd;
    INIT_DXSTRUCT(ddsd);

    HRESULT hr = overlay_surface->Lock(0, &ddsd, DDLOCK_DONOTWAIT | DDLOCK_SURFACEMEMORYPTR | DDLOCK_WRITEONLY, 0);

    if (SUCCEEDED(hr))
    {
        unsigned char *out = (unsigned char *) ddsd.lpSurface;
        unsigned char *in = img->planes[PLANE_Y];

        for (DWORD i = 0; i < ddsd.dwHeight; i++, out += ddsd.lPitch, in += img->stride[PLANE_Y])
        {
            memcpy(out, in , ddsd.dwWidth);
        }

        in = img->planes[PLANE_U];

        for (DWORD i = 0; i < ddsd.dwHeight / 2; i++, out += ddsd.lPitch / 2, in += img->stride[PLANE_U])
        {
            memcpy(out, in , ddsd.dwWidth / 2);
        }

        in = img->planes[PLANE_V];

        for (DWORD i = 0; i < ddsd.dwHeight / 2; i++, out += ddsd.lPitch / 2, in += img->stride[PLANE_V])
        {
            memcpy(out, in , ddsd.dwWidth / 2);
        }

        HRE(overlay_surface->Unlock(0));

        RECT dest_rect;
        dest_rect.left   = 0;
        dest_rect.top    = 0;
        dest_rect.right  = display_width;
        dest_rect.bottom = display_height;
        RECT src_rect = dest_rect;

        primary_surface->Blt(&client_rect, overlay_surface, &src_rect,  DDBLT_ASYNC, NULL);
    }
    else
    {
        switch (hr)
        {
        case DDERR_INVALIDOBJECT:
            printf("DDERR_INVALIDOBJECT\n");
            break;
        case DDERR_INVALIDPARAMS:
            printf("DDERR_INVALIDPARAMS\n");
            break;
        case DDERR_OUTOFMEMORY:
            printf("DDERR_OUTOFMEMORY\n");
            break;
        case DDERR_SURFACEBUSY:
            printf("DDERR_SURFACEBUSY\n");
            break;
        case DDERR_SURFACELOST:
            printf("DDERR_SURFACELOST\n");
            break;
        case DDERR_WASSTILLDRAWING:
            printf("DDERR_WASSTILLDRAWING\n");
            break;
        default:
            printf("other\n");
            break;
        };
    }

    return 0;
}
void destroy_surface(void)
{
}
#else
#define Sleep usleep
extern "C" int _kbhit(void);
#include <SDL/SDL.h>
#include <SDL/SDL_thread.h>
#include <SDL/SDL_audio.h>
#include <SDL/SDL_timer.h>

#include <strings.h>
#include <iostream>
#include <stdio.h>
using namespace std;

struct pt_data
{
    SDL_Surface **ptscreen;
    SDL_Event *ptsdlevent;
    SDL_Rect *drect;
    SDL_mutex *affmutex;
} ptdata;

static Uint32 SDL_VIDEO_Flags = SDL_ANYFORMAT | SDL_DOUBLEBUF | SDL_RESIZABLE;

static int event_thread(void *data);

const SDL_VideoInfo *info;
char driver[128];
const char *videodevice = NULL;
SDL_Surface *pscreen;
SDL_Overlay *overlay;
SDL_Rect drect;
SDL_Event sdlevent;
SDL_Thread *mythread;
SDL_mutex *affmutex;
int status;
unsigned char *p = NULL;
unsigned char d1[500], d2[500];
int w, h;

int setup_surface(void)
{

    if (SDL_Init(SDL_INIT_VIDEO) < 0)
    {
        fprintf(stderr, "Couldn't initialize SDL: %s\n", SDL_GetError());
        exit(1);
    }

    if (SDL_VideoDriverName(driver, sizeof(driver)))
    {
        printf("Video driver: %s\n", driver);
    }

    info = SDL_GetVideoInfo();

    if (videodevice == NULL || *videodevice == 0)
    {
        videodevice = "/dev/video0";
    }

    pscreen = SDL_SetVideoMode(display_width, display_height, 0, SDL_VIDEO_Flags);
    overlay = SDL_CreateYUVOverlay(display_width, display_height, SDL_YV12_OVERLAY, pscreen);

    p = (unsigned char *) overlay->pixels[0];
    drect.x = 0;
    drect.y = 0;
    drect.w = pscreen->w;
    drect.h = pscreen->h;

    SDL_WM_SetCaption("Receive Decompress and Play", NULL);
    SDL_LockYUVOverlay(overlay);
    SDL_UnlockYUVOverlay(overlay);

    /* initialize thread data */
    ptdata.ptscreen = &pscreen;
    ptdata.ptsdlevent = &sdlevent;
    ptdata.drect = &drect;
    affmutex = SDL_CreateMutex();
    ptdata.affmutex = affmutex;
    mythread = SDL_CreateThread(event_thread, (void *) &ptdata);

    return 0;
};
int show_frame(vpx_image_t *img)
{
    char caption[512];
    sprintf(caption, "Receive Decompress and Play");
    SDL_LockMutex(affmutex);
    SDL_WM_SetCaption(caption, NULL);
    SDL_LockYUVOverlay(overlay);
    int i;

    unsigned char *in = img->planes[PLANE_Y];
    unsigned char *p = (unsigned char *) overlay->pixels[0];

    for (i = 0; i < display_height; i++, in += img->stride[PLANE_Y], p += display_width)
        memcpy(p, in, display_width);

    in = img->planes[PLANE_U];

    for (i = 0; i < display_height / 2; i++, in += img->stride[PLANE_U], p += display_width / 2)
        memcpy(p, in, display_width / 2);

    in = img->planes[PLANE_V];

    for (i = 0; i < display_height / 2; i++, in += img->stride[PLANE_U], p += display_width / 2)
        memcpy(p, in, display_width / 2);

    SDL_UnlockYUVOverlay(overlay);
    SDL_DisplayYUVOverlay(overlay, &drect);
    SDL_UnlockMutex(affmutex);
    return 0;
}

void destroy_surface(void)
{
    SDL_WaitThread(mythread, &status);
    SDL_DestroyMutex(affmutex);
    SDL_Quit();
}

static int event_thread(void *data)
{
    struct pt_data *gdata = (struct pt_data *) data;
    SDL_Surface *pscreen = *gdata->ptscreen;
    SDL_Event *sdlevent = gdata->ptsdlevent;
    SDL_Rect *drect = gdata->drect;
    SDL_mutex *affmutex = gdata->affmutex;

    while (signalquit)
    {
        SDL_LockMutex(affmutex);

        while (SDL_PollEvent(sdlevent))     //scan the event queue
        {
            switch (sdlevent->type)
            {
            case SDL_VIDEORESIZE:
                pscreen = SDL_SetVideoMode(sdlevent->resize.w & 0xfffe,
                                           sdlevent->resize.h & 0xfffe, 0, SDL_VIDEO_Flags);
                drect->w = sdlevent->resize.w & 0xfffe;
                drect->h = sdlevent->resize.h & 0xfffe;
                break;
            case SDL_KEYUP:
                break;
            case SDL_KEYDOWN:

                switch (sdlevent->key.keysym.sym)
                {
                case SDLK_a:
                    break;
                case SDLK_s:
                    break;
                case SDLK_z:
                    break;
                case SDLK_x:
                    break;
                default :
                    break;
                }

                break;
            case SDL_QUIT:
                printf("\nStop asked\n");
                signalquit = 0;
                break;
            }
        }           //end if poll

        SDL_UnlockMutex(affmutex);
        SDL_Delay(50);
    }               //end main loop

    return 0;

}

#endif

typedef struct
{
    unsigned int size;
    unsigned int count;
    unsigned int add_ptr;
    unsigned int max;
    unsigned int ssrc;
    unsigned short oldest_seq;
    SKIPS s[SS];
    unsigned int skip_ptr;
    PACKET p[PS];
    unsigned int last_frame_timestamp;
    unsigned short last_seq;

} DEPACKETIZER;
DEPACKETIZER y;

int create_depacketizer(DEPACKETIZER *x)
{
    unsigned int sn;
    x->size           = PACKET_SIZE;
    x->max			  = PS;
    x->skip_ptr = 0;
    x->count = 0;
    x->add_ptr = 0;
    x->last_frame_timestamp = 0xffffffff;
    x->last_seq = 0xffff;
    x->ssrc = 411;

    // skip store is initialized to no skips in store
    for (sn = 0; sn < SS; sn++)
        x->s[sn].received = 1;

    return 0; // SUCCESS
}
int remove_skip(DEPACKETIZER *p, unsigned short seq)
{
    int i;
    unsigned int skip_fill = 0;

    // remove packet from skip store if its there it came out of order...
    for (i = 0; i < SS; i++)
    {
        if (seq == p->s[i].seq)
        {
            p->s[i].received = 1;
            p->s[i].given_up = 0;
            p->s[i].age = 0;
            //p->s[i].seq = 0;
            vpxlog_dbg(SKIP, "Unskip %d \n", seq);
            skip_fill = 1;
            break;
        }
    }

    return skip_fill;
}
int remove_skip_less(DEPACKETIZER *p, unsigned short seq)
{
    int i;
    unsigned int skip_fill = 0;

    // remove packet from skip store if its there it came out of order...
    for (i = 0; i < SS; i++)
    {
        if ((unsigned short)(p->s[i].seq - seq) > 32767 && !p->s[i].received)
        {
            p->s[i].received = 1;
            p->s[i].given_up = 0;
            p->s[i].age = 0;
            //p->s[i].seq = 0;
            vpxlog_dbg(SKIP, "Unskip less than %d : %d \n", seq, p->s[i].seq);
            skip_fill = 1;
        }
    }

    return skip_fill;
}
int add_skip(DEPACKETIZER *p, unsigned short sn)
{
    // maybe we need to check if skip store is completely full?
    if (!p->s[p->skip_ptr].given_up && !p->s[p->skip_ptr].received)
    {
        // if it is what do we do?
        sn += 0;
        vpxlog_dbg(REBUILD, "Skip Store filled!!!\n");
    }

    // clear data that might mess us up
    p->p[sn &PSM].redundant_count = 0;
    p->p[sn &PSM].type = DATAPACKET;
    p->p[sn &PSM].size = 0;
    p->s[p->skip_ptr].arrival = (unsigned short)(get_time() & 0xffff);
    p->s[p->skip_ptr].retry = 0;
    p->s[p->skip_ptr].seq = sn;
    p->s[p->skip_ptr].age = 0;
    p->s[p->skip_ptr].received = 0;
    p->s[p->skip_ptr].given_up = 0;
    p->skip_ptr = ((p->skip_ptr + 1)&SSM);
    return 0;
}
void check_recovery(DEPACKETIZER *p,  PACKET *x)
{
    if ((x->frame_type == KEY || x->frame_type == GOLD || x->frame_type == ALTREF))
    {
        unsigned short seq = x->seq;//p->oldest_seq;
        unsigned short lastPossibleSeq = p->oldest_seq;//p->last_seq;
        PACKET *tp = &p->p[seq&PSM];
        vpxlog_dbg(REBUILD, "Received keyframe or recovery frame -> %d, %d \n", seq, p->p[x->seq&PSM].timestamp);

        // if we are on a new frame drop everything older than where we are now.
        if (x->new_frame)
        {
            p->oldest_seq = seq;
            p->last_frame_timestamp = x->timestamp - 1;
            seq--;
            remove_skip_less(p, seq);
        }
        // find first non dropped packet prior to now.
        else while (seq != lastPossibleSeq)
            {
                tp = &p->p[seq&PSM];

                // new timestamp that isn't empty
                if (tp->size != 0 && tp->timestamp != x->timestamp && tp->seq == seq)
                {
                    remove_skip_less(p, seq);
                    break;
                }

                seq--;
            }

        given_up = 0;
    }
}

int read_packet(DEPACKETIZER *p, tc8 *data, unsigned int size)
{
    PACKET *x = (PACKET *) data;
    unsigned int skip_fill = 0;

    // random drops
    if ((rand() & 1023) < drop_simulation)
        return 0;

    // wrong ssrc exit
    if (p->ssrc != x->ssrc)
        return 0;

    // already received the packet (ignore this one)
    if (p->p[x->seq &PSM].seq == x->seq && p->p[x->seq&PSM].size)
        return 0;

    // on the first received packet record first time ever numbers
    if (!first_time_stamp_ever)
    {
        first_time_stamp_ever = x->timestamp;
        first_seq_ever = x->seq;
        p->oldest_seq = x->seq;
        p->last_seq = p->oldest_seq - 1;
        vpxlog_dbg(REBUILD, "Received First TimeStamp ever! -> %d, %d new=%d\n", x->seq, x->timestamp, x->new_frame);

        if (x->new_frame != 1)
        {
            add_skip(p, x->seq - 1);
            p->oldest_seq = x->seq - 1;
            vpxlog_dbg(REBUILD, "First packet not start of new frame! -> %d, \n", x->seq - 1);
        }
    }

    // if we are on the first frame ever and there's an older
    if (first_time_stamp_ever == x->timestamp && first_seq_ever > x->seq)
    {
        first_seq_ever = x->seq;
        p->oldest_seq = x->seq;

        if (x->new_frame == 1)
        {
            first_time_stamp_ever = x->timestamp - 1;
        }
        else
        {
            add_skip(p, x->seq - 1);
            p->oldest_seq = x->seq - 1;
            vpxlog_dbg(REBUILD, "Old seq around! -> %d, \n", x->seq - 1);
        }
    }

    // toss the packet if its for a frame we've already thrown out or displayed
    // maybe roll over is an issue we need to address
    if (x->timestamp < p->last_frame_timestamp + 1)
    {
        vpxlog_dbg(DISCARD, "Tossing old seq :%d \n", x->seq);

        // make sure that if we toss our oldest seq we've seen we update
        if (x->seq - p->oldest_seq > 0 && x->seq - p->oldest_seq < 32768)
        {
            p->oldest_seq = x->seq + 1;
            remove_skip_less(p, p->oldest_seq);
        }

        return 0;
    }

    skip_fill = remove_skip(p, x->seq);

    // this clears the case that we rebuild a packet after we requested a resend
    if (!skip_fill && p->last_seq - x->seq > 0 && p->last_seq - x->seq < 32768)
        skip_fill = 1;

    // copy to the packet store
    x->size = size - PACKET_HEADER_SIZE;

    if (x->size < PACKET_SIZE)
        memset(x->data + x->size, 0, PACKET_SIZE - x->size);

    p->p[x->seq &PSM] = *x;

    vpxlog_dbg(LOG_PACKET, "Received Packet %d, %d : new: %d, frame type: %d given_up: %d oldest: %d \n", x->seq, p->p[x->seq&PSM].timestamp, x->new_frame, x->frame_type, given_up, p->oldest_seq);

    // if we get a key frame or recovery frame set this as new frame
    check_recovery(p, x);

    // do we have a skip
    if (!skip_fill && x->seq != (unsigned short)(p->last_seq + 1) && x->seq != p->last_seq)
    {
        unsigned short sn;

        // add to skip store
        for (sn = p->last_seq + 1; sn != x->seq; sn++)
        {
            vpxlog_dbg(SKIP, "Skipped Packet  %d\n", sn);
            add_skip(p, sn);
        }
    }

    if (!skip_fill)
        p->last_seq = x->seq;

    return 0;
}

int rebuild_packet(DEPACKETIZER *p, unsigned short seq)
{
    unsigned short seqp, seqj;
    long long *in[MAX_NUMERATOR];
    long long *out = (long long *) p->p[seq&PSM].data;
    unsigned int i, j = 0;
    unsigned int redundant_count = 0;
    PACKET *pp = &p->p[(seq-1)&PSM];
    PACKET *np = &p->p[(seq+1)&PSM];

    // if last packet has type count 1 we don't need this one its type!
    // don't bother rebuilding
    if (pp->redundant_count == 1)
    {
        p->p[seq &PSM].type = XORPACKET;
        p->p[seq &PSM].size = 0;

        if (seq == p->oldest_seq)
            p->oldest_seq++;

        return -1;
    }

    // if 1 ago is empty, check 2 ago in case we lost redundant packet
    if (p->p[(seq-2)&PSM].redundant_count == 1)
        pp = &p->p[(seq-2)&PSM];

    // no point doing this frame before the last one is ready
    if (pp->timestamp < p->last_frame_timestamp)
        return -1;

    p->p[seq &PSM].type = DATAPACKET;

    // search through subsequent packets for the redundant packet
    for (seqp = seq + 1; seqp != (unsigned short)(seq + MAX_NUMERATOR); seqp++)
    {
        // found redundant packet filled in ?
        if (p->p[seqp&PSM].type && p->p[seqp&PSM].size)
        {
            redundant_count = p->p[seqp&PSM].redundant_count;

            // if initiate call this seq isn't covered.
            if (redundant_count < (unsigned short)(seqp - seq))
            {
                return -1;
            }

            break;
        }
    }

    // go back through the packets and set up input pointers
    for (seqj = seqp; seqj != seqp - 1 - redundant_count; seqj--)
    {
        // set up pointer to data for each seq in recovery frame
        if (seqj != seq)
        {
            // if its missing or the seq is wrong return a failure.
            if (p->p[seqj &PSM].size == 0 || p->p[seqj &PSM].seq != seqj)
            {
                return -1;
            }

            in[j++] = (long long *) p->p[seqj&PSM].data;
        }
    }

    // nothing was listed as type?
    if (!redundant_count)
    {
        return -1;
    }

    // go through a full packet's worth of data.
    for (j = 0; j < PACKET_SIZE / sizeof(long long); j++)
    {
        // start with the most recent packet
        *out = *(in[0]);

        // xor all the older packets with out
        for (i = 1; i < redundant_count; i++)
        {
            *out ^= *(in[i]);
            in[i]++;
        }

        out++;
        in[0]++;
    }

    // real data filled to the brim with data.
    p->p[seq &PSM].seq = seq;
    p->p[seq &PSM].type = DATAPACKET;
    p->p[seq &PSM].size = PACKET_SIZE;
    p->p[seq &PSM].timestamp = pp->timestamp;
    p->p[seq &PSM].new_frame = 0;
    p->p[seq &PSM].end_frame = 0;
    p->p[seq &PSM].frame_type = pp->frame_type;

    // logging what packets we used to rebuild
    if (LOG_MASK & REBUILD)
    {
        unsigned short last = seqj + redundant_count + 2;
        seqj++;
        vpxlog_dbg(REBUILD, "Rebuilt Lost Sequence :%d, %d from: ", seq, p->p[seq&PSM].timestamp);

        for (; seqj != last; seqj++)
            if (seq != seqj)
                vpxlog_dbg_no_head(REBUILD, "%d, ", p->p[seqj&PSM].seq);

        vpxlog_dbg_no_head(REBUILD, "\n");
    }

    // if np is type and end_frame this packet ends frame
    if (np->end_frame && np->type)
        p->p[seq &PSM].end_frame = 1;

    // last packet ends frame
    if (pp->end_frame)
    {
        // if next packet is a new frame we have to fabricate a frame..
        if (np->new_frame)
        {
            p->p[seq &PSM].timestamp = (pp->timestamp + np->timestamp) / 2;
            p->p[seq &PSM].new_frame = 1;
            p->p[seq &PSM].end_frame = 1;
        }
        else
        {
            // this must be the frame start
            p->p[seq &PSM].frame_type = np->frame_type;
            p->p[seq &PSM].timestamp = np->timestamp;
            p->p[seq &PSM].new_frame = 1;
        }
    }

    check_recovery(p, &p->p[seq&PSM]);
    return 0;
}

int frame_ready(DEPACKETIZER *p)
{
    // check if we have a whole frame.
    unsigned short seq = p->oldest_seq; // f->first_seq;
    unsigned short last_possible_seq = p->last_seq;
    PACKET *tp = &p->p[seq&PSM];

    unsigned int timestamp = p->p[seq&PSM].timestamp;

    if (timestamp < p->last_frame_timestamp + 1)
    {
        vpxlog_dbg(FRAME, "Trying to play an old frame:%d, timestamp :%d , last Time :%d \n", seq, timestamp, p->last_frame_timestamp);
        return 0;
    }

    // seems like this should be unnecessary???
    while (timestamp && p->p[seq &PSM].timestamp == timestamp && !p->p[seq&PSM].new_frame)
        seq--;

    p->oldest_seq = seq;
    remove_skip_less(p, p->oldest_seq);

    // first seq not a new frame. Frames not ready.
    if (!p->p[seq&PSM].new_frame)
    {
        if (p->p[(seq-1)&PSM].type == XORPACKET)
            p->p[seq &PSM].new_frame = 1;
        else
            return 0;
    }

    // loop through all frames and see if every packet between start and
    // end is present or we are missing type frames.
    while (seq != last_possible_seq)
    {
        tp = &p->p[seq&PSM];

        // timestamp needs to differ and the packet has to have data
        if (tp->timestamp != timestamp || tp->size == 0)
        {
            // here we have a whole frame but end frame marker not set properly
            if (tp->new_frame && tp->size > 0)
            {
                p->p[(seq-1)&PSM].end_frame = 1;
                return 1;
            }

            // if missing packet is not type frame is not ready.
            if (p->p[(seq-1)&PSM].redundant_count != 1)
                return 0;

            // make sure frame is marked type
            tp->type = XORPACKET;
        }
        else if (tp->end_frame)
            return 1;

        seq++;
    }

    return 0;
}
int get_frame(DEPACKETIZER *p, unsigned char *data, int size, unsigned int *outsize, unsigned int *timestamp)
{
    *outsize = 0;

    // check if we have a whole frame.
    if (frame_ready(p))
    {
        unsigned short seq = p->oldest_seq;
        unsigned short last_possible_seq = p->last_seq;
        *timestamp = p->p[seq&PSM].timestamp;

        // build a frame from the packets we have.
        while (seq != last_possible_seq)
        {
            PACKET *tp = &p->p[seq&PSM];

            // timestamp needs to match and size must be > 0
            if (tp->timestamp == *timestamp && tp->size > 0 && tp->type == DATAPACKET)
            {
                memcpy(data, tp->data, tp->size);
                data += tp->size;
                *outsize += tp->size;
                tp->size = 0;

                if (tp->end_frame)
                    break;
            }

            // its a skip clear from skip remove it
            if (tp->size == 0)
            {
                remove_skip(p, seq);
            }

            seq++;
        }

        // if we have a xorpacket frame at the end of our frame throw it out
        if (p->p[(seq+1)&PSM].timestamp == *timestamp && p->p[(seq+1)&PSM].type == XORPACKET)
        {
            seq++;
        }

        p->last_frame_timestamp = *timestamp;
        p->oldest_seq = seq + 1;


        return 1;
    }

    return 0;
}

int age_skip_store(DEPACKETIZER *p, struct vpxsocket *vpx_sock, union vpx_sockaddr_x *address)
{
    unsigned int request_count = 0;
    unsigned int i;
    unsigned short now = (unsigned short)(get_time() & 0xffff);

    if (given_up)
    {
        // we've given up on a frame do nothing else until we get a recovery frame.
        unsigned short time_to_retry = 0;
        unsigned short seq = p->s[givenup_skip].seq;

        if (p->s[givenup_skip].arrival <= now)
            p->s[givenup_skip].age = now - p->s[givenup_skip].arrival;
        else
            p->s[givenup_skip].age = (unsigned short)((unsigned int)(0xffff + now) - p->s[givenup_skip].arrival);

        time_to_retry = (p->s[givenup_skip].age > (p->s[givenup_skip].retry * retry_interval));

        if (time_to_retry && ((rand() & 1023) >= drop_simulation))
        {
            // Tell the sender we want to give up
            int bytes_sent;
            tc8 buffer[40];
            buffer[0] = 'g';
            buffer[1] = seq & 0x00ff;
            buffer[2] = (seq & 0xff00) >> 8;
            vpx_net_sendto(vpx_sock, buffer, 3, &bytes_sent, *address);
            vpxlog_dbg(DISCARD, "Give up forever on sequence %d now %d :age :%d retry:%d \n", seq, now, p->s[givenup_skip].age, p->s[givenup_skip].retry);
            p->s[givenup_skip].retry ++;
        }

        return 0;
    }

    for (i = 0; i < SS; i++)
    {
        if (!p->s[i].received && !p->s[i].given_up)
        {
            request_count ++;
        }
    }

    // go through the skip store
    for (i = 0; i < SS; i++)
    {
        // if this skip is still in play
        if (!p->s[i].received && !p->s[i].given_up)
        {
            unsigned short seq = p->s[i].seq;
            unsigned short time_to_retry = 0;
            unsigned int is_redundant = (p->p[(p->s[i].seq - 1)&PSM].redundant_count == 1);

            // calculate the age of the skip including wrap around
            if (p->s[i].arrival <= now)
                p->s[i].age = now - p->s[i].arrival;
            else
                p->s[i].age = (unsigned short)((unsigned int)(0xffff + now) - p->s[i].arrival);

            time_to_retry = (p->s[i].age > (p->s[i].retry * retry_interval));

            // if its redundant don't bother rebuilding requesting it again.
            if (is_redundant)
            {
                p->s[i].given_up = 1;
                p->p[p->s[i].seq &PSM].size = 0;

                vpxlog_dbg(LOG_PACKET, "Lost redundant packet %d, ignoring \n", seq);

            }
            // try and rebuild from recovery packets
            else if (rebuild_packet(p, seq) == 0)
            {
                p->s[i].received = 1;
                p->s[i].age = 0;
            }
            // time to give up we wasted enough time
            else if (time_to_retry && (p->s[i].age > skip_timeout || request_count > retry_count))
            {
                given_up = 1;
                givenup_skip = i;
                vpxlog_dbg(LOG_PACKET, "Giving up: %d age:%d request_count:%d\n", seq, p->s[i].age, request_count);
                break;
            }
            // request a resend
            else if (time_to_retry && ((rand() & 1023) >= drop_simulation))
            {
                int bytes_sent;
                tc8 buffer[40];
                buffer[0] = 'r';
                buffer[1] = seq & 0x00ff;
                buffer[2] = (seq & 0xff00) >> 8;
                vpx_net_sendto(vpx_sock, buffer, 3, &bytes_sent, *address);
                vpxlog_dbg(DISCARD, "Lost %d, skip: %d, Requesting Resend\n", seq, i);
                p->s[i].retry++;
            }
        }

        // if we're giving up on this one and its the oldest of the bunch increase the oldest seq
        if (p->oldest_seq == p->s[i].seq && p->s[i].given_up)
        {
            p->oldest_seq++;
        }
    }

    return 0;
}
//#define DEBUG_FILES 1
#ifdef DEBUG_FILES
void debug_frame(vpx_image_t *img)
{
    unsigned char *in = img->planes[PLANE_Y];

    for (unsigned int i = 0; i < display_height; i++, in += img->stride[PLANE_Y])
    {
        fwrite(in , display_width, 1, outFile);
    }

    in = img->planes[PLANE_U];

    for (unsigned int i = 0; i < display_height / 2; i++, in += img->stride[PLANE_U])
    {
        fwrite(in , display_width / 2, 1, outFile);
    }

    in = img->planes[PLANE_V];

    for (unsigned int i = 0; i < display_height / 2; i++, in += img->stride[PLANE_V])
    {
        fwrite(in , display_width / 2, 1, outFile);
    }

}
#endif

int main(int argc, char *argv[])
{
    printf("ReceiveDecompressAndPlay (-? for help) \n");

    while (--argc > 0)
    {
        if (argv[argc][0] == '-')
        {
            switch (argv[argc][1])
            {
            case 'w':
            case 'W':
                display_width = atoi(argv[argc--+1]);
                break;
            case 'h':
            case 'H':
                display_height = atoi(argv[argc--+1]);
                break;
            case 'f':
            case 'F':
                capture_frame_rate = atoi(argv[argc--+1]);
                break;
            case 'b':
            case 'B':
                video_bitrate = atoi(argv[argc--+1]);
                break;
            case 'n':
            case 'N':
                fec_numerator = atoi(argv[argc--+1]);
                break;
            case 'd':
            case 'D':
                fec_denominator = atoi(argv[argc--+1]);
                break;
            case 't':
            case 'T':
                skip_timeout = atoi(argv[argc--+1]);
                break;
            case 'i':
            case 'I':
                retry_interval = atoi(argv[argc--+1]);
                break;
            case 'c':
            case 'C':
                retry_count = atoi(argv[argc--+1]);
                break;
            case 'l':
            case 'L':
                drop_simulation = atoi(argv[argc--+1]);
                break;
            case 's':
            case 'S':
                send_port = atoi(argv[argc--+1]);
                break;
            case 'r':
            case 'R':
                recv_port = atoi(argv[argc--+1]);
                break;
            default:
                printf(
                    "ReceiveDecompressAndPlay: \n"
                    "========================: \n"
                    "Receives, decompresses and plays video received from the GrabCompressAndSend sample.\n\n"
                    "-w [640]  request capture width \n"
                    "-h [480]  request capture height \n"
                    "-f [30]   request capture frame rate\n"
                    "-b [300]  video_bitrate = ato\n"
                    "-n [6]    fec_numerator ( redundancy numerator)\n"
                    "-d [5]    fec_denominator ( redundancy denominator) \n"
                    "          6/5 means 1 xor packet for every 5 packets, \n"
                    "	       4/1 means 3 duplicate packets for every packet\n"
                    "-t [800]  milliseconds before giving up and requesting recovery \n"
                    "-i [50]   time in milliseconds between attempts at a packet resend\n"
                    "-c [12]   number of lost packets before requesting recovery \n"
                    "-l [0]    packets to lose out of every 1000 \n"
                    "-s [1408] port to send requests to\n"
                    "-r [1407] port to receive requests on. \n"
                    "\n");
                exit(0);
                break;
            }
        }
    }

    vpxlog_dbg(FRAME, "%dx%d %dfps, %dkbps, %d/%dFEC,%d skip, %d retry interval, %d count,  %d drop simulation \n",
                display_width, display_height, capture_frame_rate, video_bitrate, fec_numerator, fec_denominator ,
                skip_timeout, retry_interval, retry_count, drop_simulation);


    struct vpxsocket vpx_sock, vpx_sock2;
    union vpx_sockaddr_x address, address2;

    TCRV rc;
    tc32 bytes_read;

#ifdef DEBUG_FILES
    FILE *f = fopen("out2.rtp", "wb");
    char fn[512];
    sprintf(fn, "decoded_%dx%d", display_width, display_height);
    FILE *out_file = fopen(fn, "wb");
    FILE *vpx_file = fopen("decode.vpx", "wb");
#endif

    int responded = 0;

    vpx_dec_ctx_t          decoder;
    uint8_t               *buf = NULL;
    vp8_postproc_cfg_t	  ppcfg;
    vpx_codec_dec_cfg_t     cfg = {0};
//	vpx_dec_init(&decoder, &vpx_codec_vp8_algo);
    vpx_codec_dec_init(&decoder, &vpx_codec_vp8_dx_algo, &cfg, 0);

    buf = (uint8_t *) malloc(display_width * display_height * 3 / 2);

    /* Config post processing settings for decoder */
    ppcfg.post_proc_flag = VP8_DEMACROBLOCK | VP8_DEBLOCK | VP8_ADDNOISE;
    ppcfg.deblocking_level = 5	 ;
    ppcfg.noise_level = 1  ;
    vpx_codec_control(&decoder, VP8_SET_POSTPROC, &ppcfg);

    create_depacketizer(&y);

    vpx_net_init();

    if (TC_OK != vpx_net_open(&vpx_sock, vpx_IPv4, vpx_UDP))
        return -1;

    vpx_net_set_read_timeout(&vpx_sock, 20);
    vpx_net_bind(&vpx_sock, 0, recv_port);

    if (TC_OK != vpx_net_open(&vpx_sock2, vpx_IPv4, vpx_UDP))
        return -1;

    int bytes_sent;

    while (!_kbhit())
    {
        char initPacket[PACKET_SIZE];
        sprintf(initPacket, "configuration  %d %d %d %d %d %d ", display_width, display_height, capture_frame_rate, video_bitrate, fec_numerator, fec_denominator);
        rc = vpx_net_recvfrom(&vpx_sock, one_packet, sizeof(one_packet), &bytes_read, &address);

        if (rc != TC_OK && rc != TC_WOULDBLOCK && rc != TC_TIMEDOUT)
            vpxlog_dbg(DISCARD, "error\n");

        if (bytes_read == -1)
            bytes_read = 0;

        if (bytes_read)
        {
            if (!responded)
            {
                char add[400];
                sprintf(add, "%d.%d.%d.%d",
                        ((unsigned char *)&address.sa_in.sin_addr)[0],
                        ((unsigned char *)&address.sa_in.sin_addr)[1],
                        ((unsigned char *)&address.sa_in.sin_addr)[2],
                        ((unsigned char *)&address.sa_in.sin_addr)[3]);

                vpxlog_dbg(LOG_PACKET, "Address of Sender : %s \n", add);
                vpx_net_get_addr_info(add, send_port, vpx_IPv4, vpx_UDP, &address2);
                responded = 1;
            }

            if (strncmp(one_packet, "initiate call", PACKET_SIZE) == 0)
            {
                rc = vpx_net_sendto(&vpx_sock2, (tc8 *) &initPacket, PACKET_SIZE , &bytes_sent, address2);
            }

            if (strncmp(one_packet, "confirmed", PACKET_SIZE) == 0)
            {
                rc = vpx_net_sendto(&vpx_sock2, (tc8 *) &initPacket, PACKET_SIZE , &bytes_sent, address2);
                break;
            }
        }

        Sleep(200);
    }

    setup_surface();

    /* Message loop for display window's thread */
    while (!_kbhit() && signalquit)
    {
        rc = vpx_net_recvfrom(&vpx_sock, one_packet, sizeof(one_packet), &bytes_read, &address);

        if (rc != TC_OK && rc != TC_WOULDBLOCK && rc != TC_TIMEDOUT)
            vpxlog_dbg(DISCARD, "error %d\n", rc);

        if (bytes_read == -1)
        {
//			vpxlog_dbg("-1 bytes_read \n");
            bytes_read = 0;
        }

        if (bytes_read)
        {
            unsigned int timestamp;
            unsigned int size;
            read_packet(&y, one_packet, bytes_read);

            while (get_frame(&y, compressed_video_buffer, sizeof(compressed_video_buffer), &size, &timestamp))
            {
                lag_In_milli_seconds = (unsigned int)((timestamp - first_time_stamp_ever) / 1000.0 - (get_time() - time_of_first_display));
                vpxlog_dbg(FRAME, "Received frame %d, Lag: %d \n", timestamp, lag_In_milli_seconds);

#ifdef DEBUG_FILES
                fwrite(&size, 4, 1, vpx_file);
                fwrite(compressed_video_buffer, size, 1, vpx_file);
#endif

                if (!time_of_first_display)
                {
#ifdef WINDOWS
                    ShowWindow(hwnd, SW_SHOWNOACTIVATE);
                    UpdateWindow(hwnd);
#endif
                    time_of_first_display = get_time();
                }

                vpx_dec_iter_t  iter = NULL;
                vpx_image_t    *img;

                if (vpx_codec_decode(&decoder, compressed_video_buffer, sizeof(compressed_video_buffer), 0, 0))
                {
                    vpxlog_dbg(FRAME, "Failed to decode frame: %s\n", vpx_codec_error(&decoder));
                    return -1;
                }

                img = vpx_codec_get_frame(&decoder, &iter);
                show_frame(img);
#ifdef DEBUG_FILES
                debug_frame(img);
#endif

            };

            if (!responded)
            {
                char add[400];
                sprintf(add, "%d.%d.%d.%d",
                        ((char *)&address.sa_in.sin_addr)[0],
                        ((char *)&address.sa_in.sin_addr)[1],
                        ((char *)&address.sa_in.sin_addr)[2],
                        ((char *)&address.sa_in.sin_addr)[3]);

                vpxlog_dbg(LOG_PACKET, "Address of Sender : %s \n", add);
                vpx_net_get_addr_info(add, send_port, vpx_IPv4, vpx_UDP, &address2);
                responded = 1;
            }

#ifdef DEBUG_FILES
            fwrite(one_packet, bytes_read, 1, f);
#endif

        }
        else
            age_skip_store(&y, &vpx_sock2, &address2);

    }

    signalquit = 0;

#ifdef DEBUG_FILES
    fclose(f);
    fclose(out_file);
    fclose(vpx_file);
#endif

    if (vpx_codec_destroy(&decoder))
    {
        vpxlog_dbg(DISCARD, "Failed to destroy decoder: %s\n", vpx_codec_error(&decoder));
        return -1;
    }

    free(buf);

    vpx_net_close(&vpx_sock);
    vpx_net_destroy();
    destroy_surface();
    return 0;
}
