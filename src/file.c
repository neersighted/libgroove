/*
 * Copyright (c) 2013 Andrew Kelley
 *
 * This file is part of libgroove, which is MIT licensed.
 * See http://opensource.org/licenses/MIT
 */

#include "file.h"
#include "util.h"
#include "groove_private.h"

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/unistd.h>

static int decode_interrupt_cb(void *ctx) {
    struct GrooveFilePrivate *f = (struct GrooveFilePrivate *)ctx;
    return f ? GROOVE_ATOMIC_LOAD(f->abort_request) : 0;
}

static int avio_read_packet_callback(void *opaque, uint8_t *buf, int buf_size) {
    struct GrooveFilePrivate *f = (struct GrooveFilePrivate *)opaque;
    return f->custom_io->read_packet(f->custom_io, buf, buf_size);
}

static int avio_write_packet_callback(void *opaque, uint8_t *buf, int buf_size) {
    struct GrooveFilePrivate *f = (struct GrooveFilePrivate *)opaque;
    return f->custom_io->write_packet(f->custom_io, buf, buf_size);
}

static int64_t avio_seek_callback(void *opaque, int64_t offset, int whence) {
    struct GrooveFilePrivate *f = (struct GrooveFilePrivate *)opaque;
    return f->custom_io->seek(f->custom_io, offset, whence);
}

static int file_read_packet(struct GrooveCustomIo *custom_io, uint8_t *buf, int buf_size) {
    struct GrooveFilePrivate *f = (struct GrooveFilePrivate *)custom_io->userdata;
    return fread(buf, 1, buf_size, f->stdfile);
}

static int file_write_packet(struct GrooveCustomIo *custom_io, uint8_t *buf, int buf_size) {
    struct GrooveFilePrivate *f = (struct GrooveFilePrivate *)custom_io->userdata;
    return fwrite(buf, 1, buf_size, f->stdfile);
}

static int64_t file_seek(struct GrooveCustomIo *custom_io, int64_t offset, int whence) {
    struct GrooveFilePrivate *f = (struct GrooveFilePrivate *)custom_io->userdata;

    if (whence & GROOVE_SEEK_FORCE) {
        // doesn't matter
        whence -= GROOVE_SEEK_FORCE;
    }

    if (whence & GROOVE_SEEK_SIZE) {
        int err;
        struct stat st;
        if ((err = fstat(fileno(f->stdfile), &st))) {
            return err;
        }
        return st.st_size;
    }

    switch (whence) {
        case SEEK_SET:
        case SEEK_CUR:
        case SEEK_END:
            return fseek(f->stdfile, offset, whence);
    }
    return -1;
}

static void init_file_state(struct GrooveFilePrivate *f) {
    struct Groove *groove = f->groove;
    memset(f, 0, sizeof(struct GrooveFilePrivate));
    f->groove = groove;
    f->stream_idx = -1;
    f->seek_pos = -1;
    GROOVE_ATOMIC_STORE(f->abort_request, false);
}

struct GrooveFile *groove_file_create(struct Groove *groove) {
    struct GrooveFilePrivate *f = ALLOCATE_NONZERO(struct GrooveFilePrivate, 1);
    if (!f)
        return NULL;

    init_file_state(f);

    return &f->externals;
}

int groove_file_open_custom(struct GrooveFile *file, struct GrooveCustomIo *custom_io,
        const char *filename_hint)
{
    struct GrooveFilePrivate *f = (struct GrooveFilePrivate *) file;

    f->custom_io = custom_io;

    if (pthread_mutex_init(&f->seek_mutex, NULL)) {
        groove_file_close(file);
        return GrooveErrorSystemResources;
    }

    f->ctx = avformat_alloc_context();
    if (!f->ctx) {
        groove_file_close(file);
        return GrooveErrorNoMem;
    }
    file->filename = f->ctx->filename;
    f->ctx->interrupt_callback.callback = decode_interrupt_cb;
    f->ctx->interrupt_callback.opaque = f;

    const int buffer_size = 8 * 1024;
    f->avio_buf = ALLOCATE_NONZERO(unsigned char, buffer_size);
    if (!f->avio_buf) {
        groove_file_close(file);
        return GrooveErrorNoMem;
    }

    f->avio = avio_alloc_context(f->avio_buf, buffer_size, 0, f,
            avio_read_packet_callback, avio_write_packet_callback, avio_seek_callback);
    if (!f->avio) {
        groove_file_close(file);
        return GrooveErrorNoMem;
    }
    f->avio->seekable = AVIO_SEEKABLE_NORMAL;
    f->avio->direct = AVIO_FLAG_DIRECT;

    f->ctx->pb = f->avio;
    int err = avformat_open_input(&f->ctx, filename_hint, NULL, NULL);
    if (err < 0) {
        assert(err != AVERROR(EINVAL));
        groove_file_close(file);
        if (err == AVERROR(ENOMEM)) {
            return GrooveErrorNoMem;
        } else if (err == AVERROR(ENOENT)) {
            return GrooveErrorFileNotFound;
        } else if (err == AVERROR(EPERM)) {
            return GrooveErrorPermissions;
        } else {
            return GrooveErrorUnknownFormat;
        }
    }

    err = avformat_find_stream_info(f->ctx, NULL);
    if (err < 0) {
        groove_file_close(file);
        return GrooveErrorStreamNotFound;
    }

    // set all streams to discard. in a few lines here we will find the audio
    // stream and cancel discarding it
    if (f->ctx->nb_streams > INT_MAX) {
        groove_file_close(file);
        return GrooveErrorTooManyStreams;
    }

    int stream_count = (int)f->ctx->nb_streams;
    for (int i = 0; i < stream_count; i++)
        f->ctx->streams[i]->discard = AVDISCARD_ALL;

    f->stream_idx = av_find_best_stream(f->ctx, AVMEDIA_TYPE_AUDIO, -1, -1, &f->decoder, 0);

    if (f->stream_idx < 0) {
        groove_file_close(file);
        return GrooveErrorStreamNotFound;
    }

    if (!f->decoder) {
        groove_file_close(file);
        return GrooveErrorDecoderNotFound;
    }

    f->stream = f->ctx->streams[f->stream_idx];
    f->stream->discard = AVDISCARD_DEFAULT;

    f->decoder_ctx = avcodec_alloc_context3(f->decoder);
    if (!f->decoder_ctx) {
        groove_file_close(file);
        return GrooveErrorNoMem;
    }

    if (avcodec_parameters_to_context(f->decoder_ctx, f->stream->codecpar) < 0) {
        groove_file_close(file);
        return GrooveErrorDecoding;
    }

    if (!f->decoder_ctx->channel_layout)
        f->decoder_ctx->channel_layout = av_get_default_channel_layout(f->decoder_ctx->channels);
    if (!f->decoder_ctx->channel_layout) {
        groove_file_close(file);
        return GrooveErrorInvalidChannelLayout;
    }

    if (avcodec_open2(f->decoder_ctx, f->decoder, NULL) < 0) {
        groove_file_close(file);
        return GrooveErrorDecoding;
    }

    return 0;
}

int groove_file_open(struct GrooveFile *file,
        const char *filename, const char *filename_hint)
{
    struct GrooveFilePrivate *f = (struct GrooveFilePrivate *) file;

    f->stdfile = fopen(filename, "rb");
    if (!f->stdfile) {
        int err = errno;
        assert(err != EINVAL);
        groove_file_close(file);
        if (err == ENOMEM) {
            return GrooveErrorNoMem;
        } else if (err == ENOENT) {
            return GrooveErrorFileNotFound;
        } else if (err == EPERM) {
            return GrooveErrorPermissions;
        } else {
            return GrooveErrorFileSystem;
        }
    }

    f->prealloc_custom_io.userdata = f;
    f->prealloc_custom_io.read_packet = file_read_packet;
    f->prealloc_custom_io.write_packet = file_write_packet;
    f->prealloc_custom_io.seek = file_seek;

    return groove_file_open_custom(file, &f->prealloc_custom_io, filename_hint);
}

// should be safe to call no matter what state the file is in
void groove_file_close(struct GrooveFile *file) {
    if (!file)
        return;

    struct GrooveFilePrivate *f = (struct GrooveFilePrivate *)file;

    GROOVE_ATOMIC_STORE(f->abort_request, true);

    av_packet_unref(&f->pkt);

    if (f->stream) {
        f->stream_idx = -1;
        f->stream = NULL;
    }

    // disable interrupting
    GROOVE_ATOMIC_STORE(f->abort_request, false);

    if (f->ctx)
        avformat_close_input(&f->ctx);

    if (f->decoder_ctx)
        avcodec_free_context(&f->decoder_ctx);

    if (f->stdfile)
        fclose(f->stdfile);

    if (f->avio)
        av_free(f->avio);

    pthread_mutex_destroy(&f->seek_mutex);

    init_file_state(f);
}

void groove_file_destroy(struct GrooveFile *file) {
    struct GrooveFilePrivate *f = (struct GrooveFilePrivate *)file;

    if (!file)
        return;

    groove_file_close(file);

    DEALLOCATE(f);
}


const char *groove_file_short_names(struct GrooveFile *file) {
    struct GrooveFilePrivate *f = (struct GrooveFilePrivate *) file;
    return f->ctx->iformat->name;
}

double groove_file_duration(struct GrooveFile *file) {
    struct GrooveFilePrivate *f = (struct GrooveFilePrivate *) file;
    double time_base = av_q2d(f->stream->time_base);
    return time_base * f->stream->duration;
}

void groove_file_audio_format(struct GrooveFile *file, struct GrooveAudioFormat *audio_format) {
    struct GrooveFilePrivate *f = (struct GrooveFilePrivate *) file;

    AVCodecParameters *codecpar = f->stream->codecpar;
    audio_format->sample_rate = codecpar->sample_rate;
    from_ffmpeg_layout(codecpar->channel_layout, &audio_format->layout);
    audio_format->format = from_ffmpeg_format(codecpar->format);
    audio_format->is_planar = from_ffmpeg_format_planar(codecpar->format);
}

struct GrooveTag *groove_file_metadata_get(struct GrooveFile *file, const char *key,
        const struct GrooveTag *prev, int flags)
{
    struct GrooveFilePrivate *f = (struct GrooveFilePrivate *) file;
    const AVDictionaryEntry *e = (const AVDictionaryEntry *) prev;
    if (key && key[0] == 0)
        flags |= AV_DICT_IGNORE_SUFFIX;
    return (struct GrooveTag *) av_dict_get(f->ctx->metadata, key, e, flags);
}

int groove_file_metadata_set(struct GrooveFile *file, const char *key,
        const char *value, int flags)
{
    file->dirty = 1;
    struct GrooveFilePrivate *f = (struct GrooveFilePrivate *) file;
    return av_dict_set(&f->ctx->metadata, key, value, flags);
}

const char *groove_tag_key(struct GrooveTag *tag) {
    AVDictionaryEntry *e = (AVDictionaryEntry *) tag;
    return e->key;
}

const char *groove_tag_value(struct GrooveTag *tag) {
    AVDictionaryEntry *e = (AVDictionaryEntry *) tag;
    return e->value;
}

static void cleanup_save(struct GrooveFile *file) {
    struct GrooveFilePrivate *f = (struct GrooveFilePrivate *) file;

    av_packet_unref(&f->pkt);
    if (f->tempfile_exists) {
        remove(f->sav_ctx->filename);
        f->tempfile_exists = 0;
    }
    if (f->sav_ctx) {
        avio_closep(&f->sav_ctx->pb);
        avformat_free_context(f->sav_ctx);
        f->sav_ctx = NULL;
    }
}

int groove_file_save_as(struct GrooveFile *file, const char *filename) {
    struct GrooveFilePrivate *f = (struct GrooveFilePrivate *) file;

    // detect output format
    AVOutputFormat *oformat = av_guess_format(f->ctx->iformat->name, f->ctx->filename, NULL);
    if (!oformat) {
        return GrooveErrorUnknownFormat;
    }

    // allocate output media context
    f->sav_ctx = avformat_alloc_context();
    if (!f->sav_ctx) {
        cleanup_save(file);
        return GrooveErrorNoMem;
    }

    strcpy(f->sav_ctx->filename, filename);
    f->sav_ctx->oformat = oformat;
    f->sav_ctx->flags |= AVFMT_FLAG_BITEXACT;

    // open output file if needed
    if (!(oformat->flags & AVFMT_NOFILE)) {
        if (avio_open(&f->sav_ctx->pb, f->sav_ctx->filename, AVIO_FLAG_WRITE) < 0) {
            cleanup_save(file);
            return GrooveErrorFileSystem;
        }
        f->tempfile_exists = 1;
    }

    if (f->ctx->nb_streams > INT_MAX) {
        cleanup_save(file);
        return GrooveErrorTooManyStreams;
    }

    // add all the streams
    int stream_count = (int)f->ctx->nb_streams;
    for (int i = 0; i < stream_count; i++) {
        AVStream *in_stream = f->ctx->streams[i];
        if (in_stream->codecpar->codec_type != AVMEDIA_TYPE_AUDIO)
            continue;

        AVStream *out_stream = avformat_new_stream(f->sav_ctx, NULL);
        if (!out_stream) {
            cleanup_save(file);
            return GrooveErrorNoMem;
        }

        out_stream->id = in_stream->id;
        out_stream->disposition = in_stream->disposition;
        out_stream->time_base = in_stream->time_base;

        // copy parameters
        if (avcodec_parameters_copy(out_stream->codecpar, in_stream->codecpar) < 0) {
            cleanup_save(file);
            return GrooveErrorEncoding;
        }
    }

    // set metadata
    av_dict_copy(&f->sav_ctx->metadata, f->ctx->metadata, 0);

    if (avformat_write_header(f->sav_ctx, NULL) < 0) {
        cleanup_save(file);
        return GrooveErrorEncoding;
    }

    AVPacket *pkt = &f->pkt;
    for (;;) {
        int err = av_read_frame(f->ctx, pkt);
        if (err == AVERROR_EOF) {
            break;
        } else if (err < 0) {
            cleanup_save(file);
            return GrooveErrorDecoding;
        }

        AVStream *in_stream = f->ctx->streams[pkt->stream_index];
        if (in_stream->codecpar->codec_type != AVMEDIA_TYPE_AUDIO)
            continue;

        if (av_write_frame(f->sav_ctx, pkt) < 0) {
            cleanup_save(file);
            return GrooveErrorEncoding;
        }
        av_packet_unref(pkt);
    }

    if (av_write_trailer(f->sav_ctx) < 0) {
        cleanup_save(file);
        return GrooveErrorEncoding;
    }

    f->tempfile_exists = 0;
    cleanup_save(file);

    return 0;
}

int groove_file_save(struct GrooveFile *file) {
    if (!file->dirty)
        return GrooveErrorNoChanges;

    struct GrooveFilePrivate *f = (struct GrooveFilePrivate *) file;

    int temp_filename_len;
    char *temp_filename = groove_create_rand_name(f->groove,
            &temp_filename_len, f->ctx->filename, strlen(f->ctx->filename));

    if (!temp_filename) {
        cleanup_save(file);
        return GrooveErrorNoMem;
    }

    int err;
    if ((err = groove_file_save_as(file, temp_filename))) {
        cleanup_save(file);
        return err;
    }

    if (rename(temp_filename, f->ctx->filename)) {
        f->tempfile_exists = 1;
        cleanup_save(file);
        return GrooveErrorFileSystem;
    }

    file->dirty = 0;
    return 0;
}
