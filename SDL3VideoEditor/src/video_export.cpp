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
#include "glm/glm.hpp"
#include "shared.hpp"
#include "video_export.hpp"
#include "effects.hpp"

extern "C" {
    #include <libavcodec/avcodec.h>
    #include <libavformat/avformat.h>
    #include <libswscale/swscale.h>
    #include <libswresample/swresample.h>
    #include <libavutil/imgutils.h>
    #include <libavutil/opt.h>
    #include <libavutil/channel_layout.h>
}

std::thread thumbnail_worker;
std::queue<ThumbnailRequest> thumbnail_request_queue;
std::queue<ThumbnailResult> thumbnail_result_queue;
std::mutex request_mutex;
std::mutex result_mutex;
std::condition_variable worker_cv;
std::atomic<bool> stop_thumbnail_worker_flag = false;

void start_thumbnail_worker();
void stop_thumbnail_worker();

// Debug function to check OpenGL context status
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

// Function to check if a file is a video file
bool is_video_file(const std::string& path) {
    std::string ext = std::filesystem::path(path).extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    return ext == ".mp4" || ext == ".avi" || ext == ".mov" || ext == ".m4v" || ext == ".mkv";
}

// Function to initialize video
bool initialize_video_resources(GLResources& res, const std::string& path) {
    if (res.video_cache.count(path)) {
        return res.video_cache[path].is_initialized;
    }

    VideoData video;
    
    // Open the file
    video.format_ctx = avformat_alloc_context();
    if (avformat_open_input(&video.format_ctx, path.c_str(), nullptr, nullptr) != 0) {
        std::cerr << "Could not open video file: " << path << std::endl;
        return false;
    }
    
    // Find stream info
    if (avformat_find_stream_info(video.format_ctx, nullptr) < 0) {
        std::cerr << "Could not find stream info for: " << path << std::endl;
        avformat_close_input(&video.format_ctx);
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
    video.frame = av_frame_alloc(); // Only the raw decoded frame
    if (!video.frame) { /* error handling */ avcodec_free_context(&video.codec_ctx); avformat_close_input(&video.format_ctx); return false; }

    video.width = video.codec_ctx->width;
    video.height = video.codec_ctx->height;

    // Initialize SWS context for YUV->RGB conversion
     video.sws_ctx = sws_getContext(
        video.width, video.height, video.codec_ctx->pix_fmt,
        video.width, video.height, AV_PIX_FMT_RGB24, // Target RGB
        SWS_BILINEAR, nullptr, nullptr, nullptr
    );
    if (!video.sws_ctx) { /* error handling */ av_frame_free(&video.frame); avcodec_free_context(&video.codec_ctx); avformat_close_input(&video.format_ctx); return false; }

    video.packet = av_packet_alloc();
    if (!video.packet) { /* error handling */ sws_freeContext(video.sws_ctx); av_frame_free(&video.frame); avcodec_free_context(&video.codec_ctx); avformat_close_input(&video.format_ctx); return false; }

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

    // --- PBO INITIALIZATION ---
    // Calculate the size needed for one frame's pixel data.
    video.pbo_buffer_size = video.width * video.height * 3; // For RGB24

    // Create two PBOs for double-buffering.
    glGenBuffers(2, video.pbos);

    // Initialize the first PBO
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, video.pbos[0]);
    glBufferData(GL_PIXEL_UNPACK_BUFFER, video.pbo_buffer_size, 0, GL_STREAM_DRAW);

    // Initialize the second PBO
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, video.pbos[1]);
    glBufferData(GL_PIXEL_UNPACK_BUFFER, video.pbo_buffer_size, 0, GL_STREAM_DRAW);

    // Unbind the PBO
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
    // --- END PBO INITIALIZATION ---

    // Create OpenGL texture (initially empty)
    glGenTextures(1, &video.texture_id);
    glBindTexture(GL_TEXTURE_2D, video.texture_id);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, video.width, video.height, 0, GL_RGB, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
     glBindTexture(GL_TEXTURE_2D, 0); // Unbind

    video.is_initialized = true;
    auto [it, success] = res.video_cache.emplace(path, std::move(video));
    if (!success) {
         // Handle error: Element with this path already existed? Or emplace failed?
         std::cerr << "Error: Failed to emplace VideoData into cache for " << path << std::endl;
         // Need to clean up partially initialized 'video' object before returning false
         av_packet_free(&video.packet);
         sws_freeContext(video.sws_ctx);
         av_frame_free(&video.frame);
         avcodec_free_context(&video.codec_ctx);
         avformat_close_input(&video.format_ctx);
         // Also delete the GL texture if created
         if(video.texture_id) glDeleteTextures(1, &video.texture_id);
         return false;
    }
     // If successful, the 'video' object has been moved from, use the iterator 'it'
    std::cout << "Initialized video resources for: " << path << " (ID: " << it->second.texture_id << ")" << std::endl;

    glBindTexture(GL_TEXTURE_2D, 0); // Unbind texture (was after the move before)
    return true;
}

// Function to load necessary resources (textures, ffmpeg contexts) for a specific clip
void load_resources_for_clip(GLResources& res, const Clip& clip) {
    if (is_video_file(clip.path)) {
        if (!res.video_cache.count(clip.path) || !res.video_cache[clip.path].is_initialized) {
            initialize_video_resources(res, clip.path);
        }
        // Optionally initialize audio resources here too if needed for streaming
        // if (clip.type == ClipType::Audio || !clip.is_audio_only) {
        //     initialize_audio_resources(res, clip.path);
        // }
         // Preload audio for waveform/export if not already done
        if ((clip.type == ClipType::Audio || !clip.is_audio_only) && !res.preloaded_audio.count(clip.path)) {
            PreloadedAudio audio;
            if (preload_audio_file(clip.path, audio, 0.0f, -1.0f)) { // Preload entire file for now
                 res.preloaded_audio[clip.path] = std::move(audio);
                 std::cout << "Preloaded audio for: " << clip.path << std::endl;
            } else {
                 std::cerr << "Failed to preload audio for: " << clip.path << std::endl;
            }
        }

    } else { // It's an image
        if (!res.texture_cache.count(clip.path)) {
            // This case is handled by the separate load_textures call usually,
            // but we can add it here for robustness if needed.
            load_textures(res, {clip}); // Load just this one image if missing
        }
    }
}

bool ensure_video_decoded_upto(VideoData& video, double target_time_seconds) {
    if (!video.is_initialized) return false;
    if (video.is_seeking) return false; // Don't decode while another operation is in progress

    // --- Seeking Logic ---
    // Check if a seek is necessary. This is the case if the target time is
    // significantly behind the current position or completely outside our small frame cache.
    bool needs_seek = false;
    if (video.last_decoded_pts < 0) { // Not decoded anything yet
        needs_seek = true;
    } else if (video.frame_cache.empty()) { // Cache is empty, might as well seek
        needs_seek = true;
    }
    // Check if target is outside the current cache's time range
    else if (target_time_seconds < video.frame_cache.front().pts - 0.5 || // Target is before the cache starts
             target_time_seconds > video.frame_cache.back().pts + 1.0) {   // Target is significantly after cache end
        needs_seek = true;
    }


    if (needs_seek) {
        video.is_seeking = true; // Signal start of seek

        // Perform the seek
        int64_t seek_target_ts = av_rescale_q(static_cast<int64_t>(target_time_seconds * AV_TIME_BASE),
                                              AV_TIME_BASE_Q, video.time_base);

        // Seek to the keyframe *before* the target time
        int ret = av_seek_frame(video.format_ctx, video.video_stream_idx, seek_target_ts, AVSEEK_FLAG_BACKWARD);

        if (ret < 0) {
            char errbuf[AV_ERROR_MAX_STRING_SIZE];
            av_make_error_string(errbuf, sizeof(errbuf), ret);
            std::cerr << "Warning: Error seeking video " << video.format_ctx->url << " to " << target_time_seconds << "s: " << errbuf << std::endl;
        }

        // CRITICAL: Always flush the decoder's buffers after a seek.
        avcodec_flush_buffers(video.codec_ctx);
        // The cache is now invalid, clear it.
        video.frame_cache.clear();
        // Reset our tracking of the last decoded frame. This is important.
        video.last_decoded_pts = -1.0; 
        
        // std::cout << "Seeked video " << video.format_ctx->url << " to ~" << target_time_seconds << "s" << std::endl;
        
        video.is_seeking = false; // Signal end of seek
    }


    // --- Sequential Decoding Logic ---
    // This part now runs correctly for both smooth playback (incrementing a few frames)
    // and after a seek (decoding from the new keyframe to the target).
    while (video.last_decoded_pts < target_time_seconds) {
        // Try receiving frames first (in case some are buffered)
        int ret = avcodec_receive_frame(video.codec_ctx, video.frame);

        if (ret == AVERROR(EAGAIN)) {
            // Need more packets
            av_packet_unref(video.packet); // Ensure packet is clean
            ret = av_read_frame(video.format_ctx, video.packet);
            if (ret < 0) {
                if (ret == AVERROR_EOF) {
                    // End of file, flush the decoder to get any remaining frames.
                    avcodec_send_packet(video.codec_ctx, nullptr);
                }
                // Any other error means we stop.
                break; 
            }

            // Send the new packet if it's for our video stream
            if (video.packet->stream_index == video.video_stream_idx) {
                 if (avcodec_send_packet(video.codec_ctx, video.packet) < 0) {
                    av_packet_unref(video.packet);
                    break; // Error sending packet
                 }
            }
             av_packet_unref(video.packet);
             continue; // Loop back to try receiving again
        
        } else if (ret == AVERROR_EOF) {
            // Decoder is fully flushed and has no more frames.
            return true; // Reached end, we are "up to" any future time.
        
        } else if (ret < 0) {
             // A real decoding error occurred.
            return false;
        }

        // --- Successfully Received a Frame ---
        double pts_sec = (video.frame->pts == AV_NOPTS_VALUE) ?
                         video.last_decoded_pts : // Best guess if PTS is missing
                         static_cast<double>(video.frame->pts) * av_q2d(video.time_base);

        // After a seek, the first few frames might have odd timestamps. We only want to move forward.
        if (video.last_decoded_pts > 0 && pts_sec < video.last_decoded_pts) {
            av_frame_unref(video.frame);
            continue;
        }

        // Convert and cache the frame
        DecodedFrame decoded;
        decoded.width = video.width;
        decoded.height = video.height;
        decoded.pts = pts_sec;
        decoded.pixels.resize(video.width * video.height * 3); // RGB24

        uint8_t* dest_data[4] = {decoded.pixels.data(), nullptr, nullptr, nullptr};
        int dest_linesize[4] = {video.width * 3, 0, 0, 0};
        sws_scale(video.sws_ctx, video.frame->data, video.frame->linesize, 0, video.height, dest_data, dest_linesize);

        // Add to cache and manage cache size
        video.frame_cache.push_back(std::move(decoded));
        if (video.frame_cache.size() > VideoData::MAX_CACHE_SIZE) {
            video.frame_cache.pop_front();
        }
        video.last_decoded_pts = pts_sec;
        av_frame_unref(video.frame);

        // If we've decoded past the target, we're done for this call.
        if (pts_sec >= target_time_seconds) {
            break;
        }
    }

    return true; // Successfully decoded up to or past the target
}

// Finds the best frame in cache and uploads it ASYNCHRONOUSLY to the texture via PBOs.
bool update_texture_from_cache(VideoData& video, double target_time_seconds) {
    if (!video.is_initialized || video.frame_cache.empty() || video.pbos[0] == 0) {
        return false;
    }

    // Find the closest frame in the cache (your existing logic for this is fine)
    const DecodedFrame* best_frame = nullptr;
    double min_diff = std::numeric_limits<double>::max();
    for (const auto& frame : video.frame_cache) {
        double diff = std::abs(frame.pts - target_time_seconds);
        if (diff < min_diff) {
            min_diff = diff;
            best_frame = &frame;
        }
    }

    if (!best_frame) {
        return false; // No suitable frame found
    }

    // --- ASYNCHRONOUS UPLOAD LOGIC ---
    
    // 1. Bind the texture we want to update.
    glBindTexture(GL_TEXTURE_2D, video.texture_id);
    
    // 2. Bind the PBO that will receive the new pixel data.
    // We ping-pong between pbo_index 0 and 1.
    video.pbo_index = (video.pbo_index + 1) % 2;
    int current_pbo_id = video.pbos[video.pbo_index];
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, current_pbo_id);

    // 3. Upload the pixel data to the PBO.
    // First, we map the buffer, which gives us a pointer we can write to.
    // This call will wait until the GPU is done with any *previous* operations on this PBO,
    // which is why double-buffering is essential.
    glBufferData(GL_PIXEL_UNPACK_BUFFER, video.pbo_buffer_size, 0, GL_STREAM_DRAW); // Orphan the buffer
    GLubyte* pbo_ptr = (GLubyte*)glMapBuffer(GL_PIXEL_UNPACK_BUFFER, GL_WRITE_ONLY);
    
    if (pbo_ptr) {
        // Copy the decoded frame's pixels directly into the PBO's memory.
        memcpy(pbo_ptr, best_frame->pixels.data(), video.pbo_buffer_size);
        glUnmapBuffer(GL_PIXEL_UNPACK_BUFFER); // Release the pointer.
    } else {
        // Handle error if mapping fails.
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
        return false;
    }

    // 4. Issue the ASYNCHRONOUS texture update command.
    // The last argument is `(void*)0`, which OpenGL interprets as an offset into the
    // currently bound GL_PIXEL_UNPACK_BUFFER (our PBO), NOT a pointer to system RAM.
    // This call returns IMMEDIATELY, letting the CPU continue its work. The GPU
    // will perform the transfer from the PBO to the texture in the background.
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, video.width, video.height, GL_RGB, GL_UNSIGNED_BYTE, (void*)0);

    // 5. Unbind everything for good measure.
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
    glBindTexture(GL_TEXTURE_2D, 0);

    return true;
}

// --- The NEW, consolidated preview update function ---
void update_video_previews(GLResources& res, const std::vector<Clip>& active_clips, float current_time) {
    // Step 1: Group all required media times by their source file path.
    // This is the core optimization. We avoid redundant work on the same file.
    std::map<std::string, std::vector<double>> required_times_by_path;

    for (const auto& clip : active_clips) {
        // Only process video clips that are actually from video files.
        if (clip.type == ClipType::Video && is_video_file(clip.path)) {
            // Calculate the timestamp needed from the source media file.
            float media_time = (current_time - clip.start_time) + clip.media_start;
            
            // Only add valid, positive times to the request list.
            if (media_time >= 0) {
                required_times_by_path[clip.path].push_back(media_time);
            }
        }
    }

    // Step 2: For each unique video file, perform a single consolidated decoding pass.
    for (auto const& [path, times] : required_times_by_path) {
        auto it = res.video_cache.find(path);
        if (it == res.video_cache.end() || !it->second.is_initialized) {
            continue; // Skip if this video's resources aren't ready.
        }

        VideoData& video = it->second;

        // Find the LATEST timestamp required for this file among all its clip segments.
        double max_required_time = 0.0;
        if (!times.empty()) {
            max_required_time = *std::max_element(times.begin(), times.end());
        }

        // Add a small buffer to decode slightly ahead for smooth playback.
        float decode_ahead_time = 2.0f / 30.0f; // 2 frames at 30fps
        double decode_target_time = max_required_time + decode_ahead_time;

        // Call the powerful ensure_video_decoded_upto function just ONCE for this file.
        // It will efficiently seek or decode to get the file's internal state ready.
        ensure_video_decoded_upto(video, decode_target_time);
    }
    
    // Step 3: Now that all caches are populated, update the textures for each clip.
    // This part is fast as it just finds the best frame in the cache and does a GL upload.
    for (const auto& clip : active_clips) {
        if (clip.type == ClipType::Video && is_video_file(clip.path)) {
            auto it = res.video_cache.find(clip.path);
            if (it != res.video_cache.end() && it->second.is_initialized) {
                float media_time = (current_time - clip.start_time) + clip.media_start;
                if (media_time >= 0) {
                    // This function just reads from the cache we populated above.
                    update_texture_from_cache(it->second, media_time);
                }
            }
        }
    }
}


// Loads ONLY image textures
void load_textures(GLResources& res, const std::vector<Clip>& clips) {
    for (const auto& clip : clips) {
        // Skip if already loaded or if it's a video file (handled separately)
        if (res.texture_cache.count(clip.path) || is_video_file(clip.path)) {
            continue;
        }

        if (!std::filesystem::exists(clip.path)) {
            std::cerr << "Image file does not exist: " << clip.path << std::endl;
            continue;
        }

        SDL_Surface* surface = IMG_Load(clip.path.c_str());
        if (!surface) {
            std::cerr << "Failed to load image: " << clip.path << " - " << SDL_GetError() << std::endl;
            continue;
        }

        SDL_Surface* formatted_surface = nullptr; // Surface to use for texture upload
        SDL_PixelFormat target_format_sdl;       // Target SDL pixel format value
        GLenum gl_format, gl_internal_format;

        // Determine target format (prefer RGBA if alpha exists)
        // In SDL3, surface->format is the SDL_PixelFormat value directly
        bool has_alpha = SDL_ISPIXELFORMAT_ALPHA(surface->format);

        if (has_alpha) {
            target_format_sdl = SDL_PIXELFORMAT_RGBA32; // Common 32-bit RGBA format
            gl_format = GL_RGBA;
            gl_internal_format = GL_RGBA;
        } else {
            target_format_sdl = SDL_PIXELFORMAT_RGB24;  // Common 24-bit RGB format
            gl_format = GL_RGB;
            gl_internal_format = GL_RGB;
        }

        // Check if conversion is needed
        if (surface->format != target_format_sdl) {
            std::cout << "Converting surface " << clip.path << " from "
                      << SDL_GetPixelFormatName(surface->format) << " to "
                      << SDL_GetPixelFormatName(target_format_sdl) << std::endl;

            // SDL_ConvertSurface takes the target format value directly
            formatted_surface = SDL_ConvertSurface(surface, target_format_sdl);

            if (!formatted_surface) {
                std::cerr << "Failed to convert image surface for " << clip.path << ": " << SDL_GetError() << std::endl;
                SDL_DestroySurface(surface); // Free original surface
                continue;
            }
            SDL_DestroySurface(surface); // Free original surface after successful conversion
        } else {
            // No conversion needed, use the original surface
            formatted_surface = surface;
        }

        GLuint tex_id = 0;
        glGenTextures(1, &tex_id);
        if (tex_id == 0) {
             std::cerr << "Failed to generate texture ID for " << clip.path << std::endl;
             SDL_DestroySurface(formatted_surface);
             continue;
        }
        glBindTexture(GL_TEXTURE_2D, tex_id);

        // Upload pixel data
        glTexImage2D(GL_TEXTURE_2D, 0, gl_internal_format, formatted_surface->w, formatted_surface->h, 0,
                     gl_format, GL_UNSIGNED_BYTE, formatted_surface->pixels);

        GLenum err = glGetError();
        if (err != GL_NO_ERROR) {
            std::cerr << "OpenGL error creating texture for " << clip.path << ": " << err << std::endl;
            glDeleteTextures(1, &tex_id);
            SDL_DestroySurface(formatted_surface);
            continue;
        }

        // Set texture parameters
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

        res.texture_cache[clip.path] = tex_id; // Store the texture ID
        SDL_DestroySurface(formatted_surface); // Free the surface used for upload
        std::cout << "Loaded image texture: " << clip.path << " (ID: " << tex_id << ")" << std::endl;
    }
    glBindTexture(GL_TEXTURE_2D, 0); // Unbind texture unit
}


// Renders using *existing* textures. Does NO decoding.
void render_frame(GLResources& res, float current_time,
    const std::vector<Clip>& sorted_clips,
    int width, int height) {

    glBindFramebuffer(GL_FRAMEBUFFER, res.fbo);
    glViewport(0, 0, width, height);
    glClearColor(0.1f, 0.1f, 0.1f, 1.0f); // Dark gray background
    glClear(GL_COLOR_BUFFER_BIT); // No depth buffer needed for 2D compositing usually

    glEnable(GL_TEXTURE_2D);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA); // Standard alpha blending

    // Simple Ortho projection for [-1, 1] space
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(-1.0 * width, 1.0 * width, -1.0 * height, 1.0 * height, -1.0, 1.0);

    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();

    bool rendered_any = false;
    for (const auto& clip : sorted_clips) {
        // Set blending mode based on clip
        switch (clip.blend_mode) {
            case BlendMode::Normal:
                glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
                break;
            case BlendMode::Additive:
                glBlendFunc(GL_SRC_ALPHA, GL_ONE);
                break;
            case BlendMode::Multiply:
                glBlendFunc(GL_DST_COLOR, GL_ZERO);
                break;
            case BlendMode::Screen:
                glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_COLOR);
                break;
            case BlendMode::Darken:
                glBlendFunc(GL_MIN, GL_ONE); // approximate
                break;
            case BlendMode::Lighten:
                glBlendFunc(GL_MAX, GL_ONE); // approximate
                break;
            case BlendMode::Difference:
                glBlendFunc(GL_ONE_MINUS_DST_COLOR, GL_ONE_MINUS_SRC_COLOR);
                break;
            case BlendMode::Subtract:
                glBlendFunc(GL_ZERO, GL_ONE_MINUS_SRC_COLOR);
                break;
            case BlendMode::Divide:
                // Not directly possible with glBlendFunc, would need shader
                glBlendFunc(GL_ONE, GL_ONE); // approximate
                break;
            case BlendMode::Overlay:
                // Overlay needs a shader normally. Approximate with multiply/screen hybrid
                glBlendFunc(GL_SRC_ALPHA, GL_ONE);
                break;
        }

        // Check if clip is active and is visual (Video type)
        if (clip.type == ClipType::Video &&
        current_time >= clip.start_time &&
        current_time < (clip.start_time + clip.duration))
        {
            float local_time = current_time - clip.start_time; // relative to clip start

        // --- Evaluate keyframes ---
        float evaluated_pos_x = clip.pos_x_track.Evaluate(local_time);
        float evaluated_pos_y = clip.pos_y_track.Evaluate(local_time);
        float evaluated_scale = clip.scale_track.Evaluate(local_time);
        float evaluated_rotation = clip.rotation_track.Evaluate(local_time);
        float evaluated_opacity = clip.opacity_track.Evaluate(local_time);

        // Fallback / Clamp
        if (clip.pos_x_track.keyframes.empty()) evaluated_pos_x = clip.pos_x;
        if (clip.pos_y_track.keyframes.empty()) evaluated_pos_y = clip.pos_y;
        if (clip.scale_track.keyframes.empty()) evaluated_scale = clip.scale;
        if (clip.rotation_track.keyframes.empty()) evaluated_rotation = clip.rotation;
        if (clip.opacity_track.keyframes.empty()) evaluated_opacity = clip.opacity;

        evaluated_scale = std::max(0.0f, evaluated_scale);     // Never allow 0 scale
        evaluated_opacity = std::clamp(evaluated_opacity, 0.0f, 1.0f); // Clamp opacity

        // (Optionally: evaluated_rotation if you add that too)

        GLuint tex_id = 0;
        bool is_video = is_video_file(clip.path);

        // Find the texture
        if (is_video) {
            auto vid_it = res.video_cache.find(clip.path);
            if (vid_it != res.video_cache.end() && vid_it->second.is_initialized) {
                tex_id = vid_it->second.texture_id;
            }
        } else {
            auto img_it = res.texture_cache.find(clip.path);
            if (img_it != res.texture_cache.end()) {
                tex_id = img_it->second;
            }
        }

        if (tex_id != 0) {
            // If the clip has effects, we need to handle them differently
            if (clip.has_effects) {
                // Create a temporary FBO to capture the clip with transforms applied
                GLuint transformed_tex;
                GLuint transformed_fbo = create_temp_fbo(glm::vec2(width, height), transformed_tex);
                
                // Render the clip with transforms to the temporary FBO
                glBindFramebuffer(GL_FRAMEBUFFER, transformed_fbo);
                glViewport(0, 0, width, height);
                
                // Bind the clip's texture
                glBindTexture(GL_TEXTURE_2D, tex_id);
                
                glPushMatrix();
                // Apply transforms
                glTranslatef(evaluated_pos_x * width, evaluated_pos_y * height, 0.0f);
                glRotatef(evaluated_rotation, 0.0f, 0.0f, 1.0f);
                glScalef(evaluated_scale, evaluated_scale, 1.0f);
                glColor4f(1.0f, 1.0f, 1.0f, evaluated_opacity);
                
                // Draw the texture with transforms applied
                glBegin(GL_QUADS);
                    glTexCoord2f(0.0f, 0.0f); glVertex2f(-1.0f * width, -1.0f * height);
                    glTexCoord2f(1.0f, 0.0f); glVertex2f(1.0f * width, -1.0f * height);
                    glTexCoord2f(1.0f, 1.0f); glVertex2f(1.0f * width, 1.0f * height);
                    glTexCoord2f(0.0f, 1.0f); glVertex2f(-1.0f * width, 1.0f * height);
                glEnd();
                
                glPopMatrix();
                
                // Process the effects using the transformed texture
                glBindFramebuffer(GL_FRAMEBUFFER, res.fbo); // Back to main FBO
                clip.effect_graph->Process(transformed_tex, res.fbo, current_time, glm::vec2(width, height));
                
                // Clean up
                destroy_temp_fbo(transformed_fbo, transformed_tex);
            } else {
                // Normal rendering (no effects)
                glBindTexture(GL_TEXTURE_2D, tex_id);
                glPushMatrix();
                
                // Apply transforms
                glTranslatef(evaluated_pos_x * width, evaluated_pos_y * height, 0.0f);
                glRotatef(evaluated_rotation, 0.0f, 0.0f, 1.0f);
                glScalef(evaluated_scale, evaluated_scale, 1.0f);
                glColor4f(1.0f, 1.0f, 1.0f, evaluated_opacity);
                
                // Draw the quad
                glBegin(GL_QUADS);
                    glTexCoord2f(0.0f, 0.0f); glVertex2f(-1.0f * width, -1.0f * height);
                    glTexCoord2f(1.0f, 0.0f); glVertex2f(1.0f * width, -1.0f * height);
                    glTexCoord2f(1.0f, 1.0f); glVertex2f(1.0f * width, 1.0f * height);
                    glTexCoord2f(0.0f, 1.0f); glVertex2f(-1.0f * width, 1.0f * height);
                glEnd();
                
                glPopMatrix();
            }
            
            rendered_any = true;
        } else {
            // Optionally render a placeholder if texture is missing/not ready
            // std::cerr << "Texture ID 0 for active clip: " << clip.path << std::endl;
        }
    }
        }

        // Reset color and disable states
        glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
        glDisable(GL_BLEND);
        glDisable(GL_TEXTURE_2D);
        glBindTexture(GL_TEXTURE_2D, 0);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);

    // if (!rendered_any) { std::cout << "Rendered empty frame at " << current_time << std::endl; }
}


// --- Cleanup ---
void cleanup_video_resources(GLResources& res) {
    std::cout << "Cleaning up video resources..." << std::endl;
    for (auto& [path, video] : res.video_cache) {
        if (video.is_initialized) {
            std::cout << "  Cleaning: " << path << std::endl;
            av_packet_free(&video.packet);
            sws_freeContext(video.sws_ctx);
            av_frame_free(&video.frame);
            // av_frame_free(&video.frame_rgb); // Removed
            // av_free(video.buffer);          // Removed
            avcodec_free_context(&video.codec_ctx);
            avformat_close_input(&video.format_ctx);

            if (video.pbos[0] != 0 || video.pbos[1] != 0) {
                glDeleteBuffers(2, video.pbos);
                video.pbos[0] = 0;
                video.pbos[1] = 0;
            }

            video.is_initialized = false; // Mark as cleaned
        }
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
    for (auto const& [path, tex_id] : res.texture_cache) {
        glDeleteTextures(1, &tex_id);
    }
    res.texture_cache.clear();

    // Clean up thumbnail textures
    std::cout << "Cleaning up thumbnail textures..." << std::endl;
    for (auto& [path, tex_vec] : res.clip_thumbnail_textures) {
        for (GLuint tex_id : tex_vec) {
            if (tex_id) glDeleteTextures(1, &tex_id);
        }
    }
    res.clip_thumbnail_textures.clear();
    res.generated_thumbnails_map.clear(); // Clear the tracking map too

    // Also delete video textures managed by VideoData
    for (auto const& [path, video_data] : res.video_cache) {
         if (video_data.texture_id) {
            glDeleteTextures(1, &video_data.texture_id);
            // No need to modify video_data here as it will be cleared
        }
    }
    // The video_cache map itself is cleared in cleanup_video_resources
     std::cout << "GL resources cleaned." << std::endl;
}


// --- Audio Preloading (Keep as is, but ensure it's called appropriately) ---
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


// --- Video Export (Needs significant rewrite for performance) ---
// This version still uses the slower render_frame + glReadPixels approach
// but incorporates the updated render_frame and resource loading.
// For faster export, pipe to FFmpeg or use libavcodec directly.
bool start_video_export(const std::string& output_path, int width, int height, int fps,
    int duration_frames, const std::vector<Clip>& clips, SDL_Window* window) {

std::cout << "Starting video export (using glReadPixels method)..." << std::endl;
SDL_GLContext current_context = SDL_GL_GetCurrentContext();
if (!current_context) { /* error */ return false; }

// Backup GL state (optional but good practice)
// GLint previous_viewport[4]; glGetIntegerv(GL_VIEWPORT, previous_viewport);
// GLint previous_fbo; glGetIntegerv(GL_FRAMEBUFFER_BINDING, &previous_fbo);

// Prepare resources specifically for export resolution
GLResources export_res;
if (!setup_gl_resources(export_res, width, height)) {
std::cerr << "Failed to setup GL resources for export." << std::endl;
return false;
}

// Load all necessary resources (textures, video/audio contexts, preload audio)
for (const auto& clip : clips) {
load_resources_for_clip(export_res, clip);
}

// Sort clips by layer once
auto sorted_clips = clips;
std::sort(sorted_clips.begin(), sorted_clips.end(), [](const Clip& a, const Clip& b) {
return a.layer < b.layer;
});

// Open raw output files
std::ofstream video_file("video.raw", std::ios::binary);
std::ofstream audio_file("temp_audio.raw", std::ios::binary);
if (!video_file || !audio_file) { /* error */ cleanup_gl_resources(export_res); cleanup_video_resources(export_res); return false; }


std::vector<uint8_t> pixels(width * height * 3); // RGB
const int audio_sample_rate = 44100;
const int audio_channels = 2;
const int samples_per_frame = audio_sample_rate / fps;
std::vector<float> audio_float(samples_per_frame * audio_channels, 0.0f);
std::vector<int16_t> audio_s16(samples_per_frame * audio_channels);


// --- Render and Export Loop ---
for (int frame_idx = 0; frame_idx < duration_frames; ++frame_idx) {
float current_time = static_cast<float>(frame_idx) / fps;

// 1. Update video textures for this frame time
// Collect active video clips for this frame
std::vector<Clip> active_video_clips;
for(const auto& clip : sorted_clips) {
if (clip.type == ClipType::Video && is_video_file(clip.path) &&
current_time >= clip.start_time && current_time < clip.start_time + clip.duration) {
active_video_clips.push_back(clip);
}
}
// Update previews (decode and upload textures)
update_video_previews(export_res, active_video_clips, current_time);


// 2. Render the composited frame to FBO
render_frame(export_res, current_time, sorted_clips, width, height);

// 3. Read back pixels (Slow part)
glBindFramebuffer(GL_FRAMEBUFFER, export_res.fbo);
glReadBuffer(GL_COLOR_ATTACHMENT0); // Ensure reading from the color attachment
glReadPixels(0, 0, width, height, GL_RGB, GL_UNSIGNED_BYTE, pixels.data());
glBindFramebuffer(GL_FRAMEBUFFER, 0); // Unbind FBO

// Flip vertically (OpenGL bottom-left origin vs Image top-left)
/* size_t row_bytes = width * 3;
std::vector<uint8_t> row_buffer(row_bytes);
for (int y = 0; y < height / 2; ++y) {
uint8_t* top_row = pixels.data() + y * row_bytes;
uint8_t* bottom_row = pixels.data() + (height - 1 - y) * row_bytes;
memcpy(row_buffer.data(), top_row, row_bytes);
memcpy(top_row, bottom_row, row_bytes);
memcpy(bottom_row, row_buffer.data(), row_bytes);
} */

// Write video frame
video_file.write(reinterpret_cast<const char*>(pixels.data()), pixels.size());


// 4. Mix Audio
std::fill(audio_float.begin(), audio_float.end(), 0.0f);
for (const auto& clip : sorted_clips) {
if (clip.type == ClipType::Audio &&
current_time >= clip.start_time &&
current_time < (clip.start_time + clip.duration))
{
auto audio_it = export_res.preloaded_audio.find(clip.path);
if (audio_it != export_res.preloaded_audio.end()) {
 const PreloadedAudio& audio = audio_it->second;
  // Calculate start sample in the preloaded buffer based on clip timing
  float time_into_clip = current_time - clip.start_time;
  float effective_media_start = clip.media_start; // Use the clip's media start
  float time_in_media = time_into_clip + effective_media_start;

 int start_sample_idx = static_cast<int>(time_in_media * audio.sample_rate);

 for (int i = 0; i < samples_per_frame; ++i) {
     int current_sample_idx = start_sample_idx + i;
      // Check bounds for the preloaded audio samples
     if (current_sample_idx * audio.channels + (audio.channels - 1) < audio.samples.size() && current_sample_idx >= 0) {
         for (int ch = 0; ch < audio.channels; ++ch) {
             int16_t sample_s16 = audio.samples[current_sample_idx * audio.channels + ch];
             audio_float[i * audio.channels + ch] += static_cast<float>(sample_s16) / 32768.0f;
         }
     } else {
         // Optionally handle running out of samples (e.g., break loop)
          break;
     }
 }
}
}
}

// Clamp and convert float audio to S16
for (size_t i = 0; i < audio_float.size(); ++i) {
float sample_f = std::max(-1.0f, std::min(1.0f, audio_float[i])); // Clamp
audio_s16[i] = static_cast<int16_t>(sample_f * 32767.0f);
}
audio_file.write(reinterpret_cast<const char*>(audio_s16.data()), audio_s16.size() * sizeof(int16_t));


// Progress update
if (frame_idx % 30 == 0 || frame_idx == duration_frames - 1) {
std::cout << "Export progress: Frame " << frame_idx + 1 << "/" << duration_frames << std::endl;
}
}

video_file.close();
audio_file.close();

// --- FFmpeg Command (Keep as is for now) ---
std::string ffmpeg_cmd =
"ffmpeg -y "
"-f rawvideo -pix_fmt rgb24 -s " + std::to_string(width) + "x" + std::to_string(height) + // Use -s for size
" -r " + std::to_string(fps) + " -i video.raw "
"-f s16le -ac 2 -ar " + std::to_string(audio_sample_rate) + " -i temp_audio.raw "
"-c:v libx264 -preset fast -crf 22 -pix_fmt yuv420p " // Use pix_fmt for output
"-c:a aac -b:a 192k "
"-shortest \"" + output_path + "\""; // Use shortest to stop when shortest input ends

std::cout << "Running FFmpeg command:\n" << ffmpeg_cmd << std::endl;
int ret = system(ffmpeg_cmd.c_str());

// Cleanup
std::filesystem::remove("video.raw");
std::filesystem::remove("temp_audio.raw");
cleanup_gl_resources(export_res);
cleanup_video_resources(export_res); // Cleans up FFmpeg contexts too

// Restore GL state (if backed up)
// glBindFramebuffer(GL_FRAMEBUFFER, previous_fbo);
// glViewport(previous_viewport[0], previous_viewport[1], previous_viewport[2], previous_viewport[3]);


if (ret != 0) {
std::cerr << "FFmpeg export failed with code " << ret << std::endl;
return false;
}

std::cout << "Video export finished successfully." << std::endl;
return true;
}

std::vector<float> GenerateWaveformPreview(const std::vector<int16_t>& samples, int channels, int samples_per_pixel = 256) {
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

void QueueClipThumbnails(GLResources& res, const Clip& clip) { // Pass const Clip&
    if (clip.type == ClipType::Video && is_video_file(clip.path)) {
        // Check if thumbnails are already being generated or are done for this clip path
        if (res.clip_thumbnail_textures.count(clip.path)) {
             // Already processed or processing started, could add logic to re-queue if needed
             // std::cout << "Thumbnails already queued/generated for " << clip.path << std::endl;
            return;
        }

        const int max_thumbs = static_cast<int>(clip.duration); // Generate more thumbs if needed
        const float interval = std::max(0.5f, clip.duration / static_cast<float>(max_thumbs)); // Ensure minimum interval

        // Create placeholder entry in the map immediately so we know processing started
        res.clip_thumbnail_textures[clip.path] = {}; // Empty vector initially
        res.generated_thumbnails_map[clip.path] = {}; // Empty map for generated timestamps


        { // Lock the request queue
            std::lock_guard<std::mutex> lock(request_mutex);
            for (int i = 0; i < max_thumbs; ++i) {
                float timestamp = i * interval + clip.media_start; // Use media_start
                 // Clamp timestamp to be within the actual media duration if known
                 // (Need media duration - maybe get it during AddNewClip or load_resources)
                // float media_duration = get_media_duration(clip.path); // Hypothetical function
                // if (media_duration > 0) timestamp = std::min(timestamp, media_duration - 0.1f);

                timestamp = std::max(0.0f, timestamp); // Ensure non-negative

                ThumbnailRequest req;
                req.clip_path = clip.path;
                req.timestamp = timestamp;
                thumbnail_request_queue.push(req);
                 // std::cout << "Queued thumb request for " << clip.path << " at " << timestamp << std::endl;
            }
        } // Unlock request_mutex

        worker_cv.notify_one(); // Signal the worker thread
    }
}

// Replace the existing ProcessThumbnailTasks in video_export.cpp or main.cpp

void ProcessThumbnailResults(GLResources& res, int max_per_frame = 2) { // Process a couple per frame
    for (int i = 0; i < max_per_frame; ++i) {
        ThumbnailResult result;

        // --- Check for results ---
        {
            std::lock_guard<std::mutex> lock(result_mutex);
            if (thumbnail_result_queue.empty()) {
                break; // No more results pending for now
            }
            result = std::move(thumbnail_result_queue.front()); // Move the result
            thumbnail_result_queue.pop();
        } // Unlock result_mutex

        // --- Process the result ---
        if (result.success && !result.pixels.empty()) {
             // Check if this exact timestamp was already processed (e.g., due to requeueing)
            auto& gen_map = res.generated_thumbnails_map[result.clip_path];
            if (gen_map.count(result.timestamp)) {
                continue; // Already have this one
            }


            // Create OpenGL Texture on Main Thread
            GLuint tex_id = 0;
            glGenTextures(1, &tex_id);
            if (tex_id == 0) {
                 std::cerr << "Failed to generate texture ID for thumbnail!" << std::endl;
                 continue; // Skip this one
            }
            glBindTexture(GL_TEXTURE_2D, tex_id);

            // Upload pixel data (RGB)
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, result.width, result.height, 0,
                         GL_RGB, GL_UNSIGNED_BYTE, result.pixels.data());

            GLenum err = glGetError();
            if (err != GL_NO_ERROR) {
                std::cerr << "OpenGL error creating thumbnail texture for " << result.clip_path << ": " << err << std::endl;
                glDeleteTextures(1, &tex_id); // Clean up failed texture
                continue; // Skip
            }

            // Set texture parameters (simple bilinear is fine for thumbs)
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

            glBindTexture(GL_TEXTURE_2D, 0); // Unbind

            // Store the texture ID in the main resource map
            // Note: This assumes res.clip_thumbnail_textures[result.clip_path] was initialized in QueueClipThumbnails
            if (res.clip_thumbnail_textures.count(result.clip_path)) {
                res.clip_thumbnail_textures[result.clip_path].push_back(tex_id);
                 gen_map[result.timestamp] = tex_id; // Mark timestamp as generated
                // std::cout << "Generated thumbnail texture " << tex_id << " for " << result.clip_path << " at " << result.timestamp << std::endl;
            } else {
                std::cerr << "Warning: Thumbnail result received for clip path not found in texture map: " << result.clip_path << std::endl;
                glDeleteTextures(1, &tex_id); // Clean up unused texture
            }
        } else {
            // Handle failure case if needed (e.g., log error)
            // std::cerr << "Thumbnail generation failed for " << result.clip_path << " at " << result.timestamp << ": " << result.error_message << std::endl;
        }
    } // end loop
}

// Add this new function to video_export.cpp

void thumbnail_worker_func() {
    std::cout << "Thumbnail worker thread started." << std::endl;
    while (!stop_thumbnail_worker_flag) {
        ThumbnailRequest request;

        // --- Wait for a request ---
        {
            std::unique_lock<std::mutex> lock(request_mutex);
            worker_cv.wait(lock, [&] {
                return !thumbnail_request_queue.empty() || stop_thumbnail_worker_flag;
            });

            if (stop_thumbnail_worker_flag) {
                break; // Exit signal received
            }

            if (thumbnail_request_queue.empty()) {
                continue; // Spurious wakeup?
            }

            request = thumbnail_request_queue.front();
            thumbnail_request_queue.pop();
        } // Unlock request_mutex

        // --- Process the request ---
        ThumbnailResult result;
        result.clip_path = request.clip_path;
        result.timestamp = request.timestamp;
        result.width = THUMBNAIL_WIDTH;
        result.height = THUMBNAIL_HEIGHT;
        result.success = false; // Assume failure initially

        AVFormatContext* fmt_ctx = nullptr;
        AVCodecContext* codec_ctx = nullptr;
        AVFrame* frame = nullptr;
        AVFrame* rgb_frame = nullptr;
        AVPacket* packet = nullptr;
        SwsContext* sws_ctx = nullptr;
        int video_stream_idx = -1;
        uint8_t* buffer = nullptr;

        try { // Use try-catch for easier cleanup on error
            // 1. Open Input
            if (avformat_open_input(&fmt_ctx, request.clip_path.c_str(), nullptr, nullptr) != 0) {
                throw std::runtime_error("Could not open video file: " + request.clip_path);
            }
            if (avformat_find_stream_info(fmt_ctx, nullptr) < 0) {
                throw std::runtime_error("Could not find stream info for: " + request.clip_path);
            }

            // 2. Find Video Stream & Codec
            video_stream_idx = av_find_best_stream(fmt_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
            if (video_stream_idx < 0) {
                throw std::runtime_error("No video stream found in: " + request.clip_path);
            }
            AVCodecParameters* codec_params = fmt_ctx->streams[video_stream_idx]->codecpar;
            const AVCodec* codec = avcodec_find_decoder(codec_params->codec_id);
            if (!codec) {
                throw std::runtime_error("Unsupported codec in: " + request.clip_path);
            }
            codec_ctx = avcodec_alloc_context3(codec);
            if (!codec_ctx || avcodec_parameters_to_context(codec_ctx, codec_params) < 0 || avcodec_open2(codec_ctx, codec, nullptr) < 0) {
                throw std::runtime_error("Failed to setup codec context for: " + request.clip_path);
            }

            AVRational time_base = fmt_ctx->streams[video_stream_idx]->time_base;

            // 3. Seek
            int64_t target_ts = av_rescale_q(static_cast<int64_t>(request.timestamp * AV_TIME_BASE), AV_TIME_BASE_Q, time_base);
            // Seek slightly before the target to ensure we get the right frame or the one just before it
            if (av_seek_frame(fmt_ctx, video_stream_idx, target_ts, AVSEEK_FLAG_BACKWARD) < 0) {
                 // Don't throw, just log warning and try decoding from start if seek fails near beginning
                 std::cerr << "Warning: Seek failed for " << request.clip_path << " at " << request.timestamp << "s. Trying from start." << std::endl;
                 // Optionally seek to 0 if target_ts wasn't 0?
                 // av_seek_frame(fmt_ctx, video_stream_idx, 0, AVSEEK_FLAG_BACKWARD);
            }
            avcodec_flush_buffers(codec_ctx); // Important after seek

            // 4. Allocate Decoding Resources
            frame = av_frame_alloc();
            rgb_frame = av_frame_alloc(); // For the RGB conversion result
            packet = av_packet_alloc();
            if (!frame || !rgb_frame || !packet) {
                throw std::runtime_error("Failed to allocate FFmpeg frame/packet.");
            }

            // Allocate buffer for RGB frame manually
             int numBytes = av_image_get_buffer_size(AV_PIX_FMT_RGB24, THUMBNAIL_WIDTH, THUMBNAIL_HEIGHT, 1); // Use alignment 1 for simplicity
             buffer = (uint8_t*)av_malloc(numBytes * sizeof(uint8_t));
             if (!buffer) {
                 throw std::runtime_error("Failed to allocate RGB buffer.");
             }
            av_image_fill_arrays(rgb_frame->data, rgb_frame->linesize, buffer, AV_PIX_FMT_RGB24, THUMBNAIL_WIDTH, THUMBNAIL_HEIGHT, 1);


            // 5. Decode until the target frame is found or passed
            int decode_ret = 0;
            bool frame_decoded = false;
            while (av_read_frame(fmt_ctx, packet) >= 0) {
                if (packet->stream_index == video_stream_idx) {
                    decode_ret = avcodec_send_packet(codec_ctx, packet);
                    if (decode_ret < 0) {
                        // Error sending packet, might be recoverable or EOF flush needed
                        break;
                    }

                    while (decode_ret >= 0) {
                        decode_ret = avcodec_receive_frame(codec_ctx, frame);
                        if (decode_ret == AVERROR(EAGAIN) || decode_ret == AVERROR_EOF) {
                            break; // Need more packets or finished
                        } else if (decode_ret < 0) {
                            av_packet_unref(packet);
                            throw std::runtime_error("Error receiving frame during decoding.");
                        }

                        // Check if this frame is at or after our target timestamp
                        double current_pts_sec = (frame->pts == AV_NOPTS_VALUE) ? -1.0 : static_cast<double>(frame->pts) * av_q2d(time_base);
                        if (current_pts_sec < 0 && frame->pkt_dts != AV_NOPTS_VALUE) { // Fallback to DTS if PTS missing
                             current_pts_sec = static_cast<double>(frame->pkt_dts) * av_q2d(time_base);
                        }


                        if (current_pts_sec >= request.timestamp || current_pts_sec < 0 ) { // Use frame if >= target or PTS is unknown after seek
                             // Initialize SWS context *here* now that we have the source frame format
                            sws_ctx = sws_getCachedContext(sws_ctx, // Reuse context
                                                           codec_ctx->width, codec_ctx->height, codec_ctx->pix_fmt,
                                                           THUMBNAIL_WIDTH, THUMBNAIL_HEIGHT, AV_PIX_FMT_RGB24,
                                                           SWS_BILINEAR, // Use bilinear for speed
                                                           nullptr, nullptr, nullptr);
                            if (!sws_ctx) {
                                throw std::runtime_error("Failed to create SwsContext.");
                            }

                            // Convert the frame directly to thumbnail size RGB
                            sws_scale(sws_ctx, (const uint8_t* const*)frame->data, frame->linesize, 0, codec_ctx->height,
                                      rgb_frame->data, rgb_frame->linesize);

                            // Copy pixels to result vector
                            result.pixels.resize(numBytes);
                            memcpy(result.pixels.data(), buffer, numBytes);
                            result.success = true;
                            frame_decoded = true;
                            av_frame_unref(frame); // Release the decoded frame
                            break; // Found our frame
                        }
                        av_frame_unref(frame); // Release frame if it wasn't the target
                    } // end receive loop
                } // end if video stream
                av_packet_unref(packet); // Release packet
                if (frame_decoded) {
                    break; // Exit read loop
                }
            } // end read loop

            // If loop finished without decoding, maybe try flushing? (Less critical for single frame)
             if (!frame_decoded) {
                // Optional: Add flush logic here if needed, similar to ensure_video_decoded_upto EOF handling
                // but convert the *first* flushed frame if timestamp was near the end.
                result.error_message = "Reached EOF or error before finding target frame.";
             }


        } catch (const std::runtime_error& e) {
            result.success = false;
            result.error_message = e.what();
            std::cerr << "Thumbnail Error (" << request.clip_path << " @ " << request.timestamp << "s): " << e.what() << std::endl;
        }

        // --- Cleanup FFmpeg resources ---
        av_free(buffer); // Free the manually allocated buffer
        sws_freeContext(sws_ctx);
        av_frame_free(&frame);
        av_frame_free(&rgb_frame);
        av_packet_free(&packet);
        if (codec_ctx) avcodec_free_context(&codec_ctx);
        if (fmt_ctx) avformat_close_input(&fmt_ctx);

        // --- Enqueue the result ---
        {
            std::lock_guard<std::mutex> lock(result_mutex);
            thumbnail_result_queue.push(std::move(result));
        } // Unlock result_mutex

    } // end main worker loop
    std::cout << "Thumbnail worker thread finished." << std::endl;
}

// Add these functions, call from main() init and cleanup

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
