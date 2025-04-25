#pragma once

extern "C" {
    #include <libavformat/avformat.h>
    #include <libavcodec/avcodec.h>
    #include <libswscale/swscale.h>
    #include <libswresample/swresample.h>
    #include <libavutil/imgutils.h>
    #include <libavutil/samplefmt.h>
    #include <libavutil/rational.h> // For AVRational
}

#include <SDL_opengl.h>
#include <cstdio>
#include <vector>
#include <functional>
#include <iostream>
#include <algorithm>
#include <map> // Use map for ordered cache
#include <deque> // For frame queue
#include <mutex> // For thread safety (if adding threads later)
#include <atomic> // For atomic flags
#include <SDL.h>
#include <SDL_render.h>
#include <string>
#include "shared.hpp"

struct Clip;

// Holds RGB data for a single decoded frame
struct DecodedFrame {
    std::vector<uint8_t> pixels;
    int width = 0;
    int height = 0;
    double pts = -1.0; // Presentation timestamp in seconds
};

struct VideoData {
    AVFormatContext* format_ctx = nullptr;
    AVCodecContext* codec_ctx = nullptr;
    AVFrame* frame = nullptr;       // Raw decoded frame (YUV etc.)
    // AVFrame* frame_rgb = nullptr; // We'll manage RGB buffer separately now
    SwsContext* sws_ctx = nullptr;
    AVPacket* packet = nullptr;
    int video_stream_idx = -1;
    int width = 0;
    int height = 0;
    // uint8_t* buffer = nullptr;   // Managed within DecodedFrame now
    GLuint texture_id = 0;
    bool is_initialized = false;

    // Decoding State & Cache
    double last_requested_pts = -1.0; // Last time requested by the application
    double last_decoded_pts = -1.0;   // Last PTS successfully decoded
    AVRational time_base = {0, 1};    // Stream time_base
    int64_t duration_ts = 0;          // Duration in stream time_base
    double duration_sec = 0.0;        // Duration in seconds

    std::deque<DecodedFrame> frame_cache; // Simple FIFO cache of decoded RGB frames
    static const size_t MAX_CACHE_SIZE = 5; // Adjust as needed

    // Seeking state
    bool is_seeking = false; // Use a regular bool for now
    double seek_target_pts = -1.0;

    // (Optional for threading later)
    // std::mutex cache_mutex;
    // std::atomic<bool> decoder_running = false;
    // std::thread decoder_thread;
};

struct AudioData {
    // ... (keep existing AudioData struct)
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
    AVRational time_base = {0, 1}; // Use AVRational
    bool finished = false;
    double last_seek_time = -1.0;
};


struct PreloadedAudio {
    std::vector<int16_t> samples;
    int sample_rate = 44100;
    int channels = 2;
    float duration = 0.0f;
    std::vector<float> waveform;
};

struct GLResources {
    GLuint fbo = 0;
    GLuint render_tex = 0; // For final composition / export readback
    std::map<std::string, GLuint> texture_cache; // Image textures
    std::map<std::string, VideoData> video_cache; // Video state and textures
    std::map<std::string, AudioData> audio_cache; // Audio state
    std::map<std::string, PreloadedAudio> preloaded_audio; // Preloaded audio for waveforms/export
    std::unordered_map<std::string, std::vector<GLuint>> preloaded_thumbs;

    // Current frame texture IDs for active video clips (updated by decoder logic)
    // We can just use video_cache[path].texture_id directly
};

bool setup_gl_resources(GLResources& res, int width, int height);
void load_textures(GLResources& res, const std::vector<Clip>& clips); // Only loads images now

bool is_video_file(const std::string& path);

// Renamed: initializes FFmpeg contexts, called by load_resources_for_clip
bool initialize_video_resources(GLResources& res, const std::string& path);
bool initialize_audio_resources(GLResources& res, const std::string& path); // For potential future audio streaming

// New function to load all resources for a clip (video/audio contexts + image textures)
void load_resources_for_clip(GLResources& res, const Clip& clip);

// Decodes frames up to target_time, updates cache, returns true if target is reachable
bool ensure_video_decoded_upto(VideoData& video, double target_time_seconds);

// Gets the *best available* cached frame near target_time and uploads if needed
bool update_texture_from_cache(VideoData& video, double target_time_seconds);

// The main preview update function, orchestrates decoding requests and texture uploads
void update_video_previews(GLResources& res, const std::vector<Clip>& active_clips, float current_time);

// Renders the final composited frame using *existing* textures
void render_frame(GLResources& res, float current_time, const std::vector<Clip>& sorted_clips, int width, int height);

void cleanup_video_resources(GLResources& res); // Cleans FFmpeg video resources
void cleanup_gl_resources(GLResources& res);    // Cleans OpenGL textures/FBO

bool preload_audio_file(const std::string& path, PreloadedAudio& out, float media_start, float media_duration);

bool start_video_export(const std::string& output_path,
                       int width, int height, int fps,
                       int duration_frames,
                       const std::vector<Clip>& clips,
                       SDL_Window* window); // Keep signature, implementation needs update

std::vector<float> GenerateWaveformPreview(const std::vector<int16_t>& samples, int channels, int samples_per_pixel);