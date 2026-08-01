#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdbool.h>

#include <SDL2/SDL.h>
#include <SDL2/SDL_image.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/channel_layout.h>

#define WINDOW_MIN_DIM 600
#define WINDOW_MAX_DIM 1000

typedef enum {
    MODE_IMAGE,
    MODE_AUDIO,
    MODE_VIDEO,
    MODE_UNSUPPORTED
} app_mode;

typedef struct {
    bool is_playing;
    bool is_looping;
    bool is_eof;
    float current_time;
    float total_time;
    int volume;
} player_state;

typedef struct {
    AVFormatContext *fmt_ctx;
    AVCodecContext *vid_ctx;
    AVCodecContext *aud_ctx;
    struct SwsContext *sws_ctx;
    struct SwrContext *swr_ctx;
    int vid_idx;
    int aud_idx;
    double last_vid_pts;
    double last_aud_pts;
    double fps;
    SDL_AudioDeviceID audio_dev;
    uint8_t *aud_buf;
    unsigned int aud_buf_size;
    SDL_Texture *video_texture;
} media_context;

app_mode detect_file_type(const char *filename) {
    const char *dot = strrchr(filename, '.');
    if (!dot) return MODE_UNSUPPORTED;

    if (strcasecmp(dot, ".jpg") == 0 || 
        strcasecmp(dot, ".jpeg") == 0 || 
        strcasecmp(dot, ".png") == 0 || 
        strcasecmp(dot, ".webp") == 0) {
        return MODE_IMAGE;
    }
    
    if (strcasecmp(dot, ".mp3") == 0 || 
        strcasecmp(dot, ".wav") == 0 || 
        strcasecmp(dot, ".ogg") == 0 || 
        strcasecmp(dot, ".m4a") == 0) {
        return MODE_AUDIO;
    }

    if (strcasecmp(dot, ".gif") == 0) {
        return MODE_VIDEO;
    }

    return MODE_UNSUPPORTED;
}

const char *get_file_name(const char *path) {
    const char *slash = strrchr(path, '/');
    const char *bslash = strrchr(path, '\\');
    const char *name = path;
    if (slash && slash + 1 > name) name = slash + 1;
    if (bslash && bslash + 1 > name) name = bslash + 1;
    return name;
}

void compute_window_size(int src_w, int src_h, int *out_w, int *out_h) {
    if (src_w <= 0 || src_h <= 0) {
        *out_w = WINDOW_MIN_DIM;
        *out_h = WINDOW_MIN_DIM;
        return;
    }

    double scale = 1.0;
    if (src_w > WINDOW_MAX_DIM || src_h > WINDOW_MAX_DIM) {
        double scale_w = (double)WINDOW_MAX_DIM / src_w;
        double scale_h = (double)WINDOW_MAX_DIM / src_h;
        scale = (scale_w < scale_h) ? scale_w : scale_h;
    } else if (src_w < WINDOW_MIN_DIM || src_h < WINDOW_MIN_DIM) {
        double scale_w = (double)WINDOW_MIN_DIM / src_w;
        double scale_h = (double)WINDOW_MIN_DIM / src_h;
        scale = (scale_w > scale_h) ? scale_w : scale_h;
    }

    *out_w = (int)(src_w * scale + 0.5);
    *out_h = (int)(src_h * scale + 0.5);
}

SDL_Rect compute_fit_rect(int src_w, int src_h, int win_w, int win_h) {
    SDL_Rect r = { 0, 0, win_w, win_h };
    if (src_w <= 0 || src_h <= 0) return r;

    float src_aspect = (float)src_w / (float)src_h;
    float win_aspect = (float)win_w / (float)win_h;

    if (src_aspect > win_aspect) {
        r.w = win_w;
        r.h = (int)(win_w / src_aspect);
        r.x = 0;
        r.y = (win_h - r.h) / 2;
    } else {
        r.h = win_h;
        r.w = (int)(win_h * src_aspect);
        r.y = 0;
        r.x = (win_w - r.w) / 2;
    }
    return r;
}

void apply_volume(uint8_t *buf, int bytes, int volume) {
    if (volume == 100) return;
    int16_t *samples = (int16_t *)buf;
    int count = bytes / 2;
    float vol_mult = (float)volume / 100.0f;
    for (int i = 0; i < count; i++) {
        int32_t s = samples[i];
        s = (int32_t)(s * vol_mult);
        if (s > 32767) s = 32767;
        else if (s < -32768) s = -32768;
        samples[i] = (int16_t)s;
    }
}

/* Shared by the restart-at-end (k/space), 10s skip (j/l), and end-of-file
   auto-loop paths - they all used to repeat this same seek+flush+resync
   sequence separately. */
void seek_media(media_context *ctx, player_state *state, float seconds) {
    int64_t target = (int64_t)(seconds * AV_TIME_BASE);
    av_seek_frame(ctx->fmt_ctx, -1, target, AVSEEK_FLAG_BACKWARD);
    if (ctx->vid_ctx) avcodec_flush_buffers(ctx->vid_ctx);
    if (ctx->aud_ctx) avcodec_flush_buffers(ctx->aud_ctx);
    if (ctx->audio_dev) SDL_ClearQueuedAudio(ctx->audio_dev);
    ctx->last_vid_pts = seconds;
    ctx->last_aud_pts = seconds;
    state->current_time = seconds;
    state->is_eof = false;
}

int main(int argc, char *argv[]) {
    if (argc < 2) return 1;

    SDL_SetHint(SDL_HINT_VIDEO_MINIMIZE_ON_FOCUS_LOSS, "0");

    const char *file_path = argv[1];
    app_mode mode = detect_file_type(file_path);

    if (mode == MODE_UNSUPPORTED) {
        printf("Unsupported file format.\n");
        return 1;
    }

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER) < 0) {
        printf("SDL_Init error: %s\n", SDL_GetError());
        return -1;
    }
    IMG_Init(IMG_INIT_JPG | IMG_INIT_PNG | IMG_INIT_WEBP);

    int win_w = WINDOW_MIN_DIM, win_h = WINDOW_MIN_DIM;

    SDL_Surface *preloaded_image = NULL;
    player_state state = { .is_playing = true, .is_looping = false, .is_eof = false, .current_time = 0.0f, .total_time = 0.0f, .volume = 100 };
    media_context ctx = {0};
    ctx.vid_idx = -1;
    ctx.aud_idx = -1;

    if (mode == MODE_IMAGE) {
        preloaded_image = IMG_Load(file_path);
        if (preloaded_image) compute_window_size(preloaded_image->w, preloaded_image->h, &win_w, &win_h);
    } else if (mode == MODE_VIDEO || mode == MODE_AUDIO) {
        if (avformat_open_input(&ctx.fmt_ctx, file_path, NULL, NULL) >= 0) {
            avformat_find_stream_info(ctx.fmt_ctx, NULL);
            if (ctx.fmt_ctx->duration != AV_NOPTS_VALUE && ctx.fmt_ctx->duration > 0) {
                state.total_time = (float)ctx.fmt_ctx->duration / AV_TIME_BASE;
            }

            for (unsigned int i = 0; i < ctx.fmt_ctx->nb_streams; i++) {
                AVCodecParameters *par = ctx.fmt_ctx->streams[i]->codecpar;
                if (par->codec_type == AVMEDIA_TYPE_VIDEO) {
                    if (!(ctx.fmt_ctx->streams[i]->disposition & AV_DISPOSITION_ATTACHED_PIC)) {
                        if (ctx.vid_idx < 0) {
                            ctx.vid_idx = i;
                        }
                    } else if (!preloaded_image) {
                        AVPacket *pic = &ctx.fmt_ctx->streams[i]->attached_pic;
                        SDL_RWops *rw = SDL_RWFromMem(pic->data, pic->size);
                        if (rw) preloaded_image = IMG_Load_RW(rw, 1);
                    }
                } else if (par->codec_type == AVMEDIA_TYPE_AUDIO) {
                    if (ctx.aud_idx < 0) ctx.aud_idx = i;
                }
            }

            if (ctx.aud_idx >= 0) {
                AVCodecParameters *par = ctx.fmt_ctx->streams[ctx.aud_idx]->codecpar;
                const AVCodec *codec = avcodec_find_decoder(par->codec_id);
                ctx.aud_ctx = avcodec_alloc_context3(codec);
                avcodec_parameters_to_context(ctx.aud_ctx, par);
                avcodec_open2(ctx.aud_ctx, codec, NULL);

                ctx.swr_ctx = swr_alloc();
                av_opt_set_chlayout(ctx.swr_ctx, "in_chlayout", &par->ch_layout, 0);
                av_opt_set_int(ctx.swr_ctx, "in_sample_rate", par->sample_rate, 0);
                av_opt_set_sample_fmt(ctx.swr_ctx, "in_sample_fmt", par->format, 0);

                AVChannelLayout out_ch = AV_CHANNEL_LAYOUT_STEREO;
                av_opt_set_chlayout(ctx.swr_ctx, "out_chlayout", &out_ch, 0);
                av_opt_set_int(ctx.swr_ctx, "out_sample_rate", 44100, 0);
                av_opt_set_sample_fmt(ctx.swr_ctx, "out_sample_fmt", AV_SAMPLE_FMT_S16, 0);
                swr_init(ctx.swr_ctx);

                SDL_AudioSpec wanted = {0};
                wanted.freq = 44100;
                wanted.format = AUDIO_S16SYS;
                wanted.channels = 2;
                wanted.samples = 1024;
                ctx.audio_dev = SDL_OpenAudioDevice(NULL, 0, &wanted, NULL, 0);
                if (ctx.audio_dev) SDL_PauseAudioDevice(ctx.audio_dev, 0);
            }

            if (ctx.vid_idx >= 0) {
                AVCodecParameters *par = ctx.fmt_ctx->streams[ctx.vid_idx]->codecpar;
                const AVCodec *codec = avcodec_find_decoder(par->codec_id);
                ctx.vid_ctx = avcodec_alloc_context3(codec);
                avcodec_parameters_to_context(ctx.vid_ctx, par);
                avcodec_open2(ctx.vid_ctx, codec, NULL);
                
                AVRational fps_rational = ctx.fmt_ctx->streams[ctx.vid_idx]->avg_frame_rate;
                ctx.fps = (fps_rational.den && fps_rational.num) ? av_q2d(fps_rational) : 30.0;
                if (ctx.fps <= 0.0) ctx.fps = 30.0;

                compute_window_size(par->width, par->height, &win_w, &win_h);
            } else if (preloaded_image) {
                compute_window_size(preloaded_image->w, preloaded_image->h, &win_w, &win_h);
            }
        }
    }

    char window_title[512];
    snprintf(window_title, sizeof(window_title), "%s", get_file_name(file_path));

    SDL_Window *window = SDL_CreateWindow(window_title,
                                          SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                          win_w, win_h,
                                          SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
    if (!window) return -1;

    SDL_Renderer *renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!renderer) return -1;

    SDL_Texture *media_texture = NULL;
    SDL_Rect video_fit_rect = { 0, 0, win_w, win_h };

    if (preloaded_image) {
        media_texture = SDL_CreateTextureFromSurface(renderer, preloaded_image);
        SDL_FreeSurface(preloaded_image);
    }
    
    if (ctx.vid_ctx) {
        ctx.video_texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STREAMING,
                                              ctx.vid_ctx->width, ctx.vid_ctx->height);
    }

    AVPacket *packet = av_packet_alloc();
    AVFrame *frame = av_frame_alloc();

    float img_zoom = 1.0f;
    float img_offset_x = 0.0f;
    float img_offset_y = 0.0f;
    double img_angle = 0.0;
    bool img_is_dragging = false;
    int tex_w = 0, tex_h = 0;
    float pan_speed = 30.0f;

    if (media_texture) SDL_QueryTexture(media_texture, NULL, NULL, &tex_w, &tex_h);
    if (mode == MODE_IMAGE) {
        img_offset_x = (win_w - tex_w) / 2.0f;
        img_offset_y = (win_h - tex_h) / 2.0f;
    }

    bool running = true;
    SDL_Event event;
    Uint32 last_time = SDL_GetTicks();

    while (running) {
        Uint32 current_ticks = SDL_GetTicks();
        float delta = (current_ticks - last_time) / 1000.0f;
        last_time = current_ticks;

        if (state.is_playing && mode != MODE_IMAGE) {
            state.current_time += delta;
            if (state.total_time > 0.0f && state.current_time > state.total_time) {
                state.current_time = state.total_time;
            }
        }

        SDL_GetWindowSize(window, &win_w, &win_h);
        if (ctx.vid_ctx) {
            video_fit_rect = compute_fit_rect(ctx.vid_ctx->width, ctx.vid_ctx->height, win_w, win_h);
        }

        if (mode == MODE_IMAGE) {
            bool render_needed = false;

            if (SDL_WaitEvent(&event)) {
                if (event.type == SDL_QUIT) running = false;
                else if (event.type == SDL_MOUSEWHEEL) {
                    if (event.wheel.y > 0) img_zoom *= 1.1f;
                    else if (event.wheel.y < 0) img_zoom /= 1.1f;
                    render_needed = true;
                }
                else if (event.type == SDL_MOUSEBUTTONDOWN && event.button.button == SDL_BUTTON_LEFT) img_is_dragging = true;
                else if (event.type == SDL_MOUSEBUTTONUP && event.button.button == SDL_BUTTON_LEFT) img_is_dragging = false;
                else if (event.type == SDL_MOUSEMOTION && img_is_dragging) {
                    img_offset_x += event.motion.xrel;
                    img_offset_y += event.motion.yrel;
                    render_needed = true;
                }
                else if (event.type == SDL_KEYDOWN) {
                    SDL_Keycode sym = event.key.keysym.sym;
                    if (sym == SDLK_r) {
                        img_angle += 90.0;
                        if (img_angle >= 360.0) img_angle = 0.0;
                        render_needed = true;
                    }
                    else if (sym == SDLK_PLUS || sym == SDLK_EQUALS || sym == SDLK_KP_PLUS) { img_zoom *= 1.1f; render_needed = true; }
                    else if (sym == SDLK_MINUS || sym == SDLK_KP_MINUS) { img_zoom /= 1.1f; render_needed = true; }
                    else if (sym == SDLK_w || sym == SDLK_UP) { img_offset_y += pan_speed; render_needed = true; }
                    else if (sym == SDLK_s || sym == SDLK_DOWN) { img_offset_y -= pan_speed; render_needed = true; }
                    else if (sym == SDLK_a || sym == SDLK_LEFT) { img_offset_x += pan_speed; render_needed = true; }
                    else if (sym == SDLK_d || sym == SDLK_RIGHT) { img_offset_x -= pan_speed; render_needed = true; }
                }
                else if (event.type == SDL_WINDOWEVENT) render_needed = true;
            }

            if (render_needed || img_is_dragging == false) {
                SDL_SetRenderDrawColor(renderer, 30, 30, 30, 255);
                SDL_RenderClear(renderer);
                if (media_texture) {
                    SDL_Rect dest_rect = { (int)img_offset_x, (int)img_offset_y, (int)(tex_w * img_zoom), (int)(tex_h * img_zoom) };
                    SDL_RenderCopyEx(renderer, media_texture, NULL, &dest_rect, img_angle, NULL, SDL_FLIP_NONE);
                }
                SDL_RenderPresent(renderer);
            }
        }
        else { 
            while (SDL_PollEvent(&event)) {
                if (event.type == SDL_QUIT) running = false;
                if (event.type == SDL_KEYDOWN) {
                    SDL_Keycode sym = event.key.keysym.sym;
                    if (sym == SDLK_k || sym == SDLK_SPACE) {
                        if (state.is_eof || (state.total_time > 0.0f && state.current_time >= state.total_time)) {
                            seek_media(&ctx, &state, 0.0f);
                            state.is_playing = true;
                            if (ctx.audio_dev) SDL_PauseAudioDevice(ctx.audio_dev, 0);
                        } else {
                            state.is_playing = !state.is_playing;
                            if (ctx.audio_dev) SDL_PauseAudioDevice(ctx.audio_dev, state.is_playing ? 0 : 1);
                        }
                    }
                    else if (sym == SDLK_j || sym == SDLK_LEFT || sym == SDLK_l || sym == SDLK_RIGHT) {
                        float target_time = state.current_time + ((sym == SDLK_j || sym == SDLK_LEFT) ? -10.0f : 10.0f);
                        if (target_time < 0) target_time = 0.0f;
                        if (state.total_time > 0.0f && target_time > state.total_time) target_time = state.total_time;
                        seek_media(&ctx, &state, target_time);
                    }
                    else if (sym == SDLK_o) {
                        state.is_looping = !state.is_looping;
                    }
                    else if (sym == SDLK_PLUS || sym == SDLK_EQUALS || sym == SDLK_KP_PLUS || sym == SDLK_UP) {
                        state.volume += 10;
                        if (state.volume > 100) state.volume = 100;
                    }
                    else if (sym == SDLK_MINUS || sym == SDLK_KP_MINUS || sym == SDLK_DOWN) {
                        state.volume -= 10;
                        if (state.volume < 0) state.volume = 0;
                    }
                }
            }

            if (state.is_playing && ctx.fmt_ctx) {
                bool eof = false;
                while (!eof) {
                    bool need_video = (ctx.vid_idx >= 0) && (ctx.last_vid_pts < state.current_time);
                    bool need_audio = (ctx.aud_idx >= 0) && (SDL_GetQueuedAudioSize(ctx.audio_dev) < 16384);

                    if (!need_video && !need_audio) break;

                    int ret = av_read_frame(ctx.fmt_ctx, packet);
                    if (ret < 0) { eof = true; break; }

                    if (packet->stream_index == ctx.vid_idx) {
                        if (avcodec_send_packet(ctx.vid_ctx, packet) == 0) {
                            while (avcodec_receive_frame(ctx.vid_ctx, frame) == 0) {
                                int64_t pts = frame->best_effort_timestamp;
                                if (pts == AV_NOPTS_VALUE) pts = frame->pts;
                                if (pts != AV_NOPTS_VALUE) {
                                    ctx.last_vid_pts = pts * av_q2d(ctx.fmt_ctx->streams[ctx.vid_idx]->time_base);
                                } else {
                                    ctx.last_vid_pts += (ctx.fps > 0 ? 1.0 / ctx.fps : 0.033);
                                }
                                
                                void *pixels; int pitch;
                                if (ctx.video_texture && SDL_LockTexture(ctx.video_texture, NULL, &pixels, &pitch) == 0) {
                                    ctx.sws_ctx = sws_getCachedContext(ctx.sws_ctx,
                                        frame->width, frame->height, (enum AVPixelFormat)frame->format,
                                        frame->width, frame->height, AV_PIX_FMT_RGBA,
                                        SWS_BILINEAR, NULL, NULL, NULL);

                                    if (ctx.sws_ctx) {
                                        uint8_t *dst_data[4] = { (uint8_t *)pixels, NULL, NULL, NULL };
                                        int dst_linesize[4] = { pitch, 0, 0, 0 };
                                        sws_scale(ctx.sws_ctx, (const uint8_t *const *)frame->data, frame->linesize,
                                                  0, frame->height, dst_data, dst_linesize);
                                    }
                                    SDL_UnlockTexture(ctx.video_texture);
                                }
                            }
                        }
                    } else if (packet->stream_index == ctx.aud_idx) {
                        if (avcodec_send_packet(ctx.aud_ctx, packet) == 0) {
                            while (avcodec_receive_frame(ctx.aud_ctx, frame) == 0) {
                                int64_t pts = frame->best_effort_timestamp;
                                if (pts != AV_NOPTS_VALUE) {
                                    ctx.last_aud_pts = pts * av_q2d(ctx.fmt_ctx->streams[ctx.aud_idx]->time_base);
                                }

                                int out_samples = swr_get_out_samples(ctx.swr_ctx, frame->nb_samples);
                                unsigned int required_size = out_samples * 2 * 2;
                                if (ctx.aud_buf_size < required_size) {
                                    ctx.aud_buf = realloc(ctx.aud_buf, required_size);
                                    ctx.aud_buf_size = required_size;
                                }

                                uint8_t *out_data[1] = { ctx.aud_buf };
                                int converted = swr_convert(ctx.swr_ctx, out_data, out_samples, (const uint8_t **)frame->data, frame->nb_samples);
                                if (converted > 0) {
                                    int bytes = converted * 4;
                                    apply_volume(ctx.aud_buf, bytes, state.volume);
                                    SDL_QueueAudio(ctx.audio_dev, ctx.aud_buf, bytes);
                                }
                            }
                        }
                    }
                    av_packet_unref(packet);
                }

                if (eof && (!ctx.audio_dev || SDL_GetQueuedAudioSize(ctx.audio_dev) == 0)) {
                    if (state.is_looping) {
                        seek_media(&ctx, &state, 0.0f);
                    } else {
                        state.is_playing = false;
                        state.is_eof = true;
                        if (ctx.audio_dev) SDL_PauseAudioDevice(ctx.audio_dev, 1);
                    }
                }
            }

            SDL_SetRenderDrawColor(renderer, 20, 20, 20, 255);
            SDL_RenderClear(renderer);

            if (mode == MODE_AUDIO) {
                if (media_texture) {
                    SDL_Rect fit_rect = compute_fit_rect(tex_w, tex_h, win_w, win_h);
                    SDL_RenderCopy(renderer, media_texture, NULL, &fit_rect);
                } else {
                    SDL_Rect fallback_rect = { (win_w - WINDOW_MIN_DIM) / 2, (win_h - WINDOW_MIN_DIM) / 2, WINDOW_MIN_DIM, WINDOW_MIN_DIM };
                    SDL_SetRenderDrawColor(renderer, 40, 40, 40, 255);
                    SDL_RenderFillRect(renderer, &fallback_rect);
                    SDL_SetRenderDrawColor(renderer, 100, 50, 50, 255);
                    SDL_RenderDrawLine(renderer, fallback_rect.x, fallback_rect.y, fallback_rect.x + fallback_rect.w, fallback_rect.y + fallback_rect.h);
                    SDL_RenderDrawLine(renderer, fallback_rect.x + fallback_rect.w, fallback_rect.y, fallback_rect.x, fallback_rect.y + fallback_rect.h);
                }
            } else if (mode == MODE_VIDEO && ctx.video_texture) {
                SDL_RenderCopy(renderer, ctx.video_texture, NULL, &video_fit_rect);
            }

            int bar_height = 8;
            float progress = state.total_time > 0.0f ? state.current_time / state.total_time : 0.0f;
            if (progress > 1.0f) progress = 1.0f;
            if (progress < 0.0f) progress = 0.0f;

            SDL_Rect fg_bar = { 0, win_h - bar_height, (int)(win_w * progress), bar_height };
            SDL_SetRenderDrawColor(renderer, 0, 150, 255, 255);
            SDL_RenderFillRect(renderer, &fg_bar);

            Uint32 window_flags = SDL_GetWindowFlags(window);
            if (window_flags & (SDL_WINDOW_MINIMIZED | SDL_WINDOW_HIDDEN)) {
                SDL_Delay(10); 
            } else {
                SDL_RenderPresent(renderer);
            }
        }
    }

    if (frame) av_frame_free(&frame);
    if (packet) av_packet_free(&packet);
    if (ctx.aud_buf) free(ctx.aud_buf);
    if (ctx.audio_dev) SDL_CloseAudioDevice(ctx.audio_dev);
    if (ctx.swr_ctx) swr_free(&ctx.swr_ctx);
    if (ctx.sws_ctx) sws_freeContext(ctx.sws_ctx);
    if (ctx.vid_ctx) avcodec_free_context(&ctx.vid_ctx);
    if (ctx.aud_ctx) avcodec_free_context(&ctx.aud_ctx);
    if (ctx.fmt_ctx) avformat_close_input(&ctx.fmt_ctx);
    if (ctx.video_texture) SDL_DestroyTexture(ctx.video_texture);
    if (media_texture) SDL_DestroyTexture(media_texture);

    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    IMG_Quit();
    SDL_Quit();
    return 0;
}