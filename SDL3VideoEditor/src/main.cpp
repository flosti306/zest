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
    #include <tinyfiledialogs.h>
}
#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_sdlrenderer3.h>
#include <imgui_impl_opengl3.h>
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
#include "project_io.hpp"

/* TODO

Fix Layer export
Fix whitespace export
Add Frames for the timing
Preview
Zoom (resizing)
Docking
UI


*/

struct GradientData {
    ImVec4 window_grad_top;
    ImVec4 window_grad_bottom;
};

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

// === Playback state ===
bool playing = false;
float playhead_time = 0.0f;
Uint64 last_playback_time = SDL_GetTicksNS();

int preview_width = 1280;
int preview_height = 720;

Clip* selected_clip = nullptr;

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
    float& playhead_time,
    float& max_duration,
    float& zoom_factor,
    bool& layers_changed
);

void UpdatePreview(GLResources& res, const std::vector<Clip>& clips, int width, int height, float playhead_time);

void RenderPreviewWindow(GLuint preview_tex, int preview_width, int preview_height);


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
    colors[ImGuiCol_MenuBarBg]             = ImVec4(0.18f, 0.18f, 0.21f, 1.00f);
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
    // We'll store them as floats in ImGui's UserData (if you have space available)
    ImGui::GetIO().UserData = IM_NEW(GradientData)();
    GradientData* gradient_data = (GradientData*)ImGui::GetIO().UserData;
    gradient_data->window_grad_top = bg_light;
    gradient_data->window_grad_bottom = bg_dark;
}

// Create a gradient texture once
GLuint CreateGradientTexture(ImVec4 top_color, ImVec4 bottom_color, int height = 1280) {
    GLuint texture_id;
    glGenTextures(1, &texture_id);
    glBindTexture(GL_TEXTURE_2D, texture_id);
    
    // Create a 2D texture with dithering
    const int width = 32; // Wider for better dithering pattern
    unsigned char* data = new unsigned char[width * height * 4];
    
    // Bayer dithering matrix 8x8
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
    
    // Dithering strength - adjust as needed
    const float dither_strength = 1.0f/255.0f * 1.5f;
    
    for (int y = 0; y < height; y++) {
        float t = (float)y / (float)(height - 1);
        // Improved easing function
        float eased_t = t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f);
        
        // Blend colors in linear space for perceptual correctness
        ImVec4 col_mix;
        col_mix.x = powf(powf(bottom_color.x, 2.2f) * (1.0f - eased_t) + powf(top_color.x, 2.2f) * eased_t, 1.0f/2.2f);
        col_mix.y = powf(powf(bottom_color.y, 2.2f) * (1.0f - eased_t) + powf(top_color.y, 2.2f) * eased_t, 1.0f/2.2f);
        col_mix.z = powf(powf(bottom_color.z, 2.2f) * (1.0f - eased_t) + powf(top_color.z, 2.2f) * eased_t, 1.0f/2.2f);
        col_mix.w = bottom_color.w * (1.0f - eased_t) + top_color.w * eased_t;
        
        for (int x = 0; x < width; x++) {
            // Apply dithering pattern
            int pattern_x = x % 8;
            int pattern_y = y % 8;
            float dither_value = bayer8x8[pattern_y * 8 + pattern_x];
            
            // Apply dithering to each component
            float r = col_mix.x + (dither_value - 0.5f) * dither_strength;
            float g = col_mix.y + (dither_value - 0.5f) * dither_strength;
            float b = col_mix.z + (dither_value - 0.5f) * dither_strength;
            
            // Clamp values
            r = r < 0.0f ? 0.0f : (r > 1.0f ? 1.0f : r);
            g = g < 0.0f ? 0.0f : (g > 1.0f ? 1.0f : g);
            b = b < 0.0f ? 0.0f : (b > 1.0f ? 1.0f : b);
            
            int idx = (y * width + x) * 4;
            data[idx + 0] = (unsigned char)(r * 255.0f);
            data[idx + 1] = (unsigned char)(g * 255.0f);
            data[idx + 2] = (unsigned char)(b * 255.0f);
            data[idx + 3] = (unsigned char)(col_mix.w * 255.0f);
        }
    }
    
    // Upload to texture with proper dimensions
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, data);
    
    // Use trilinear filtering
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    
    // Generate mipmaps
    glGenerateMipmap(GL_TEXTURE_2D);
    
    delete[] data;
    return texture_id;
}

void ApplyWindowBackgroundGradients() {
    static GLuint gradient_texture = 0;
    static ImVec4 last_top_color = ImVec4(0,0,0,0);
    static ImVec4 last_bottom_color = ImVec4(0,0,0,0);
    
    GradientData* gradient_data = (GradientData*)ImGui::GetIO().UserData;
    if (!gradient_data)
        return;
    
    // Recreate texture if colors have changed
    if (gradient_texture == 0 || 
        memcmp(&last_top_color, &gradient_data->window_grad_top, sizeof(ImVec4)) != 0 ||
        memcmp(&last_bottom_color, &gradient_data->window_grad_bottom, sizeof(ImVec4)) != 0) {
        
        if (gradient_texture != 0)
            glDeleteTextures(1, &gradient_texture);
            
        gradient_texture = CreateGradientTexture(gradient_data->window_grad_top, gradient_data->window_grad_bottom);
        last_top_color = gradient_data->window_grad_top;
        last_bottom_color = gradient_data->window_grad_bottom;
    }
    
    // Now draw the texture
    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    if (!draw_list || gradient_texture == 0)
        return;
    
    ImVec2 window_pos = ImGui::GetWindowPos();
    ImVec2 window_size = ImGui::GetWindowSize();
    
    // Convert OpenGL texture to ImGui texture ID
    ImTextureID tex_id = (ImTextureID)(intptr_t)gradient_texture;
    
    // Draw the texture stretched to cover the window
    draw_list->AddImage(
        tex_id,
        window_pos,
        ImVec2(window_pos.x + window_size.x, window_pos.y + window_size.y),
        ImVec2(0, 0),
        ImVec2(1, 1)
    );
    
    // Add a subtle highlight at the top (optional)
    draw_list->AddRectFilled(
        ImVec2(window_pos.x, window_pos.y),
        ImVec2(window_pos.x + window_size.x, window_pos.y + 1.5f),
        IM_COL32(255, 255, 255, 15)
    );
}



void AddNewClip(std::vector<Clip>& clips, const std::string& input_path, float video_duration, int layer = 0) {
    // Reserve space for 2 potential new clips
    clips.reserve(clips.size() + 2);
    
    std::string base_name = std::filesystem::path(input_path).filename().string();
    
    // Create video clip
    size_t video_index = clips.size();
    clips.emplace_back();
    Clip& video_clip = clips[video_index]; // Get reference that won't change
    
    video_clip.name = base_name + " [Video]";
    video_clip.path = input_path;
    video_clip.type = ClipType::Video;
    video_clip.start_time = 0.0f;
    video_clip.duration = video_duration;
    video_clip.layer = layer;
    
    // Attempt to load audio
    PreloadedAudio audio;
    if (preload_audio_file(input_path, audio)) {
        size_t audio_index = clips.size();
        clips.emplace_back();
        Clip& audio_clip = clips[audio_index]; // Get reference that won't change
        
        audio_clip.name = base_name + " [Audio]";
        audio_clip.path = input_path;
        audio_clip.type = ClipType::Audio;
        audio_clip.start_time = 0.0f;
        audio_clip.duration = video_duration;
        audio_clip.layer = layer + 1;
        audio_clip.waveform = std::move(audio.waveform);
        
        // Link both ways using pointers that are now safe (no reallocation will happen)
        audio_clip.linked_clip = &clips[video_index];
        video_clip.linked_clip = &clips[audio_index];
    }
}


// Modified get_video_duration using FFmpeg
float get_video_duration(const std::string& input_path) {
    AVFormatContext* fmt_ctx = nullptr;
    if (avformat_open_input(&fmt_ctx, input_path.c_str(), nullptr, nullptr) != 0) {
        return -1;
    }
    
    if (avformat_find_stream_info(fmt_ctx, nullptr) < 0) {
        avformat_close_input(&fmt_ctx);
        return -1;
    }
    
    float duration = static_cast<float>(fmt_ctx->duration / AV_TIME_BASE);
    avformat_close_input(&fmt_ctx);
    return duration;
}


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

void RenderDockSpace() {
    static ImGuiDockNodeFlags dockspace_flags = ImGuiDockNodeFlags_None;

    ImGuiWindowFlags window_flags = ImGuiWindowFlags_MenuBar | ImGuiWindowFlags_NoDocking;
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    ImGui::SetNextWindowViewport(viewport->ID);

    window_flags |= ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse |
                    ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove;
    window_flags |= ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);

    ImGui::Begin("DockSpace Demo", nullptr, window_flags);
    ImGui::PopStyleVar(2);

    ImGuiID dockspace_id = ImGui::GetID("MyDockSpace");
    ImGui::DockSpace(dockspace_id, ImVec2(0.0f, 0.0f), dockspace_flags);

    ImGui::End();
}


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

    // === State ===
    char input_path[256] = "";
    char output_path[256] = "output.mp4";
    float start_time = 0.0f, duration = 10.0f, max_duration = 0.0f;
    float zoom_factor = 1.0f;
    float video_duration = 0.0f;
    bool file_dropped = false;
    bool process_success = false;
    std::string process_message;

    std::vector<Clip> clips;
    int playhead_position = 0;
    int last_playhead_position = -1;
    static bool layers_changed = false;

    /* bool playing = false;
    float playhead_time = 0.0f;
    Uint64 last_playback_time = SDL_GetTicksNS();
 */
    GLResources gl_resources;
    setup_gl_resources(gl_resources, preview_width, preview_height);

    // === Main Loop ===
    bool running = true;
    while (running) {
        SDL_Event event;

        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_EVENT_QUIT)
                running = false;

            if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_SPACE && event.key.down) {
                playing = !playing;
                std::cout << "Playback " << (playing ? "started" : "paused") << "\n";
            }

            if (event.type == SDL_EVENT_DROP_FILE) {
                if (event.drop.data) {
                    strncpy(input_path, event.drop.data, IM_ARRAYSIZE(input_path) - 1);
                    file_dropped = true;

                    video_duration = get_video_duration(input_path);
                    process_message = (video_duration == -1)
                        ? "Failed to determine video duration!"
                        : "Video duration loaded successfully!";

                    std::cout << "Input Path: " << input_path << "\n";
                    std::cout << "Video Duration: " << video_duration << "\n";

                    AddNewClip(clips, input_path, video_duration);
                    load_textures(gl_resources, clips);
                    UpdatePreview(gl_resources, clips, preview_width, preview_height, playhead_time);
                    
                }
            }

            if (event.type == SDL_EVENT_KEY_DOWN &&
                event.key.key == SDL_Keycode{'b'} &&
                (event.key.mod & SDL_KMOD_CTRL) &&
                selected_clip) {
            
                float cut_time = playhead_time;
            
                // Check if the cut is within clip bounds
                float clip_start_global = selected_clip->start_time;
                float clip_end_global = selected_clip->start_time + selected_clip->duration;
                if (cut_time <= clip_start_global || cut_time >= clip_end_global) {
                    std::cout << "Cut is outside clip bounds: " << cut_time << "\n";
                } else {
                    float relative_cut = cut_time - selected_clip->start_time;
                    float new_media_start = selected_clip->media_start + relative_cut;
            
                    // First clip: update duration
                    selected_clip->duration = relative_cut;
            
                    // Second clip: create from remaining duration
                    Clip new_clip = *selected_clip;
                    new_clip.start_time = cut_time;
                    new_clip.media_start = new_media_start;
                    new_clip.duration = clip_end_global - cut_time;
            
                    new_clip.name += " (cut)";
                    clips.push_back(new_clip);
            
                    std::cout << "Blade tool: split clip at " << cut_time << "s\n";
                }
            }
            
            

            ImGui_ImplSDL3_ProcessEvent(&event);
        }

        if (playing) {
            Uint64 now = SDL_GetTicksNS();
            float delta_time = (now - last_playback_time) / 1'000'000'000.0f;
            last_playback_time = now;
            playhead_time += delta_time;
            load_textures(gl_resources, clips);
            UpdatePreview(gl_resources, clips, preview_width, preview_height, playhead_time);
        } else {
            last_playback_time = SDL_GetTicksNS();
        }

        // === ImGui Frame ===
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();   
        
        RenderDockSpace();

        ImGui::Begin("FFmpeg Video Editor");
        ApplyWindowBackgroundGradients();
        ImGui::Text("Drag and drop a video file or enter the file path:");
        ImGui::InputText("Input Path", input_path, IM_ARRAYSIZE(input_path));
        ImGui::Text("Set output file path:");
        ImGui::InputText("Output Path", output_path, IM_ARRAYSIZE(output_path));

        DrawTimelineEditor(clips, playhead_time, max_duration, zoom_factor, layers_changed);

        if (playhead_position != last_playhead_position || layers_changed) {
            UpdatePreview(gl_resources, clips, preview_width, preview_height, playhead_time);
            last_playhead_position = playhead_position;
            layers_changed = false;
        }

        RenderPreviewWindow(gl_resources.render_tex, preview_width, preview_height);
        ImGui::Separator();

        if (ImGui::Button("Cut Video")) {
            SDL_GL_MakeCurrent(window, gl_context);

            if (!SDL_GL_GetCurrentContext()) {
                std::cerr << "OpenGL context is not active!\n";
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
                        process_success = start_video_export(output_path, 1920, 1080, fps, total_frames_needed, clips, window);
                        process_message = process_success ? "Video cut successfully!" : "Failed to cut video!";
                    }
                } else {
                    process_message = "Input file does not exist!";
                }
            }
        }

        ImGui::TextWrapped("Status: %s", process_message.c_str());

        ImGui::Separator();

        if (ImGui::Button("Save Project")) {
            const char* filters[] = { "*.zest" };
            const char* save_path = tinyfd_saveFileDialog(
                "Save Project", "project.zest", 1, filters, "Zest Project Files (*.zest)"
            );

            if (save_path && SaveProject(save_path, clips, playhead_time, zoom_factor)) {
                process_message = "Project saved!";
                std::cout << "Project saved to: " << save_path << "\n";
            } else if (save_path) {
                std::cerr << "Failed to save project\n";
                process_message = "Failed to save project!";
            }
        }
        ImGui::SameLine();

        if (ImGui::Button("Load Project")) {
            const char* filters[] = { "*.zest" };
            const char* load_path = tinyfd_openFileDialog(
                "Load Project", "", 1, filters, "Zest Project Files (*.zest)", 0
            );

            if (load_path && LoadProject(load_path, clips, playhead_time, zoom_factor)) {
                load_textures(gl_resources, clips);
                UpdatePreview(gl_resources, clips, preview_width, preview_height, playhead_time);
                layers_changed = true;
                process_message = "Project loaded!";
                std::cout << "Project loaded from: " << load_path << "\n";
            } else if (load_path) {
                std::cerr << "Failed to load project\n";
                process_message = "Failed to load project!";
            }
        }



        ImGui::End();

        ImGui::Begin("Timeline");
        ApplyWindowBackgroundGradients();
        ImGui::Text("Playhead: %.2f sec", playhead_time);
        ImGui::SliderFloat("Seek", &playhead_time, 0.0f, max_duration); // replace 60 with actual duration if needed
        ImGui::End();

        ImGui::Begin("Inspector");
        ApplyWindowBackgroundGradients();

        if (selected_clip) {
            ImGui::Text("Selected Clip: %s", selected_clip->name.c_str());

            ImGui::SeparatorText("Transform");

            ImGui::SliderFloat("Position X", &selected_clip->pos_x, -1.0f, 1.0f);
            ImGui::SliderFloat("Position Y", &selected_clip->pos_y, -1.0f, 1.0f);
            ImGui::SliderFloat("Scale",      &selected_clip->scale,  0.1f, 4.0f);
            ImGui::SliderFloat("Opacity",    &selected_clip->opacity, 0.0f, 1.0f);

            ImGui::SeparatorText("Trimming");

            ImGui::InputFloat("Trim Start",  &selected_clip->media_start, 0.1f);
            ImGui::InputFloat("Duration",      &selected_clip->duration);

            ImGui::SeparatorText("Layering");

            ImGui::InputInt("Layer", &selected_clip->layer);
        } else {
            ImGui::Text("No clip selected.");
        }

        ImGui::End();


        ImGui::Render();

        // === OpenGL render ===
        int w, h;
        SDL_GetWindowSize(window, &w, &h);
        glViewport(0, 0, w, h);
        glClearColor(0.12f, 0.12f, 0.12f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        SDL_GL_SwapWindow(window);

        if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable) {
            SDL_Window* backup_current_window = SDL_GL_GetCurrentWindow();
            SDL_GLContext backup_context = SDL_GL_GetCurrentContext();
        
            ImGui::UpdatePlatformWindows();
            ImGui::RenderPlatformWindowsDefault();
        
            SDL_GL_MakeCurrent(backup_current_window, backup_context);
        }
    }

    // Cleanup
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    SDL_GL_DestroyContext(gl_context);
    SDL_DestroyWindow(window);
    SDL_Quit();

    return 0;
}



void DrawTimelineEditor(
    std::vector<Clip>& clips,
    float& playhead_time,
    float& max_duration,
    float& zoom_factor,
    bool& layers_changed
) {
    ImGui::Begin("Timeline Editor");
    ApplyWindowBackgroundGradients();

    const float timeline_width = ImGui::GetContentRegionAvail().x;
    const float layer_height = 60.0f;
    const float layer_padding = 10.0f;

    // Determine the number of layers
    int max_layer = 0;
    for (const auto& clip : clips) {
        max_layer = std::max(max_layer, clip.layer);
    }
    max_layer += 1;

    float timeline_height = max_layer * (layer_height + layer_padding);

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

    // Draw timeline background
    ImVec2 timeline_start = ImGui::GetCursorScreenPos();
    ImVec2 timeline_end = ImVec2(timeline_start.x + timeline_width, timeline_start.y + timeline_height);
    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    draw_list->AddRectFilled(timeline_start, timeline_end, IM_COL32(100, 100, 100, 255));

    // Layer lines
    for (int layer = 0; layer < max_layer; ++layer) {
        float y = timeline_start.y + layer * (layer_height + layer_padding);
        draw_list->AddLine(ImVec2(timeline_start.x, y), ImVec2(timeline_end.x, y), IM_COL32(255, 255, 255, 255));
    }

    // === Draw Ruler Overlay ===
    const float tick_major_height = timeline_height;
    const float tick_minor_height = timeline_height;
    const float label_y = timeline_start.y - 18.0f; // above timeline
    const float tick_alpha_major = 90;
    const float tick_alpha_minor = 40;
    const float label_alpha = 160;

    int major_step = 1;
    while (pixels_per_second * major_step < 80.0f)
        major_step *= 2;

    int num_ticks = int(project_duration) + 4;
    for (int t = 0; t < num_ticks; ++t) {
        float x = timeline_start.x + t * pixels_per_second;

        if (x < timeline_start.x - 100 || x > timeline_end.x + 100)
            continue;

        bool is_major = (t % major_step == 0);
        ImU32 color = IM_COL32(255, 255, 255, is_major ? tick_alpha_major : tick_alpha_minor);

        float tick_height = is_major ? tick_major_height : tick_minor_height;

        draw_list->AddLine(
            ImVec2(x, timeline_start.y),
            ImVec2(x, timeline_start.y + tick_height),
            color
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

        float layer_offset_y = clip.layer * (layer_height + layer_padding);
        float clip_start_x = timeline_start.x + (clip.start_time * pixels_per_second);
        float clip_end_x = clip_start_x + (clip.duration * pixels_per_second);

        ImVec2 clip_rect_min = ImVec2(clip_start_x, timeline_start.y + layer_offset_y + layer_padding);
        ImVec2 clip_rect_max = ImVec2(clip_end_x, timeline_start.y + layer_offset_y + layer_height);

        // Draw clip body
        ImU32 fill_color = (clip.type == ClipType::Audio) ? IM_COL32(80, 120, 200, 255) : IM_COL32(255, 100, 100, 255);
        draw_list->AddRectFilled(clip_rect_min, clip_rect_max, fill_color);
        draw_list->AddRect(clip_rect_min, clip_rect_max, IM_COL32(255, 255, 255, 255));

        // If audio clip, draw waveform
        if (clip.type == ClipType::Audio && !clip.waveform.empty()) {
            int waveform_samples = clip.waveform.size();
            for (int s = 0; s < waveform_samples; ++s) {
                float amp = clip.waveform[s];
                float norm = float(s) / waveform_samples;
                float x = clip_rect_min.x + norm * (clip_rect_max.x - clip_rect_min.x);
                float center_y = (clip_rect_min.y + clip_rect_max.y) / 2.0f;
                float height = (clip_rect_max.y - clip_rect_min.y) * amp * 0.8f;
                draw_list->AddLine(ImVec2(x, center_y - height), ImVec2(x, center_y + height), IM_COL32(255, 200, 200, 255));
            }
        }

        

        if (&clip == selected_clip) {
            draw_list->AddRect(clip_rect_min, clip_rect_max, IM_COL32(255, 171, 64, 255), 0.0f, 0, 3.0f);
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

        // Context menu for linking
        if (ImGui::BeginPopupContextItem(("clip_context" + std::to_string(i)).c_str())) {
            if (clip.linked_clip && ImGui::MenuItem("Unlink Audio/Video")) {
                clip.linked_clip->linked_clip = nullptr;
                clip.linked_clip = nullptr;
            }
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

    // Dragging the playhead
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

    // Clicking anywhere to move playhead
    if (!clicked_on_clip && ImGui::IsWindowHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        ImVec2 mouse = ImGui::GetMousePos();
        if (mouse.y > timeline_start.y && mouse.y < timeline_end.y) {
            playhead_time = (mouse.x - timeline_start.x) / pixels_per_second;
            playhead_time = std::clamp(playhead_time, 0.0f, max_duration);
        }
    }

    ImGui::End();

    ImGui::Begin("Active Clips");
    ApplyWindowBackgroundGradients();
    ImGui::Text("Clip List:");
    ImGui::Separator();

    for (size_t i = 0; i < clips.size(); ++i) {
        auto& clip = clips[i];
        if (&clip == selected_clip)
            ImGui::TextColored(ImVec4(1.00f, 0.56f, 0.15f, 1.00f), "->");
        ImGui::SameLine();
        ImGui::Text("%zu: %s (Start: %f, Duration: %f, Layer: %d)", i, clip.name.c_str(), clip.start_time, clip.duration, clip.layer);
        if (ImGui::Button(("Delete##" + std::to_string(i)).c_str())) {
            if (selected_clip == &clip) selected_clip = nullptr;
            clips.erase(clips.begin() + i);
            break;
        }
    }

    ImGui::End();
}



void UpdatePreview(GLResources& res, const std::vector<Clip>& clips, int width, int height, float playhead_time) {
    static float last_preview_time = -1.0f;

    if (std::abs(playhead_time - last_preview_time) < 0.01f) return;

    std::vector<Clip> sorted_clips = clips;
    std::sort(sorted_clips.begin(), sorted_clips.end(), [](const Clip& a, const Clip& b) {
        return a.layer < b.layer;
    });

    render_frame(res, playhead_time, sorted_clips, width, height);
    last_preview_time = playhead_time;
}


void RenderPreviewWindow(GLuint preview_tex, int preview_width, int preview_height) {
    ImGui::Begin("Video Preview");
    ApplyWindowBackgroundGradients();

    if (preview_tex) {
        // Maintain aspect ratio
        float aspect = static_cast<float>(preview_width) / preview_height;

        ImVec2 avail = ImGui::GetContentRegionAvail();
        ImVec2 preview_size = avail;

        // Fit to width, maintain aspect ratio
        preview_size.y = preview_size.x / aspect;

        // Clip if height too large
        if (preview_size.y > avail.y) {
            preview_size.y = avail.y;
            preview_size.x = preview_size.y * aspect;
        }

        // Flip vertically: use UVs (0,1) to (1,0)
        ImGui::Image(
            (ImTextureID)(intptr_t)preview_tex,
            preview_size,
            ImVec2(0, 1),  // <-- top-left
            ImVec2(1, 0)   // <-- bottom-right
        );
    } else {
        ImGui::Text("Preview unavailable");
    }

    ImGui::End();
}