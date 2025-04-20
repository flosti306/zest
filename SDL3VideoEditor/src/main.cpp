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

/* TODO

Fix Layer export
Fix whitespace export
Add Frames for the timing
Preview
Zoom (resizing)
Docking
UI


*/

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

void UpdatePreview(GLResources& res, const std::vector<Clip>& clips, int width, int height, float playhead_time);

void RenderPreviewWindow(GLuint preview_tex, int preview_width, int preview_height);


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

    ImGui::StyleColorsDark();
    io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeui.ttf", 18.0f);

    ImGui_ImplSDL3_InitForOpenGL(window, gl_context);
    ImGui_ImplOpenGL3_Init("#version 130"); // Or "#version 330 core" for modern GL

    SetupImGuiStyle();

    // === State ===
    char input_path[256] = "";
    char output_path[256] = "output.mp4";
    int start_time = 0, duration = 10, max_duration = 0;
    float zoom_factor = 1.0f;
    int video_duration = 0;
    bool file_dropped = false;
    bool process_success = false;
    std::string process_message;

    std::vector<Clip> clips;
    int playhead_position = 0;
    int last_playhead_position = -1;
    static bool layers_changed = false;

    bool playing = false;
    float playhead_time = 0.0f;
    Uint64 last_playback_time = SDL_GetTicksNS();

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

        ImGui::Begin("FFmpeg Video Editor");
        ImGui::Text("Drag and drop a video file or enter the file path:");
        ImGui::InputText("Input Path", input_path, IM_ARRAYSIZE(input_path));
        ImGui::Text("Set output file path:");
        ImGui::InputText("Output Path", output_path, IM_ARRAYSIZE(output_path));

        DrawTimelineEditor(clips, playhead_position, start_time, duration, max_duration, zoom_factor, last_playhead_position, layers_changed);

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
        ImGui::End();

        ImGui::Begin("Timeline");
        ImGui::Text("Playhead: %.2f sec", playhead_time);
        ImGui::SliderFloat("Seek", &playhead_time, 0.0f, 60.0f); // replace 60 with actual duration if needed
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