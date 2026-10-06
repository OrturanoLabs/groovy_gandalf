/*
 * main.cpp
 * Audio-reactive video loop player for Gandalf Sax using BTrack.
 *
 * Copyright (C) 2026 Orturano Labs
 * License: GPL-3.0-or-later
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <time.h>
#include <math.h>
#include <atomic>

#include <SDL2/SDL.h>

extern "C" {
    #include <libavformat/avformat.h>
    #include <libswscale/swscale.h>
    #include <libavcodec/avcodec.h>
    #include <libavutil/imgutils.h>
}

#include "BTrack.h"

// ==== CONFIGURAZIONE PARAMETRI ====================================================

#define FRAMES_PER_BEAT 12.0   // Numero di frame video che compongono 1 battuta/beat
#define AUDIO_BUFFER_SIZE 512  // Hop size ottimale per BTrack

// Frame del video da far coincidere esattamente con il beat
static const int FRAME_OFFSET = 10;

// ==== STRUTTURE DATI =============================================================

typedef struct {
    uint8_t* pixels;
    int width;
    int height;
    int linesize;
} Frame;

typedef struct {
    Frame* frames;
    int count;
    int width;
    int height;
} VideoBuffer;

// ==== STATO GLOBALE AUDIO & BTRACK ================================================

static std::atomic<double> shared_target_fps(20.0);
static std::atomic<double> shared_bpm(120.0);

static std::atomic<bool> first_beat_received(false);
static std::atomic<bool> beat_event(false);

static BTrack btrack(AUDIO_BUFFER_SIZE, 1024);
static double audio_frame_double[AUDIO_BUFFER_SIZE];

// ==== GESTIONE TIMING ============================================================

static inline struct timespec now_monotonic(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts;
}

static inline struct timespec add_seconds(struct timespec ts, double seconds) {
    ts.tv_sec += (time_t)seconds;
    ts.tv_nsec += (long)((seconds - (time_t)seconds) * 1e9);

    if (ts.tv_nsec >= 1000000000L) {
        ts.tv_sec++;
        ts.tv_nsec -= 1000000000L;
    }
    return ts;
}

static inline double diff_seconds(struct timespec start, struct timespec end) {
    return (end.tv_sec - start.tv_sec) + (end.tv_nsec - start.tv_nsec) / 1e9;
}

void sleep_until(struct timespec target) {
    clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &target, NULL);
}

// ==== GESTIONE FRAME E MEMORIA ===================================================

Frame create_frame(int width, int height) {
    Frame f;
    f.width = width;
    f.height = height;
    f.linesize = width * 3;
    size_t size = (size_t)f.linesize * height;
    f.pixels = (uint8_t*)malloc(size);
    return f;
}

void destroy_frame(Frame* f) {
    if (f->pixels) {
        free(f->pixels);
        f->pixels = NULL;
    }
}

void destroy_video(VideoBuffer* video) {
    for (int i = 0; i < video->count; i++) {
        destroy_frame(&video->frames[i]);
    }
    free(video->frames);
}

// ==== AUDIO CALLBACK CON BTRACK ==================================================

void audio_capture_callback(void *userdata, Uint8 *stream, int len) {
    int16_t *samples = (int16_t*)stream;
    int num_samples = len / sizeof(int16_t);

    if (num_samples < AUDIO_BUFFER_SIZE) return;

    for (int i = 0; i < AUDIO_BUFFER_SIZE; i++) {
        audio_frame_double[i] = (double)samples[i] / 32768.0;
    }

    // Processa audio con BTrack
    btrack.processAudioFrame(audio_frame_double);

    double calculated_bpm = btrack.getCurrentTempoEstimate();
    bool is_beat = btrack.beatDueInCurrentFrame();

    if (is_beat) {
        first_beat_received.store(true);
        beat_event.store(true);
    }

    if (calculated_bpm >= 60.0 && calculated_bpm <= 200.0) {
        shared_bpm.store(calculated_bpm);

        double new_fps = (calculated_bpm / 60.0) * FRAMES_PER_BEAT;
        shared_target_fps.store(new_fps);
    }

    printf("\r[LIVE BTRACK] BPM: %5.1f | FPS Video: %5.2f %s",
           shared_bpm.load(),
           shared_target_fps.load(),
           is_beat ? " [BEAT!]" : "        ");
    fflush(stdout);
}

// ==== DECODIFICA VIDEO ============================================================

bool load_video(const char* filename, VideoBuffer* video) {
    AVFormatContext* format_ctx = NULL;

    if (avformat_open_input(&format_ctx, filename, NULL, NULL) < 0) {
        printf("\nImpossibile aprire il file video: %s\n", filename);
        return false;
    }

    avformat_find_stream_info(format_ctx, NULL);

    int video_stream = av_find_best_stream(format_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    if (video_stream < 0) {
        printf("\nNessuno stream video trovato.\n");
        return false;
    }

    AVStream* stream = format_ctx->streams[video_stream];
    const AVCodec* codec = avcodec_find_decoder(stream->codecpar->codec_id);
    AVCodecContext* codec_ctx = avcodec_alloc_context3(codec);

    avcodec_parameters_to_context(codec_ctx, stream->codecpar);
    avcodec_open2(codec_ctx, codec, NULL);

    int width = codec_ctx->width;
    int height = codec_ctx->height;
    video->width = width;
    video->height = height;

    int max_frames = 200;
    video->frames = (Frame*)malloc(sizeof(Frame) * max_frames);
    video->count = 0;

    AVFrame* frame = av_frame_alloc();
    AVFrame* rgb_frame = av_frame_alloc();
    AVPacket* packet = av_packet_alloc();

    int rgb_linesize = width * 3;
    uint8_t* rgb_buffer = (uint8_t*)malloc(rgb_linesize * height);

    av_image_fill_arrays(
        rgb_frame->data, rgb_frame->linesize,
        rgb_buffer, AV_PIX_FMT_RGB24,
        width, height, 1
    );

    struct SwsContext* sws = sws_getContext(
        width, height, codec_ctx->pix_fmt,
        width, height, AV_PIX_FMT_RGB24,
        SWS_BILINEAR, NULL, NULL, NULL
    );

    while (av_read_frame(format_ctx, packet) >= 0) {
        if (packet->stream_index != video_stream) {
            av_packet_unref(packet);
            continue;
        }

        avcodec_send_packet(codec_ctx, packet);

        while (avcodec_receive_frame(codec_ctx, frame) == 0) {
            sws_scale(
                sws, (const uint8_t* const*)frame->data, frame->linesize,
                      0, height, rgb_frame->data, rgb_frame->linesize
            );

            Frame out = create_frame(width, height);
            memcpy(out.pixels, rgb_buffer, rgb_linesize * height);

            video->frames[video->count++] = out;

            if (video->count >= max_frames) break;
        }

        av_packet_unref(packet);
        if (video->count >= max_frames) break;
    }

    printf("Caricati %d frame in RAM.\n", video->count);

    free(rgb_buffer);
    sws_freeContext(sws);
    av_frame_free(&frame);
    av_frame_free(&rgb_frame);
    av_packet_free(&packet);
    avcodec_free_context(&codec_ctx);
    avformat_close_input(&format_ctx);

    return true;
}

// ==== LOOP DI RIPRODUZIONE ========================================================

void playback_loop(VideoBuffer* video) {
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) < 0) {
        printf("Errore inizializzazione SDL: %s\n", SDL_GetError());
        return;
    }

    SDL_AudioSpec want, have;
    SDL_zero(want);
    want.freq = 44100;
    want.format = AUDIO_S16SYS;
    want.channels = 1;
    want.samples = AUDIO_BUFFER_SIZE;
    want.callback = audio_capture_callback;

    SDL_AudioDeviceID dev = SDL_OpenAudioDevice(NULL, 1, &want, &have, 0);
    if (dev == 0) {
        printf("Impossibile aprire l'input audio (%s).\n", SDL_GetError());
    } else {
        SDL_PauseAudioDevice(dev, 0);
        printf("\nCattura audio avviata. In attesa del primo beat...\n");
    }

    SDL_Window* window = SDL_CreateWindow(
        "Groovy Gandalf - BTrack Engine",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        video->width, video->height, 0
    );

    SDL_Renderer* renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);
    SDL_Texture* texture = SDL_CreateTexture(
        renderer, SDL_PIXELFORMAT_RGB24,
        SDL_TEXTUREACCESS_STREAMING, video->width, video->height
    );

    bool running = true;

    // --- FASE 1: ATTESA DEL PRIMO BEAT ---
    // Calcola l'indice iniziale tenendo conto dell'offset scelto
    int initial_frame_idx = (FRAME_OFFSET % video->count + video->count) % video->count;

    while (running && !first_beat_received.load()) {
        Frame* frame = &video->frames[initial_frame_idx];
        SDL_UpdateTexture(texture, NULL, frame->pixels, frame->linesize);
        SDL_RenderClear(renderer);
        SDL_RenderCopy(renderer, texture, NULL, NULL);
        SDL_RenderPresent(renderer);

        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT) running = false;
        }

        SDL_Delay(10);
    }

    // --- FASE 2: AVVIO E RIPRODUZIONE SINCRONIZZATA ---
    struct timespec current_target = now_monotonic();
    long long global_frame = FRAME_OFFSET;

    while (running) {
        // Resync di fase ad ogni beat: azzera il timer e riallinea al frame di offset
        if (beat_event.exchange(false)) {
            global_frame = FRAME_OFFSET;
            current_target = now_monotonic();
        }

        double current_fps = shared_target_fps.load();
        double current_frame_duration = 1.0 / current_fps;

        current_target = add_seconds(current_target, current_frame_duration);

        struct timespec now = now_monotonic();
        if (diff_seconds(current_target, now) > 0.5) {
            current_target = now;
        }

        sleep_until(current_target);

        // Garantisce che il valore sia positivo anche per offset negativi
        int frame_index = (int)((global_frame % video->count + video->count) % video->count);
        Frame* frame = &video->frames[frame_index];

        SDL_UpdateTexture(texture, NULL, frame->pixels, frame->linesize);
        SDL_RenderClear(renderer);
        SDL_RenderCopy(renderer, texture, NULL, NULL);
        SDL_RenderPresent(renderer);

        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT) running = false;
        }

        global_frame++;
    }

    if (dev != 0) {
        SDL_CloseAudioDevice(dev);
    }

    SDL_DestroyTexture(texture);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
}

// ==== MAIN ========================================================================

int main(int argc, char** argv) {
    const char* filename = "gandalf.mp4";

    VideoBuffer video;
    if (!load_video(filename, &video)) {
        return 1;
    }

    printf("Offset frame impostato a: %d\n", FRAME_OFFSET);
    printf("Avvio riproduzione BTrack...\n");
    playback_loop(&video);

    destroy_video(&video);
    return 0;
}
