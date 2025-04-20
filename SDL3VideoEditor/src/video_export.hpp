#pragma once

extern "C" {
    #include <libavformat/avformat.h>
    #include <libavcodec/avcodec.h>
    #include <libswscale/swscale.h>
    #include <libswresample/swresample.h>
    #include <libavutil/imgutils.h>
    #include <libavutil/samplefmt.h>
}

#include <SDL_opengl.h>
#include <cstdio>
#include <vector>
#include <functional>
#include <iostream>
#include <algorithm>
#include <SDL.h>
#include <SDL_render.h>
#include <string>
#include "shared.hpp"


struct Clip;

struct VideoData {
    AVFormatContext* format_ctx = nullptr;
    AVCodecContext* codec_ctx = nullptr;
    AVFrame* frame = nullptr;
    AVFrame* frame_rgb = nullptr;
    SwsContext* sws_ctx = nullptr;
    AVPacket* packet = nullptr;
    int video_stream_idx = -1;
    int width = 0;
    int height = 0;
    uint8_t* buffer = nullptr;
    GLuint texture_id = 0;
    bool is_initialized = false;
    double current_pts = 0;       // Current presentation timestamp 
    double time_base = 0;         // Time base for video stream
    int frames_read = 0;          // Number of frames read
    bool reached_eof = false;
};

struct AudioData {
    AVFormatContext* fmt_ctx = nullptr;
    AVCodecContext* codec_ctx = nullptr;
    SwrContext* swr_ctx = nullptr;
    AVFrame* frame = nullptr;
    AVPacket* packet = nullptr;
    int stream_index = -1;
    int sample_rate = 44100;
    int channels = 2;
    AVSampleFormat format = AV_SAMPLE_FMT_S16;
    int64_t next_pts = 0;
    double time_base = 0;
    bool finished = false;
    double last_seek_time = -1.0;
};

struct PreloadedAudio {
    std::vector<int16_t> samples;
    int sample_rate = 44100;
    int channels = 2;
    float duration = 0.0f;
};

// Add a map for video data to the GLResources struct
struct GLResources {
    GLuint fbo = 0;
    GLuint render_tex = 0;
    std::map<std::string, GLuint> texture_cache;
    std::map<std::string, VideoData> video_cache;
    std::map<std::string, AudioData> audio_cache;
    std::unordered_map<std::string, PreloadedAudio> preloaded_audio;
};

bool setup_gl_resources(GLResources& res, int width, int height);
void load_textures(GLResources& res, const std::vector<Clip>& clips);
void render_frame(GLResources& res, float time, const std::vector<Clip>&, int width, int height);
void cleanup_video_resources(GLResources& res);

bool start_video_export(const std::string& output_path, 
                       int width, int height, int fps,
                       int duration_frames,
                       const std::vector<Clip>& clips,
                       SDL_Window* window);

