#include "features.h"

#include <netinet/in.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <unistd.h>

#include <libavutil/log.h>
#include <libavutil/avstring.h>
#include <libavutil/opt.h>
#include <libavfilter/internal.h>

#define MAX_HTTP_RESPONS_SIZE 1024

static const int portno = 8000;
static const char *host_addr = "192.168.3.221";
static const char *api_push_text = "/api/gen_audio";
static const char *api_get_audio = "/api/get_audio/";
static const char *request_fmt = "POST %s HTTP/1.1\r\n%s\r\n\r\n%s";

int open_input(AVFormatContext **fmt_ctx, AVCodecContext **dec_ctx, int *audio_stream_index, const char *filename)
{
    int ret;
    AVCodec *dec;

    if ((ret = avformat_open_input(fmt_ctx, filename, NULL, NULL)) < 0)
    {
        av_log(NULL, AV_LOG_ERROR, "Cannot open input file\n");
        return ret;
    }

    if ((ret = avformat_find_stream_info(*fmt_ctx, NULL)) < 0)
    {
        av_log(NULL, AV_LOG_ERROR, "Cannot find stream information\n");
        return ret;
    }

    /* select the audio stream */
    ret = av_find_best_stream(*fmt_ctx, AVMEDIA_TYPE_AUDIO, -1, -1, &dec, 0);
    if (ret < 0)
    {
        av_log(NULL, AV_LOG_ERROR, "Cannot find an audio stream in the input file\n");
        return ret;
    }
    *audio_stream_index = ret;

    /* create decoding context */
    *dec_ctx = avcodec_alloc_context3(dec);
    if (*dec_ctx == NULL)
        return AVERROR(ENOMEM);
    avcodec_parameters_to_context(*dec_ctx, (*fmt_ctx)->streams[*audio_stream_index]->codecpar);

    /* init the audio decoder */
    if ((ret = avcodec_open2(*dec_ctx, dec, NULL)) < 0)
    {
        av_log(NULL, AV_LOG_ERROR, "Cannot open audio decoder\n");
        return ret;
    }

    return 0;
}

void tts_cleanup(SubTTSContext *ctx)
{
    avfilter_graph_free(&ctx->graph);
    av_packet_free(&ctx->decoded_pkt);
    ff_framequeue_free(&ctx->sub_frame_fifo);
}

int tts_config_filtercontext(AVFilterContext *filter_ctx, AVCodecContext *dec_ctx)
{
    char args[512];
    if (!dec_ctx->channel_layout)
        dec_ctx->channel_layout = av_get_default_channel_layout(dec_ctx->channels);
    snprintf(args, sizeof(args), "sample_rate=%d:sample_fmt=%s:channel_layout=0x%" PRIx64,
             dec_ctx->sample_rate, av_get_sample_fmt_name(dec_ctx->sample_fmt), dec_ctx->channel_layout);
    return avfilter_init_str(filter_ctx, args);
}

int tts_init(SubTTSContext *ctx)
{

    ctx->graph = avfilter_graph_alloc();
    if (!ctx->graph)
    {
        return AVERROR(ENOMEM);
    }

    ff_framequeue_init(&ctx->sub_frame_fifo, &ctx->graph->internal->frame_queues);
    ctx->decoded_pkt = av_packet_alloc();
    if (!ctx->decoded_pkt)
    {
        return AVERROR(ENOMEM);
    }

    ctx->is_ready = 0;

    return 0;
}

static int tts_api(const char *text, float duration, int sample_rate, char *sample_fmt, char *channel_layout, char *response)
{
    /* first what are we going to send and where are we going to send it? */
    struct sockaddr_in serv_addr;
    int sockfd, bytes, sent, received, total;

    /* fill in the parameters */
    char *body = av_asprintf("{\"text\":\"%s\",\"duration\":\"%f\",\"sample_rate\":\"%d\",\"sample_fmt\":\"%s\",\"channel_layout\":\"%s\"}",
                             text, duration, sample_rate, sample_fmt, channel_layout);
    char port_str[10];
    char text_len_str[10];
    char *header;
    char *message;
    sprintf(port_str, "%d", portno);
    sprintf(text_len_str, "%d", (int)strlen(body));
    header = av_asprintf("Host: %s:%s\r\nContent-Type: application/json\r\nContent-Length: %s",
                         host_addr, port_str, text_len_str);
    message = av_asprintf(request_fmt, api_push_text, header, body);
    av_freep(&header);
    av_freep(&body);

    /* create the socket */
    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0)
    {
        av_log(NULL, AV_LOG_ERROR, "ERROR opening socket\n");
        goto fail;
    }

    /* fill in the structure */
    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(portno);
    serv_addr.sin_addr.s_addr = inet_addr(host_addr);

    /* connect the socket */
    if (connect(sockfd, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0)
    {
        av_log(NULL, AV_LOG_ERROR, "ERROR connecting\n");
        goto fail;
    }

    /* send the request */
    total = strlen(message);
    sent = 0;
    do
    {
        bytes = write(sockfd, message + sent, total - sent);
        if (bytes < 0)
        {
            av_log(NULL, AV_LOG_ERROR, "ERROR writing message to socket\n");
            return AVERROR(EIO);
        }
        if (bytes == 0)
            break;
        sent += bytes;
    } while (sent < total);

    /* receive the response */
    total = MAX_HTTP_RESPONS_SIZE - 1;
    received = 0;
    do
    {
        bytes = read(sockfd, response + received, total - received);
        if (bytes < 0)
        {
            av_log(NULL, AV_LOG_ERROR, "ERROR reading response from socket\n");
            goto fail;
        }
        if (bytes == 0)
            break;
        received += bytes;
        if (response[received - 1] == '}')
            break;
    } while (received < total);

    if (received == total)
    {
        av_log(NULL, AV_LOG_ERROR, "ERROR storing complete response from socket\n");
        goto fail;
    }

    /* close the socket */
    close(sockfd);
fail:
    av_freep(&message);
    return AVERROR(EIO);
}

static char *get_uri_value_from_response(const char *response)
{
    char *ret = NULL;
    ret = strrchr(response, '"');
    if (!ret)
        return NULL;
    *ret = '\0';
    ret = strrchr(response, '"');
    if (!ret)
        return NULL;

    return ++ret;
}

int subtitle_to_audio(AVSubtitle *sub, SubTTSContext *ctx, char *out_uri)
{
    int ret = 0;
    int i;
    float duration;
    char *p, *ass, *response_api;

    if (sub == NULL)
        return AVERROR(ENOENT);

    duration = (float)(sub->end_display_time - sub->start_display_time) / 1000.0f; // in seconds unit
    ass = sub->rects[0]->ass;
    response_api = av_malloc(MAX_HTTP_RESPONS_SIZE);
    memset(response_api, 0, MAX_HTTP_RESPONS_SIZE);
    p = strchr(sub->rects[0]->ass, ',');
    p++;
    for (i = 0; i < 7; i++)
    {
        if (!p)
        {
            ret = AVERROR(ENOEXEC);
            goto end;
        }
        p = strchr(p, ',');
        p++;
    }
    ass = av_strdup(p);
    tts_api(ass, duration, ctx->sample_rate, ctx->sample_fmt, ctx->channel_layout, response_api);
    p = get_uri_value_from_response(response_api);
    if (p)
    {
        sprintf(out_uri, "http://%s:%d%s%s", host_addr, portno, api_get_audio, p);
    }
    else
    {
        ret = AVERROR(ENOENT);
    }

end:
    av_freep(&response_api);
    av_freep(&ass);
    return ret;
}

AVFrame *fc_create_silent_frame(int sample_rate, int format, uint64_t channel_layout)
{
    AVFrame *silent = NULL;
    silent = av_frame_alloc();
    if (!silent)
    {
        return NULL;
    }
    silent->format = format;
    silent->sample_rate = sample_rate;
    silent->channel_layout = channel_layout;
    silent->channels = av_get_channel_layout_nb_channels(channel_layout);
    silent->nb_samples = 1024;
    if (av_frame_get_buffer(silent, 0) < 0)
    {
        av_frame_free(&silent);
        return NULL;
    }
    av_samples_set_silence(silent->data, 0, silent->nb_samples,
                           silent->channels, silent->format);
    return silent;
}

#define MIX_AUDIO(name, type, type_up, min, max)                                                         \
    static void fc_mix_frame_##name(SubTTSContext *ctx, AVFrame *frame)                                  \
    {                                                                                                    \
        int mix_sample, i;                                                                               \
        AVFrame *sub_frame;                                                                              \
        type *audio, *sub;                                                                               \
        type_up sum;                                                                                     \
        sub_frame = ff_framequeue_peek(&ctx->sub_frame_fifo, 0);                                         \
        if ((frame->nb_samples - ctx->sample_offset_a) < (sub_frame->nb_samples - ctx->sample_offset_s)) \
        {                                                                                                \
            mix_sample = frame->nb_samples - ctx->sample_offset_a;                                       \
        }                                                                                                \
        else                                                                                             \
        {                                                                                                \
            mix_sample = sub_frame->nb_samples - ctx->sample_offset_s;                                   \
        }                                                                                                \
        audio = (type *)frame->data[frame->channels * ctx->sample_offset_a];                             \
        sub = (type *)sub_frame->data[sub_frame->channels * ctx->sample_offset_s];                       \
        for (i = 0; i < mix_sample; i++, audio++, sub++)                                                 \
        {                                                                                                \
            sum = (type_up)*audio + (type_up)*sub;                                                       \
            if (sum < min)                                                                               \
                sum = min;                                                                               \
            if (sum > max)                                                                               \
                sum = max;                                                                               \
            *audio = (type)sum;                                                                          \
        }                                                                                                \
        ctx->sample_offset_a += mix_sample;                                                              \
        ctx->sample_offset_s += mix_sample;                                                              \
        if (ctx->sample_offset_a == frame->nb_samples)                                                   \
        {                                                                                                \
            ctx->sample_offset_a = 0;                                                                    \
        }                                                                                                \
        if (ctx->sample_offset_s == sub_frame->nb_samples)                                               \
        {                                                                                                \
            sub_frame = ff_framequeue_take(&ctx->sub_frame_fifo);                                        \
            av_freep(&sub_frame);                                                                        \
            ctx->sample_offset_s = 0;                                                                    \
        }                                                                                                \
    }

MIX_AUDIO(u8, uint8_t, uint16_t, 0, UINT8_MAX)
MIX_AUDIO(s16, int16_t, int32_t, INT16_MIN, INT16_MAX)
MIX_AUDIO(s32, int32_t, int64_t, INT32_MIN, INT32_MAX)
MIX_AUDIO(flt, float, float, -1.0, 1.0)
MIX_AUDIO(dbl, double, double, -1.0, 1.0)

#define MIX_AUDIO_P(name, type, type_up, min, max)                                                       \
    static void fc_mix_frame_##name##p(SubTTSContext *ctx, AVFrame *frame)                               \
    {                                                                                                    \
        int mix_sample, chan, i, channels, gap;                                                          \
        AVFrame *sub_frame;                                                                              \
        type **audio, **sub;                                                                             \
        type_up sum;                                                                                     \
        gap = sizeof(type);                                                                              \
        channels = frame->channels;                                                                      \
        sub_frame = ff_framequeue_peek(&ctx->sub_frame_fifo, 0);                                         \
        audio = (type **)malloc(channels * sizeof(type *));                                              \
        sub = (type **)malloc(channels * sizeof(type *));                                                \
        for (chan = 0; chan < channels; chan++)                                                          \
        {                                                                                                \
            audio[chan] = (type *)&frame->extended_data[chan][gap * ctx->sample_offset_a];               \
            sub[chan] = (type *)&sub_frame->extended_data[chan][gap * ctx->sample_offset_s];             \
        }                                                                                                \
        if ((frame->nb_samples - ctx->sample_offset_a) < (sub_frame->nb_samples - ctx->sample_offset_s)) \
        {                                                                                                \
            mix_sample = frame->nb_samples - ctx->sample_offset_a;                                       \
        }                                                                                                \
        else                                                                                             \
        {                                                                                                \
            mix_sample = sub_frame->nb_samples - ctx->sample_offset_s;                                   \
        }                                                                                                \
        for (i = 0; i < mix_sample; i++)                                                                 \
        {                                                                                                \
            for (chan = 0; chan < channels; chan++)                                                      \
            {                                                                                            \
                sum = (type_up)*audio[chan] + (type_up)*sub[chan];                                       \
                if (sum < min)                                                                           \
                    sum = min;                                                                           \
                if (sum > max)                                                                           \
                    sum = max;                                                                           \
                *audio[chan] = (type)sum;                                                                \
                audio[chan]++;                                                                           \
                sub[chan]++;                                                                             \
            }                                                                                            \
        }                                                                                                \
        ctx->sample_offset_a += mix_sample;                                                              \
        ctx->sample_offset_s += mix_sample;                                                              \
        if (ctx->sample_offset_a = frame->nb_samples)                                                    \
        {                                                                                                \
            ctx->sample_offset_a = 0;                                                                    \
        }                                                                                                \
        if (ctx->sample_offset_s = sub_frame->nb_samples)                                                \
        {                                                                                                \
            sub_frame = ff_framequeue_take(&ctx->sub_frame_fifo);                                        \
            av_freep(&sub_frame);                                                                        \
            ctx->sample_offset_s = 0;                                                                    \
        }                                                                                                \
        free(audio);                                                                                     \
        free(sub);                                                                                       \
    }

MIX_AUDIO_P(u8, uint8_t, uint16_t, 0, UINT8_MAX)
MIX_AUDIO_P(s16, int16_t, int32_t, INT16_MIN, INT16_MAX)
MIX_AUDIO_P(s32, int32_t, int64_t, INT32_MIN, INT32_MAX)
MIX_AUDIO_P(flt, float, float, -1.0, 1.0)
MIX_AUDIO_P(dbl, double, double, -1.0, 1.0)

void tts_setup(SubTTSContext *ctx, AVCodecContext *prefer_codec)
{
    ctx->sample_rate = prefer_codec->sample_rate;
    ctx->sample_fmt = av_get_sample_fmt_name(prefer_codec->sample_fmt);
    ctx->channel_layout = av_get_channel_layout_name(prefer_codec->channel_layout);

    switch (prefer_codec->sample_fmt)
    {
    case AV_SAMPLE_FMT_U8:
        ctx->fc_mix = fc_mix_frame_u8;
        break;
    case AV_SAMPLE_FMT_U8P:
        ctx->fc_mix = fc_mix_frame_u8p;
        break;
    case AV_SAMPLE_FMT_S16:
        ctx->fc_mix = fc_mix_frame_s16;
        break;
    case AV_SAMPLE_FMT_S16P:
        ctx->fc_mix = fc_mix_frame_s16p;
        break;
    case AV_SAMPLE_FMT_S32:
        ctx->fc_mix = fc_mix_frame_s32;
        break;
    case AV_SAMPLE_FMT_S32P:
        ctx->fc_mix = fc_mix_frame_s32p;
        break;
    case AV_SAMPLE_FMT_FLT:
        ctx->fc_mix = fc_mix_frame_flt;
        break;
    case AV_SAMPLE_FMT_FLTP:
        ctx->fc_mix = fc_mix_frame_fltp;
        break;
    case AV_SAMPLE_FMT_DBL:
        ctx->fc_mix = fc_mix_frame_dbl;
        break;
    case AV_SAMPLE_FMT_DBLP:
        ctx->fc_mix = fc_mix_frame_dblp;
        break;
    }

    ctx->is_ready = 1;
}
