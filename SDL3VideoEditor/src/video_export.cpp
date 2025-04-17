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
#include "shared.hpp"
#include "video_export.hpp"

extern "C" {
    #include <libavcodec/avcodec.h>
    #include <libavformat/avformat.h>
    #include <libswscale/swscale.h>
    #include <libswresample/swresample.h>
    #include <libavutil/imgutils.h>
    #include <libavutil/opt.h>
    #include <libavutil/channel_layout.h>
}

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
};

// Add a map for video data to the GLResources struct
struct GLResources {
    GLuint fbo = 0;
    GLuint render_tex = 0;
    std::map<std::string, GLuint> texture_cache;
    std::map<std::string, VideoData> video_cache;
    std::map<std::string, AudioData> audio_cache;
};

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

static bool setup_gl_resources(GLResources& res, int width, int height) {
    // Create FBO
    glGenFramebuffers(1, &res.fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, res.fbo);
    
    // Create render texture
    glGenTextures(1, &res.render_tex);
    glBindTexture(GL_TEXTURE_2D, res.render_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, width, height, 0,
                GL_RGB, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    // Attach texture to FBO
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                          GL_TEXTURE_2D, res.render_tex, 0);

    // Check if FBO is complete
    GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        std::cerr << "Framebuffer not complete! Status: " << status << std::endl;
        return false;
    }
    
    // Unbind FBO and texture for now
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glBindTexture(GL_TEXTURE_2D, 0);
    
    return true;
}

// Function to check if a file is a video file
bool is_video_file(const std::string& path) {
    std::string ext = std::filesystem::path(path).extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    return ext == ".mp4" || ext == ".avi" || ext == ".mov" || ext == ".m4v" || ext == ".mkv";
}

// Function to initialize video
bool init_video(GLResources& res, const std::string& path) {
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
    video.frame = av_frame_alloc();
    video.frame_rgb = av_frame_alloc();
    if (!video.frame || !video.frame_rgb) {
        std::cerr << "Failed to allocate frames for: " << path << std::endl;
        av_frame_free(&video.frame);
        av_frame_free(&video.frame_rgb);
        avcodec_free_context(&video.codec_ctx);
        avformat_close_input(&video.format_ctx);
        return false;
    }
    
    // Get video dimensions
    video.width = video.codec_ctx->width;
    video.height = video.codec_ctx->height;
    
    // Allocate buffer for RGB frame
    int numBytes = av_image_get_buffer_size(AV_PIX_FMT_RGB24, video.width, video.height, 1);
    video.buffer = (uint8_t*)av_malloc(numBytes * sizeof(uint8_t));
    
    // Configure RGB frame
    av_image_fill_arrays(video.frame_rgb->data, video.frame_rgb->linesize, 
                         video.buffer, AV_PIX_FMT_RGB24, 
                         video.width, video.height, 1);
    
    // Initialize conversion context
    video.sws_ctx = sws_getContext(
        video.width, video.height, video.codec_ctx->pix_fmt,
        video.width, video.height, AV_PIX_FMT_RGB24,
        SWS_BILINEAR, nullptr, nullptr, nullptr
    );
    
    if (!video.sws_ctx) {
        std::cerr << "Failed to initialize scaling context for: " << path << std::endl;
        av_free(video.buffer);
        av_frame_free(&video.frame);
        av_frame_free(&video.frame_rgb);
        avcodec_free_context(&video.codec_ctx);
        avformat_close_input(&video.format_ctx);
        return false;
    }
    
    // Allocate packet
    video.packet = av_packet_alloc();
    if (!video.packet) {
        std::cerr << "Failed to allocate packet for: " << path << std::endl;
        sws_freeContext(video.sws_ctx);
        av_free(video.buffer);
        av_frame_free(&video.frame);
        av_frame_free(&video.frame_rgb);
        avcodec_free_context(&video.codec_ctx);
        avformat_close_input(&video.format_ctx);
        return false;
    }
    
    // Create OpenGL texture
    glGenTextures(1, &video.texture_id);
    glBindTexture(GL_TEXTURE_2D, video.texture_id);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, video.width, video.height, 0,
                GL_RGB, GL_UNSIGNED_BYTE, nullptr);
    
    GLenum err = glGetError();
    if (err != GL_NO_ERROR) {
        std::cerr << "OpenGL error when creating texture for " << path << ": " << err << std::endl;
        av_packet_free(&video.packet);
        sws_freeContext(video.sws_ctx);
        av_free(video.buffer);
        av_frame_free(&video.frame);
        av_frame_free(&video.frame_rgb);
        avcodec_free_context(&video.codec_ctx);
        avformat_close_input(&video.format_ctx);
        return false;
    }
    
    video.is_initialized = true;
    res.video_cache[path] = video;
    res.texture_cache[path] = video.texture_id;
    
    std::cout << "Successfully initialized video: " << path << " (" << video.width << "x" << video.height << ")" << std::endl;
    
    return true;
}

// Replace seek_video_frame with this improved function
bool update_video_frame(VideoData& video, float target_time_seconds) {
    // Initialize time_base if not set
    if (video.time_base == 0) {
        video.time_base = av_q2d(video.format_ctx->streams[video.video_stream_idx]->time_base);
    }
    
    // Reset if we need to go backward or if we're at EOF
    double target_pts = target_time_seconds / video.time_base;
    if (target_pts < video.current_pts || video.reached_eof) {
        // Seek to beginning
        int ret = av_seek_frame(video.format_ctx, video.video_stream_idx, 0, AVSEEK_FLAG_BACKWARD);
        if (ret < 0) {
            std::cerr << "Error seeking to beginning of video" << std::endl;
            return false;
        }
        avcodec_flush_buffers(video.codec_ctx);
        video.current_pts = 0;
        video.frames_read = 0;
        video.reached_eof = false;
        
        // If target is 0, we're done
        if (target_time_seconds <= 0) {
            return true;
        }
    }
    
    // Read frames until we reach target time
    bool frame_updated = false;
    
    while (!frame_updated && !video.reached_eof) {
        int ret = av_read_frame(video.format_ctx, video.packet);
        if (ret < 0) {
            if (ret == AVERROR_EOF) {
                video.reached_eof = true;
                
                // Flush buffered frames
                avcodec_send_packet(video.codec_ctx, nullptr);
            } else {
                std::cerr << "Error reading frame" << std::endl;
                return false;
            }
        } else if (video.packet->stream_index != video.video_stream_idx) {
            // Not a video packet
            av_packet_unref(video.packet);
            continue;
        } else {
            // Send the packet to the decoder
            ret = avcodec_send_packet(video.codec_ctx, video.packet);
            if (ret < 0) {
                std::cerr << "Error sending packet to decoder" << std::endl;
                av_packet_unref(video.packet);
                return false;
            }
            av_packet_unref(video.packet);
        }
        
        // Try to receive a frame
        while (true) {
            ret = avcodec_receive_frame(video.codec_ctx, video.frame);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                break;
            } else if (ret < 0) {
                std::cerr << "Error receiving frame from decoder" << std::endl;
                return false;
            }
            
            // Get the PTS (presentation timestamp)
            double pts = 0;
            if (video.frame->pts != AV_NOPTS_VALUE) {
                pts = video.frame->pts * video.time_base;
            } else {
                // If no PTS, estimate based on frame count
                pts = video.frames_read * (1.0 / av_q2d(video.format_ctx->streams[video.video_stream_idx]->r_frame_rate));
            }
            
            video.current_pts = pts;
            video.frames_read++;
            
            // Check if we've reached or passed our target time
            if (pts >= target_time_seconds || video.reached_eof) {
                // Convert the frame to RGB
                sws_scale(video.sws_ctx, video.frame->data, video.frame->linesize, 0,
                          video.height, video.frame_rgb->data, video.frame_rgb->linesize);
                
                // Update texture
                glBindTexture(GL_TEXTURE_2D, video.texture_id);
                glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, video.width, video.height,
                               GL_RGB, GL_UNSIGNED_BYTE, video.frame_rgb->data[0]);
                
                GLenum err = glGetError();
                if (err != GL_NO_ERROR) {
                    std::cerr << "OpenGL error when updating texture: " << err << std::endl;
                    return false;
                }
                
                frame_updated = true;
                break;
            }
        }
    }
    
    // If we've read at least one frame, consider it a success
    return video.frames_read > 0;
}

// Update load_textures function to handle both images and videos
static void load_textures(GLResources& res, const std::vector<Clip>& clips) {
    for (const auto& clip : clips) {
        if (res.texture_cache.count(clip.path)) continue;

        // Check if file exists
        if (!std::filesystem::exists(clip.path)) {
            std::cerr << "File does not exist: " << clip.path << std::endl;
            continue;
        }

        // If it's a video file, initialize it and continue
        if (is_video_file(clip.path)) {
            bool success = init_video(res, clip.path);
            if (success) {
                std::cout << "Video loaded: " << clip.path << std::endl;
            }
            continue;
        }

        // Handle images normally
        SDL_Surface* surface = IMG_Load(clip.path.c_str());
        if (!surface) {
            std::cerr << "Failed to load image: " << clip.path << " - " << SDL_GetError() << std::endl;
            continue;
        }

        // Debug output to check surface properties
        std::cout << "Image loaded: " << clip.path << std::endl;
        std::cout << "  Dimensions: " << surface->w << "x" << surface->h << std::endl;
        std::cout << "  Pitch: " << surface->pitch << std::endl;
        std::cout << "  Pixels: " << (surface->pixels ? "Valid" : "NULL") << std::endl;
        
        // Get format information for SDL3
        SDL_PixelFormat pixel_format = surface->format;
        int bpp = SDL_BYTESPERPIXEL(pixel_format);
        std::cout << "  Format: " << SDL_GetPixelFormatName(pixel_format) << " (" << bpp << " bytes per pixel)" << std::endl;

        GLuint tex_id = 0;
        glGenTextures(1, &tex_id);
        
        if (tex_id == 0) {
            std::cerr << "Failed to generate texture ID for: " << clip.path << std::endl;
            SDL_DestroySurface(surface);
            continue;
        }
        
        glBindTexture(GL_TEXTURE_2D, tex_id);
        
        // Determine the GL format based on SDL format
        GLenum internal_format, format;
        
        // Handle common SDL pixel formats
        if (pixel_format == SDL_PIXELFORMAT_RGB24) {
            internal_format = GL_RGB;
            format = GL_RGB;
        } else if (pixel_format == SDL_PIXELFORMAT_RGBA32) {
            internal_format = GL_RGBA;
            format = GL_RGBA;
        } else {
            // For other formats, use bytes per pixel as a fallback
            if (bpp == 4) {
                internal_format = GL_RGBA;
                format = GL_RGBA;
            } else if (bpp == 3) {
                internal_format = GL_RGB;
                format = GL_RGB;
            } else {
                std::cerr << "Unsupported image format in: " << clip.path << " (" << bpp << " bytes per pixel)" << std::endl;
                glDeleteTextures(1, &tex_id);
                SDL_DestroySurface(surface);
                continue;
            }
        }
        
        // Debug output
        std::cout << "  OpenGL format: " << (format == GL_RGB ? "GL_RGB" : "GL_RGBA") << std::endl;
        
        // Check if we need to convert surface format
        SDL_Surface* converted_surface = surface;
        if ((format == GL_RGB && pixel_format != SDL_PIXELFORMAT_RGB24) ||
            (format == GL_RGBA && pixel_format != SDL_PIXELFORMAT_RGBA32)) {
            
            SDL_PixelFormat target_format = (format == GL_RGB) ? SDL_PIXELFORMAT_RGB24 : SDL_PIXELFORMAT_RGBA32;
            converted_surface = SDL_ConvertSurface(surface, target_format);
            if (!converted_surface) {
                std::cerr << "Failed to convert surface format: " << SDL_GetError() << std::endl;
                glDeleteTextures(1, &tex_id);
                SDL_DestroySurface(surface);
                continue;
            }
            
            if (converted_surface != surface) {
                SDL_DestroySurface(surface);
                surface = converted_surface;
            }
            
            std::cout << "  Converted to: " << SDL_GetPixelFormatName(target_format) << std::endl;
        }
        
        glTexImage2D(GL_TEXTURE_2D, 0, internal_format, surface->w, surface->h, 0,
                    format, GL_UNSIGNED_BYTE, surface->pixels);
                    
        // Check for OpenGL errors
        GLenum err = glGetError();
        if (err != GL_NO_ERROR) {
            std::cerr << "OpenGL error when loading texture " << clip.path << ": " << err << std::endl;
            glDeleteTextures(1, &tex_id);
            SDL_DestroySurface(surface);
            continue;
        }
        
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

        res.texture_cache[clip.path] = tex_id;
        SDL_DestroySurface(surface);
        
        std::cout << "Successfully loaded texture: " << clip.path << " (ID: " << tex_id << ")" << std::endl;
    }
}

// Update render_frame to handle video frames
static void render_frame(GLResources& res, float current_time, 
                        const std::vector<Clip>& sorted_clips,
                        int width, int height) {
    // Bind the framebuffer for rendering
    glBindFramebuffer(GL_FRAMEBUFFER, res.fbo);
    glViewport(0, 0, width, height);
    
    // Clear the buffer with a distinct color to confirm rendering is happening
    glClearColor(0.2f, 0.0f, 0.2f, 1.0f); // Purple background to easily spot rendering issues
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    
    // Enable texturing and blending
    glEnable(GL_TEXTURE_2D);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    
    // Setup orthogonal projection
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(-1.0, 1.0, -1.0, 1.0, -1.0, 1.0);
    
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    
    // Debug variables
    bool rendered_any_clip = false;
    int clips_rendered = 0;
    
    // Render clips sorted by layer
    for (const auto& clip : sorted_clips) {
        if (current_time >= clip.start_time && 
            current_time <= (clip.start_time + clip.duration)) {
            
            std::cout << "Rendering clip: " << clip.path << " at time " << current_time << std::endl;
            
            // If it's a video clip, update the frame based on the current time
            if (is_video_file(clip.path)) {
                auto video_it = res.video_cache.find(clip.path);
                if (video_it != res.video_cache.end() && video_it->second.is_initialized) {
                    // Calculate the video-local time
                    float clip_time = current_time - clip.start_time;
                    // If it's a video clip, update the frame based on the current time
                    if (is_video_file(clip.path)) {
                        auto video_it = res.video_cache.find(clip.path);
                        if (video_it != res.video_cache.end() && video_it->second.is_initialized) {
                            // Calculate the video-local time
                            float clip_time = current_time - clip.start_time;
                            if (!update_video_frame(video_it->second, clip_time)) {
                                std::cerr << "Failed to update frame at time " << clip_time << " for " << clip.path << std::endl;
                                continue;
                            }
                        }
                    }
                }
            }
            
            auto it = res.texture_cache.find(clip.path);
            if (it != res.texture_cache.end()) {
                glBindTexture(GL_TEXTURE_2D, it->second);
                
                // Check for errors after binding texture
                GLenum err = glGetError();
                if (err != GL_NO_ERROR) {
                    std::cerr << "Error binding texture " << it->second << ": " << err << std::endl;
                    continue;
                }
                
                // Draw textured quad
                glBegin(GL_QUADS);
                // Set texture color to ensure visibility 
                glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
                glTexCoord2f(0.0f, 1.0f); glVertex2f(-1.0f, -1.0f);
                glTexCoord2f(1.0f, 1.0f); glVertex2f(1.0f, -1.0f);
                glTexCoord2f(1.0f, 0.0f); glVertex2f(1.0f, 1.0f);
                glTexCoord2f(0.0f, 0.0f); glVertex2f(-1.0f, 1.0f);
                glEnd();
                
                rendered_any_clip = true;
                clips_rendered++;
            } else {
                std::cerr << "Texture not found for " << clip.path << std::endl;
            }
        }
    }
    
    if (!rendered_any_clip) {
        std::cout << "No clips rendered at time " << current_time << std::endl;
    } else {
        std::cout << "Rendered " << clips_rendered << " clips at time " << current_time << std::endl;
    }
    
    // Disable states
    glDisable(GL_BLEND);
    glDisable(GL_TEXTURE_2D);
    
    // Unbind framebuffer
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

// Add cleanup for video resources to the start_video_export function
void cleanup_video_resources(GLResources& res) {
    for (auto& [path, video] : res.video_cache) {
        if (video.is_initialized) {
            av_packet_free(&video.packet);
            sws_freeContext(video.sws_ctx);
            av_free(video.buffer);
            av_frame_free(&video.frame);
            av_frame_free(&video.frame_rgb);
            avcodec_free_context(&video.codec_ctx);
            avformat_close_input(&video.format_ctx);
        }
    }
}

static int64_t get_default_channel_layout(int channels) {
    #if LIBAVUTIL_VERSION_MAJOR < 58
        return av_get_default_channel_layout(channels);
    #else
        switch (channels) {
            case 1: return AV_CH_LAYOUT_MONO;
            case 2: return AV_CH_LAYOUT_STEREO;
            case 6: return AV_CH_LAYOUT_5POINT1;
            default: return 0;
        }
    #endif
}

static bool init_audio(AudioData& audio, const std::string& path) {
    if (avformat_open_input(&audio.fmt_ctx, path.c_str(), nullptr, nullptr) < 0) {
        std::cerr << "Failed to open input audio file: " << path << std::endl;
        return false;
    }

    if (avformat_find_stream_info(audio.fmt_ctx, nullptr) < 0) {
        std::cerr << "Failed to find audio stream info: " << path << std::endl;
        return false;
    }

    audio.stream_index = av_find_best_stream(audio.fmt_ctx, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    if (audio.stream_index < 0) {
        std::cerr << "No audio stream found in: " << path << std::endl;
        return false;
    }

    AVCodecParameters* params = audio.fmt_ctx->streams[audio.stream_index]->codecpar;
    const AVCodec* decoder = avcodec_find_decoder(params->codec_id);
    if (!decoder) {
        std::cerr << "Failed to find decoder for audio: " << path << std::endl;
        return false;
    }

    audio.codec_ctx = avcodec_alloc_context3(decoder);
    if (!audio.codec_ctx) {
        std::cerr << "Failed to allocate codec context\n";
        return false;
    }

    if (avcodec_parameters_to_context(audio.codec_ctx, params) < 0) {
        std::cerr << "Failed to copy codec params\n";
        return false;
    }

    if (avcodec_open2(audio.codec_ctx, decoder, nullptr) < 0) {
        std::cerr << "Failed to open audio decoder\n";
        return false;
    }

    audio.frame = av_frame_alloc();
    audio.packet = av_packet_alloc();

    AVChannelLayout in_ch_layout = params->ch_layout;
    AVChannelLayout out_ch_layout;
    av_channel_layout_default(&out_ch_layout, 2);  // stereo output

    audio.swr_ctx = swr_alloc();
    if (!audio.swr_ctx) {
        std::cerr << "Failed to allocate SwrContext\n";
        return false;
    }

    av_opt_set_chlayout(audio.swr_ctx, "in_chlayout", &in_ch_layout, 0);
    av_opt_set_chlayout(audio.swr_ctx, "out_chlayout", &out_ch_layout, 0);
    av_opt_set_int(audio.swr_ctx, "in_sample_rate", params->sample_rate, 0);
    av_opt_set_int(audio.swr_ctx, "out_sample_rate", audio.sample_rate, 0);
    av_opt_set_sample_fmt(audio.swr_ctx, "in_sample_fmt", (AVSampleFormat)params->format, 0);
    av_opt_set_sample_fmt(audio.swr_ctx, "out_sample_fmt", audio.format, 0);

    if (swr_init(audio.swr_ctx) < 0) {
        std::cerr << "Failed to initialize SwrContext\n";
        swr_free(&audio.swr_ctx);
        av_channel_layout_uninit(&out_ch_layout);
        return false;
    }

    av_channel_layout_uninit(&out_ch_layout);  // free copied layout

    audio.channels = 2;
    audio.time_base = av_q2d(audio.fmt_ctx->streams[audio.stream_index]->time_base);

    std::cout << "Audio initialized: " << path << " | channels: " << audio.channels
              << ", sample_rate: " << audio.sample_rate << ", format: " << audio.format << std::endl;

    return true;
}


static int decode_audio_at_time(AudioData& audio, float time_sec, std::vector<uint8_t>& out_buffer) {
    if (!audio.swr_ctx || !swr_is_initialized(audio.swr_ctx)) {
        std::cerr << "SWResample context is not initialized\n";
        return 0;
    }

    const double pts_target = time_sec / audio.time_base;
    bool got_frame = false;
    int total_samples = 0;

    while (!audio.finished) {
        int ret = av_read_frame(audio.fmt_ctx, audio.packet);
        if (ret < 0) {
            audio.finished = true;
            break;
        }

        if (audio.packet->stream_index != audio.stream_index) {
            av_packet_unref(audio.packet);
            continue;
        }

        ret = avcodec_send_packet(audio.codec_ctx, audio.packet);
        av_packet_unref(audio.packet);
        if (ret < 0) {
            std::cerr << "Error sending audio packet to decoder\n";
            continue;
        }

        while (avcodec_receive_frame(audio.codec_ctx, audio.frame) == 0) {
            got_frame = true;

            int64_t frame_pts = (audio.frame->pts != AV_NOPTS_VALUE)
                ? audio.frame->pts
                : audio.frame->best_effort_timestamp;

            double frame_time = frame_pts * audio.time_base;

            std::cout << "Audio frame PTS: " << frame_pts << ", time: " << frame_time
                      << ", target: " << time_sec << std::endl;

            int nb_samples = audio.frame->nb_samples;
            if (nb_samples <= 0) continue;

            int buffer_size = nb_samples * audio.channels * av_get_bytes_per_sample(audio.format);
            out_buffer.resize(buffer_size);

            uint8_t* out[] = { out_buffer.data() };
            int out_samples = swr_convert(audio.swr_ctx, out, nb_samples,
                                          (const uint8_t**)audio.frame->data, nb_samples);

            total_samples += out_samples;
            return out_samples;
        }

        if (!got_frame) {
            continue; // decoder may need more packets
        }
    }

    return total_samples;
}


void cleanup_audio_resources(std::map<std::string, AudioData>& audio_cache) {
    for (auto& [_, a] : audio_cache) {
        av_packet_free(&a.packet);
        av_frame_free(&a.frame);
        swr_free(&a.swr_ctx);
        avcodec_free_context(&a.codec_ctx);
        avformat_close_input(&a.fmt_ctx);
    }
}

bool start_video_export(const std::string& output_path, int width, int height, int fps, int duration_frames, const std::vector<Clip>& clips, SDL_Window* window) {
    std::cout << "Starting video export process..." << std::endl;
    check_gl_context();

    SDL_GLContext current_context = SDL_GL_GetCurrentContext();
    if (!current_context) {
        std::cerr << "No current OpenGL context" << std::endl;
        return false;
    }

    GLint previous_viewport[4];
    glGetIntegerv(GL_VIEWPORT, previous_viewport);
    GLint previous_framebuffer;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &previous_framebuffer);
    GLint previous_texture;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &previous_texture);

    auto sorted_clips = clips;
    std::sort(sorted_clips.begin(), sorted_clips.end(), [](const Clip& a, const Clip& b) { return a.layer < b.layer; });

    GLResources res;
    if (!setup_gl_resources(res, width, height)) {
        std::cerr << "Failed to set up OpenGL resources" << std::endl;
        return false;
    }

    load_textures(res, sorted_clips);
    for (const auto& clip : sorted_clips) {
        if (is_video_file(clip.path)) {
            if (init_audio(res.audio_cache[clip.path], clip.path)) {
                std::cout << "Initialized audio for " << clip.path << std::endl;
            } else {
                std::cerr << "Failed to initialize audio for " << clip.path << std::endl;
            }
        }
    }

    std::ofstream video_file("video.raw", std::ios::binary);
    std::ofstream audio_file("temp_audio.raw", std::ios::binary);
    if (!video_file || !audio_file) {
        std::cerr << "Failed to open raw output files" << std::endl;
        return false;
    }

    std::vector<uint8_t> pixels(width * height * 3);
    std::vector<uint8_t> audio_mixed(44100 * 2 * 2 / fps);

    for (int frame = 0; frame < duration_frames; ++frame) {
        float current_time = static_cast<float>(frame) / fps;
        render_frame(res, current_time, sorted_clips, width, height);

        glBindFramebuffer(GL_FRAMEBUFFER, res.fbo);
        glReadBuffer(GL_COLOR_ATTACHMENT0);
        glReadPixels(0, 0, width, height, GL_RGB, GL_UNSIGNED_BYTE, pixels.data());

        const size_t row_size = width * 3;
        for (int y = 0; y < height / 2; ++y) {
            uint8_t* top = pixels.data() + y * row_size;
            uint8_t* bottom = pixels.data() + (height - 1 - y) * row_size;
            std::swap_ranges(top, top + row_size, bottom);
        }

        video_file.write(reinterpret_cast<const char*>(pixels.data()), pixels.size());

        std::fill(audio_mixed.begin(), audio_mixed.end(), 0);
        bool mixed_any = false;

        for (const auto& clip : sorted_clips) {
            if (current_time >= clip.start_time && current_time < clip.start_time + clip.duration) {
                float local_time = current_time - clip.start_time;
                std::cout << "[time " << current_time << "] checking clip: " << clip.path << " (start=" << clip.start_time << ", dur=" << clip.duration << ")" << std::endl;

                auto it = res.audio_cache.find(clip.path);
                if (it == res.audio_cache.end()) {
                    std::cout << "  [AUDIO MISSING in cache]" << std::endl;
                } else {
                    std::cout << "  [AUDIO FOUND in cache]" << std::endl;
                }

                if (it != res.audio_cache.end()) {
                    std::vector<uint8_t> temp;
                    int samples = decode_audio_at_time(it->second, local_time, temp);
                    std::cout << "Decoded " << samples << " samples from " << clip.path << " at t=" << local_time << std::endl;

                    if (samples > 0 && !temp.empty()) {
                        mixed_any = true;
                        for (size_t j = 0; j < temp.size() && j < audio_mixed.size(); ++j) {
                            int mixed = ((int16_t*)audio_mixed.data())[j / 2] + ((int16_t*)temp.data())[j / 2];
                            ((int16_t*)audio_mixed.data())[j / 2] = std::clamp(mixed, -32768, 32767);
                        }
                    }
                }
            }
        }

        bool nonzero = std::any_of(audio_mixed.begin(), audio_mixed.end(), [](uint8_t b) { return b != 0; });
        std::cout << "Frame " << frame << " audio: " << (mixed_any ? (nonzero ? "non-silent" : "silent") : "no mix") << std::endl;

        audio_file.write(reinterpret_cast<const char*>(audio_mixed.data()), audio_mixed.size());

        if (frame % 10 == 0 || frame == duration_frames - 1) {
            std::cout << "Exported frame " << frame + 1 << "/" << duration_frames << std::endl;
        }
    }

    video_file.close();
    audio_file.close();

    std::error_code ec;
    auto video_size = std::filesystem::file_size("video.raw", ec);
    auto audio_size = std::filesystem::file_size("temp_audio.raw", ec);
    std::cout << "Final video.raw size: " << video_size << " bytes\n";
    std::cout << "Final temp_audio.raw size: " << audio_size << " bytes\n";

    std::string ffmpeg_cmd =
        "ffmpeg -y "
        "-f rawvideo -pix_fmt rgb24 -video_size " + std::to_string(width) + "x" + std::to_string(height) +
        " -r " + std::to_string(fps) + " -i video.raw "
        "-f s16le -ac 2 -ar 44100 -i temp_audio.raw "
        "-shortest -vf format=yuv420p -c:v libx264 -preset fast -crf 18 "
        "-c:a aac -b:a 128k \"" + output_path + "\"";

    std::cout << "Running FFmpeg command:\n" << ffmpeg_cmd << std::endl;

    int ret = system(ffmpeg_cmd.c_str());
    if (ret != 0) {
        std::cerr << "FFmpeg export failed!" << std::endl;
    } else {
        std::cout << "Export completed successfully!" << std::endl;
    }

    std::filesystem::remove("video.raw");
    std::filesystem::remove("temp_audio.raw");

    glDeleteFramebuffers(1, &res.fbo);
    glDeleteTextures(1, &res.render_tex);
    for (auto& [path, tex] : res.texture_cache) {
        glDeleteTextures(1, &tex);
    }

    cleanup_audio_resources(res.audio_cache);
    cleanup_video_resources(res);

    glBindFramebuffer(GL_FRAMEBUFFER, previous_framebuffer);
    glViewport(previous_viewport[0], previous_viewport[1], previous_viewport[2], previous_viewport[3]);
    glBindTexture(GL_TEXTURE_2D, previous_texture);

    return true;
}