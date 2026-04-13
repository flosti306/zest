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
#include <thread>
#include <condition_variable>
#include <future>
#include <queue> // Can use queue or deque
#include <set> // For tracking requested frames
#include <SDL.h>
#include <SDL_render.h>
#include <string>
#include "shared.hpp"

// Forward declare structs if needed by function signatures below
struct Clip;
struct GLResources;
struct ThumbnailRequest;
struct ThumbnailResult;
struct DecodedFrameRequest;
struct DecodedFrameResult;

extern std::thread thumbnail_worker;
extern std::queue<ThumbnailRequest> thumbnail_request_queue;
extern std::queue<ThumbnailResult> thumbnail_result_queue;
extern std::mutex request_mutex;
extern std::mutex result_mutex;
extern std::condition_variable worker_cv;
extern std::atomic<bool> stop_thumbnail_worker_flag;

extern std::thread decoder_thread;
extern std::deque<DecodedFrameRequest> decoder_request_queue;
extern std::queue<DecodedFrameResult> decoder_result_queue;
extern std::mutex decoder_request_mutex;
extern std::mutex decoder_result_mutex;
extern std::condition_variable decoder_worker_cv;
extern std::atomic<bool> stop_decoder_worker_flag;

constexpr int THUMBNAIL_WIDTH = 64;
constexpr int THUMBNAIL_HEIGHT = 36;

// --- Add these function declarations globally ---
void start_thumbnail_worker();
void stop_thumbnail_worker();
void ProcessThumbnailResults(GLResources& res, int max_per_frame);
void thumbnail_worker_func(); // Declaration for the worker thread function itself

// --- Add NEW function declarations for the decoder thread ---
void start_decoder_worker();
void stop_decoder_worker();
void process_decoded_frames(GLResources& res, int max_per_frame); // Processes results from the thread
void decoder_worker_func(); // The thread's main function

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

    // Seeking state
    bool is_seeking = false; // Use a regular bool for now
    double seek_target_pts = -1.0;

    GLuint pbos[2] = {0, 0};
    int pbo_index = 0; // Will be 0 or 1, to ping-pong between them
    int pbo_buffer_size = 0;
    

    // Enhanced request tracking
    std::set<double> pending_requests;
    std::chrono::steady_clock::time_point last_request_time;
    double last_successful_decode_time = -1.0;
    
    // Performance tracking
    int consecutive_cache_hits = 0;
    bool is_actively_playing = false;
    
    // Request throttling
    static constexpr double MIN_REQUEST_INTERVAL = 0.001; // Allow more frequent requests
    static constexpr double CACHE_TOLERANCE = 1.0 / 60.0; // 1 frame at 60fps
    static constexpr int MAX_PENDING_REQUESTS = 30;       // Allow more in-flight requests
    
    // Cache management
    static constexpr size_t MAX_CACHE_SIZE = 150; // Reduced from potentially larger value
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
    // Store the actual textures here, mapped by clip path
    std::unordered_map<std::string, std::vector<GLuint>> clip_thumbnail_textures;
    // Add a map to track which timestamps have generated textures for a clip
    std::unordered_map<std::string, std::unordered_map<float, GLuint>> generated_thumbnails_map;
};

struct ThumbnailRequest {
    std::string clip_path; // Use path to identify clip safely
    float timestamp = 0.0f;
    // Optional: add a unique ID if needed for complex scenarios
};

struct ThumbnailResult {
    std::string clip_path;
    float timestamp = 0.0f;
    std::vector<uint8_t> pixels; // Decoded RGB pixels
    int width = 0;
    int height = 0;
    bool success = false;
    std::string error_message;
};

enum class RequestPriority {
    Normal = 0, // For playback prefetching
    High = 1      // For scrubbing, pausing, and first frame
};

struct DecodedFrameRequest {
    std::string clip_path;
    double timestamp = 0.0f; // The media timestamp to decode
    RequestPriority priority = RequestPriority::Normal;
    std::shared_ptr<std::promise<DecodedFrame>> sync_promise;
};

struct DecodedFrameResult {
    std::string clip_path;
    bool success = false;
    std::string error_message;
    DecodedFrame frame; // Contains the pixels, w, h, and pts
};

struct DecoderState {
    AVFormatContext* fmt_ctx = nullptr;
    AVCodecContext* codec_ctx = nullptr;
    SwsContext* sws_ctx = nullptr; // For color conversion
    int video_stream_idx = -1;
    AVRational time_base = {0, 1};
    double last_decoded_pts = -1.0; // State for this specific decoder

    AVBufferRef* hw_device_ctx = nullptr;
    AVPixelFormat hw_pix_fmt = AV_PIX_FMT_NONE;

    ~DecoderState() {
        sws_freeContext(sws_ctx);
        if (codec_ctx) avcodec_free_context(&codec_ctx);
        if (fmt_ctx) avformat_close_input(&fmt_ctx);
        if (hw_device_ctx) av_buffer_unref(&hw_device_ctx);
    }
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

// Renders the final composited frame using *existing* textures
void render_frame(GLResources& res, float current_time, const std::vector<Clip>& sorted_clips, int width, int height, int fps);

void cleanup_video_resources(GLResources& res); // Cleans FFmpeg video resources
void cleanup_gl_resources(GLResources& res);    // Cleans OpenGL textures/FBO

bool preload_audio_file(const std::string& path, PreloadedAudio& out, float media_start, float media_duration);

bool start_video_export(const std::string& output_path,
                       int width, int height, int fps,
                       int duration_frames,
                       const std::vector<Clip>& clips,
                       SDL_Window* window); 

// Global UI function for the Render Window
void DrawRenderWindow(const std::vector<Clip>& clips, bool* p_open, int& render_width, int& render_height, int& export_fps, float max_duration);

std::vector<float> GenerateWaveformPreview(const std::vector<int16_t>& samples, int channels, int samples_per_pixel);

void QueueClipThumbnails(GLResources& res, const Clip& clip);
void ProcessThumbnailTasks(GLResources& res, int max_per_frame);

void start_thumbnail_worker();
void stop_thumbnail_worker();

void update_video_previews(GLResources& res, const std::vector<Clip>& active_clips, float current_time, bool is_playing, bool is_scrubbing);
bool update_texture_from_cache(VideoData& video, double target_time_seconds, bool strict);
void update_playback_state(GLResources& res, float current_time, float last_time, bool& is_playing_out, bool& is_scrubbing_out);
bool should_request_frame(VideoData& video, double target_time);

struct AudioPlaybackState {
    SDL_AudioDeviceID device = 0;
    SDL_AudioStream* stream = nullptr;
    bool is_playing = false;
    float volume = 1.0f;
    double current_time = 0.0;
    std::vector<const Clip*> active_clips;
};

// Function declarations for audio playback
bool initialize_audio_playback(AudioPlaybackState& state);
void cleanup_audio_playback(AudioPlaybackState& state);
void update_audio_playback(AudioPlaybackState& state, const std::vector<Clip>& clips, float current_time, GLResources& res);
void pause_audio_playback(AudioPlaybackState& state);
void resume_audio_playback(AudioPlaybackState& state);
void set_audio_volume(AudioPlaybackState& state, float volume);