#include <vector>
#include <algorithm>

extern "C" {
    #include <libavformat/avformat.h>
    #include <libavcodec/avcodec.h>
    #include <libavutil/opt.h>
    #include <libswscale/swscale.h>
    #include <libswresample/swresample.h>
    #include <libavfilter/avfilter.h>
    #include <libavfilter/buffersink.h>
    #include <libavfilter/buffersrc.h>
    #include <libavutil/pixdesc.h>
    #include <libavutil/channel_layout.h>
    #include <glad/glad.h>
}
#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_sdlrenderer3.h>
#include <SDL.h>
#include <SDL_video.h>
#include <SDL_image.h>
#include <SDL_render.h>
#include <SDL_image.h>
#include <iostream>
#include <string>
#include <sstream>
#include <filesystem>
#include <cstdio>
#include <cstdlib> // for system()
#include <regex>
#include <fstream> // for std::ofstream

#include "video_export.hpp"
#include "shared.hpp"

/* TODO

Fix Layer export
Fix whitespace export
Add Frames for the timing
Preview
Zoom (resizing)
Docking
UI


*/

/* struct Clip {
    std::string name;
    int start_time;
    int duration;
    int layer;
    std::string path;
}; */

struct StreamContext {
    AVFormatContext* fmt_ctx = nullptr;
    AVCodecContext* dec_ctx = nullptr;
    int stream_idx = -1;
    AVFrame* frame = nullptr;
    AVPacket* pkt = nullptr;
    int64_t next_pts = 0;
};

struct OutputStream {
    AVStream* stream = nullptr;
    AVCodecContext* enc_ctx = nullptr;
    SwsContext* sws_ctx = nullptr;
    SwrContext* swr_ctx = nullptr;
    int64_t next_pts = 0;
};

struct StreamState {
    int64_t next_video_dts = AV_NOPTS_VALUE;
    int64_t next_audio_dts = AV_NOPTS_VALUE;
};
std::vector<StreamState> stream_states;

// Error checking macro
#define CHECK_AV_ERROR(ret, message) \
    if (ret < 0) { \
        char errbuf[AV_ERROR_MAX_STRING_SIZE]; \
        av_make_error_string(errbuf, sizeof(errbuf), ret); \
        std::cerr << "ERROR: " << message << ": " << errbuf << std::endl; \
        std::cerr << "At " << __FILE__ << ":" << __LINE__ << std::endl; \
        return false; \
    }

#define FFMPEG_CHECK(condition, message) \
if (condition) { \
    std::cerr << "Error: " << message << std::endl; \
    return false; \
}

// Forward declaration
void DrawTimelineEditor(
    std::vector<Clip>& clips,
    int& playhead_position,
    int& start_time,
    int& duration,
    int& max_duration,
    float& zoom_factor,
    int& last_playhead_position,
    bool& layers_changed
);

SDL_Texture* CreatePreviewTexture(SDL_Renderer* renderer, const std::vector<Clip>& clips, int playhead_time);

void UpdatePreview(SDL_Renderer* renderer, SDL_Texture*& preview_texture, const std::vector<Clip>& clips, int playhead_time);

void RenderPreviewWindow(SDL_Texture* preview_texture);

void SetupImGuiStyle() {
    ImGuiStyle& style = ImGui::GetStyle();
    ImVec4* colors = style.Colors;

    colors[ImGuiCol_Text] = ImVec4(1.00f, 1.00f, 1.00f, 1.00f);
    colors[ImGuiCol_TextDisabled] = ImVec4(0.24f, 0.24f, 0.24f, 1.00f);
    colors[ImGuiCol_WindowBg] = ImVec4(0.14f, 0.14f, 0.14f, 1.00f);
    colors[ImGuiCol_ChildBg] = ImVec4(0.14f, 0.14f, 0.14f, 1.00f);
    colors[ImGuiCol_PopupBg] = ImVec4(0.14f, 0.14f, 0.14f, 1.00f);
    colors[ImGuiCol_Border] = ImVec4(0.43f, 0.43f, 0.50f, 0.50f);
    colors[ImGuiCol_BorderShadow] = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
    colors[ImGuiCol_FrameBg] = ImVec4(0.20f, 0.20f, 0.20f, 1.00f);
    colors[ImGuiCol_FrameBgHovered] = ImVec4(0.24f, 0.24f, 0.24f, 1.00f);
    colors[ImGuiCol_FrameBgActive] = ImVec4(0.24f, 0.24f, 0.24f, 1.00f);
    colors[ImGuiCol_TitleBg] = ImVec4(0.14f, 0.14f, 0.14f, 1.00f);
    colors[ImGuiCol_TitleBgActive] = ImVec4(0.14f, 0.14f, 0.14f, 1.00f);
    colors[ImGuiCol_TitleBgCollapsed] = ImVec4(0.14f, 0.14f, 0.14f, 1.00f);
    colors[ImGuiCol_MenuBarBg] = ImVec4(0.20f, 0.20f, 0.20f, 1.00f);
    colors[ImGuiCol_ScrollbarBg] = ImVec4(0.14f, 0.14f, 0.14f, 1.00f);
    colors[ImGuiCol_ScrollbarGrab] = ImVec4(0.24f, 0.24f, 0.24f, 1.00f);
    colors[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.24f, 0.24f, 0.24f, 1.00f);
    colors[ImGuiCol_ScrollbarGrabActive] = ImVec4(0.24f, 0.24f, 0.24f, 1.00f);
    colors[ImGuiCol_CheckMark] = ImVec4(0.86f, 0.93f, 0.89f, 1.00f);
    colors[ImGuiCol_SliderGrab] = ImVec4(0.24f, 0.24f, 0.24f, 1.00f);
    colors[ImGuiCol_SliderGrabActive] = ImVec4(0.24f, 0.24f, 0.24f, 1.00f);
    colors[ImGuiCol_Button] = ImVec4(0.20f, 0.20f, 0.20f, 1.00f);
    colors[ImGuiCol_ButtonHovered] = ImVec4(0.24f, 0.24f, 0.24f, 1.00f);
    colors[ImGuiCol_ButtonActive] = ImVec4(0.24f, 0.24f, 0.24f, 1.00f);
    colors[ImGuiCol_Header] = ImVec4(0.24f, 0.24f, 0.24f, 1.00f);
    colors[ImGuiCol_HeaderHovered] = ImVec4(0.24f, 0.24f, 0.24f, 1.00f);
    colors[ImGuiCol_HeaderActive] = ImVec4(0.24f, 0.24f, 0.24f, 1.00f);
    colors[ImGuiCol_Separator] = ImVec4(0.43f, 0.43f, 0.50f, 0.50f);
    colors[ImGuiCol_SeparatorHovered] = ImVec4(0.43f, 0.43f, 0.50f, 0.50f);
    colors[ImGuiCol_SeparatorActive] = ImVec4(0.43f, 0.43f, 0.50f, 0.50f);
    colors[ImGuiCol_ResizeGrip] = ImVec4(0.24f, 0.24f, 0.24f, 1.00f);
    colors[ImGuiCol_ResizeGripHovered] = ImVec4(0.24f, 0.24f, 0.24f, 1.00f);
    colors[ImGuiCol_ResizeGripActive] = ImVec4(0.24f, 0.24f, 0.24f, 1.00f);
    colors[ImGuiCol_Tab] = ImVec4(0.14f, 0.14f, 0.14f, 1.00f);
    colors[ImGuiCol_TabHovered] = ImVec4(0.24f, 0.24f, 0.24f, 1.00f);
    colors[ImGuiCol_TabActive] = ImVec4(0.24f, 0.24f, 0.24f, 1.00f);
    colors[ImGuiCol_TabUnfocused] = ImVec4(0.14f, 0.14f, 0.14f, 1.00f);
    colors[ImGuiCol_TabUnfocusedActive] = ImVec4(0.24f, 0.24f, 0.24f, 1.00f);
    colors[ImGuiCol_PlotLines] = ImVec4(0.86f, 0.93f, 0.89f, 1.00f);
    colors[ImGuiCol_PlotLinesHovered] = ImVec4(0.86f, 0.93f, 0.89f, 1.00f);
    colors[ImGuiCol_PlotHistogram] = ImVec4(0.86f, 0.93f, 0.89f, 1.00f);
    colors[ImGuiCol_PlotHistogramHovered] = ImVec4(0.86f, 0.93f, 0.89f, 1.00f);
    colors[ImGuiCol_TextSelectedBg] = ImVec4(0.24f, 0.24f, 0.24f, 1.00f);
    colors[ImGuiCol_DragDropTarget] = ImVec4(0.86f, 0.93f, 0.89f, 1.00f);
    colors[ImGuiCol_NavHighlight] = ImVec4(0.86f, 0.93f, 0.89f, 1.00f);
    colors[ImGuiCol_NavWindowingHighlight] = ImVec4(0.86f, 0.93f, 0.89f, 1.00f);
    colors[ImGuiCol_NavWindowingDimBg] = ImVec4(0.14f, 0.14f, 0.14f, 1.00f);
    colors[ImGuiCol_ModalWindowDimBg] = ImVec4(0.14f, 0.14f, 0.14f, 1.00f);

    style.WindowRounding = 0.0f;
    style.FrameRounding = 0.0f;
    style.GrabRounding = 0.0f;
    style.ScrollbarRounding = 0.0f;
    style.TabRounding = 0.0f;
}

void AddNewClip(std::vector<Clip>& clips, const std::string& input_path, int video_duration, int layer = 0) {
    std::string clip_name = std::filesystem::path(input_path).filename().string();
    clips.push_back(Clip{clip_name, 0, video_duration, layer, input_path});  // Default start at 0 and a default duration
}

// Modified get_video_duration using FFmpeg
int get_video_duration(const std::string& input_path) {
    AVFormatContext* fmt_ctx = nullptr;
    if (avformat_open_input(&fmt_ctx, input_path.c_str(), nullptr, nullptr) != 0) {
        return -1;
    }
    
    if (avformat_find_stream_info(fmt_ctx, nullptr) < 0) {
        avformat_close_input(&fmt_ctx);
        return -1;
    }
    
    int duration = static_cast<int>(fmt_ctx->duration / AV_TIME_BASE);
    avformat_close_input(&fmt_ctx);
    return duration;
}

std::string escape_path(const std::string& path) {
    std::string result = path;
    
    // Replace backslashes with forward slashes
    std::replace(result.begin(), result.end(), '\\', '/');
    
    // Escape special characters for FFmpeg
    std::string escaped;
    escaped.reserve(result.size() * 2);
    escaped += '\'';
    
    for (char c : result) {
        if (c == '\'')
            escaped += "\\'";
        else
            escaped += c;
    }
    
    escaped += '\'';
    return escaped;
}

// Get preferred sample format for an audio codec (avoiding deprecated sample_fmts)
AVSampleFormat get_preferred_sample_format(const AVCodec* codec) {
    // Default to float planar if we can't determine
    AVSampleFormat default_format = AV_SAMPLE_FMT_FLTP;
    
    // For AAC, we know these are common formats
    if (codec->id == AV_CODEC_ID_AAC) {
        return AV_SAMPLE_FMT_FLTP;
    }
    
    // For other codecs, try to find a supported format using newer API
    #if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(59, 37, 100)
        // Use new API if available
        if (codec->sample_fmts) {
            return codec->sample_fmts[0]; // Use the first supported sample format
        }
    #endif
    
    return default_format;
}

static int check_filter_status(AVFilterContext* ctx) {
    if (!ctx) return 0;
    
    AVFrame* tmp_frame = av_frame_alloc();
    if (!tmp_frame) return 0;
    
    int ret = av_buffersink_get_frame(ctx, tmp_frame);
    av_frame_free(&tmp_frame);
    
    return (ret == AVERROR_EOF) ? 1 : 0;
}

/* // Simple video editor function
bool execute_ffmpeg_cut(const std::vector<Clip>& clips, const std::string& output_path,
    int width = 1920, int height = 1080, int fps = 30) {
    if (clips.empty()) {
        std::cerr << "No clips provided\n";
        return false;
    }

    // Sort clips by layer (lower layers first, so higher layers overlay them)
    std::vector<Clip> sorted_clips = clips;
    std::sort(sorted_clips.begin(), sorted_clips.end(), 
    [](const Clip& a, const Clip& b) { return a.layer < b.layer; });

    // Create filter graph
    AVFilterGraph* filter_graph = avfilter_graph_alloc();
    FFMPEG_CHECK(!filter_graph, "Failed to allocate filter graph")

    // Create output file context
    AVFormatContext* output_ctx = nullptr;
    int ret = avformat_alloc_output_context2(&output_ctx, nullptr, nullptr, output_path.c_str());
    FFMPEG_CHECK(ret < 0, "Failed to create output context")

    // Setup output codecs and streams
    const AVCodec* video_codec = avcodec_find_encoder(AV_CODEC_ID_H264);
    const AVCodec* audio_codec = avcodec_find_encoder(AV_CODEC_ID_AAC);

    FFMPEG_CHECK(!video_codec, "H.264 encoder not found")
    FFMPEG_CHECK(!audio_codec, "AAC encoder not found")

    AVStream* video_stream = avformat_new_stream(output_ctx, nullptr);
    AVStream* audio_stream = avformat_new_stream(output_ctx, nullptr);

    FFMPEG_CHECK(!video_stream || !audio_stream, "Failed to create output streams")

    AVCodecContext* video_ctx = avcodec_alloc_context3(video_codec);
    AVCodecContext* audio_ctx = avcodec_alloc_context3(audio_codec);

    FFMPEG_CHECK(!video_ctx || !audio_ctx, "Failed to allocate codec contexts")

    // Configure video context
    video_ctx->width = width;
    video_ctx->height = height;
    video_ctx->time_base = (AVRational){1, fps};
    video_ctx->framerate = (AVRational){fps, 1};
    video_ctx->pix_fmt = AV_PIX_FMT_YUV420P;
    video_ctx->bit_rate = 2000000;  // 2 Mbps
    video_ctx->gop_size = fps;      // One keyframe per second

    // Configure audio context
    audio_ctx->sample_rate = 44100;
    audio_ctx->time_base = (AVRational){1, 44100};
    audio_ctx->bit_rate = 128000;   // 128 kbps

    // Set audio channel layout using AVChannelLayout (for newer FFmpeg versions)
    #if LIBAVUTIL_VERSION_MAJOR >= 57
    av_channel_layout_default(&audio_ctx->ch_layout, 2);
    #else
    audio_ctx->channel_layout = AV_CH_LAYOUT_STEREO;
    audio_ctx->channels = 2;
    #endif

    // Get appropriate sample format without using deprecated fields
    audio_ctx->sample_fmt = get_preferred_sample_format(audio_codec);

    // Open codecs
    ret = avcodec_open2(video_ctx, video_codec, nullptr);
    FFMPEG_CHECK(ret < 0, "Failed to open video codec")

    ret = avcodec_open2(audio_ctx, audio_codec, nullptr);
    FFMPEG_CHECK(ret < 0, "Failed to open audio codec")

    // Copy parameters to output streams
    ret = avcodec_parameters_from_context(video_stream->codecpar, video_ctx);
    FFMPEG_CHECK(ret < 0, "Failed to copy video codec parameters")

    ret = avcodec_parameters_from_context(audio_stream->codecpar, audio_ctx);
    FFMPEG_CHECK(ret < 0, "Failed to copy audio codec parameters")

    // Create the buffer sink filters (output endpoints)
    const AVFilter* buffersink = avfilter_get_by_name("buffersink");
    const AVFilter* abuffersink = avfilter_get_by_name("abuffersink");

    AVFilterContext* buffersink_ctx = nullptr;
    AVFilterContext* abuffersink_ctx = nullptr;

    // Using different approaches based on FFmpeg version
    /* #if LIBAVFILTER_VERSION_INT >= AV_VERSION_INT(7, 0, 0)
    // Newer FFmpeg - Create sink without parameters
    ret = avfilter_graph_create_filter(&buffersink_ctx, buffersink, "out_v", 
                        nullptr, nullptr, filter_graph);
    FFMPEG_CHECK(ret < 0, "Failed to create video buffer sink")

    // Set accepted pixel formats - use the method available in newer FFmpeg
    enum AVPixelFormat pix_fmts[] = { AV_PIX_FMT_YUV420P, AV_PIX_FMT_NONE };
    // Try alternative approach if the function is not available
    ret = av_opt_set_int_list(buffersink_ctx, "pix_fmts", pix_fmts,
                    AV_PIX_FMT_NONE, AV_OPT_SEARCH_CHILDREN);
    FFMPEG_CHECK(ret < 0, "Failed to set output pixel formats")
    #else */
    // Older FFmpeg - we need a different approach
    // Define filter graph description string
    /*char args[512];
    snprintf(args, sizeof(args), "video_size=%dx%d:pix_fmt=%d:time_base=%d/%d:pixel_aspect=%d/%d",
    width, height, AV_PIX_FMT_YUV420P, 1, fps, 1, 1);

    // Create the buffer sink filter
    ret = avfilter_graph_create_filter(&buffersink_ctx, buffersink, "out_v", 
                        nullptr, nullptr, filter_graph);
    FFMPEG_CHECK(ret < 0, "Failed to create video buffer sink")
    /* #endif */

    // Create audio sink
    /*ret = avfilter_graph_create_filter(&abuffersink_ctx, abuffersink, "out_a", 
                    nullptr, nullptr, filter_graph);
    FFMPEG_CHECK(ret < 0, "Failed to create audio buffer sink")

    // Set audio format options on the sink
    ret = av_opt_set_sample_fmt(abuffersink_ctx, "sample_fmts", 
                audio_ctx->sample_fmt, AV_OPT_SEARCH_CHILDREN);
    FFMPEG_CHECK(ret < 0, "Failed to set output sample format")

    ret = av_opt_set_int(abuffersink_ctx, "sample_rates", audio_ctx->sample_rate, 
        AV_OPT_SEARCH_CHILDREN);
    FFMPEG_CHECK(ret < 0, "Failed to set output sample rate")

    #if LIBAVUTIL_VERSION_MAJOR >= 57
    // For newer FFmpeg versions with AVChannelLayout
    char ch_layout_buf[128];
    av_channel_layout_describe(&audio_ctx->ch_layout, ch_layout_buf, sizeof(ch_layout_buf));
    ret = av_opt_set(abuffersink_ctx, "ch_layouts", ch_layout_buf, AV_OPT_SEARCH_CHILDREN);
    #else
    // For older FFmpeg versions
    char ch_layout[64];
    av_get_channel_layout_string(ch_layout, sizeof(ch_layout), 0, AV_CH_LAYOUT_STEREO);
    ret = av_opt_set(abuffersink_ctx, "channel_layouts", ch_layout, AV_OPT_SEARCH_CHILDREN);
    #endif
    FFMPEG_CHECK(ret < 0, "Failed to set output channel layout")

    // Process each clip and build filter chains
    std::vector<AVFilterContext*> video_outputs;
    std::vector<AVFilterContext*> audio_outputs;

    int clip_index = 0;
    for (const auto& clip : sorted_clips) {
        // Open input file
        AVFormatContext* input_ctx = nullptr;
        ret = avformat_open_input(&input_ctx, clip.path.c_str(), nullptr, nullptr);
        if (ret < 0) {
            std::cerr << "Failed to open: " << clip.path << std::endl;
            continue;
        }

        ret = avformat_find_stream_info(input_ctx, nullptr);
        FFMPEG_CHECK(ret < 0, "Failed to find stream info")

        // Find best streams
        int video_stream_idx = av_find_best_stream(input_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
        int audio_stream_idx = av_find_best_stream(input_ctx, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);

        // Create filter chain for video if available
        if (video_stream_idx >= 0) {
            std::string filter_desc;

            // Create source name
            char src_name[64], out_name[64];
            snprintf(src_name, sizeof(src_name), "v_src_%d", clip_index);
            snprintf(out_name, sizeof(out_name), "v_out_%d", clip_index);

            // Escape path properly
            std::string escaped_path = escape_path(clip.path);

            // Create filter specification
            // Convert microseconds to seconds for filter timing
            double start_sec = clip.start_time / 1000000.0;
            double duration_sec = clip.duration / 1000000.0;

            filter_desc = "movie=" + escaped_path + ":stream_index=" + std::to_string(video_stream_idx) + 
                "[" + src_name + "];" +
                "[" + src_name + "]trim=start=" + std::to_string(start_sec) + 
                ":duration=" + std::to_string(duration_sec) + "," +
                "setpts=PTS-STARTPTS," +
                "scale=" + std::to_string(width) + ":" + std::to_string(height) + 
                ":force_original_aspect_ratio=decrease," +
                "pad=" + std::to_string(width) + ":" + std::to_string(height) + 
                ":(ow-iw)/2:(oh-ih)/2" +
                "[" + out_name + "]";

            AVFilterInOut *inputs = nullptr, *outputs = nullptr;

            // Parse the filter graph
            ret = avfilter_graph_parse2(filter_graph, filter_desc.c_str(), &inputs, &outputs);
            if (ret < 0) {
                std::cerr << "Failed to parse video filter graph for clip: " << clip.path << std::endl;
                avformat_close_input(&input_ctx);
                continue;
            }

            // Find output filter - must happen before config
            AVFilterContext* out_ctx = avfilter_graph_get_filter(filter_graph, out_name);
            if (out_ctx) {
                video_outputs.push_back(out_ctx);
            }

            avfilter_inout_free(&inputs);
            avfilter_inout_free(&outputs);
        }

        // Create filter chain for audio if available
        if (audio_stream_idx >= 0) {
            std::string filter_desc;

            // Create source name
            char src_name[64], out_name[64];
            snprintf(src_name, sizeof(src_name), "a_src_%d", clip_index);
            snprintf(out_name, sizeof(out_name), "a_out_%d", clip_index);

            // Escape path properly
            std::string escaped_path = escape_path(clip.path);

            // Convert microseconds to seconds for filter timing
            double start_sec = clip.start_time / 1000000.0;
            double duration_sec = clip.duration / 1000000.0;

            // Get sample format name
            const char* sample_fmt_name = av_get_sample_fmt_name(audio_ctx->sample_fmt);
            if (!sample_fmt_name) sample_fmt_name = "fltp"; // Default to fltp if unknown

            // Create filter specification
            filter_desc = "amovie=" + escaped_path + ":stream_index=" + std::to_string(audio_stream_idx) + 
                "[" + src_name + "];" +
                "[" + src_name + "]atrim=start=" + std::to_string(start_sec) + 
                ":duration=" + std::to_string(duration_sec) + "," +
                "asetpts=PTS-STARTPTS," +
                "aformat=sample_fmts=" + std::string(sample_fmt_name) + 
                ":sample_rates=" + std::to_string(audio_ctx->sample_rate) + 
                ":channel_layouts=stereo" +
                "[" + out_name + "]";

            AVFilterInOut *inputs = nullptr, *outputs = nullptr;

            // Parse the filter graph
            ret = avfilter_graph_parse2(filter_graph, filter_desc.c_str(), &inputs, &outputs);
            if (ret < 0) {
                std::cerr << "Failed to parse audio filter graph for clip: " << clip.path << std::endl;
                avformat_close_input(&input_ctx);
                continue;
            }

            // Find output filter
            AVFilterContext* out_ctx = avfilter_graph_get_filter(filter_graph, out_name);
            if (out_ctx) {
                audio_outputs.push_back(out_ctx);
            }

            avfilter_inout_free(&inputs);
            avfilter_inout_free(&outputs);
        }

        avformat_close_input(&input_ctx);
        clip_index++;
    }

    // Connect video layers using overlay filters
    AVFilterContext* last_video = nullptr;

    if (!video_outputs.empty()) {
        last_video = video_outputs[0];

        // Link subsequent layers with overlay filter
        for (size_t i = 1; i < video_outputs.size(); i++) {
            // Create overlay filter
            const AVFilter* overlay_filter = avfilter_get_by_name("overlay");
            AVFilterContext* overlay_ctx = nullptr;

            char overlay_name[64];
            snprintf(overlay_name, sizeof(overlay_name), "overlay_%zu", i);

            ret = avfilter_graph_create_filter(&overlay_ctx, overlay_filter, overlay_name, 
                                    "shortest=1:format=yuv420", nullptr, filter_graph);
            FFMPEG_CHECK(ret < 0, "Failed to create overlay filter")

            // Link previous result as background and new clip as overlay
            ret = avfilter_link(last_video, 0, overlay_ctx, 0);
            FFMPEG_CHECK(ret < 0, "Failed to link video background")

            ret = avfilter_link(video_outputs[i], 0, overlay_ctx, 1);
            FFMPEG_CHECK(ret < 0, "Failed to link video overlay")

            // Update last video output
            last_video = overlay_ctx;
        }

        // Connect final video output to sink
        ret = avfilter_link(last_video, 0, buffersink_ctx, 0);
        FFMPEG_CHECK(ret < 0, "Failed to link video output")
    }

    // Handle audio mixing
    AVFilterContext* last_audio = nullptr;

    if (!audio_outputs.empty()) {
        if (audio_outputs.size() == 1) {
            last_audio = audio_outputs[0];
        } else {
        // Create amix filter for multiple audio tracks
        const AVFilter* amix_filter = avfilter_get_by_name("amix");
        AVFilterContext* amix_ctx = nullptr;

        char args[128];
        snprintf(args, sizeof(args), "inputs=%zu:duration=longest", audio_outputs.size());

        ret = avfilter_graph_create_filter(&amix_ctx, amix_filter, "amix", 
                                args, nullptr, filter_graph);
        FFMPEG_CHECK(ret < 0, "Failed to create audio mixer")

        // Connect all audio outputs to mixer
        for (size_t i = 0; i < audio_outputs.size(); i++) {
            ret = avfilter_link(audio_outputs[i], 0, amix_ctx, i);
            if (ret < 0) {
                std::cerr << "Failed to link audio input " << i << std::endl;
                continue;
            }
        }

        last_audio = amix_ctx;
        }

        // Connect final audio output to sink
        if (last_audio) {
            ret = avfilter_link(last_audio, 0, abuffersink_ctx, 0);
            FFMPEG_CHECK(ret < 0, "Failed to link audio output")
        }
    }

    // Configure the filter graph
    ret = avfilter_graph_config(filter_graph, nullptr);
    FFMPEG_CHECK(ret < 0, "Failed to configure filter graph")

    // Open output file
    ret = avio_open(&output_ctx->pb, output_path.c_str(), AVIO_FLAG_WRITE);
    FFMPEG_CHECK(ret < 0, "Failed to open output file")

    // Write file header
    ret = avformat_write_header(output_ctx, nullptr);
    FFMPEG_CHECK(ret < 0, "Failed to write file header")

    // Allocate frame and packet
    AVFrame* frame = av_frame_alloc();
    AVPacket* pkt = av_packet_alloc();
    FFMPEG_CHECK(!frame || !pkt, "Failed to allocate frame or packet")

    // Initialize presentation timestamps
    int64_t video_pts = 0;
    int64_t audio_pts = 0;

    // Main encoding loop with proper error handling
    bool video_done = !last_video;
    bool audio_done = !last_audio;

    while (!video_done || !audio_done) {
        // Process video frames
        if (!video_done) {
            av_frame_unref(frame);
            ret = av_buffersink_get_frame(buffersink_ctx, frame);

            if (ret >= 0) {
                // Set correct PTS
                frame->pts = video_pts++;

                // Encode video frame
                ret = avcodec_send_frame(video_ctx, frame);
                if (ret < 0) {
                    std::cerr << "Error sending video frame to encoder: " << av_err2str(ret) << std::endl;
                } else {
                    // Get encoded packets
                    while (true) {
                        av_packet_unref(pkt);
                        ret = avcodec_receive_packet(video_ctx, pkt);
                        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                            break;
                        }
                        if (ret < 0) {
                            std::cerr << "Error encoding video frame: " << av_err2str(ret) << std::endl;
                            break;
                        }
                        
                        // Set correct stream index and rescale timestamps
                        pkt->stream_index = video_stream->index;
                        av_packet_rescale_ts(pkt, video_ctx->time_base, video_stream->time_base);
                        
                        // Write packet
                        ret = av_interleaved_write_frame(output_ctx, pkt);
                        if (ret < 0) {
                            std::cerr << "Error writing video packet: " << av_err2str(ret) << std::endl;
                        }
                    }
                }
            } else if (ret == AVERROR_EOF) {
            video_done = true;
            } else if (ret != AVERROR(EAGAIN)) {
            std::cerr << "Error getting video frame from filter: " << av_err2str(ret) << std::endl;
            video_done = true;
            }
        }

        // Process audio frames
        if (!audio_done) {
            av_frame_unref(frame);
            ret = av_buffersink_get_frame(abuffersink_ctx, frame);

            if (ret >= 0) {
                // Set correct PTS
                frame->pts = audio_pts;
                audio_pts += frame->nb_samples;

                // Encode audio frame
                ret = avcodec_send_frame(audio_ctx, frame);
                if (ret < 0) {
                    std::cerr << "Error sending audio frame to encoder: " << av_err2str(ret) << std::endl;
                } else {
                    // Get encoded packets
                    while (true) {
                        av_packet_unref(pkt);
                        ret = avcodec_receive_packet(audio_ctx, pkt);
                        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                            break;
                        }
                        if (ret < 0) {
                            std::cerr << "Error encoding audio frame: " << av_err2str(ret) << std::endl;
                            break;
                        }
                        
                        // Set correct stream index and rescale timestamps
                        pkt->stream_index = audio_stream->index;
                        av_packet_rescale_ts(pkt, audio_ctx->time_base, audio_stream->time_base);
                        
                        // Write packet
                        ret = av_interleaved_write_frame(output_ctx, pkt);
                        if (ret < 0) {
                            std::cerr << "Error writing audio packet: " << av_err2str(ret) << std::endl;
                        }
                    }
                }
            } else if (ret == AVERROR_EOF) {
                audio_done = true;
            } else if (ret != AVERROR(EAGAIN)) {
                std::cerr << "Error getting audio frame from filter: " << av_err2str(ret) << std::endl;
                audio_done = true;
            }
        }
    }

    // Flush encoders
    if (last_video) {
        avcodec_send_frame(video_ctx, nullptr);
        while (true) {
            av_packet_unref(pkt);
            ret = avcodec_receive_packet(video_ctx, pkt);
            if (ret == AVERROR_EOF || ret == AVERROR(EAGAIN)) {
                break;
            }

            pkt->stream_index = video_stream->index;
            av_packet_rescale_ts(pkt, video_ctx->time_base, video_stream->time_base);
            av_interleaved_write_frame(output_ctx, pkt);
        }
    }

    if (last_audio) {
        avcodec_send_frame(audio_ctx, nullptr);
        while (true) {
            av_packet_unref(pkt);
            ret = avcodec_receive_packet(audio_ctx, pkt);
            if (ret == AVERROR_EOF || ret == AVERROR(EAGAIN)) {
                break;
            }

            pkt->stream_index = audio_stream->index;
            av_packet_rescale_ts(pkt, audio_ctx->time_base, audio_stream->time_base);
            av_interleaved_write_frame(output_ctx, pkt);
        }
    }

    // Write trailer
    av_write_trailer(output_ctx);

    // Clean up resources in the correct order
    avio_closep(&output_ctx->pb);
    avformat_free_context(output_ctx);
    avfilter_graph_free(&filter_graph);
    avcodec_free_context(&video_ctx);
    avcodec_free_context(&audio_ctx);
    av_frame_free(&frame);
    av_packet_free(&pkt);

    std::cout << "Successfully created: " << output_path << std::endl;
    return true;
}
 */
#ifndef AV_ERROR_MAX_STRING_SIZE
#define AV_ERROR_MAX_STRING_SIZE 64
#endif

// Define av_err2str as a function instead of a macro for better compatibility
#undef av_err2str
static const char* av_err2str(int errnum) {
    static char str[AV_ERROR_MAX_STRING_SIZE];
    memset(str, 0, sizeof(str));
    return av_make_error_string(str, AV_ERROR_MAX_STRING_SIZE, errnum);
}

int main(int argc, char* argv[]) {

    avformat_network_init();  // Initialize FFmpeg

    SDL_Window *window;                    // Declare a pointer

    SDL_Init(SDL_INIT_VIDEO);              // Initialize SDL3

    // Create an application window with the following settings:
    window = SDL_CreateWindow(
        "Zest",                            // window title
        640,                               // width, in pixels
        480,                               // height, in pixels
        SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_MAXIMIZED              // flags - see below
    );

    // Check that the window was successfully created
    if (window == NULL) {
        // In the case that the window could not be made...
        SDL_LogError(SDL_LOG_CATEGORY_ERROR, "Could not create window: %s\n", SDL_GetError());
        return 1;
    }

    SDL_GLContext gl_context = SDL_GL_CreateContext(window);
    if (!gl_context) {
        SDL_LogError(SDL_LOG_CATEGORY_ERROR, "Failed to create OpenGL context: %s\n", SDL_GetError());
        return 1;
    }

    // Set up vsync if desired
    // SDL_GL_SetSwapInterval(1);

    // Then initialize GLAD
    if (!gladLoadGLLoader((GLADloadproc)SDL_GL_GetProcAddress)) {
        SDL_LogError(SDL_LOG_CATEGORY_ERROR, "Failed to initialize GLAD\n");
        return 1;
    }

    SDL_Surface* icon_surface = IMG_Load("assets/logo.png"); // path to your logo
    if (icon_surface) {
        SDL_SetWindowIcon(window, icon_surface);
        SDL_DestroySurface(icon_surface); // Free after setting
    }

    SDL_Renderer *renderer = NULL;
    renderer = SDL_CreateRenderer(window, NULL);

    // create render target
    SDL_Texture* texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_BGRA8888, SDL_TEXTUREACCESS_TARGET, 300, 200);

    // load image
    SDL_Surface* surf = IMG_Load("assets/image.png");
    SDL_Texture* charlie_texture = SDL_CreateTextureFromSurface(renderer, surf);
    SDL_DestroySurface(surf);

    // draw texture
    SDL_SetRenderTarget(renderer, texture);
    SDL_SetRenderDrawColor(renderer, 80, 200, 230, 1);
    SDL_RenderClear(renderer);

    SDL_FRect rect{50, 50, 100, 200};
    SDL_RenderTexture(renderer, charlie_texture,NULL, &rect);

    SDL_SetRenderTarget(renderer, NULL);

    // init imgui
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO(); (void)io;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;     // Enable Keyboard Controls
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;         // Enable Docking

    // Setup Dear ImGui style
    ImGui::StyleColorsDark();

    // Setup Platform/Renderer backends
    ImGui_ImplSDL3_InitForSDLRenderer(window, renderer);
    ImGui_ImplSDLRenderer3_Init(renderer);

    io.Fonts->AddFontFromFileTTF("c:\\Windows\\Fonts\\segoeui.ttf", 18.0f);

    SetupImGuiStyle();

    // State variables
    char input_path[256] = "";
    char output_path[256] = "output.mp4";
    int start_time = 0;
    int duration = 10;
    bool process_success = false;
    std::string process_message = "";
    
    int max_duration = 0;
    float zoom_factor = 1.0f;

    int video_duration = 0;

    bool file_dropped = false;

    std::vector<Clip> clips; // {clip_name, start_time, duration, path}
    int playhead_position = 0;

    SDL_Texture* preview_texture = nullptr;
    int last_playhead_position = -1;
    static bool layers_changed = false;

    // Main loop
    bool running = true;
    while (running) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_EVENT_QUIT) {
                running = false;
            }

            // Handle drag-and-drop files
            if (event.type == SDL_EVENT_DROP_FILE) {
                if (event.drop.data) {
                    std::cout << "Dropped \n";
                    strncpy(input_path, event.drop.data, IM_ARRAYSIZE(input_path) - 1);

                    
                    file_dropped = true;

                    video_duration = get_video_duration(input_path);
                    if (video_duration == -1) {
                        process_message = "Failed to determine video duration!";
                    } else {
                        process_message = "Video duration loaded successfully!";
                    }

                    std::cout << "Input Path: " << input_path << "\n";
                    std::cout << "Video Duration: " << video_duration << "\n";

                    AddNewClip(clips, input_path, video_duration);

                }

            }



            ImGui_ImplSDL3_ProcessEvent(&event);
        }


        // Start ImGui frame
        ImGui_ImplSDLRenderer3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();

        // ImGui GUI
        ImGui::Begin("FFmpeg Video Editor");
        ImGui::Text("Drag and drop a video file or enter the file path:");
        ImGui::InputText("Input Path", input_path, IM_ARRAYSIZE(input_path));
        ImGui::Text("Set output file path:");
        ImGui::InputText("Output Path", output_path, IM_ARRAYSIZE(output_path));


        DrawTimelineEditor(clips, playhead_position, start_time, duration, max_duration, zoom_factor, last_playhead_position, layers_changed);

        if (playhead_position != last_playhead_position) {
            UpdatePreview(renderer, preview_texture, clips, playhead_position);
            last_playhead_position = playhead_position;
        }

        if (layers_changed || playhead_position != last_playhead_position) {
            UpdatePreview(renderer, preview_texture, clips, playhead_position);
            last_playhead_position = playhead_position;
            layers_changed = false;
        }

        // Render the preview window
        RenderPreviewWindow(preview_texture);

        ImGui::Separator();

        if (ImGui::Button("Cut Video")) {
            // Ensure OpenGL context is active
            SDL_GL_MakeCurrent(window, SDL_GL_GetCurrentContext());
            if (!SDL_GL_GetCurrentContext()) {
                std::cerr << "OpenGL context is not active!" << std::endl;
                process_message = "OpenGL context error!";
                process_success = false;
            } else {
                if (std::filesystem::exists(input_path)) {
                    if (start_time < 0 || duration <= 0 || start_time + duration > video_duration) {
                        process_message = "Invalid start time or duration!";
                        process_success = false;
                    } else {
                        int fps = 30;
                        int total_frames_needed = max_duration * fps;
                        process_success = start_video_export("output.mp4", 1920, 1080, 30, 
                            total_frames_needed, clips, window);
                        process_message = process_success ? "Video cut successfully!" : "Failed to cut video!";
                    }
                } else {
                    process_message = "Input file does not exist!";
                }
            }
        }

        ImGui::TextWrapped("Status: %s", process_message.c_str());
        ImGui::End();

        // Render ImGui
        ImGui::Render();

        SDL_SetRenderDrawColor(renderer, 30, 30, 30, 255); // Dark gray background
        SDL_RenderClear(renderer);

        ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer);
        SDL_RenderPresent(renderer);
    }
    // The window is open: could enter program loop here (see SDL_PollEvent())

    SDL_Delay(300);

    // Cleanup
    ImGui_ImplSDLRenderer3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();

    SDL_GL_DestroyContext(gl_context);

    SDL_DestroyTexture(texture);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);

    SDL_Quit();

    return 0;
}


void DrawTimelineEditor(
    std::vector<Clip>& clips,
    int& playhead_position,
    int& start_time,
    int& duration,
    int& max_duration,
    float& zoom_factor,
    int& last_playhead_position,
    bool& layers_changed
) {
    ImGui::Begin("Timeline Editor");

    float timeline_width = ImGui::GetContentRegionAvail().x;
    const float resize_edge_area = 10.0f; // Larger area for resizing
    const float move_area_width = 20.0f;
    const float layer_height = 60.0f;
    const float layer_padding = 10.0f;

    int max_layer = 0;
    for (const auto& clip : clips) {
        max_layer = std::max(max_layer, clip.layer);
    }
    max_layer += 1;

    float timeline_height = max_layer * (layer_height + layer_padding);

    ImGui::Text("Timeline:");
    ImGui::Separator();

    // Zoom controls
    if (ImGui::Button("Zoom In")) {
        zoom_factor *= 1.1f;
    }
    ImGui::SameLine();
    if (ImGui::Button("Zoom Out")) {
        zoom_factor /= 1.1f;
        max_duration *= 1.1f;
    }

    // Calculate the maximum duration of all clips
    for (const auto& clip : clips) {

        max_duration = std::max(max_duration, clip.start_time + clip.duration);
    }

    int timeline_size = max_duration / zoom_factor;

    // Draw background bar for the timeline
    ImVec2 timeline_start = ImGui::GetCursorScreenPos();
    ImVec2 timeline_end = ImVec2(timeline_start.x + timeline_width, timeline_start.y + timeline_height);
    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    draw_list->AddRectFilled(timeline_start, timeline_end, IM_COL32(100, 100, 100, 255));

    // Draw layer separators
    for (int layer = 0; layer < max_layer; ++layer) {
        float y = timeline_start.y + layer * (layer_height + layer_padding);
        draw_list->AddLine(ImVec2(timeline_start.x, y), ImVec2(timeline_end.x, y), IM_COL32(255, 255, 255, 255));
    }

    static int dragging_clip_index = -1;
    static float drag_offset_x = 0.0f;
    static bool resizing_left = false;
    static bool resizing_right = false;

    // Draw clips on the timeline
    for (size_t i = 0; i < clips.size(); ++i) {
        auto& clip = clips[i];

        // Calculate the vertical position based on the layer
        float layer_offset_y = clip.layer * (layer_height + layer_padding);

        // Convert the clip's start time and duration to screen space
        float clip_start_x = timeline_start.x + (float(clip.start_time) / max_duration) * timeline_width * zoom_factor;
        float clip_end_x = clip_start_x + (float(clip.duration) / max_duration) * timeline_width * zoom_factor;

         // Clip rectangle
        ImVec2 clip_rect_min = ImVec2(clip_start_x, timeline_start.y + layer_offset_y + layer_padding);
        ImVec2 clip_rect_max = ImVec2(clip_end_x, timeline_start.y + layer_offset_y + layer_height);

        // Draw the clip's background
        draw_list->AddRectFilled(clip_rect_min, clip_rect_max, IM_COL32(255, 100, 100, 255));

        // Highlight the borders
        draw_list->AddRect(clip_rect_min, clip_rect_max, IM_COL32(255, 255, 255, 255));

        // Add draggable area in the center of the clip
        ImVec2 drag_area_start = ImVec2(clip_start_x + move_area_width, timeline_start.y + layer_offset_y + layer_padding);
        ImVec2 drag_area_end = ImVec2(clip_end_x - move_area_width, timeline_start.y + layer_offset_y + layer_height);
        ImGui::SetCursorScreenPos(drag_area_start);
        ImGui::InvisibleButton(("clip_move" + std::to_string(i)).c_str(), ImVec2(drag_area_end.x - drag_area_start.x, layer_height - layer_padding));

        // Indicate where to drag
        if (ImGui::IsItemHovered()) {
            draw_list->AddRect(drag_area_start, drag_area_end, IM_COL32(255, 255, 0, 255));  // Yellow highlight
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);  // Change cursor to show dragging
        }

        // Make the clip draggable
        if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
            if (dragging_clip_index == -1 && !resizing_left && !resizing_right) {
                dragging_clip_index = i;
                drag_offset_x = ImGui::GetMousePos().x - clip_start_x;
            }

            if (dragging_clip_index == i) {
                // Get the mouse position
                float mouse_x = ImGui::GetMousePos().x;

                // Calculate the new start time based on the mouse position and offset
                int new_start_time = int(((mouse_x - drag_offset_x - timeline_start.x) / timeline_width * zoom_factor) * max_duration);

                // Ensure that the clip's start_time doesn't go negative
                new_start_time = std::max(0, new_start_time);

                // Make sure the clip doesn't exceed the end of the video
                new_start_time = std::min(new_start_time, max_duration - clip.duration);

                // Update the clip's start time
                clip.start_time = new_start_time;
            }
        } else if (dragging_clip_index == i) {
            dragging_clip_index = -1;
        }

          // Add resize handles for left and right edges
        float resize_handle_size = 6.0f; // Size of the resize handles (small boxes)
        ImVec2 left_handle_min = ImVec2(clip_start_x - resize_handle_size / 2, timeline_start.y + layer_offset_y + layer_padding);
        ImVec2 left_handle_max = ImVec2(clip_start_x + resize_handle_size / 2, timeline_start.y + layer_offset_y + layer_height);
        ImVec2 right_handle_min = ImVec2(clip_end_x - resize_handle_size / 2, timeline_start.y + layer_offset_y + layer_padding);
        ImVec2 right_handle_max = ImVec2(clip_end_x + resize_handle_size / 2, timeline_start.y + layer_offset_y + layer_height);

        draw_list->AddRectFilled(left_handle_min, left_handle_max, IM_COL32(255, 255, 255, 255));  // White resize handle
        draw_list->AddRectFilled(right_handle_min, right_handle_max, IM_COL32(255, 255, 255, 255));  // White resize handle

        // Left edge resizing
        ImGui::SetCursorScreenPos(left_handle_min);
        ImGui::InvisibleButton(("clip_resize_left" + std::to_string(i)).c_str(), ImVec2(resize_handle_size, layer_height - layer_padding));
        if (ImGui::IsItemHovered()) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);  // Show resize cursor for left edge
        }
        if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
            if (!resizing_right) {
                resizing_left = true;
                // Get the mouse position
                float mouse_x = ImGui::GetMousePos().x;

                // Calculate the new start time and duration based on the mouse position
                int new_start_time = int(((mouse_x - timeline_start.x) / timeline_width * zoom_factor) * max_duration);
                int new_duration = clip.duration + (clip.start_time - new_start_time);

                // Ensure that the clip's start_time doesn't go negative
                new_start_time = std::max(0, new_start_time);

                // Ensure duration is not negative
                new_duration = std::max(1, new_duration);

                // Ensure the duration does not exceed the video duration
                new_duration = std::min(max_duration - new_start_time, new_duration);

                // Update the clip's start time and duration
                clip.start_time = new_start_time;
                clip.duration = new_duration;
            }
        } else if (resizing_left) {
            resizing_left = false;
        }

        // Right edge resizing
        ImGui::SetCursorScreenPos(right_handle_min);
        ImGui::InvisibleButton(("clip_resize_right" + std::to_string(i)).c_str(), ImVec2(resize_handle_size, layer_height - layer_padding));
        if (ImGui::IsItemHovered()) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);  // Show resize cursor for right edge
        }
        if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
            if (!resizing_left) {
                resizing_right = true;
                // Get the mouse position
                float mouse_x = ImGui::GetMousePos().x;

                // Calculate the new duration based on the mouse position
                int new_duration = int(((mouse_x - clip_start_x) / timeline_width / zoom_factor) * max_duration);

                // Ensure the duration stays within bounds
                new_duration = std::min(int((max_duration - clip.start_time) ), new_duration);

                // Ensure duration is not negative
                new_duration = std::max(1, new_duration);
  
                // Update the clip's duration
                clip.duration = new_duration;
            }
        } else if (resizing_right) {
            resizing_right = false;
        }

        // Layer selection
        ImGui::SetCursorScreenPos(ImVec2(clip_start_x, timeline_start.y + layer_offset_y + layer_height + 5));
        ImGui::Text("Layer:");
        ImGui::SameLine();
        std::string layer_id = "##layer" + std::to_string(i);
        ImGui::SetCursorScreenPos(ImVec2(clip_start_x + 50, timeline_start.y + layer_offset_y + layer_height + 5));
        if (ImGui::Button(("<##" + std::to_string(i)).c_str())) {
            clip.layer = std::max(0, clip.layer - 1);
            layers_changed = true;
        }
        ImGui::SameLine();
        if (ImGui::Button((">##" + std::to_string(i)).c_str())) {
            clip.layer = clip.layer + 1;
            layers_changed = true;
        }
    }

    ImGui::Separator();
    ImGui::Text("Playhead Position: %d seconds", playhead_position);

    // Playhead position slider
    ImGui::SliderInt("Playhead", &playhead_position, 0, max_duration);

    ImGui::End();

    // Active Clips List
    ImGui::Begin("Active Clips");
    ImGui::Text("Clip List:");
    ImGui::Separator();

    for (size_t i = 0; i < clips.size(); ++i) {
        auto& clip = clips[i];
        ImGui::Text("%zu: %s (Start: %d, Duration: %d, Layer: %d)", i, clip.name.c_str(), clip.start_time, clip.duration, clip.layer);
        if (ImGui::Button(("Delete##" + std::to_string(i)).c_str())) {
            clips.erase(clips.begin() + i);
            break; // Avoid iterating over a modified vector
        }
    }

    ImGui::End();
}


// Basic FFmpeg-based preview generation
SDL_Texture* CreatePreviewTexture(SDL_Renderer* renderer, const std::vector<Clip>& clips, int playhead_time) {
    if (clips.empty()) return nullptr;

    AVFormatContext* fmt_ctx = nullptr;
    AVCodecParameters* codecpar = nullptr;
    const AVCodec* codec = nullptr;
    AVCodecContext* codec_ctx = nullptr;
    AVFrame* frame = av_frame_alloc();
    AVPacket pkt;
    int video_stream = -1;
    SDL_Texture* texture = nullptr;

    // Open first clip for preview
    if (avformat_open_input(&fmt_ctx, clips[0].path.c_str(), nullptr, nullptr) != 0) {
        return nullptr;
    }

    avformat_find_stream_info(fmt_ctx, nullptr);
    
    // Find video stream
    for (unsigned int i = 0; i < fmt_ctx->nb_streams; i++) {
        if (fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            video_stream = i;
            codecpar = fmt_ctx->streams[i]->codecpar;
            break;
        }
    }

    if (video_stream == -1) {
        avformat_close_input(&fmt_ctx);
        return nullptr;
    }

    codec = avcodec_find_decoder(codecpar->codec_id);
    codec_ctx = avcodec_alloc_context3(codec);
    avcodec_parameters_to_context(codec_ctx, codecpar);
    avcodec_open2(codec_ctx, codec, nullptr);

    // Seek to requested time
    av_seek_frame(fmt_ctx, video_stream, 
        playhead_time * AV_TIME_BASE / 1000, AVSEEK_FLAG_BACKWARD);

    while (av_read_frame(fmt_ctx, &pkt) >= 0) {
        if (pkt.stream_index == video_stream) {
            avcodec_send_packet(codec_ctx, &pkt);
            if (avcodec_receive_frame(codec_ctx, frame) == 0) {
                // Create SDL texture
                texture = SDL_CreateTexture(
                    renderer,
                    SDL_PIXELFORMAT_YV12,
                    SDL_TEXTUREACCESS_STREAMING,
                    frame->width,
                    frame->height
                );

                SDL_UpdateYUVTexture(texture, nullptr,
                    frame->data[0], frame->linesize[0],
                    frame->data[1], frame->linesize[1],
                    frame->data[2], frame->linesize[2]);
                break;
            }
        }
        av_packet_unref(&pkt);
    }

    // Cleanup
    av_frame_free(&frame);
    avcodec_free_context(&codec_ctx);
    avformat_close_input(&fmt_ctx);
    return texture;
}

void UpdatePreview(SDL_Renderer* renderer, SDL_Texture*& preview_texture, 
                  const std::vector<Clip>& clips, int playhead_time) {
    // Only regenerate if time changed significantly
    static int last_preview_time = -1;
    if (abs(playhead_time - last_preview_time) < 0.5f) return;
    
    SDL_Texture* new_texture = CreatePreviewTexture(renderer, clips, playhead_time);
    if (new_texture) {
        if (preview_texture) SDL_DestroyTexture(preview_texture);
        preview_texture = new_texture;
        last_preview_time = playhead_time;
    }
}

void RenderPreviewWindow(SDL_Texture* preview_texture) {
    ImGui::Begin("Video Preview");
    
    if (preview_texture) {
        // Get window size while maintaining aspect ratio
        float tex_w, tex_h;
        SDL_GetTextureSize(preview_texture, &tex_w, &tex_h);
        float aspect = static_cast<float>(tex_w) / tex_h;
        
        ImVec2 avail = ImGui::GetContentRegionAvail();
        ImVec2 preview_size(avail.x, avail.x / aspect);
        
        ImGui::Image(reinterpret_cast<ImTextureID>(preview_texture), preview_size);
    } else {
        ImGui::Text("Preview unavailable");
    }
    
    ImGui::End();
}