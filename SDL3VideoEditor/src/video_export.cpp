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

bool update_video_frame(VideoData& video, float target_time_seconds) {
    // Make sure time base is initialized
    if (video.time_base == 0) {
        video.time_base = av_q2d(video.format_ctx->streams[video.video_stream_idx]->time_base);
    }

    // Convert target time in seconds to stream time base using av_rescale_q
    int64_t seek_target = av_rescale_q(
        static_cast<int64_t>(target_time_seconds * AV_TIME_BASE),
        AVRational{1, AV_TIME_BASE},
        video.format_ctx->streams[video.video_stream_idx]->time_base
    );

    // Seek if target is before current or we reached EOF
    if (target_time_seconds < video.current_pts || video.reached_eof) {
        int ret = av_seek_frame(video.format_ctx, video.video_stream_idx, seek_target, AVSEEK_FLAG_BACKWARD);
        if (ret < 0) {
            std::cerr << "Error seeking to time: " << target_time_seconds << std::endl;
            return false;
        }

        avcodec_flush_buffers(video.codec_ctx);
        video.current_pts = 0;
        video.frames_read = 0;
        video.reached_eof = false;
    }

    bool frame_updated = false;

    while (!frame_updated && !video.reached_eof) {
        int ret = av_read_frame(video.format_ctx, video.packet);
        if (ret < 0) {
            if (ret == AVERROR_EOF) {
                video.reached_eof = true;
                avcodec_send_packet(video.codec_ctx, nullptr);
            } else {
                std::cerr << "Error reading frame\n";
                return false;
            }
        } else if (video.packet->stream_index != video.video_stream_idx) {
            av_packet_unref(video.packet);
            continue;
        } else {
            ret = avcodec_send_packet(video.codec_ctx, video.packet);
            av_packet_unref(video.packet);
            if (ret < 0) {
                std::cerr << "Error sending packet\n";
                return false;
            }
        }

        while (true) {
            ret = avcodec_receive_frame(video.codec_ctx, video.frame);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
            if (ret < 0) {
                std::cerr << "Error receiving frame\n";
                return false;
            }

            double pts = 0;
            if (video.frame->pts != AV_NOPTS_VALUE) {
                pts = video.frame->pts * video.time_base;
            } else {
                pts = video.frames_read * (1.0 / av_q2d(video.format_ctx->streams[video.video_stream_idx]->r_frame_rate));
            }

            video.current_pts = pts;
            video.frames_read++;

            if (pts >= target_time_seconds || video.reached_eof) {
                sws_scale(video.sws_ctx, video.frame->data, video.frame->linesize, 0,
                          video.height, video.frame_rgb->data, video.frame_rgb->linesize);

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

    return frame_updated;
}


// Update load_textures function to handle both images and videos
void load_textures(GLResources& res, const std::vector<Clip>& clips) {
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
void render_frame(GLResources& res, float current_time, 
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
                    float clip_time = current_time - clip.start_time + clip.media_start;
                    // If it's a video clip, update the frame based on the current time
                    if (is_video_file(clip.path)) {
                        auto video_it = res.video_cache.find(clip.path);
                        if (video_it != res.video_cache.end() && video_it->second.is_initialized) {
                            // Calculate the video-local time
                            float clip_time = current_time - clip.start_time;
                            if (!update_video_frame(video_it->second, clip_time + clip.media_start)) {
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
                
                glPushMatrix();

                // Apply transform based on clip attributes
                glTranslatef(clip.pos_x, clip.pos_y, 0.0f);
                glScalef(clip.scale, clip.scale, 1.0f);

                // Apply opacity via color
                glColor4f(1.0f, 1.0f, 1.0f, clip.opacity);

                // Draw textured quad (size -1 to 1 in normalized space)
                glBegin(GL_QUADS);
                    glTexCoord2f(0.0f, 1.0f); glVertex2f(-1.0f, -1.0f);
                    glTexCoord2f(1.0f, 1.0f); glVertex2f( 1.0f, -1.0f);
                    glTexCoord2f(1.0f, 0.0f); glVertex2f( 1.0f,  1.0f);
                    glTexCoord2f(0.0f, 0.0f); glVertex2f(-1.0f,  1.0f);
                glEnd();

                glPopMatrix();
                
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


bool preload_audio_file(const std::string& path, PreloadedAudio& out, float media_start = 0.0f, float media_duration = -1.0f) {
    AVFormatContext* fmt_ctx = nullptr;
    if (avformat_open_input(&fmt_ctx, path.c_str(), nullptr, nullptr) < 0) {
        std::cerr << "Failed to open input: " << path << "\n";
        return false;
    }

    if (avformat_find_stream_info(fmt_ctx, nullptr) < 0) {
        std::cerr << "Failed to find stream info\n";
        avformat_close_input(&fmt_ctx);
        return false;
    }

    int stream_index = av_find_best_stream(fmt_ctx, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    if (stream_index < 0) {
        std::cerr << "No audio stream found\n";
        avformat_close_input(&fmt_ctx);
        return false;
    }

    AVCodecParameters* params = fmt_ctx->streams[stream_index]->codecpar;
    const AVCodec* decoder = avcodec_find_decoder(params->codec_id);
    AVCodecContext* codec_ctx = avcodec_alloc_context3(decoder);
    avcodec_parameters_to_context(codec_ctx, params);
    avcodec_open2(codec_ctx, decoder, nullptr);

    SwrContext* swr = swr_alloc();
    av_opt_set_chlayout(swr, "in_chlayout", &params->ch_layout, 0);
    AVChannelLayout stereo;
    av_channel_layout_default(&stereo, 2);
    av_opt_set_chlayout(swr, "out_chlayout", &stereo, 0);
    av_opt_set_int(swr, "in_sample_rate", params->sample_rate, 0);
    av_opt_set_int(swr, "out_sample_rate", 44100, 0);
    av_opt_set_sample_fmt(swr, "in_sample_fmt", (AVSampleFormat)params->format, 0);
    av_opt_set_sample_fmt(swr, "out_sample_fmt", AV_SAMPLE_FMT_S16, 0);
    swr_init(swr);

    // Seek to media_start
    if (media_start > 0.0f) {
        int64_t seek_target = av_rescale_q(media_start * AV_TIME_BASE, AVRational{1, AV_TIME_BASE}, fmt_ctx->streams[stream_index]->time_base);
        av_seek_frame(fmt_ctx, stream_index, seek_target, AVSEEK_FLAG_BACKWARD);
        avcodec_flush_buffers(codec_ctx);
    }

    AVPacket* pkt = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();

    std::vector<int16_t> full_audio;

    float current_time = 0.0f;
    float target_end = (media_duration > 0.0f) ? (media_start + media_duration) : std::numeric_limits<float>::max();
    float time_base = av_q2d(fmt_ctx->streams[stream_index]->time_base);

    while (av_read_frame(fmt_ctx, pkt) >= 0) {
        if (pkt->stream_index != stream_index) {
            av_packet_unref(pkt);
            continue;
        }

        avcodec_send_packet(codec_ctx, pkt);
        av_packet_unref(pkt);

        while (avcodec_receive_frame(codec_ctx, frame) == 0) {
            float pts = (frame->pts != AV_NOPTS_VALUE) ? frame->pts * time_base : current_time;

            if (pts < media_start) {
                continue; // skip early frames
            }
            if (pts > target_end) {
                goto done; // break both loops
            }

            int out_nb_samples = av_rescale_rnd(
                swr_get_delay(swr, codec_ctx->sample_rate) + frame->nb_samples,
                44100, codec_ctx->sample_rate, AV_ROUND_UP);

            std::vector<int16_t> temp(out_nb_samples * 2);
            uint8_t* out_data[] = { (uint8_t*)temp.data() };
            int samples = swr_convert(swr, out_data, out_nb_samples, (const uint8_t**)frame->data, frame->nb_samples);
            if (samples > 0) {
                full_audio.insert(full_audio.end(), temp.begin(), temp.begin() + samples * 2);
            }

            current_time = pts + (float)samples / 44100.0f;
        }
    }

done:
    out.samples = std::move(full_audio);
    out.sample_rate = 44100;
    out.channels = 2;
    out.duration = (float)out.samples.size() / (2.0f * 44100.0f); // stereo

    av_channel_layout_uninit(&stereo);
    av_frame_free(&frame);
    av_packet_free(&pkt);
    avcodec_free_context(&codec_ctx);
    swr_free(&swr);
    avformat_close_input(&fmt_ctx);
    return true;
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

    // Preload audio
    for (const auto& clip : sorted_clips) {
        if (is_video_file(clip.path)) {
            PreloadedAudio preload;
            if (preload_audio_file(clip.path, preload, clip.media_start, clip.duration)) {
                res.preloaded_audio[clip.path] = std::move(preload);
            } else {
                std::cerr << "Failed to preload audio for " << clip.path << std::endl;
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
    const int audio_sample_rate = 44100;
    const int audio_channels = 2;
    const int bytes_per_sample = 2;
    const int samples_per_frame = audio_sample_rate / fps;
    std::vector<uint8_t> audio_mixed(samples_per_frame * audio_channels * bytes_per_sample);
    std::vector<float> audio_float(samples_per_frame * audio_channels, 0.0f);

    for (int frame = 0; frame < duration_frames; ++frame) {
        float current_time = static_cast<float>(frame) / fps;
        render_frame(res, current_time, sorted_clips, width, height);

        // Capture frame pixels
        glBindFramebuffer(GL_FRAMEBUFFER, res.fbo);
        glReadBuffer(GL_COLOR_ATTACHMENT0);
        glReadPixels(0, 0, width, height, GL_RGB, GL_UNSIGNED_BYTE, pixels.data());

        // Flip vertically (OpenGL is bottom-left origin)
        size_t row_size = width * 3;
        for (int y = 0; y < height / 2; ++y) {
            uint8_t* top = pixels.data() + y * row_size;
            uint8_t* bottom = pixels.data() + (height - 1 - y) * row_size;
            std::swap_ranges(top, top + row_size, bottom);
        }

        video_file.write(reinterpret_cast<const char*>(pixels.data()), pixels.size());

        // Mix audio from preloaded buffers
        std::fill(audio_float.begin(), audio_float.end(), 0.0f);
        for (const auto& clip : sorted_clips) {
            if (current_time >= clip.start_time && current_time < clip.start_time + clip.duration) {
                float local_time = current_time - clip.start_time;
                auto it = res.preloaded_audio.find(clip.path);
                if (it != res.preloaded_audio.end()) {
                    const PreloadedAudio& audio = it->second;
                    int start_sample = static_cast<int>(local_time * audio.sample_rate);
                    int max_samples = samples_per_frame;

                    for (int i = 0; i < max_samples; ++i) {
                        int sample_idx = start_sample + i;
                        if (sample_idx * audio.channels + 1 >= (int)audio.samples.size())
                            break;

                        for (int ch = 0; ch < audio.channels; ++ch) {
                            int16_t s = audio.samples[sample_idx * audio.channels + ch];
                            audio_float[i * audio.channels + ch] += s / 32768.0f;
                        }
                    }
                }
            }
        }

        // Convert float to int16 and write
        for (size_t j = 0; j < audio_float.size(); ++j) {
            float sample = std::clamp(audio_float[j], -1.0f, 1.0f);
            ((int16_t*)audio_mixed.data())[j] = static_cast<int16_t>(sample * 32767.0f);
        }

        audio_file.write(reinterpret_cast<const char*>(audio_mixed.data()), audio_mixed.size());

        if (frame % 10 == 0 || frame == duration_frames - 1) {
            std::cout << "Exported frame " << frame + 1 << "/" << duration_frames << std::endl;
        }
    }

    video_file.close();
    audio_file.close();

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

    cleanup_video_resources(res);

    glBindFramebuffer(GL_FRAMEBUFFER, previous_framebuffer);
    glViewport(previous_viewport[0], previous_viewport[1], previous_viewport[2], previous_viewport[3]);
    glBindTexture(GL_TEXTURE_2D, previous_texture);

    return true;
}