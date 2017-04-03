/*
 * Copyright (c) 2013 Andrew Kelley
 *
 * This file is part of libgroove, which is MIT licensed.
 * See http://opensource.org/licenses/MIT
 */

#ifndef GROOVE_FILE_H
#define GROOVE_FILE_H

#include "groove_internal.h"
#include "atomics.h"

#include <pthread.h>

#include <libavformat/avformat.h>

struct GrooveFilePrivate {
    struct GrooveFile externals;
    struct Groove *groove;

    // decoding
    AVFormatContext *ctx;
    AVCodec *decoder;
    AVCodecContext *decoder_ctx;
    int stream_idx;
    AVStream *stream;
    AVPacket pkt;
    double decode_pos; // position of the decode head
    struct GrooveAtomicBool abort_request; // true when we're closing the file

    // saving
    AVFormatContext *sav_ctx;
    int tempfile_exists;

    // io
    FILE *stdfile;
    AVIOContext *avio;
    unsigned char *avio_buf;
    struct GrooveCustomIo *custom_io;
    struct GrooveCustomIo prealloc_custom_io;

    // seeking
    pthread_mutex_t seek_mutex; // this mutex protects the fields in this block
    int64_t seek_pos; // -1 if no seek request
    int seek_flush; // whether the seek request wants us to flush the buffer
    bool ever_seeked;
    int eof;
    int paused;
};

#endif
