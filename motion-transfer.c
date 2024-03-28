/*
 * Copyright (c) 2024 Yahweasel
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED “AS IS” AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY
 * SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER
 * RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF
 * CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN
 * CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <stdio.h>
#include <stdlib.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>

#define ENCODER "h263p"
#define DECODER "h263p"

static char avError[1024] = {0};

#define SF0(ret, func, args) do { \
    (ret) = func args; \
    if ((ret) != 0) { \
        av_strerror((ret), avError, sizeof(avError) - 1); \
        fprintf(stderr, "%s (%d): %s\n", #func, __LINE__, avError); \
        exit(1); \
    } \
} while (0)

#define SFN(ret, func, args) do { \
    (ret) = func args; \
    if (!(ret)) { \
        perror(#func); \
        exit(1); \
    } \
} while (0)

static AVFrame *scaleFrame(
    AVFrame *inFrame, double factor, enum AVPixelFormat toPixFmt
) {
    struct SwsContext *swsc;
    AVFrame *outFrame;
    int ret;

    SFN(swsc, sws_getContext, (inFrame->width, inFrame->height, inFrame->format, 
                               inFrame->width * factor, inFrame->height * factor, toPixFmt,
                               0, NULL, NULL, NULL));
    SFN(outFrame, av_frame_alloc, ());
    ret = sws_scale_frame(swsc, outFrame, inFrame);
    if (ret < 0) {
        av_strerror(ret, avError, sizeof(avError) - 1);
        fprintf(stderr, "%s: %s\n", "sws_scale_frame", avError);
        exit(1);
    }
    return outFrame;
}

static AVFrame *inputFrame(char *inUrl)
{
    AVFormatContext *fc = NULL;
    AVCodecContext *cc;
    const AVCodec *c;
    AVPacket *pkt;
    AVFrame *frame, *yuvFrame;
    int ret;

    /* open the input */
    SF0(ret, avformat_open_input, (&fc, inUrl, NULL, NULL));
    SF0(ret, avformat_find_stream_info, (fc, NULL));
    SFN(c, avcodec_find_decoder, (fc->streams[0]->codecpar->codec_id));
    SFN(cc, avcodec_alloc_context3, (c));
    SF0(ret, avcodec_parameters_to_context, (cc, fc->streams[0]->codecpar));
    SF0(ret, avcodec_open2, (cc, c, NULL));

    /* read a frame */
    SFN(pkt, av_packet_alloc, ());
    SF0(ret, av_read_frame, (fc, pkt));
    SF0(ret, avcodec_send_packet, (cc, pkt));
    SF0(ret, avcodec_send_packet, (cc, NULL));
    av_packet_free(&pkt);
    SFN(frame, av_frame_alloc, ());
    SF0(ret, avcodec_receive_frame, (cc, frame));

    /* clean up */
    avcodec_free_context(&cc);
    avformat_close_input(&fc);

    yuvFrame = scaleFrame(frame, 1, AV_PIX_FMT_YUV420P);
    av_frame_free(&frame);

    return yuvFrame;
}

static AVCodecContext *getEncoder(AVFrame *frame)
{
    const AVCodec *c;
    AVCodecContext *cc;
    AVDictionary *opts;
    int ret;

    SFN(c, avcodec_find_encoder_by_name, (ENCODER));
    SFN(cc, avcodec_alloc_context3, (c));
    cc->time_base.num = 1;
    cc->time_base.num = 60;
    cc->width = frame->width;
    cc->height = frame->height;
    cc->pix_fmt = AV_PIX_FMT_YUV420P;
    cc->flags |= AV_CODEC_FLAG_QSCALE;
    cc->global_quality = 1;
    cc->bit_rate = frame->height * 100000LL;
    opts = NULL;
    SF0(ret, av_dict_set, (&opts, "intra_penalty", "256", 0));
    SF0(ret, av_dict_set, (&opts, "crf", "23", 0));
    SF0(ret, avcodec_open2, (cc, c, &opts));
    av_dict_free(&opts);

    return cc;
}

int main(int argc, char **argv)
{
    AVFrame *i1a, *i1b, *i2a, *o2bYUV, *o2b;
    const AVCodec *c;
    AVFormatContext *pngFmt;
    AVCodecContext *ecc, *dcc;
    AVPacket *iframePkt, *motionPkt, *pngPkt;
    AVStream *pngStr;
    const AVPixFmtDescriptor *pixDesc;
    AVDictionary *opts;
    int ret, ret2;

    /* Read input frames */
    i1a = inputFrame(argv[1]);
    i1b = inputFrame(argv[2]);
    i2a = inputFrame(argv[3]);

    /* Encode the motion */
    ecc = getEncoder(i1a);
    i1a->pts = 0;
    i1a->pict_type = AV_PICTURE_TYPE_NONE;
    SF0(ret, avcodec_send_frame, (ecc, i1a));
    i1b->pts = 1;
    i1b->pict_type = AV_PICTURE_TYPE_NONE;
    SF0(ret, avcodec_send_frame, (ecc, i1b));
    SFN(iframePkt, av_packet_alloc, ());
    ret = avcodec_receive_packet(ecc, iframePkt);
    av_packet_unref(iframePkt);
    SFN(motionPkt, av_packet_alloc, ());
    if (ret < 0) {
        SF0(ret, avcodec_send_frame, (ecc, NULL));
        SF0(ret, avcodec_receive_packet, (ecc, iframePkt));
    }
    SF0(ret, avcodec_receive_packet, (ecc, motionPkt));
    avcodec_free_context(&ecc);

    /* Encode the second non-motion */
    ecc = getEncoder(i2a);
    i2a->pts = 0;
    i2a->pict_type = AV_PICTURE_TYPE_NONE;
    SF0(ret, avcodec_send_frame, (ecc, i2a));
    av_packet_unref(iframePkt);
    ret = avcodec_receive_packet(ecc, iframePkt);
    if (ret < 0) {
        SF0(ret, avcodec_send_frame, (ecc, NULL));
        SF0(ret, avcodec_receive_packet, (ecc, iframePkt));
    }
    avcodec_free_context(&ecc);

    /* Apply the 1 motion to the 2 frame */
    SFN(c, avcodec_find_decoder_by_name, (DECODER));
    SFN(dcc, avcodec_alloc_context3, (c));
    SF0(ret, avcodec_open2, (dcc, c, NULL));
    SF0(ret, avcodec_send_packet, (dcc, iframePkt));
    SFN(o2bYUV, av_frame_alloc, ());
    ret2 = avcodec_receive_frame(dcc, o2bYUV);
    SF0(ret, avcodec_send_packet, (dcc, motionPkt));
    SF0(ret, avcodec_send_packet, (dcc, NULL));
    if (ret2 < 0)
        SF0(ret, avcodec_receive_frame, (dcc, o2bYUV));
    av_frame_unref(o2bYUV);
    SF0(ret, avcodec_receive_frame, (dcc, o2bYUV));

    o2b = scaleFrame(o2bYUV, 1, AV_PIX_FMT_RGB24);
    av_frame_free(&o2bYUV);

    /* Encode it as png */
    SFN(c, avcodec_find_encoder_by_name, ("png"));
    SFN(ecc, avcodec_alloc_context3, (c));
    ecc->time_base.num = 1;
    ecc->time_base.den = 60;
    ecc->width = o2b->width;
    ecc->height = o2b->height;
    ecc->pix_fmt = AV_PIX_FMT_RGB24;
    SF0(ret, avcodec_open2, (ecc, c, NULL));
    SF0(ret, avcodec_send_frame, (ecc, o2b));
    SFN(pngPkt, av_packet_alloc, ());
    SF0(ret, avcodec_receive_packet, (ecc, pngPkt));
    pngPkt->pts = 1;

    /* And save it as png */
    SF0(ret, avformat_alloc_output_context2, (&pngFmt, NULL, "image2", argv[4]));
    SFN(pngStr, avformat_new_stream, (pngFmt, NULL));
    SF0(ret, avcodec_parameters_from_context, (pngStr->codecpar, ecc));
    opts = NULL;
    SF0(ret, av_dict_set, (&opts, "update", "1", 0));
    SF0(ret, avformat_write_header, (pngFmt, &opts));
    av_dict_free(&opts);
    SF0(ret, av_write_frame, (pngFmt, pngPkt));
    SF0(ret, av_write_trailer, (pngFmt));

    return 0;
}
