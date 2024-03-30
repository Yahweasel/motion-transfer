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
#include <string.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>

#define ENCODER "h263p"
#define DECODER "h263p"

static char avError[1024] = {0};

struct List {
    struct List *next;
    void *val;
};

enum ArgMode {
    ARG_NONE,
    ARG_MOTION,
    ARG_INPUT,
    ARG_OUTPUT
};

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

static AVFrame *scaleFrame(AVFrame *inFrame, enum AVPixelFormat toPixFmt)
{
    struct SwsContext *swsc;
    AVFrame *outFrame;
    int ret;

    SFN(swsc, sws_getContext, (inFrame->width, inFrame->height, inFrame->format, 
                               inFrame->width, inFrame->height, toPixFmt,
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

static AVFrame *readFrame(char *inUrl)
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

    yuvFrame = scaleFrame(frame, AV_PIX_FMT_YUV420P);
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
    cc->time_base.den = 60;
    cc->width = frame->width;
    cc->height = frame->height;
    cc->pix_fmt = AV_PIX_FMT_YUV420P;
    cc->flags |= AV_CODEC_FLAG_QSCALE;
    cc->global_quality = 1;
    cc->bit_rate = frame->height * 100000LL;
    cc->keyint_min = cc->gop_size = 600;
    opts = NULL;
    SF0(ret, av_dict_set, (&opts, "intra_penalty", "256", 0));
    SF0(ret, av_dict_set, (&opts, "crf", "23", 0));
    SF0(ret, avcodec_open2, (cc, c, &opts));
    av_dict_free(&opts);

    return cc;
}

void readMotionFile(
    char *file, AVCodecContext **cc, struct List **head, struct List **tail
) {
    AVFrame *frame;
    AVPacket *pkt;
    struct List *lst;
    int ret;

    if (file) {
        frame = readFrame(file);
        frame->pict_type = AV_PICTURE_TYPE_NONE;
    } else {
        frame = NULL;
    }

    if (*cc) {
        /* The codec is available, so encode this motion */
        SF0(ret, avcodec_send_frame, (*cc, frame));
        while (1) {
            SFN(pkt, av_packet_alloc, ());
            ret = avcodec_receive_packet(*cc, pkt);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
                break;
            if (ret < 0)
                SF0(ret, avcodec_receive_packet, (*cc, pkt));
            SFN(lst, (struct List *) calloc, (1, sizeof(struct List)));
            lst->val = pkt;
            if (*tail) {
                (*tail)->next = lst;
                *tail = lst;
            } else {
                *head = *tail = lst;
            }
            pkt = NULL;
        }

    } else {
        /* The codec hasn't been allocated, so just send the initial data */
        SFN(pkt, av_packet_alloc, ());
        *cc = getEncoder(frame);
        SF0(ret, avcodec_send_frame, (*cc, frame));
        while (ret == 0) {
            av_packet_unref(pkt);
            ret = avcodec_receive_packet(*cc, pkt);
        }
        av_packet_free(&pkt);
    }

    av_frame_free(&frame);

    if (!file) {
        /* Clean up the encoder */
        avcodec_free_context(cc);
    }
}

AVFrame *applyMotion(AVFrame *firstFrame, struct List *motion)
{
    AVCodecContext *ecc, *dcc;
    const AVCodec *c;
    AVPacket *iframePkt;
    AVFrame *outputFrame;
    int ret;

    SFN(iframePkt, av_packet_alloc, ());
    SFN(outputFrame, av_frame_alloc, ());

    /* Make the I-frame packet */
    ecc = getEncoder(firstFrame);
    firstFrame->pts = 0;
    firstFrame->pict_type = AV_PICTURE_TYPE_NONE;
    SF0(ret, avcodec_send_frame, (ecc, firstFrame));
    ret = avcodec_receive_packet(ecc, iframePkt);
    if (ret < 0) {
        SF0(ret, avcodec_send_frame, (ecc, NULL));
        SF0(ret, avcodec_receive_packet, (ecc, iframePkt));
    }
    avcodec_free_context(&ecc);

    /* Decode using this I-frame and those motion packets */
    SFN(c, avcodec_find_decoder_by_name, (DECODER));
    SFN(dcc, avcodec_alloc_context3, (c));
    SF0(ret, avcodec_open2, (dcc, c, NULL));
    SF0(ret, avcodec_send_packet, (dcc, iframePkt));
    av_packet_free(&iframePkt);
    for (; motion; motion = motion->next) {
        while (1) {
            av_frame_unref(outputFrame);
            if (avcodec_receive_frame(dcc, outputFrame) >= 0)
                break;
        }
        SF0(ret, avcodec_send_packet, (dcc, (AVPacket *) motion->val));
    }
    if (avcodec_receive_frame(dcc, outputFrame) < 0) {
        SF0(ret, avcodec_send_packet, (dcc, NULL));
        SF0(ret, avcodec_receive_frame, (dcc, outputFrame));
    }
    avcodec_free_context(&dcc);

    return outputFrame;
}

void writeFrame(char *file, AVFrame *yuvFrame)
{
    AVFrame *rgbFrame;
    AVPacket *pngPkt;
    const AVCodec *c;
    AVCodecContext *ecc;
    AVFormatContext *pngFmt = NULL;
    AVStream *pngStr;
    AVDictionary *opts;
    int ret;

    rgbFrame = scaleFrame(yuvFrame, AV_PIX_FMT_RGB24);

    /* Encode it as png */
    SFN(c, avcodec_find_encoder_by_name, ("png"));
    SFN(ecc, avcodec_alloc_context3, (c));
    ecc->time_base.num = 1;
    ecc->time_base.den = 60;
    ecc->width = rgbFrame->width;
    ecc->height = rgbFrame->height;
    ecc->pix_fmt = AV_PIX_FMT_RGB24;
    SF0(ret, avcodec_open2, (ecc, c, NULL));
    SF0(ret, avcodec_send_frame, (ecc, rgbFrame));
    av_frame_free(&rgbFrame);
    SFN(pngPkt, av_packet_alloc, ());
    SF0(ret, avcodec_receive_packet, (ecc, pngPkt));
    pngPkt->pts = 1;

    /* And save it as png */
    SF0(ret, avformat_alloc_output_context2, (&pngFmt, NULL, "image2", file));
    SFN(pngStr, avformat_new_stream, (pngFmt, NULL));
    SF0(ret, avcodec_parameters_from_context, (pngStr->codecpar, ecc));
    opts = NULL;
    SF0(ret, av_dict_set, (&opts, "update", "1", 0));
    SF0(ret, avformat_write_header, (pngFmt, &opts));
    av_dict_free(&opts);
    SF0(ret, av_write_frame, (pngFmt, pngPkt));
    SF0(ret, av_write_trailer, (pngFmt));
    avformat_free_context(pngFmt);
}

int main(int argc, char **argv)
{
    enum ArgMode argMode = ARG_NONE;
    int argi;

    struct List *motionList = NULL, *motionListTail = NULL;
    AVFrame *inputFrame = NULL;
    AVCodecContext *motionCCtx = NULL;

    av_log_set_level(AV_LOG_ERROR);

    for (argi = 1; argi < argc; argi++) {
        char *arg = argv[argi];

        if (arg[0] == '-') {
            if (motionCCtx)
                readMotionFile(NULL, &motionCCtx, &motionList, &motionListTail);

            if (!strcmp(arg, "-m")) {
                struct List *lst, *next;

                /* Read in motion data */
                argMode = ARG_MOTION;

                /* Free any existing data */
                for (lst = motionList; lst; lst = next) {
                    next = lst->next;
                    av_packet_free((AVPacket **) &lst->val);
                    free(lst);
                }

            } else if (!strcmp(arg, "-i")) {
                argMode = ARG_INPUT;

            } else if (!strcmp(arg, "-o")) {
                argMode = ARG_OUTPUT;

            } else {
                fprintf(stderr, "Unrecognized argument: %s\n", arg);
                exit(1);

            }

            continue;
        }

        /* Filename */
        switch (argMode) {
            case ARG_MOTION:
                readMotionFile(arg, &motionCCtx, &motionList, &motionListTail);
                break;

            case ARG_INPUT:
                argMode = ARG_NONE;
                if (inputFrame)
                    av_frame_free(&inputFrame);
                inputFrame = readFrame(arg);
                break;

            case ARG_OUTPUT:
            {
                AVFrame *outputFrame;
                argMode = ARG_NONE;
                outputFrame = applyMotion(inputFrame, motionList);
                writeFrame(arg, outputFrame);
                av_frame_free(&outputFrame);
                break;
            }

            default:
                fprintf(stderr, "Unexpected argument: %s\n", arg);
                exit(1);
        }
    }

    return 0;
}
