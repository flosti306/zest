// Full OpenGL + FFmpeg Export Implementation for SDL3 + ImGui Video Editor
#include <glad/glad.h>
#include <SDL_opengl.h>
#include <cstdio>
#include <vector>
#include <functional>
#include <iostream>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <SDL.h>
#include <SDL_render.h>
#include <SDL_image.h>
#include <string>
#include <map>
#include <thread>
#include <limits>
#include <cmath>
#include <queue> // Added for thumbnail queues
#include <mutex> // Added
#include <condition_variable> // Added
#include <atomic> // Added

#include "shared.hpp"
#include "node.hpp" // Include Node definition
#include "video_export.hpp"

#include <imgui.h>

extern "C" {
    #include <libavcodec/avcodec.h>
    #include <libavformat/avformat.h>
    #include <libswscale/swscale.h>
    #include <libswresample/swresample.h>
    #include <libavutil/imgutils.h>
    #include <libavutil/opt.h>
    #include <libavutil/channel_layout.h>
    #include <libavutil/error.h> // Added for av_make_error_string
}

// Thumbnail Worker Globals (Keep as is)
std::thread thumbnail_worker;
std::queue<ThumbnailRequest> thumbnail_request_queue;
std::queue<ThumbnailResult> thumbnail_result_queue;
std::mutex request_mutex;
std::mutex result_mutex;
std::condition_variable worker_cv;
std::atomic<bool> stop_thumbnail_worker_flag = false;

void start_thumbnail_worker();
void stop_thumbnail_worker();

// Debug function (Keep as is)
void check_gl_context() {
    if (SDL_GL_GetCurrentContext()) {
        std::cout << "OpenGL context is active" << std::endl;
    } else {
        std::cerr << "OpenGL context is NOT active" << std::endl;
    }
    
    GLenum err = glGetError();
    if (err != GL_NO_ERROR) {
        std::cerr << "OpenGL error: " << err << std::endl;
    }
}

// Setup GL Resources (Keep as is)
bool setup_gl_resources(GLResources& res, int width, int height) {
    // Check if already setup
    if (res.fbo != 0 || res.render_tex != 0) {
        // Optionally resize existing resources if dimensions changed
        // For now, just return true if they exist
        std::cout << "GL resources already initialized." << std::endl;
        return true;
    }

    glGenFramebuffers(1, &res.fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, res.fbo);

    glGenTextures(1, &res.render_tex);
    glBindTexture(GL_TEXTURE_2D, res.render_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, width, height, 0, GL_RGB, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, res.render_tex, 0);

    GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        std::cerr << "Framebuffer not complete! Status: " << status << std::endl;
        glBindFramebuffer(GL_FRAMEBUFFER, 0); // Unbind before returning
        glDeleteTextures(1, &res.render_tex); res.render_tex = 0;
        glDeleteFramebuffers(1, &res.fbo); res.fbo = 0;
        return false;
    }

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glBindTexture(GL_TEXTURE_2D, 0);
    std::cout << "GL resources initialized successfully." << std::endl;
    return true;
}

// Function to check if a file is a video file (Keep as is)
bool is_video_file(const std::string& path) {
    std::string ext = std::filesystem::path(path).extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    return ext == ".mp4" || ext == ".avi" || ext == ".mov" || ext == ".m4v" || ext == ".mkv";
}
// Function to check if a file is an image file
bool is_image_file(const std::string& path) {
    std::string ext = std::filesystem::path(path).extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    // Add common image extensions
    return ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".bmp" || ext == ".tga";
}


// Initialize Video Resources (FFmpeg part, Keep mostly as is, operates on path)
bool initialize_video_resources(GLResources& res, const std::string& path, int& width, int& height, double& duration) {
     if (res.video_cache.count(path)) {
         // Already initialized, retrieve existing info
         width = res.video_cache[path].width;
         height = res.video_cache[path].height;
         duration = res.video_cache[path].duration_sec;
        return res.video_cache[path].is_initialized;
    }

    VideoData video; // Creates a default VideoData

    // Open the file
    video.format_ctx = avformat_alloc_context();
    if (avformat_open_input(&video.format_ctx, path.c_str(), nullptr, nullptr) != 0) {
        std::cerr << "Could not open video file: " << path << std::endl;
        avformat_free_context(video.format_ctx); // Need to free context
        return false;
    }

    // Find stream info
    if (avformat_find_stream_info(video.format_ctx, nullptr) < 0) {
        std::cerr << "Could not find stream info for: " << path << std::endl;
        avformat_close_input(&video.format_ctx); // Close input on failure
        return false;
    }

    // Find the first video stream
    video.video_stream_idx = -1;
    for (unsigned i = 0; i < video.format_ctx->nb_streams; i++) {
        if (video.format_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            video.video_stream_idx = i;
            break;
        }
    }

    if (video.video_stream_idx == -1) {
        std::cerr << "No video stream found in: " << path << std::endl;
        avformat_close_input(&video.format_ctx);
        return false;
    }

    // Get codec parameters
    AVCodecParameters* codec_params = video.format_ctx->streams[video.video_stream_idx]->codecpar;

    // Find decoder
    const AVCodec* codec = avcodec_find_decoder(codec_params->codec_id);
    if (!codec) {
        std::cerr << "Unsupported codec in: " << path << std::endl;
        avformat_close_input(&video.format_ctx);
        return false;
    }

    // Allocate codec context
    video.codec_ctx = avcodec_alloc_context3(codec);
    if (!video.codec_ctx) {
        std::cerr << "Failed to allocate codec context for: " << path << std::endl;
        avformat_close_input(&video.format_ctx);
        return false;
    }

    // Copy parameters to context
    if (avcodec_parameters_to_context(video.codec_ctx, codec_params) < 0) {
        std::cerr << "Failed to copy codec parameters for: " << path << std::endl;
        avcodec_free_context(&video.codec_ctx);
        avformat_close_input(&video.format_ctx);
        return false;
    }

    // Open codec
    if (avcodec_open2(video.codec_ctx, codec, nullptr) < 0) {
        std::cerr << "Failed to open codec for: " << path << std::endl;
        avcodec_free_context(&video.codec_ctx);
        avformat_close_input(&video.format_ctx);
        return false;
    }

    // Allocate frames
    video.frame = av_frame_alloc();
    if (!video.frame) { avcodec_free_context(&video.codec_ctx); avformat_close_input(&video.format_ctx); return false; }

    video.width = video.codec_ctx->width;
    video.height = video.codec_ctx->height;
    width = video.width; // Pass out width
    height = video.height; // Pass out height

    // Initialize SWS context for YUV->RGB conversion
     video.sws_ctx = sws_getContext(
        video.width, video.height, video.codec_ctx->pix_fmt,
        video.width, video.height, AV_PIX_FMT_RGB24, // Target RGB
        SWS_BILINEAR, nullptr, nullptr, nullptr
    );
    if (!video.sws_ctx) { av_frame_free(&video.frame); avcodec_free_context(&video.codec_ctx); avformat_close_input(&video.format_ctx); return false; }

    video.packet = av_packet_alloc();
    if (!video.packet) { sws_freeContext(video.sws_ctx); av_frame_free(&video.frame); avcodec_free_context(&video.codec_ctx); avformat_close_input(&video.format_ctx); return false; }

    // Store time base and duration
    video.time_base = video.format_ctx->streams[video.video_stream_idx]->time_base;
    video.duration_ts = video.format_ctx->streams[video.video_stream_idx]->duration;
    if (video.duration_ts > 0 && video.time_base.den > 0) {
         video.duration_sec = static_cast<double>(video.duration_ts) * av_q2d(video.time_base);
    } else if (video.format_ctx->duration != AV_NOPTS_VALUE) {
         video.duration_sec = static_cast<double>(video.format_ctx->duration) / AV_TIME_BASE;
    } else {
        video.duration_sec = 0.0; // Unknown duration
        std::cerr << "Warning: Could not determine duration for " << path << std::endl;
    }
    duration = video.duration_sec; // Pass out duration


    // Create OpenGL texture (initially empty)
    glGenTextures(1, &video.texture_id);
    glBindTexture(GL_TEXTURE_2D, video.texture_id);
    // Use GL_RGBA for flexibility, even if source is RGB, simplifies rendering pipeline
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, video.width, video.height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0); // Unbind

    video.is_initialized = true;

    // Move the fully constructed VideoData into the cache
    auto [it, success] = res.video_cache.emplace(path, std::move(video));
    if (!success) {
         // Handle error: Element with this path already existed? Or emplace failed?
         std::cerr << "Error: Failed to emplace VideoData into cache for " << path << std::endl;
         // Need to clean up partially initialized 'video' object before returning false
         // Note: Since 'video' was moved, its members are now null/default.
         // We need to call cleanup on the potential original 'video' object if emplace failed
         // This part is tricky after a move. Best to check cache *before* creating VideoData.
         // (Correction: The check is already done at the beginning)
         // If emplace failed, the original 'video' object is destroyed, releasing some resources.
         // Need manual cleanup for things not handled by destructor like GL texture.
         // Let's assume emplace won't fail if the initial check passes.
         return false; // Or handle more gracefully
    }
     // If successful, the 'video' object has been moved from, use the iterator 'it'
    std::cout << "Initialized video resources for: " << path << " (ID: " << it->second.texture_id << ")" << std::endl;

    return true;
}


// Loads ONLY image textures
// Takes Clip reference to get path, but operates on GLResources cache keyed by path
bool load_image_texture(GLResources& res, const Clip& clip, int& width, int& height) {
    // Skip if already loaded
    if (res.texture_cache.count(clip.path)) {
        // Need to get width/height if already loaded
        // This requires storing width/height alongside texture ID, or querying GL (slow)
        // For now, let's assume we need to load it fully to get dimensions reliably if not cached
        // A better approach: Store dimensions in GLResources::texture_cache alongside ID
        // Temporary workaround: Reload to get dimensions if needed (inefficient)
        // Better: Add width/height to texture_cache entry (e.g., map<string, pair<GLuint, ImVec2>>)
        // For simplicity now, just return true, assuming dimensions were stored in Clip before
        // width = clip.source_width; // Assume these were set when clip was added
        // height = clip.source_height;
        // Querying the texture size (Example - requires texture to be bound):
        glBindTexture(GL_TEXTURE_2D, res.texture_cache[clip.path]);
        glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &width);
        glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &height);
        glBindTexture(GL_TEXTURE_2D, 0);
        return true;
    }

    if (!std::filesystem::exists(clip.path)) {
        std::cerr << "Image file does not exist: " << clip.path << std::endl;
        return false;
    }

    SDL_Surface* surface = IMG_Load(clip.path.c_str());
    if (!surface) {
        std::cerr << "Failed to load image: " << clip.path << " - " << SDL_GetError() << std::endl;
        return false;
    }

    // Pass out dimensions
    width = surface->w;
    height = surface->h;

    SDL_Surface* formatted_surface = nullptr;
    SDL_PixelFormat target_format_sdl;
    GLenum gl_format, gl_internal_format;

    bool has_alpha = SDL_ISPIXELFORMAT_ALPHA(surface->format);

    if (has_alpha) {
        target_format_sdl = SDL_PIXELFORMAT_RGBA32;
        gl_format = GL_RGBA;
        gl_internal_format = GL_RGBA; // Use RGBA internal format
    } else {
        target_format_sdl = SDL_PIXELFORMAT_RGB24;
        gl_format = GL_RGB;
        gl_internal_format = GL_RGB; // Use RGB internal format
    }

    if (surface->format != target_format_sdl) {
         std::cout << "Converting surface " << clip.path << " from "
                      << SDL_GetPixelFormatName(surface->format) << " to "
                      << SDL_GetPixelFormatName(target_format_sdl) << std::endl;
        formatted_surface = SDL_ConvertSurface(surface, target_format_sdl);
        if (!formatted_surface) {
            std::cerr << "Failed to convert image surface for " << clip.path << ": " << SDL_GetError() << std::endl;
            SDL_DestroySurface(surface);
            return false;
        }
        SDL_DestroySurface(surface);
    } else {
        formatted_surface = surface;
    }

    GLuint tex_id = 0;
    glGenTextures(1, &tex_id);
    if (tex_id == 0) {
         std::cerr << "Failed to generate texture ID for " << clip.path << std::endl;
         SDL_DestroySurface(formatted_surface);
         return false;
    }
    glBindTexture(GL_TEXTURE_2D, tex_id);

    glTexImage2D(GL_TEXTURE_2D, 0, gl_internal_format, formatted_surface->w, formatted_surface->h, 0,
                 gl_format, GL_UNSIGNED_BYTE, formatted_surface->pixels);

    GLenum err = glGetError();
    if (err != GL_NO_ERROR) {
        std::cerr << "OpenGL error creating texture for " << clip.path << ": " << err << std::endl;
        glDeleteTextures(1, &tex_id);
        SDL_DestroySurface(formatted_surface);
        return false;
    }

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    res.texture_cache[clip.path] = tex_id; // Store the texture ID
    SDL_DestroySurface(formatted_surface);
    std::cout << "Loaded image texture: " << clip.path << " (ID: " << tex_id << ")" << std::endl;

    glBindTexture(GL_TEXTURE_2D, 0); // Unbind texture unit
    return true;
}


// Function to load necessary resources (textures, ffmpeg contexts) for a specific *source* clip
// This is called when a Clip is added to the media library.
bool load_resources_for_clip(GLResources& res, Clip& clip) { // Pass non-const Clip to store W/H/Duration
    bool success = true;
    if (clip.type == ClipType::Video) {
        if (!res.video_cache.count(clip.path) || !res.video_cache[clip.path].is_initialized) {
            double video_duration_double = 0.0;
            success &= initialize_video_resources(res, clip.path, clip.source_width, clip.source_height, video_duration_double);
            clip.source_duration = static_cast<float>(video_duration_double);
        } else {
            // Already loaded, just grab info
            clip.source_width = res.video_cache[clip.path].width;
            clip.source_height = res.video_cache[clip.path].height;
            clip.source_duration = static_cast<float>(res.video_cache[clip.path].duration_sec);
        }
         // Preload audio for waveform/export if not already done
        if (clip.has_audio && !res.preloaded_audio.count(clip.path)) {
            PreloadedAudio audio;
            if (preload_audio_file(clip.path, audio, 0.0f, -1.0f)) { // Preload entire file for now
                 clip.waveform = audio.waveform; // Store waveform preview in Clip struct
                 res.preloaded_audio[clip.path] = std::move(audio);
                 std::cout << "Preloaded audio for: " << clip.path << std::endl;
            } else {
                 std::cerr << "Failed to preload audio for: " << clip.path << std::endl;
                 clip.has_audio = false; // Mark as no audio if preload failed
                 success = false; // Indicate potential issue
            }
        }

    } else if (clip.type == ClipType::Image) {
        if (!res.texture_cache.count(clip.path)) {
            success &= load_image_texture(res, clip, clip.source_width, clip.source_height);
            // Images have infinite source duration conceptually, or set a default?
            clip.source_duration = std::numeric_limits<float>::max(); // Or a large number
        } else {
             // Get dimensions from cache (implement this properly later)
             GLuint tex_id = res.texture_cache[clip.path];
             glBindTexture(GL_TEXTURE_2D, tex_id);
             glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &clip.source_width);
             glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &clip.source_height);
             glBindTexture(GL_TEXTURE_2D, 0);
             clip.source_duration = std::numeric_limits<float>::max();
        }
        clip.has_audio = false; // Images don't have audio

    } else if (clip.type == ClipType::Audio) {
         // Preload audio for waveform/export if not already done
        if (!res.preloaded_audio.count(clip.path)) {
            PreloadedAudio audio;
            if (preload_audio_file(clip.path, audio, 0.0f, -1.0f)) {
                 clip.waveform = audio.waveform;
                 clip.source_duration = audio.duration;
                 clip.source_width = 0; // No visual dimensions
                 clip.source_height = 0;
                 res.preloaded_audio[clip.path] = std::move(audio);
                 std::cout << "Preloaded audio-only file: " << clip.path << std::endl;
            } else {
                 std::cerr << "Failed to preload audio-only file: " << clip.path << std::endl;
                 success = false;
            }
        } else {
            // Already loaded
            clip.waveform = res.preloaded_audio[clip.path].waveform;
            clip.source_duration = res.preloaded_audio[clip.path].duration;
        }
        clip.has_audio = true; // It is audio
        clip.is_audio_only = true;
    }
    return success;
}


// Ensure video is decoded up to a specific *media time* (Keep mostly as is, operates on VideoData)
bool ensure_video_decoded_upto(VideoData& video, double target_media_time_seconds) {
    // ... (implementation largely unchanged, uses video.frame_cache, video.last_decoded_pts etc.)
    // Make sure PTS comparisons are correct
    if (!video.is_initialized) return false;
    if (video.is_seeking) return false; // Don't decode while seeking

    // --- Seeking Logic ---
    bool needs_seek = false;
     // Target is significantly behind *cache start* or before first decoded frame
    if (video.frame_cache.empty() || target_media_time_seconds < video.frame_cache.front().pts - 1.0) {
        needs_seek = true;
    }
     // Target is significantly beyond the last decoded point (cache might be full but behind target)
    else if (target_media_time_seconds > video.last_decoded_pts + 1.0 && video.frame_cache.size() >= VideoData::MAX_CACHE_SIZE)
    {
        needs_seek = true;
    }
     // Initial state
     else if (video.last_decoded_pts < 0) {
         needs_seek = true;
     }


    if (needs_seek) {
        // Avoid seeking if target is very close to start and we haven't decoded anything
        if (target_media_time_seconds < 0.1 && video.last_decoded_pts < 0) {
             std::cout << "Skipping seek for target near 0 before first decode: " << video.format_ctx->url << std::endl;
        } else {
            video.is_seeking = true;
            video.seek_target_pts = target_media_time_seconds;

            int64_t seek_target_ts = av_rescale_q(static_cast<int64_t>(target_media_time_seconds * AV_TIME_BASE),
                                                  AV_TIME_BASE_Q, video.time_base);

            std::cout << "Seeking video " << video.format_ctx->url << " to ~" << target_media_time_seconds << "s (TS: " << seek_target_ts << ")" << std::endl;
            int ret = av_seek_frame(video.format_ctx, video.video_stream_idx, seek_target_ts, AVSEEK_FLAG_BACKWARD);

            if (ret < 0) {
                char errbuf[AV_ERROR_MAX_STRING_SIZE];
                av_make_error_string(errbuf, sizeof(errbuf), ret);
                std::cerr << "Error seeking video " << video.format_ctx->url << " to " << target_media_time_seconds << "s: " << errbuf << std::endl;
                // Don't return false, allow decoding attempt from current pos maybe? Or clear cache and retry from start?
                // For now, just log and continue, but clear buffers.
            }

            avcodec_flush_buffers(video.codec_ctx);
            video.last_decoded_pts = -1.0; // Reset target PTS after flush
            video.frame_cache.clear();     // Clear cache after seek
            video.is_seeking = false;
        }
    }


    // --- Sequential Decoding Logic ---
    // Decode until the *cache* contains a frame at or after the target time
    while (video.frame_cache.empty() || video.frame_cache.back().pts < target_media_time_seconds)
    {
        // Try receiving frames first
        int ret = avcodec_receive_frame(video.codec_ctx, video.frame);

        if (ret == AVERROR(EAGAIN)) {
            // Need more packets
            av_packet_unref(video.packet);
            ret = av_read_frame(video.format_ctx, video.packet);
            if (ret < 0) {
                if (ret == AVERROR_EOF) {
                    // End of file, flush decoder
                    avcodec_send_packet(video.codec_ctx, nullptr);
                     while (true) {
                        ret = avcodec_receive_frame(video.codec_ctx, video.frame);
                        if (ret == AVERROR_EOF || ret == AVERROR(EAGAIN)) break;
                        if (ret < 0) { std::cerr << "Error receiving flushed frame\n"; break; }

                        double pts_sec = (video.frame->pts == AV_NOPTS_VALUE) ?
                                        video.last_decoded_pts + av_q2d(video.codec_ctx->time_base) : // Estimate if no PTS
                                        static_cast<double>(video.frame->pts) * av_q2d(video.time_base);

                        // Convert and cache flushed frame
                        DecodedFrame decoded; /* ... fill ... */
                        decoded.width = video.width;
                        decoded.height = video.height;
                        decoded.pts = pts_sec;
                        decoded.pixels.resize(video.width * video.height * 3);
                        uint8_t* dest_data[4] = {decoded.pixels.data(), nullptr, nullptr, nullptr};
                        int dest_linesize[4] = {video.width * 3, 0, 0, 0};
                        sws_scale(video.sws_ctx, video.frame->data, video.frame->linesize, 0, video.height, dest_data, dest_linesize);

                        video.frame_cache.push_back(std::move(decoded));
                        if (video.frame_cache.size() > VideoData::MAX_CACHE_SIZE) video.frame_cache.pop_front();
                        video.last_decoded_pts = pts_sec;
                        av_frame_unref(video.frame); // Unref frame after use
                    }
                    std::cout << "EOF reached for " << video.format_ctx->url << std::endl;
                    return !video.frame_cache.empty(); // Success if we cached at least one frame
                } else { /* Error reading */ return false; }
            }

            // Send the new packet
            if (video.packet->stream_index == video.video_stream_idx) {
                 ret = avcodec_send_packet(video.codec_ctx, video.packet);
                if (ret < 0) { /* Error sending */ av_packet_unref(video.packet); return false; }
            }
             av_packet_unref(video.packet);
             continue; // Loop back to try receiving again
        } else if (ret == AVERROR_EOF) {
            std::cout << "Decoder finished for " << video.format_ctx->url << std::endl;
            return !video.frame_cache.empty(); // Reached end
        } else if (ret < 0) { /* Error receiving */ return false; }

        // --- Successfully Received a Frame ---
        double pts_sec = (video.frame->pts == AV_NOPTS_VALUE) ?
                         video.last_decoded_pts + av_q2d(video.codec_ctx->time_base) : // Estimate
                         static_cast<double>(video.frame->pts) * av_q2d(video.time_base);

        // Convert and cache the frame
        DecodedFrame decoded; /* ... fill ... */
        decoded.width = video.width;
        decoded.height = video.height;
        decoded.pts = pts_sec;
        decoded.pixels.resize(video.width * video.height * 3);
        uint8_t* dest_data[4] = {decoded.pixels.data(), nullptr, nullptr, nullptr};
        int dest_linesize[4] = {video.width * 3, 0, 0, 0};
        sws_scale(video.sws_ctx, video.frame->data, video.frame->linesize, 0, video.height, dest_data, dest_linesize);


        // Insert into cache sorted by PTS (or just push_back if always sequential)
        // Assuming sequential for now after seek/flush
        video.frame_cache.push_back(std::move(decoded));
        if (video.frame_cache.size() > VideoData::MAX_CACHE_SIZE) {
            video.frame_cache.pop_front();
        }
        video.last_decoded_pts = pts_sec; // Track PTS of last *added* frame
        // std::cout << "Cached frame PTS: " << pts_sec << "s for " << video.format_ctx->url << std::endl;
        av_frame_unref(video.frame); // Unref frame after use
    }


    return true; // Successfully decoded enough to satisfy the target time potentially being in cache
}


// Finds the best frame in cache for target_media_time and uploads it (Keep mostly as is)
bool update_texture_from_cache(VideoData& video, double target_media_time_seconds) {
    // ... (implementation mostly unchanged, finds best frame in video.frame_cache based on target_media_time_seconds)
     if (!video.is_initialized || video.frame_cache.empty()) {
        // std::cerr << "Warning: Cannot update texture, video not init or cache empty for " << video.format_ctx->url << std::endl;
        return false;
    }

    // Find the closest frame in the cache (prefer frame just before or at target time)
    const DecodedFrame* best_frame = nullptr;
    double min_diff = std::numeric_limits<double>::max();

     // Iterate backwards for potentially better cache coherency / finding frame <= target quickly
    for (auto it = video.frame_cache.rbegin(); it != video.frame_cache.rend(); ++it) {
        double diff = target_media_time_seconds - it->pts; // Positive diff means frame is before target
        if (diff >= 0) { // Found a frame at or before the target
             if (diff < min_diff) { // Is it the closest one found so far?
                 min_diff = diff;
                 best_frame = &(*it);
             }
        }
         // Optimization: If we are significantly past the target looking backwards,
         // and we already found a candidate (best_frame != nullptr), we can stop.
        if (diff < -0.5 && best_frame) { // Frame is > 0.5s after target
             break;
         }
    }

     // If no frame at or before target was found, use the very first frame in cache
     if (!best_frame && !video.frame_cache.empty()) {
         best_frame = &video.frame_cache.front();
         min_diff = std::abs(target_media_time_seconds - best_frame->pts);
         // std::cout << "Warning: No frame found at/before target " << target_media_time_seconds << ". Using first cached frame PTS: " << best_frame->pts << std::endl;
     }


    if (best_frame) {
        // Check if this frame is already the one displayed (optional optimization)
         static std::map<std::string, double> displayed_pts; // Key by path now? Or texture ID? Assume texture ID for now.
         std::string path_key = video.format_ctx ? video.format_ctx->url : ""; // Get path if possible
         if (!path_key.empty() && displayed_pts.count(path_key) && displayed_pts[path_key] == best_frame->pts) {
             return true; // Already displaying this frame
         }

        // Upload the pixels to the texture
        glBindTexture(GL_TEXTURE_2D, video.texture_id);
        // Use glTexSubImage2D for better performance if texture dimensions match
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, best_frame->width, best_frame->height,
                        GL_RGB, GL_UNSIGNED_BYTE, best_frame->pixels.data());
        glBindTexture(GL_TEXTURE_2D, 0); // Unbind

        GLenum err = glGetError();
        if (err != GL_NO_ERROR) {
            std::cerr << "OpenGL error updating texture " << video.texture_id << " (" << path_key << ") from cache: " << err << std::endl;
            return false;
        }
        if (!path_key.empty()) displayed_pts[path_key] = best_frame->pts; // Mark as displayed

        // std::cout << "Updated texture " << video.texture_id << " (" << path_key << ") with frame PTS: " << best_frame->pts << "s (Target: " << target_media_time_seconds << "s, Diff: " << min_diff << ")" << std::endl;
        return true;
    } else {
        // std::cerr << "Warning: No suitable frame found in cache for " << target_media_time_seconds << "s in " << (video.format_ctx ? video.format_ctx->url : "Unknown") << std::endl;
    }

    return false; // No suitable frame found in cache
}

// --- Preview Update Orchestration ---
// Iterates through *visible nodes* at the current time
void update_video_previews(GLResources& res, const std::vector<std::shared_ptr<Node>>& root_nodes, float current_time) {

    float decode_ahead_time = 10.0f / 30.0f; // Example: 10 frames at 30fps

    // Recursive function to process nodes
    std::function<void(Node*)> process_node =
        [&](Node* node) {
        if (!node || !node->visible) return;

        float local_time = current_time - node->start_time;
        if (local_time < 0.0f || local_time >= node->duration) {
            // Node not active, but check children
             for (const auto& child : node->children) { process_node(child.get()); }
            return;
        }

        if (node->type == NodeType::Media) {
            MediaNode* media_node = static_cast<MediaNode*>(node);
            if (media_node->source_clip && media_node->source_clip->type == ClipType::Video) {
                const std::string& path = media_node->source_clip->path;
                auto it = res.video_cache.find(path);
                if (it != res.video_cache.end() && it->second.is_initialized) {
                    VideoData& video_data = it->second;

                    // Calculate the time within the source media file
                    float media_time = local_time + media_node->media_start;
                    media_time = std::max(0.0f, media_time); // Ensure non-negative
                    if(video_data.duration_sec > 0) {
                        media_time = std::min(media_time, (float)video_data.duration_sec); // Clamp to duration
                    }


                    // Ensure decoding covers current time + buffer
                    float target_decode_media_time = media_time + decode_ahead_time;
                     if(video_data.duration_sec > 0) {
                        target_decode_media_time = std::min(target_decode_media_time, (float)video_data.duration_sec);
                     }

                    if (ensure_video_decoded_upto(video_data, target_decode_media_time)) {
                        // Update texture with the best available frame for the *current* media time
                        update_texture_from_cache(video_data, media_time);
                    } else {
                       // std::cerr << "Error ensuring video decoded for " << path << " at media time " << target_decode_media_time << std::endl;
                    }
                }
            }
        }

        // Process children nodes recursively
        for (const auto& child : node->children) {
            process_node(child.get());
        }
    };

    // Start processing from root nodes
    for (const auto& root_node : root_nodes) {
        process_node(root_node.get());
    }
}


// --- Render Frame ---
// Renders nodes using existing textures. Does NO decoding here.
// Takes the node hierarchy and current time.
void render_frame(GLResources& res, float current_time,
    const std::vector<std::shared_ptr<Node>>& root_nodes,
    int width, int height) {

    glBindFramebuffer(GL_FRAMEBUFFER, res.fbo);
    glViewport(0, 0, width, height);
    glClearColor(0.1f, 0.1f, 0.1f, 1.0f); // Dark gray background
    glClear(GL_COLOR_BUFFER_BIT); // No depth buffer needed for 2D compositing usually

    glEnable(GL_TEXTURE_2D);
    glEnable(GL_BLEND);
    // Default blend mode, will be overridden by nodes
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    // Simple Ortho projection matching window/FBO dimensions
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    // Match projection to the output dimensions (width, height)
    // Origin at bottom-left is standard for OpenGL FBOs
    glOrtho(0.0, (double)width, 0.0, (double)height, -1.0, 1.0);


    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();

    // --- Node Rendering Loop ---
    // We need a sorted list or traversal order based on layers/hierarchy
    // Simple approach: Collect all nodes, sort by layer, then render.
    std::vector<Node*> nodes_to_render;
    std::function<void(Node*)> collect_nodes = [&](Node* node) {
        if (!node) return;
        nodes_to_render.push_back(node);
        for (const auto& child : node->children) {
            collect_nodes(child.get()); // Recursive collection
        }
    };

    for(const auto& root : root_nodes) {
        collect_nodes(root.get());
    }

    // Sort by layer primarily (lower layers first)
    std::sort(nodes_to_render.begin(), nodes_to_render.end(), [](const Node* a, const Node* b) {
        return a->layer < b->layer;
        // Could add secondary sort criteria if needed (e.g., start time)
    });

    // Render sorted nodes
    for (Node* node : nodes_to_render) {
        if (!node->visible) continue;

        float local_time = current_time - node->start_time;
        if (local_time < 0.0f || local_time >= node->duration) continue; // Node not active

        // Set Blending Mode (TODO: Refine blend mode mapping)
         switch (node->blend_mode) {
            case BlendMode::Normal:     glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA); break;
            case BlendMode::Additive:   glBlendFunc(GL_SRC_ALPHA, GL_ONE); break;
            case BlendMode::Multiply:   glBlendFunc(GL_DST_COLOR, GL_ONE_MINUS_SRC_ALPHA); break; // Modulate
            case BlendMode::Screen:     glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_COLOR); break;
            // Other modes might require shaders or different blend funcs
            default: glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA); break;
        }

        glPushMatrix(); // Save current matrix state

        // --- Apply Node Transforms (Relative to Parent - parent transform handling not shown here yet) ---
        // For root nodes, parent transform is identity. For children, multiply by parent's matrix.
        // This requires passing down the transform matrix during traversal or calculating world transform.
        // Simplified version assuming root nodes for now:

        // 1. Evaluate properties at local_time
        float eval_pos_x = node->EvaluatePosX(local_time);
        float eval_pos_y = node->EvaluatePosY(local_time);
        float eval_scale = node->EvaluateScale(local_time);
        float eval_rotation = node->EvaluateRotation(local_time);
        float eval_opacity = node->EvaluateOpacity(local_time);

        // 2. Get Node/Media Dimensions
        float node_width = (float)width; // Default to output width
        float node_height = (float)height; // Default to output height
        GLuint tex_id = 0;
        float tex_aspect = 1.0f;
        ImVec2 tex_coords_tl = ImVec2(0,0); // Top-Left UV
        ImVec2 tex_coords_br = ImVec2(1,1); // Bottom-Right UV

        if (node->type == NodeType::Media) {
            MediaNode* media_node = static_cast<MediaNode*>(node);
            if (media_node->source_clip) {
                const Clip* source = media_node->source_clip;
                 // Use source dimensions if available
                if (source->source_width > 0 && source->source_height > 0) {
                    node_width = (float)source->source_width;
                    node_height = (float)source->source_height;
                    tex_aspect = node_width / node_height;
                }

                // Find the texture ID from GLResources cache based on source path
                if (source->type == ClipType::Video) {
                    auto vid_it = res.video_cache.find(source->path);
                    if (vid_it != res.video_cache.end() && vid_it->second.is_initialized) {
                        tex_id = vid_it->second.texture_id;
                        // Update dimensions from video data if needed
                        node_width = (float)vid_it->second.width;
                        node_height = (float)vid_it->second.height;
                        if(node_height > 0) tex_aspect = node_width / node_height;

                    }
                } else if (source->type == ClipType::Image) {
                    auto img_it = res.texture_cache.find(source->path);
                    if (img_it != res.texture_cache.end()) {
                        tex_id = img_it->second;
                         // Query texture size (or use cached size if stored)
                         glBindTexture(GL_TEXTURE_2D, tex_id);
                         int w, h;
                         glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &w);
                         glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &h);
                         glBindTexture(GL_TEXTURE_2D, 0);
                         node_width = (float)w; node_height = (float)h;
                         if(node_height > 0) tex_aspect = node_width / node_height;
                    }
                }
                 // Note: Texture coordinates might need adjustment based on media_start/duration
                 // for effects like sprite sheets, but for simple video/image, use full texture (0,0 to 1,1).
                 // OpenGL textures are typically sampled with Y=0 at bottom, Y=1 at top.
                 // If image loading (SDL_image) or video decoding gives top-left origin data,
                 // flip the V coordinate: (0,1) top-left, (1,0) bottom-right.
                 tex_coords_tl = ImVec2(0, 1); // Top-Left
                 tex_coords_br = ImVec2(1, 0); // Bottom-Right

            }
        } else if (node->type == NodeType::Group) {
            // Group nodes don't have their own texture, they just apply transforms
            // Their rendering comes from their children
            // Need to handle transform application correctly for children later
        }


        // 3. Apply Transforms (Example using immediate mode)
        // Center of the screen/FBO is (width/2, height/2)
        // Translate origin to center, apply node transforms, translate back, then draw centered quad
        glTranslatef(width / 2.0f, height / 2.0f, 0.0f); // Move origin to center

        // Apply node's translation (relative to center)
        // Assuming node pos_x/y are normalized (-1 to 1 maps to -width/2 to width/2 etc)
        glTranslatef(eval_pos_x * width * 0.5f, eval_pos_y * height * 0.5f, 0.0f);

        // Apply rotation around the node's center (which is now at the origin)
        glRotatef(eval_rotation, 0.0f, 0.0f, 1.0f);

        // Apply uniform scale
        glScalef(eval_scale, eval_scale, 1.0f);

        // --- Render the Quad if texture exists ---
        if (tex_id != 0) {
            glBindTexture(GL_TEXTURE_2D, tex_id);
            glColor4f(1.0f, 1.0f, 1.0f, eval_opacity); // Apply evaluated opacity

            // Draw a quad centered at the current origin, with size matching the media
            float half_w = node_width / 2.0f;
            float half_h = node_height / 2.0f;

            glBegin(GL_QUADS);
                glTexCoord2f(tex_coords_tl.x, tex_coords_tl.y); glVertex2f(-half_w,  half_h); // Top Left
                glTexCoord2f(tex_coords_br.x, tex_coords_tl.y); glVertex2f( half_w,  half_h); // Top Right
                glTexCoord2f(tex_coords_br.x, tex_coords_br.y); glVertex2f( half_w, -half_h); // Bottom Right
                glTexCoord2f(tex_coords_tl.x, tex_coords_br.y); glVertex2f(-half_w, -half_h); // Bottom Left
            glEnd();

            glBindTexture(GL_TEXTURE_2D, 0); // Unbind texture
        }
         // If it's a GroupNode, its children will be rendered relative to this transform state.
         // If rendering children is handled within Node::Render, this simplified loop might
         // need adjustment or a full recursive render function. Let's assume Node::Render handles it.

        glPopMatrix(); // Restore matrix state for the next node
    }


    // Reset color and disable states
    glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
    glDisable(GL_BLEND);
    glDisable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

}


// --- Cleanup ---
void cleanup_video_resources(GLResources& res) {
    std::cout << "Cleaning up video resources..." << std::endl;
    for (auto& [path, video] : res.video_cache) {
        if (video.is_initialized) {
            std::cout << "  Cleaning FFmpeg context for: " << path << std::endl;
            av_packet_free(&video.packet);
            sws_freeContext(video.sws_ctx);
            av_frame_free(&video.frame);
            avcodec_free_context(&video.codec_ctx);
            avformat_close_input(&video.format_ctx);
            video.is_initialized = false; // Mark as cleaned FFmpeg wise
        }
        // GL texture for video is cleaned in cleanup_gl_resources
    }
     res.video_cache.clear(); // Clear the map itself
}

void cleanup_gl_resources(GLResources& res) {
    std::cout << "Cleaning up GL resources..." << std::endl;
    if (res.fbo) {
        glDeleteFramebuffers(1, &res.fbo);
        res.fbo = 0;
    }
    if (res.render_tex) {
        glDeleteTextures(1, &res.render_tex);
        res.render_tex = 0;
    }
     // Clean image textures
    std::cout << "Cleaning up image textures..." << std::endl;
    for (auto const& [path, tex_id] : res.texture_cache) {
         std::cout << "  Deleting image texture: " << path << " (ID: " << tex_id << ")" << std::endl;
        glDeleteTextures(1, &tex_id);
    }
    res.texture_cache.clear();

    // Clean up video textures
    std::cout << "Cleaning up video textures..." << std::endl;
     for (auto const& [path, video_data] : res.video_cache) {
         if (video_data.texture_id) {
             std::cout << "  Deleting video texture: " << path << " (ID: " << video_data.texture_id << ")" << std::endl;
            glDeleteTextures(1, &video_data.texture_id);
            // No need to modify video_data here as the cache is cleared next
        }
    }
    // Video cache map itself is cleared in cleanup_video_resources, call after this

    // Clean up thumbnail textures
    std::cout << "Cleaning up thumbnail textures..." << std::endl;
    for (auto& [path, tex_vec] : res.clip_thumbnail_textures) {
        for (GLuint tex_id : tex_vec) {
             if (tex_id) {
                // std::cout << "  Deleting thumbnail texture for: " << path << " (ID: " << tex_id << ")" << std::endl;
                glDeleteTextures(1, &tex_id);
             }
        }
    }
    res.clip_thumbnail_textures.clear();
    res.generated_thumbnails_map.clear();

    std::cout << "GL resources cleaned." << std::endl;
}


// --- Audio Preloading (Keep as is, operates on path) ---
bool preload_audio_file(const std::string& path, PreloadedAudio& out, float media_start, float media_duration) {
    AVFormatContext* fmt_ctx = nullptr;
    if (avformat_open_input(&fmt_ctx, path.c_str(), nullptr, nullptr) < 0) { /* error */ return false; }
    if (avformat_find_stream_info(fmt_ctx, nullptr) < 0) { /* error */ avformat_close_input(&fmt_ctx); return false; }
    int stream_index = av_find_best_stream(fmt_ctx, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    if (stream_index < 0) { /* no audio */ avformat_close_input(&fmt_ctx); return false; }

    AVCodecParameters* params = fmt_ctx->streams[stream_index]->codecpar;
    const AVCodec* decoder = avcodec_find_decoder(params->codec_id);
    if (!decoder) { /* error */ avformat_close_input(&fmt_ctx); return false; }

    AVCodecContext* codec_ctx = avcodec_alloc_context3(decoder);
    if (!codec_ctx) { /* error */ avformat_close_input(&fmt_ctx); return false; }
    if (avcodec_parameters_to_context(codec_ctx, params) < 0) { /* error */ avcodec_free_context(&codec_ctx); avformat_close_input(&fmt_ctx); return false; }
    if (avcodec_open2(codec_ctx, decoder, nullptr) < 0) { /* error */ avcodec_free_context(&codec_ctx); avformat_close_input(&fmt_ctx); return false; }

    // --- Channel Layout Handling ---
    AVChannelLayout out_ch_layout = AV_CHANNEL_LAYOUT_STEREO; // Target layout

    AVChannelLayout in_ch_layout;
    av_channel_layout_default(&in_ch_layout, 0); // Initialize with default layout for 0 channels
    av_channel_layout_uninit(&in_ch_layout); // Ensure clean state

    // Try copying the layout from the input parameters
    int layout_ret = av_channel_layout_copy(&in_ch_layout, &params->ch_layout);

    // If copy failed or resulted in invalid layout, try fallback
    if (layout_ret < 0 || in_ch_layout.nb_channels <= 0) {
        std::cerr << "Warning: Invalid or uncopyable input channel layout for " << path << ". Guessing based on channel count (" << params->ch_layout.nb_channels << ")." << std::endl;

        // Use static const layouts to copy FROM
        static const AVChannelLayout static_layout_mono = AV_CHANNEL_LAYOUT_MONO;
        static const AVChannelLayout static_layout_stereo = AV_CHANNEL_LAYOUT_STEREO;

        if (params->ch_layout.nb_channels == 1) {
            av_channel_layout_copy(&in_ch_layout, &static_layout_mono);
        } else { // Default to stereo for 2 or more/unknown channels
            av_channel_layout_copy(&in_ch_layout, &static_layout_stereo);
            if (params->ch_layout.nb_channels != 2) {
                 std::cerr << "Warning: Defaulting to Stereo layout for " << params->ch_layout.nb_channels << " channels." << std::endl;
            }
        }
         // Check again if the fallback copy worked
         if (in_ch_layout.nb_channels <= 0) {
             std::cerr << "Error: Failed to set a valid input channel layout even with fallback for " << path << std::endl;
             av_channel_layout_uninit(&in_ch_layout);
             avcodec_free_context(&codec_ctx);
             avformat_close_input(&fmt_ctx);
             return false;
         }
    }
    // --- End Channel Layout Handling ---

    SwrContext* swr = nullptr;
    int swr_ret = swr_alloc_set_opts2(&swr,
                                      &out_ch_layout, AV_SAMPLE_FMT_S16, 44100,
                                      &in_ch_layout, (AVSampleFormat)params->format, params->sample_rate,
                                      0, nullptr);

    if (swr_ret < 0 || !swr || swr_init(swr) < 0) {
        std::cerr << "Failed to create or init SwrContext for " << path << std::endl;
        av_channel_layout_uninit(&in_ch_layout); // Cleanup allocated layout memory
        avcodec_free_context(&codec_ctx);
        avformat_close_input(&fmt_ctx);
        swr_free(&swr);
        return false;
    }

    // Seek logic (if needed)
    if (media_start > 0.0f) {
        int64_t seek_target_ts = av_rescale_q(static_cast<int64_t>(media_start * AV_TIME_BASE),
                                              AV_TIME_BASE_Q, fmt_ctx->streams[stream_index]->time_base);
        if (av_seek_frame(fmt_ctx, stream_index, seek_target_ts, AVSEEK_FLAG_BACKWARD) < 0) {
            std::cerr << "Warning: Failed to seek audio in " << path << " to " << media_start << "s" << std::endl;
        }
        avcodec_flush_buffers(codec_ctx); // Flush after seek
    }

    AVPacket* pkt = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    if (!pkt || !frame) { /* error handling */ /* cleanup */ return false; }

    std::vector<int16_t> full_audio;
    double current_time_sec = 0.0;
    double time_base_sec = av_q2d(fmt_ctx->streams[stream_index]->time_base);
    double target_end_sec = (media_duration > 0.0f) ? (media_start + media_duration) : std::numeric_limits<double>::max();

    // --- Decoding Loop ---
    while (av_read_frame(fmt_ctx, pkt) >= 0) {
        if (pkt->stream_index != stream_index) { av_packet_unref(pkt); continue; }

        int send_ret = avcodec_send_packet(codec_ctx, pkt);
        av_packet_unref(pkt); // Always unref packet
        if (send_ret < 0) { /* error handling */ break; }

        while (true) {
            int recv_ret = avcodec_receive_frame(codec_ctx, frame);
            if (recv_ret == AVERROR(EAGAIN) || recv_ret == AVERROR_EOF) break; // Need more input or EOF
            if (recv_ret < 0) { /* error handling */ goto done_preload; } // Error receiving

            double pts_sec = (frame->pts != AV_NOPTS_VALUE) ? frame->pts * time_base_sec : current_time_sec;

            if (pts_sec < media_start) { av_frame_unref(frame); continue; } // Skip frames before start
            if (pts_sec >= target_end_sec) { av_frame_unref(frame); goto done_preload; } // Reached end duration

            // Estimate output samples (can be generous)
            int estimated_out_samples = av_rescale_rnd(swr_get_delay(swr, params->sample_rate) + frame->nb_samples,
                                                     44100, params->sample_rate, AV_ROUND_UP);
            const int buffer_size = estimated_out_samples * out_ch_layout.nb_channels * av_get_bytes_per_sample(AV_SAMPLE_FMT_S16);
            if (buffer_size <= 0) { av_frame_unref(frame); continue; } // Avoid invalid buffer size

            uint8_t* out_buf[1];
            out_buf[0] = (uint8_t*)av_malloc(buffer_size);
            if (!out_buf[0]) { /* memory error */ av_frame_unref(frame); goto done_preload; }

            int actual_out_samples = swr_convert(swr, out_buf, estimated_out_samples,
                                                (const uint8_t**)frame->data, frame->nb_samples);

            if (actual_out_samples > 0) {
                full_audio.insert(full_audio.end(), (int16_t*)out_buf[0], (int16_t*)out_buf[0] + actual_out_samples * out_ch_layout.nb_channels);
            }
            av_free(out_buf[0]);
            av_frame_unref(frame); // Unref frame after use

            // Update time tracking (approximation is okay here)
            current_time_sec = pts_sec + (double)frame->nb_samples / params->sample_rate;
        }
    }

done_preload:
    // --- Flush Decoder ---
    avcodec_send_packet(codec_ctx, nullptr); // Send flush packet
    while (true) {
        int flush_ret = avcodec_receive_frame(codec_ctx, frame);
        if (flush_ret == AVERROR(EAGAIN) || flush_ret == AVERROR_EOF) break;
        if (flush_ret < 0) break; // Error flushing

        // Check pts against target_end_sec even during flush
        double pts_sec = (frame->pts != AV_NOPTS_VALUE) ? frame->pts * time_base_sec : current_time_sec;
        if (pts_sec >= target_end_sec && media_duration > 0.0f) { av_frame_unref(frame); continue; }

        int estimated_out_samples = av_rescale_rnd(frame->nb_samples, 44100, params->sample_rate, AV_ROUND_UP);
        const int buffer_size = estimated_out_samples * out_ch_layout.nb_channels * av_get_bytes_per_sample(AV_SAMPLE_FMT_S16);
        if (buffer_size <= 0) { av_frame_unref(frame); continue; }

        uint8_t* out_buf[1];
        out_buf[0] = (uint8_t*)av_malloc(buffer_size);
        if (!out_buf[0]) { av_frame_unref(frame); break; } // Error

        int actual_out_samples = swr_convert(swr, out_buf, estimated_out_samples,
                                            (const uint8_t**)frame->data, frame->nb_samples);
        if (actual_out_samples > 0) {
            full_audio.insert(full_audio.end(), (int16_t*)out_buf[0], (int16_t*)out_buf[0] + actual_out_samples * out_ch_layout.nb_channels);
        }
        av_free(out_buf[0]);
        av_frame_unref(frame);
    }
    // --- End Flush ---

    out.samples = std::move(full_audio);
    out.sample_rate = 44100;
    out.channels = out_ch_layout.nb_channels; // Use actual output channels
    out.duration = (float)out.samples.size() / (float)std::max(1, out.channels * out.sample_rate); // Prevent division by zero
    out.waveform = GenerateWaveformPreview(out.samples, out.channels, 256);

    // Cleanup
    av_frame_free(&frame);
    av_packet_free(&pkt);
    swr_free(&swr);
    avcodec_free_context(&codec_ctx);
    av_channel_layout_uninit(&in_ch_layout); // Cleanup copied layout
    // No need to uninit out_ch_layout if it's static
    avformat_close_input(&fmt_ctx);
    return !out.samples.empty(); // Return true if some audio was loaded
}

// --- Mix Audio (Keep as is, operates on PreloadedAudio) ---
void mix_audio_from_memory(const PreloadedAudio& audio, float time_sec, std::vector<float>& mix_buffer) {
    int start_sample = static_cast<int>(time_sec * audio.sample_rate);
    int sample_count = mix_buffer.size();

    for (int i = 0; i < sample_count; ++i) {
        int idx = (start_sample + i);
        if (idx * audio.channels + 1 >= (int)audio.samples.size())
            break;

        for (int ch = 0; ch < audio.channels; ++ch) {
            int16_t sample = audio.samples[idx * audio.channels + ch];
            mix_buffer[i * audio.channels + ch] += sample / 32768.0f;
        }
    }
}


// --- Video Export ---
// Needs significant update to use Nodes instead of Clips
bool start_video_export(const std::string& output_path, int width, int height, int fps,
    float total_duration_sec, // Use total duration instead of frames
    const std::vector<std::shared_ptr<Node>>& root_nodes, // Use nodes
    const std::map<std::string, Clip>& media_library, // Pass media library
    SDL_Window* window) {

    std::cout << "Starting video export (using glReadPixels method)..." << std::endl;
    SDL_GLContext current_context = SDL_GL_GetCurrentContext();
    if (!current_context) { /* error */ return false; }

    // Prepare resources specifically for export resolution
    GLResources export_res; // Use a separate resource cache for export? Or main one?
                            // Using separate avoids polluting main cache, but duplicates loading.
                            // Let's use a separate one for clarity.
    if (!setup_gl_resources(export_res, width, height)) {
        std::cerr << "Failed to setup GL resources for export." << std::endl;
        return false;
    }

    // Load all necessary *source* resources referenced by the nodes
    std::function<void(Node*)> load_node_resources =
        [&](Node* node) {
        if (!node) return;
        if (node->type == NodeType::Media) {
            MediaNode* mn = static_cast<MediaNode*>(node);
            if (mn->source_clip) {
                // Find the clip in the main library (passed in)
                auto lib_it = media_library.find(mn->source_clip->path);
                if (lib_it != media_library.end()) {
                     // Load resource into the *export* cache using the library clip info
                     Clip temp_clip_copy = lib_it->second; // Make a copy to pass
                     load_resources_for_clip(export_res, temp_clip_copy); // Load into export_res
                }
            }
        }
        for (const auto& child : node->children) {
            load_node_resources(child.get());
        }
    };

    for (const auto& root : root_nodes) {
        load_node_resources(root.get());
    }


    // Open raw output files
    std::ofstream video_file("video.raw", std::ios::binary);
    std::ofstream audio_file("temp_audio.raw", std::ios::binary);
    if (!video_file || !audio_file) { /* error */ cleanup_gl_resources(export_res); cleanup_video_resources(export_res); return false; }


    std::vector<uint8_t> pixels(width * height * 3); // RGB
    const int audio_sample_rate = 44100;
    const int audio_channels = 2; // Stereo output
    const int samples_per_frame = audio_sample_rate / fps;
    std::vector<float> audio_float_mix_buffer(samples_per_frame * audio_channels, 0.0f);
    std::vector<int16_t> audio_s16_output_buffer(samples_per_frame * audio_channels);

    int total_frames = static_cast<int>(std::ceil(total_duration_sec * fps));

    // --- Render and Export Loop ---
    for (int frame_idx = 0; frame_idx < total_frames; ++frame_idx) {
        float current_time = static_cast<float>(frame_idx) / fps;

        // 1. Update video textures for nodes active around this frame time
        // Need to pass the export_res cache here
        update_video_previews(export_res, root_nodes, current_time);


        // 2. Render the composited frame to FBO using nodes
        // Pass the export_res cache here
        render_frame(export_res, current_time, root_nodes, width, height);

        // 3. Read back pixels (Slow part)
        glBindFramebuffer(GL_FRAMEBUFFER, export_res.fbo);
        glReadBuffer(GL_COLOR_ATTACHMENT0);
        glReadPixels(0, 0, width, height, GL_RGB, GL_UNSIGNED_BYTE, pixels.data());
        glBindFramebuffer(GL_FRAMEBUFFER, 0);

        // Flip vertically if needed (OpenGL FBO is bottom-left, FFmpeg raw expects top-left)
        // Re-enable this if FFmpeg input requires top-left origin raw frames
         size_t row_bytes = width * 3;
         std::vector<uint8_t> row_buffer(row_bytes);
         for (int y = 0; y < height / 2; ++y) {
             uint8_t* top_row = pixels.data() + y * row_bytes;
             uint8_t* bottom_row = pixels.data() + (height - 1 - y) * row_bytes;
             memcpy(row_buffer.data(), top_row, row_bytes);
             memcpy(top_row, bottom_row, row_bytes);
             memcpy(bottom_row, row_buffer.data(), row_bytes);
         }

        // Write video frame
        video_file.write(reinterpret_cast<const char*>(pixels.data()), pixels.size());


        // 4. Mix Audio from active Audio MediaNodes
        std::fill(audio_float_mix_buffer.begin(), audio_float_mix_buffer.end(), 0.0f);

        std::function<void(Node*)> mix_audio_node =
            [&](Node* node) {
            if (!node || !node->visible) return;

            float local_time = current_time - node->start_time;
            if (local_time < 0.0f || local_time >= node->duration) {
                 for (const auto& child : node->children) mix_audio_node(child.get());
                 return; // Node not active
            }

            if (node->type == NodeType::Media) {
                MediaNode* media_node = static_cast<MediaNode*>(node);
                // Check if it's designated as audio OR if source clip has audio
                if (media_node->is_audio || (media_node->source_clip && media_node->source_clip->has_audio)) {
                    if (media_node->source_clip) {
                        // Find the preloaded audio data in the *export* cache
                        auto audio_it = export_res.preloaded_audio.find(media_node->source_clip->path);
                        if (audio_it != export_res.preloaded_audio.end()) {
                            const PreloadedAudio& audio_data = audio_it->second;
                            if (audio_data.sample_rate == audio_sample_rate && audio_data.channels > 0 && !audio_data.samples.empty()) {
                                // Calculate the starting sample index within the preloaded audio buffer
                                float time_in_media = local_time + media_node->media_start;
                                time_in_media = std::max(0.0f, time_in_media); // Clamp start
                                int start_sample_in_source = static_cast<int>(time_in_media * audio_data.sample_rate);

                                // Mix samples for this frame
                                for (int i = 0; i < samples_per_frame; ++i) {
                                    int source_sample_index = start_sample_in_source + i;
                                    // Check bounds for the source audio samples
                                    size_t source_s16_index_ch0 = (size_t)source_sample_index * audio_data.channels;
                                    if (source_s16_index_ch0 + (audio_data.channels - 1) < audio_data.samples.size()) {
                                         // TODO: Apply node opacity/volume factor here?
                                         float volume = media_node->EvaluateOpacity(local_time); // Use opacity as volume for now

                                         // Mix into stereo output buffer
                                        for (int out_ch = 0; out_ch < audio_channels; ++out_ch) {
                                            int source_ch = (audio_data.channels == 1) ? 0 : out_ch % audio_data.channels; // Handle mono -> stereo
                                            int16_t sample_s16 = audio_data.samples[source_s16_index_ch0 + source_ch];
                                            // Add to float mix buffer, applying volume
                                            audio_float_mix_buffer[i * audio_channels + out_ch] += (static_cast<float>(sample_s16) / 32768.0f) * volume;
                                        }
                                    } else {
                                        // Ran out of source samples for this node instance
                                        break;
                                    }
                                }
                            }
                        }
                    }
                }
            }
             // Recurse for children
             for (const auto& child : node->children) mix_audio_node(child.get());
        };

        for (const auto& root : root_nodes) {
            mix_audio_node(root.get());
        }


        // Clamp and convert final mix buffer float audio to S16
        for (size_t i = 0; i < audio_float_mix_buffer.size(); ++i) {
            float sample_f = std::clamp(audio_float_mix_buffer[i], -1.0f, 1.0f); // Clamp [-1, 1]
            audio_s16_output_buffer[i] = static_cast<int16_t>(sample_f * 32767.0f);
        }
        audio_file.write(reinterpret_cast<const char*>(audio_s16_output_buffer.data()), audio_s16_output_buffer.size() * sizeof(int16_t));


        // Progress update
        if (frame_idx % 30 == 0 || frame_idx == total_frames - 1) {
            std::cout << "Export progress: Frame " << frame_idx + 1 << "/" << total_frames << " (Time: " << current_time << "s)" << std::endl;
        }
    }

    video_file.close();
    audio_file.close();

    // --- FFmpeg Command (remains similar) ---
    std::string ffmpeg_cmd =
        "ffmpeg -y "
        "-f rawvideo -pix_fmt rgb24 -s " + std::to_string(width) + "x" + std::to_string(height) +
        " -r " + std::to_string(fps) + " -i video.raw "
        "-f s16le -ac " + std::to_string(audio_channels) + " -ar " + std::to_string(audio_sample_rate) + " -i temp_audio.raw "
        "-c:v libx264 -preset fast -crf 22 -pix_fmt yuv420p " // Ensure output format is widely compatible
        "-c:a aac -b:a 192k "
        "-shortest \"" + output_path + "\""; // Use shortest if inputs might have slightly different effective lengths

    std::cout << "Running FFmpeg command:\n" << ffmpeg_cmd << std::endl;
    int ret = system(ffmpeg_cmd.c_str());

    // Cleanup temporary files and export resources
    std::filesystem::remove("video.raw");
    std::filesystem::remove("temp_audio.raw");
    cleanup_gl_resources(export_res);
    cleanup_video_resources(export_res);

    if (ret != 0) {
        std::cerr << "FFmpeg export failed with code " << ret << std::endl;
        return false;
    }

    std::cout << "Video export finished successfully." << std::endl;
    return true;
}


// Generate Waveform (Keep as is)
std::vector<float> GenerateWaveformPreview(const std::vector<int16_t>& samples, int channels, int samples_per_pixel) {
    std::vector<float> waveform;
    int total_samples = samples.size() / channels;

    for (int i = 0; i < total_samples; i += samples_per_pixel) {
        float max_amp = 0.0f;
        for (int j = 0; j < samples_per_pixel && (i + j) < total_samples; ++j) {
            for (int ch = 0; ch < channels; ++ch) {
                float sample = samples[(i + j) * channels + ch] / 32768.0f;
                max_amp = std::max(max_amp, std::abs(sample));
            }
        }
        waveform.push_back(max_amp);
    }

    return waveform;
}

// --- Thumbnail Generation ---
// Queue based on *source clip* path and duration
void QueueClipThumbnails(GLResources& res, const Clip& clip) { // Takes source Clip
     // Only queue for Video or Image types that have a path
     if ((clip.type != ClipType::Video && clip.type != ClipType::Image) || clip.path.empty()) {
         return;
     }

    // Check if thumbnails are already being generated or are done for this clip path
    if (res.clip_thumbnail_textures.count(clip.path)) {
        return; // Already processed or processing started
    }

    // Determine number of thumbs and interval based on source duration (if video)
    int max_thumbs = 10; // Default for images or short videos
    float interval = 0.5f; // Default interval
    float thumb_duration = clip.source_duration;

    if (clip.type == ClipType::Video && thumb_duration > 1.0f) {
         max_thumbs = std::max(1, std::min(50, static_cast<int>(thumb_duration))); // More thumbs for longer videos, capped
         interval = thumb_duration / static_cast<float>(max_thumbs + 1); // Spread out thumbs
         interval = std::max(0.25f, interval); // Minimum interval
    } else if (clip.type == ClipType::Image) {
        max_thumbs = 1; // Only need one thumbnail for images
        interval = 0;
        thumb_duration = 0; // Not applicable
    }


    // Create placeholder entry in the map immediately
    res.clip_thumbnail_textures[clip.path] = {}; // Empty vector initially
    res.generated_thumbnails_map[clip.path] = {}; // Empty map for generated timestamps


    { // Lock the request queue
        std::lock_guard<std::mutex> lock(request_mutex);
        for (int i = 0; i < max_thumbs; ++i) {
            // Timestamp is relative to the start of the source media (media_start is a node property)
            float timestamp = (i + 0.5f) * interval; // Use midpoint of interval

            // Clamp timestamp for videos
            if (clip.type == ClipType::Video && thumb_duration > 0) {
                 timestamp = std::min(timestamp, thumb_duration - 0.05f); // Ensure slightly before end
            }
            timestamp = std::max(0.0f, timestamp); // Ensure non-negative

            ThumbnailRequest req;
            req.clip_path = clip.path;
            req.timestamp = (clip.type == ClipType::Image) ? 0.0f : timestamp; // Timestamp 0 for images
            thumbnail_request_queue.push(req);
             // std::cout << "Queued thumb request for " << req.clip_path << " at " << req.timestamp << std::endl;
        }
    } // Unlock request_mutex

    worker_cv.notify_one(); // Signal the worker thread
}


// Process Thumbnail Results (Keep mostly as is, operates on GLResources cache)
void ProcessThumbnailResults(GLResources& res, int max_per_frame) {
    // ... (implementation largely unchanged)
    // Ensure it correctly stores texture ID in res.clip_thumbnail_textures[result.clip_path]
    // And updates res.generated_thumbnails_map[result.clip_path]
     for (int i = 0; i < max_per_frame; ++i) {
        ThumbnailResult result;
        { // Scope for lock
            std::lock_guard<std::mutex> lock(result_mutex);
            if (thumbnail_result_queue.empty()) break;
            result = std::move(thumbnail_result_queue.front());
            thumbnail_result_queue.pop();
        } // Lock released

        if (!result.success || result.pixels.empty()) {
             std::cerr << "Thumbnail gen failed for " << result.clip_path << " at " << result.timestamp << ": " << result.error_message << std::endl;
             // Maybe add a "failed" placeholder texture to the map? For now, just skip.
             continue;
        }

        // Check if main GLResources still exists and path is relevant
        if (!res.clip_thumbnail_textures.count(result.clip_path)) {
             std::cerr << "Warning: Received thumbnail for unknown/removed path: " << result.clip_path << std::endl;
             continue; // Skip if the clip was removed from the library
        }

        // Check if this specific timestamp was already generated (e.g., duplicate request)
        auto& gen_map = res.generated_thumbnails_map[result.clip_path];
        if (gen_map.count(result.timestamp)) {
             // std::cout << "Skipping duplicate thumbnail result for " << result.clip_path << " at " << result.timestamp << std::endl;
             continue;
        }

        // Create OpenGL Texture on Main Thread
        GLuint tex_id = 0;
        glGenTextures(1, &tex_id);
        if (tex_id == 0) {
             std::cerr << "Failed to generate texture ID for thumbnail!" << std::endl;
             continue;
        }
        glBindTexture(GL_TEXTURE_2D, tex_id);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, result.width, result.height, 0, GL_RGB, GL_UNSIGNED_BYTE, result.pixels.data());

        GLenum err = glGetError();
        if (err != GL_NO_ERROR) {
            std::cerr << "OpenGL error creating thumbnail texture for " << result.clip_path << ": " << err << std::endl;
            glDeleteTextures(1, &tex_id);
            continue;
        }

        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glBindTexture(GL_TEXTURE_2D, 0);

        // Store texture ID and mark timestamp as generated
        res.clip_thumbnail_textures[result.clip_path].push_back(tex_id);
        gen_map[result.timestamp] = tex_id;
        // std::cout << "Generated thumbnail texture " << tex_id << " for " << result.clip_path << " at " << result.timestamp << std::endl;

    } // end loop
}

// Thumbnail Worker Func (Keep mostly as is, operates on path)
void thumbnail_worker_func() {
    // ... (implementation mostly unchanged, takes path and timestamp)
    // Ensure it handles image files correctly (e.g., load frame 0 or handle separately)
    std::cout << "Thumbnail worker thread started." << std::endl;
    while (!stop_thumbnail_worker_flag) {
        ThumbnailRequest request;
        { // Scope for lock
            std::unique_lock<std::mutex> lock(request_mutex);
            worker_cv.wait(lock, [&] { return !thumbnail_request_queue.empty() || stop_thumbnail_worker_flag; });
            if (stop_thumbnail_worker_flag) break;
            if (thumbnail_request_queue.empty()) continue; // Spurious wakeup
            request = thumbnail_request_queue.front();
            thumbnail_request_queue.pop();
        } // Lock released

        // --- Process the request ---
        ThumbnailResult result;
        result.clip_path = request.clip_path;
        result.timestamp = request.timestamp;
        result.width = THUMBNAIL_WIDTH; // Defined in video_export.hpp
        result.height = THUMBNAIL_HEIGHT; // Defined in video_export.hpp
        result.success = false;

        // --- Handle Images using SDL_image (simpler) ---
        if (is_image_file(request.clip_path)) {
             SDL_Surface* surface = IMG_Load(request.clip_path.c_str());
             if (!surface) {
                 result.error_message = "IMG_Load failed: " + std::string(SDL_GetError());
             } else {
                 SDL_Surface* rgb_surface = SDL_ConvertSurface(surface, SDL_PIXELFORMAT_RGB24);
                 if (!rgb_surface) {
                     result.error_message = "SDL_ConvertSurfaceFormat failed: " + std::string(SDL_GetError());
                 } else {
                     // --- Resize using a simple method (or SwsScale if preferred) ---
                     // Option 1: Simple manual resize (Nearest neighbor - fast but low quality)
                     // Option 2: Use FFmpeg's SwsScale even for images
                     // Let's use SwsScale for consistency

                     SwsContext* sws_ctx_img = sws_getContext(
                         rgb_surface->w, rgb_surface->h, AV_PIX_FMT_RGB24, // Input
                         result.width, result.height, AV_PIX_FMT_RGB24, // Output
                         SWS_BILINEAR, nullptr, nullptr, nullptr);

                     if (!sws_ctx_img) {
                         result.error_message = "Failed to create SwsContext for image resize.";
                     } else {
                         result.pixels.resize(result.width * result.height * 3);
                         uint8_t* dst_data[1] = { result.pixels.data() };
                         int dst_linesize[1] = { result.width * 3 };
                         const uint8_t* src_data[1] = { (uint8_t*)rgb_surface->pixels };
                         int src_linesize[1] = { rgb_surface->pitch };

                         sws_scale(sws_ctx_img, src_data, src_linesize, 0, rgb_surface->h, dst_data, dst_linesize);
                         result.success = true;
                         sws_freeContext(sws_ctx_img);
                     }
                     SDL_DestroySurface(rgb_surface);
                 }
                 SDL_DestroySurface(surface);
             }
        }
        // --- Handle Videos using FFmpeg (existing logic) ---
        else if (is_video_file(request.clip_path)) {
            AVFormatContext* fmt_ctx = nullptr; /* ... */
            AVCodecContext* codec_ctx = nullptr; /* ... */
            AVFrame* frame = nullptr; /* ... */
            AVFrame* rgb_frame = nullptr; /* ... */
            AVPacket* packet = nullptr; /* ... */
            SwsContext* sws_ctx = nullptr; /* ... */
            int video_stream_idx = -1; /* ... */
            uint8_t* buffer = nullptr; /* ... */

            try {
                // --- FFmpeg Setup (Open, Find Stream, Codec) ---
                if (avformat_open_input(&fmt_ctx, request.clip_path.c_str(), nullptr, nullptr) != 0) throw std::runtime_error("avformat_open_input failed");
                if (avformat_find_stream_info(fmt_ctx, nullptr) < 0) throw std::runtime_error("avformat_find_stream_info failed");
                video_stream_idx = av_find_best_stream(fmt_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
                if (video_stream_idx < 0) throw std::runtime_error("No video stream found");
                // ... (Codec setup as before) ...
                 AVCodecParameters* codec_params = fmt_ctx->streams[video_stream_idx]->codecpar;
                 const AVCodec* codec = avcodec_find_decoder(codec_params->codec_id);
                 if (!codec) throw std::runtime_error("Unsupported codec");
                 codec_ctx = avcodec_alloc_context3(codec);
                 if (!codec_ctx || avcodec_parameters_to_context(codec_ctx, codec_params) < 0 || avcodec_open2(codec_ctx, codec, nullptr) < 0) throw std::runtime_error("Codec context setup failed");


                AVRational time_base = fmt_ctx->streams[video_stream_idx]->time_base;
                 if (time_base.den == 0) time_base = {1, AV_TIME_BASE}; // Fallback timebase

                // --- Seek ---
                int64_t target_ts = av_rescale_q(static_cast<int64_t>(request.timestamp * AV_TIME_BASE), AV_TIME_BASE_Q, time_base);
                if (av_seek_frame(fmt_ctx, video_stream_idx, target_ts, AVSEEK_FLAG_BACKWARD) < 0) {
                    std::cerr << "Warning: Seek failed for thumb " << request.clip_path << " at " << request.timestamp << "s. Trying from start." << std::endl;
                }
                avcodec_flush_buffers(codec_ctx);

                // --- Allocate ---
                frame = av_frame_alloc(); rgb_frame = av_frame_alloc(); packet = av_packet_alloc();
                if (!frame || !rgb_frame || !packet) throw std::runtime_error("Alloc failed");
                int numBytes = av_image_get_buffer_size(AV_PIX_FMT_RGB24, result.width, result.height, 1);
                buffer = (uint8_t*)av_malloc(numBytes);
                if (!buffer) throw std::runtime_error("Buffer alloc failed");
                av_image_fill_arrays(rgb_frame->data, rgb_frame->linesize, buffer, AV_PIX_FMT_RGB24, result.width, result.height, 1);


                // --- Decode Loop (find frame >= timestamp) ---
                int decode_ret = 0; bool frame_decoded = false;
                while (av_read_frame(fmt_ctx, packet) >= 0) {
                    if (packet->stream_index == video_stream_idx) {
                        decode_ret = avcodec_send_packet(codec_ctx, packet);
                        if (decode_ret < 0) break; // Error or EOF flush needed potentially

                        while (decode_ret >= 0) {
                            decode_ret = avcodec_receive_frame(codec_ctx, frame);
                            if (decode_ret == AVERROR(EAGAIN) || decode_ret == AVERROR_EOF) break;
                            if (decode_ret < 0) { av_packet_unref(packet); throw std::runtime_error("Error receiving frame"); }

                            // Check PTS
                            double current_pts_sec = (frame->pts != AV_NOPTS_VALUE && time_base.den != 0) ? static_cast<double>(frame->pts) * av_q2d(time_base) : -1.0;
                            if (current_pts_sec < 0 && frame->pkt_dts != AV_NOPTS_VALUE && time_base.den != 0) { // Fallback DTS
                                current_pts_sec = static_cast<double>(frame->pkt_dts) * av_q2d(time_base);
                            }

                            // Use frame if it's the first one after seek OR its PTS is >= target
                            if (current_pts_sec >= request.timestamp || current_pts_sec < 0) {
                                sws_ctx = sws_getCachedContext(sws_ctx, codec_ctx->width, codec_ctx->height, codec_ctx->pix_fmt,
                                                               result.width, result.height, AV_PIX_FMT_RGB24, SWS_BILINEAR, nullptr, nullptr, nullptr);
                                if (!sws_ctx) { av_frame_unref(frame); throw std::runtime_error("SwsContext failed"); }

                                sws_scale(sws_ctx, (const uint8_t* const*)frame->data, frame->linesize, 0, codec_ctx->height,
                                          rgb_frame->data, rgb_frame->linesize);

                                result.pixels.resize(numBytes);
                                memcpy(result.pixels.data(), buffer, numBytes);
                                result.success = true;
                                frame_decoded = true;
                                av_frame_unref(frame);
                                break; // Found suitable frame
                            }
                            av_frame_unref(frame); // Unref if not the target
                        } // end receive loop
                    } // end if video stream
                    av_packet_unref(packet);
                    if (frame_decoded) break; // Exit read loop
                } // end read loop

                 if (!frame_decoded) {
                    result.error_message = "Reached EOF or error before finding target frame.";
                 }

            } catch (const std::runtime_error& e) {
                result.success = false;
                result.error_message = e.what();
                 std::cerr << "Thumbnail Error (" << request.clip_path << " @ " << request.timestamp << "s): " << e.what() << std::endl;
            }

            // --- FFmpeg Cleanup ---
            av_free(buffer);
            sws_freeContext(sws_ctx);
            av_frame_free(&frame); av_frame_free(&rgb_frame); av_packet_free(&packet);
            if (codec_ctx) avcodec_free_context(&codec_ctx);
            if (fmt_ctx) avformat_close_input(&fmt_ctx);
        } else {
             result.error_message = "Unsupported file type for thumbnails.";
        }


        // --- Enqueue the result ---
        {
            std::lock_guard<std::mutex> lock(result_mutex);
            thumbnail_result_queue.push(std::move(result));
        } // Unlock result_mutex

    } // end main worker loop
    std::cout << "Thumbnail worker thread finished." << std::endl;
}


void start_thumbnail_worker() {
    stop_thumbnail_worker_flag = false;
    thumbnail_worker = std::thread(thumbnail_worker_func);
}

void stop_thumbnail_worker() {
    if (thumbnail_worker.joinable()) {
        stop_thumbnail_worker_flag = true;
         // Clear queue and notify so the worker wakes up to check the flag
        {
            std::lock_guard<std::mutex> lock(request_mutex);
            std::queue<ThumbnailRequest>().swap(thumbnail_request_queue); // Clear queue
        }
        worker_cv.notify_one(); // Wake up worker thread
        thumbnail_worker.join();
        std::cout << "Thumbnail worker joined." << std::endl;
    }
     // Clear any remaining results
     {
        std::lock_guard<std::mutex> lock(result_mutex);
        std::queue<ThumbnailResult>().swap(thumbnail_result_queue);
     }
}