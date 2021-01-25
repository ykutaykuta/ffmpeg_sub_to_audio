#ifndef FEATURES_H
#define FEATURES_H

#include <libavformat/avformat.h>   /* AVFormatContext, AVCodecContext, AVSubtitle */
#include <libavfilter/avfilter.h>   /* AVFilterGraph, AVFilterContext */
#include <libavfilter/framequeue.h> /* FFFrameQueue */

typedef struct SubTTSContext
{
    int is_ready;
    AVFilterGraph *graph;                                      /**< filtergraph for subtitle tts */
    FFFrameQueue sub_frame_fifo;                               /**< list of frame info for the first input */
    AVPacket *decoded_pkt;                                     /**< packet for processing */
    int sample_offset_s;                                       /**< offset of current sample in the first frame of queue */
    int sample_offset_a;                                       /**< offset of current sample in the mixing audio frame */
    int sample_rate;                                           /**< sample rate for config */
    char *channel_layout;                                      /**< channel layout for config */
    char *sample_fmt;                                          /**< sample format for config */
    void (*fc_mix)(struct SubTTSContext *ctx, AVFrame *frame); /**< function mix audio */
} SubTTSContext;

int open_input(AVFormatContext **fmt_ctx, AVCodecContext **dec_ctx, int *audio_stream_index, const char *filename);

int tts_init(SubTTSContext *ctx);
void tts_setup(SubTTSContext *ctx, AVCodecContext *prefer_codec);
void tts_cleanup(SubTTSContext *ctx);

int tts_config_filtercontext(AVFilterContext *filter_ctx, AVCodecContext *dec_ctx);

AVFrame *fc_create_silent_frame(int sample_rate, int format, uint64_t channel_layout);

int subtitle_to_audio(AVSubtitle *sub, SubTTSContext *ctx, char *out_uri);

#endif /* FEATURES_H */
