/*
 * main.c
 * This program loads the gandalf video and plays it at
 * a specified framerate.
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

#include <SDL2/SDL.h>

#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>


typedef struct{
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



//==== TIMING MANAGEMENT ============================================================

/*
 *  Restituisce il tempo monotonic corrente
 *      per evitare drift, jitter cumulativo
 *      e diepndenza dal system clock
 *
 *  @return tempo monotonic
 */
static inline struct timespec now_monotonic();


/*
 *  Somma secondi a un timespec
 *
 *  @param timespec di partenza
 *  @param secondi da sommare
 *
 *  @return timespec sommato
 */
static inline struct timespec add_seconds(struct timespec ts, double seconds);


/*
 *  Sleep assoluto preciso
 *
 *  @param timespec target
 */
void sleep_until(struct timespec target);



//==== VIDEO AND FRAMES MANAGEMENT ==================================================

/*
 *  Alloca frame RGB
 *
 *  @param larghezza del frame
 *  @param altezza del frame
 *
 *  @return frame allocato
 */
Frame create_frame(int width, int height);


/*
 *  Apre il video, decodifica tutti i frame,
 *  converte da YUV a RGB
 *  salva tutto in RAM
 *
 *  @param nome del file
 *  @param puntatore al buffer di frame
 *
 *  @return status
 */
bool load_video(const char* filename, VideoBuffer* video);


/*
 * Playback dei frame in RAM
 *
 * @param struct con i frame
 * @param target
 */
void playback_loop(VideoBuffer* video, double target_fps);


void destroy_frame(Frame* f);

void destroy_video(VideoBuffer* video);



// ==== MAIN ========================================================================


int main(int argc, char** argv)
{
    if (argc < 2){
        printf("Usage:\n./player target_fps\n");
        return 1;
    }

    const char* filename = "gandalf.mp4";

    double target_fps = atof(argv[1]);


    // carica i fotogrammi del video

    VideoBuffer video;

    if (!load_video(filename, &video)) return 1;


    // riproduce il video

    playback_loop(
        &video,
        target_fps);


    // pulizia

    destroy_video(&video);

    return 0;
}





























static inline struct timespec now_monotonic(){

    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);

    return ts;
}


static inline struct timespec add_seconds(struct timespec ts, double seconds){

    // parte intera in secondi
    ts.tv_sec += (time_t)seconds;

    // parte nanosecondi
    ts.tv_nsec +=
    (long)((seconds - (time_t)seconds) * 1e9);

    // normalizzazione
    if (ts.tv_nsec >= 1000000000L)
    {
        ts.tv_sec++;
        ts.tv_nsec -= 1000000000L;
    }

    return ts;
}


void sleep_until(struct timespec target){

    clock_nanosleep(
        CLOCK_MONOTONIC,
        TIMER_ABSTIME,
        &target,
        NULL);
}






Frame create_frame(int width, int height){

    Frame f;

    f.width = width;
    f.height = height;

    //RGB24: 3 bytes per pixel
    f.linesize = width * 3;

    size_t size =
    f.linesize * height;

    f.pixels = malloc(size);

    return f;
}


bool load_video(const char* filename, VideoBuffer* video){

    // contiene le info sul video
    AVFormatContext* format_ctx = NULL;

    if (avformat_open_input(
        &format_ctx,
        filename,
        NULL,
        NULL) < 0){

        printf("Cannot open file\n");
        return false;
    }


    // legge info stream
    avformat_find_stream_info(format_ctx, NULL);

    // trova lo stream video
    int video_stream = av_find_best_stream(format_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);

    if (video_stream < 0){
        printf("No video stream\n");
        return false;
    }

    AVStream* stream = format_ctx->streams[video_stream];

    // open decoder
    const AVCodec* codec = avcodec_find_decoder(stream->codecpar->codec_id);

    AVCodecContext* codec_ctx = avcodec_alloc_context3(codec);

    avcodec_parameters_to_context(codec_ctx, stream->codecpar);

    avcodec_open2(codec_ctx, codec, NULL);

    int width = codec_ctx->width;
    int height = codec_ctx->height;

    video->width = width;
    video->height = height;


    // allora il numero di frame (facciamo 20 per stare larghi)
    int max_frames = 20;

    video->frames = malloc(sizeof(Frame) * max_frames);

    video->count = 0;


    // strutture ffmpeg

    AVFrame* frame = av_frame_alloc();

    AVFrame* rgb_frame = av_frame_alloc();

    AVPacket* packet = av_packet_alloc();


    // conversione in RGB

    int rgb_linesize = width * 3;

    uint8_t* rgb_buffer = malloc(rgb_linesize * height);

    av_image_fill_arrays(
        rgb_frame->data,
        rgb_frame->linesize,
        rgb_buffer,
        AV_PIX_FMT_RGB24,
        width,
        height,
        1);

    struct SwsContext* sws = sws_getContext(
        width,
        height,
        codec_ctx->pix_fmt,
        width,
        height,
        AV_PIX_FMT_RGB24,
        SWS_BILINEAR,
        NULL,
        NULL,
        NULL);


    // decodifica
    // scorre tutti i pacchetti

    while (av_read_frame(format_ctx, packet) >= 0){

        // se il pacchetto non è video lo salta
        if (packet->stream_index != video_stream){
            av_packet_unref(packet);
            continue;
        }

        avcodec_send_packet(codec_ctx, packet);

        while (avcodec_receive_frame(codec_ctx, frame) == 0){

            // conversione in RGB
            sws_scale(
                sws,
                (const uint8_t* const*)frame->data,
                frame->linesize,
                0,
                height,
                rgb_frame->data,
                rgb_frame->linesize);

            // copia il frame
            Frame out = create_frame(width, height);

            memcpy(out.pixels, rgb_buffer, rgb_linesize * height);

            // salva il frame
            video->frames[video->count++] = out;

            // controlla di non eccedere il numero di frame allocati
            if (video->count >= max_frames) break;

        }

        av_packet_unref(packet);

        if (video->count >= max_frames) break;
    }

    printf("Loaded %d frames\n", video->count);


    free(rgb_buffer);

    sws_freeContext(sws);

    av_frame_free(&frame);
    av_frame_free(&rgb_frame);

    av_packet_free(&packet);

    avcodec_free_context(&codec_ctx);

    avformat_close_input(&format_ctx);

    return true;
}



void playback_loop(VideoBuffer* video, double target_fps){


    // inizializza SDL

    SDL_Init(SDL_INIT_VIDEO);

    SDL_Window* window =
    SDL_CreateWindow(
        "Groovy Gandalf",
        SDL_WINDOWPOS_CENTERED,
        SDL_WINDOWPOS_CENTERED,
        video->width,
        video->height,
        0);


    // disabilita vsync per non alterare il timing

    SDL_Renderer* renderer =
    SDL_CreateRenderer(
        window,
        -1,
        SDL_RENDERER_ACCELERATED);

    SDL_Texture* texture =
    SDL_CreateTexture(
        renderer,
        SDL_PIXELFORMAT_RGB24,
        SDL_TEXTUREACCESS_STREAMING,
        video->width,
        video->height);


    double frame_duration = 1.0 / target_fps;

    // tempo iniziale assoluto
    struct timespec start = now_monotonic();

    // contatore globale frame mostrati
    // calcoliamo sempre il tempo assoluto sulla base di questo
    // in modo da evitare drift
    long long global_frame = 0;

    bool running = true;



    while (running)
    {

        int frame_index = global_frame % video->count;

        Frame* frame = &video->frames[frame_index];

        // T = T0 + n * dt
        struct timespec target =
        add_seconds(
            start,
            global_frame * frame_duration);


        sleep_until(target);

        // carica frame
        SDL_UpdateTexture(
            texture,
            NULL,
            frame->pixels,
            frame->linesize);

        // render
        SDL_RenderClear(renderer);

        SDL_RenderCopy(
            renderer,
            texture,
            NULL,
            NULL);

        SDL_RenderPresent(renderer);

        SDL_Event event;

        while (SDL_PollEvent(&event))
            if (event.type == SDL_QUIT) running = false;


        global_frame++;
    }

    SDL_DestroyTexture(texture);

    SDL_DestroyRenderer(renderer);

    SDL_DestroyWindow(window);

    SDL_Quit();
}



void destroy_frame(Frame* f){

    if (f->pixels){

        free(f->pixels);
        f->pixels = NULL;
    }
}

void destroy_video(VideoBuffer* video){

    for (int i = 0; i < video->count; i++) {
        destroy_frame(&video->frames[i]);
    }

    free(video->frames);
}

