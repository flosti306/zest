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
#include <stack>

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
#include <glm/glm.hpp> // For vec2, vec3 etc.
#include <glm/gtc/matrix_transform.hpp> // For ortho

// Project headers
#include "video_export.hpp" // Include AFTER system headers and glad/imgui
#include "shared.hpp"
#include "project_io.hpp"
#include "effects.hpp"

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

int render_width = 1920;
int render_height = 1080;
int export_fps = 30;

Clip* selected_clip = nullptr; // Keep track of selected clip

bool show_thumbs = false;

// --- Mask Drawing Editor State ---
static bool mask_editor_open = false;
static MaskEffectNode* current_editing_mask_node = nullptr;
static GLuint mask_draw_fbo = 0;
static GLuint mask_draw_texture = 0;
static int mask_draw_width = 0;  // Will be set based on preview size
static int mask_draw_height = 0;
static float mask_brush_size = 20.0f; // In pixels
static glm::vec3 mask_brush_color = glm::vec3(1.0f, 1.0f, 1.0f); // White = paint mask
static bool mask_needs_apply = false; // Flag if drawing occurred
static GLuint background_clip_texture_id = 0; // Texture of the clip being masked
static bool mask_editor_show_background = true; // NEW: Toggle state
static GLuint mask_editor_preview_program = 0; // Shader for combined preview
static GLuint mask_editor_dummy_vao = 0;       // VAO for fullscreen quad drawing

static bool smart_mask_editor_open = false;
static MaskEffectNode* current_editing_smart_mask_node = nullptr;
static GLuint smart_mask_editor_bg_clip_tex = 0; // Texture of the clip frame to show
static GLuint smart_mask_editor_overlay_tex = 0; // Texture of the generated GrabCut mask
static ImVec2 smart_mask_roi_start_pos; // For drawing the ROI rectangle
static ImVec2 smart_mask_roi_end_pos;
static bool smart_mask_drawing_roi = false;
static DecodedFrame smart_mask_current_decoded_frame; // To hold pixels for OpenCV
enum class SmartMaskEditTool { ROI_RECT, PAINT_FG, PAINT_BG };
static SmartMaskEditTool smart_mask_current_tool = SmartMaskEditTool::ROI_RECT;
static cv::Mat smart_mask_editor_scribble_hints_cv; // CV_8UC1, stores GC_BGD, GC_FGD, etc.
static GLuint smart_mask_editor_scribble_display_tex = 0; // To visualize scribbles
static float smart_mask_brush_size_px = 10.0f;
static cv::Mat smart_mask_bgd_model;
static cv::Mat smart_mask_fgd_model;
static bool smart_mask_is_initialized = false;
static cv::Mat last_grabcut_mask_cv;

std::stack<std::string> undo_stack;
std::stack<std::string> redo_stack;

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
    ImVec4 frame_bg         = ImVec4(0.19f, 0.19f, 0.22f, 1.00f);

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
        audio_clip.volume = 1.0f;
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

    if (is_video_file(input_path)) {
        auto it = res.video_cache.find(input_path);
        if (it != res.video_cache.end() && it->second.is_initialized) {
            VideoData& video = it->second;
            float first_frame_time = 0.0f; // Or clip.media_start if needed
            if (should_request_frame(video, first_frame_time)) {
                std::lock_guard<std::mutex> lock(decoder_request_mutex);
                DecodedFrameRequest req;
                req.clip_path = input_path;
                req.timestamp = first_frame_time;
                decoder_request_queue.push_back(req);
                decoder_worker_cv.notify_one();
            }
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

// Helper to create/resize the drawing FBO/Texture
bool setup_mask_draw_buffer(int width, int height) {
    if (width <= 0 || height <= 0) return false;

    // Cleanup existing if dimensions change
    if (mask_draw_fbo != 0 && (width != mask_draw_width || height != mask_draw_height)) {
        glDeleteFramebuffers(1, &mask_draw_fbo);
        glDeleteTextures(1, &mask_draw_texture);
        mask_draw_fbo = 0;
        mask_draw_texture = 0;
        std::cout << "Recreating mask draw buffer due to size change." << std::endl;
    }

    mask_draw_width = width;
    mask_draw_height = height;

    if (mask_draw_fbo == 0) {
        glGenFramebuffers(1, &mask_draw_fbo);
        glGenTextures(1, &mask_draw_texture);

        glBindTexture(GL_TEXTURE_2D, mask_draw_texture);
        // Use RGB8 for simple white/black/gray drawing
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB8, width, height, 0, GL_RGB, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

        glBindFramebuffer(GL_FRAMEBUFFER, mask_draw_fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, mask_draw_texture, 0);

        GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
        if (status != GL_FRAMEBUFFER_COMPLETE) {
            std::cerr << "Mask Draw Framebuffer incomplete! Status: " << status << std::endl;
            glDeleteFramebuffers(1, &mask_draw_fbo);
            glDeleteTextures(1, &mask_draw_texture);
            mask_draw_fbo = 0;
            mask_draw_texture = 0;
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            return false;
        }
         glBindFramebuffer(GL_FRAMEBUFFER, 0);
         std::cout << "Mask draw buffer created (" << width << "x" << height << ")" << std::endl;

         // --- Initial Clear ---
         // Clear the texture to a neutral state (e.g., black = transparent mask)
         // Or potentially initialize from the node's existing texture if it exists?
         // For simplicity, clear to black.
         GLint last_fbo; glGetIntegerv(GL_FRAMEBUFFER_BINDING, &last_fbo);
         glBindFramebuffer(GL_FRAMEBUFFER, mask_draw_fbo);
         glViewport(0,0, width, height);
         glClearColor(0.0f, 0.0f, 0.0f, 1.0f); // Clear to black
         glClear(GL_COLOR_BUFFER_BIT);
         glBindFramebuffer(GL_FRAMEBUFFER, last_fbo); // Restore previous FBO
         // Restore viewport? Assumes caller will set it later.
    }
    return true;
}

// Helper to clean up resources when editor closes
void cleanup_mask_draw_buffer() {
    if (mask_draw_fbo != 0) {
        glDeleteFramebuffers(1, &mask_draw_fbo);
        mask_draw_fbo = 0;
    }
    if (mask_draw_texture != 0) {
        glDeleteTextures(1, &mask_draw_texture);
        mask_draw_texture = 0;
    }
    mask_draw_width = 0;
    mask_draw_height = 0;
    current_editing_mask_node = nullptr;
    background_clip_texture_id = 0;
    std::cout << "Mask draw buffer cleaned up." << std::endl;
}

// Function to open the editor
void OpenMaskEditor(MaskEffectNode* node, GLuint clip_texture_for_background) {
     if (!node) return;
     // Setup buffer with current preview dimensions
     if (!setup_mask_draw_buffer(preview_width, preview_height)) {
         tinyfd_messageBox("Error", "Could not create mask drawing buffer.", "ok", "error", 1);
         return;
     }
     current_editing_mask_node = node;
     background_clip_texture_id = clip_texture_for_background; // Store the background
     mask_editor_open = true;
     mask_needs_apply = false; // Reset apply flag

     // Optional: Initialize the drawing buffer with the node's current mask texture if it exists
     if (node->mask_texture != 0) {
         // This requires ensuring the node's texture is the same size as the draw buffer
         // Or resampling. For simplicity, we start fresh by clearing.
         // If you want to load previous drawing:
         // 1. Check if node->mask_texture exists and has size mask_draw_width/height
         // 2. If so, copy node->mask_texture to mask_draw_texture using FBO blit or glCopyTexSubImage2D
         // Otherwise, clear as done in setup_mask_draw_buffer.
     }
}

// Function to draw a brush stroke (using immediate mode OpenGL for simplicity)
void DrawBrushStroke(glm::vec2 uv, float brush_size_pixels, glm::vec3 color) {
    if (!mask_draw_fbo || mask_draw_width <= 0 || mask_draw_height <= 0) return;

    // Backup GL state
    GLint last_viewport[4]; glGetIntegerv(GL_VIEWPORT, last_viewport);
    GLint last_fbo; glGetIntegerv(GL_FRAMEBUFFER_BINDING, &last_fbo);
    // GLboolean last_blend = glIsEnabled(GL_BLEND);
    // GLboolean last_texture = glIsEnabled(GL_TEXTURE_2D);
    // GLboolean last_depth = glIsEnabled(GL_DEPTH_TEST);

    // Setup for drawing into FBO
    glBindFramebuffer(GL_FRAMEBUFFER, mask_draw_fbo);
    glViewport(0, 0, mask_draw_width, mask_draw_height);

    // Simple Ortho projection matching buffer dimensions
    glMatrixMode(GL_PROJECTION);
    glPushMatrix();
    glLoadIdentity();
    glOrtho(0.0, mask_draw_width, mask_draw_height, 0.0, -1.0, 1.0); // Top-left origin

    glMatrixMode(GL_MODELVIEW);
    glPushMatrix();
    glLoadIdentity();

    // Disable things we don't need
    glDisable(GL_TEXTURE_2D);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND); // Overwrite with solid color

    // Convert UV to pixel coordinates
    float px = uv.x * mask_draw_width;
    float py = uv.y * mask_draw_height;
    float radius = brush_size_pixels * 0.5f;

    // Draw a simple circle using a triangle fan
    int num_segments = 20;
    glColor3f(color.r, color.g, color.b);
    glBegin(GL_TRIANGLE_FAN);
    glVertex2f(px, py); // Center
    for (int i = 0; i <= num_segments; i++) {
        float angle = i * (2.0f * M_PI / num_segments);
        glVertex2f(px + cos(angle) * radius, py + sin(angle) * radius);
    }
    glEnd();

    // Restore GL State
    glMatrixMode(GL_PROJECTION);
    glPopMatrix();
    glMatrixMode(GL_MODELVIEW);
    glPopMatrix();

    glBindFramebuffer(GL_FRAMEBUFFER, last_fbo);
    glViewport(last_viewport[0], last_viewport[1], last_viewport[2], last_viewport[3]);
    // if (last_blend) glEnable(GL_BLEND);
    // if (last_texture) glEnable(GL_TEXTURE_2D);
    // if (last_depth) glEnable(GL_DEPTH_TEST);
    // glColor4f(1,1,1,1); // Reset color

    mask_needs_apply = true; // Mark that we've drawn something
}

// Function to apply the drawn mask to the node's texture
bool ApplyDrawnMask() {
    if (!current_editing_mask_node || !mask_draw_fbo || !mask_draw_texture || mask_draw_width <= 0 || mask_draw_height <= 0) {
        std::cerr << "ApplyDrawnMask: Invalid state." << std::endl;
        return false;
    }

    // --- Ensure the target node's texture exists and matches size ---
    bool texture_created_or_resized = false;
    // Check if texture exists
    if (current_editing_mask_node->mask_texture == 0) {
        glGenTextures(1, &current_editing_mask_node->mask_texture);
        texture_created_or_resized = true;
        std::cout << "Generated mask texture ID " << current_editing_mask_node->mask_texture << " for node." << std::endl;
    } else {
        // Optional: Check if size matches, recreate if not (more robust)
        GLint existing_width = 0, existing_height = 0;
        glBindTexture(GL_TEXTURE_2D, current_editing_mask_node->mask_texture);
        glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &existing_width);
        glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &existing_height);
        glBindTexture(GL_TEXTURE_2D, 0); // Unbind after checking
        if (existing_width != mask_draw_width || existing_height != mask_draw_height) {
            std::cout << "Resizing node mask texture from " << existing_width << "x" << existing_height
                      << " to " << mask_draw_width << "x" << mask_draw_height << std::endl;
            texture_created_or_resized = true; // Need to re-init texture image
        }
    }

    // Initialize or re-initialize texture image if needed
    if (texture_created_or_resized) {
        glBindTexture(GL_TEXTURE_2D, current_editing_mask_node->mask_texture);
        // Use RGB8 format matching the drawing buffer
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB8, mask_draw_width, mask_draw_height, 0, GL_RGB, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glBindTexture(GL_TEXTURE_2D, 0);
    }

    // --- Create a temporary FBO for the destination texture ---
    GLuint dest_fbo = 0;
    glGenFramebuffers(1, &dest_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, dest_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, current_editing_mask_node->mask_texture, 0);

    GLenum dest_status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (dest_status != GL_FRAMEBUFFER_COMPLETE) {
        std::cerr << "ApplyDrawnMask: Destination FBO incomplete! Status: " << dest_status << std::endl;
        glBindFramebuffer(GL_FRAMEBUFFER, 0); // Unbind potentially incomplete FBO
        glDeleteFramebuffers(1, &dest_fbo);
        return false;
    }
    // Destination FBO is ready and bound to GL_FRAMEBUFFER

    // --- Perform the Blit with Vertical Flip ---
    GLint last_read_fbo, last_draw_fbo;
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &last_read_fbo);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &last_draw_fbo);

    glBindFramebuffer(GL_READ_FRAMEBUFFER, mask_draw_fbo); // Source: our drawing buffer
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, dest_fbo);      // Destination: the temp FBO for the node's texture

    // Blit operation with flip:
    // Source rectangle: (0, 0) bottom-left to (w, h) top-right
    // Dest rectangle:   (0, h) top-left   to (w, 0) bottom-right
    // This vertical flip aligns the top-left drawing origin with OpenGL's bottom-left texture origin.
    glBlitFramebuffer(
        0, 0, mask_draw_width, mask_draw_height, // Source Rect (x0, y0, x1, y1)
        0, mask_draw_height, mask_draw_width, 0, // Dest Rect -> FLIPS Y-AXIS!
        GL_COLOR_BUFFER_BIT,                     // Mask (copy color)
        GL_NEAREST                               // Filter (Nearest is fine for exact copy)
    );

    GLenum err = glGetError();
    if (err != GL_NO_ERROR) {
        std::cerr << "OpenGL Error (" << err << ") during glBlitFramebuffer!" << std::endl;
         // Restore bindings before returning
         glBindFramebuffer(GL_READ_FRAMEBUFFER, last_read_fbo);
         glBindFramebuffer(GL_DRAW_FRAMEBUFFER, last_draw_fbo);
         glDeleteFramebuffers(1, &dest_fbo); // Clean up temp dest FBO
        return false;
    }

    // --- Restore state and Cleanup ---
    glBindFramebuffer(GL_READ_FRAMEBUFFER, last_read_fbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, last_draw_fbo);
    glDeleteFramebuffers(1, &dest_fbo); // Clean up temp dest FBO

    current_editing_mask_node->mask_texture_path = ""; // Mark as drawn, not file-based
    current_editing_mask_node->mask_type = MaskEffectNode::MaskType::Texture; // Ensure type is set
    mask_needs_apply = false;
    std::cout << "Applied drawn mask (flipped) to node texture ID: " << current_editing_mask_node->mask_texture << std::endl;
    return true;
}

void SetupMaskEditorPreviewResources() {
    // Use the same vertex shader as your effects if it's a simple passthrough
    // Or create a specific "shaders/mask_editor_preview.vert"
    mask_editor_preview_program = LoadShaderProgram("shaders/mask_preview.vert", "shaders/mask_preview.frag");
    if (mask_editor_preview_program == 0) {
        std::cerr << "FATAL: Failed to load mask editor preview shader!" << std::endl;
        // Handle error
    }
    // mask_editor_dummy_vao is NO LONGER NEEDED if using RenderFullscreenQuad
    mask_editor_dummy_vao = 0; // Or delete it if it was created
    std::cout << "Loaded mask editor preview shaders. VAO will be from RenderFullscreenQuad." << std::endl;
}

// --- The Editor Window Drawing Function ---
// Call this function from your main loop if mask_editor_open is true
void DrawMaskEditorWindow(GLResources& res) { // Pass GLResources if needed later
    if (!mask_editor_open || !current_editing_mask_node) {
        cleanup_mask_draw_buffer(); // Clean up if node became null somehow
        mask_editor_open = false;
        return;
    }

    static GLuint mask_preview_fbo = 0;
    static GLuint mask_preview_texture = 0;
    static int mask_preview_width = 0;
    static int mask_preview_height = 0;

    ImGui::SetNextWindowSize(ImVec2(600, 700), ImGuiCond_FirstUseEver); // Decent default size
    if (ImGui::Begin("Mask Drawing Editor", &mask_editor_open)) {

         // --- Draw Controls ---
         ImGui::Text("Editing Mask for Effect: %s", current_editing_mask_node->name.c_str());
         ImGui::Separator();

         ImGui::SliderFloat("Brush Size", &mask_brush_size, 1.0f, 100.0f, "%.0f pixels");

         if (ImGui::RadioButton("Paint (White)", mask_brush_color.r > 0.5f)) {
             mask_brush_color = glm::vec3(1.0f, 1.0f, 1.0f);
         }
         ImGui::SameLine();
         if (ImGui::RadioButton("Erase (Black)", mask_brush_color.r < 0.5f)) {
             mask_brush_color = glm::vec3(0.0f, 0.0f, 0.0f);
         }
         ImGui::SameLine();
         // Optional: Clear button
         if (ImGui::Button("Clear")) {
             GLint last_fbo; glGetIntegerv(GL_FRAMEBUFFER_BINDING, &last_fbo);
             glBindFramebuffer(GL_FRAMEBUFFER, mask_draw_fbo);
             glViewport(0,0, mask_draw_width, mask_draw_height);
             glClearColor(0.0f, 0.0f, 0.0f, 1.0f); // Clear to black
             glClear(GL_COLOR_BUFFER_BIT);
             glBindFramebuffer(GL_FRAMEBUFFER, last_fbo);
             mask_needs_apply = true; // Need to apply the clear
         }

         ImGui::Separator();

         ImGui::Checkbox("Show Background Preview", &mask_editor_show_background);
        ImGui::Separator();

         // --- Drawing Canvas ---
         ImGui::Text("Canvas:");
         ImVec2 canvas_pos = ImGui::GetCursorScreenPos();
         ImVec2 canvas_size = ImGui::GetContentRegionAvail();
         // Try to maintain aspect ratio of the drawing buffer
         float aspect = (float)mask_draw_width / (float)mask_draw_height;
         if (canvas_size.x / aspect <= canvas_size.y) {
             canvas_size.y = canvas_size.x / aspect;
         } else {
             canvas_size.x = canvas_size.y * aspect;
         }
         canvas_size.x = std::max(canvas_size.x, 50.0f); // Min size
         canvas_size.y = std::max(canvas_size.y, 50.0f);

         // --- Render the Preview ---
        ImVec2 uv0 = ImVec2(0, 0); // Top-left for ImGui display
        ImVec2 uv1 = ImVec2(1, 1); // Bottom-right for ImGui display

        if (mask_editor_show_background && background_clip_texture_id != 0 && mask_draw_texture != 0 && mask_editor_preview_program != 0) {
            // --- Ensure preview FBO/texture are set up for current canvas size ---
            // We'll use the mask_draw_width/height as the rendering resolution for the preview FBO
            // This keeps the preview 1:1 with the drawing buffer.
            if (mask_preview_fbo == 0 || mask_preview_width != mask_draw_width || mask_preview_height != mask_draw_height) {
                if (mask_preview_fbo != 0) {
                    glDeleteFramebuffers(1, &mask_preview_fbo);
                    glDeleteTextures(1, &mask_preview_texture);
                }
                mask_preview_width = mask_draw_width;
                mask_preview_height = mask_draw_height;
        
                glGenFramebuffers(1, &mask_preview_fbo);
                glGenTextures(1, &mask_preview_texture);
        
                glBindTexture(GL_TEXTURE_2D, mask_preview_texture);
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, mask_preview_width, mask_preview_height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                glBindTexture(GL_TEXTURE_2D, 0);
        
                glBindFramebuffer(GL_FRAMEBUFFER, mask_preview_fbo);
                glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, mask_preview_texture, 0);
        
                if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
                    std::cerr << "ERROR: Mask Preview FBO incomplete!" << std::endl;
                    // Cleanup and disable combined preview
                    glDeleteFramebuffers(1, &mask_preview_fbo); mask_preview_fbo = 0;
                    glDeleteTextures(1, &mask_preview_texture); mask_preview_texture = 0;
                }
                glBindFramebuffer(GL_FRAMEBUFFER, 0); // Unbind
                std::cout << "Mask Preview FBO created/resized: " << mask_preview_width << "x" << mask_preview_height << std::endl;
            }
        
            if (mask_preview_fbo != 0) {
                // --- Render to the mask_preview_fbo ---
                GLint last_fbo; glGetIntegerv(GL_FRAMEBUFFER_BINDING, &last_fbo);
                GLint last_vp[4]; glGetIntegerv(GL_VIEWPORT, last_vp);
                // GLboolean last_blend = glIsEnabled(GL_BLEND); // Backup other states if necessary
        
                glBindFramebuffer(GL_FRAMEBUFFER, mask_preview_fbo);
                glViewport(0, 0, mask_preview_width, mask_preview_height);
                // glClearColor(0.0f, 0.0f, 0.0f, 0.0f); // Optional: Clear if needed
                // glClear(GL_COLOR_BUFFER_BIT);
        
                glUseProgram(mask_editor_preview_program);
        
                glActiveTexture(GL_TEXTURE0);
                glBindTexture(GL_TEXTURE_2D, background_clip_texture_id);
                glUniform1i(glGetUniformLocation(mask_editor_preview_program, "u_BackgroundTexture"), 0);
        
                glActiveTexture(GL_TEXTURE1);
                glBindTexture(GL_TEXTURE_2D, mask_draw_texture); // This is the one drawn by DrawBrushStroke
                glUniform1i(glGetUniformLocation(mask_editor_preview_program, "u_DrawnMaskTexture"), 1);
                
                // Set procedural mask uniforms (get values from current_editing_mask_node)
                // This allows visualizing the rect/circle from the node's properties as a guide
                // while you are *drawing* a texture mask.
                glUniform2f(glGetUniformLocation(mask_editor_preview_program, "u_Resolution"), (float)mask_preview_width, (float)mask_preview_height);
                if (current_editing_mask_node->mask_type == MaskEffectNode::MaskType::Rectangle) {
                    glUniform1i(glGetUniformLocation(mask_editor_preview_program, "u_ProceduralMaskType"), 1);
                    glUniform1f(glGetUniformLocation(mask_editor_preview_program, "u_ProceduralFeather"), current_editing_mask_node->feather_track.Evaluate(playhead_time)); // Or current_editing_mask_node->feather
                    glUniform1i(glGetUniformLocation(mask_editor_preview_program, "u_InvertProcedural"), current_editing_mask_node->invert);
                    glUniform2f(glGetUniformLocation(mask_editor_preview_program, "u_RectCenter"), current_editing_mask_node->rect_center_x_track.Evaluate(playhead_time), current_editing_mask_node->rect_center_y_track.Evaluate(playhead_time));
                    glUniform2f(glGetUniformLocation(mask_editor_preview_program, "u_RectSize"), current_editing_mask_node->rect_size_x_track.Evaluate(playhead_time), current_editing_mask_node->rect_size_y_track.Evaluate(playhead_time));
                    glUniform1f(glGetUniformLocation(mask_editor_preview_program, "u_RectRotation"), glm::radians(current_editing_mask_node->rect_rotation_track.Evaluate(playhead_time)));
                    glUniform1f(glGetUniformLocation(mask_editor_preview_program, "u_RectCornerRadius"), current_editing_mask_node->rect_corner_radius_track.Evaluate(playhead_time));
                } else if (current_editing_mask_node->mask_type == MaskEffectNode::MaskType::Circle) {
                    glUniform1i(glGetUniformLocation(mask_editor_preview_program, "u_ProceduralMaskType"), 2);
                    glUniform1f(glGetUniformLocation(mask_editor_preview_program, "u_ProceduralFeather"), current_editing_mask_node->feather_track.Evaluate(playhead_time));
                    glUniform1i(glGetUniformLocation(mask_editor_preview_program, "u_InvertProcedural"), current_editing_mask_node->invert);
                    glUniform2f(glGetUniformLocation(mask_editor_preview_program, "u_CircleCenter"), current_editing_mask_node->circle_center_x_track.Evaluate(playhead_time), current_editing_mask_node->circle_center_y_track.Evaluate(playhead_time));
                    glUniform1f(glGetUniformLocation(mask_editor_preview_program, "u_CircleRadius"), current_editing_mask_node->circle_radius_track.Evaluate(playhead_time));
                    glUniform1f(glGetUniformLocation(mask_editor_preview_program, "u_CircleAspectRatio"), current_editing_mask_node->circle_aspect_ratio_track.Evaluate(playhead_time));
                } else {
                    glUniform1i(glGetUniformLocation(mask_editor_preview_program, "u_ProceduralMaskType"), 0); // None
                }
        
        
                RenderFullscreenQuad(mask_preview_width, mask_draw_height); // Use your existing utility!
        
                // Restore GL state
                glBindFramebuffer(GL_FRAMEBUFFER, last_fbo);
                glViewport(last_vp[0], last_vp[1], last_vp[2], last_vp[3]);
                glUseProgram(0); // Or last program
                glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, 0);
                glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_2D, 0);
                // Restore other states like blend if changed
            }
        
            // --- Display the rendered mask_preview_texture using ImGui::Image ---
            if (mask_preview_texture != 0) {
                // `mask_preview_texture` was rendered with bottom-left origin.
                // ImGui::Image default UVs (0,0) to (1,1) expect bottom-left.
                // However, if your RenderFullscreenQuad internally flips UVs for some reason, adjust here.
                // Assuming standard behavior:
                ImGui::Image((ImTextureID)(intptr_t)mask_preview_texture, canvas_size, ImVec2(0, 1), ImVec2(1, 0)); // Flip Y for display
            } else {
                ImGui::Text("Preview FBO Error"); // Fallback
                ImGui::Image((ImTextureID)(intptr_t)mask_draw_texture, canvas_size, ImVec2(0, 1), ImVec2(1, 0)); // Show B&W
            }
        
        } else {
            // Show only the drawn mask (B&W) - this part was working
            ImGui::Image((ImTextureID)(intptr_t)mask_draw_texture, canvas_size, ImVec2(0, 1), ImVec2(1, 0));
        }

         // Display the current state of the mask texture
         // ImGui::Image((ImTextureID)(intptr_t)mask_draw_texture, canvas_size, ImVec2(0, 1), ImVec2(1, 0)); // Flip UVs for display

         // Use InvisibleButton to capture input over the image area
         ImGui::SetCursorScreenPos(canvas_pos); // Reset cursor pos to overlay button correctly
         ImGui::InvisibleButton("##mask_canvas", canvas_size);

         ImVec2 canvas_min = ImGui::GetItemRectMin(); // Top-left of the canvas area in screen coords
         ImVec2 canvas_max = ImGui::GetItemRectMax(); // Bottom-right

         // Handle drawing input if hovering over the canvas
         if (ImGui::IsItemHovered()) {
             ImGui::SetMouseCursor(ImGuiMouseCursor_Hand); // Indicate drawable area
             if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                 ImVec2 mouse_pos_abs = ImGui::GetMousePos();
                 ImVec2 mouse_pos_rel = ImVec2(mouse_pos_abs.x - canvas_min.x, mouse_pos_abs.y - canvas_min.y);

                 // Convert relative pixel position to UV coordinates [0, 1]
                 glm::vec2 uv = glm::vec2(mouse_pos_rel.x / canvas_size.x, mouse_pos_rel.y / canvas_size.y);

                 // Clamp UVs (optional, prevents drawing outside if mouse slips)
                 uv.x = std::max(0.0f, std::min(1.0f, uv.x));
                 uv.y = std::max(0.0f, std::min(1.0f, uv.y));

                 // Draw the stroke
                 DrawBrushStroke(uv, mask_brush_size, mask_brush_color);
             }
         }

         // --- Apply Button ---
         ImGui::Separator();
         bool applied_this_frame = false;
        if (ImGui::Button("Apply Mask")) {
                applied_this_frame = ApplyDrawnMask(); // ApplyDrawnMask now returns bool
                if (applied_this_frame) {
                    tinyfd_messageBox("Success", "Mask applied.", "ok", "info", 1);
                } else {
                    tinyfd_messageBox("Error", "Failed to apply mask.", "ok", "error", 1);
                }
        }
         ImGui::SameLine();
         if (ImGui::Button("Close")) {
             mask_editor_open = false;
             // Optional: Ask if user wants to apply changes if mask_needs_apply is true
         }
         if (mask_needs_apply) {
             ImGui::SameLine();
             ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "Changes not applied!");
         }

    }
    ImGui::End();

    // If window was closed by user, cleanup
    if (!mask_editor_open) {
        // Optional: Prompt to save/apply if mask_needs_apply is true
        cleanup_mask_draw_buffer();
    }
}

std::string SerializeProject(const std::vector<Clip>& clips, float playhead_time, float zoom_factor) {
    std::ostringstream oss;
    SaveProjectToStream(oss, clips, playhead_time, zoom_factor); // Implement this using your SaveProject logic
    return oss.str();
}

bool DeserializeProject(const std::string& data, std::vector<Clip>& clips, float& playhead_time, float& zoom_factor) {
    std::istringstream iss(data);
    return LoadProjectFromStream(iss, clips, playhead_time, zoom_factor); // Implement this using your LoadProject logic
}

void PushUndo(const std::vector<Clip>& clips, float playhead_time, float zoom_factor) {
    undo_stack.push(SerializeProject(clips, playhead_time, zoom_factor));
    // Clear redo stack on new action
    while (!redo_stack.empty()) redo_stack.pop();
}

void Undo(std::vector<Clip>& clips, float& playhead_time, float& zoom_factor) {
    if (undo_stack.empty()) return;
    // Push current state to redo stack
    redo_stack.push(SerializeProject(clips, playhead_time, zoom_factor));
    // Restore previous state
    std::string prev = undo_stack.top(); undo_stack.pop();
    DeserializeProject(prev, clips, playhead_time, zoom_factor);
}

void Redo(std::vector<Clip>& clips, float& playhead_time, float& zoom_factor) {
    if (redo_stack.empty()) return;
    // Push current state to undo stack
    undo_stack.push(SerializeProject(clips, playhead_time, zoom_factor));
    // Restore next state
    std::string next = redo_stack.top(); redo_stack.pop();
    DeserializeProject(next, clips, playhead_time, zoom_factor);
}

// --- Forward Declarations ---
template<typename T>
void DrawKeyframeTrackEditor(const std::string& label, KeyframeTrack<T>& track);
void DrawTimelineEditor(std::vector<Clip>& clips, float& playhead_time, float& max_duration, float& zoom_factor, std::atomic<bool>& layers_changed, Clip*& selected_clip, GLResources& res, float& last_playhead_time_for_velocity);
void UpdatePreview(GLResources& res, const std::vector<Clip>& sorted_clips, int width, int height, float playhead_time, bool force_update);
void RenderPreviewWindow(GLuint preview_tex, int preview_width, int preview_height);
void DrawEffectUIForClip(Clip& clip, GLResources& gl_resources);
void OpenSmartMaskEditor(MaskEffectNode* node, GLuint clip_texture_for_background, const DecodedFrame& decoded_frame_for_cv);
void DrawSmartMaskEditorWindow();

// --- Main Application ---
int main(int argc, char* argv[]) {
    avformat_network_init();

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) < 0) {
        std::cerr << "Failed to initialize SDL: " << SDL_GetError() << std::endl;
        return 1;
    }

    // Initialize SDL audio subsystem specifically
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) < 0) {
        std::cerr << "Failed to initialize SDL audio subsystem: " << SDL_GetError() << std::endl;
        SDL_Quit();
        return 1;
    }

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

    ImGui_ImplSDL3_InitForOpenGL(window, gl_context);
    ImGui_ImplOpenGL3_Init("#version 330"); // Or "#version 330 core" for modern GL

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
    float last_playhead_time_for_velocity = 0.0f;
    float playhead_velocity = 0.0f; // in timeline seconds per real-time second

    GLResources gl_resources;
    if (!setup_gl_resources(gl_resources, preview_width, preview_height)) {
        std::cerr << "Failed to initialize initial GL resources!" << std::endl;
        ImGui_ImplOpenGL3_Shutdown(); ImGui_ImplSDL3_Shutdown(); ImGui::DestroyContext();
        // Use SDL_GL_DestroyContext (Fix 1)
        SDL_GL_DestroyContext(gl_context);
        SDL_DestroyWindow(window); SDL_Quit();
        return 1;
    }

    SetupMaskEditorPreviewResources();

    if(show_thumbs) start_thumbnail_worker();
    start_decoder_worker();

    bool running = true;
    last_frame_ticks = SDL_GetTicks();

    // Initialize audio playback
    AudioPlaybackState audio_state;
    if (!initialize_audio_playback(audio_state)) {
        std::cerr << "Failed to initialize audio playback. Continuing without audio." << std::endl;
    } else {
        std::cout << "Audio playback initialized successfully." << std::endl;
    }

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
            } else if (event.type == SDL_EVENT_KEY_DOWN && (event.key.key == SDLK_LEFT || event.key.key == SDLK_RIGHT) && event.key.down) {
                if (export_fps > 0) {
                    // 1. Calculate the duration of a single frame.
                    const float frame_duration = 1.0f / static_cast<float>(export_fps);

                    // 2. Pause playback, as frame-stepping is a manual action.
                    if (playing) {
                        playing = false;
                        std::cout << "Playback paused due to frame-step." << std::endl;
                    }

                    // 3. Adjust playhead time by one frame.
                    if (event.key.key == SDLK_LEFT) {
                        playhead_time -= frame_duration;
                    } else { // Right Arrow
                        playhead_time += frame_duration;
                    }

                    // 4. Clamp the playhead to the timeline boundaries.
                    playhead_time = std::max(0.0f, std::min(playhead_time, max_duration));
                }
            }
            // Use event.key.key and SDLK_B (Fix 3 & 4)
            else if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_B && (SDL_GetModState() & SDL_KMOD_CTRL) && selected_clip && event.key.down) {
                // --- DEFINITIVE, POINTER-SAFE BLADE TOOL LOGIC ---
                PushUndo(clips, playhead_time, zoom_factor);

                // 1. Validate the cut position
                float cut_time = playhead_time;
                float clip_start_global = selected_clip->start_time;
                float clip_end_global = selected_clip->start_time + selected_clip->duration;

                if (cut_time <= clip_start_global + 0.01f || cut_time >= clip_end_global - 0.01f) {
                    std::cout << "Split position is too close to the edge of the clip." << std::endl;
                    continue; // Use continue to skip the rest of the event block
                }

                // 2. Find the INDICES of the original clips (video and audio). This is pointer-safe.
                ptrdiff_t video_idx = -1;
                ptrdiff_t audio_idx = -1;

                // Find the index of the selected clip first
                ptrdiff_t selected_idx = -1;
                for(size_t i = 0; i < clips.size(); ++i) {
                    if (&clips[i] == selected_clip) {
                        selected_idx = i;
                        break;
                    }
                }
                if (selected_idx == -1) continue; // Should not happen, but safe

                if (clips[selected_idx].type == ClipType::Video) {
                    video_idx = selected_idx;
                    // Find the linked audio clip's index
                    if (clips[selected_idx].linked_clip) {
                        for(size_t i = 0; i < clips.size(); ++i) {
                            if (&clips[i] == clips[selected_idx].linked_clip) {
                                audio_idx = i;
                                break;
                            }
                        }
                    }
                } else { // Selected clip is Audio
                    audio_idx = selected_idx;
                    // Find the linked video clip's index
                    if (clips[selected_idx].linked_clip) {
                        for(size_t i = 0; i < clips.size(); ++i) {
                            if (&clips[i] == clips[selected_idx].linked_clip) {
                                video_idx = i;
                                break;
                            }
                        }
                    }
                }

                // 3. Calculate split timings
                float left_duration = cut_time - clips[video_idx].start_time;
                float right_duration = clip_end_global - cut_time;
                float media_start_offset_for_right_part = left_duration;

                // 4. Create the new "right-hand" clips. They are not in the vector yet.
                Clip right_video_part = clips[video_idx]; // Copy original
                right_video_part.start_time = cut_time;
                right_video_part.duration = right_duration;
                right_video_part.media_start += media_start_offset_for_right_part;
                right_video_part.name += " (B)";
                right_video_part.selected = false;
                right_video_part.linked_clip = nullptr; // Links will be set later

                // 5. Modify the original clips (the "left-hand" parts) using their safe indices.
                clips[video_idx].duration = left_duration;
                // Important: Unlink originals for now. We will relink them after vector modification.
                clips[video_idx].linked_clip = nullptr;

                if (audio_idx != -1) {
                    clips[audio_idx].duration = left_duration;
                    clips[audio_idx].linked_clip = nullptr;
                }
                
                // 6. Push new clips to the vector. This is the point where pointers could become invalid.
                clips.push_back(right_video_part);
                ptrdiff_t right_video_idx = clips.size() - 1;

                if (audio_idx != -1) {
                    Clip right_audio_part = clips[audio_idx]; // Copy original audio
                    right_audio_part.start_time = cut_time;
                    right_audio_part.duration = right_duration;
                    right_audio_part.media_start += media_start_offset_for_right_part;
                    right_audio_part.name += " (B)";
                    right_audio_part.selected = false;
                    right_audio_part.linked_clip = nullptr;
                    
                    clips.push_back(right_audio_part);
                    ptrdiff_t right_audio_idx = clips.size() - 1;

                    // 7. Re-establish ALL links using the now-stable indices.
                    // Link the left pair
                    clips[video_idx].linked_clip = &clips[audio_idx];
                    clips[audio_idx].linked_clip = &clips[video_idx];
                    // Link the right pair
                    clips[right_video_idx].linked_clip = &clips[right_audio_idx];
                    clips[right_audio_idx].linked_clip = &clips[right_video_idx];
                }

                // 8. Finalize state
                layers_changed = true;
                selected_clip = &clips[right_video_idx]; // Safely select the new clip
                std::cout << "Split clip at " << cut_time << "s. All links correctly maintained." << std::endl;
                // --- End Blade Tool ---
            }
            // Use event.key.key and SDLK_DELETE (Fix 3 & 4)
            else if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_DELETE && selected_clip && event.key.down) {
                PushUndo(clips, playhead_time, zoom_factor);
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
            } else if (event.type == SDL_EVENT_KEY_DOWN && (SDL_GetModState() & SDL_KMOD_CTRL)) {
                if (event.key.key == SDLK_Z) {
                    Undo(clips, playhead_time, zoom_factor);
                    layers_changed = true;
                    process_message = "Undo";
                } else if (event.key.key == SDLK_Y) {
                    Redo(clips, playhead_time, zoom_factor);
                    layers_changed = true;
                    process_message = "Redo";
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
                    PushUndo(clips, playhead_time, zoom_factor);
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
        
        static float last_time = 0.0f;
        float current_time = playhead_time; 

        // --- OPTIMIZATION: Centralized Preview Update Logic ---
        
        // 1. Determine active clips for the current frame
    std::vector<Clip> active_clips_for_preview;
    for (const auto& clip : clips) {
        if (playhead_time >= clip.start_time && playhead_time < clip.start_time + clip.duration) {
            active_clips_for_preview.push_back(clip);
        }
    }
    
    // 2. Determine playback state (playing, scrubbing, paused)
    bool is_playing_live, is_scrubbing;
    // Pass the master 'playing' atomic to correctly determine state
    update_playback_state(gl_resources, playhead_time, last_time, is_playing_live, is_scrubbing);
    last_time = playhead_time;
    is_playing_live = is_playing_live && playing.load(); 

    // 3. Queue decode requests based on state (prefetching with priority)
    update_video_previews(gl_resources, active_clips_for_preview, playhead_time, is_playing_live, is_scrubbing);
    
    // 4. Process any completed frames from the decoder thread
    process_decoded_frames(gl_resources, 30); // Process up to 30 frames per UI loop

    // 5. Update textures from the cache using asynchronous PBO uploads
    for (const auto& clip : active_clips_for_preview) {
        if (is_video_file(clip.path)) {
            auto it = gl_resources.video_cache.find(clip.path);
            if (it != gl_resources.video_cache.end() && it->second.is_initialized) {
                double media_time = (playhead_time - clip.start_time) + clip.media_start;
                // Use 'strict' mode when not in smooth playback for frame-accuracy
                update_texture_from_cache(it->second, media_time, !is_playing_live);
            }
        }
    }

    // 6. Render the final composite frame to the FBO.
    std::vector<Clip> sorted_clips = clips;
    std::sort(sorted_clips.begin(), sorted_clips.end(), [](const Clip& a, const Clip& b) { return a.layer < b.layer; });
    render_frame(gl_resources, playhead_time, sorted_clips, preview_width, preview_height);

        // --- End of Centralized Preview Update Logic ---

        if(show_thumbs) ProcessThumbnailResults(gl_resources, 2);

        if (delta_time > 0) {
            // Velocity = (change in timeline position) / (change in real time)
            playhead_velocity = std::abs(playhead_time - last_playhead_time_for_velocity) / delta_time;
        }
        last_playhead_time_for_velocity = playhead_time;

        ImGui_ImplOpenGL3_NewFrame(); ImGui_ImplSDL3_NewFrame(); ImGui::NewFrame();
        RenderDockSpace();

        ImGui::Begin("Controls");
        ApplyWindowBackgroundGradients();
        ImGui::InputText("Output Path", output_path, sizeof(output_path));
        ImGui::InputInt("FPS", &export_fps, 1, 60, 1);
        ImGui::Text("Resolution");
        ImGui::InputInt2("##Resolution", &render_width, ImGuiInputTextFlags_CharsDecimal);
        ImGui::Separator();
        if (ImGui::Button(playing ? "Pause (Space)" : "Play (Space)")) playing = !playing.load();
        ImGui::SameLine(); 
        ImGui::Text("Time: %.2f / %.2f", playhead_time, max_duration);
        if (ImGui::SliderFloat("##Seek", &playhead_time, 0.0f, max_duration, "%.2f s")) {
             layers_changed = true; playing = false;
        }
        ImGui::Separator();
        if (ImGui::Button("Export Video")) {
            SDL_GL_MakeCurrent(window, gl_context);
            std::filesystem::path out_p(output_path);
            if (!out_p.has_filename()) { process_message = "Output path is not a valid filename!"; }
            else {
                 int export_duration_frames = static_cast<int>(std::ceil(max_duration * export_fps));
                 if (export_duration_frames <= 0) { process_message = "Cannot export empty timeline!"; }
                 else {
                    process_message = "Exporting...";
                    bool success = start_video_export(output_path, render_width, render_height, export_fps, export_duration_frames, clips, window);
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

        DrawTimelineEditor(clips, playhead_time, max_duration, zoom_factor, layers_changed, selected_clip, gl_resources, last_playhead_time_for_velocity);
        RenderPreviewWindow(gl_resources.render_tex, preview_width, preview_height);

        if (mask_editor_open) {
            DrawMaskEditorWindow(gl_resources); // Pass gl_resources if needed inside editor
        }

        if (smart_mask_editor_open) {
            DrawSmartMaskEditorWindow(); // Call this similar to DrawMaskEditorWindow
        }

        /* // --- The PREVIEW RENDERING LOGIC ---
        // This logic is now simpler. We don't need fancy rate limiting because the main thread
        // is so fast. We just update the texture if the playhead moved.
       // This block runs every frame to keep the preview live.
        
        // 1. Update video textures from cache for any active video clips.
        // This is a fast, non-blocking operation that just uploads the best-available frame.
        for (const auto& clip : active_clips_for_preview) {
            if (is_video_file(clip.path)) {
                auto it = gl_resources.video_cache.find(clip.path);
                if (it != gl_resources.video_cache.end() && it->second.is_initialized) {
                    double media_time = (playhead_time - clip.start_time) + clip.media_start;
                    // update_texture_from_cache is now robust enough to handle any media_time value.
                    update_texture_from_cache(it->second, media_time, !playing);
                }
            }
        }
        
        // 2. Render the final composite frame to the FBO using the now-updated textures.
        // This is a fast GPU-only operation.
        std::vector<Clip> sorted_clips = clips;
        std::sort(sorted_clips.begin(), sorted_clips.end(), [](const Clip& a, const Clip& b) { return a.layer < b.layer; });
        render_frame(gl_resources, playhead_time, sorted_clips, preview_width, preview_height);

        RenderPreviewWindow(gl_resources.render_tex, preview_width, preview_height); */

        ImGui::Begin("Inspector"); ApplyWindowBackgroundGradients();
        if (selected_clip) {
            ImGui::Text("Selected: %s", selected_clip->name.c_str()); 
            ImGui::Text("Path: %s", selected_clip->path.c_str());
            bool changed = false;

            // Common properties for all clip types
            ImGui::SeparatorText("Timing & Trimming");
            changed |= ImGui::InputFloat("Start Time", &selected_clip->start_time, 0.1f, 1.0f, "%.2f"); 
            selected_clip->start_time = std::max(0.0f, selected_clip->start_time);
            changed |= ImGui::InputFloat("Media Start", &selected_clip->media_start, 0.1f, 1.0f, "%.2f"); 
            selected_clip->media_start = std::max(0.0f, selected_clip->media_start);
            changed |= ImGui::InputFloat("Duration", &selected_clip->duration, 0.1f, 1.0f, "%.2f"); 
            selected_clip->duration = std::max(0.01f, selected_clip->duration);

            ImGui::SeparatorText("Layering");
            changed |= ImGui::InputInt("Layer", &selected_clip->layer); 
            selected_clip->layer = std::max(0, selected_clip->layer);
            const char* blend_modes[] = { "Normal", "Additive", "Multiply", "Screen", "Darken", "Lighten", "Difference", "Subtract", "Divide", "Overlay"};
            int current_mode = static_cast<int>(selected_clip->blend_mode);

            if (ImGui::Combo("Blend Mode", &current_mode, blend_modes, IM_ARRAYSIZE(blend_modes))) {
                selected_clip->blend_mode = static_cast<BlendMode>(current_mode);
                changed = true;
            }

            // Type-specific properties
            if (selected_clip->type == ClipType::Video) {
                ImGui::SeparatorText("Transform");
                changed |= ImGui::SliderFloat("Pos X", &selected_clip->pos_x, -1.0f, 1.0f, "%.3f");
                changed |= ImGui::SliderFloat("Pos Y", &selected_clip->pos_y, -1.0f, 1.0f, "%.3f");
                changed |= ImGui::SliderFloat("Scale", &selected_clip->scale, 0.0f, 10.0f, "%.3f");
                changed |= ImGui::SliderFloat("Rotation", &selected_clip->rotation, 0.0f, 360.0f, "%.3f");
                changed |= ImGui::SliderFloat("Opacity", &selected_clip->opacity, 0.0f, 1.0f, "%.3f");

                ImGui::SeparatorText("Keying");
                DrawKeyframeTrackEditor("Opacity Keyframes", selected_clip->opacity_track);
                DrawKeyframeTrackEditor("Position X Keyframes", selected_clip->pos_x_track);
                DrawKeyframeTrackEditor("Position Y Keyframes", selected_clip->pos_y_track);
                DrawKeyframeTrackEditor("Rotation Keyframes", selected_clip->rotation_track);
                DrawKeyframeTrackEditor("Scale Keyframes", selected_clip->scale_track);

                ImGui::SeparatorText("Effects");
                DrawEffectUIForClip(*selected_clip, gl_resources);
            } else if (selected_clip->type == ClipType::Audio) {
                ImGui::SeparatorText("Audio Properties");
                changed |= ImGui::SliderFloat("Volume", &selected_clip->volume, 0.0f, 1.0f, "%.3f");
                DrawKeyframeTrackEditor("Volume Keyframes", selected_clip->volume_track);

                // Display waveform if available
                if (!selected_clip->waveform.empty()) {
                    ImGui::SeparatorText("Waveform");
                    ImVec2 waveform_size = ImVec2(ImGui::GetContentRegionAvail().x, 100);
                    ImDrawList* draw_list = ImGui::GetWindowDrawList();
                    ImVec2 waveform_pos = ImGui::GetCursorScreenPos();
                    ImVec2 waveform_end = ImVec2(waveform_pos.x + waveform_size.x, waveform_pos.y + waveform_size.y);
                    
                    // Draw background
                    draw_list->AddRectFilled(waveform_pos, waveform_end, IM_COL32(30, 30, 30, 255));
                    
                    // Draw waveform
                    float center_y = waveform_pos.y + waveform_size.y * 0.5f;
                    float max_height = waveform_size.y * 0.4f;
                    ImU32 waveform_color = IM_COL32(100, 200, 255, 255);
                    
                    for (size_t i = 0; i < selected_clip->waveform.size(); ++i) {
                        float x = waveform_pos.x + (float)i / selected_clip->waveform.size() * waveform_size.x;
                        float height = selected_clip->waveform[i] * max_height;
                        draw_list->AddLine(
                            ImVec2(x, center_y - height),
                            ImVec2(x, center_y + height),
                            waveform_color
                        );
                    }
                    
                    ImGui::Dummy(waveform_size);
                }
            }

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

        // Update audio playback
        if (playing) {
            update_audio_playback(audio_state, clips, playhead_time, gl_resources);
        }

    } // End main loop

    std::cout << "Cleaning up resources..." << std::endl;

    stop_thumbnail_worker();
    stop_decoder_worker();

    if (ImGui::GetIO().UserData) { IM_DELETE((GradientData*)ImGui::GetIO().UserData); ImGui::GetIO().UserData = nullptr; }
    cleanup_gl_resources(gl_resources); cleanup_video_resources(gl_resources);
    ImGui_ImplOpenGL3_Shutdown(); ImGui_ImplSDL3_Shutdown(); ImGui::DestroyContext();
    // Use SDL_GL_DestroyContext (Fix 1)
    SDL_GL_DestroyContext(gl_context);
    SDL_DestroyWindow(window); SDL_Quit(); avformat_network_deinit();
    std::cout << "Exiting application." << std::endl;

    // Cleanup
    cleanup_audio_playback(audio_state);

    return 0;
}


// --- Updated UpdatePreview Function ---
void UpdatePreview(GLResources& res, const std::vector<Clip>& sorted_clips, int width, int height, float playhead_time, bool force_update) {
    render_frame(res, playhead_time, sorted_clips, width, height);
}

// --- RenderFullscreenQuad Function ---
void RenderFullscreenQuad(float width, float height) {
    // Backup GL state
    GLint last_viewport[4];
    glGetIntegerv(GL_VIEWPORT, last_viewport);

    // Set viewport to match the quad size
    glViewport(0, 0, static_cast<GLsizei>(width), static_cast<GLsizei>(height));

    // Render a simple fullscreen quad
    glBegin(GL_QUADS);
    glTexCoord2f(0.0f, 0.0f); glVertex2f(-1.0f, -1.0f);
    glTexCoord2f(1.0f, 0.0f); glVertex2f(1.0f, -1.0f);
    glTexCoord2f(1.0f, 1.0f); glVertex2f(1.0f, 1.0f);
    glTexCoord2f(0.0f, 1.0f); glVertex2f(-1.0f, 1.0f);
    glEnd();

    // Restore GL state
    glViewport(last_viewport[0], last_viewport[1], last_viewport[2], last_viewport[3]);
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
    GLResources& res,
    float& last_playhead_time_for_velocity
) {
    ImGui::Begin("Timeline Editor");
    ApplyWindowBackgroundGradients();

    // Constants
    const float label_width = 120.0f; // Wider area for labels
    const float timeline_width = ImGui::GetContentRegionAvail().x - label_width;
    const float layer_height = 60.0f;
    const float layer_padding = 10.0f;
    const float scrollbar_height = 12.0f;

    static float scroll_x = 0.0f;

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
    ImVec2 timeline_end = ImVec2(timeline_start.x + timeline_width, timeline_start.y + timeline_height);
    
    ImDrawList* draw_list = ImGui::GetWindowDrawList();

    // --- NEW: Calculate and clamp scroll range ---
    float max_scroll_x = std::max(0.0f, timeline_size - timeline_width);
    scroll_x = std::clamp(scroll_x, 0.0f, max_scroll_x);
    
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

    // --- NEW: Push a clipping rectangle for the scrolling timeline area ---
    draw_list->PushClipRect(ImVec2(timeline_start.x, timeline_start.y - 25.0f), timeline_end, true);

    // Draw timeline background with subtle gradient
    draw_list->AddRectFilledMultiColor(
        timeline_start, 
        timeline_end,
        ImGui::ColorConvertFloat4ToU32(ImVec4(0.16f, 0.16f, 0.18f, 1.00f)), // Top color
        ImGui::ColorConvertFloat4ToU32(ImVec4(0.16f, 0.16f, 0.18f, 1.00f)), // Top right
        ImGui::ColorConvertFloat4ToU32(ImVec4(0.14f, 0.14f, 0.16f, 1.00f)), // Bottom right
        ImGui::ColorConvertFloat4ToU32(ImVec4(0.14f, 0.14f, 0.16f, 1.00f))  // Bottom left
    );


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

    int major_step_seconds = 1;
    while (pixels_per_second * major_step_seconds < 80.0f) {
        major_step_seconds = (major_step_seconds == 1) ? 2 : (major_step_seconds == 2 ? 5 : major_step_seconds * 2);
    }
    
    int first_visible_sec = static_cast<int>(scroll_x / pixels_per_second);
    int last_visible_sec = static_cast<int>((scroll_x + timeline_width) / pixels_per_second) + 1;

    for (int t = first_visible_sec; t <= last_visible_sec; ++t) {
        float x = timeline_start.x + t * pixels_per_second - scroll_x; // Apply scroll offset
    
        bool is_major = (t % major_step_seconds == 0);

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

        // --- UPDATED with scroll offset ---
        float clip_start_x = timeline_start.x + (clip.start_time * pixels_per_second) - scroll_x;
        float clip_end_x = clip_start_x + (clip.duration * pixels_per_second);

        // --- Culling: Don't process or draw clips that are fully off-screen ---
        if (clip_end_x < timeline_start.x || clip_start_x > timeline_end.x) {
            continue;
        }

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
            PushUndo(clips, playhead_time, zoom_factor);
            if (dragging_clip_index == -1) {
                dragging_clip_index = i;
                float mouse_x = ImGui::GetMousePos().x;
                drag_offset_time = (mouse_x - clip_start_x) / pixels_per_second;
            }

            if (dragging_clip_index == i) {
                float mouse_x = ImGui::GetMousePos().x;
                float new_start = (mouse_x - timeline_start.x + scroll_x) / pixels_per_second - drag_offset_time;
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
                PushUndo(clips, playhead_time, zoom_factor);
                resizing_left = true;
                float mouse_x = ImGui::GetMousePos().x;
                float new_start = (mouse_x - timeline_start.x + scroll_x) / pixels_per_second;
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
                PushUndo(clips, playhead_time, zoom_factor);
                resizing_right = true;
                float mouse_x = ImGui::GetMousePos().x;
                float new_end = (mouse_x - timeline_start.x + scroll_x) / pixels_per_second;
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
    float playhead_x = timeline_start.x + playhead_time * pixels_per_second - scroll_x;
    
    const float line_width = 2.5f;
    const float head_width = 12.5f;
    const float head_height = 18.0f;
    const ImU32 playhead_color = IM_COL32(255, 143, 38, 255);
    const ImU32 shadow_color = IM_COL32(0, 0, 0, 80);

    // ===== PRECISE SHADOW ALIGNMENT =====
    const float shadow_offset = 1.5f;
    const float shadow_blur = 3.0f;

    if (playhead_x >= timeline_start.x && playhead_x <= timeline_end.x) {

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

    }

    // Dragging the playhead (only within timeline area)
    static bool dragging_playhead = false;
    ImGui::SetCursorScreenPos(ImVec2(playhead_x - 3, timeline_start.y));
    ImGui::InvisibleButton("##Playhead", ImVec2(6, timeline_height));
    if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
        dragging_playhead = true;
        float mouse_x = ImGui::GetMousePos().x;
        playhead_time = (mouse_x - timeline_start.x + scroll_x) / pixels_per_second;
        playhead_time = std::clamp(playhead_time, 0.0f, max_duration);
        last_playhead_time_for_velocity = playhead_time; 
    } else if (dragging_playhead && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        dragging_playhead = false;
    }

    // Clicking anywhere to move playhead (only within timeline area)
    if (!clicked_on_clip && ImGui::IsWindowHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        ImVec2 mouse = ImGui::GetMousePos();
        if (mouse.y > timeline_start.y && mouse.y < timeline_end.y && 
            mouse.x > timeline_start.x && mouse.x < timeline_end.x) {
            playhead_time = (mouse.x - timeline_start.x + scroll_x) / pixels_per_second;
            playhead_time = std::clamp(playhead_time, 0.0f, max_duration);
            last_playhead_time_for_velocity = playhead_time; 
        }
    }

    draw_list->PopClipRect();

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

    // --- NEW: Custom Scrollbar ---
    if (max_scroll_x > 0) {
        ImGui::SetCursorScreenPos(ImVec2(timeline_start.x, timeline_end.y + 2)); // Position below timeline
        // Background
        draw_list->AddRectFilled(ImGui::GetCursorScreenPos(), ImVec2(timeline_end.x, timeline_end.y + scrollbar_height), IM_COL32(25, 25, 28, 255), 4.0f);
        
        // Handle
        float handle_width = (timeline_width / timeline_size) * timeline_width;
        handle_width = std::max(handle_width, 25.0f); // Min handle size
        float handle_pos_x = timeline_start.x + (scroll_x / max_scroll_x) * (timeline_width - handle_width);
    
        ImGui::SetCursorScreenPos(ImVec2(handle_pos_x, timeline_end.y + 2));
        ImGui::InvisibleButton("##scrollbar_handle", ImVec2(handle_width, scrollbar_height - 4));
        
        ImU32 handle_col = ImGui::IsItemActive() ? col_accent_active : (ImGui::IsItemHovered() ? col_accent_hover : col_accent_muted);
        draw_list->AddRectFilled(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), handle_col, 4.0f);
    
        if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
            float delta_x = ImGui::GetIO().MouseDelta.x;
            float scroll_ratio = delta_x / (timeline_width - handle_width);
            scroll_x += scroll_ratio * max_scroll_x;
            scroll_x = std::clamp(scroll_x, 0.0f, max_scroll_x);
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

void DrawEffectUIForClip(Clip& clip, GLResources& gl_resources) {
    if (!clip.effect_graph)
        clip.effect_graph = std::make_shared<EffectGraph>();

    if (ImGui::CollapsingHeader("Effects")) {
        if (ImGui::Button("Add Gaussian Blur")) {
            auto blur = std::make_shared<GaussianBlurNode>();
            blur->name = "Blur";
            blur->blur_amount = 5.0f;
            clip.effect_graph->nodes.push_back(blur);
        }
        if (ImGui::Button("Add Color Grading")) {
            auto color_grade = std::make_shared<ColorGradingNode>();
            color_grade->name = "Color Grading";
            color_grade->brightness = 0.0f;
            color_grade->contrast = 1.0f;
            color_grade->saturation = 1.0f;
            color_grade->temperature = 0.0f;
            color_grade->tint = 0.0f;
            color_grade->gamma = 1.0f;
            clip.effect_graph->nodes.push_back(color_grade);
        }
        if (ImGui::Button("Add LUT")) {
            auto lut = std::make_shared<LUTColorGradingNode>();
            lut->name = "LUT Color Grading";
            lut->strength = 1.0f;
            clip.effect_graph->nodes.push_back(lut);
        }
        if (ImGui::Button("Add Mask")) {
            auto mask = std::make_shared<MaskEffectNode>();
            // Default settings are set in constructor
            clip.effect_graph->nodes.push_back(mask);
        }
        if (ImGui::Button("Add Solid Color Overlay")) {
            auto solid_fx = std::make_shared<SolidColorEffectNode>();
            // Default values are set in constructor
            clip.effect_graph->nodes.push_back(solid_fx);
        }
        if (ImGui::Button("Add Gradient Overlay")) {
            auto grad_fx = std::make_shared<GradientEffectNode>();
            // Default values are set in constructor
            clip.effect_graph->nodes.push_back(grad_fx);
        }
        if (ImGui::Button("Add Drop Shadow")) {
            auto shadow_fx = std::make_shared<DropShadowEffectNode>();
            // Default values are in constructor
            clip.effect_graph->nodes.push_back(shadow_fx);
        }

        for (size_t i = 0; i < clip.effect_graph->nodes.size(); ++i) {
            bool effect_changed = false;
            auto& node = clip.effect_graph->nodes[i];
            ImGui::PushID(int(i));
            if (ImGui::TreeNode(node->name.c_str())) {
                // Render type-specific UI
                if (auto blur = std::dynamic_pointer_cast<GaussianBlurNode>(node)) {
                    ImGui::SliderFloat("Blur Amount", &blur->blur_amount, 0.0f, 50.0f);
                } else if (auto clr_grade = std::dynamic_pointer_cast<ColorGradingNode>(node)) {
                    ImGui::SliderFloat("Brightness", &clr_grade->brightness, -1.0f, 1.0f);
                    ImGui::SliderFloat("Contrast", &clr_grade->contrast, 0.0f, 2.0f);
                    ImGui::SliderFloat("Saturation", &clr_grade->saturation, 0.0f, 2.0f);
                    ImGui::SliderFloat("Temperature", &clr_grade->temperature, -1.0f, 1.0f);
                    ImGui::SliderFloat("Tint", &clr_grade->tint, -1.0f, 1.0f);
                    ImGui::SliderFloat("Gamma", &clr_grade->gamma, 0.1f, 3.0f);
                } else if (auto lut = std::dynamic_pointer_cast<LUTColorGradingNode>(node)) {
                    ImGui::SliderFloat("Strength", &lut->strength, 0.0f, 1.0f);
                    if (ImGui::Button("Load LUT")) {
                        const char* load_path = tinyfd_openFileDialog("Load Project", "", 1, NULL, "", 0);
                        if (load_path) lut->lut_path = load_path;
                    }
                } else if (auto mask = std::dynamic_pointer_cast<MaskEffectNode>(node)) {
                    const char* mask_types[] = { "None", "Rectangle", "Circle", "Texture", "Smart"};
                    int current_type = static_cast<int>(mask->mask_type);
                    if (ImGui::Combo("Mask Type", &current_type, mask_types, IM_ARRAYSIZE(mask_types))) {
                        mask->mask_type = static_cast<MaskEffectNode::MaskType>(current_type);
                        effect_changed = true;
                    }

                    effect_changed |= ImGui::Checkbox("Invert Mask", &mask->invert);
                    effect_changed |= ImGui::SliderFloat("Feather", &mask->feather, 0.0f, 0.5f, "%.3f"); // Adjust range as needed
                    DrawKeyframeTrackEditor("Feather Keyframes", mask->feather_track);

                    ImGui::Separator();

                    switch (mask->mask_type) {
                        case MaskEffectNode::MaskType::Rectangle:
                            ImGui::Text("Rectangle Properties (Normalized)");
                            effect_changed |= ImGui::SliderFloat2("Center##Rect", &mask->rect_center.x, 0.0f, 1.0f);
                            DrawKeyframeTrackEditor("Center X Keyframes##Rect", mask->rect_center_x_track);
                            DrawKeyframeTrackEditor("Center Y Keyframes##Rect", mask->rect_center_y_track);
                            effect_changed |= ImGui::SliderFloat2("Size##Rect", &mask->rect_size.x, 0.0f, 1.0f);
                            DrawKeyframeTrackEditor("Size X Keyframes##Rect", mask->rect_size_x_track);
                            DrawKeyframeTrackEditor("Size Y Keyframes##Rect", mask->rect_size_y_track);
                            effect_changed |= ImGui::SliderFloat("Rotation##Rect", &mask->rect_rotation, -180.0f, 180.0f);
                            DrawKeyframeTrackEditor("Rotation Keyframes##Rect", mask->rect_rotation_track);
                            effect_changed |= ImGui::SliderFloat("Corner Radius##Rect", &mask->rect_corner_radius, 0.0f, 0.5f, "%.3f");
                            DrawKeyframeTrackEditor("Corner Radius Keyframes##Rect", mask->rect_corner_radius_track);
                            break;
                        case MaskEffectNode::MaskType::Circle:
                            ImGui::Text("Circle Properties (Normalized)");
                            effect_changed |= ImGui::SliderFloat2("Center##Circle", &mask->circle_center.x, 0.0f, 1.0f);
                            DrawKeyframeTrackEditor("Center X Keyframes##Circle", mask->circle_center_x_track);
                            DrawKeyframeTrackEditor("Center Y Keyframes##Circle", mask->circle_center_y_track);
                            effect_changed |= ImGui::SliderFloat("Radius##Circle", &mask->circle_radius, 0.0f, 1.0f); // Radius relative to smaller dimension? Or just normalized? Let's stick to normalized for now.
                            DrawKeyframeTrackEditor("Radius Keyframes##Circle", mask->circle_radius_track);
                            effect_changed |= ImGui::SliderFloat("Aspect Ratio##Circle", &mask->circle_aspect_ratio, 0.1f, 10.0f, "%.2f", ImGuiSliderFlags_Logarithmic);
                            DrawKeyframeTrackEditor("Aspect Ratio Keyframes##Circle", mask->circle_aspect_ratio_track);
                            break;
                        case MaskEffectNode::MaskType::Texture:
                            ImGui::Text("Texture Mask Properties");
                            if (ImGui::Button("Load Mask Texture")) {
                                const char* filterPatterns[3] = { "*.png", "*.bmp", "*.jpg" }; // Allow common image formats
                                const char* path = tinyfd_openFileDialog("Load Mask Texture", "", 3, filterPatterns, "Image Files", 0);
                                if (path) {
                                     if (mask->loadMaskTexture(path)) {
                                         effect_changed = true;
                                         if (current_editing_mask_node == mask.get()) cleanup_mask_draw_buffer();
                                     } else {
                                        tinyfd_messageBox("Error", "Failed to load mask texture.", "ok", "error", 1);
                                     }
                                }
                            }
                            ImGui::Text("Path: %s", mask->mask_texture_path.empty() ? "None" : mask->mask_texture_path.c_str());
                             if (mask->mask_texture != 0) {
                                 ImGui::Image((ImTextureID)(intptr_t)mask->mask_texture, ImVec2(64, 64), ImVec2(0, 0), ImVec2(1, 1), ImVec4(1,1,1,1), ImVec4(1,1,1,0.5)); // Show preview
                             } else {
                                 ImGui::TextColored(ImVec4(1,0,0,1), "No mask texture loaded!");
                             }
                            // TODO: Button "Create/Edit Drawn Mask" - opens a dedicated editor window
                            if (ImGui::Button("Create/Edit Drawn Mask")) {
                                // Need the clip's current texture for background guide
                                GLuint bg_tex_id = 0;
                                if (is_video_file(clip.path)) { // Assuming 'clip' is available here
                                    auto vid_it = gl_resources.video_cache.find(clip.path); // Assuming gl_resources is available
                                    if (vid_it != gl_resources.video_cache.end() && vid_it->second.is_initialized) {
                                        bg_tex_id = vid_it->second.texture_id;
                                    }
                                } else {
                                    auto img_it = gl_resources.texture_cache.find(clip.path);
                                    if (img_it != gl_resources.texture_cache.end()) {
                                        bg_tex_id = img_it->second;
                                    }
                                }
                                if (bg_tex_id != 0) {
                                   OpenMaskEditor(mask.get(), bg_tex_id);
                                } else {
                                    tinyfd_messageBox("Error", "Cannot open mask editor: Clip texture not available.", "ok", "warning", 1);
                                }
                           }

                           ImGui::Text("Path: %s", mask->mask_texture_path.empty() ? "(Using Drawn Mask)" : mask->mask_texture_path.c_str());
                            if (mask->mask_texture != 0) {
                                ImGui::Image((ImTextureID)(intptr_t)mask->mask_texture, ImVec2(128, 128), ImVec2(0, 0), ImVec2(1, 1), ImVec4(1, 1, 1, 1), ImVec4(0, 0, 0, 0.5));
                            } else {
                                ImGui::TextColored(ImVec4(1,0,0,1), "No mask texture loaded!");
                            }
                           break; // End MaskType::Texture case
                        case MaskEffectNode::MaskType::Smart_Interactive:
                            ImGui::Text("Smart Mask");
                            if (ImGui::Button("Edit Interactive Smart Mask...")) {
                                DecodedFrame frame_for_smart_mask;
                                GLuint bg_tex_id_for_smart_mask = 0;

                                // selected_clip should be the same as 'clip' passed to this function
                                // Ensure 'clip' (the function parameter) is the correct one.
                                // If selected_clip is a global, it might be different.
                                // Assuming 'clip' is the relevant one for this effect:
                                if (is_video_file(clip.path)) { // Check if it's a video
                                    auto it = gl_resources.video_cache.find(clip.path);
                                    if (it != gl_resources.video_cache.end() && it->second.is_initialized) {
                                        VideoData& vid_data = it->second;
                                        // Use playhead_time (global or passed in) to determine the current frame
                                        // Make sure playhead_time is accessible here.
                                        float clip_media_time = playhead_time - clip.start_time + clip.media_start;
                                        
                                        ensure_video_decoded_upto(vid_data, clip_media_time);
                                        
                                        const DecodedFrame* found_frame = nullptr;
                                        double min_diff = std::numeric_limits<double>::max();
                                        for (const auto& cached_f : vid_data.frame_cache) {
                                            double diff = std::abs(cached_f.pts - clip_media_time);
                                            if (diff < min_diff) {
                                                min_diff = diff;
                                                found_frame = &cached_f;
                                            }
                                        }
                                        if (found_frame) {
                                            frame_for_smart_mask = *found_frame; 
                                            bg_tex_id_for_smart_mask = vid_data.texture_id;
                                        }
                                    }
                                } else { // It's an image clip
                                    auto img_it = gl_resources.texture_cache.find(clip.path);
                                    if (img_it != gl_resources.texture_cache.end()) {
                                        bg_tex_id_for_smart_mask = img_it->second;
                                        // For images, DecodedFrame needs to be constructed from the image pixels.
                                        // This part is missing if you want to apply GrabCut to images.
                                        // You'd need a function to load image into a DecodedFrame structure
                                        // or adapt RunGrabCut to take a GLuint texture ID and read its pixels.
                                        // For now, let's assume Smart Mask is primarily for video frames.
                                        // If you want it for images, this path needs more work.
                                        std::cerr << "Warning: Smart Mask for still images requires loading image pixels into DecodedFrame." << std::endl;
                                    }
                                }


                                if (!frame_for_smart_mask.pixels.empty() && bg_tex_id_for_smart_mask != 0) {
                                    // --- THIS IS THE FIX ---
                                    OpenSmartMaskEditor(mask.get(), bg_tex_id_for_smart_mask, frame_for_smart_mask);
                                    // --- END FIX ---
                                } else {
                                    tinyfd_messageBox("Error", "Could not get current video frame (or its texture) for smart mask.", "ok", "error", 1);
                                    if (frame_for_smart_mask.pixels.empty()) std::cerr << "Frame for smart mask pixels is empty." << std::endl;
                                    if (bg_tex_id_for_smart_mask == 0) std::cerr << "Background tex ID for smart mask is 0." << std::endl;
                                }
                            }
                            break;
                        case MaskEffectNode::MaskType::None:
                            break; // No specific controls needed
                    }
                } else if (auto solid_color_node = std::dynamic_pointer_cast<SolidColorEffectNode>(node)) {
                    ImGui::Text("Solid Color Properties:");
                    ImGui::ColorEdit4("Color##Solid", &solid_color_node->color.x); // ImGui uses float pointers for colors
                    DrawKeyframeTrackEditor("Red", solid_color_node->red_track);
                    DrawKeyframeTrackEditor("Green", solid_color_node->green_track);
                    DrawKeyframeTrackEditor("Blue", solid_color_node->blue_track);
                    DrawKeyframeTrackEditor("Alpha", solid_color_node->alpha_track);
                    ImGui::SliderFloat("Blend with Original##Solid", &solid_color_node->blend_with_original, 0.0f, 1.0f);
                    DrawKeyframeTrackEditor("Blend", solid_color_node->blend_track);

                } else if (auto gradient_node = std::dynamic_pointer_cast<GradientEffectNode>(node)) {
                    ImGui::Text("Gradient Properties:");
                    const char* grad_types[] = { "Linear", "Radial" };
                    int current_g_type = static_cast<int>(gradient_node->type);
                    if (ImGui::Combo("Type##Gradient", &current_g_type, grad_types, IM_ARRAYSIZE(grad_types))) {
                        gradient_node->type = static_cast<GradientEffectNode::GradientType>(current_g_type);
                    }

                    ImGui::ColorEdit4("Start Color##Gradient", &gradient_node->color_start.x);
                    ImGui::ColorEdit4("End Color##Gradient", &gradient_node->color_end.x);
                    // Example keyframes for alpha
                    DrawKeyframeTrackEditor("Start Alpha", gradient_node->start_color_alpha_track);
                    DrawKeyframeTrackEditor("End Alpha", gradient_node->end_color_alpha_track);


                    if (gradient_node->type == GradientEffectNode::GradientType::Linear) {
                        ImGui::SliderFloat2("Start Point##LinearGrad", &gradient_node->start_point.x, 0.0f, 1.0f, "%.2f");
                        ImGui::SliderFloat2("End Point##LinearGrad", &gradient_node->end_point.x, 0.0f, 1.0f, "%.2f");
                    } else if (gradient_node->type == GradientEffectNode::GradientType::Radial) {
                        ImGui::SliderFloat2("Center##RadialGrad", &gradient_node->center_point.x, 0.0f, 1.0f, "%.2f");
                        ImGui::SliderFloat("Inner Radius##RadialGrad", &gradient_node->radius_inner, 0.0f, 1.0f, "%.3f");
                        ImGui::SliderFloat("Outer Radius##RadialGrad", &gradient_node->radius_outer, 0.0f, 1.5f, "%.3f"); // Outer can go beyond 1
                        ImGui::SliderFloat("Aspect Ratio##RadialGrad", &gradient_node->aspect_ratio, 0.1f, 10.0f, "%.2f", ImGuiSliderFlags_Logarithmic);
                        ImGui::TextWrapped("Aspect ratio for radial. 1.0 attempts to use viewport aspect. Other values override.");
                    }

                    ImGui::SliderFloat("Intensity##Gradient", &gradient_node->intensity, 0.0f, 1.0f);
                    DrawKeyframeTrackEditor("Intensity", gradient_node->intensity_track);
                } else if (auto shadow_node = std::dynamic_pointer_cast<DropShadowEffectNode>(node)) {
                    ImGui::Text("Drop Shadow Properties:");
                    ImGui::DragFloat2("Offset##Shadow", &shadow_node->offset.x, 0.001f, -1.0f, 1.0f, "%.3f");
                    DrawKeyframeTrackEditor("Offset X##Shadow", shadow_node->offset_x_track);
                    DrawKeyframeTrackEditor("Offset Y##Shadow", shadow_node->offset_y_track);

                    ImGui::ColorEdit4("Shadow Color##Shadow", &shadow_node->shadow_color.x);
                    // Add keyframe editors for shadow_color components if desired
                    // DrawKeyframeTrackEditor("Shadow R", shadow_node->shadow_r_track); ... etc.


                    ImGui::SliderFloat("Blur Amount##Shadow", &shadow_node->blur_amount, 0.0f, 50.0f);
                    DrawKeyframeTrackEditor("Blur Amount##Shadow", shadow_node->blur_amount_track);
                }

                // Render common UI elements
                ImGui::Checkbox("Enabled", &node->enabled);
                if (ImGui::Button("Remove")) {
                    clip.effect_graph->nodes.erase(clip.effect_graph->nodes.begin() + i);
                    ImGui::TreePop();
                    ImGui::PopID();
                    break;
                }

                ImGui::TreePop();
            }
            ImGui::PopID();
        }
    }

    clip.has_effects = clip.effect_graph && !clip.effect_graph->nodes.empty();
}

// Function to open the editor
void OpenSmartMaskEditor(MaskEffectNode* node, GLuint clip_texture_for_background, const DecodedFrame& decoded_frame_for_cv) {
    if (!node) return;
    current_editing_smart_mask_node = node;
    smart_mask_editor_bg_clip_tex = clip_texture_for_background;
    smart_mask_current_decoded_frame = decoded_frame_for_cv; // Make a copy for OpenCV processing

    // Reset ROI and existing overlay
    smart_mask_roi_start_pos = ImVec2(-1, -1);
    smart_mask_roi_end_pos = ImVec2(-1, -1);
    smart_mask_drawing_roi = false;
    if (smart_mask_editor_overlay_tex != 0) {
        glDeleteTextures(1, &smart_mask_editor_overlay_tex);
        smart_mask_editor_overlay_tex = 0;
    }

    if (smart_mask_editor_scribble_display_tex != 0) {
        glDeleteTextures(1, &smart_mask_editor_scribble_display_tex);
        smart_mask_editor_scribble_display_tex = 0;
    }
    smart_mask_bgd_model.release();
    smart_mask_fgd_model.release();
    last_grabcut_mask_cv.release();
    smart_mask_is_initialized = false;

    /* smart_mask_editor_scribble_hints_cv.release(); // Clear OpenCV Mat
    smart_mask_current_tool = SmartMaskEditTool::ROI_RECT; // Default tool
    // Optionally, if the node already has a current_smart_mask_texture,
    // you could load it into smart_mask_editor_overlay_tex here as a starting point. */

    smart_mask_editor_open = true;
}

void UpdateScribbleDisplayTexture() {
    if (smart_mask_editor_scribble_hints_cv.empty()) return;

    if (smart_mask_editor_scribble_display_tex == 0) {
        glGenTextures(1, &smart_mask_editor_scribble_display_tex);
    }
    glBindTexture(GL_TEXTURE_2D, smart_mask_editor_scribble_display_tex);

    cv::Mat display_scribbles_rgb(smart_mask_editor_scribble_hints_cv.rows, smart_mask_editor_scribble_hints_cv.cols, CV_8UC3);
    for (int r = 0; r < display_scribbles_rgb.rows; ++r) {
        for (int c = 0; c < display_scribbles_rgb.cols; ++c) {
            uchar hint_val = smart_mask_editor_scribble_hints_cv.at<uchar>(r,c);
            cv::Vec3b& S_pixel = display_scribbles_rgb.at<cv::Vec3b>(r,c);
            if (hint_val == cv::GC_FGD) S_pixel = cv::Vec3b(0, 255, 0);   // Green for FG
            else if (hint_val == cv::GC_BGD) S_pixel = cv::Vec3b(0, 0, 255);    // Blue for BG
            else S_pixel = cv::Vec3b(0,0,0); // Others (probable) as black for this temp display
        }
    }
    // cv::flip(display_scribbles_rgb, display_scribbles_rgb, 0); // If needed for GL upload convention

    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, display_scribbles_rgb.cols, display_scribbles_rgb.rows, 0, GL_BGR, GL_UNSIGNED_BYTE, display_scribbles_rgb.data);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST); // Use NEAREST for crisp scribbles
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glBindTexture(GL_TEXTURE_2D, 0);
}

void DrawSmartMaskEditorWindow() {
    if (!smart_mask_editor_open || !current_editing_smart_mask_node) {
        smart_mask_editor_open = false; // Ensure it closes if node is bad
        if (smart_mask_editor_overlay_tex != 0) { // Cleanup
             glDeleteTextures(1, &smart_mask_editor_overlay_tex);
             smart_mask_editor_overlay_tex = 0;
        }
        return;
    }

    ImGui::SetNextWindowSize(ImVec2(700, 800), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Smart Mask Editor (Interactive)", &smart_mask_editor_open)) {
        ImGui::Text("Editing Smart Mask for: %s", current_editing_smart_mask_node->name.c_str());
        ImGui::Text("Paint inside the subject to add to the selection.");
        ImGui::Text("Hold ALT and paint to remove from the selection.");
        ImGui::Text("Hold CTRL and drag to resize the brush.");
        ImGui::Separator();

        // --- Display Area for Clip Frame and ROI drawing ---
        ImVec2 canvas_avail_size = ImGui::GetContentRegionAvail();
        canvas_avail_size.y -= (ImGui::GetTextLineHeightWithSpacing() * 3); // Space for buttons

        float frame_aspect = (smart_mask_current_decoded_frame.width > 0 && smart_mask_current_decoded_frame.height > 0)
                             ? (float)smart_mask_current_decoded_frame.width / smart_mask_current_decoded_frame.height
                             : 16.0f / 9.0f;

        ImVec2 display_size = canvas_avail_size;
        if (canvas_avail_size.x / frame_aspect <= canvas_avail_size.y) {
            display_size.y = canvas_avail_size.x / frame_aspect;
        } else {
            display_size.x = canvas_avail_size.y * frame_aspect;
        }
        display_size.x = std::max(50.0f, display_size.x);
        display_size.y = std::max(50.0f, display_size.y);
        
        ImVec2 canvas_p0 = ImGui::GetCursorScreenPos(); // Top-left of the drawing area
        ImVec2 canvas_p1 = ImVec2(canvas_p0.x + display_size.x, canvas_p0.y + display_size.y);
        ImDrawList* draw_list = ImGui::GetWindowDrawList();

        // Display background clip frame
        if (smart_mask_editor_bg_clip_tex != 0) {
            // UVs (0,0) to (1,1) for ImGui::Image usually assumes texture is top-left origin.
            // If your video textures are standard GL (bottom-left), flip Y.
            draw_list->AddImage((ImTextureID)(intptr_t)smart_mask_editor_bg_clip_tex, canvas_p0, canvas_p1, ImVec2(0,0), ImVec2(1,1));
        } else {
            draw_list->AddRectFilled(canvas_p0, canvas_p1, IM_COL32(30,30,30,255));
            ImGui::SetCursorScreenPos(ImVec2(canvas_p0.x + 5, canvas_p0.y + 5));
            ImGui::Text("Background frame unavailable");
        }
        
        // 2. OPTIMIZATION: Draw contour overlay instead of semi-transparent mask
        if (!last_grabcut_mask_cv.empty()) {
            std::vector<std::vector<cv::Point>> contours;
            cv::Mat contour_input = last_grabcut_mask_cv.clone();
            // Find contours on the binary mask (where pixel == 1 or 3)
            cv::threshold(contour_input, contour_input, 2, 255, cv::THRESH_BINARY);
            contour_input.convertTo(contour_input, CV_8U);
            cv::findContours(contour_input, contours, cv::RETR_EXTERNAL, cv::CHAIN_APPROX_SIMPLE);

            for (const auto& contour : contours) {
                for (size_t i = 0; i < contour.size(); ++i) {
                    cv::Point p1 = contour[i];
                    cv::Point p2 = contour[(i + 1) % contour.size()];

                    // Convert CV points to ImGui screen coordinates
                    ImVec2 im_p1 = ImVec2(
                        canvas_p0.x + ((float)p1.x / smart_mask_current_decoded_frame.width) * display_size.x,
                        canvas_p0.y + ((float)p1.y / smart_mask_current_decoded_frame.height) * display_size.y
                    );
                    ImVec2 im_p2 = ImVec2(
                        canvas_p0.x + ((float)p2.x / smart_mask_current_decoded_frame.width) * display_size.x,
                        canvas_p0.y + ((float)p2.y / smart_mask_current_decoded_frame.height) * display_size.y
                    );
                    draw_list->AddLine(im_p1, im_p2, IM_COL32(137, 255, 0, 255), 2.0f);
                }
            }
        }

        ImGui::SetCursorScreenPos(canvas_p0);
        ImGui::InvisibleButton("##smart_mask_canvas", display_size);
        bool is_hovered = ImGui::IsItemHovered();

        // --- Core Interactive Logic ---
        if (is_hovered) {
            bool is_alt_down = ImGui::IsKeyDown(ImGuiKey_LeftAlt) || ImGui::IsKeyDown(ImGuiKey_RightAlt);
            bool is_ctrl_down = ImGui::IsKeyDown(ImGuiKey_LeftCtrl) || ImGui::IsKeyDown(ImGuiKey_RightCtrl);
            ImVec2 mouse_pos = ImGui::GetMousePos();

            // A. Brush resizing
            if (is_ctrl_down && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                smart_mask_brush_size_px += ImGui::GetIO().MouseDelta.x;
                smart_mask_brush_size_px = std::max(1.0f, std::min(500.0f, smart_mask_brush_size_px));
            }
            // B. Painting
            else if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                ImVec2 mouse_pos_on_canvas = ImVec2(mouse_pos.x - canvas_p0.x, mouse_pos.y - canvas_p0.y);
                int img_px = static_cast<int>((mouse_pos_on_canvas.x / display_size.x) * smart_mask_current_decoded_frame.width);
                int img_py = static_cast<int>((mouse_pos_on_canvas.y / display_size.y) * smart_mask_current_decoded_frame.height);

                if (img_px >= 0 && img_px < smart_mask_current_decoded_frame.width && img_py >= 0 && img_py < smart_mask_current_decoded_frame.height) {
                    if (smart_mask_editor_scribble_hints_cv.empty()) {
                        smart_mask_editor_scribble_hints_cv.create(smart_mask_current_decoded_frame.height, smart_mask_current_decoded_frame.width, CV_8UC1);
                        smart_mask_editor_scribble_hints_cv.setTo(cv::GC_BGD); // Probable background
                    }
                    uchar paint_value = is_alt_down ? cv::GC_BGD : cv::GC_FGD; // Background or Foreground
                    cv::circle(smart_mask_editor_scribble_hints_cv, cv::Point(img_px, img_py), static_cast<int>(smart_mask_brush_size_px / 2.0f), cv::Scalar(paint_value), -1);
                }
            }

            // C. Run Analysis on Mouse Release
            if (ImGui::IsMouseReleased(ImGuiMouseButton_Left) && !is_ctrl_down) {
                if (!smart_mask_editor_scribble_hints_cv.empty()) {
                    GLuint generated_tex_id = 0;
                    if (!smart_mask_is_initialized) {
                        // First stroke: initialize with a rect around the scribbles
                        cv::Rect roi = cv::boundingRect(smart_mask_editor_scribble_hints_cv);
                        generated_tex_id = current_editing_smart_mask_node->RunGrabCut(smart_mask_current_decoded_frame, MaskEffectNode::GrabCutInitMode::RECT, roi, cv::Mat(), smart_mask_bgd_model, smart_mask_fgd_model, false);
                        smart_mask_is_initialized = true;
                    } else {
                        // Subsequent strokes: refine using the scribbles
                        generated_tex_id = current_editing_smart_mask_node->RunGrabCut(smart_mask_current_decoded_frame, MaskEffectNode::GrabCutInitMode::MASK, cv::Rect(), smart_mask_editor_scribble_hints_cv, smart_mask_bgd_model, smart_mask_fgd_model, true);
                    }
                    
                    // Update our overlay and clear scribbles for the next stroke
                    if (smart_mask_editor_overlay_tex != 0) glDeleteTextures(1, &smart_mask_editor_overlay_tex);
                    smart_mask_editor_overlay_tex = generated_tex_id;
                    smart_mask_editor_scribble_hints_cv.setTo(cv::GC_PR_BGD); // Reset for next refinement
                }
            }

            // D. Draw Live Brush Cursor
            draw_list->AddCircle(mouse_pos, smart_mask_brush_size_px / 2.0f, IM_COL32(255, 255, 255, 200), 0, 1.5f);
            if (is_alt_down) {
                draw_list->AddLine(ImVec2(mouse_pos.x - 5, mouse_pos.y), ImVec2(mouse_pos.x + 5, mouse_pos.y), IM_COL32(255, 255, 255, 255), 2.0f);
            } else {
                draw_list->AddLine(ImVec2(mouse_pos.x - 5, mouse_pos.y), ImVec2(mouse_pos.x + 5, mouse_pos.y), IM_COL32(255, 255, 255, 255), 2.0f);
                draw_list->AddLine(ImVec2(mouse_pos.x, mouse_pos.y - 5), ImVec2(mouse_pos.x, mouse_pos.y + 5), IM_COL32(255, 255, 255, 255), 2.0f);
            }
        }

        // --- Buttons are now simpler ---
        ImGui::SetCursorPosY(canvas_p1.y + 10);
        if (ImGui::Button("Apply Smart Mask")) {
            if (smart_mask_editor_overlay_tex != 0 && current_editing_smart_mask_node) {
                // 1. Delete any existing smart_interactive_mask_texture on the node
                if (current_editing_smart_mask_node->smart_interactive_mask_texture != 0) {
                    glDeleteTextures(1, &current_editing_smart_mask_node->smart_interactive_mask_texture);
                }

                // 2. Transfer ownership of the texture from the editor's overlay to the node
                current_editing_smart_mask_node->smart_interactive_mask_texture = smart_mask_editor_overlay_tex;
                smart_mask_editor_overlay_tex = 0; // Editor no longer owns it

                // 3. Set the mask type on the node
                current_editing_smart_mask_node->mask_type = MaskEffectNode::MaskType::Smart_Interactive;
                
                // 4. Store the ROI used, if you want to recall it later (optional)
                // current_editing_smart_mask_node->grabcut_roi_rect = ... (convert normalized back to node's frame pixels if needed)

                // layers_changed = true; // Signal main preview to update
                
                tinyfd_messageBox("Success", "Smart mask applied to effect.", "ok", "info", 1);
                smart_mask_editor_open = false; // Close editor on apply
            } else {
                tinyfd_messageBox("Warning", "No smart mask generated/visible in overlay to apply. Analyze frame first.", "ok", "warning", 1);
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Clear Hints")) {
            smart_mask_roi_start_pos = ImVec2(-1,-1);
            smart_mask_drawing_roi = false;
            smart_mask_editor_scribble_hints_cv.release();
            if (smart_mask_editor_scribble_display_tex != 0) {
                glDeleteTextures(1, &smart_mask_editor_scribble_display_tex);
                smart_mask_editor_scribble_display_tex = 0;
            }
            if (smart_mask_editor_overlay_tex != 0) { // Also clear analysis overlay
                glDeleteTextures(1, &smart_mask_editor_overlay_tex);
                smart_mask_editor_overlay_tex = 0;
            }
            smart_mask_bgd_model.release();
            smart_mask_fgd_model.release();
            smart_mask_is_initialized = false;
        }

    }
    ImGui::End();

    if (!smart_mask_editor_open) { // Cleanup if window was closed
        if (smart_mask_editor_overlay_tex != 0) { // If editor closed with an unapplied overlay
             glDeleteTextures(1, &smart_mask_editor_overlay_tex);
             smart_mask_editor_overlay_tex = 0;
        }
        if (smart_mask_editor_scribble_display_tex != 0) {
            glDeleteTextures(1, &smart_mask_editor_scribble_display_tex);
            smart_mask_editor_scribble_display_tex = 0;
        }
        smart_mask_editor_scribble_hints_cv.release();
        smart_mask_bgd_model.release();
        smart_mask_fgd_model.release();
        last_grabcut_mask_cv.release();
        smart_mask_is_initialized = false;
    }
}