#include <vector>
#include <algorithm>
#include <iostream>
#include <string>
#include <sstream>
#include <filesystem>
#include <cstdio>
#include <cstdlib> // For system()
#include <regex>
#include <fstream> // For std::ofstream
#include <atomic>  // For layers_changed, playing
#include <cmath>   // For std::abs
#include <map>     // For GLResources caches
#include <deque>   // For VideoData frame_cache
#include <mutex>   // For potential future threading
#include <limits>  // For numeric_limits

// External C libraries (FFmpeg, glad, tinyfiledialogs)
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
    #include <libavutil/error.h> // For av_err2str
    #include <libavutil/imgutils.h> // For av_image_* functions
    #include <glad/glad.h>
    #include <tinyfiledialogs.h>
}

// C++ libraries (ImGui, SDL3)
#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_opengl3.h>
#include <SDL3/SDL.h>
#include <SDL3/SDL_video.h>
#include <SDL3/SDL_image.h> // Use SDL_image for SDL3

// Project headers
#include "video_export.hpp" // Include AFTER system headers and glad/imgui
#include "shared.hpp"
#include "project_io.hpp"

// Global gradient data storage (used by ApplyWindowBackgroundGradients)
struct GradientData {
    ImVec4 window_grad_top;
    ImVec4 window_grad_bottom;
};

// === Playback state ===
std::atomic<bool> playing = false;
float playhead_time = 0.0f;
Uint64 last_frame_ticks = 0; // Use SDL_GetTicks for frame delta calculation

int preview_width = 1280;
int preview_height = 720;

Clip* selected_clip = nullptr; // Keep track of selected clip

bool show_thumbs = false;

// Error checking macros
#define CHECK_AV_ERROR(ret, message) \
    if (ret < 0) { \
        char errbuf[AV_ERROR_MAX_STRING_SIZE]; \
        av_make_error_string(errbuf, sizeof(errbuf), ret); \
        std::cerr << "ERROR: " << message << ": " << errbuf << " (code " << ret << ")" << std::endl; \
        std::cerr << "At " << __FILE__ << ":" << __LINE__ << std::endl; \
        /* Consider returning false or throwing an exception */ \
    }

#define FFMPEG_CHECK(condition, message) \
if (condition) { \
    std::cerr << "Error: " << message << std::endl; \
    /* Consider returning false or throwing an exception */ \
}

// --- ImGui Styling and Custom Rendering ---

void SetupImGuiStyle() {
    ImGuiStyle& style = ImGui::GetStyle();
    ImVec4* colors = style.Colors;

    // Refined color palette with enhanced visual clarity
    ImVec4 bg_dark          = ImVec4(0.13f, 0.12f, 0.15f, 1.00f); // Darker base for gradient
    ImVec4 bg_light         = ImVec4(0.17f, 0.16f, 0.19f, 1.00f); // Lighter top for gradient
    ImVec4 bg_alt           = ImVec4(0.15f, 0.15f, 0.17f, 1.00f);

    // Refined orange tones for better clarity and distinction
    ImVec4 accent           = ImVec4(1.00f, 0.56f, 0.15f, 1.00f); // More vibrant base orange
    ImVec4 accent_hover     = ImVec4(1.00f, 0.67f, 0.25f, 1.00f); // Brighter, more distinct hover
    ImVec4 accent_active    = ImVec4(1.00f, 0.42f, 0.00f, 1.00f); // Deeper, richer active state
    ImVec4 accent_muted     = ImVec4(0.85f, 0.48f, 0.12f, 1.00f); // Muted orange for secondary elements

    ImVec4 text             = ImVec4(0.98f, 0.98f, 0.98f, 1.00f);
    ImVec4 text_secondary   = ImVec4(0.80f, 0.80f, 0.80f, 1.00f);
    ImVec4 frame_bg         = ImVec4(0.16f, 0.16f, 0.18f, 1.00f);

    // Apply the new color palette
    colors[ImGuiCol_Text]                  = text;
    colors[ImGuiCol_TextDisabled]          = ImVec4(0.60f, 0.60f, 0.60f, 1.00f);
    colors[ImGuiCol_WindowBg]              = bg_dark; // Base color for gradient (will be modified in rendering)
    colors[ImGuiCol_ChildBg]               = ImVec4(0.18f, 0.18f, 0.21f, 0.80f); // Slightly transparent
    colors[ImGuiCol_PopupBg]               = ImVec4(0.12f, 0.12f, 0.14f, 0.94f);
    colors[ImGuiCol_Border]                = ImVec4(0.30f, 0.30f, 0.32f, 0.50f);
    colors[ImGuiCol_BorderShadow]          = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
    colors[ImGuiCol_FrameBg]               = frame_bg;
    colors[ImGuiCol_FrameBgHovered]        = ImVec4(accent_hover.x, accent_hover.y, accent_hover.z, 0.40f);
    colors[ImGuiCol_FrameBgActive]         = ImVec4(accent_active.x, accent_active.y, accent_active.z, 0.67f);
    colors[ImGuiCol_TitleBg]               = bg_alt;
    colors[ImGuiCol_TitleBgActive]         = accent_muted;
    colors[ImGuiCol_TitleBgCollapsed]      = ImVec4(bg_dark.x, bg_dark.y, bg_dark.z, 0.75f);
    colors[ImGuiCol_MenuBarBg]             = ImVec4(0.12f, 0.12f, 0.15f, 1.00f);
    colors[ImGuiCol_ScrollbarBg]           = ImVec4(0.18f, 0.18f, 0.21f, 0.80f);
    colors[ImGuiCol_ScrollbarGrab]         = ImVec4(0.35f, 0.35f, 0.37f, 1.00f);
    colors[ImGuiCol_ScrollbarGrabHovered]  = accent_hover;
    colors[ImGuiCol_ScrollbarGrabActive]   = accent_active;
    colors[ImGuiCol_CheckMark]             = ImVec4(1.00f, 0.65f, 0.20f, 1.00f); // Brighter, more visible
    colors[ImGuiCol_SliderGrab]            = accent;
    colors[ImGuiCol_SliderGrabActive]      = accent_active;
    colors[ImGuiCol_Button]                = ImVec4(accent.x, accent.y, accent.z, 0.85f);
    colors[ImGuiCol_ButtonHovered]         = accent_hover;
    colors[ImGuiCol_ButtonActive]          = accent_active;
    colors[ImGuiCol_Header]                = ImVec4(accent.x, accent.y, accent.z, 0.55f);
    colors[ImGuiCol_HeaderHovered]         = ImVec4(accent_hover.x, accent_hover.y, accent_hover.z, 0.80f);
    colors[ImGuiCol_HeaderActive]          = accent_active;
    colors[ImGuiCol_Separator]             = ImVec4(0.43f, 0.43f, 0.50f, 0.50f);
    colors[ImGuiCol_SeparatorHovered]      = ImVec4(accent_hover.x, accent_hover.y, accent_hover.z, 0.78f);
    colors[ImGuiCol_SeparatorActive]       = accent_active;
    colors[ImGuiCol_ResizeGrip]            = ImVec4(accent.x, accent.y, accent.z, 0.25f);
    colors[ImGuiCol_ResizeGripHovered]     = ImVec4(accent_hover.x, accent_hover.y, accent_hover.z, 0.67f);
    colors[ImGuiCol_ResizeGripActive]      = accent_active;
    colors[ImGuiCol_Tab]                   = ImVec4(0.18f, 0.18f, 0.20f, 1.00f);
    colors[ImGuiCol_TabHovered]            = accent_hover;
    colors[ImGuiCol_TabActive]             = accent;
    colors[ImGuiCol_TabUnfocused]          = ImVec4(0.18f, 0.18f, 0.21f, 1.00f);
    colors[ImGuiCol_TabUnfocusedActive]    = ImVec4(accent_muted.x, accent_muted.y, accent_muted.z, 0.60f);
    colors[ImGuiCol_DockingPreview]        = ImVec4(accent.x, accent.y, accent.z, 0.50f);
    colors[ImGuiCol_DockingEmptyBg]        = ImVec4(0.12f, 0.12f, 0.13f, 1.00f);
    colors[ImGuiCol_PlotLines]             = ImVec4(0.90f, 0.70f, 0.40f, 1.00f);
    colors[ImGuiCol_PlotLinesHovered]      = accent_hover;
    colors[ImGuiCol_PlotHistogram]         = ImVec4(0.90f, 0.60f, 0.30f, 1.00f);
    colors[ImGuiCol_PlotHistogramHovered]  = ImVec4(1.00f, 0.70f, 0.35f, 1.00f);
    colors[ImGuiCol_TextSelectedBg]        = ImVec4(accent.x, accent.y, accent.z, 0.35f);
    colors[ImGuiCol_DragDropTarget]        = ImVec4(1.00f, 0.65f, 0.30f, 0.90f);
    colors[ImGuiCol_NavHighlight]          = accent;
    colors[ImGuiCol_NavWindowingHighlight] = ImVec4(1.00f, 1.00f, 1.00f, 0.70f);
    colors[ImGuiCol_NavWindowingDimBg]     = ImVec4(0.80f, 0.80f, 0.80f, 0.20f);
    colors[ImGuiCol_ModalWindowDimBg]      = ImVec4(0.10f, 0.10f, 0.10f, 0.65f);

    // Style adjustments for a premium look
    style.WindowPadding     = ImVec2(8, 8);
    style.WindowRounding    = 6.0f;
    style.FramePadding      = ImVec2(6, 4);
    style.FrameRounding     = 5.0f;
    style.ItemSpacing       = ImVec2(8, 6);
    style.ItemInnerSpacing  = ImVec2(6, 4);
    style.IndentSpacing     = 25.0f;
    style.ScrollbarSize     = 12.0f;
    style.ScrollbarRounding = 6.0f;
    style.GrabMinSize       = 10.0f;
    style.GrabRounding      = 4.0f;
    style.TabRounding       = 5.0f;
    style.WindowTitleAlign  = ImVec2(0.5f, 0.5f);
    style.WindowBorderSize  = 1.0f;
    style.FrameBorderSize   = 0.0f;
    style.PopupRounding     = 5.0f;
    style.Alpha             = 1.0f;

    // Store gradient colors in user data for later use
    if (!ImGui::GetIO().UserData) {
        ImGui::GetIO().UserData = IM_NEW(GradientData)();
    }
    GradientData* gradient_data = (GradientData*)ImGui::GetIO().UserData;
    gradient_data->window_grad_top = bg_light;
    gradient_data->window_grad_bottom = bg_dark;
}

// Create a gradient texture once
GLuint CreateGradientTexture(ImVec4 top_color, ImVec4 bottom_color, int height = 1280) {
    GLuint texture_id;
    glGenTextures(1, &texture_id);
    glBindTexture(GL_TEXTURE_2D, texture_id);

    const int width = 32;
    unsigned char* data = new unsigned char[width * height * 4];
    const float bayer8x8[64] = {
        0/64.0f,  32/64.0f,  8/64.0f, 40/64.0f,  2/64.0f, 34/64.0f, 10/64.0f, 42/64.0f,
        48/64.0f, 16/64.0f, 56/64.0f, 24/64.0f, 50/64.0f, 18/64.0f, 58/64.0f, 26/64.0f,
        12/64.0f, 44/64.0f,  4/64.0f, 36/64.0f, 14/64.0f, 46/64.0f,  6/64.0f, 38/64.0f,
        60/64.0f, 28/64.0f, 52/64.0f, 20/64.0f, 62/64.0f, 30/64.0f, 54/64.0f, 22/64.0f,
        3/64.0f,  35/64.0f, 11/64.0f, 43/64.0f,  1/64.0f, 33/64.0f,  9/64.0f, 41/64.0f,
        51/64.0f, 19/64.0f, 59/64.0f, 27/64.0f, 49/64.0f, 17/64.0f, 57/64.0f, 25/64.0f,
        15/64.0f, 47/64.0f,  7/64.0f, 39/64.0f, 13/64.0f, 45/64.0f,  5/64.0f, 37/64.0f,
        63/64.0f, 31/64.0f, 55/64.0f, 23/64.0f, 61/64.0f, 29/64.0f, 53/64.0f, 21/64.0f
    };
    const float dither_strength = 1.0f/255.0f * 1.5f;

    for (int y = 0; y < height; y++) {
        float t = (float)y / (float)(height - 1);
        float eased_t = t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f);
        ImVec4 col_mix;
        col_mix.x = powf(powf(bottom_color.x, 2.2f) * (1.0f - eased_t) + powf(top_color.x, 2.2f) * eased_t, 1.0f/2.2f);
        col_mix.y = powf(powf(bottom_color.y, 2.2f) * (1.0f - eased_t) + powf(top_color.y, 2.2f) * eased_t, 1.0f/2.2f);
        col_mix.z = powf(powf(bottom_color.z, 2.2f) * (1.0f - eased_t) + powf(top_color.z, 2.2f) * eased_t, 1.0f/2.2f);
        col_mix.w = bottom_color.w * (1.0f - eased_t) + top_color.w * eased_t;
        for (int x = 0; x < width; x++) {
            int pattern_x = x % 8;
            int pattern_y = y % 8;
            float dither_value = bayer8x8[pattern_y * 8 + pattern_x];
            float r = col_mix.x + (dither_value - 0.5f) * dither_strength;
            float g = col_mix.y + (dither_value - 0.5f) * dither_strength;
            float b = col_mix.z + (dither_value - 0.5f) * dither_strength;
            r = std::max(0.0f, std::min(1.0f, r));
            g = std::max(0.0f, std::min(1.0f, g));
            b = std::max(0.0f, std::min(1.0f, b));
            int idx = (y * width + x) * 4;
            data[idx + 0] = (unsigned char)(r * 255.0f);
            data[idx + 1] = (unsigned char)(g * 255.0f);
            data[idx + 2] = (unsigned char)(b * 255.0f);
            data[idx + 3] = (unsigned char)(col_mix.w * 255.0f);
        }
    }
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, data);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glGenerateMipmap(GL_TEXTURE_2D);
    delete[] data;
    glBindTexture(GL_TEXTURE_2D, 0);
    return texture_id;
}

void ApplyWindowBackgroundGradients() {
    static GLuint gradient_texture = 0;
    static ImVec4 last_top_color = ImVec4(-1,-1,-1,-1);
    static ImVec4 last_bottom_color = ImVec4(-1,-1,-1,-1);

    GradientData* gradient_data = (GradientData*)ImGui::GetIO().UserData;
    if (!gradient_data) return;

    if (gradient_texture == 0 ||
        memcmp(&last_top_color, &gradient_data->window_grad_top, sizeof(ImVec4)) != 0 ||
        memcmp(&last_bottom_color, &gradient_data->window_grad_bottom, sizeof(ImVec4)) != 0)
    {
        if (gradient_texture != 0) {
            glDeleteTextures(1, &gradient_texture);
        }
        gradient_texture = CreateGradientTexture(gradient_data->window_grad_top, gradient_data->window_grad_bottom);
        last_top_color = gradient_data->window_grad_top;
        last_bottom_color = gradient_data->window_grad_bottom;
        std::cout << "Recreated gradient texture." << std::endl;
    }

    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    if (!draw_list || gradient_texture == 0) return;

    ImVec2 window_pos = ImGui::GetWindowPos();
    ImVec2 window_size = ImGui::GetWindowSize();
    ImTextureID tex_id = (ImTextureID)(intptr_t)gradient_texture;
    draw_list->AddImage(tex_id, window_pos, ImVec2(window_pos.x + window_size.x, window_pos.y + window_size.y), ImVec2(0, 0), ImVec2(1, 1), IM_COL32_WHITE);
}

// --- Docking Setup ---
void RenderDockSpace() {
    static ImGuiDockNodeFlags dockspace_flags = ImGuiDockNodeFlags_PassthruCentralNode; // Allow background rendering

    ImGuiWindowFlags window_flags = ImGuiWindowFlags_MenuBar | ImGuiWindowFlags_NoDocking;
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    ImGui::SetNextWindowViewport(viewport->ID);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    window_flags |= ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove;
    window_flags |= ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus;

    if (dockspace_flags & ImGuiDockNodeFlags_PassthruCentralNode)
        window_flags |= ImGuiWindowFlags_NoBackground;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::Begin("DockSpace Demo", nullptr, window_flags);
    ImGui::PopStyleVar(); // Pop WindowPadding
    ImGui::PopStyleVar(2); // Pop Rounding, BorderSize

    ImGuiIO& io = ImGui::GetIO();
    if (io.ConfigFlags & ImGuiConfigFlags_DockingEnable) {
        ImGuiID dockspace_id = ImGui::GetID("MyDockSpace");
        ImGui::DockSpace(dockspace_id, ImVec2(0.0f, 0.0f), dockspace_flags);
    }

    if (ImGui::BeginMenuBar()) {
        if (ImGui::BeginMenu("File")) {
            if (ImGui::MenuItem("Load Project...")) { /* Trigger load */ }
            if (ImGui::MenuItem("Save Project...")) { /* Trigger save */ }
            if (ImGui::MenuItem("Export Video...")) { /* Trigger export */ }
            ImGui::Separator();
            if (ImGui::MenuItem("Exit")) {
                 SDL_Event quit_event; quit_event.type = SDL_EVENT_QUIT; SDL_PushEvent(&quit_event);
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Edit")) {
             ImGui::MenuItem("Undo", "CTRL+Z", nullptr, false);
             ImGui::MenuItem("Redo", "CTRL+Y", nullptr, false);
             ImGui::EndMenu();
        }
        ImGui::EndMenuBar();
    }
    ImGui::End();
}

// --- Clip Management ---
void AddNewClip(std::vector<Clip>& clips, const std::string& input_path, float video_duration, int layer, GLResources& res) {
    Clip temp_clip;
    temp_clip.path = input_path;
    temp_clip.type = ClipType::Video;
    load_resources_for_clip(res, temp_clip);

    clips.reserve(clips.size() + 2);
    std::string base_name = std::filesystem::path(input_path).filename().string();

    size_t video_index = clips.size();
    clips.emplace_back();
    Clip& video_clip = clips[video_index];
    video_clip.name = base_name + " [Video]";
    video_clip.path = input_path;
    video_clip.type = ClipType::Video;
    video_clip.start_time = playhead_time;
    video_clip.duration = video_duration;
    video_clip.layer = layer;
    video_clip.media_start = 0.0f;
    video_clip.is_audio_only = false;
    video_clip.opacity = 1.0f;
    video_clip.scale = 1.0f;
    video_clip.pos_x = 0.0f;
    video_clip.pos_y = 0.0f;
    video_clip.selected = false;

    if(show_thumbs) QueueClipThumbnails(res, video_clip);

    auto audio_it = res.preloaded_audio.find(input_path);
    if (audio_it != res.preloaded_audio.end() && !audio_it->second.samples.empty()) {
        size_t audio_index = clips.size();
        clips.emplace_back();
        Clip& audio_clip = clips[audio_index];
        audio_clip.name = base_name + " [Audio]";
        audio_clip.path = input_path;
        audio_clip.type = ClipType::Audio;
        audio_clip.start_time = video_clip.start_time;
        audio_clip.duration = audio_it->second.duration;
        audio_clip.layer = layer;
        audio_clip.media_start = 0.0f;
        audio_clip.waveform = audio_it->second.waveform;
        audio_clip.is_audio_only = false;
        audio_clip.opacity = 1.0f;
        audio_clip.scale = 1.0f;
        audio_clip.pos_x = 0.0f;
        audio_clip.pos_y = 0.0f;
        audio_clip.selected = false;

        audio_clip.linked_clip = &clips[video_index];
        video_clip.linked_clip = &clips[audio_index];
        video_clip.has_audio = true;
        std::cout << "Added linked Video and Audio clips for: " << input_path << std::endl;
    } else {
        video_clip.has_audio = false;
        video_clip.linked_clip = nullptr;
        if (audio_it != res.preloaded_audio.end() && audio_it->second.samples.empty()) {
            std::cout << "Added Video clip (Audio stream found but empty/failed preload) for: " << input_path << std::endl;
        } else {
             std::cout << "Added Video clip (No audio stream found/preloaded) for: " << input_path << std::endl;
        }
    }
}

// --- Video Duration Helper ---
float get_video_duration(const std::string& input_path) {
    AVFormatContext* fmt_ctx = nullptr;
    float duration = -1.0f;
    AVDictionary* fmt_opts = nullptr;

    if (avformat_open_input(&fmt_ctx, input_path.c_str(), nullptr, &fmt_opts) != 0) {
        std::cerr << "ERROR: Could not open input file " << input_path << std::endl;
        av_dict_free(&fmt_opts);
        return -1.0f;
    }
    av_dict_free(&fmt_opts);

    if (avformat_find_stream_info(fmt_ctx, nullptr) < 0) {
        std::cerr << "ERROR: Could not find stream information for " << input_path << std::endl;
        avformat_close_input(&fmt_ctx);
        return -1.0f;
    }

    int video_stream_idx = av_find_best_stream(fmt_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (video_stream_idx >= 0) {
        AVStream* stream = fmt_ctx->streams[video_stream_idx];
        if (stream->duration != AV_NOPTS_VALUE && stream->time_base.den != 0) {
            duration = static_cast<float>(stream->duration) * av_q2d(stream->time_base);
            if (duration <= 0) duration = -1.0f;
        }
    }

    if (duration < 0 && fmt_ctx->duration != AV_NOPTS_VALUE) {
        duration = static_cast<float>(fmt_ctx->duration) / AV_TIME_BASE;
    }

    avformat_close_input(&fmt_ctx);

    if (duration < 0) {
        std::cerr << "Warning: Could not determine duration reliably for " << input_path << ". Returning 0." << std::endl;
        return 0.0f;
    }
    return duration;
}

// --- Forward Declarations ---
template<typename T>
void DrawKeyframeTrackEditor(const std::string& label, KeyframeTrack<T>& track);
void DrawTimelineEditor(std::vector<Clip>& clips, float& playhead_time, float& max_duration, float& zoom_factor, std::atomic<bool>& layers_changed, Clip*& selected_clip, GLResources& res);
void UpdatePreview(GLResources& res, const std::vector<Clip>& sorted_clips, int width, int height, float playhead_time, bool force_update);
void RenderPreviewWindow(GLuint preview_tex, int preview_width, int preview_height);

// --- Main Application ---
int main(int argc, char* argv[]) {
    avformat_network_init();

    SDL_Init(SDL_INIT_VIDEO);

    SDL_Window* window = SDL_CreateWindow(
        "Zest",
        640, 480,
        SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_MAXIMIZED
    );

    if (!window) {
        SDL_LogError(SDL_LOG_CATEGORY_ERROR, "Could not create window: %s\n", SDL_GetError());
        return 1;
    }

    SDL_GLContext gl_context = SDL_GL_CreateContext(window);
    if (!gl_context) {
        SDL_LogError(SDL_LOG_CATEGORY_ERROR, "Failed to create OpenGL context: %s\n", SDL_GetError());
        return 1;
    }

    if (!gladLoadGLLoader((GLADloadproc)SDL_GL_GetProcAddress)) {
        SDL_LogError(SDL_LOG_CATEGORY_ERROR, "Failed to initialize GLAD\n");
        return 1;
    }

    SDL_Surface* icon_surface = IMG_Load("assets/logo.png");
    if (icon_surface) {
        SDL_SetWindowIcon(window, icon_surface);
        SDL_DestroySurface(icon_surface);
    }

    // === ImGui Setup ===
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO(); (void)io;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;

    ImGui::StyleColorsDark();
    io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeui.ttf", 18.0f);

    ImGui_ImplSDL3_InitForOpenGL(window, gl_context);
    ImGui_ImplOpenGL3_Init("#version 130"); // Or "#version 330 core" for modern GL

    SetupImGuiStyle();
    std::string font_path = "C:\\Windows\\Fonts\\segoeui.ttf";
    if (std::filesystem::exists(font_path)) {
        io.Fonts->AddFontFromFileTTF(font_path.c_str(), 18.0f);
    } else {
        std::cerr << "Warning: Font not found at " << font_path << ". Using default ImGui font." << std::endl;
    }

    char input_path[FILENAME_MAX] = "";
    char output_path[FILENAME_MAX] = "output.mp4";
    float max_duration = 10.0f;
    float zoom_factor = 1.0f;
    std::atomic<bool> layers_changed = true;
    bool file_dropped_this_frame = false;
    std::string process_message = "Ready.";
    std::vector<Clip> clips;
    float last_preview_update_time = -1.0f;
    const float PREVIEW_UPDATE_INTERVAL = 1.0f / 60.0f;

    GLResources gl_resources;
    if (!setup_gl_resources(gl_resources, preview_width, preview_height)) {
        std::cerr << "Failed to initialize initial GL resources!" << std::endl;
        ImGui_ImplOpenGL3_Shutdown(); ImGui_ImplSDL3_Shutdown(); ImGui::DestroyContext();
        // Use SDL_GL_DestroyContext (Fix 1)
        SDL_GL_DestroyContext(gl_context);
        SDL_DestroyWindow(window); SDL_Quit();
        return 1;
    }

    if(show_thumbs) start_thumbnail_worker();

    bool running = true;
    last_frame_ticks = SDL_GetTicks();

    while (running) {
        Uint64 current_ticks = SDL_GetTicks();
        float delta_time = (current_ticks - last_frame_ticks) / 1000.0f;
        delta_time = std::min(delta_time, 0.1f);
        last_frame_ticks = current_ticks;

        file_dropped_this_frame = false;
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            ImGui_ImplSDL3_ProcessEvent(&event);

            if (event.type == SDL_EVENT_QUIT) running = false;

            if (event.type == SDL_EVENT_WINDOW_RESIZED || event.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED) {
                 int w, h; SDL_GetWindowSizeInPixels(window, &w, &h); glViewport(0, 0, w, h);
            }

            if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_SPACE && event.key.down) {
                playing = !playing.load();
                std::cout << "Playback " << (playing ? "started" : "paused") << "\n";
            }
            // Use event.key.key and SDLK_B (Fix 3 & 4)
            else if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_B && (SDL_GetModState() & SDL_KMOD_CTRL) && selected_clip && event.key.down) {
                // --- Blade Tool Logic ---
                float cut_time = playhead_time;
                float clip_start_global = selected_clip->start_time;
                float clip_end_global = selected_clip->start_time + selected_clip->duration;
                if (cut_time > clip_start_global && cut_time < clip_end_global) {
                    Clip* linked_clip_ptr = selected_clip->linked_clip;
                    float relative_cut = cut_time - clip_start_global;
                    float new_media_start = selected_clip->media_start + relative_cut;
                    Clip new_clip = *selected_clip;
                    new_clip.start_time = cut_time;
                    new_clip.media_start = new_media_start;
                    new_clip.duration = clip_end_global - cut_time;
                    new_clip.name += " (Split)";
                    new_clip.selected = false;
                    new_clip.linked_clip = nullptr;
                    selected_clip->duration = relative_cut;
                    selected_clip->linked_clip = nullptr;
                    Clip* new_linked_clip_ptr = nullptr;
                    if (linked_clip_ptr) {
                        bool linked_found = false;
                        for (const auto& c : clips) { if (&c == linked_clip_ptr) { linked_found = true; break; } }
                        if (linked_found) {
                            Clip new_linked_clip = *linked_clip_ptr;
                            new_linked_clip.start_time = cut_time;
                            new_linked_clip.media_start = linked_clip_ptr->media_start + relative_cut;
                            new_linked_clip.duration = new_clip.duration;
                            new_linked_clip.name += " (Split)";
                            new_linked_clip.selected = false;
                            new_linked_clip.linked_clip = nullptr;
                            linked_clip_ptr->duration = selected_clip->duration;
                            linked_clip_ptr->linked_clip = nullptr;
                            clips.push_back(new_linked_clip);
                            new_linked_clip_ptr = &clips.back();
                        } else {
                            std::cerr << "Warning: Linked clip pointer was invalid during split." << std::endl;
                            linked_clip_ptr = nullptr;
                        }
                    }
                    clips.push_back(new_clip);
                    Clip* new_clip_ptr = &clips.back();
                    if (linked_clip_ptr) {
                        selected_clip->linked_clip = linked_clip_ptr;
                        linked_clip_ptr->linked_clip = selected_clip;
                        if (new_linked_clip_ptr) {
                            new_clip_ptr->linked_clip = new_linked_clip_ptr;
                            new_linked_clip_ptr->linked_clip = new_clip_ptr;
                        }
                    }
                    layers_changed = true;
                    selected_clip = new_clip_ptr;
                    std::cout << "Split clip at " << cut_time << "s\n";
                } else {
                    std::cout << "Split position (" << cut_time << ") is not within the selected clip bounds (" << clip_start_global << " - " << clip_end_global << ")\n";
                }
                // --- End Blade Tool ---
            }
            // Use event.key.key and SDLK_DELETE (Fix 3 & 4)
            else if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_DELETE && selected_clip && event.key.down) {
                ptrdiff_t selected_index = -1;
                for(size_t i = 0; i < clips.size(); ++i) { if (&clips[i] == selected_clip) { selected_index = i; break; } }
                if (selected_index != -1) {
                        Clip& clip_to_delete = clips[selected_index];
                    if (clip_to_delete.linked_clip) clip_to_delete.linked_clip->linked_clip = nullptr;
                        clips.erase(clips.begin() + selected_index);
                        selected_clip = nullptr;
                        layers_changed = true;
                        process_message = "Deleted clip.";
                        std::cout << "Deleted selected clip." << std::endl;
                }
            }

            if (event.type == SDL_EVENT_DROP_FILE) {
                if (event.drop.data) {
                    strncpy(input_path, event.drop.data, sizeof(input_path) - 1);
                    input_path[sizeof(input_path) - 1] = '\0';
                    file_dropped_this_frame = true;
                }
            }
        } // End SDL_PollEvent loop

        if (file_dropped_this_frame) {
            std::string dropped_path_str = input_path;
            std::cout << "File dropped: " << dropped_path_str << std::endl;
            if (std::filesystem::exists(dropped_path_str)) {
                float duration = get_video_duration(dropped_path_str);
                if (duration >= 0) {
                    AddNewClip(clips, dropped_path_str, duration, 0, gl_resources);
                    layers_changed = true;
                    process_message = "Added clip: " + std::filesystem::path(dropped_path_str).filename().string();
                } else {
                    process_message = "Failed to get duration for: " + std::filesystem::path(dropped_path_str).filename().string();
                    std::cerr << process_message << std::endl;
                }
            } else {
                process_message = "Dropped file path does not exist: " + dropped_path_str;
                std::cerr << process_message << std::endl;
            }
            input_path[0] = '\0';
        }

        if (playing) {
            playhead_time += delta_time;
            if (playhead_time > max_duration) { playhead_time = max_duration; playing = false; }
            playhead_time = std::max(0.0f, playhead_time);
        }

        std::vector<Clip> active_clips_for_preview;
        float preview_time_window = 1.0f;
        for (const auto& clip : clips) {
             // Need is_video_file (Fix 5 - requires declaration in hpp)
            if (clip.type == ClipType::Video && is_video_file(clip.path)) {
                 float clip_start = clip.start_time; float clip_end = clip.start_time + clip.duration;
                if (std::max(clip_start, playhead_time - preview_time_window) < std::min(clip_end, playhead_time + preview_time_window)) {
                    active_clips_for_preview.push_back(clip);
                }
            }
        }
        update_video_previews(gl_resources, active_clips_for_preview, playhead_time);
        if(show_thumbs) ProcessThumbnailResults(gl_resources, 2);

        ImGui_ImplOpenGL3_NewFrame(); ImGui_ImplSDL3_NewFrame(); ImGui::NewFrame();
        RenderDockSpace();

        ImGui::Begin("Controls");
        ApplyWindowBackgroundGradients();
        ImGui::InputText("Output Path", output_path, sizeof(output_path)); ImGui::Separator();
        if (ImGui::Button(playing ? "Pause (Space)" : "Play (Space)")) playing = !playing.load();
        ImGui::SameLine(); ImGui::Text("Time: %.2f / %.2f", playhead_time, max_duration);
        if (ImGui::SliderFloat("##Seek", &playhead_time, 0.0f, max_duration, "%.2f s")) {
             layers_changed = true; playing = false;
        }
        ImGui::Separator();
        if (ImGui::Button("Export Video")) {
            SDL_GL_MakeCurrent(window, gl_context);
            std::filesystem::path out_p(output_path);
            if (!out_p.has_filename()) { process_message = "Output path is not a valid filename!"; }
            else {
                 int export_fps = 30;
                 int export_duration_frames = static_cast<int>(std::ceil(max_duration * export_fps));
                 if (export_duration_frames <= 0) { process_message = "Cannot export empty timeline!"; }
                 else {
                    process_message = "Exporting...";
                    bool success = start_video_export(output_path, preview_width, preview_height, export_fps, export_duration_frames, clips, window);
                    process_message = success ? "Export finished successfully!" : "Export failed!";
                }
            }
        }
        ImGui::Text("Status: %s", process_message.c_str()); ImGui::Separator();
        if (ImGui::Button("Save Project")) {
            const char* filters[] = { "*.zest" };
            const char* save_path = tinyfd_saveFileDialog("Save Project", "project.zest", 1, filters, "Zest Project Files (*.zest)");
            if (save_path) {
                 if (SaveProject(save_path, clips, playhead_time, zoom_factor)) {
                    process_message = "Project saved to " + std::string(save_path); std::cout << process_message << std::endl;
                } else { process_message = "Failed to save project!"; std::cerr << process_message << std::endl; }
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Load Project")) {
            const char* filters[] = { "*.zest" };
            const char* load_path = tinyfd_openFileDialog("Load Project", "", 1, filters, "Zest Project Files (*.zest)", 0);
            if (load_path) {
                std::vector<Clip> loaded_clips; float loaded_playhead = 0; float loaded_zoom = 1.0f;
                // In the project loading block:
                if (LoadProject(load_path, loaded_clips, loaded_playhead, loaded_zoom)) {
                    playing = false;
                    clips.clear();
                    selected_clip = nullptr;
                    cleanup_gl_resources(gl_resources);
                    cleanup_video_resources(gl_resources);
                    gl_resources.preloaded_audio.clear();
                    setup_gl_resources(gl_resources, preview_width, preview_height);
                    
                    clips = std::move(loaded_clips);
                    playhead_time = loaded_playhead;
                    zoom_factor = loaded_zoom;
                    
                    // Reload audio waveforms for all clips
                    for (auto& clip : clips) {
                        if (clip.type == ClipType::Audio) {
                            // Force reload audio data
                            load_resources_for_clip(gl_resources, clip);
                            if(show_thumbs) QueueClipThumbnails(gl_resources, clip);
                            // Link waveform pointer after reload
                            auto audio_it = gl_resources.preloaded_audio.find(clip.path);
                            if (audio_it != gl_resources.preloaded_audio.end()) {
                                clip.waveform = audio_it->second.waveform;
                            }
                        }
                    }
                    
                    layers_changed = true;
                    process_message = "Project loaded from " + std::string(load_path);
                } else { process_message = "Failed to load project!"; std::cerr << process_message << std::endl; }
            }
        }
        ImGui::End(); // End Controls

        DrawTimelineEditor(clips, playhead_time, max_duration, zoom_factor, layers_changed, selected_clip, gl_resources);

        bool force_preview_update = layers_changed.load();
        if (force_preview_update || std::abs(playhead_time - last_preview_update_time) > PREVIEW_UPDATE_INTERVAL) {
            std::vector<Clip> sorted_clips = clips;
            std::sort(sorted_clips.begin(), sorted_clips.end(), [](const Clip& a, const Clip& b) { return a.layer < b.layer; });
            UpdatePreview(gl_resources, sorted_clips, preview_width, preview_height, playhead_time, force_preview_update);
            last_preview_update_time = playhead_time;
            layers_changed = false;
        }
        RenderPreviewWindow(gl_resources.render_tex, preview_width, preview_height);

        ImGui::Begin("Inspector"); ApplyWindowBackgroundGradients();
        if (selected_clip) {
            ImGui::Text("Selected: %s", selected_clip->name.c_str()); ImGui::Text("Path: %s", selected_clip->path.c_str());
            bool changed = false;
            ImGui::SeparatorText("Transform");
            changed |= ImGui::SliderFloat("Pos X", &selected_clip->pos_x, -1.0f, 1.0f, "%.3f");
            changed |= ImGui::SliderFloat("Pos Y", &selected_clip->pos_y, -1.0f, 1.0f, "%.3f");
            changed |= ImGui::SliderFloat("Scale", &selected_clip->scale, 0.0f, 10.0f, "%.3f");
            changed |= ImGui::SliderFloat("Rotation", &selected_clip->rotation, 0.0f, 360.0f, "%.3f");
            changed |= ImGui::SliderFloat("Opacity", &selected_clip->opacity, 0.0f, 1.0f, "%.3f");
            ImGui::SeparatorText("Timing & Trimming");
            changed |= ImGui::InputFloat("Start Time", &selected_clip->start_time, 0.1f, 1.0f, "%.2f"); selected_clip->start_time = std::max(0.0f, selected_clip->start_time);
            changed |= ImGui::InputFloat("Media Start", &selected_clip->media_start, 0.1f, 1.0f, "%.2f"); selected_clip->media_start = std::max(0.0f, selected_clip->media_start);
            changed |= ImGui::InputFloat("Duration", &selected_clip->duration, 0.1f, 1.0f, "%.2f"); selected_clip->duration = std::max(0.01f, selected_clip->duration);
            ImGui::SeparatorText("Layering");
            changed |= ImGui::InputInt("Layer", &selected_clip->layer); selected_clip->layer = std::max(0, selected_clip->layer);
            const char* blend_modes[] = { "Normal", "Additive", "Multiply", "Screen", "Darken", "Lighten", "Difference", "Subtract", "Divide", "Overlay"};
            int current_mode = static_cast<int>(selected_clip->blend_mode);

            if (ImGui::Combo("Blend Mode", &current_mode, blend_modes, IM_ARRAYSIZE(blend_modes))) {
                selected_clip->blend_mode = static_cast<BlendMode>(current_mode);
                changed = true;
            }
            ImGui::SeparatorText("Keying");
            DrawKeyframeTrackEditor("Opacity Keyframes", selected_clip->opacity_track);
            DrawKeyframeTrackEditor("Position X Keyframes", selected_clip->pos_x_track);
            DrawKeyframeTrackEditor("Position Y Keyframes", selected_clip->pos_y_track);
            DrawKeyframeTrackEditor("Rotation Keyframes", selected_clip->rotation_track);
            DrawKeyframeTrackEditor("Scale Keyframes", selected_clip->scale_track);
            if (changed) {
                layers_changed = true;
                if (selected_clip->linked_clip) {
                    if (selected_clip->type == ClipType::Video) {
                        selected_clip->linked_clip->start_time = selected_clip->start_time;
                        selected_clip->linked_clip->duration = selected_clip->duration;
                    }
                }
            }
        } else ImGui::Text("No clip selected.");
        ImGui::End(); // End Inspector

        ImGui::Begin("Active Clips"); ApplyWindowBackgroundGradients();
        ImGui::Text("Clip List (%zu clips):", clips.size()); ImGui::Separator();
        ImGuiListClipper clipper; clipper.Begin(clips.size());
        while (clipper.Step()) {
            for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
                 Clip& clip = clips[i]; bool is_selected = (&clip == selected_clip); ImGui::PushID(i);
                 if (is_selected) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.7f, 0.3f, 1.0f));
                 if (ImGui::Selectable(clip.name.c_str(), is_selected, ImGuiSelectableFlags_AllowItemOverlap)) selected_clip = &clip;
                 if (is_selected) ImGui::PopStyleColor();
                 ImGui::SameLine(300); ImGui::TextDisabled("T:%.2f D:%.2f L:%d MStart:%.2f %s", clip.start_time, clip.duration, clip.layer, clip.media_start, clip.linked_clip ? "[L]" : "");
                 ImGui::SameLine(ImGui::GetWindowWidth() - 50);
                 if (ImGui::SmallButton("Del")) {
                     if (clip.linked_clip) clip.linked_clip->linked_clip = nullptr;
                     clips.erase(clips.begin() + i);
                     if (&clip == selected_clip) selected_clip = nullptr;
                     layers_changed = true; ImGui::PopID(); clipper.End(); goto end_debug_loop_main;
                 }
                 ImGui::PopID();
            }
        } end_debug_loop_main:;
        ImGui::End(); // End Active Clips Debug

        ImGui::Render();

        int display_w, display_h; SDL_GetWindowSizeInPixels(window, &display_w, &display_h); glViewport(0, 0, display_w, display_h);
        ImVec4 bg_col = ImGui::GetStyle().Colors[ImGuiCol_WindowBg]; glClearColor(bg_col.x * bg_col.w, bg_col.y * bg_col.w, bg_col.z * bg_col.w, bg_col.w); glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable) {
            SDL_Window* backup_current_window = SDL_GL_GetCurrentWindow(); SDL_GLContext backup_context = SDL_GL_GetCurrentContext();
            ImGui::UpdatePlatformWindows(); ImGui::RenderPlatformWindowsDefault();
            if (SDL_GL_GetCurrentContext() != backup_context) SDL_GL_MakeCurrent(backup_current_window, backup_context);
        }
        SDL_GL_SwapWindow(window);

    } // End main loop

    std::cout << "Cleaning up resources..." << std::endl;

    stop_thumbnail_worker();

    if (ImGui::GetIO().UserData) { IM_DELETE((GradientData*)ImGui::GetIO().UserData); ImGui::GetIO().UserData = nullptr; }
    cleanup_gl_resources(gl_resources); cleanup_video_resources(gl_resources);
    ImGui_ImplOpenGL3_Shutdown(); ImGui_ImplSDL3_Shutdown(); ImGui::DestroyContext();
    // Use SDL_GL_DestroyContext (Fix 1)
    SDL_GL_DestroyContext(gl_context);
    SDL_DestroyWindow(window); SDL_Quit(); avformat_network_deinit();
    std::cout << "Exiting application." << std::endl;
    return 0;
}


// --- Updated UpdatePreview Function ---
void UpdatePreview(GLResources& res, const std::vector<Clip>& sorted_clips, int width, int height, float playhead_time, bool force_update) {
    render_frame(res, playhead_time, sorted_clips, width, height);
}

// --- RenderPreviewWindow ---
void RenderPreviewWindow(GLuint preview_tex, int preview_width, int preview_height) {
    ImGui::Begin("Video Preview"); ApplyWindowBackgroundGradients();
    ImVec2 available_size = ImGui::GetContentRegionAvail();
    available_size.x = std::max(available_size.x, 1.0f); available_size.y = std::max(available_size.y, 1.0f);
    ImVec2 render_size = ImVec2((float)preview_width, (float)preview_height);
    if (render_size.x <= 0 || render_size.y <= 0) { ImGui::Text("Invalid preview dimensions."); ImGui::End(); return; }
    float aspect_ratio = render_size.x / render_size.y;
    ImVec2 display_size = available_size;
    if (available_size.x / aspect_ratio <= available_size.y) display_size.y = available_size.x / aspect_ratio;
    else display_size.x = available_size.y * aspect_ratio;
    display_size.x = std::max(display_size.x, 1.0f); display_size.y = std::max(display_size.y, 1.0f);
    ImVec2 cursor_pos = ImGui::GetCursorPos();
    ImVec2 centered_pos = ImVec2(cursor_pos.x + (available_size.x - display_size.x) * 0.5f, cursor_pos.y + (available_size.y - display_size.y) * 0.5f);
    ImGui::SetCursorPos(centered_pos);
    if (preview_tex != 0) ImGui::Image((ImTextureID)(intptr_t)preview_tex, display_size, ImVec2(0, 0), ImVec2(1, 1));
    else {
        ImGui::Dummy(display_size); ImU32 placeholder_col = IM_COL32(40, 40, 45, 255);
        ImVec2 rect_min = ImGui::GetItemRectMin(); ImVec2 rect_max = ImGui::GetItemRectMax();
        ImGui::GetWindowDrawList()->AddRectFilled(rect_min, rect_max, placeholder_col);
        ImGui::GetWindowDrawList()->AddText(rect_min, IM_COL32(200, 200, 200, 255), "Preview Unavailable");
    }
    ImGui::End();
}

// --- DrawTimelineEditor ---
void DrawTimelineEditor(
    std::vector<Clip>& clips,
    float& playhead_time,
    float& max_duration,
    float& zoom_factor,
    std::atomic<bool>& layers_changed,
    Clip*& selected_clip,
    GLResources& res
) {
    ImGui::Begin("Timeline Editor");
    ApplyWindowBackgroundGradients();

    // Constants
    const float label_width = 120.0f; // Wider area for labels
    const float timeline_width = ImGui::GetContentRegionAvail().x - label_width;
    const float layer_height = 60.0f;
    const float layer_padding = 10.0f;

    // Styling constants from theme
    ImVec4 bg_dark = ImVec4(0.13f, 0.12f, 0.15f, 1.00f);
    ImVec4 bg_light = ImVec4(0.17f, 0.16f, 0.19f, 1.00f);
    ImVec4 accent = ImVec4(1.00f, 0.56f, 0.15f, 1.00f);
    ImVec4 accent_hover = ImVec4(1.00f, 0.67f, 0.25f, 1.00f);
    ImVec4 accent_active = ImVec4(1.00f, 0.42f, 0.00f, 1.00f);
    ImVec4 accent_muted = ImVec4(0.85f, 0.48f, 0.12f, 1.00f);
    ImVec4 text = ImVec4(0.98f, 0.98f, 0.98f, 1.00f);
    ImVec4 text_secondary = ImVec4(0.80f, 0.80f, 0.80f, 1.00f);
    ImVec4 frame_bg = ImVec4(0.16f, 0.16f, 0.18f, 1.00f);

    // Convert to IM_COL32 for draw list use
    ImU32 col_bg_dark = ImGui::ColorConvertFloat4ToU32(bg_dark);
    ImU32 col_bg_light = ImGui::ColorConvertFloat4ToU32(bg_light);
    ImU32 col_accent = ImGui::ColorConvertFloat4ToU32(accent);
    ImU32 col_accent_hover = ImGui::ColorConvertFloat4ToU32(accent_hover);
    ImU32 col_accent_active = ImGui::ColorConvertFloat4ToU32(accent_active);
    ImU32 col_accent_muted = ImGui::ColorConvertFloat4ToU32(accent_muted);
    ImU32 col_text = ImGui::ColorConvertFloat4ToU32(text);
    ImU32 col_text_secondary = ImGui::ColorConvertFloat4ToU32(text_secondary);
    ImU32 col_frame_bg = ImGui::ColorConvertFloat4ToU32(frame_bg);


    // Determine the number of layers
    int max_video_layer = 0;
    int max_audio_layer = 0;

    for (const auto& clip : clips) {
        if (clip.type == ClipType::Video)
            max_video_layer = std::max(max_video_layer, clip.layer);
        else if (clip.type == ClipType::Audio)
            max_audio_layer = std::max(max_audio_layer, clip.layer);
    }
    max_video_layer += 1;
    max_audio_layer += 1;

    float timeline_height = (max_video_layer + max_audio_layer) * (layer_height + layer_padding);

    // === Compute true visible project length
    float project_duration = 0.0f;
    for (const auto& clip : clips) {
        float clip_end = clip.start_time + clip.duration;
        if (clip_end > project_duration)
            project_duration = clip_end;
    }

    max_duration = project_duration;

    float pixels_per_second = 100.0f * zoom_factor;
    float timeline_size = project_duration * pixels_per_second;

    // Zoom buttons
    float focus_ratio = playhead_time / max_duration;

    if (ImGui::Button("Zoom In")) {
        zoom_factor *= 1.1f;
        playhead_time = std::clamp(focus_ratio * project_duration, 0.0f, project_duration);
    }
    ImGui::SameLine();
    if (ImGui::Button("Zoom Out")) {
        zoom_factor /= 1.1f;
        playhead_time = std::clamp(focus_ratio * project_duration, 0.0f, project_duration);
    }

    // Get starting positions for labels and timeline
    ImVec2 cursor_pos = ImGui::GetCursorScreenPos();
    ImVec2 labels_start = cursor_pos;
    ImVec2 timeline_start = ImVec2(labels_start.x + label_width, labels_start.y);
    
    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    
    // Draw labels background with gradient (top to bottom)
    ImVec2 labels_end = ImVec2(labels_start.x + label_width, labels_start.y + timeline_height);
    draw_list->AddRectFilledMultiColor(
        labels_start, 
        labels_end,
        ImGui::ColorConvertFloat4ToU32(ImVec4(0.18f, 0.17f, 0.20f, 1.00f)), // Top color
        ImGui::ColorConvertFloat4ToU32(ImVec4(0.18f, 0.17f, 0.20f, 1.00f)), // Top right
        ImGui::ColorConvertFloat4ToU32(ImVec4(0.15f, 0.14f, 0.17f, 1.00f)), // Bottom right
        ImGui::ColorConvertFloat4ToU32(ImVec4(0.15f, 0.14f, 0.17f, 1.00f))  // Bottom left
    );
    
    // Draw a separator line between labels and timeline
    draw_list->AddLine(
        ImVec2(labels_start.x + label_width, labels_start.y),
        ImVec2(labels_start.x + label_width, labels_start.y + timeline_height),
        ImGui::ColorConvertFloat4ToU32(ImVec4(0.40f, 0.40f, 0.42f, 0.80f)),
        2.0f
    );

    // Draw timeline background with subtle gradient
    ImVec2 timeline_end = ImVec2(timeline_start.x + timeline_width, timeline_start.y + timeline_height);
    draw_list->AddRectFilledMultiColor(
        timeline_start, 
        timeline_end,
        ImGui::ColorConvertFloat4ToU32(ImVec4(0.16f, 0.16f, 0.18f, 1.00f)), // Top color
        ImGui::ColorConvertFloat4ToU32(ImVec4(0.16f, 0.16f, 0.18f, 1.00f)), // Top right
        ImGui::ColorConvertFloat4ToU32(ImVec4(0.14f, 0.14f, 0.16f, 1.00f)), // Bottom right
        ImGui::ColorConvertFloat4ToU32(ImVec4(0.14f, 0.14f, 0.16f, 1.00f))  // Bottom left
    );

    // Draw the labels in their own area
    // Video track labels
    for (int i = 0; i < max_video_layer; ++i) {
        float y = labels_start.y + (max_video_layer - i - 1) * (layer_height + layer_padding);
        
        // Better styling for labels
        ImVec2 label_rect_min = ImVec2(labels_start.x, y);
        ImVec2 label_rect_max = ImVec2(labels_start.x + label_width - 2, y + layer_height);
        
         // Gradient background for label using theme colors
         draw_list->AddRectFilledMultiColor(
            label_rect_min,
            label_rect_max,
            ImGui::ColorConvertFloat4ToU32(ImVec4(0.60f, 0.30f, 0.20f, 1.00f)), // Top
            ImGui::ColorConvertFloat4ToU32(ImVec4(0.45f, 0.22f, 0.15f, 1.00f)), // Top right
            ImGui::ColorConvertFloat4ToU32(ImVec4(0.17f, 0.16f, 0.19f, 1.00f)), // Bottom right
            ImGui::ColorConvertFloat4ToU32(ImVec4(0.17f, 0.16f, 0.19f, 1.00f))  // Bottom left
        );
        
        // Add subtle accent color border
        draw_list->AddRect(
            label_rect_min,
            label_rect_max,
            ImGui::ColorConvertFloat4ToU32(ImVec4(accent.x, accent.y, accent.z, 0.3f)),
            4.0f, // Rounded corners
            0,
            1.0f  // Line width
        );
        
        // Label text with better positioning and styling
        std::string label = "Video " + std::to_string(i + 1);
        ImVec2 text_size = ImGui::CalcTextSize(label.c_str());
        draw_list->AddText(
            ImVec2(labels_start.x + (label_width - text_size.x) / 2, y + (layer_height - text_size.y) / 2),
            col_text,
            label.c_str()
        );
    }
    
    // Audio track labels
    for (int i = 0; i < max_audio_layer; ++i) {
        float y = labels_start.y + (max_video_layer + i) * (layer_height + layer_padding);
        
        // Better styling for labels
        ImVec2 label_rect_min = ImVec2(labels_start.x, y);
        ImVec2 label_rect_max = ImVec2(labels_start.x + label_width - 2, y + layer_height);
        
        // Gradient background for label with slightly different hue
        draw_list->AddRectFilledMultiColor(
            label_rect_min,
            label_rect_max,
            ImGui::ColorConvertFloat4ToU32(ImVec4(0.19f, 0.18f, 0.22f, 1.00f)), // Top
            ImGui::ColorConvertFloat4ToU32(ImVec4(0.19f, 0.18f, 0.22f, 1.00f)), // Top right
            ImGui::ColorConvertFloat4ToU32(ImVec4(0.25f, 0.30f, 0.45f, 1.00f)), // Bottom right
            ImGui::ColorConvertFloat4ToU32(ImVec4(0.18f, 0.22f, 0.35f, 1.00f))  // Bottom left
        );
        
        // Add subtle accent color border
        draw_list->AddRect(
            label_rect_min,
            label_rect_max,
            ImGui::ColorConvertFloat4ToU32(ImVec4(accent.x, accent.y, accent.z, 0.3f)),
            4.0f, // Rounded corners
            0,
            1.0f  // Line width
        );
        
        // Label text with better positioning and styling
        std::string label = "Audio " + std::to_string(i + 1);
        ImVec2 text_size = ImGui::CalcTextSize(label.c_str());
        draw_list->AddText(
            ImVec2(labels_start.x + (label_width - text_size.x) / 2, y + (layer_height - text_size.y) / 2),
            col_text,
            label.c_str()
        );
    }

    // Layer lines with subtle styling
    for (int layer = 0; layer <= (max_video_layer + max_audio_layer); ++layer) {
        float y = timeline_start.y + layer * (layer_height + layer_padding);
        // Subtle grid lines
        draw_list->AddLine(
            ImVec2(timeline_start.x, y),
            ImVec2(timeline_end.x, y),
            ImGui::ColorConvertFloat4ToU32(ImVec4(1.0f, 1.0f, 1.0f, 0.12f))
        );
    }

    // === Draw Ruler Overlay ===
    const float tick_major_height = timeline_height;
    const float tick_minor_height = timeline_height;
    const float label_y = timeline_start.y - 18.0f; // above timeline
    const float tick_alpha_major = 90;
    const float tick_alpha_minor = 40;
    const float label_alpha = 160;

    draw_list->AddLine(
        ImVec2(timeline_start.x, timeline_start.y),
        ImVec2(timeline_end.x, timeline_start.y),
        ImGui::ColorConvertFloat4ToU32(ImVec4(accent.x, accent.y, accent.z, 0.5f)),
        1.0f
    );

    int major_step = 1;
    while (pixels_per_second * major_step < 80.0f)
        major_step *= 2;

    int num_ticks = int(project_duration) + 4;
    for (int t = 0; t < num_ticks; ++t) {
        float x = timeline_start.x + t * pixels_per_second;

        if (x < timeline_start.x - 100 || x > timeline_end.x + 100)
            continue;

        bool is_major = (t % major_step == 0);
        ImU32 tick_color = is_major ? 
            ImGui::ColorConvertFloat4ToU32(ImVec4(1.0f, 0.71f, 0.52f, 0.3f)) : 
            ImGui::ColorConvertFloat4ToU32(ImVec4(1.0f, 0.69f, 0.46f, 0.18f));

        float tick_height = is_major ? tick_major_height : tick_minor_height;

        draw_list->AddLine(
            ImVec2(x, timeline_start.y),
            ImVec2(x, timeline_start.y + tick_height),
            tick_color
        );

        if (is_major) {
            char label[16];
            snprintf(label, sizeof(label), "%d", t);
            draw_list->AddText(ImVec2(x + 3, label_y), IM_COL32(255, 255, 255, label_alpha), label);
        }
    }

    static int dragging_clip_index = -1;
    static float drag_offset_time = 0.0f;
    static bool resizing_left = false;
    static bool resizing_right = false;
    bool clicked_on_clip = false;

    for (size_t i = 0; i < clips.size(); ++i) {
        auto& clip = clips[i];

        float clip_start_x = timeline_start.x + (clip.start_time * pixels_per_second);
        float clip_end_x = clip_start_x + (clip.duration * pixels_per_second);

        int y_index = clip.type == ClipType::Video
        ? max_video_layer - clip.layer - 1
        : max_video_layer + clip.layer;

        float layer_y = timeline_start.y + y_index * (layer_height + layer_padding);

        ImVec2 clip_rect_min = ImVec2(clip_start_x, layer_y + layer_padding);
        ImVec2 clip_rect_max = ImVec2(clip_end_x, layer_y + layer_height);

        // Draw clip body with gradient and themed colors
        ImU32 fill_color_top, fill_color_bottom;
        if (clip.type == ClipType::Audio) {
            // Blue gradient for audio with orange tint
            fill_color_top = ImGui::ColorConvertFloat4ToU32(ImVec4(0.25f, 0.30f, 0.45f, 1.00f));
            fill_color_bottom = ImGui::ColorConvertFloat4ToU32(ImVec4(0.18f, 0.22f, 0.35f, 1.00f));
        } else {
            // Red-orange gradient for video
            fill_color_top = ImGui::ColorConvertFloat4ToU32(ImVec4(0.60f, 0.30f, 0.20f, 1.00f));
            fill_color_bottom = ImGui::ColorConvertFloat4ToU32(ImVec4(0.45f, 0.22f, 0.15f, 1.00f));
        }
        
        // Draw clip body with gradient
        draw_list->AddRectFilledMultiColor(
            clip_rect_min,
            clip_rect_max,
            fill_color_top,
            fill_color_top,
            fill_color_bottom,
            fill_color_bottom
        );
        
        // Draw clip border with slight rounding
        ImU32 border_color = ImGui::ColorConvertFloat4ToU32(ImVec4(0.65f, 0.65f, 0.68f, 0.8f));
        draw_list->AddRect(
            clip_rect_min,
            clip_rect_max,
            border_color,
            4.0f, // Rounded corners
            0,
            1.0f  // Line width
        );

        // Draw thumbnails if video
        if (clip.type == ClipType::Video && is_video_file(clip.path) && show_thumbs) {
            // Find the thumbnails vector in GLResources using the clip path
            auto thumb_it = res.clip_thumbnail_textures.find(clip.path); // Need access to GLResources 'res' here! Pass it in.
           if (thumb_it != res.clip_thumbnail_textures.end()) {
                const auto& thumbnails = thumb_it->second; // Get reference to vector<GLuint>

               if (!thumbnails.empty()) {
                    const float thumb_spacing = 2.0f;
                    const float thumb_height = (clip_rect_max.y - clip_rect_min.y) - 8.0f;
                    const float thumb_width = thumb_height * (static_cast<float>(THUMBNAIL_WIDTH) / static_cast<float>(THUMBNAIL_HEIGHT)); // Use defined aspect ratio
                    const float total_thumb_area_width = clip_rect_max.x - clip_rect_min.x;
                    const float available_width_per_thumb = total_thumb_area_width / static_cast<float>(thumbnails.size());

                    float current_x = clip_rect_min.x + thumb_spacing;
                    float thumb_y = clip_rect_min.y + 4.0f;

                    for (size_t t = 0; t < thumbnails.size(); ++t) {
                        // Calculate position based on index, distribute evenly
                        float thumb_x = clip_rect_min.x + (t * available_width_per_thumb);
                        // Clamp width to available space per thumb or actual thumb width
                        float display_thumb_width = std::min(thumb_width, available_width_per_thumb - thumb_spacing);

                        ImVec2 thumb_min(thumb_x + thumb_spacing / 2.0f, thumb_y);
                        ImVec2 thumb_max(thumb_x + thumb_spacing / 2.0f + display_thumb_width, thumb_y + thumb_height);

                        // Ensure thumbnail doesn't exceed clip bounds
                        thumb_max.x = std::min(thumb_max.x, clip_rect_max.x - thumb_spacing / 2.0f);
                        if (thumb_min.x >= thumb_max.x || thumb_min.y >= thumb_max.y) continue; // Skip if zero size

                        GLuint tex = thumbnails[t];
                        if (tex != 0) { // Check if texture ID is valid
                            draw_list->AddImage(
                                (ImTextureID)(intptr_t)tex,
                                thumb_min, thumb_max,
                                ImVec2(0, 0), ImVec2(1, 1),
                                IM_COL32_WHITE
                            );
                           // Optional: Add border around actual thumbnail
                           // draw_list->AddRect(thumb_min, thumb_max, IM_COL32(255, 255, 255, 50), 2.0f);
                        } else {
                            // Optionally draw a placeholder if texture ID is 0 (still loading?)
                            draw_list->AddRectFilled(thumb_min, thumb_max, IM_COL32(50, 50, 55, 150), 2.0f);
                        }
                    }
                } else {
                    // Draw a placeholder if the vector exists but is empty (loading)
                    ImVec2 placeholder_min = ImVec2(clip_rect_min.x + 4, clip_rect_min.y + 4);
                    ImVec2 placeholder_max = ImVec2(clip_rect_max.x - 4, clip_rect_max.y - 4);
                    draw_list->AddRectFilled(placeholder_min, placeholder_max, IM_COL32(50, 50, 55, 100), 4.0f);
                    draw_list->AddText(placeholder_min, IM_COL32_WHITE, "...");
                }
           }
           // else: Thumbnails haven't been queued yet or clip path not found (shouldn't happen if queued correctly)
       }

        // Clip title with shadow effect for better readability
        std::string clip_title = clip.name;
        ImVec2 title_size = ImGui::CalcTextSize(clip_title.c_str());
        ImVec2 title_pos = ImVec2(clip_rect_min.x + 6, clip_rect_min.y + 6);
        
        // Text shadow
        draw_list->AddText(
            ImVec2(title_pos.x + 1, title_pos.y + 1),
            ImGui::ColorConvertFloat4ToU32(ImVec4(0.0f, 0.0f, 0.0f, 0.8f)),
            clip_title.c_str()
        );
        
        // Actual text
        draw_list->AddText(
            title_pos,
            ImGui::ColorConvertFloat4ToU32(ImVec4(0.95f, 0.95f, 0.95f, 0.95f)),
            clip_title.c_str()
        );

        // If audio clip, draw stylized waveform
        if (clip.type == ClipType::Audio && !clip.waveform.empty()) {
            int waveform_samples = clip.waveform.size();
            ImU32 waveform_color = ImGui::ColorConvertFloat4ToU32(ImVec4(1.0f, 0.9f, 0.7f, 0.8f));
            ImU32 waveform_highlight = ImGui::ColorConvertFloat4ToU32(ImVec4(1.0f, 0.75f, 0.4f, 0.9f));
            
            float center_y = (clip_rect_min.y + clip_rect_max.y) / 2.0f;
            float max_height = (clip_rect_max.y - clip_rect_min.y) * 0.7f;
            
            for (int s = 0; s < waveform_samples; ++s) {
                float amp = clip.waveform[s];
                float norm = float(s) / waveform_samples;
                float x = clip_rect_min.x + norm * (clip_rect_max.x - clip_rect_min.x);
                float height = max_height * amp;
                
                // Gradient color based on amplitude
                auto Lerp = [](const ImVec4& a, const ImVec4& b, float t) {
                    return ImVec4(
                        a.x + t * (b.x - a.x),
                        a.y + t * (b.y - a.y),
                        a.z + t * (b.z - a.z),
                        a.w + t * (b.w - a.w)
                    );
                };
                ImU32 line_color = ImGui::GetColorU32(Lerp(
                    ImVec4(0.6f, 0.7f, 0.9f, 0.7f),
                    ImVec4(accent.x, accent.y, accent.z, 0.9f),
                    amp
                ));
                
                draw_list->AddLine(
                    ImVec2(x, center_y - height), 
                    ImVec2(x, center_y + height), 
                    line_color,
                    1.0f
                );
            }
        }


        // Selection highlight with accent color
        if (&clip == selected_clip) {
            // Highlight with orange accent color from theme
            draw_list->AddRect(
                clip_rect_min, 
                clip_rect_max, 
                col_accent,
                4.0f, // Rounded corners
                0,
                2.0f  // Thicker line for selection
            );
            clip.selected = true;
        } else {
            clip.selected = false;
        }

        // Selection
        if (!resizing_left && !resizing_right && dragging_clip_index == -1 &&
            ImGui::IsMouseHoveringRect(clip_rect_min, clip_rect_max) && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            selected_clip = &clip;
            clicked_on_clip = true;
        }

        // Deselect if clicked outside any clips
        if (!clicked_on_clip && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && ImGui::IsWindowHovered()) {
            selected_clip = nullptr;
        }

        // Clip dragging
        ImVec2 drag_area_start = ImVec2(clip_start_x + 10.0f, clip_rect_min.y);
        ImVec2 drag_area_end = ImVec2(clip_end_x - 10.0f, clip_rect_max.y);
        ImGui::SetCursorScreenPos(drag_area_start);
        ImGui::InvisibleButton(("clip_drag" + std::to_string(i)).c_str(), ImVec2(drag_area_end.x - drag_area_start.x, drag_area_end.y - drag_area_start.y));

        if (ImGui::IsItemHovered()) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);

        if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
            if (dragging_clip_index == -1) {
                dragging_clip_index = i;
                float mouse_x = ImGui::GetMousePos().x;
                drag_offset_time = (mouse_x - clip_start_x) / pixels_per_second;
            }

            if (dragging_clip_index == i) {
                float mouse_x = ImGui::GetMousePos().x;
                float new_start = (mouse_x - timeline_start.x) / pixels_per_second - drag_offset_time;
                new_start = std::max(0.0f, new_start);
                float old_start = clip.start_time;
                clip.start_time = new_start;

                // Sync linked clip's start_time if it's not actively being dragged or resized
                if (clip.linked_clip && clip.linked_clip != selected_clip) {
                    float delta = new_start - old_start;
                    clip.linked_clip->start_time += delta;
                }
            }
        } else if (dragging_clip_index == i) {
            dragging_clip_index = -1;
        }

        // Left resizing
        float handle_w = 6.0f;
        ImVec2 left_min = ImVec2(clip_start_x - handle_w / 2, clip_rect_min.y);
        ImVec2 left_max = ImVec2(clip_start_x + handle_w / 2, clip_rect_max.y);

        ImGui::SetCursorScreenPos(left_min);
        ImGui::InvisibleButton(("clip_resize_L" + std::to_string(i)).c_str(), ImVec2(handle_w, clip_rect_max.y - clip_rect_min.y));
        if (ImGui::IsItemHovered()) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);

        if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
            if (!resizing_right) {
                resizing_left = true;
                float mouse_x = ImGui::GetMousePos().x;
                float new_start = (mouse_x - timeline_start.x) / pixels_per_second;
                float new_duration = clip.start_time + clip.duration - new_start;
                float delta = new_start - clip.start_time;
                if (delta > 0.0f) clip.media_start += delta;
                clip.start_time = std::max(0.0f, new_start);
                clip.duration = std::max(0.1f, new_duration);

                if (clip.linked_clip && clip.linked_clip != selected_clip) {
                    clip.linked_clip->start_time += delta;
                    clip.linked_clip->duration = clip.duration;
                }
            }
        } else if (resizing_left) resizing_left = false;

        // Right resizing
        ImVec2 right_min = ImVec2(clip_end_x - handle_w / 2, clip_rect_min.y);
        ImVec2 right_max = ImVec2(clip_end_x + handle_w / 2, clip_rect_max.y);

        ImGui::SetCursorScreenPos(right_min);
        ImGui::InvisibleButton(("clip_resize_R" + std::to_string(i)).c_str(), ImVec2(handle_w, clip_rect_max.y - clip_rect_min.y));
        if (ImGui::IsItemHovered()) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);

        if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
            if (!resizing_left) {
                resizing_right = true;
                float mouse_x = ImGui::GetMousePos().x;
                float new_end = (mouse_x - timeline_start.x) / pixels_per_second;
                float old_duration = clip.duration;
                clip.duration = std::max(0.1f, new_end - clip.start_time);

                float delta = clip.duration - old_duration;
                if (clip.linked_clip && clip.linked_clip != selected_clip) {
                    clip.linked_clip->duration += delta;
                }
            }
        } else if (resizing_right) resizing_right = false;

        // Create unique ID and hit area for context menu
        ImGui::SetCursorScreenPos(clip_rect_min);
        ImGui::InvisibleButton(("clip_context" + std::to_string(i)).c_str(), 
                            ImVec2(clip_rect_max.x - clip_rect_min.x, clip_rect_max.y - clip_rect_min.y));

        // Context menu for linking with themed style
        if (ImGui::BeginPopupContextItem(("clip_context" + std::to_string(i)).c_str())) {
            ImGui::PushStyleColor(ImGuiCol_Text, text);
            ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(accent.x, accent.y, accent.z, 0.55f));
            ImGui::PushStyleColor(ImGuiCol_HeaderHovered, accent_hover);
            ImGui::PushStyleColor(ImGuiCol_HeaderActive, accent_active);
            
            if (clip.linked_clip && ImGui::MenuItem("Unlink Audio/Video")) {
                clip.linked_clip->linked_clip = nullptr;
                clip.linked_clip = nullptr;
            }
            
            ImGui::PopStyleColor(4);
            ImGui::EndPopup();
        }      
    }

    // Playhead rendering
    float playhead_x = timeline_start.x + playhead_time * pixels_per_second;
    
    const float line_width = 2.5f;
    const float head_width = 12.5f;
    const float head_height = 18.0f;
    const ImU32 playhead_color = IM_COL32(255, 143, 38, 255);
    const ImU32 shadow_color = IM_COL32(0, 0, 0, 80);

    // ===== PRECISE SHADOW ALIGNMENT =====
    const float shadow_offset = 1.5f;
    const float shadow_blur = 3.0f;

    // Head shadow (mathematically offset path)
    draw_list->PathClear();
    draw_list->PathLineTo(ImVec2(playhead_x + shadow_offset, timeline_start.y + head_height + shadow_offset));
    draw_list->PathBezierCubicCurveTo(
        ImVec2(playhead_x - head_width*0.42f + shadow_offset, timeline_start.y + head_height*0.66f + shadow_offset),
        ImVec2(playhead_x - head_width*0.66f + shadow_offset, timeline_start.y + line_width + shadow_offset),
        ImVec2(playhead_x + shadow_offset, timeline_start.y + shadow_offset)
    );
    draw_list->PathBezierCubicCurveTo(
        ImVec2(playhead_x + head_width*0.66f + shadow_offset, timeline_start.y + line_width + shadow_offset),
        ImVec2(playhead_x + head_width*0.42f + shadow_offset, timeline_start.y + head_height*0.66f + shadow_offset),
        ImVec2(playhead_x + shadow_offset, timeline_start.y + head_height + shadow_offset)
    );
    draw_list->PathStroke(shadow_color, false, shadow_blur);

    // Line shadow with perfect width matching
    draw_list->AddLine(
        ImVec2(playhead_x + shadow_offset, timeline_start.y + head_height + shadow_offset - line_width/2),
        ImVec2(playhead_x + shadow_offset, timeline_start.y + timeline_height),
        shadow_color, line_width + shadow_blur
    );

    // ===== MATHEMATICALLY PERFECT HEAD-LINE CONNECTION =====
    const float connection_radius = line_width * 0.8f;
    const ImVec2 connection_point = ImVec2(playhead_x, timeline_start.y + head_height - connection_radius);

    // Head shape with exact line-width termination
    draw_list->PathClear();
    draw_list->PathLineTo(connection_point);
    draw_list->PathBezierCubicCurveTo(
        ImVec2(playhead_x - head_width*0.47f, timeline_start.y + head_height*0.6f),
        ImVec2(playhead_x - head_width*0.65f, timeline_start.y + line_width*1.95f),
        ImVec2(playhead_x, timeline_start.y)
    );
    draw_list->PathBezierCubicCurveTo(
        ImVec2(playhead_x + head_width*0.65f, timeline_start.y + line_width*1.95f),
        ImVec2(playhead_x + head_width*0.47f, timeline_start.y + head_height*0.6f),
        connection_point
    );
    draw_list->PathFillConvex(playhead_color);

    // Vertical line with exact width matching
    draw_list->AddLine(
        connection_point,
        ImVec2(playhead_x - 1.0f, timeline_start.y + timeline_height),
        playhead_color, line_width
    );

    // Connection reinforcement
    draw_list->AddCircleFilled(
        connection_point,
        connection_radius,
        playhead_color
    );

    // Dragging the playhead (only within timeline area)
    static bool dragging_playhead = false;
    ImGui::SetCursorScreenPos(ImVec2(playhead_x - 3, timeline_start.y));
    ImGui::InvisibleButton("##Playhead", ImVec2(6, timeline_height));
    if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
        dragging_playhead = true;
        float mouse_x = ImGui::GetMousePos().x;
        playhead_time = (mouse_x - timeline_start.x) / pixels_per_second;
        playhead_time = std::clamp(playhead_time, 0.0f, max_duration);
    } else if (dragging_playhead && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        dragging_playhead = false;
    }

    // Clicking anywhere to move playhead (only within timeline area)
    if (!clicked_on_clip && ImGui::IsWindowHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        ImVec2 mouse = ImGui::GetMousePos();
        if (mouse.y > timeline_start.y && mouse.y < timeline_end.y && 
            mouse.x > timeline_start.x && mouse.x < timeline_end.x) {
            playhead_time = (mouse.x - timeline_start.x) / pixels_per_second;
            playhead_time = std::clamp(playhead_time, 0.0f, max_duration);
        }
    }

    // Need to advance cursor past the timeline for future components
    ImGui::SetCursorScreenPos(ImVec2(labels_start.x, labels_start.y + timeline_height + 5));

    ImGui::End();
}

template<typename T>
void DrawKeyframeTrackEditor(const std::string& label, KeyframeTrack<T>& track) {
    if (ImGui::TreeNode(label.c_str())) {
        int to_remove = -1;

        for (size_t i = 0; i < track.keyframes.size(); ++i) {
            auto& kf = track.keyframes[i];

            ImGui::PushID(static_cast<int>(i));
            ImGui::Separator();

            // --- Editable Time
            ImGui::SetNextItemWidth(80);
            ImGui::DragFloat("Time", &kf.time, 0.01f, 0.0f, 999.0f, "%.3f");

            ImGui::SameLine();
            // --- Editable Value
            ImGui::SetNextItemWidth(120);
            ImGui::DragScalar("Value", ImGuiDataType_Float, &kf.value, 0.01f);

            ImGui::SameLine();
            // --- Interpolation Type
            const char* interp_labels[] = {"Linear", "EaseInOut", "Hold"};
            int interp_idx = static_cast<int>(kf.interp);
            ImGui::SetNextItemWidth(100);
            if (ImGui::Combo("Interp", &interp_idx, interp_labels, IM_ARRAYSIZE(interp_labels))) {
                kf.interp = static_cast<InterpolationType>(interp_idx);
            }

            ImGui::SameLine();
            if (ImGui::Button("Delete")) {
                to_remove = static_cast<int>(i);
            }

            ImGui::PopID();
        }

        if (to_remove >= 0) {
            track.keyframes.erase(track.keyframes.begin() + to_remove);
        }

        // Add new keyframe button
        if (ImGui::Button("Add Keyframe")) {
            track.keyframes.push_back(Keyframe<T>{
                0.0f,    // time
                T{},     // value (default-constructed)
                InterpolationType::Linear
            });
        }

        ImGui::TreePop();
    }
}