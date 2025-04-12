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

/* TODO

Fix Layer export
Fix whitespace export
Add Frames for the timing
Preview
Zoom (resizing)
Docking
UI


*/

struct Clip {
    std::string name;
    int start_time;
    int duration;
    int layer;
    std::string path;
};

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

void AddNewClip(std::vector<Clip>& clips, const std::string& input_path, int video_duration, int layer = 0) {
    std::string clip_name = std::filesystem::path(input_path).filename().string();
    clips.push_back(Clip{clip_name, 0, video_duration, layer, input_path});  // Default start at 0 and a default duration
}

int get_video_duration(const std::string& input_path) {
    std::stringstream command;
    command << "ffmpeg -i \"" << input_path << "\" 2>&1";

    FILE* pipe = popen(command.str().c_str(), "r");
    if (!pipe) return -1;

    char buffer[128];
    std::string result = "";
    while (fgets(buffer, sizeof(buffer), pipe) != NULL) {
        result += buffer;
    }
    pclose(pipe);

    // Use regex to extract the duration from FFmpeg's output
    std::regex duration_regex(R"(Duration:\s(\d+):(\d+):(\d+)\.\d+)");
    std::smatch match;
    if (std::regex_search(result, match, duration_regex)) {
        int hours = std::stoi(match[1].str());
        int minutes = std::stoi(match[2].str());
        int seconds = std::stoi(match[3].str());
        return hours * 3600 + minutes * 60 + seconds;
    }

    return -1; // Duration not found
}

bool execute_ffmpeg_cut(const std::vector<Clip>& clips, const std::string& output_path) {
    // Calculate total duration
    int total_duration = 0;
    for (const auto& clip : clips) {
        int end_time = clip.start_time + clip.duration;
        if (end_time > total_duration) {
            total_duration = end_time;
        }
    }

    // Generate base video (black background)
    std::string base_video = "base.mp4";
    std::stringstream base_command;
    base_command << "ffmpeg -y -f lavfi -i color=c=black:s=1920x1080:r=30:d=" << total_duration
                 << " -c:v libx264 -t " << total_duration << " " << base_video;
    if (system(base_command.str().c_str()) != 0) {
        std::cerr << "Failed to create base video." << std::endl;
        return false;
    }

    // Sort clips by layer (ascending) to overlay lower layers first
    std::vector<Clip> sorted_clips = clips;
    std::sort(sorted_clips.begin(), sorted_clips.end(), [](const Clip& a, const Clip& b) {
        return a.layer < b.layer;
    });

    // Process each clip into a temporary file
    std::vector<std::string> temp_files;
    for (size_t i = 0; i < sorted_clips.size(); ++i) {
        const auto& clip = sorted_clips[i];
        std::string temp_output = "temp_" + std::to_string(i) + ".mp4";
        std::stringstream clip_command;
        
        // Extract clip from source (assuming clip.duration is the source duration to use)
        clip_command << "ffmpeg -y -ss 0 -t " << clip.duration << " -i \"" << clip.path << "\" "
                     << "-c:v libx264 -c:a aac " << temp_output;
        
        if (system(clip_command.str().c_str()) != 0) {
            std::cerr << "Failed to process clip: " << clip.name << std::endl;
            return false;
        }
        temp_files.push_back(temp_output);
    }

    // Build filter_complex string
    std::stringstream filter_complex;
    std::vector<std::string> clip_streams;

    // Base video stream
    filter_complex << "[0:v]setpts=PTS-STARTPTS[base];";

    // Process each clip's video stream with PTS adjustment
    for (size_t i = 0; i < sorted_clips.size(); ++i) {
        const auto& clip = sorted_clips[i];
        filter_complex << "[" << (i + 1) << ":v]setpts=PTS-STARTPTS+" 
                      << clip.start_time << "/TB[clip" << i << "v];";
        clip_streams.push_back("[clip" + std::to_string(i) + "v]");
    }

    // Build overlay chain
    std::string overlay_chain = "[base]";
    for (size_t i = 0; i < sorted_clips.size(); ++i) {
        filter_complex << overlay_chain << clip_streams[i] 
                      << "overlay=eof_action=pass[ol" << i << "];";
        overlay_chain = "[ol" + std::to_string(i) + "]";
    }

    // Add final format conversion
    filter_complex << overlay_chain << "format=yuv420p[outv];";

    // Audio handling
    filter_complex << "";
    for (size_t i = 0; i < sorted_clips.size(); ++i) {
        filter_complex << "[" << (i + 1) << ":a]";
    }
    filter_complex << "amix=inputs=" << sorted_clips.size() 
                 << ":duration=longest[outa];";

    // Build the ffmpeg command
    std::stringstream overlay_command;
    overlay_command << "ffmpeg -y -i " << base_video << " ";
    for (const auto& temp_file : temp_files) {
        overlay_command << "-i " << temp_file << " ";
    }
    overlay_command << "-filter_complex \"" << filter_complex.str() << "\" "
                   << "-map \"[outv]\" -map \"[outa]\" "
                   << "-c:v libx264 -c:a aac -preset fast "
                   << output_path;

    std::cout << "##################### Executing: " << overlay_command.str() << std::endl;

    bool success = system(overlay_command.str().c_str()) == 0;

    // Cleanup temporary files
    remove(base_video.c_str());
    for (const auto& temp_file : temp_files) {
        remove(temp_file.c_str());
    }

    return success;
}

int main(int argc, char* argv[]) {

    SDL_Window *window;                    // Declare a pointer

    SDL_Init(SDL_INIT_VIDEO);              // Initialize SDL3

    // Create an application window with the following settings:
    window = SDL_CreateWindow(
        "An SDL3 window",                  // window title
        640,                               // width, in pixels
        480,                               // height, in pixels
        SDL_WINDOW_OPENGL | SDL_WINDOW_FULLSCREEN                 // flags - see below
    );

    // Check that the window was successfully created
    if (window == NULL) {
        // In the case that the window could not be made...
        SDL_LogError(SDL_LOG_CATEGORY_ERROR, "Could not create window: %s\n", SDL_GetError());
        return 1;
    }

     SDL_Renderer *renderer = NULL;
     renderer = SDL_CreateRenderer(window, NULL);

    // create render target
    SDL_Texture* texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_BGRA8888, SDL_TEXTUREACCESS_TARGET, 300, 200);

    // load image
    SDL_Surface* surf = IMG_Load("assets/image.jpg");
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
            if (std::filesystem::exists(input_path)) {
                if (start_time < 0 || duration <= 0 || start_time + duration > video_duration) {
                    process_message = "Invalid start time or duration!";
                    process_success = false;
                } else {
                    process_success = execute_ffmpeg_cut(clips, output_path);
                    process_message = process_success ? "Video cut successfully!" : "Failed to cut video!";
                }
            } else {
                process_message = "Input file does not exist!";
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


SDL_Texture* CreatePreviewTexture(SDL_Renderer* renderer, const std::vector<Clip>& clips, int playhead_time) {
    const std::string preview_path = "preview_frame.jpg";
    std::vector<Clip> active_clips;

    // 1. Collect active clips and sort by layer (ascending)
    for (const auto& clip : clips) {
        if (playhead_time >= clip.start_time && playhead_time < clip.start_time + clip.duration) {
            active_clips.push_back(clip);
        }
    }
    std::sort(active_clips.begin(), active_clips.end(), [](const Clip& a, const Clip& b) {
        return a.layer < b.layer; // Lower layers first
    });

    // 2. Build FFmpeg command
    std::stringstream cmd;
    cmd << "ffmpeg -y ";

    // Base black background (60s duration)
    cmd << "-f lavfi -i color=black:s=1920x1080:r=30:d=60 ";

    // Add active clips as subsequent inputs
    std::vector<std::string> clip_filters;
    for (size_t i = 0; i < active_clips.size(); ++i) {
        const auto& clip = active_clips[i];
        float clip_time = playhead_time - clip.start_time;
        cmd << "-ss " << clip_time << " -i \"" << clip.path << "\" ";
        clip_filters.push_back("[" + std::to_string(i+1) + ":v]scale=1920:1080:force_original_aspect_ratio=disable[v" + 
                             std::to_string(i) + "]");
    }

    // Build filtergraph
    cmd << "-filter_complex \"";
    cmd << "[0:v]setpts=PTS-STARTPTS[base];"; // Base layer

    // Add scaling filters
    for (const auto& filter : clip_filters) {
        cmd << filter << ";";
    }

    // Overlay chain (lower layers first)
    std::string overlay_chain = "[base]";
    for (size_t i = 0; i < active_clips.size(); ++i) {
        cmd << overlay_chain << "[v" << i << "]overlay=eof_action=pass[ol" << i << "];";
        overlay_chain = "[ol" + std::to_string(i) + "]";
    }

    cmd << overlay_chain << "format=yuv420p[vout]\" ";
    cmd << "-map \"[vout]\" -frames:v 1 " << preview_path << " 2> ffmpeg_log.txt";

    // Execute and load texture
    int result = system(cmd.str().c_str());
    if (result != 0) {
        std::cerr << "FFmpeg failed. Command:\n" << cmd.str() << "\n";
        return nullptr;
    }

    SDL_Surface* surface = IMG_Load(preview_path.c_str());
    if (!surface) {
        std::cerr << "Failed to load preview image: " << SDL_GetError() << "\n";
        return nullptr;
    }
    
    SDL_Texture* texture = SDL_CreateTextureFromSurface(renderer, surface);
    SDL_DestroySurface(surface);
    remove(preview_path.c_str());
    
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