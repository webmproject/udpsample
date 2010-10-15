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

#include "vpx_network.h"

#include <stdio.h>
#include <stdarg.h>
#include <stdio.h>
#include <ctype.h>  //for tolower
#include <string.h>


extern "C"
{
#include "rtp.h"
#include "vpx/vpx_encoder.h"
#include "vpx/vp8cx.h"
}

unsigned int drop_first = 60;
unsigned int count_captured_frames = 0;

const int size_buffer = 1680;
#define FAIL_ON_NONZERO(x) if((x)) { vpxlog_dbg(ERRORS,#x"\n");return -1; };
#define FAIL_ON_ZERO(x) if(!(x)) { vpxlog_dbg(ERRORS,#x"\n");return -1; };
#define FAIL_ON_NEGATIVE(x) if((x)<0) { vpxlog_dbg(ERRORS,#x"\n");return -1; };

#ifdef WINDOWS
#include "stdafx.h"
#include <dshow.h>
#include <atlcomcli.h>
#include <atlbase.h>
#include "qedit.h"
#pragma comment(lib, "strmiids.lib")

#include "tctypes.h"
#include <conio.h>
#include <vfw.h>

GUID MEDIASUBTYPE_I420 =
{
    MAKEFOURCC('I', '4', '2', '0'),
    0x0000, 0x0010, 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71
};

CComPtr<IGraphBuilder> graph;
CComPtr<IBaseFilter> capture, null_filter, grabber, video_filter;
CComPtr<IPin> cap_out_pin, grab_in_pin, grab_out_pin, null_in_pin, null_out_pin;
CComPtr<ISampleGrabber> sample_grabber;
CComPtr<IAMStreamConfig> config;
CComQIPtr<IMediaControl, &IID_IMediaControl> control;
CComPtr<IVideoWindow> video_window;
#else
#define Sleep usleep
extern "C" int _kbhit(void);
#endif

vpx_image_t raw;
bool buffer_has_frame = false;
double buffer_time;
long long last_time_in_nanoseconds = 0;

int display_width = 640;
int display_height = 480;
int capture_frame_rate = 30;
int video_bitrate = 400;
int fec_numerator = 6;
int fec_denominator = 5;
unsigned short send_port = 1407;
unsigned short recv_port = 1408;

#define PS 2048
#define PSM  (PS-1)
#define MAX_NUMERATOR 16

typedef enum
{
    NONE,
    XOR,
    RS
} FEC_TYPE;

typedef struct
{
    unsigned int size;
    FEC_TYPE fecType;
    unsigned int fec_numerator;
    unsigned int fec_denominator;
    unsigned int new_fec_denominator;
    unsigned int count;
    unsigned int add_ptr;
    unsigned int send_ptr;
    unsigned int max;
    unsigned int fec_count;
    unsigned short seq;
    PACKET packet[PS];
} PACKETIZER;

PACKETIZER x;
tc8 one_packet[8000];

unsigned char output_video_buffer[1280*1024*3];

#ifdef WINDOWS
HRESULT FindFilter(CLSID cls, IBaseFilter **pp, bool name, CComVariant &filter)
{
    HRESULT hr;
    CComPtr<ICreateDevEnum> dev_enum;
    hr = dev_enum.CoCreateInstance(CLSID_SystemDeviceEnum);

    if (!SUCCEEDED(hr))
        return hr;

    CComPtr<IEnumMoniker> enum_moniker;
    hr = dev_enum->CreateClassEnumerator(cls, &enum_moniker, 0);

    if (!SUCCEEDED(hr))
        return hr;

    CComPtr<IMoniker> moniker;
    ULONG fetched;

    while (enum_moniker->Next(1, &moniker, &fetched) == S_OK)
    {
        CComPtr<IPropertyBag> iPropBag;
        hr = moniker->BindToStorage(0, 0, IID_IPropertyBag, (void **)&iPropBag);

        if (SUCCEEDED(hr))
        {
            // To retrieve the filter's friendly name, do the following:
            CComVariant var_name;
            hr = iPropBag->Read(L"FriendlyName", &var_name, 0);

            if (SUCCEEDED(hr))
            {
                if (!name || var_name == filter)
                {
                    // To create an instance of the filter, do the following:
                    hr = moniker->BindToObject(NULL, NULL, IID_IBaseFilter, (void **)pp);
                    break;
                }
            }
        }

        moniker = NULL;
    }

    return hr;
}

HRESULT GetFirstPin(IBaseFilter *p, PIN_DIRECTION pd, IPin **pp)
{
    CComPtr<IEnumPins> enum_moniker;
    p->EnumPins(&enum_moniker);
    CComPtr<IPin> pin;

    while (enum_moniker && enum_moniker->Next(1, &pin, NULL) == S_OK)
    {
        PIN_INFO pi;
        pin->QueryPinInfo(&pi);

        if (pi.dir == pd)
        {
            *pp = pin.Detach();
            return S_OK;
        }

        pin = NULL;
    }

    return E_FAIL;
}
class CVideoCallback : public ISampleGrabberCB
{
public:
    FILE *output_file;
    STDMETHODIMP_(ULONG) AddRef()
    {
        return 2;
    }
    STDMETHODIMP_(ULONG) Release()
    {
        return 1;
    }
    STDMETHODIMP QueryInterface(REFIID riid, void **ppv)
    {
        if (riid == IID_ISampleGrabberCB || riid == IID_IUnknown)
        {
            *ppv = (void *)static_cast<ISampleGrabberCB *>(this);
            return NOERROR;
        }

        return E_NOINTERFACE;
    }
    CVideoCallback(char *filename, int decimate)
    {
//		output_file=fopen(filename,"wb");
    }
    ~CVideoCallback()
    {
//		fclose(output_file);
    }
    STDMETHODIMP SampleCB(double sample_time, IMediaSample *sample)
    {
        return S_OK;
    }
    STDMETHODIMP BufferCB(double sample_time, BYTE *buffer, long buffer_len)
    {

        buffer_has_frame = false;
        memcpy(raw.img_data, buffer, buffer_len);
        buffer_has_frame = true;
        buffer_time = sample_time;
        //fwrite(inputVideoBuffer,buffer_len,sizeof(BYTE),output_file);
        return S_OK;
    }
};
CVideoCallback video_callback("demo.yv12", 0);
CComQIPtr<ISampleGrabberCB, &IID_ISampleGrabberCB> video_callback_ptr(&video_callback);
#define HRE(y) if(FAILED(hr=y)) {vpxlog_dbg(FRAME,#y##":%x\n",hr);return hr;};

int start_capture(void)
{
    HRESULT hr;

    vpxlog_dbg(FRAME, "Creating filters...\n");
    HRE(graph.CoCreateInstance(CLSID_FilterGraph));
    HRE(FindFilter(CLSID_VideoInputDeviceCategory, &capture, false, CComVariant(L"")));
    HRE(CoCreateInstance(CLSID_SampleGrabber, 0, CLSCTX_INPROC_SERVER, IID_IBaseFilter, reinterpret_cast<void **>(&grabber)));
    HRE(grabber->QueryInterface(IID_ISampleGrabber, (void **)&sample_grabber));
    sample_grabber->SetBufferSamples(true);
    HRE(CoCreateInstance(CLSID_VideoRenderer, 0, CLSCTX_INPROC_SERVER, IID_IBaseFilter, reinterpret_cast<void **>(&video_filter)));
    HRE(graph->AddFilter(capture, L"Capture"));
    HRE(graph->AddFilter(grabber, L"SampleGrabber"));
    HRE(graph->AddFilter(video_filter, L"Video Renderer"));

    vpxlog_dbg(FRAME, "Getting pins...\n");
    HRE(GetFirstPin(capture, PINDIR_OUTPUT, &cap_out_pin));
    HRE(grabber->FindPin(L"In", &grab_in_pin));
    HRE(grabber->FindPin(L"Out", &grab_out_pin));
    HRE(video_filter->FindPin(L"In", &null_in_pin));

    vpxlog_dbg(FRAME, "Connecting pins...\n");
    AM_MEDIA_TYPE pmt;
    ZeroMemory(&pmt, sizeof(AM_MEDIA_TYPE));
    pmt.subtype = MEDIASUBTYPE_I420;
    pmt.majortype = MEDIATYPE_Video;
    pmt.formattype = FORMAT_VideoInfo;
    pmt.pbFormat = reinterpret_cast<BYTE *>(CoTaskMemAlloc(sizeof(VIDEOINFOHEADER)));
    ZeroMemory(pmt.pbFormat, sizeof(VIDEOINFOHEADER));
    pmt.cbFormat = sizeof(VIDEOINFOHEADER);
    pmt.bFixedSizeSamples = 1;
    pmt.bTemporalCompression = 0;

    VIDEOINFOHEADER *pVih = reinterpret_cast<VIDEOINFOHEADER *>(pmt.pbFormat);
    pVih->bmiHeader.biCompression = mmioFOURCC('I', '4', '2', '0');
    pVih->bmiHeader.biWidth = display_width;
    pVih->bmiHeader.biHeight = display_height;
    pVih->bmiHeader.biBitCount = 12;
    pVih->bmiHeader.biPlanes = 3;
    pVih->bmiHeader.biSizeImage = pVih->bmiHeader.biWidth * pVih->bmiHeader.biHeight * pVih->bmiHeader.biBitCount / 8;
    pVih->AvgTimePerFrame = 10000000 / capture_frame_rate;
    pVih->rcSource.top = 0;
    pVih->rcSource.left = 0;
    pVih->rcSource.bottom = display_height;
    pVih->rcSource.right = display_width;
    pVih->rcTarget.top = 0;
    pVih->rcTarget.left = 0;
    pVih->rcTarget.bottom = display_height;
    pVih->rcTarget.right = display_width;
    pmt.lSampleSize = pVih->bmiHeader.biSizeImage;

    HRE(cap_out_pin->QueryInterface(IID_IAMStreamConfig, (void **)&config));
    HRE(config->SetFormat(&pmt));
    AM_MEDIA_TYPE *amt;
    HRE(config->GetFormat(&amt));
    VIDEOINFOHEADER *pVih2 = reinterpret_cast<VIDEOINFOHEADER *>(amt->pbFormat);

    HRE(graph->Connect(cap_out_pin, grab_in_pin));
    HRE(graph->Connect(grab_out_pin, null_in_pin));
    HRE(video_filter->QueryInterface(IID_IVideoWindow, (void **)&video_window));
    HRE(sample_grabber->SetCallback(video_callback_ptr, 1));

    vpxlog_dbg(FRAME, "Running graph...\n");
    control = graph;

    HRE(control->Run());
    return 0;
}

int get_frame(void)
{
    if (buffer_has_frame)
    {
        buffer_time = get_time() / 1000.000;
        return 0;
    }
    else
        return -1;
}
#else

#ifdef MACOSX

#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <time.h>
#include <sys/time.h>

#include <vidcap/vidcap.h>


struct my_source_context
{
    vidcap_src *src;
    char name[VIDCAP_NAME_LENGTH];
};


char frame[1280*720*3/2];

int get_frame(void)
{
    if (buffer_has_frame)
    {
        buffer_time = get_time() / 1000.000;
        return 0;
    }
    else
        return -1;
}

static int user_capture_callback(vidcap_src *src,
                                 void *user_data,
                                 struct vidcap_capture_info *cap_info)
{
    memcpy(raw.img_data, cap_info->video_data, display_width * display_height * 3 / 2);

    buffer_has_frame = 1;

    return 0;
}
const int sleep_ms = 10000;
vidcap_state *vc;
vidcap_sapi *sapi;

struct vidcap_sapi_info sapi_info;
struct vidcap_src_info *src_list;
int src_list_len;
struct my_source_context *ctx_list;
int start_capture(void)
{
    int i;

    FAIL_ON_ZERO(vc = vidcap_initialize());
    FAIL_ON_ZERO(sapi = vidcap_sapi_acquire(vc, 0));

    FAIL_ON_NONZERO(vidcap_sapi_info_get(sapi, &sapi_info));

    src_list_len = vidcap_src_list_update(sapi);

    if (src_list_len < 0)
    {
        vpxlog_dbg(ERRORS, "failed vidcap_src_list_update()\n");
        return -1;
    }
    else if (src_list_len == 0)
    {
        vpxlog_dbg(ERRORS, "no sources available\n");
        return -1;
    }

    FAIL_ON_ZERO(src_list = (struct vidcap_src_info *) calloc(src_list_len,
                            sizeof(struct vidcap_src_info)))

    FAIL_ON_NONZERO(vidcap_src_list_get(sapi, src_list_len, src_list));
    FAIL_ON_ZERO(ctx_list = (my_source_context *) calloc(src_list_len, sizeof(*ctx_list)))

    for (i = 0; i < src_list_len; ++i)
    {
        struct vidcap_fmt_info fmt_info;
        ctx_list[i].src = vidcap_src_acquire(sapi, &src_list[i]);
        fmt_info.width = display_width;
        fmt_info.height = display_height;
        fmt_info.fps_numerator = capture_frame_rate;
        fmt_info.fps_denominator = 1;
        fmt_info.fourcc = 100; // i420

        FAIL_ON_NONZERO(vidcap_format_bind(ctx_list[i].src, &fmt_info));
        FAIL_ON_NONZERO(vidcap_format_info_get(ctx_list[i].src, &fmt_info));

        sprintf(ctx_list[i].name, "source %d", i);

        FAIL_ON_NONZERO(vidcap_src_capture_start(ctx_list[i].src,
                        user_capture_callback,
                        &ctx_list[i]))
    }

    free(src_list);
    return 0;

}
int stop_capture(void)
{
    int i;

    for (i = 0; i < src_list_len; ++i)
    {
        if (!ctx_list[i].src)
            continue;

        FAIL_ON_NONZERO(vidcap_src_capture_stop(ctx_list[i].src))
        FAIL_ON_NONZERO(vidcap_src_release(ctx_list[i].src))
    }

    free(ctx_list);
    FAIL_ON_NONZERO(vidcap_sapi_release(sapi))

    vidcap_destroy(vc);

    return 0;
}

#else
#include <iostream>
#include <fcntl.h>
#include <sys/mman.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "uvcvideo.h"
#include <sys/ioctl.h>
#include <sys/select.h>
#define NB_BUFFER 4

using namespace std;
char frame[1280*720*3/2];

int uyvy2yv12(char *uyvy, int w, int h)
{
    unsigned char *y = raw.img_data;
    unsigned char *u = w * h + y;
    unsigned char *v = w / 2 * h / 2 + u;
    int i, j;

    char *p = uyvy;

    // pretty clearly a very slow way to do this even in c
    // super easy simd conversion
    for (; y < u; p += 4)
    {
        *y++ = p[0];
        *y++ = p[2];
    }

    p = uyvy;

    for (i = 0; i<(h >> 1); i++, p += (w << 1))
        for (j = 0; j<(w >> 1); j++, p += 4)
            * u++ = p[1];

    p = uyvy;

    for (i = 0; i<(h >> 1); i++, p += (w << 1))
        for (j = 0; j<(w >> 1); j++, p += 4)
            * v++ = p[3];

    return 0;
}

struct v4l2_capability cap;
struct v4l2_format fmt;
struct v4l2_buffer buf;
struct v4l2_requestbuffers rb;
void *mem[NB_BUFFER];
int fd;
int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

int start_capture(void)
{

    FAIL_ON_NEGATIVE(fd = open("/dev/video0", O_RDWR | O_NONBLOCK))

    memset(&cap, 0, sizeof(struct v4l2_capability));

    FAIL_ON_NEGATIVE(ioctl(fd, VIDIOC_QUERYCAP, &cap))

    memset(&fmt, 0, sizeof(struct v4l2_format));
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.width = display_width;
    fmt.fmt.pix.height = display_height;
    fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
    fmt.fmt.pix.field = V4L2_FIELD_ANY;

    FAIL_ON_NEGATIVE(ioctl(fd, VIDIOC_S_FMT, &fmt))

    struct v4l2_streamparm setfps;

    memset(&setfps, 0, sizeof(struct v4l2_streamparm));
    setfps.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    setfps.parm.capture.timeperframe.numerator = 1;
    setfps.parm.capture.timeperframe.denominator = capture_frame_rate;

    FAIL_ON_NONZERO(ioctl(fd, VIDIOC_S_PARM, &setfps))

    memset(&rb, 0, sizeof(struct v4l2_requestbuffers));
    rb.count = NB_BUFFER;
    rb.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    rb.memory = V4L2_MEMORY_MMAP;

    FAIL_ON_NONZERO(ioctl(fd, VIDIOC_REQBUFS, &rb))

    int i;

    /* map the buffers */
    for (i = 0; i < NB_BUFFER; i++)
    {
        memset(&buf, 0, sizeof(struct v4l2_buffer));
        buf.index = i;
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;

        FAIL_ON_NEGATIVE(ioctl(fd, VIDIOC_QUERYBUF, &buf))

        mem[i] = mmap(0 , buf.length, PROT_READ, MAP_SHARED, fd, buf.m.offset);

        if (mem[i] == MAP_FAILED)
        {
            cout << "Error mapping buffers." << endl;
            return -1;
        }
    }

    /* Queue the buffers. */
    for (i = 0; i < NB_BUFFER; ++i)
    {
        memset(&buf, 0, sizeof(struct v4l2_buffer));
        buf.index = i;
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;

        FAIL_ON_NEGATIVE(ioctl(fd, VIDIOC_QBUF, &buf))
    }

    // start streaming
    FAIL_ON_NEGATIVE(ioctl(fd, VIDIOC_STREAMON, &type))
    return -1;
}

int get_frame(void)
{
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;

    // get a frame if we can
    if (ioctl(fd, VIDIOC_DQBUF, &buf) < 0)
        return -1;

    // super ugly conversion :)
    if (buf.bytesused > 0)
    {
        uyvy2yv12((char *) mem[buf.index], display_width, display_height);
        //fwrite(frame, w*h*3/2, 1, captureFile);
    }

    // put the buffer back
    FAIL_ON_NEGATIVE(ioctl(fd, VIDIOC_QBUF, &buf))

    buffer_time = get_time() / 1000.000;

    if (count_captured_frames++ < drop_first)
        return -1;

    return 0;
}

int stop_capture(void)
{
    //fclose(captureFile);
    FAIL_ON_NEGATIVE(ioctl(fd, VIDIOC_STREAMOFF, &type))

    return 0;
}

#endif
#endif

int create_packetizer(
    PACKETIZER *x,
    FEC_TYPE fecType,
    unsigned int fec_numerator,
    unsigned int fec_denominator
)
{
    x->size            = PACKET_SIZE;
    x->fecType 	   = fecType;
    x->fec_numerator   = fec_numerator;
    x->fec_denominator = fec_denominator;
    x->new_fec_denominator = fec_denominator;
    x->max			   = PS;
    x->count = 0;
    x->add_ptr = 0;
    x->send_ptr = 0;
    x->fec_count = x->fec_denominator;

    x->seq = 7;
    x->send_ptr = x->add_ptr = (x->seq & PSM);
    return 0; // SUCCESS
}

int make_redundant_packet
(
    PACKETIZER *p,
    unsigned int end_frame,
    unsigned int time,
    unsigned int frametype
)
{
    long long *in[MAX_NUMERATOR];
    long long *out = (long long *) p->packet[p->add_ptr].data;
    unsigned int i, j;
    unsigned int max_size = 0;
    unsigned int max_round;

    // make a number of exact duplicates of this packet
    if (p->fec_denominator == 1)
    {
        int dups = p->fec_numerator - p->fec_denominator ;
        void *duplicand = (void *) &p->packet[(p->add_ptr-1)&PSM];

        while (dups)
        {
            memcpy((void *) &p->packet[p->add_ptr], duplicand, sizeof(PACKET));
            dups --;
            p->add_ptr++;
            p->add_ptr &= PSM;
        }

        p->fec_denominator = p->new_fec_denominator;
        p->fec_count = p->fec_denominator;
        p->count ++;
        return 0;
    }

    p->packet[p->add_ptr].timestamp = time;
    p->packet[p->add_ptr].seq = p->seq;
    p->packet[p->add_ptr].size = max_size;
    p->packet[p->add_ptr].type = XORPACKET;
    p->packet[p->add_ptr].redundant_count = p->fec_denominator;
    p->packet[p->add_ptr].new_frame = 0;
    p->packet[p->add_ptr].end_frame = end_frame;
    p->packet[p->add_ptr].frame_type = frametype;

    // find address of last denominator packets data store in in ptr
    for (i = 0; i < p->fec_denominator; i++)
    {
        int ptr = ((p->add_ptr - i - 1)&PSM);
        in[i] = (long long *) p->packet[ptr].data;;
        max_size = (max_size > p->packet[ptr].size ? max_size : p->packet[ptr].size);
    }

    // go through a full packet size
    max_round = (max_size + sizeof(long long) - 1) / sizeof(long long);

    for (j = 0; j < max_round; j++)
    {
        // start with the most recent packet
        *out = *(in[0]);

        // xor all the older packets with out
        for (i = 1; i < p->fec_denominator; i++)
        {
            *out ^= *(in[i]);
            in[i]++;
        }

        in[0]++;
        out++;
    }

    p->seq ++;

    // move to the next packet
    p->add_ptr ++;
    p->add_ptr &= PSM;

    // add one to our packet count
    p->count ++;

    if (p->count > p->max)
        return -1; // filled up our packet buffer

    p->fec_denominator = p->new_fec_denominator;
    p->fec_count = p->fec_denominator;
    return 0;
}

int packetize
(
    PACKETIZER *p,
    unsigned int time,
    unsigned char *data,
    unsigned int size,
    unsigned int frame_type
)
{
    int new_frame = 1;

    // more bytes to copy around
    while (size > 0)
    {
        unsigned int psize = (p->size < size ? p->size : size);
        p->packet[p->add_ptr].timestamp = time;
        p->packet[p->add_ptr].seq = p->seq;
        p->packet[p->add_ptr].size = psize;
        p->packet[p->add_ptr].type = DATAPACKET;

        if (p->fec_denominator == 1)
            p->packet[p->add_ptr].redundant_count = 2;
        else
            p->packet[p->add_ptr].redundant_count = p->fec_count;

        p->packet[p->add_ptr].new_frame = new_frame;
        p->packet[p->add_ptr].frame_type = frame_type;
        //vpxlog_dbg(SKIP, "%c", (frame_type==NORMAL?'N':'O'));

        new_frame = 0;

        memcpy(p->packet[p->add_ptr].data, data, psize);

        // make sure rest of packet is 0'ed out for redundancy if necessary.
        if (size < p->size)
            memset(p->packet[p->add_ptr].data + psize, 0, p->size - psize);

        data += psize;
        size -= psize;
        p->packet[p->add_ptr].end_frame = (size == 0);

        p->seq ++;
        p->add_ptr ++;
        p->add_ptr &= PSM;

        p->count ++;

        if (p->count > p->max)
            return -1; // filled up our packet buffer

        // time for redundancy?
        p->fec_count --;

        if (!p->fec_count)
            make_redundant_packet(p, (size == 0), time, frame_type);
    }

    return 0;
}
#define WRITEFILE
//#define ONEWAY
int send_packet(PACKETIZER *p, struct vpxsocket *vpxSock, union vpx_sockaddr_x address)
{
    TCRV rc;
    tc32 bytes_sent;

    if (p->send_ptr == p->add_ptr)
        return -1;

    p->packet[p->send_ptr].ssrc = 411;
    vpxlog_dbg(LOG_PACKET, "Sent Packet %d, %d, %d : new=%d \n", p->packet[p->send_ptr].seq, p->packet[p->send_ptr].timestamp, p->packet[p->send_ptr].frame_type, p->packet[p->send_ptr].new_frame);
#ifndef ONEWAY
    rc = vpx_net_sendto(vpxSock, (tc8 *) &p->packet[p->send_ptr], PACKET_HEADER_SIZE + p->packet[p->send_ptr].size, &bytes_sent, address);
#endif
    p->send_ptr ++;
    p->send_ptr &= PSM;
    p->count --;

    return 0;
}
void ctx_exit_on_error(vpx_codec_ctx_t *ctx, const char *s)
{
    if (ctx->err)
    {
        vpxlog_dbg(FRAME, "%s: %s\n", s, vpx_codec_error(ctx));
        exit(EXIT_FAILURE);
    }
}

int main(int argc, char *argv[])
{
    char ip[512];
    int flags = 0;
    strncpy(ip, "127.0.0.1", 512);
    printf("GrabCompressAndSend: (-? for help) \n");

    vpx_codec_enc_cfg_t    cfg;

    vpx_codec_enc_config_default(&vpx_codec_vp8_cx_algo, &cfg, 0);
    cfg.rc_target_bitrate = video_bitrate;
    cfg.g_w = display_width;
    cfg.g_h = display_height;
    cfg.g_timebase.num = 1;
    cfg.g_timebase.den = (int) 10000000;
    cfg.rc_end_usage = VPX_CBR;
    cfg.g_pass = VPX_RC_ONE_PASS;
    cfg.g_lag_in_frames = 0;
    cfg.rc_min_quantizer = 20;
    cfg.rc_max_quantizer = 50;
    cfg.rc_dropframe_thresh = 1;
    cfg.rc_buf_optimal_sz = 1000;
    cfg.rc_buf_initial_sz = 1000;
    cfg.rc_buf_sz = 1000;
    cfg.g_error_resilient = 1;
    cfg.kf_mode = VPX_KF_DISABLED;
    cfg.kf_max_dist = 999999;
    cfg.g_threads = 1;


    int cpu_used = 8;
    int static_threshold = 1200;

    while (--argc > 0)
    {
        if (argv[argc][0] == '-')
        {
            switch (argv[argc][1])
            {
            case 'm':
            case 'M':
                cfg.rc_dropframe_thresh = atoi(argv[argc--+1]);
                break;
            case 'c':
            case 'C':
                cpu_used = atoi(argv[argc--+1]);
                break;
            case 't':
            case 'T':
                static_threshold = atoi(argv[argc--+1]);
                break;
            case 'b':
            case 'B':
                cfg.rc_min_quantizer = atoi(argv[argc--+1]);
                break;
            case 'q':
            case 'Q':
                cfg.rc_max_quantizer = atoi(argv[argc--+1]);
                break;
            case 'd':
            case 'D':
                drop_first = atoi(argv[argc--+1]);
                break;
            case 'i':
            case 'I':
                strncpy(ip, argv[argc--+1], 512);
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
                    "========================: \n"
                    "Captures, compresses and sends video to ReceiveDecompressand play sample\n\n"
                    "-m [1] buffer level at which to drop frames 0 shuts it off \n"
                    "-c [12] amount of cpu to leave free of 16 \n"
                    "-t [1200] sad score below which is just a copy \n"
                    "-b [20] minimum quantizer ( best frame quality )\n"
                    "-q [52] maximum frame quantizer ( worst frame quality ) \n"
                    "-d [60] number of frames to drop at the start\n"
                    "-i [127.0.0.1]    Port to send data to. \n"
                    "-s [1408] port to send requests to\n"
                    "-r [1407] port to receive requests on. \n"
                    "\n");
                exit(0);
                break;
            }
        }
    }

    struct vpxsocket vpx_socket, vpx_socket2;

    union vpx_sockaddr_x address, address2;

    TCRV rc;

    int i;

    int bytes_read;

#ifdef WINDOWS
    HRESULT hr;

    HRE(CoInitialize(NULL));

#endif

#ifdef WRITEFILE
    FILE *out_file = fopen("test.vpx", "wb");

#endif

    int request_recovery = 0;

    int gold_recovery_seq = 0;

    int altref_recovery_seq = 0;

    unsigned int recovery_flags[] =
    {
        0, //   NORMAL,
        VPX_EFLAG_FORCE_KF, //   KEY,
        VP8_EFLAG_FORCE_GF | VP8_EFLAG_NO_UPD_ARF |
        VP8_EFLAG_NO_REF_LAST | VP8_EFLAG_NO_REF_ARF, //   GOLD = 2,
        VP8_EFLAG_FORCE_ARF | VP8_EFLAG_NO_UPD_GF |
        VP8_EFLAG_NO_REF_LAST | VP8_EFLAG_NO_REF_GF  //   ALTREF = 3
    };

    vpx_net_init();

    // data send socket
    FAIL_ON_NONZERO(vpx_net_open(&vpx_socket, vpx_IPv4, vpx_UDP))
    FAIL_ON_NONZERO(vpx_net_get_addr_info(ip, send_port, vpx_IPv4, vpx_UDP, &address))
    // feedback socket
    FAIL_ON_NONZERO(vpx_net_open(&vpx_socket2, vpx_IPv4, vpx_UDP))
    vpx_net_set_read_timeout(&vpx_socket2, 0);

    rc = vpx_net_bind(&vpx_socket2, 0 , recv_port);

    vpx_net_set_send_timeout(&vpx_socket, vpx_NET_NO_TIMEOUT);

    // make sure 2 way discussion taking place before getting started

    int bytes_sent;

#ifndef ONEWAY

    while (!_kbhit())
    {
        char init_packet[PACKET_SIZE] = "initiate call";
        rc = vpx_net_sendto(&vpx_socket, (tc8 *) &init_packet, PACKET_SIZE , &bytes_sent, address);
        Sleep(200);

        rc = vpx_net_recvfrom(&vpx_socket2, one_packet, sizeof(one_packet), &bytes_read, &address2);

        if (rc != TC_OK && rc != TC_WOULDBLOCK)
            vpxlog_dbg(LOG_PACKET, "error\n");

        if (bytes_read == -1)
            bytes_read = 0;

        if (bytes_read)
        {
            if (strncmp(one_packet, "configuration ", 14) == 0)
            {
                sscanf(one_packet + 14, "%d %d %d %d %d %d", &display_width, &display_height, &capture_frame_rate, &video_bitrate, &fec_numerator, &fec_denominator);
                printf("Dimensions: %dx%-d %dfps %dkbps %d/%dFEC\n", display_width, display_height, capture_frame_rate, video_bitrate, fec_numerator, fec_denominator);
                break;
            }
        }
    }

    char init_packet[PACKET_SIZE] = "confirmed";
    rc = vpx_net_sendto(&vpx_socket, (tc8 *) &init_packet, PACKET_SIZE , &bytes_sent, address);
    Sleep(200);
    rc = vpx_net_sendto(&vpx_socket, (tc8 *) &init_packet, PACKET_SIZE , &bytes_sent, address);
    Sleep(200);
    rc = vpx_net_sendto(&vpx_socket, (tc8 *) &init_packet, PACKET_SIZE , &bytes_sent, address);
#endif

    vpx_codec_ctx_t        encoder;
    vpx_img_alloc(&raw, IMG_FMT_YV12, display_width, display_height, 1);

    cfg.rc_target_bitrate = video_bitrate;

    vpx_codec_enc_init(&encoder, &vpx_codec_vp8_cx_algo, &cfg, 0);
    vpx_codec_control_(&encoder, VP8E_SET_CPUUSED, cpu_used);
    vpx_codec_control_(&encoder, VP8E_SET_STATIC_THRESHOLD, static_threshold);
    vpx_codec_control_(&encoder, VP8E_SET_ENABLEAUTOALTREF, 0);

    create_packetizer(&x, XOR, fec_numerator, fec_denominator);
    //HRE(CoInitialize(NULL));

    start_capture();
    vpx_net_set_read_timeout(&vpx_socket2, 1);

    for (i = 0; !_kbhit();)
    {

        // if there is nothing to send
#ifndef ONEWAY
        rc = vpx_net_recvfrom(&vpx_socket2, one_packet, sizeof(one_packet), &bytes_read, &address2);

        if (rc != TC_OK && rc != TC_WOULDBLOCK && rc != TC_TIMEDOUT)
            vpxlog_dbg(LOG_PACKET, "error\n");

        if (bytes_read == -1)
            bytes_read = 0;

        if (bytes_read)
        {
            unsigned char command = one_packet[0];
            unsigned short seq = *((unsigned short *)(1 + one_packet));
            int bytes_sent;

            PACKET *tp = &x.packet[seq&PSM];
            vpxlog_dbg(SKIP, "Command :%c Seq:%d FT:%c RecoverySeq:%d AltSeq:%d \n",
                        command, seq, (tp->frame_type == NORMAL ? 'N' : 'G'),
                        gold_recovery_seq, altref_recovery_seq);

            // requested to resend a packet ( ignore if we are about to send a recovery frame)
            if (command == 'r' && request_recovery == 0)
            {
                rc = vpx_net_sendto(&vpx_socket, (tc8 *) &x.packet[seq&PSM],
                                    PACKET_HEADER_SIZE + x.packet[seq&PSM].size, &bytes_sent, address);
                vpxlog_dbg(SKIP, "Sent recovery packet %c:%d, %d,%d\n", command, tp->frame_type, seq, tp->timestamp);
            }

            int recovery_seq = gold_recovery_seq;
            int recovery_type = GOLD;
            int other_recovery_seq = altref_recovery_seq;
            int other_recovery_type = ALTREF;

            if ((unsigned short)(recovery_seq - altref_recovery_seq > 32768))
            {
                recovery_seq = altref_recovery_seq;
                recovery_type = ALTREF;
                other_recovery_seq = gold_recovery_seq;
                other_recovery_type = GOLD;
            }

            // if requested to recover but seq is before recovery RESEND
            if ((unsigned short)(seq - recovery_seq) > 32768 || command != 'g')
            {
                rc = vpx_net_sendto(&vpx_socket, (tc8 *) &x.packet[seq&PSM],
                                    PACKET_HEADER_SIZE + x.packet[seq&PSM].size, &bytes_sent, address);
                vpxlog_dbg(SKIP, "Sent recovery packet %c:%d, %d,%d\n", command, tp->frame_type, seq, tp->timestamp);
                continue;
            }

            // requested  recovery frame and its a normal frame packet that's lost and seq is after our recovery frame so make a long term ref frame
            if (tp->frame_type == NORMAL && (unsigned short)(seq - recovery_seq) > 0 && (unsigned short)(seq - recovery_seq) < 32768)
            {
                request_recovery = recovery_type;
                vpxlog_dbg(SKIP, "Requested recovery frame %c:%c,%d,%d\n", command, (recovery_type == GOLD ? 'G' : 'A'), x.packet[gold_recovery_seq&PSM].frame_type, seq, x.packet[gold_recovery_seq&PSM].timestamp);
                continue;
            }

            // so the other one is too old request a recovery frame from our older reference buffer.
            if ((unsigned short)(seq - other_recovery_seq) > 0 && (unsigned short)(seq - other_recovery_seq) < 32768)
            {
                request_recovery = other_recovery_type;
                vpxlog_dbg(SKIP, "Requested recovery frame %c:%c,%d,%d\n", command, (other_recovery_type == GOLD ? 'G' : 'A'), x.packet[gold_recovery_seq&PSM].frame_type, seq, x.packet[gold_recovery_seq&PSM].timestamp);
                continue;
            }

            // nothing else we can do ask for a key
            request_recovery = KEY;
            vpxlog_dbg(SKIP, "Requested key frame %c:%d,%d\n", command, tp->frame_type, seq, tp->timestamp);

            continue;
        }

#endif
        send_packet(&x, &vpx_socket, address);
        vpx_net_set_read_timeout(&vpx_socket2, 1);

        // check to see if we have a frame in our packet store.
        if (get_frame() == 0)
        {
            // do we have room in our packet store for a frame
            if (x.add_ptr - x.send_ptr < 20)
            {
                int frame_type;
                long long time_in_nano_seconds = (long long)(buffer_time * 10000000.000 + .5);
                unsigned int rtptime = (unsigned int)((long long)(buffer_time * 1000000.000) & 0xffffffff);
                double fps = 10000000.000 / (time_in_nano_seconds - last_time_in_nanoseconds);

                //printf("%14.4g\n",fps);
                const vpx_codec_cx_pkt_t *pkt;
                vpx_codec_iter_t iter = NULL;
                flags = recovery_flags[request_recovery];
                vpx_codec_encode(&encoder, &raw, time_in_nano_seconds, 30000000, flags, VPX_DL_REALTIME);
                ctx_exit_on_error(&encoder, "Failed to encode frame");

                while ((pkt = vpx_codec_get_cx_data(&encoder, &iter)))
                {
                    if (pkt->kind == VPX_CODEC_CX_FRAME_PKT)
                    {
                        last_time_in_nanoseconds = time_in_nano_seconds;

                        frame_type = request_recovery;

                        // a recovery frame was requested move sendptr to current ptr, so that we
                        // don't spend datarate sending packets that won't be used.
                        if (request_recovery)
                        {
                            x.send_ptr = x.add_ptr ;
                            request_recovery = 0;
                        }

                        if (frame_type == GOLD || frame_type == KEY)
                            gold_recovery_seq = x.seq;

                        if (frame_type == ALTREF || frame_type == KEY)
                            altref_recovery_seq = x.seq;

                        packetize(&x, rtptime, (unsigned char *) pkt->data.frame.buf, pkt->data.frame.sz, frame_type);

                        vpxlog_dbg(FRAME, "Frame %d %d %d %10.4g %d\n", x.packet[x.send_ptr].seq, pkt->data.frame.sz, x.packet[x.send_ptr].timestamp, fps, gold_recovery_seq);
#ifdef WRITEFILE
                        fwrite(&pkt->data.frame.sz, 4, 1, out_file);
                        fwrite(pkt->data.frame.buf, pkt->data.frame.sz, 1, out_file);
#endif
                        i++;
                    }
                }
            }

            buffer_has_frame = false;
        }

    }

#ifdef WINDOWS
//    graph->Abort();
    CoUninitialize();
#endif

    vpx_net_close(&vpx_socket2);
    vpx_net_close(&vpx_socket);
    vpx_net_destroy();

#ifdef WRITEFILE
    fclose(out_file);
#endif

    vpx_codec_destroy(&encoder);
    vpx_img_free(&raw);
    return 0;
}
