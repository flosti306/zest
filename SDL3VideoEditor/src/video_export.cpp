// Full OpenGL + FFmpeg Export Implementation for SDL3 + ImGui Video Editor
#include <glad/glad.h>
#include <SDL_opengl.h>
#include <cstdio>
#include <vector>
#include <functional>
#include <iostream>
#include <algorithm>
#include <filesystem>
#include <SDL.h>
#include <SDL_render.h>
#include <SDL_image.h>
#include <string>
#include <map>
#include "shared.hpp"
#include "video_export.hpp"

extern "C" {
    #include <libavcodec/avcodec.h>
    #include <libavformat/avformat.h>
    #include <libswscale/swscale.h>
    #include <libavutil/imgutils.h>
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
};

// Add a map for video data to the GLResources struct
struct GLResources {
    GLuint fbo = 0;
    GLuint render_tex = 0;
    std::map<std::string, GLuint> texture_cache;
    std::map<std::string, VideoData> video_cache;
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

// Function to seek to the nearest frame for the given time
bool seek_video_frame(VideoData& video, float time_seconds) {
    int64_t timestamp = (int64_t)(time_seconds * AV_TIME_BASE);
    int stream_index = video.video_stream_idx;
    int flags = AVSEEK_FLAG_BACKWARD;
    
    AVRational time_base = video.format_ctx->streams[stream_index]->time_base;
    int64_t seek_target = av_rescale_q(timestamp, AV_TIME_BASE_Q, time_base);
    
    if (av_seek_frame(video.format_ctx, stream_index, seek_target, flags) < 0) {
        std::cerr << "Error seeking video" << std::endl;
        return false;
    }
    
    avcodec_flush_buffers(video.codec_ctx);
    
    int got_frame = 0;
    while (!got_frame) {
        int ret = av_read_frame(video.format_ctx, video.packet);
        if (ret < 0) {
            std::cerr << "Error reading frame" << std::endl;
            return false;
        }
        
        if (video.packet->stream_index == stream_index) {
            ret = avcodec_send_packet(video.codec_ctx, video.packet);
            if (ret < 0) {
                av_packet_unref(video.packet);
                std::cerr << "Error sending packet to decoder" << std::endl;
                return false;
            }
            
            ret = avcodec_receive_frame(video.codec_ctx, video.frame);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                av_packet_unref(video.packet);
                continue;
            } else if (ret < 0) {
                av_packet_unref(video.packet);
                std::cerr << "Error receiving frame from decoder" << std::endl;
                return false;
            }
            
            got_frame = 1;
        }
        
        av_packet_unref(video.packet);
    }
    
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
    
    return true;
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
                    if (!seek_video_frame(video_it->second, clip_time)) {
                        std::cerr << "Failed to seek to frame at time " << clip_time << " for " << clip.path << std::endl;
                        continue;
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

bool start_video_export(const std::string& output_path,
    int width, int height, int fps,
    int duration_frames,
    const std::vector<Clip>& clips,
    SDL_Window* window) {

std::cout << "Starting video export process..." << std::endl;
check_gl_context();

// Use the existing context instead of creating a new one
SDL_GLContext current_context = SDL_GL_GetCurrentContext();
if (!current_context) {
std::cerr << "No current OpenGL context" << std::endl;
return false;
}

std::cout << "Using existing OpenGL context" << std::endl;

// Save the current OpenGL state we'll need to restore
GLint previous_viewport[4];
glGetIntegerv(GL_VIEWPORT, previous_viewport);

GLint previous_framebuffer;
glGetIntegerv(GL_FRAMEBUFFER_BINDING, &previous_framebuffer);

GLint previous_texture;
glGetIntegerv(GL_TEXTURE_BINDING_2D, &previous_texture);

// Sort clips by layer
auto sorted_clips = clips;
std::sort(sorted_clips.begin(), sorted_clips.end(),
[](const Clip& a, const Clip& b) { return a.layer < b.layer; });

// Set up OpenGL resources
GLResources res;
if (!setup_gl_resources(res, width, height)) {
std::cerr << "Failed to set up OpenGL resources" << std::endl;
return false;
}

std::cout << "OpenGL resources set up successfully" << std::endl;

// Load all textures
load_textures(res, sorted_clips);

// Set up FFmpeg process
std::string cmd = 
"ffmpeg -y -f rawvideo -pix_fmt rgb24 -video_size " +
std::to_string(width) + "x" + std::to_string(height) +
" -r " + std::to_string(fps) +
" -i - -vf format=yuv420p -c:v libx264 -preset fast -crf 18 \"" + 
output_path + "\"";

FILE* ffmpeg = _popen(cmd.c_str(), "wb");
if (!ffmpeg) {
std::cerr << "Failed to start FFmpeg process: " << strerror(errno) << std::endl;

// Clean up OpenGL resources
glDeleteFramebuffers(1, &res.fbo);
glDeleteTextures(1, &res.render_tex);
for (auto& [path, tex] : res.texture_cache) {
glDeleteTextures(1, &tex);
}

return false;
}

std::cout << "FFmpeg process started" << std::endl;

// Allocate pixel buffer for reading frames
std::vector<uint8_t> pixels(width * height * 3);

// Process each frame
for (int frame = 0; frame < duration_frames; ++frame) {
float current_time = static_cast<float>(frame) / fps;

// Render the current frame
render_frame(res, current_time, sorted_clips, width, height);

// Read pixels from framebuffer
glBindFramebuffer(GL_FRAMEBUFFER, res.fbo);
glReadBuffer(GL_COLOR_ATTACHMENT0);

GLenum err = glGetError();
if (err != GL_NO_ERROR) {
std::cerr << "OpenGL error before reading pixels: " << err << std::endl;
}

glReadPixels(0, 0, width, height, GL_RGB, GL_UNSIGNED_BYTE, pixels.data());

err = glGetError();
if (err != GL_NO_ERROR) {
std::cerr << "OpenGL error after reading pixels: " << err << std::endl;
}

// Flip the image vertically (OpenGL uses bottom-left origin)
const size_t row_size = width * 3;
for (int y = 0; y < height / 2; ++y) {
uint8_t* top = pixels.data() + y * row_size;
uint8_t* bottom = pixels.data() + (height - 1 - y) * row_size;
std::swap_ranges(top, top + row_size, bottom);
}

// Write frame data to FFmpeg
size_t written = fwrite(pixels.data(), 1, pixels.size(), ffmpeg);
if (written != pixels.size()) {
std::cerr << "Failed to write frame data to FFmpeg. Written: " << written << " of " << pixels.size() << std::endl;
_pclose(ffmpeg);

// Clean up OpenGL resources
glDeleteFramebuffers(1, &res.fbo);
glDeleteTextures(1, &res.render_tex);
for (auto& [path, tex] : res.texture_cache) {
glDeleteTextures(1, &tex);
}

// Restore OpenGL state
glBindFramebuffer(GL_FRAMEBUFFER, previous_framebuffer);
glViewport(previous_viewport[0], previous_viewport[1], previous_viewport[2], previous_viewport[3]);
glBindTexture(GL_TEXTURE_2D, previous_texture);

return false;
}

if (frame % 10 == 0 || frame == duration_frames - 1) {
std::cout << "Exported frame " << frame + 1 << "/" << duration_frames << std::endl;
}
}

// Clean up FFmpeg
fflush(ffmpeg);
_pclose(ffmpeg);
std::cout << "FFmpeg process closed" << std::endl;

// Clean up OpenGL resources
glDeleteFramebuffers(1, &res.fbo);
glDeleteTextures(1, &res.render_tex);

for (auto& [path, tex] : res.texture_cache) {
glDeleteTextures(1, &tex);
}

// Clean up OpenGL resources
glDeleteFramebuffers(1, &res.fbo);
glDeleteTextures(1, &res.render_tex);

for (auto& [path, tex] : res.texture_cache) {
    glDeleteTextures(1, &tex);
}

// Clean up video resources
cleanup_video_resources(res);

// Restore OpenGL state
glBindFramebuffer(GL_FRAMEBUFFER, previous_framebuffer);
glViewport(previous_viewport[0], previous_viewport[1], previous_viewport[2], previous_viewport[3]);
glBindTexture(GL_TEXTURE_2D, previous_texture);

std::cout << "OpenGL state restored" << std::endl;

return true;
}