// Full OpenGL + FFmpeg Export Implementation for SDL3 + ImGui Video Editor
#include "video_export.hpp"
#include "effects.hpp"
#include "glm/glm.hpp"
#include "shared.hpp"
#include <SDL.h>
#include <SDL_image.h>
#include <SDL_opengl.h>
#include <SDL_render.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <glad/glad.h>
#include <imgui.h>
#include <iostream>
#include <limits>
#include <map>
#include <string>
#include <thread>
#include <tinyfiledialogs.h>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/audio_fifo.h>
#include <libavutil/channel_layout.h>
#include <libavutil/hwcontext.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

std::thread thumbnail_worker;
std::queue<ThumbnailRequest> thumbnail_request_queue;
std::queue<ThumbnailResult> thumbnail_result_queue;
std::mutex request_mutex;
std::mutex result_mutex;
std::condition_variable worker_cv;
std::atomic<bool> stop_thumbnail_worker_flag = false;

std::thread decoder_thread;
std::deque<DecodedFrameRequest> decoder_request_queue;
std::queue<DecodedFrameResult> decoder_result_queue;
std::mutex decoder_request_mutex;
std::mutex decoder_result_mutex;
std::condition_variable decoder_worker_cv;
std::atomic<bool> stop_decoder_worker_flag = false;

// Add request deduplication
std::mutex pending_requests_mutex;
std::set<std::pair<std::string, double>> global_pending_requests;

std::mutex playback_perf_mutex;
PlaybackPerfStats playback_perf_stats;

PlaybackPerfStats get_playback_perf_stats() {
  std::lock_guard<std::mutex> lock(playback_perf_mutex);
  return playback_perf_stats;
}

void reset_playback_perf_stats() {
  std::lock_guard<std::mutex> lock(playback_perf_mutex);
  playback_perf_stats = PlaybackPerfStats{};
}

void start_thumbnail_worker();
void stop_thumbnail_worker();
bool decode_frame_at_timestamp(DecoderState *state, double timestamp,
                               DecodedFrameResult &result);

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

bool setup_gl_resources(GLResources &res, int width, int height) {
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
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, width, height, 0, GL_RGB,
               GL_UNSIGNED_BYTE, nullptr);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                         res.render_tex, 0);

  GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
  if (status != GL_FRAMEBUFFER_COMPLETE) {
    std::cerr << "Framebuffer not complete! Status: " << status << std::endl;
    glBindFramebuffer(GL_FRAMEBUFFER, 0); // Unbind before returning
    glDeleteTextures(1, &res.render_tex);
    res.render_tex = 0;
    glDeleteFramebuffers(1, &res.fbo);
    res.fbo = 0;
    return false;
  }

  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  glBindTexture(GL_TEXTURE_2D, 0);
  std::cout << "GL resources initialized successfully." << std::endl;
  return true;
}

// Function to check if a file is a video file
bool is_video_file(const std::string &path) {
  std::string ext = std::filesystem::path(path).extension().string();
  std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
  return ext == ".mp4" || ext == ".avi" || ext == ".mov" || ext == ".m4v" ||
         ext == ".mkv";
}

// Function to initialize video
bool initialize_video_resources(GLResources &res, const std::string &path) {
  if (res.video_cache.count(path)) {
    return res.video_cache[path].is_initialized;
  }

  VideoData video;
  video.consecutive_cache_hits = 0;
  video.is_actively_playing = false;
  video.last_request_time = std::chrono::steady_clock::now();

  // Open the file
  video.format_ctx = avformat_alloc_context();
  if (avformat_open_input(&video.format_ctx, path.c_str(), nullptr, nullptr) !=
      0) {
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
    if (video.format_ctx->streams[i]->codecpar->codec_type ==
        AVMEDIA_TYPE_VIDEO) {
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
  AVCodecParameters *codec_params =
      video.format_ctx->streams[video.video_stream_idx]->codecpar;

  // Find decoder
  const AVCodec *codec = avcodec_find_decoder(codec_params->codec_id);
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

  for (int i = 0;; i++) {
    const AVCodecHWConfig *config = avcodec_get_hw_config(codec, i);
    if (!config)
      break;
    if (config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX) {
      if (config->device_type == AV_HWDEVICE_TYPE_CUDA) {
        if (av_hwdevice_ctx_create(&video.hw_device_ctx, config->device_type,
                                   NULL, NULL, 0) >= 0) {
          video.hw_pix_fmt = config->pix_fmt;
          video.codec_ctx->hw_device_ctx = av_buffer_ref(video.hw_device_ctx);
          std::cout << "Hardware acceleration enabled for " << path << " (CUDA)"
                    << std::endl;
          break;
        }
      }
    }
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
  if (!video.frame) {             /* error handling */
    avcodec_free_context(&video.codec_ctx);
    avformat_close_input(&video.format_ctx);
    return false;
  }

  video.width = video.codec_ctx->width;
  video.height = video.codec_ctx->height;

  // Initialize SWS context for YUV->RGB conversion
  video.sws_ctx =
      sws_getContext(video.width, video.height, video.codec_ctx->pix_fmt,
                     video.width, video.height, AV_PIX_FMT_RGB24, // Target RGB
                     SWS_BILINEAR, nullptr, nullptr, nullptr);
  if (!video.sws_ctx) { /* error handling */
    av_frame_free(&video.frame);
    avcodec_free_context(&video.codec_ctx);
    avformat_close_input(&video.format_ctx);
    return false;
  }

  video.packet = av_packet_alloc();
  if (!video.packet) { /* error handling */
    sws_freeContext(video.sws_ctx);
    av_frame_free(&video.frame);
    avcodec_free_context(&video.codec_ctx);
    avformat_close_input(&video.format_ctx);
    return false;
  }

  // Store time base and duration
  video.time_base =
      video.format_ctx->streams[video.video_stream_idx]->time_base;
  video.duration_ts =
      video.format_ctx->streams[video.video_stream_idx]->duration;
  if (video.duration_ts > 0 && video.time_base.den > 0) {
    video.duration_sec =
        static_cast<double>(video.duration_ts) * av_q2d(video.time_base);
  } else if (video.format_ctx->duration != AV_NOPTS_VALUE) {
    video.duration_sec =
        static_cast<double>(video.format_ctx->duration) / AV_TIME_BASE;
  } else {
    video.duration_sec = 0.0; // Unknown duration
    std::cerr << "Warning: Could not determine duration for " << path
              << std::endl;
  }

  // --- PBO INITIALIZATION ---
  // Calculate the size needed for one frame's pixel data.
  video.pbo_buffer_size = video.width * video.height * 3; // For RGB24

  // Create two PBOs for double-buffering.
  glGenBuffers(2, video.pbos);

  // Initialize the first PBO
  glBindBuffer(GL_PIXEL_UNPACK_BUFFER, video.pbos[0]);
  glBufferData(GL_PIXEL_UNPACK_BUFFER, video.pbo_buffer_size, 0,
               GL_STREAM_DRAW);

  // Initialize the second PBO
  glBindBuffer(GL_PIXEL_UNPACK_BUFFER, video.pbos[1]);
  glBufferData(GL_PIXEL_UNPACK_BUFFER, video.pbo_buffer_size, 0,
               GL_STREAM_DRAW);

  // Unbind the PBO
  glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
  // --- END PBO INITIALIZATION ---

  // Create OpenGL texture (initially empty)
  glGenTextures(1, &video.texture_id);
  glBindTexture(GL_TEXTURE_2D, video.texture_id);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, video.width, video.height, 0, GL_RGB,
               GL_UNSIGNED_BYTE, nullptr);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glBindTexture(GL_TEXTURE_2D, 0); // Unbind

  video.is_initialized = true;
  auto [it, success] = res.video_cache.emplace(path, std::move(video));
  if (!success) {
    // Handle error: Element with this path already existed? Or emplace failed?
    std::cerr << "Error: Failed to emplace VideoData into cache for " << path
              << std::endl;
    // Need to clean up partially initialized 'video' object before returning
    // false
    av_packet_free(&video.packet);
    sws_freeContext(video.sws_ctx);
    av_frame_free(&video.frame);
    avcodec_free_context(&video.codec_ctx);
    avformat_close_input(&video.format_ctx);
    // Also delete the GL texture if created
    if (video.texture_id)
      glDeleteTextures(1, &video.texture_id);
    return false;
  }
  // If successful, the 'video' object has been moved from, use the iterator
  // 'it'
  std::cout << "Initialized video resources for: " << path
            << " (ID: " << it->second.texture_id << ")" << std::endl;

  glBindTexture(GL_TEXTURE_2D, 0); // Unbind texture (was after the move before)
  return true;
}

// Function to load necessary resources (textures, ffmpeg contexts) for a
// specific clip
void load_resources_for_clip(GLResources &res, const Clip &clip) {
  if (is_video_file(clip.path)) {
    if (!res.video_cache.count(clip.path) ||
        !res.video_cache[clip.path].is_initialized) {
      initialize_video_resources(res, clip.path);
    }
    // Optionally initialize audio resources here too if needed for streaming
    // if (clip.type == ClipType::Audio || !clip.is_audio_only) {
    //     initialize_audio_resources(res, clip.path);
    // }
    // Preload audio for waveform/export if not already done
    if ((clip.type == ClipType::Audio || !clip.is_audio_only) &&
        !res.preloaded_audio.count(clip.path)) {
      PreloadedAudio audio;
      if (preload_audio_file(clip.path, audio, 0.0f,
                             -1.0f)) { // Preload entire file for now
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

bool ensure_video_decoded_upto(VideoData &video, double target_time_seconds) {
  if (!video.is_initialized)
    return false;
  if (video.is_seeking)
    return false; // Don't decode while another operation is in progress

  // --- Seeking Logic ---
  // Check if a seek is necessary. This is the case if the target time is
  // significantly behind the current position or completely outside our small
  // frame cache.
  bool needs_seek = false;
  if (video.last_decoded_pts < 0) { // Not decoded anything yet
    needs_seek = true;
  } else if (video.frame_cache.empty()) { // Cache is empty, might as well seek
    needs_seek = true;
  }
  // Check if target is outside the current cache's time range
  else if (target_time_seconds < video.frame_cache.front().pts -
                                     0.5 || // Target is before the cache starts
           target_time_seconds >
               video.frame_cache.back().pts +
                   1.0) { // Target is significantly after cache end
    needs_seek = true;
  }

  if (needs_seek) {
    video.is_seeking = true; // Signal start of seek

    // Perform the seek
    int64_t seek_target_ts =
        av_rescale_q(static_cast<int64_t>(target_time_seconds * AV_TIME_BASE),
                     AV_TIME_BASE_Q, video.time_base);

    // Seek to the keyframe *before* the target time
    int ret = av_seek_frame(video.format_ctx, video.video_stream_idx,
                            seek_target_ts, AVSEEK_FLAG_BACKWARD);

    if (ret < 0) {
      char errbuf[AV_ERROR_MAX_STRING_SIZE];
      av_make_error_string(errbuf, sizeof(errbuf), ret);
      std::cerr << "Warning: Error seeking video " << video.format_ctx->url
                << " to " << target_time_seconds << "s: " << errbuf
                << std::endl;
    }

    // CRITICAL: Always flush the decoder's buffers after a seek.
    avcodec_flush_buffers(video.codec_ctx);
    // The cache is now invalid, clear it.
    video.frame_cache.clear();
    // Reset our tracking of the last decoded frame. This is important.
    video.last_decoded_pts = -1.0;

    // std::cout << "Seeked video " << video.format_ctx->url << " to ~" <<
    // target_time_seconds << "s" << std::endl;

    video.is_seeking = false; // Signal end of seek
  }

  // --- Sequential Decoding Logic ---
  // This part now runs correctly for both smooth playback (incrementing a few
  // frames) and after a seek (decoding from the new keyframe to the target).
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
    double pts_sec =
        (video.frame->pts == AV_NOPTS_VALUE) ? video.last_decoded_pts
                                             : // Best guess if PTS is missing
            static_cast<double>(video.frame->pts) * av_q2d(video.time_base);

    // After a seek, the first few frames might have odd timestamps. We only
    // want to move forward.
    if (video.last_decoded_pts > 0 && pts_sec < video.last_decoded_pts) {
      av_frame_unref(video.frame);
      continue;
    }

    AVFrame *frame_to_convert = video.frame;
    AVFrame *sw_frame = nullptr;

    if (video.frame->format == video.hw_pix_fmt) {
      sw_frame = av_frame_alloc();
      if (av_hwframe_transfer_data(sw_frame, video.frame, 0) < 0) {
        av_frame_free(&sw_frame);
        av_frame_unref(video.frame);
        return false;
      }
      frame_to_convert = sw_frame;
    }

    // Convert and cache the frame
    DecodedFrame decoded;
    decoded.width = video.width;
    decoded.height = video.height;
    decoded.pts = pts_sec;
    decoded.pixels.resize(video.width * video.height * 3); // RGB24

    AVColorRange color_range = frame_to_convert->color_range;
    AVColorSpace color_space = frame_to_convert->colorspace;

    if (color_range == AVCOL_RANGE_UNSPECIFIED)
      color_range = AVCOL_RANGE_MPEG;
    if (color_space == AVCOL_SPC_UNSPECIFIED)
      color_space = AVCOL_SPC_BT709;

    video.sws_ctx = sws_getCachedContext(
        video.sws_ctx, frame_to_convert->width, frame_to_convert->height,
        (AVPixelFormat)frame_to_convert->format, video.width, video.height,
        AV_PIX_FMT_RGB24, SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);

    if (video.sws_ctx) {
      sws_setColorspaceDetails(video.sws_ctx, sws_getCoefficients(color_space),
                               color_range,
                               sws_getCoefficients(AVCOL_SPC_BT709),
                               AVCOL_RANGE_MPEG, 0, 1 << 16, 1 << 16);
    }

    uint8_t *dest_data[4] = {decoded.pixels.data(), nullptr, nullptr, nullptr};
    int dest_linesize[4] = {video.width * 3, 0, 0, 0};
    if (video.sws_ctx) {
      sws_scale(video.sws_ctx, frame_to_convert->data,
                frame_to_convert->linesize, 0, frame_to_convert->height,
                dest_data, dest_linesize);
    }

    if (sw_frame)
      av_frame_free(&sw_frame);

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

// Finds the best frame in cache and uploads it ASYNCHRONOUSLY to the texture
// via PBOs.
bool update_texture_from_cache(VideoData &video, double target_time_seconds,
                               bool strict = false) {
  if (!video.is_initialized || video.frame_cache.empty() ||
      video.pbos[0] == 0) {
    std::lock_guard<std::mutex> perf_lock(playback_perf_mutex);
    playback_perf_stats.texture_upload_misses++;
    return false;
  }

  const DecodedFrame *best_frame = nullptr;
  double min_diff = std::numeric_limits<double>::max();
  for (const auto &frame : video.frame_cache) {
    double diff = std::abs(frame.pts - target_time_seconds);
    if (diff < min_diff) {
      min_diff = diff;
      best_frame = &frame;
    }
  }

  // --- STRICT MODE: Only show if exact frame is available ---
  if (strict && (min_diff > 0.05)) { // 5ms tolerance
    std::lock_guard<std::mutex> perf_lock(playback_perf_mutex);
    playback_perf_stats.texture_upload_misses++;
    return false; // Don't update texture, wait for exact frame
  }

  if (!best_frame) {
    std::lock_guard<std::mutex> perf_lock(playback_perf_mutex);
    playback_perf_stats.texture_upload_misses++;
    return false;
  }

  // ...existing PBO upload logic...
  const auto upload_start = std::chrono::steady_clock::now();
  glBindTexture(GL_TEXTURE_2D, video.texture_id);
  video.pbo_index = (video.pbo_index + 1) % 2;
  int current_pbo_id = video.pbos[video.pbo_index];
  glBindBuffer(GL_PIXEL_UNPACK_BUFFER, current_pbo_id);
  glBufferData(GL_PIXEL_UNPACK_BUFFER, video.pbo_buffer_size, 0,
               GL_STREAM_DRAW);
  GLubyte *pbo_ptr =
      (GLubyte *)glMapBuffer(GL_PIXEL_UNPACK_BUFFER, GL_WRITE_ONLY);
  if (pbo_ptr) {
    memcpy(pbo_ptr, best_frame->pixels.data(), video.pbo_buffer_size);
    glUnmapBuffer(GL_PIXEL_UNPACK_BUFFER);
  } else {
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
    std::lock_guard<std::mutex> perf_lock(playback_perf_mutex);
    playback_perf_stats.texture_upload_misses++;
    return false;
  }
  glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, video.width, video.height, GL_RGB,
                  GL_UNSIGNED_BYTE, (void *)0);
  glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
  glBindTexture(GL_TEXTURE_2D, 0);

  const double upload_ms =
      std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() -
                                                upload_start)
          .count();
  {
    std::lock_guard<std::mutex> perf_lock(playback_perf_mutex);
    playback_perf_stats.texture_upload_hits++;
    const double prev_samples =
        static_cast<double>(playback_perf_stats.texture_upload_hits - 1);
    playback_perf_stats.avg_texture_upload_ms =
        ((playback_perf_stats.avg_texture_upload_ms * prev_samples) +
         upload_ms) /
        static_cast<double>(playback_perf_stats.texture_upload_hits);
  }

  return true;
}

// Loads ONLY image textures
void load_textures(GLResources &res, const std::vector<Clip> &clips) {
  for (const auto &clip : clips) {
    // Skip if already loaded or if it's a video file (handled separately)
    if (res.texture_cache.count(clip.path) || is_video_file(clip.path)) {
      continue;
    }

    if (!std::filesystem::exists(clip.path)) {
      std::cerr << "Image file does not exist: " << clip.path << std::endl;
      continue;
    }

    SDL_Surface *surface = IMG_Load(clip.path.c_str());
    if (!surface) {
      std::cerr << "Failed to load image: " << clip.path << " - "
                << SDL_GetError() << std::endl;
      continue;
    }

    SDL_Surface *formatted_surface =
        nullptr;                       // Surface to use for texture upload
    SDL_PixelFormat target_format_sdl; // Target SDL pixel format value
    GLenum gl_format, gl_internal_format;

    // Determine target format (prefer RGBA if alpha exists)
    // In SDL3, surface->format is the SDL_PixelFormat value directly
    bool has_alpha = SDL_ISPIXELFORMAT_ALPHA(surface->format);

    if (has_alpha) {
      target_format_sdl = SDL_PIXELFORMAT_RGBA32; // Common 32-bit RGBA format
      gl_format = GL_RGBA;
      gl_internal_format = GL_RGBA;
    } else {
      target_format_sdl = SDL_PIXELFORMAT_RGB24; // Common 24-bit RGB format
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
        std::cerr << "Failed to convert image surface for " << clip.path << ": "
                  << SDL_GetError() << std::endl;
        SDL_DestroySurface(surface); // Free original surface
        continue;
      }
      SDL_DestroySurface(
          surface); // Free original surface after successful conversion
    } else {
      // No conversion needed, use the original surface
      formatted_surface = surface;
    }

    GLuint tex_id = 0;
    glGenTextures(1, &tex_id);
    if (tex_id == 0) {
      std::cerr << "Failed to generate texture ID for " << clip.path
                << std::endl;
      SDL_DestroySurface(formatted_surface);
      continue;
    }
    glBindTexture(GL_TEXTURE_2D, tex_id);

    // Upload pixel data
    glTexImage2D(GL_TEXTURE_2D, 0, gl_internal_format, formatted_surface->w,
                 formatted_surface->h, 0, gl_format, GL_UNSIGNED_BYTE,
                 formatted_surface->pixels);

    GLenum err = glGetError();
    if (err != GL_NO_ERROR) {
      std::cerr << "OpenGL error creating texture for " << clip.path << ": "
                << err << std::endl;
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
    std::cout << "Loaded image texture: " << clip.path << " (ID: " << tex_id
              << ")" << std::endl;
  }
  glBindTexture(GL_TEXTURE_2D, 0); // Unbind texture unit
}

// Renders using *existing* textures. Does NO decoding.
void render_frame(GLResources &res, float current_time,
                  const std::vector<Clip> &sorted_clips, int width, int height,
                  int fps) {

  glBindFramebuffer(GL_FRAMEBUFFER, res.fbo);
  glViewport(0, 0, width, height);
  glClearColor(0.0f, 0.0f, 0.0f, 0.0f); // Clear with transparent black
  glClear(
      GL_COLOR_BUFFER_BIT); // No depth buffer needed for 2D compositing usually

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
  for (const auto &clip : sorted_clips) {
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
      // Overlay needs a shader normally. Approximate with multiply/screen
      // hybrid
      glBlendFunc(GL_SRC_ALPHA, GL_ONE);
      break;
    }

    // Check if clip is active and is visual (Video type)
    if (clip.type == ClipType::Video && current_time >= clip.start_time &&
        current_time < (clip.start_time + clip.duration)) {
      float local_time =
          current_time - clip.start_time; // relative to clip start

      // --- Evaluate keyframes ---
      float evaluated_pos_x = clip.pos_x_track.Evaluate(local_time);
      float evaluated_pos_y = clip.pos_y_track.Evaluate(local_time);
      float evaluated_scale = clip.scale_track.Evaluate(local_time);
      float evaluated_rotation = clip.rotation_track.Evaluate(local_time);
      float evaluated_opacity = clip.opacity_track.Evaluate(local_time);

      // Fallback / Clamp
      if (clip.pos_x_track.keyframes.empty())
        evaluated_pos_x = clip.pos_x;
      if (clip.pos_y_track.keyframes.empty())
        evaluated_pos_y = clip.pos_y;
      if (clip.scale_track.keyframes.empty())
        evaluated_scale = clip.scale;
      if (clip.rotation_track.keyframes.empty())
        evaluated_rotation = clip.rotation;
      if (clip.opacity_track.keyframes.empty())
        evaluated_opacity = clip.opacity;

      evaluated_scale = std::max(0.0f, evaluated_scale); // Never allow 0 scale
      evaluated_opacity =
          std::clamp(evaluated_opacity, 0.0f, 1.0f); // Clamp opacity

      GLuint tex_id = 0;
      bool is_video = is_video_file(clip.path);

      // Find the texture
      if (clip.path != "Composition") {
        if (is_video) {
          auto vid_it = res.video_cache.find(clip.path);
          if (vid_it != res.video_cache.end() &&
              vid_it->second.is_initialized) {
            tex_id = vid_it->second.texture_id;
          }
        } else {
          auto img_it = res.texture_cache.find(clip.path);
          if (img_it != res.texture_cache.end()) {
            tex_id = img_it->second;
          }
        }
      } else {
        tex_id = -1; // special id for compositions
      }

      if (tex_id != 0) {
        // If the clip has effects, we need to handle them differently
        if (clip.has_effects && clip.effect_graph) {
          // --- BEST PRACTICE: Disable blending when rendering into temp FBOs
          // ---
          glDisable(GL_BLEND);
          // ---

          // --- STAGE 1: Bake transforms into a temporary texture ---
          GLuint transformed_tex;
          GLuint transformed_fbo =
              create_temp_fbo(glm::vec2(width, height), transformed_tex);

          glBindFramebuffer(GL_FRAMEBUFFER, transformed_fbo);
          glViewport(0, 0, width, height);
          glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
          glClear(GL_COLOR_BUFFER_BIT);

          glBindTexture(GL_TEXTURE_2D, tex_id);
          glPushMatrix();
          glTranslatef(evaluated_pos_x * width, evaluated_pos_y * height, 0.0f);
          glRotatef(evaluated_rotation, 0.0f, 0.0f, 1.0f);
          glScalef(evaluated_scale, evaluated_scale, 1.0f);
          glColor4f(1.0f, 1.0f, 1.0f, 1.0f);

          glBegin(GL_QUADS);
          glTexCoord2f(0.0f, 0.0f);
          glVertex2f(-1.0f * width, -1.0f * height);
          glTexCoord2f(1.0f, 0.0f);
          glVertex2f(1.0f * width, -1.0f * height);
          glTexCoord2f(1.0f, 1.0f);
          glVertex2f(1.0f * width, 1.0f * height);
          glTexCoord2f(0.0f, 1.0f);
          glVertex2f(-1.0f * width, 1.0f * height);
          glEnd();
          glPopMatrix();

          // --- STAGE 2: Process the effect chain ---
          GLuint final_effect_tex;
          GLuint final_effect_fbo =
              create_temp_fbo(glm::vec2(width, height), final_effect_tex);
          clip.effect_graph->ProcessNodeGraph(transformed_tex, final_effect_fbo,
                                              current_time,
                                              glm::vec2(width, height), fps);

          // --- STAGE 3: Composite the result onto the main FBO ---

          // --- BEST PRACTICE: Re-enable blending for the final composite ---
          glEnable(GL_BLEND);
          // Set the correct blend function for compositing
          glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
          // ---

          glBindFramebuffer(GL_FRAMEBUFFER, res.fbo);
          glViewport(0, 0, width, height);
          glBindTexture(GL_TEXTURE_2D, final_effect_tex);
          glColor4f(1.0f, 1.0f, 1.0f, evaluated_opacity);

          glPushMatrix();
          glLoadIdentity();
          glBegin(GL_QUADS);
          glTexCoord2f(0.0f, 0.0f);
          glVertex2f(-1.0f * width, -1.0f * height);
          glTexCoord2f(1.0f, 0.0f);
          glVertex2f(1.0f * width, -1.0f * height);
          glTexCoord2f(1.0f, 1.0f);
          glVertex2f(1.0f * width, 1.0f * height);
          glTexCoord2f(0.0f, 1.0f);
          glVertex2f(-1.0f * width, 1.0f * height);
          glEnd();
          glPopMatrix();

          // --- STAGE 4: Clean up temporary resources ---
          destroy_temp_fbo(transformed_fbo, transformed_tex);
          destroy_temp_fbo(final_effect_fbo, final_effect_tex);

        } else {
          // Original rendering path for clips with NO effects
          // Make sure blending is enabled for normal clips too
          glEnable(GL_BLEND);
          glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

          glBindTexture(GL_TEXTURE_2D, tex_id);
          glPushMatrix();

          // Apply transforms
          glTranslatef(evaluated_pos_x * width, evaluated_pos_y * height, 0.0f);
          glRotatef(evaluated_rotation, 0.0f, 0.0f, 1.0f);
          glScalef(evaluated_scale, evaluated_scale, 1.0f);
          glColor4f(1.0f, 1.0f, 1.0f, evaluated_opacity);

          // Draw the quad
          glBegin(GL_QUADS);
          glTexCoord2f(0.0f, 0.0f);
          glVertex2f(-1.0f * width, -1.0f * height);
          glTexCoord2f(1.0f, 0.0f);
          glVertex2f(1.0f * width, -1.0f * height);
          glTexCoord2f(1.0f, 1.0f);
          glVertex2f(1.0f * width, 1.0f * height);
          glTexCoord2f(0.0f, 1.0f);
          glVertex2f(-1.0f * width, 1.0f * height);
          glEnd();

          glPopMatrix();
        }

        rendered_any = true;
      } else {
        // Optionally render a placeholder if texture is missing/not ready
        // std::cerr << "Texture ID 0 for active clip: " << clip.path <<
        // std::endl;
      }
    }
  }

  // Reset color and disable states
  glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
  glDisable(GL_BLEND);
  glDisable(GL_TEXTURE_2D);
  glBindTexture(GL_TEXTURE_2D, 0);
  glBindFramebuffer(GL_FRAMEBUFFER, 0);

  // if (!rendered_any) { std::cout << "Rendered empty frame at " <<
  // current_time << std::endl; }
}

// --- Cleanup ---
void cleanup_video_resources(GLResources &res) {
  std::cout << "Cleaning up video resources..." << std::endl;
  for (auto &[path, video] : res.video_cache) {
    if (video.is_initialized) {
      std::cout << "  Cleaning: " << path << std::endl;
      av_packet_free(&video.packet);
      sws_freeContext(video.sws_ctx);
      av_frame_free(&video.frame);
      // av_frame_free(&video.frame_rgb); // Removed
      // av_free(video.buffer);          // Removed
      avcodec_free_context(&video.codec_ctx);
      avformat_close_input(&video.format_ctx);
      if (video.hw_device_ctx) {
        av_buffer_unref(&video.hw_device_ctx);
        video.hw_device_ctx = nullptr;
      }

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

void cleanup_gl_resources(GLResources &res) {
  std::cout << "Cleaning up GL resources..." << std::endl;
  if (res.fbo) {
    glDeleteFramebuffers(1, &res.fbo);
    res.fbo = 0;
  }
  if (res.render_tex) {
    glDeleteTextures(1, &res.render_tex);
    res.render_tex = 0;
  }
  for (auto const &[path, tex_id] : res.texture_cache) {
    glDeleteTextures(1, &tex_id);
  }
  res.texture_cache.clear();

  // Clean up thumbnail textures
  std::cout << "Cleaning up thumbnail textures..." << std::endl;
  for (auto &[path, tex_vec] : res.clip_thumbnail_textures) {
    for (GLuint tex_id : tex_vec) {
      if (tex_id)
        glDeleteTextures(1, &tex_id);
    }
  }
  res.clip_thumbnail_textures.clear();
  res.generated_thumbnails_map.clear(); // Clear the tracking map too

  // Also delete video textures managed by VideoData
  for (auto const &[path, video_data] : res.video_cache) {
    if (video_data.texture_id) {
      glDeleteTextures(1, &video_data.texture_id);
      // No need to modify video_data here as it will be cleared
    }
  }
  // The video_cache map itself is cleared in cleanup_video_resources
  std::cout << "GL resources cleaned." << std::endl;
}

// --- Audio Preloading (Keep as is, but ensure it's called appropriately) ---
bool preload_audio_file(const std::string &path, PreloadedAudio &out,
                        float media_start, float media_duration) {
  AVFormatContext *fmt_ctx = nullptr;
  if (avformat_open_input(&fmt_ctx, path.c_str(), nullptr, nullptr) <
      0) { /* error */
    return false;
  }
  if (avformat_find_stream_info(fmt_ctx, nullptr) < 0) { /* error */
    avformat_close_input(&fmt_ctx);
    return false;
  }
  int stream_index =
      av_find_best_stream(fmt_ctx, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
  if (stream_index < 0) { /* no audio */
    avformat_close_input(&fmt_ctx);
    return false;
  }

  AVCodecParameters *params = fmt_ctx->streams[stream_index]->codecpar;
  const AVCodec *decoder = avcodec_find_decoder(params->codec_id);
  if (!decoder) { /* error */
    avformat_close_input(&fmt_ctx);
    return false;
  }

  AVCodecContext *codec_ctx = avcodec_alloc_context3(decoder);
  if (!codec_ctx) { /* error */
    avformat_close_input(&fmt_ctx);
    return false;
  }
  if (avcodec_parameters_to_context(codec_ctx, params) < 0) { /* error */
    avcodec_free_context(&codec_ctx);
    avformat_close_input(&fmt_ctx);
    return false;
  }
  if (avcodec_open2(codec_ctx, decoder, nullptr) < 0) { /* error */
    avcodec_free_context(&codec_ctx);
    avformat_close_input(&fmt_ctx);
    return false;
  }

  // --- Channel Layout Handling ---
  AVChannelLayout out_ch_layout = AV_CHANNEL_LAYOUT_STEREO; // Target layout

  AVChannelLayout in_ch_layout;
  av_channel_layout_default(&in_ch_layout,
                            0); // Initialize with default layout for 0 channels
  av_channel_layout_uninit(&in_ch_layout); // Ensure clean state

  // Try copying the layout from the input parameters
  int layout_ret = av_channel_layout_copy(&in_ch_layout, &params->ch_layout);

  // If copy failed or resulted in invalid layout, try fallback
  if (layout_ret < 0 || in_ch_layout.nb_channels <= 0) {
    std::cerr << "Warning: Invalid or uncopyable input channel layout for "
              << path << ". Guessing based on channel count ("
              << params->ch_layout.nb_channels << ")." << std::endl;

    // Use static const layouts to copy FROM
    static const AVChannelLayout static_layout_mono = AV_CHANNEL_LAYOUT_MONO;
    static const AVChannelLayout static_layout_stereo =
        AV_CHANNEL_LAYOUT_STEREO;

    if (params->ch_layout.nb_channels == 1) {
      av_channel_layout_copy(&in_ch_layout, &static_layout_mono);
    } else { // Default to stereo for 2 or more/unknown channels
      av_channel_layout_copy(&in_ch_layout, &static_layout_stereo);
      if (params->ch_layout.nb_channels != 2) {
        std::cerr << "Warning: Defaulting to Stereo layout for "
                  << params->ch_layout.nb_channels << " channels." << std::endl;
      }
    }
    // Check again if the fallback copy worked
    if (in_ch_layout.nb_channels <= 0) {
      std::cerr << "Error: Failed to set a valid input channel layout even "
                   "with fallback for "
                << path << std::endl;
      av_channel_layout_uninit(&in_ch_layout);
      avcodec_free_context(&codec_ctx);
      avformat_close_input(&fmt_ctx);
      return false;
    }
  }
  // --- End Channel Layout Handling ---

  SwrContext *swr = nullptr;
  int swr_ret = swr_alloc_set_opts2(
      &swr, &out_ch_layout, AV_SAMPLE_FMT_S16, 44100, &in_ch_layout,
      (AVSampleFormat)params->format, params->sample_rate, 0, nullptr);

  if (swr_ret < 0 || !swr || swr_init(swr) < 0) {
    std::cerr << "Failed to create or init SwrContext for " << path
              << std::endl;
    av_channel_layout_uninit(&in_ch_layout); // Cleanup allocated layout memory
    avcodec_free_context(&codec_ctx);
    avformat_close_input(&fmt_ctx);
    swr_free(&swr);
    return false;
  }

  // Seek logic (if needed)
  if (media_start > 0.0f) {
    int64_t seek_target_ts =
        av_rescale_q(static_cast<int64_t>(media_start * AV_TIME_BASE),
                     AV_TIME_BASE_Q, fmt_ctx->streams[stream_index]->time_base);
    if (av_seek_frame(fmt_ctx, stream_index, seek_target_ts,
                      AVSEEK_FLAG_BACKWARD) < 0) {
      std::cerr << "Warning: Failed to seek audio in " << path << " to "
                << media_start << "s" << std::endl;
    }
    avcodec_flush_buffers(codec_ctx); // Flush after seek
  }

  AVPacket *pkt = av_packet_alloc();
  AVFrame *frame = av_frame_alloc();
  if (!pkt || !frame) { /* error handling */ /* cleanup */
    return false;
  }

  std::vector<int16_t> full_audio;
  double current_time_sec = 0.0;
  double time_base_sec = av_q2d(fmt_ctx->streams[stream_index]->time_base);
  double target_end_sec = (media_duration > 0.0f)
                              ? (media_start + media_duration)
                              : std::numeric_limits<double>::max();

  // --- Decoding Loop ---
  while (av_read_frame(fmt_ctx, pkt) >= 0) {
    if (pkt->stream_index != stream_index) {
      av_packet_unref(pkt);
      continue;
    }

    int send_ret = avcodec_send_packet(codec_ctx, pkt);
    av_packet_unref(pkt); // Always unref packet
    if (send_ret < 0) {   /* error handling */
      break;
    }

    while (true) {
      int recv_ret = avcodec_receive_frame(codec_ctx, frame);
      if (recv_ret == AVERROR(EAGAIN) || recv_ret == AVERROR_EOF)
        break;            // Need more input or EOF
      if (recv_ret < 0) { /* error handling */
        goto done_preload;
      } // Error receiving

      double pts_sec = (frame->pts != AV_NOPTS_VALUE)
                           ? frame->pts * time_base_sec
                           : current_time_sec;

      if (pts_sec < media_start) {
        av_frame_unref(frame);
        continue;
      } // Skip frames before start
      if (pts_sec >= target_end_sec) {
        av_frame_unref(frame);
        goto done_preload;
      } // Reached end duration

      // Estimate output samples (can be generous)
      int estimated_out_samples = av_rescale_rnd(
          swr_get_delay(swr, params->sample_rate) + frame->nb_samples, 44100,
          params->sample_rate, AV_ROUND_UP);
      const int buffer_size = estimated_out_samples *
                              out_ch_layout.nb_channels *
                              av_get_bytes_per_sample(AV_SAMPLE_FMT_S16);
      if (buffer_size <= 0) {
        av_frame_unref(frame);
        continue;
      } // Avoid invalid buffer size

      uint8_t *out_buf[1];
      out_buf[0] = (uint8_t *)av_malloc(buffer_size);
      if (!out_buf[0]) { /* memory error */
        av_frame_unref(frame);
        goto done_preload;
      }

      int actual_out_samples =
          swr_convert(swr, out_buf, estimated_out_samples,
                      (const uint8_t **)frame->data, frame->nb_samples);

      if (actual_out_samples > 0) {
        full_audio.insert(full_audio.end(), (int16_t *)out_buf[0],
                          (int16_t *)out_buf[0] +
                              actual_out_samples * out_ch_layout.nb_channels);
      }
      av_free(out_buf[0]);
      av_frame_unref(frame); // Unref frame after use

      // Update time tracking (approximation is okay here)
      current_time_sec =
          pts_sec + (double)frame->nb_samples / params->sample_rate;
    }
  }

done_preload:
  // --- Flush Decoder ---
  avcodec_send_packet(codec_ctx, nullptr); // Send flush packet
  while (true) {
    int flush_ret = avcodec_receive_frame(codec_ctx, frame);
    if (flush_ret == AVERROR(EAGAIN) || flush_ret == AVERROR_EOF)
      break;
    if (flush_ret < 0)
      break; // Error flushing

    // Check pts against target_end_sec even during flush
    double pts_sec = (frame->pts != AV_NOPTS_VALUE) ? frame->pts * time_base_sec
                                                    : current_time_sec;
    if (pts_sec >= target_end_sec && media_duration > 0.0f) {
      av_frame_unref(frame);
      continue;
    }

    int estimated_out_samples = av_rescale_rnd(
        frame->nb_samples, 44100, params->sample_rate, AV_ROUND_UP);
    const int buffer_size = estimated_out_samples * out_ch_layout.nb_channels *
                            av_get_bytes_per_sample(AV_SAMPLE_FMT_S16);
    if (buffer_size <= 0) {
      av_frame_unref(frame);
      continue;
    }

    uint8_t *out_buf[1];
    out_buf[0] = (uint8_t *)av_malloc(buffer_size);
    if (!out_buf[0]) {
      av_frame_unref(frame);
      break;
    } // Error

    int actual_out_samples =
        swr_convert(swr, out_buf, estimated_out_samples,
                    (const uint8_t **)frame->data, frame->nb_samples);
    if (actual_out_samples > 0) {
      full_audio.insert(full_audio.end(), (int16_t *)out_buf[0],
                        (int16_t *)out_buf[0] +
                            actual_out_samples * out_ch_layout.nb_channels);
    }
    av_free(out_buf[0]);
    av_frame_unref(frame);
  }
  // --- End Flush ---

  out.samples = std::move(full_audio);
  out.sample_rate = 44100;
  out.channels = out_ch_layout.nb_channels; // Use actual output channels
  out.duration =
      (float)out.samples.size() /
      (float)std::max(1, out.channels *
                             out.sample_rate); // Prevent division by zero
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

void mix_audio_from_memory(const PreloadedAudio &audio, float time_sec,
                           std::vector<float> &mix_buffer) {
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
// --- Video Export Optimization ---
// 1. Pre-mix audio to a temp file (FFmpeg needs a seekable/stable audio source
// usually, or a second pipe which is complex).
// 2. Pipe video frames directly to FFmpeg stdin to avoid massive disk I/O.
// 3. Use PBOs for asynchronous GPU readback.

bool start_video_export(const std::string &output_path, int width, int height,
                        int fps, int duration_frames,
                        const std::vector<Clip> &clips, SDL_Window *window) {

  std::cout << "Starting optimized video export using FFmpeg C-API..."
            << std::endl;
  SDL_GLContext current_context = SDL_GL_GetCurrentContext();
  if (!current_context) {
    std::cerr << "No GL context!" << std::endl;
    return false;
  }

  GLResources export_res;
  if (!setup_gl_resources(export_res, width, height)) {
    std::cerr << "Failed to setup GL resources for export." << std::endl;
    return false;
  }

  // Load all necessary resources (textures, video/audio contexts, preload
  // audio)
  for (const auto &clip : clips) {
    load_resources_for_clip(export_res, clip);
  }

  // Sort clips by layer once
  auto sorted_clips = clips;
  std::sort(sorted_clips.begin(), sorted_clips.end(),
            [](const Clip &a, const Clip &b) { return a.layer < b.layer; });

  AVFormatContext *fmt_ctx = nullptr;
  if (avformat_alloc_output_context2(&fmt_ctx, nullptr, nullptr,
                                     output_path.c_str()) < 0)
    return false;

  const AVCodec *video_codec = avcodec_find_encoder_by_name("h264_nvenc");
  if (!video_codec) {
    std::cerr << "Warning: h264_nvenc not found, falling back to libx264"
              << std::endl;
    video_codec = avcodec_find_encoder(AV_CODEC_ID_H264);
    if (!video_codec)
      return false;
  }

  AVStream *video_stream = avformat_new_stream(fmt_ctx, video_codec);
  AVCodecContext *video_enc_ctx = avcodec_alloc_context3(video_codec);
  video_enc_ctx->width = width;
  video_enc_ctx->height = height;
  video_enc_ctx->time_base = {1, fps};
  video_enc_ctx->framerate = {fps, 1};
  video_enc_ctx->pix_fmt = AV_PIX_FMT_YUV420P;
  video_stream->time_base = {1, fps};
  if (fmt_ctx->oformat->flags & AVFMT_GLOBALHEADER)
    video_enc_ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

  if (std::string(video_codec->name) == "h264_nvenc") {
    av_opt_set(video_enc_ctx->priv_data, "preset", "p4", 0);
    av_opt_set(video_enc_ctx->priv_data, "cq", "22", 0);
  } else {
    av_opt_set(video_enc_ctx->priv_data, "preset", "fast", 0);
    av_opt_set(video_enc_ctx->priv_data, "crf", "22", 0);
  }
  if (avcodec_open2(video_enc_ctx, video_codec, nullptr) < 0) {
    std::cerr << "Failed to open video codec." << std::endl;
    return false;
  }
  avcodec_parameters_from_context(video_stream->codecpar, video_enc_ctx);

  const AVCodec *audio_codec = avcodec_find_encoder(AV_CODEC_ID_AAC);
  AVStream *audio_stream = avformat_new_stream(fmt_ctx, audio_codec);
  AVCodecContext *audio_enc_ctx = avcodec_alloc_context3(audio_codec);
  audio_enc_ctx->sample_rate = 44100;

  AVChannelLayout ch_layout = AV_CHANNEL_LAYOUT_STEREO;
  av_channel_layout_copy(&audio_enc_ctx->ch_layout, &ch_layout);
  audio_enc_ctx->sample_fmt = audio_codec->sample_fmts
                                  ? audio_codec->sample_fmts[0]
                                  : AV_SAMPLE_FMT_FLTP;
  audio_enc_ctx->bit_rate = 192000;
  audio_stream->time_base = {1, audio_enc_ctx->sample_rate};

  if (fmt_ctx->oformat->flags & AVFMT_GLOBALHEADER)
    audio_enc_ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

  if (avcodec_open2(audio_enc_ctx, audio_codec, nullptr) < 0) {
    std::cerr << "Failed to open audio codec." << std::endl;
    return false;
  }
  avcodec_parameters_from_context(audio_stream->codecpar, audio_enc_ctx);

  if (!(fmt_ctx->oformat->flags & AVFMT_NOFILE)) {
    if (avio_open(&fmt_ctx->pb, output_path.c_str(), AVIO_FLAG_WRITE) < 0)
      return false;
  }
  if (avformat_write_header(fmt_ctx, nullptr) < 0)
    return false;

  SwsContext *sws_ctx = sws_getContext(
      width, height, AV_PIX_FMT_RGB24, width, height, AV_PIX_FMT_YUV420P,
      SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);
  AVFrame *video_frame = av_frame_alloc();
  video_frame->format = video_enc_ctx->pix_fmt;
  video_frame->width = width;
  video_frame->height = height;
  av_frame_get_buffer(video_frame, 0);

  AVFrame *audio_frame = av_frame_alloc();
  audio_frame->format = audio_enc_ctx->sample_fmt;
  audio_frame->nb_samples =
      audio_enc_ctx->frame_size ? audio_enc_ctx->frame_size : 1024;
  av_channel_layout_copy(&audio_frame->ch_layout, &audio_enc_ctx->ch_layout);
  audio_frame->sample_rate = audio_enc_ctx->sample_rate;
  av_frame_get_buffer(audio_frame, 0);

  SwrContext *swr_ctx = nullptr;
  swr_alloc_set_opts2(&swr_ctx, &audio_enc_ctx->ch_layout,
                      audio_enc_ctx->sample_fmt, audio_enc_ctx->sample_rate,
                      &ch_layout, AV_SAMPLE_FMT_FLT, 44100, 0, nullptr);
  swr_init(swr_ctx);

  AVPacket *pkt = av_packet_alloc();

  int samples_per_frame = 44100 / fps;
  std::vector<float> audio_mix_buffer(samples_per_frame * 2);
  AVAudioFifo *audio_fifo = av_audio_fifo_alloc(AV_SAMPLE_FMT_FLT, 2, 1);

  int64_t next_video_pts = 0;
  int64_t next_audio_pts = 0;

  auto encode_write_frame = [&](AVCodecContext *enc_ctx, AVFrame *frame,
                                AVStream *stream) {
    if (avcodec_send_frame(enc_ctx, frame) < 0)
      return;
    while (avcodec_receive_packet(enc_ctx, pkt) >= 0) {
      av_packet_rescale_ts(pkt, enc_ctx->time_base, stream->time_base);
      pkt->stream_index = stream->index;
      av_interleaved_write_frame(fmt_ctx, pkt);
      av_packet_unref(pkt);
    }
  };

  GLuint pbos[2];
  glGenBuffers(2, pbos);
  glBindBuffer(GL_PIXEL_PACK_BUFFER, pbos[0]);
  glBufferData(GL_PIXEL_PACK_BUFFER, width * height * 3, 0, GL_STREAM_READ);
  glBindBuffer(GL_PIXEL_PACK_BUFFER, pbos[1]);
  glBufferData(GL_PIXEL_PACK_BUFFER, width * height * 3, 0, GL_STREAM_READ);
  glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);

  int pbo_index = 0, next_pbo_index = 1;

  for (int frame_idx = 0; frame_idx <= duration_frames; ++frame_idx) {
    if (frame_idx < duration_frames) {
      float current_time = static_cast<float>(frame_idx) / fps;
      std::fill(audio_mix_buffer.begin(), audio_mix_buffer.end(), 0.0f);

      for (const auto &clip : sorted_clips) {
        if (clip.type == ClipType::Audio && current_time >= clip.start_time &&
            current_time < (clip.start_time + clip.duration)) {
          auto it = export_res.preloaded_audio.find(clip.path);
          if (it != export_res.preloaded_audio.end()) {
            const auto &audio = it->second;
            float time_into = current_time - clip.start_time + clip.media_start;
            int start_idx = static_cast<int>(time_into * audio.sample_rate);
            float vol = clip.volume_track.keyframes.empty()
                            ? clip.volume
                            : clip.volume_track.Evaluate(current_time -
                                                         clip.start_time) *
                                  clip.volume;
            for (int i = 0; i < samples_per_frame; ++i) {
              if ((start_idx + i) * audio.channels + 1 < audio.samples.size()) {
                audio_mix_buffer[i * 2] +=
                    (audio.samples[(start_idx + i) * audio.channels] /
                     32768.0f) *
                    vol;
                audio_mix_buffer[i * 2 + 1] +=
                    (audio.samples[(start_idx + i) * audio.channels + 1] /
                     32768.0f) *
                    vol;
              }
            }
          }
        }
      }
      void *input_data[] = {(void *)audio_mix_buffer.data()};
      if (audio_fifo) {
        av_audio_fifo_realloc(audio_fifo, av_audio_fifo_size(audio_fifo) +
                                              samples_per_frame);
        av_audio_fifo_write(audio_fifo, input_data, samples_per_frame);

        while (av_audio_fifo_size(audio_fifo) >= audio_frame->nb_samples) {
          float *temp_fifo_buf = new float[audio_frame->nb_samples * 2];
          void *fifo_read_data[] = {temp_fifo_buf};
          av_audio_fifo_read(audio_fifo, fifo_read_data,
                             audio_frame->nb_samples);

          uint8_t **converted_data = nullptr;
          av_samples_alloc_array_and_samples(
              &converted_data, nullptr, audio_enc_ctx->ch_layout.nb_channels,
              audio_frame->nb_samples, audio_enc_ctx->sample_fmt, 0);
          swr_convert(swr_ctx, converted_data, audio_frame->nb_samples,
                      (const uint8_t **)fifo_read_data,
                      audio_frame->nb_samples);
          av_samples_copy(
              audio_frame->data, converted_data, 0, 0, audio_frame->nb_samples,
              audio_enc_ctx->ch_layout.nb_channels, audio_enc_ctx->sample_fmt);

          av_freep(&converted_data[0]);
          av_freep(&converted_data);
          delete[] temp_fifo_buf;

          audio_frame->pts = next_audio_pts;
          next_audio_pts += audio_frame->nb_samples;
          encode_write_frame(audio_enc_ctx, audio_frame, audio_stream);
        }
      }

      std::vector<Clip> active_video;
      for (const auto &clip : sorted_clips)
        if (clip.type == ClipType::Video && is_video_file(clip.path) &&
            current_time >= clip.start_time &&
            current_time < clip.start_time + clip.duration)
          active_video.push_back(clip);

      for (const auto &clip : active_video) {
        auto it = export_res.video_cache.find(clip.path);
        if (it != export_res.video_cache.end()) {
          float media_time =
              (current_time - clip.start_time) + clip.media_start;
          if (media_time >= 0) {
            ensure_video_decoded_upto(it->second, media_time);
            update_texture_from_cache(it->second, media_time, false);
          }
        }
      }

      render_frame(export_res, current_time, sorted_clips, width, height, fps);

      glBindFramebuffer(GL_FRAMEBUFFER, export_res.fbo);
      glReadBuffer(GL_COLOR_ATTACHMENT0);
      glBindBuffer(GL_PIXEL_PACK_BUFFER, pbos[pbo_index]);
      glPixelStorei(GL_PACK_ALIGNMENT, 1);
      glReadPixels(0, 0, width, height, GL_RGB, GL_UNSIGNED_BYTE, 0);
      glPixelStorei(GL_PACK_ALIGNMENT, 4);
      glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
      glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }

    if (frame_idx > 0) {
      glBindBuffer(GL_PIXEL_PACK_BUFFER, pbos[next_pbo_index]);
      GLubyte *src = (GLubyte *)glMapBuffer(GL_PIXEL_PACK_BUFFER, GL_READ_ONLY);
      if (src) {
        std::vector<uint8_t> flipped(width * height * 3);
        for (int y = 0; y < height; ++y) {
          memcpy(&flipped[y * width * 3], &src[y * width * 3], width * 3);
        }
        if (av_frame_make_writable(video_frame) < 0) {
          std::cerr << "Failed to make video_frame writable\n";
        }
        const uint8_t *in_data[4] = {flipped.data(), nullptr, nullptr, nullptr};
        int in_linesize[4] = {width * 3, 0, 0, 0};
        sws_scale(sws_ctx, in_data, in_linesize, 0, height, video_frame->data,
                  video_frame->linesize);

        video_frame->pts = next_video_pts++;
        encode_write_frame(video_enc_ctx, video_frame, video_stream);

        glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
      }
      glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    }

    pbo_index = next_pbo_index;
    next_pbo_index = (next_pbo_index + 1) % 2;

    if (frame_idx % 30 == 0 && frame_idx < duration_frames) {
      std::cout << "Exporting frame " << frame_idx + 1 << "/" << duration_frames
                << std::endl;
    }
  }

  encode_write_frame(video_enc_ctx, nullptr, video_stream);

  if (audio_fifo && av_audio_fifo_size(audio_fifo) > 0) {
    int r = av_audio_fifo_size(audio_fifo);
    audio_frame->nb_samples = r;
    float *temp_fifo_buf = new float[r * 2];
    void *fifo_read_data[] = {temp_fifo_buf};
    av_audio_fifo_read(audio_fifo, fifo_read_data, r);

    uint8_t **converted_data = nullptr;
    av_samples_alloc_array_and_samples(&converted_data, nullptr,
                                       audio_enc_ctx->ch_layout.nb_channels, r,
                                       audio_enc_ctx->sample_fmt, 0);
    swr_convert(swr_ctx, converted_data, r, (const uint8_t **)fifo_read_data,
                r);
    av_samples_copy(audio_frame->data, converted_data, 0, 0, r,
                    audio_enc_ctx->ch_layout.nb_channels,
                    audio_enc_ctx->sample_fmt);

    av_freep(&converted_data[0]);
    av_freep(&converted_data);
    delete[] temp_fifo_buf;

    audio_frame->pts = next_audio_pts;
    encode_write_frame(audio_enc_ctx, audio_frame, audio_stream);
  }
  encode_write_frame(audio_enc_ctx, nullptr, audio_stream);

  av_write_trailer(fmt_ctx);

  if (!(fmt_ctx->oformat->flags & AVFMT_NOFILE))
    avio_closep(&fmt_ctx->pb);
  avformat_free_context(fmt_ctx);
  avcodec_free_context(&video_enc_ctx);
  avcodec_free_context(&audio_enc_ctx);
  av_frame_free(&video_frame);
  av_frame_free(&audio_frame);
  av_packet_free(&pkt);
  sws_freeContext(sws_ctx);
  swr_free(&swr_ctx);
  if (audio_fifo)
    av_audio_fifo_free(audio_fifo);

  glDeleteBuffers(2, pbos);
  cleanup_gl_resources(export_res);
  cleanup_video_resources(export_res);

  std::cout << "Video export finished." << std::endl;
  return true;
}

std::vector<float> GenerateWaveformPreview(const std::vector<int16_t> &samples,
                                           int channels,
                                           int samples_per_pixel = 256) {
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

void QueueClipThumbnails(GLResources &res,
                         const Clip &clip) { // Pass const Clip&
  if (clip.type == ClipType::Video && is_video_file(clip.path)) {
    // Check if thumbnails are already being generated or are done for this clip
    // path
    if (res.clip_thumbnail_textures.count(clip.path)) {
      // Already processed or processing started, could add logic to re-queue if
      // needed std::cout << "Thumbnails already queued/generated for " <<
      // clip.path << std::endl;
      return;
    }

    const int max_thumbs =
        static_cast<int>(clip.duration); // Generate more thumbs if needed
    const float interval = std::max(
        0.5f, clip.duration /
                  static_cast<float>(max_thumbs)); // Ensure minimum interval

    // Create placeholder entry in the map immediately so we know processing
    // started
    res.clip_thumbnail_textures[clip.path] = {}; // Empty vector initially
    res.generated_thumbnails_map[clip.path] =
        {}; // Empty map for generated timestamps

    { // Lock the request queue
      std::lock_guard<std::mutex> lock(request_mutex);
      for (int i = 0; i < max_thumbs; ++i) {
        float timestamp =
            i * interval +
            clip.media_start; // Use media_start
                              // Clamp timestamp to be within the actual media
                              // duration if known (Need media duration - maybe
                              // get it during AddNewClip or load_resources)
        // float media_duration = get_media_duration(clip.path); // Hypothetical
        // function if (media_duration > 0) timestamp = std::min(timestamp,
        // media_duration - 0.1f);

        timestamp = std::max(0.0f, timestamp); // Ensure non-negative

        ThumbnailRequest req;
        req.clip_path = clip.path;
        req.timestamp = timestamp;
        thumbnail_request_queue.push(req);
        // std::cout << "Queued thumb request for " << clip.path << " at " <<
        // timestamp << std::endl;
      }
    } // Unlock request_mutex

    worker_cv.notify_one(); // Signal the worker thread
  }
}

// Replace the existing ProcessThumbnailTasks in video_export.cpp or main.cpp

void ProcessThumbnailResults(
    GLResources &res, int max_per_frame = 2) { // Process a couple per frame
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
      // Check if this exact timestamp was already processed (e.g., due to
      // requeueing)
      auto &gen_map = res.generated_thumbnails_map[result.clip_path];
      if (gen_map.count(result.timestamp)) {
        continue; // Already have this one
      }

      // Create OpenGL Texture on Main Thread
      GLuint tex_id = 0;
      glGenTextures(1, &tex_id);
      if (tex_id == 0) {
        std::cerr << "Failed to generate texture ID for thumbnail!"
                  << std::endl;
        continue; // Skip this one
      }
      glBindTexture(GL_TEXTURE_2D, tex_id);

      // Upload pixel data (RGB)
      glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, result.width, result.height, 0,
                   GL_RGB, GL_UNSIGNED_BYTE, result.pixels.data());

      GLenum err = glGetError();
      if (err != GL_NO_ERROR) {
        std::cerr << "OpenGL error creating thumbnail texture for "
                  << result.clip_path << ": " << err << std::endl;
        glDeleteTextures(1, &tex_id); // Clean up failed texture
        continue;                     // Skip
      }

      // Set texture parameters (simple bilinear is fine for thumbs)
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

      glBindTexture(GL_TEXTURE_2D, 0); // Unbind

      // Store the texture ID in the main resource map
      // Note: This assumes res.clip_thumbnail_textures[result.clip_path] was
      // initialized in QueueClipThumbnails
      if (res.clip_thumbnail_textures.count(result.clip_path)) {
        res.clip_thumbnail_textures[result.clip_path].push_back(tex_id);
        gen_map[result.timestamp] = tex_id; // Mark timestamp as generated
        // std::cout << "Generated thumbnail texture " << tex_id << " for " <<
        // result.clip_path << " at " << result.timestamp << std::endl;
      } else {
        std::cerr << "Warning: Thumbnail result received for clip path not "
                     "found in texture map: "
                  << result.clip_path << std::endl;
        glDeleteTextures(1, &tex_id); // Clean up unused texture
      }
    } else {
      // Handle failure case if needed (e.g., log error)
      // std::cerr << "Thumbnail generation failed for " << result.clip_path <<
      // " at " << result.timestamp << ": " << result.error_message <<
      // std::endl;
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

    AVFormatContext *fmt_ctx = nullptr;
    AVCodecContext *codec_ctx = nullptr;
    AVFrame *frame = nullptr;
    AVFrame *rgb_frame = nullptr;
    AVPacket *packet = nullptr;
    SwsContext *sws_ctx = nullptr;
    int video_stream_idx = -1;
    uint8_t *buffer = nullptr;

    try { // Use try-catch for easier cleanup on error
      // 1. Open Input
      if (avformat_open_input(&fmt_ctx, request.clip_path.c_str(), nullptr,
                              nullptr) != 0) {
        throw std::runtime_error("Could not open video file: " +
                                 request.clip_path);
      }
      if (avformat_find_stream_info(fmt_ctx, nullptr) < 0) {
        throw std::runtime_error("Could not find stream info for: " +
                                 request.clip_path);
      }

      // 2. Find Video Stream & Codec
      video_stream_idx =
          av_find_best_stream(fmt_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
      if (video_stream_idx < 0) {
        throw std::runtime_error("No video stream found in: " +
                                 request.clip_path);
      }
      AVCodecParameters *codec_params =
          fmt_ctx->streams[video_stream_idx]->codecpar;
      const AVCodec *codec = avcodec_find_decoder(codec_params->codec_id);
      if (!codec) {
        throw std::runtime_error("Unsupported codec in: " + request.clip_path);
      }
      codec_ctx = avcodec_alloc_context3(codec);
      if (!codec_ctx ||
          avcodec_parameters_to_context(codec_ctx, codec_params) < 0 ||
          avcodec_open2(codec_ctx, codec, nullptr) < 0) {
        throw std::runtime_error("Failed to setup codec context for: " +
                                 request.clip_path);
      }

      AVRational time_base = fmt_ctx->streams[video_stream_idx]->time_base;

      // 3. Seek
      int64_t target_ts =
          av_rescale_q(static_cast<int64_t>(request.timestamp * AV_TIME_BASE),
                       AV_TIME_BASE_Q, time_base);
      // Seek slightly before the target to ensure we get the right frame or the
      // one just before it
      if (av_seek_frame(fmt_ctx, video_stream_idx, target_ts,
                        AVSEEK_FLAG_BACKWARD) < 0) {
        // Don't throw, just log warning and try decoding from start if seek
        // fails near beginning
        std::cerr << "Warning: Seek failed for " << request.clip_path << " at "
                  << request.timestamp << "s. Trying from start." << std::endl;
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
      int numBytes = av_image_get_buffer_size(
          AV_PIX_FMT_RGB24, THUMBNAIL_WIDTH, THUMBNAIL_HEIGHT,
          1); // Use alignment 1 for simplicity
      buffer = (uint8_t *)av_malloc(numBytes * sizeof(uint8_t));
      if (!buffer) {
        throw std::runtime_error("Failed to allocate RGB buffer.");
      }
      av_image_fill_arrays(rgb_frame->data, rgb_frame->linesize, buffer,
                           AV_PIX_FMT_RGB24, THUMBNAIL_WIDTH, THUMBNAIL_HEIGHT,
                           1);

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
              throw std::runtime_error(
                  "Error receiving frame during decoding.");
            }

            // Check if this frame is at or after our target timestamp
            double current_pts_sec =
                (frame->pts == AV_NOPTS_VALUE)
                    ? -1.0
                    : static_cast<double>(frame->pts) * av_q2d(time_base);
            if (current_pts_sec < 0 &&
                frame->pkt_dts !=
                    AV_NOPTS_VALUE) { // Fallback to DTS if PTS missing
              current_pts_sec =
                  static_cast<double>(frame->pkt_dts) * av_q2d(time_base);
            }

            if (current_pts_sec >= request.timestamp ||
                current_pts_sec <
                    0) { // Use frame if >= target or PTS is unknown after seek
              // Initialize SWS context *here* now that we have the source frame
              // format
              sws_ctx = sws_getCachedContext(
                  sws_ctx, // Reuse context
                  codec_ctx->width, codec_ctx->height, codec_ctx->pix_fmt,
                  THUMBNAIL_WIDTH, THUMBNAIL_HEIGHT, AV_PIX_FMT_RGB24,
                  SWS_BILINEAR, // Use bilinear for speed
                  nullptr, nullptr, nullptr);
              if (!sws_ctx) {
                throw std::runtime_error("Failed to create SwsContext.");
              }

              // Convert the frame directly to thumbnail size RGB
              sws_scale(sws_ctx, (const uint8_t *const *)frame->data,
                        frame->linesize, 0, codec_ctx->height, rgb_frame->data,
                        rgb_frame->linesize);

              // Copy pixels to result vector
              result.pixels.resize(numBytes);
              memcpy(result.pixels.data(), buffer, numBytes);
              result.success = true;
              frame_decoded = true;
              av_frame_unref(frame); // Release the decoded frame
              break;                 // Found our frame
            }
            av_frame_unref(frame); // Release frame if it wasn't the target
          } // end receive loop
        } // end if video stream
        av_packet_unref(packet); // Release packet
        if (frame_decoded) {
          break; // Exit read loop
        }
      } // end read loop

      // If loop finished without decoding, maybe try flushing? (Less critical
      // for single frame)
      if (!frame_decoded) {
        // Optional: Add flush logic here if needed, similar to
        // ensure_video_decoded_upto EOF handling but convert the *first*
        // flushed frame if timestamp was near the end.
        result.error_message =
            "Reached EOF or error before finding target frame.";
      }

    } catch (const std::runtime_error &e) {
      result.success = false;
      result.error_message = e.what();
      std::cerr << "Thumbnail Error (" << request.clip_path << " @ "
                << request.timestamp << "s): " << e.what() << std::endl;
    }

    // --- Cleanup FFmpeg resources ---
    av_free(buffer); // Free the manually allocated buffer
    sws_freeContext(sws_ctx);
    av_frame_free(&frame);
    av_frame_free(&rgb_frame);
    av_packet_free(&packet);
    if (codec_ctx)
      avcodec_free_context(&codec_ctx);
    if (fmt_ctx)
      avformat_close_input(&fmt_ctx);

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
      std::queue<ThumbnailRequest>().swap(
          thumbnail_request_queue); // Clear queue
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

void decoder_worker_func() {
  std::cout << "Decoder worker thread started." << std::endl;
  // The decoder_cache now holds our more advanced DecoderState
  std::map<std::string, std::unique_ptr<DecoderState>> decoder_cache;

  while (!stop_decoder_worker_flag) {
    DecodedFrameRequest request;
    // ... (your existing request-waiting logic is unchanged)
    {
      std::unique_lock<std::mutex> lock(decoder_request_mutex);
      if (!decoder_worker_cv.wait_for(lock, std::chrono::milliseconds(100),
                                      [&] {
                                        return !decoder_request_queue.empty() ||
                                               stop_decoder_worker_flag;
                                      })) {
        continue;
      }
      if (stop_decoder_worker_flag)
        break;
      request = std::move(decoder_request_queue.front());
      decoder_request_queue.pop_front();
    }

    DecodedFrameResult result;
    result.clip_path = request.clip_path;
    result.success = false;
    result.requested_timestamp = request.timestamp;

    // --- Get or Create Decoder State ---
    DecoderState *state = nullptr;
    auto it = decoder_cache.find(request.clip_path);
    if (it == decoder_cache.end()) {
      auto new_state = std::make_unique<DecoderState>();
      try {
        if (avformat_open_input(&new_state->fmt_ctx, request.clip_path.c_str(),
                                nullptr, nullptr) != 0)
          throw std::runtime_error("Could not open video file");
        if (avformat_find_stream_info(new_state->fmt_ctx, nullptr) < 0)
          throw std::runtime_error("Could not find stream info");

        AVStream *stream = nullptr;
        const AVCodec *codec = nullptr;
        new_state->video_stream_idx = av_find_best_stream(
            new_state->fmt_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, &codec, 0);
        if (new_state->video_stream_idx < 0)
          throw std::runtime_error("No video stream found");
        stream = new_state->fmt_ctx->streams[new_state->video_stream_idx];

        // --- OPTIMIZATION 1: Try to enable Hardware Acceleration ---
        for (int i = 0;; i++) {
          const AVCodecHWConfig *config = avcodec_get_hw_config(codec, i);
          if (!config)
            break;
          if (config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX) {
            // For this example, we'll prefer CUDA on Windows by default.
            // Other options: AV_HWDEVICE_TYPE_D3D11VA, AV_HWDEVICE_TYPE_QSV,
            // AV_HWDEVICE_TYPE_VIDEOTOOLBOX
            if (config->device_type == AV_HWDEVICE_TYPE_CUDA) {
              if (av_hwdevice_ctx_create(&new_state->hw_device_ctx,
                                         config->device_type, NULL, NULL,
                                         0) >= 0) {
                new_state->hw_pix_fmt = config->pix_fmt;
                std::cout << "Hardware acceleration enabled for "
                          << request.clip_path << " (CUDA)" << std::endl;
                break;
              }
            }
          }
        }

        new_state->codec_ctx = avcodec_alloc_context3(codec);
        if (!new_state->codec_ctx)
          throw std::runtime_error("Failed to alloc codec context");
        if (avcodec_parameters_to_context(new_state->codec_ctx,
                                          stream->codecpar) < 0)
          throw std::runtime_error("Failed to copy params");

        if (new_state->hw_device_ctx) {
          new_state->codec_ctx->hw_device_ctx =
              av_buffer_ref(new_state->hw_device_ctx);
        }

        // --- OPTIMIZATION 2: Enable Multi-Threaded Decoding ---
        // Set thread_count to 0 to let FFmpeg decide the optimal number of
        // threads.
        new_state->codec_ctx->thread_count = 0;

        if (avcodec_open2(new_state->codec_ctx, codec, nullptr) < 0)
          throw std::runtime_error("Failed to open codec");
        new_state->time_base = stream->time_base;

        auto inserted_it =
            decoder_cache.emplace(request.clip_path, std::move(new_state));
        state = inserted_it.first->second.get();

      } catch (const std::runtime_error &e) {
        // ... (error handling)
      }
    } else {
      state = it->second.get();
    }

    // The decode_frame_at_timestamp function is now responsible for handling
    // everything
    if (decode_frame_at_timestamp(state, request.timestamp, result)) {
      // Success
    }

    if (request.sync_promise) {
      request.sync_promise->set_value(std::move(result.frame));
    } else {
      std::lock_guard<std::mutex> lock(decoder_result_mutex);
      decoder_result_queue.push(std::move(result));
    }
  }
}

// Helper function to decode a single frame (extracted for clarity)
bool decode_frame_at_timestamp(DecoderState *state, double timestamp,
                               DecodedFrameResult &result) {
  const double SEEK_THRESHOLD = 1.5; // Seek if target is more than 1.5s away

  // Seek if the target is far from our last decoded position for this specific
  // stream
  if (state->last_decoded_pts < 0 || timestamp < state->last_decoded_pts ||
      std::abs(timestamp - state->last_decoded_pts) > SEEK_THRESHOLD) {
    int64_t target_ts =
        av_rescale_q(static_cast<int64_t>(timestamp * AV_TIME_BASE),
                     AV_TIME_BASE_Q, state->time_base);
    if (av_seek_frame(state->fmt_ctx, state->video_stream_idx, target_ts,
                      AVSEEK_FLAG_BACKWARD) < 0) {
      std::cerr << "Warning: Seek failed for " << result.clip_path
                << " to timestamp " << timestamp << std::endl;
    }
    avcodec_flush_buffers(state->codec_ctx);
    state->last_decoded_pts = -1.0; // Invalidate last PTS after seek
  }

  AVFrame *frame = av_frame_alloc();
  AVFrame *sw_frame = av_frame_alloc(); // For CPU-side data
  AVPacket *packet = av_packet_alloc();
  if (!frame || !sw_frame || !packet) {
    if (frame)
      av_frame_free(&frame);
    if (sw_frame)
      av_frame_free(&sw_frame);
    if (packet)
      av_packet_free(&packet);
    result.error_message = "Failed to allocate frame/packet";
    return false;
  }

  bool frame_found = false;

  auto process_frame = [&]() -> bool {
    double frame_pts =
        (frame->pts == AV_NOPTS_VALUE)
            ? -1.0
            : static_cast<double>(frame->pts) * av_q2d(state->time_base);
    state->last_decoded_pts = frame_pts;

    if (frame_pts >= timestamp) {
      AVFrame *frame_to_convert = frame;

      // If it's a hardware frame, we need to transfer it to CPU memory first
      if (frame->format == state->hw_pix_fmt) {
        if (av_hwframe_transfer_data(sw_frame, frame, 0) < 0) {
          result.error_message =
              "Failed to transfer hardware frame to system memory.";
          return false;
        }
        frame_to_convert = sw_frame;
      }

      // --- FIX 1: Correct Color Range & Space Handling ---
      AVColorRange color_range = frame_to_convert->color_range;
      AVColorSpace color_space = frame_to_convert->colorspace;

      if (color_range == AVCOL_RANGE_UNSPECIFIED)
        color_range = AVCOL_RANGE_MPEG;
      if (color_space == AVCOL_SPC_UNSPECIFIED)
        color_space = AVCOL_SPC_BT709;

      // Configure sws_scale with this information to remove guesswork
      state->sws_ctx = sws_getCachedContext(
          state->sws_ctx, frame_to_convert->width, frame_to_convert->height,
          (AVPixelFormat)frame_to_convert->format, frame_to_convert->width,
          frame_to_convert->height, AV_PIX_FMT_RGB24, SWS_FAST_BILINEAR,
          nullptr, nullptr, nullptr);

      if (state->sws_ctx) {
        sws_setColorspaceDetails(state->sws_ctx,
                                 sws_getCoefficients(color_space), color_range,
                                 sws_getCoefficients(AVCOL_SPC_BT709),
                                 AVCOL_RANGE_MPEG, 0, 1 << 16, 1 << 16);

        result.frame.width = frame_to_convert->width;
        result.frame.height = frame_to_convert->height;
        result.frame.pts = frame_pts;
        result.frame.pixels.resize(result.frame.width * result.frame.height *
                                   3);

        uint8_t *dest_data[4] = {result.frame.pixels.data(), nullptr, nullptr,
                                 nullptr};
        int dest_linesize[4] = {result.frame.width * 3, 0, 0, 0};
        sws_scale(state->sws_ctx, frame_to_convert->data,
                  frame_to_convert->linesize, 0, frame_to_convert->height,
                  dest_data, dest_linesize);
      }

      return true;
    }
    return false;
  };

  // Pre-drain any already-buffered frames leftover from previous EAGAIN
  while (avcodec_receive_frame(state->codec_ctx, frame) >= 0) {
    if (process_frame()) {
      result.success = true;
      frame_found = true;
      goto cleanup_and_return;
    }
  }

  // Then process new packets
  while (av_read_frame(state->fmt_ctx, packet) >= 0) {
    if (packet->stream_index == state->video_stream_idx) {
      if (avcodec_send_packet(state->codec_ctx, packet) >= 0) {
        while (avcodec_receive_frame(state->codec_ctx, frame) >= 0) {
          if (process_frame()) {
            result.success = true;
            frame_found = true;
            av_packet_unref(packet);
            goto cleanup_and_return;
          }
        }
      }
    }
    av_packet_unref(packet);
  }

cleanup_and_fail:
  frame_found = false;
cleanup_and_return:
  av_frame_free(&frame);
  av_frame_free(&sw_frame);
  av_packet_free(&packet);
  return frame_found;
}

// OPTIMIZATION: Update playback state and clear cache on pause.
void update_playback_state(GLResources &res, float current_time,
                           float last_time, bool master_playing,
                           bool &is_playing_out, bool &is_scrubbing_out) {
  float time_delta = std::abs(current_time - last_time);
  if (master_playing) {
    is_playing_out = true;
    is_scrubbing_out = false;
  } else {
    is_playing_out = false;
    is_scrubbing_out =
        time_delta >= 0.05f; // User is dragging the playhead manually
  }

  for (auto &[path, video] : res.video_cache) {
    bool was_actively_playing = video.is_actively_playing;
    video.is_actively_playing = is_playing_out;

    if (is_scrubbing_out) {
      // OPTIMIZATION: Intelligent cache trimming instead of full clear
      // Keep a 2-second buffer around the current playhead time during scrubs
      const double trim_threshold = 2.0;
      video.frame_cache.erase(
          std::remove_if(
              video.frame_cache.begin(), video.frame_cache.end(),
              [current_time, trim_threshold](const DecodedFrame &frame) {
                return std::abs(frame.pts - current_time) > trim_threshold;
              }),
          video.frame_cache.end());
    } else if (was_actively_playing && !video.is_actively_playing) {
      // Paused: clear cache to save memory
      video.frame_cache.clear();
    }
  }
}

// --- NEW: Functions to manage the thread ---
void start_decoder_worker() {
  stop_decoder_worker_flag = false;
  decoder_thread = std::thread(decoder_worker_func);
}

void stop_decoder_worker() {
  if (decoder_thread.joinable()) {
    stop_decoder_worker_flag = true;
    {
      std::lock_guard<std::mutex> lock(decoder_request_mutex);
      std::deque<DecodedFrameRequest>().swap(decoder_request_queue);
    }
    decoder_worker_cv.notify_one();
    decoder_thread.join();
    std::cout << "Decoder worker joined." << std::endl;
  }
  {
    std::lock_guard<std::mutex> lock(decoder_result_mutex);
    std::queue<DecodedFrameResult>().swap(decoder_result_queue);
  }
}

bool is_frame_available_in_cache(const VideoData &video, double target_time,
                                 double tolerance) {
  if (video.frame_cache.empty())
    return false;

  // Check if the target time is within the range of the cached frames
  if (target_time < video.frame_cache.front().pts - tolerance ||
      target_time > video.frame_cache.back().pts + tolerance) {
    return false;
  }

  // Binary search would be faster here if the cache is always sorted, which it
  // is.
  auto it = std::lower_bound(
      video.frame_cache.begin(), video.frame_cache.end(),
      target_time - tolerance,
      [](const DecodedFrame &frame, double time) { return frame.pts < time; });

  if (it != video.frame_cache.end() &&
      std::abs(it->pts - target_time) <= tolerance) {
    return true;
  }

  return false;
}

bool should_request_frame(VideoData &video, double target_time) {
  auto now = std::chrono::steady_clock::now();

  // Throttle requests - don't request too frequently
  auto time_since_last_request =
      std::chrono::duration_cast<std::chrono::duration<double>>(
          now - video.last_request_time)
          .count();

  if (time_since_last_request < VideoData::MIN_REQUEST_INTERVAL) {
    return false;
  }

  // Don't request if we already have too many pending
  if (video.pending_requests.size() >= VideoData::MAX_PENDING_REQUESTS) {
    return false;
  }

  // Check if frame is already in cache with tolerance
  if (is_frame_available_in_cache(video, target_time,
                                  VideoData::CACHE_TOLERANCE)) {
    return false;
  }

  // Check if we already have a pending request for this time (with tolerance)
  for (double pending_time : video.pending_requests) {
    if (std::abs(pending_time - target_time) <= VideoData::CACHE_TOLERANCE) {
      return false;
    }
  }

  // Check global pending requests to avoid duplicate work across videos
  {
    std::lock_guard<std::mutex> lock(pending_requests_mutex);
    for (const auto &[path, time] : global_pending_requests) {
      if (std::abs(time - target_time) <= VideoData::CACHE_TOLERANCE) {
        return false;
      }
    }
  }

  return true;
}

// Add this new function to force cache refresh for a clip
void force_cache_refresh(GLResources &res, const std::string &clip_path) {
  auto it = res.video_cache.find(clip_path);
  if (it != res.video_cache.end()) {
    VideoData &video = it->second;
    video.frame_cache.clear();
    video.last_decoded_pts = -1.0;
    video.is_seeking = false;

    // Force a seek to the current position
    if (video.format_ctx && video.video_stream_idx >= 0) {
      int64_t seek_target_ts =
          av_rescale_q(static_cast<int64_t>(0 * AV_TIME_BASE), AV_TIME_BASE_Q,
                       video.time_base);
      av_seek_frame(video.format_ctx, video.video_stream_idx, seek_target_ts,
                    AVSEEK_FLAG_BACKWARD);
      avcodec_flush_buffers(video.codec_ctx);
    }
  }
}

// OPTIMIZATION: New prefetching logic with request prioritization
void update_video_previews(GLResources &res,
                           const std::vector<Clip> &active_clips,
                           float current_time, bool is_playing,
                           bool is_scrubbing) {

  // On a new scrub action, clear out old prefetch requests.
  if (is_scrubbing) {
    std::vector<std::pair<std::string, double>> removed_requests;
    {
      std::lock_guard<std::mutex> lock(decoder_request_mutex);
      decoder_request_queue.erase(
          std::remove_if(
              decoder_request_queue.begin(), decoder_request_queue.end(),
              [&](const DecodedFrameRequest &req) {
                // Remove ALL requests when scrubbing, as they are now stale.
                removed_requests.push_back({req.clip_path, req.timestamp});
                return true;
              }),
          decoder_request_queue.end());
    }
    if (!removed_requests.empty()) {
      std::lock_guard<std::mutex> perf_lock(playback_perf_mutex);
      playback_perf_stats.requests_dropped_stale +=
          static_cast<uint64_t>(removed_requests.size());
    }
    std::lock_guard<std::mutex> pending_lock(pending_requests_mutex);
    for (const auto &[path, ts] : removed_requests) {
      auto video_it = res.video_cache.find(path);
      if (video_it != res.video_cache.end()) {
        video_it->second.pending_requests.erase(ts);
      }
      global_pending_requests.erase({path, ts});
    }
  }

  for (const auto &clip : active_clips) {
    if (clip.type != ClipType::Video || !is_video_file(clip.path))
      continue;

    auto it = res.video_cache.find(clip.path);
    if (it == res.video_cache.end() || !it->second.is_initialized)
      continue;

    VideoData &video = it->second;
    double media_time = (current_time - clip.start_time) + clip.media_start;
    if (media_time < 0)
      continue;

    auto enqueue_request = [&](double request_time, RequestPriority priority,
                               bool front) {
      {
        std::lock_guard<std::mutex> pending_lock(pending_requests_mutex);
        global_pending_requests.insert({clip.path, request_time});
      }
      video.pending_requests.insert(request_time);
      video.last_request_time = std::chrono::steady_clock::now();

      std::lock_guard<std::mutex> lock(decoder_request_mutex);
      if (front) {
        decoder_request_queue.push_front({clip.path, request_time, priority});
      } else {
        decoder_request_queue.push_back({clip.path, request_time, priority});
      }
      decoder_worker_cv.notify_one();

      std::lock_guard<std::mutex> perf_lock(playback_perf_mutex);
      if (priority == RequestPriority::High) {
        playback_perf_stats.requests_enqueued_high++;
      } else {
        playback_perf_stats.requests_enqueued_normal++;
      }
      playback_perf_stats.decoder_queue_size =
          static_cast<int>(decoder_request_queue.size());
      playback_perf_stats.pending_clip_requests =
          static_cast<int>(video.pending_requests.size());
    };

    // --- Prioritized Request Logic ---

    // 1. If scrubbing or paused, send a HIGH priority request for the exact
    // frame.
    if (is_scrubbing || !is_playing) {
      if (should_request_frame(video, media_time)) {
        // Push to the FRONT of the deque
        enqueue_request(media_time, RequestPriority::High, true);
      }
    }

    // 2. During smooth playback, prefetch with NORMAL priority.
    if (is_playing) {
      // Prevent backlog growth: drop stale prefetch entries for this clip.
      std::vector<double> removed_prefetch_times;
      {
        std::lock_guard<std::mutex> lock(decoder_request_mutex);
        decoder_request_queue.erase(
            std::remove_if(
                decoder_request_queue.begin(), decoder_request_queue.end(),
                [&](const DecodedFrameRequest &req) {
                  const bool should_remove =
                      req.clip_path == clip.path &&
                      req.timestamp < (media_time - 0.25);
                  if (should_remove) {
                    removed_prefetch_times.push_back(req.timestamp);
                  }
                  return should_remove;
                }),
            decoder_request_queue.end());
      }
      if (!removed_prefetch_times.empty()) {
        {
          std::lock_guard<std::mutex> perf_lock(playback_perf_mutex);
          playback_perf_stats.requests_dropped_stale +=
              static_cast<uint64_t>(removed_prefetch_times.size());
        }
        for (double ts : removed_prefetch_times) {
          video.pending_requests.erase(ts);
          std::lock_guard<std::mutex> pending_lock(pending_requests_mutex);
          global_pending_requests.erase({clip.path, ts});
        }
      }

      int queue_pressure = 0;
      {
        std::lock_guard<std::mutex> lock(decoder_request_mutex);
        queue_pressure = static_cast<int>(decoder_request_queue.size());
      }
      const int prefetch_count =
          (queue_pressure > 20) ? 2 : (queue_pressure > 12 ? 4 : 6);
      const size_t max_pending_per_clip = 12;
      const float frame_duration =
          (video.time_base.den > 0)
              ? (1.0f /
                 (float)av_q2d(video.format_ctx->streams[video.video_stream_idx]
                                   ->r_frame_rate))
              : (1.0f / 30.0f);

      for (int i = 0; i <= prefetch_count; ++i) { // Start at 0 to ensure current frame is requested
        double prefetch_time = media_time + i * frame_duration;
        if (prefetch_time > video.duration_sec)
          break;

        if (video.pending_requests.size() < max_pending_per_clip &&
            decoder_request_queue.size() < VideoData::MAX_PENDING_REQUESTS &&
            should_request_frame(video, prefetch_time)) {
          // Push to the BACK of the deque
          enqueue_request(prefetch_time, RequestPriority::Normal, false);
        }
      }
    }
  }

  int request_queue_size = 0;
  int global_pending_size = 0;
  {
    std::lock_guard<std::mutex> lock(decoder_request_mutex);
    request_queue_size = static_cast<int>(decoder_request_queue.size());
  }
  {
    std::lock_guard<std::mutex> lock(pending_requests_mutex);
    global_pending_size = static_cast<int>(global_pending_requests.size());
  }
  {
    std::lock_guard<std::mutex> perf_lock(playback_perf_mutex);
    playback_perf_stats.decoder_queue_size = request_queue_size;
    playback_perf_stats.pending_global_requests = global_pending_size;
  }
}

// --- NEW: Function to process results from the decoder thread ---
void process_decoded_frames(GLResources &res, int max_per_frame = 5) {
  for (int i = 0; i < max_per_frame; ++i) {
    DecodedFrameResult result;
    {
      std::lock_guard<std::mutex> lock(decoder_result_mutex);
      if (decoder_result_queue.empty())
        break;
      result = std::move(decoder_result_queue.front());
      decoder_result_queue.pop();
      std::lock_guard<std::mutex> perf_lock(playback_perf_mutex);
      playback_perf_stats.decoder_result_queue_size =
          static_cast<int>(decoder_result_queue.size());
    }

    auto it = res.video_cache.find(result.clip_path);
    if (it == res.video_cache.end())
      continue;

    VideoData &video = it->second;

    // Remove the fulfilled/failed request from pending sets.
    for (auto pending_it = video.pending_requests.begin();
         pending_it != video.pending_requests.end();) {
      if (std::abs(*pending_it - result.requested_timestamp) <=
          VideoData::CACHE_TOLERANCE) {
        pending_it = video.pending_requests.erase(pending_it);
      } else {
        ++pending_it;
      }
    }
    {
      std::lock_guard<std::mutex> pending_lock(pending_requests_mutex);
      for (auto global_it = global_pending_requests.begin();
           global_it != global_pending_requests.end();) {
        if (global_it->first == result.clip_path &&
            std::abs(global_it->second - result.requested_timestamp) <=
                VideoData::CACHE_TOLERANCE) {
          global_it = global_pending_requests.erase(global_it);
        } else {
          ++global_it;
        }
      }
    }

    if (!result.success)
    {
      std::lock_guard<std::mutex> perf_lock(playback_perf_mutex);
      playback_perf_stats.decoded_frames_failed++;
      continue;
    }

    {
      std::lock_guard<std::mutex> perf_lock(playback_perf_mutex);
      playback_perf_stats.decoded_frames_success++;
    }

    // Fast path: decode results are usually in chronological order.
    if (video.frame_cache.empty() ||
        result.frame.pts >= video.frame_cache.back().pts) {
      video.frame_cache.push_back(std::move(result.frame));
    } else {
      auto insert_pos = std::lower_bound(
          video.frame_cache.begin(), video.frame_cache.end(), result.frame.pts,
          [](const DecodedFrame &a, double pts) { return a.pts < pts; });
      video.frame_cache.insert(insert_pos, std::move(result.frame));
    }

    // Trim cache if it's too large, removing the oldest frames.
    while (video.frame_cache.size() > VideoData::MAX_CACHE_SIZE) {
      video.frame_cache.pop_front();
    }
  }
}

bool initialize_audio_playback(AudioPlaybackState &state) {
  SDL_AudioSpec desired_spec = {};
  desired_spec.freq = 44100;
  desired_spec.format = SDL_AUDIO_S16;
  desired_spec.channels = 2;

  state.device =
      SDL_OpenAudioDevice(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &desired_spec);
  if (!state.device) {
    std::cerr << "Failed to open audio device: " << SDL_GetError() << std::endl;
    return false;
  }
  std::cout << "Audio device opened successfully. Device ID: " << state.device
            << std::endl;

  SDL_AudioSpec src_spec = {};
  src_spec.freq = 44100;
  src_spec.format = SDL_AUDIO_S16;
  src_spec.channels = 2;

  SDL_AudioSpec dst_spec =
      src_spec; // Use same format for source and destination

  state.stream = SDL_CreateAudioStream(&src_spec, &dst_spec);
  if (!state.stream) {
    std::cerr << "Failed to create audio stream: " << SDL_GetError()
              << std::endl;
    SDL_CloseAudioDevice(state.device);
    state.device = 0;
    return false;
  }
  std::cout << "Audio stream created successfully." << std::endl;

  if (SDL_BindAudioStream(state.device, state.stream) < 0) {
    std::cerr << "Failed to bind audio stream: " << SDL_GetError() << std::endl;
    SDL_DestroyAudioStream(state.stream);
    SDL_CloseAudioDevice(state.device);
    state.stream = nullptr;
    state.device = 0;
    return false;
  }
  std::cout << "Audio stream bound successfully." << std::endl;

  state.is_playing = true;
  state.volume = 1.0f;
  return true;
}

void update_audio_playback(AudioPlaybackState &state,
                           const std::vector<Clip> &clips, float current_time,
                           GLResources &res) {
  if (!state.device || !state.stream) {
    std::cerr << "Audio device or stream not initialized" << std::endl;
    return;
  }

  // Find active audio clips
  std::vector<const Clip *> new_active_clips;
  for (const auto &clip : clips) {
    if (clip.type == ClipType::Audio && current_time >= clip.start_time &&
        current_time < (clip.start_time + clip.duration)) {
      new_active_clips.push_back(&clip);
    }
  }

  // Check if active clips changed
  bool clips_changed = new_active_clips.size() != state.active_clips.size();
  if (!clips_changed) {
    for (size_t i = 0; i < new_active_clips.size(); ++i) {
      if (new_active_clips[i] != state.active_clips[i]) {
        clips_changed = true;
        break;
      }
    }
  }

  // If clips changed, clear the stream and update active clips
  if (clips_changed) {
    SDL_ClearAudioStream(state.stream);
    state.active_clips = std::move(new_active_clips);
  }

  // Calculate how much audio data we need to generate
  int queued = SDL_GetAudioStreamQueued(state.stream);
  int needed = 2048 - queued; // Smaller buffer size for lower latency
  if (needed <= 0)
    return; // We have enough data queued

  // Mix audio from active clips
  std::vector<float> mixed_audio;
  const int samples_per_frame = needed;
  mixed_audio.resize(samples_per_frame * 2); // Stereo
  std::fill(mixed_audio.begin(), mixed_audio.end(), 0.0f);

  for (const auto *clip : state.active_clips) {
    auto audio_it = res.preloaded_audio.find(clip->path);
    if (audio_it != res.preloaded_audio.end()) {
      const PreloadedAudio &audio = audio_it->second;
      float time_into_clip = current_time - clip->start_time;
      float effective_media_start = clip->media_start;
      float time_in_media = time_into_clip + effective_media_start;

      // Calculate sample position
      int64_t start_sample =
          static_cast<int64_t>(time_in_media * audio.sample_rate);

      // Get the clip's volume from keyframes or base volume if no keyframes
      float clip_volume =
          clip->volume_track.keyframes.empty()
              ? clip->volume
              : clip->volume_track.Evaluate(time_into_clip) * clip->volume;

      // Simple nearest-neighbor sampling for now
      for (int i = 0; i < samples_per_frame; ++i) {
        int64_t current_sample = start_sample + i;
        if (current_sample >= 0 &&
            (current_sample + 1) * audio.channels <= audio.samples.size()) {

          for (int ch = 0; ch < audio.channels; ++ch) {
            int16_t sample =
                audio.samples[current_sample * audio.channels + ch];
            mixed_audio[i * 2 + ch] += (static_cast<float>(sample) / 32768.0f) *
                                       clip_volume * state.volume;
          }
        }
      }
    }
  }

  // Simple limiter to prevent clipping
  float max_sample = 0.0f;
  for (float sample : mixed_audio) {
    max_sample = std::max(max_sample, std::abs(sample));
  }

  if (max_sample > 1.0f) {
    float scale = 1.0f / max_sample;
    for (float &sample : mixed_audio) {
      sample *= scale;
    }
  }

  // Convert float audio to S16 and send to stream
  std::vector<int16_t> audio_s16(samples_per_frame * 2);
  for (size_t i = 0; i < mixed_audio.size(); ++i) {
    float sample_f = std::max(-1.0f, std::min(1.0f, mixed_audio[i]));
    audio_s16[i] = static_cast<int16_t>(sample_f * 32767.0f);
  }

  int result = SDL_PutAudioStreamData(state.stream, audio_s16.data(),
                                      audio_s16.size() * sizeof(int16_t));
  if (result < 0) {
    std::cerr << "Failed to put audio data into stream: " << SDL_GetError()
              << std::endl;
  }
}

void cleanup_audio_playback(AudioPlaybackState &state) {
  if (state.stream) {
    SDL_DestroyAudioStream(state.stream);
    state.stream = nullptr;
  }
  if (state.device) {
    SDL_CloseAudioDevice(state.device);
    state.device = 0;
  }
  state.is_playing = false;
  state.active_clips.clear();
}

void pause_audio_playback(AudioPlaybackState &state) {
  if (state.device && state.is_playing) {
    SDL_PauseAudioDevice(state.device);
    state.is_playing = false;
  }
}

void resume_audio_playback(AudioPlaybackState &state) {
  if (state.device && !state.is_playing) {
    SDL_ResumeAudioDevice(state.device);
    state.is_playing = true;
  }
}

void set_audio_volume(AudioPlaybackState &state, float volume) {
  state.volume = std::max(0.0f, std::min(1.0f, volume));
  if (state.device) {
    SDL_SetAudioDeviceGain(state.device, state.volume);
  }
}

void DrawRenderWindow(const std::vector<Clip> &clips, bool *p_open,
                      int &render_width, int &render_height, int &export_fps,
                      float max_duration) {
  if (!ImGui::Begin("Render Video", p_open)) {
    ImGui::End();
    return;
  }

  static char output_path[1024] = "output.mp4";
  ImGui::InputText("Output Path", output_path, sizeof(output_path));
  ImGui::SameLine();
  if (ImGui::Button("Browse...")) {
    const char *filters[] = {"*.mp4", "*.avi", "*.mov"};
    const char *path = tinyfd_saveFileDialog("Export Video", output_path, 3,
                                             filters, "Video Files");
    if (path)
      strncpy(output_path, path, sizeof(output_path));
  }

  ImGui::InputInt("Width", &render_width);
  ImGui::InputInt("Height", &render_height);
  ImGui::InputInt("FPS", &export_fps);
  ImGui::Text("Duration: %.2fs", max_duration);

  static std::string status_msg = "";

  if (ImGui::Button("Start Export")) {
    int total_frames = static_cast<int>(std::ceil(max_duration * export_fps));
    if (total_frames > 0) {
      status_msg = "Exporting...";
      SDL_Window *current_window = SDL_GL_GetCurrentWindow();
      bool success =
          start_video_export(output_path, render_width, render_height,
                             export_fps, total_frames, clips, current_window);
      status_msg = success ? "Export Successful!" : "Export Failed!";
    } else {
      status_msg = "Invalid duration.";
    }
  }

  ImGui::Text("%s", status_msg.c_str());

  ImGui::End();
}
