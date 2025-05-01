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
#include <map>     // For GLResources caches, media_library
#include <deque>   // For VideoData frame_cache
#include <mutex>   // For potential future threading
#include <limits>  // For numeric_limits
#include <memory>  // For shared_ptr

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
#include <imgui_stdlib.h>
#include <imgui_internal.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_opengl3.h>
#include <SDL3/SDL.h>
#include <SDL3/SDL_video.h>
#include <SDL3/SDL_image.h>

// Project headers
#include "shared.hpp"       // Core types, KeyframeTrack
#include "node.hpp"         // Node definitions
#include "video_export.hpp" // Export, Resource Mgmt, Thumbnails (Needs Node awareness)
#include "project_io.hpp"   // Project Save/Load (Needs Node awareness)

// Global gradient data storage (Keep as is)
struct GradientData {
    ImVec4 window_grad_top;
    ImVec4 window_grad_bottom;
};

// === Global State ===
std::atomic<bool> playing = false;
float playhead_time = 0.0f;
Uint64 last_frame_ticks = 0;

int preview_width = 1280;
int preview_height = 720;

int render_width = 1920;
int render_height = 1080;
int export_fps = 30;
float timeline_max_duration = 10.0f; // Max duration displayed on timeline ruler initially

// --- Node/Clip Management ---
std::map<std::string, Clip> media_library; // Stores source Clip info, keyed by path
std::vector<std::shared_ptr<Node>> timeline_root_nodes; // Top-level nodes on timeline
Node* selected_node = nullptr; // Keep track of selected node

char output_path[FILENAME_MAX] = "output.mp4";

bool show_thumbs = true; // Enable/disable thumbnail generation/display

GLResources gl_resources; // Global OpenGL/FFmpeg resources cache

std::atomic<bool> timeline_needs_redraw = true; // Flag for redraw/update needs
std::string status_message = "Ready.";

// Error checking macros (Keep as is)
#define CHECK_AV_ERROR(ret, message) /* ... */
#define FFMPEG_CHECK(condition, message) /* ... */

// --- ImGui Styling and Custom Rendering ---
void SetupImGuiStyle();
GLuint CreateGradientTexture(ImVec4 top, ImVec4 bottom, int height = 1280);
void ApplyWindowBackgroundGradients();
void RenderDockSpace();

// --- Timeline / Preview ---
void DrawTimelineEditor(
    std::vector<std::shared_ptr<Node>>& root_nodes, // Use Nodes
    float& playhead_time,
    float& view_duration, // The duration the timeline view covers
    float& zoom_factor, // Controls pixels_per_second
    std::atomic<bool>& needs_redraw,
    Node*& selected_node, // Use Node*
    GLResources& res
);
void UpdatePreview(GLResources& res, const std::vector<std::shared_ptr<Node>>& root_nodes, int width, int height, float current_time);
void RenderPreviewWindow(GLuint preview_tex, int preview_width, int preview_height);

// --- Keyframe Editor ---
template<typename T>
void DrawKeyframeTrackEditor(const std::string& label, KeyframeTrack<T>& track, std::atomic<bool>& changed_flag); // Pass flag


// --- Media Handling ---
// Adds a Clip to the library if new, and creates MediaNode(s) on the timeline
void AddMediaToTimeline(const std::string& file_path, float timeline_time, int target_layer) {
    if (!std::filesystem::exists(file_path)) {
        status_message = "Error: File not found: " + file_path;
        std::cerr << status_message << std::endl;
        return;
    }

    std::string base_name = std::filesystem::path(file_path).filename().string();
    Clip* source_clip_ptr = nullptr;

    // 1. Check/Add to Media Library
    auto it = media_library.find(file_path);
    if (it == media_library.end()) {
        // New media, determine type and load initial info
        Clip new_clip;
        new_clip.path = file_path;
        new_clip.name = base_name;

        if (is_video_file(file_path)) {
            new_clip.type = ClipType::Video;
             // Detect audio during resource loading
             new_clip.has_audio = true; // Assume true initially, load_resources will verify
        } else if (is_image_file(file_path)) {
            new_clip.type = ClipType::Image;
            new_clip.has_audio = false;
        } else {
             // Try loading as audio-only? Or check extension?
             // For now, assume video/image based on common extensions
             // A more robust check would try avformat_open_input
             status_message = "Unsupported file type: " + base_name;
             std::cerr << status_message << std::endl;
             return;
        }

        // Load resources (gets duration, w/h, preloads audio, creates textures/contexts)
        if (!load_resources_for_clip(gl_resources, new_clip)) {
             status_message = "Failed to load resources for: " + base_name;
             std::cerr << status_message << std::endl;
             // Don't add to library if essential resources failed
             return;
        }

        // Add to library and get pointer
        auto [inserted_it, success] = media_library.emplace(file_path, std::move(new_clip));
        if (!success) { /* Should not happen if find failed */ return; }
        source_clip_ptr = &inserted_it->second;

         // Queue thumbnails for the *new* source clip
         if (show_thumbs) {
            QueueClipThumbnails(gl_resources, *source_clip_ptr);
         }

    } else {
        // Media already in library
        source_clip_ptr = &it->second;
    }

    if (!source_clip_ptr) return; // Should not happen

    // 2. Create Media Node(s)
    bool created_node = false;
    std::shared_ptr<Node> video_or_image_node = nullptr;
    std::shared_ptr<Node> audio_node = nullptr;

    // Create Video or Image Node
    if (source_clip_ptr->type == ClipType::Video || source_clip_ptr->type == ClipType::Image) {
        auto new_media_node = std::make_shared<MediaNode>(false); // false = not audio primary
        new_media_node->name = source_clip_ptr->name + " [" + (source_clip_ptr->type == ClipType::Video ? "Video" : "Image") + "]";
        new_media_node->source_clip = source_clip_ptr;
        new_media_node->start_time = timeline_time;
        // Duration: Use source duration for video, default 5s for image?
        new_media_node->duration = (source_clip_ptr->type == ClipType::Video) ? source_clip_ptr->source_duration : 5.0f;
        new_media_node->duration = std::max(0.1f, new_media_node->duration); // Ensure minimum duration
        new_media_node->layer = target_layer;
        new_media_node->media_start = 0.0f; // Default start at beginning of media
        new_media_node->visible = true;
        // Initialize default transform values (could inherit from source clip if needed)
        new_media_node->pos_x = 0.0f; new_media_node->pos_y = 0.0f;
        new_media_node->scale = 1.0f; new_media_node->rotation = 0.0f;
        new_media_node->opacity = 1.0f;

        timeline_root_nodes.push_back(new_media_node);
        video_or_image_node = new_media_node;
        created_node = true;
    }

    // Create separate Audio Node if source has audio and isn't audio-only type
    if (source_clip_ptr->has_audio && source_clip_ptr->type != ClipType::Audio) {
         auto new_audio_node = std::make_shared<MediaNode>(true); // true = is audio primary
         new_audio_node->name = source_clip_ptr->name + " [Audio]";
         new_audio_node->source_clip = source_clip_ptr;
         new_audio_node->start_time = timeline_time; // Sync start time
         new_audio_node->duration = source_clip_ptr->source_duration; // Use full audio duration
         new_audio_node->duration = std::max(0.1f, new_audio_node->duration);
         new_audio_node->layer = target_layer; // Same layer? Or layer below? Let's use same for now.
         new_audio_node->media_start = 0.0f;
         new_audio_node->visible = true;
         new_audio_node->is_audio = true; // Mark explicitly

         timeline_root_nodes.push_back(new_audio_node);
         audio_node = new_audio_node;
         created_node = true;

         // Link the nodes (using weak_ptr to avoid cycles)
         if (video_or_image_node) {
            std::static_pointer_cast<MediaNode>(video_or_image_node)->linked_node = audio_node;
         }
         new_audio_node->linked_node = video_or_image_node; // Assuming MediaNode type
    }

    if (created_node) {
        status_message = "Added node(s) for: " + base_name;
        timeline_needs_redraw = true;
    }
}


// Helper to get actual project duration based on node timings
float calculate_project_duration(const std::vector<std::shared_ptr<Node>>& root_nodes) {
    float max_time = 0.0f;
     std::function<void(Node*)> find_max_time =
         [&](Node* node) {
         if (!node) return;
         max_time = std::max(max_time, node->start_time + node->duration);
         for (const auto& child : node->children) {
             find_max_time(child.get()); // Recurse if nodes can be nested
         }
     };
     for (const auto& root : root_nodes) {
         find_max_time(root.get());
     }
     return std::max(10.0f, max_time); // Return at least 10s
}

// --- Main Application ---
int main(int argc, char* argv[]) {
    // --- SDL / OpenGL / ImGui Initialization ---
    avformat_network_init();
    SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS); // Need Events for dropping files

    SDL_Window* window = SDL_CreateWindow("Zest Node Editor", 1600, 900, SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_MAXIMIZED);
    if (!window) { /* error */ return 1; }

    SDL_GLContext gl_context = SDL_GL_CreateContext(window);
    if (!gl_context) { /* error */ return 1; }
    if (!gladLoadGLLoader((GLADloadproc)SDL_GL_GetProcAddress)) { /* error */ return 1; }

    // SDL_SetHint(SDL_HINT_DROP_EVENT, "1"); // Enable file drop events

    // Icon
    SDL_Surface* icon_surface = IMG_Load("assets/logo.png");
    if (icon_surface) { SDL_SetWindowIcon(window, icon_surface); SDL_DestroySurface(icon_surface); }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO(); (void)io;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_DockingEnable | ImGuiConfigFlags_ViewportsEnable;

    SetupImGuiStyle(); // Apply custom style
    // Font loading (ensure path is correct)
    std::string font_path = "C:\\Windows\\Fonts\\segoeui.ttf";
    if (std::filesystem::exists(font_path)) { io.Fonts->AddFontFromFileTTF(font_path.c_str(), 18.0f); }
    else { io.Fonts->AddFontDefault(); std::cerr << "Warning: Font not found. Using default." << std::endl; }


    ImGui_ImplSDL3_InitForOpenGL(window, gl_context);
    ImGui_ImplOpenGL3_Init("#version 130");

    // --- Application State ---
    float timeline_zoom_factor = 1.0f; // Affects pixels per second
    bool file_dropped_this_frame = false;
    std::string dropped_file_path;
    float last_preview_update_time = -1.0f;
    const float PREVIEW_UPDATE_INTERVAL = 1.0f / 60.0f; // Target ~60fps preview updates

    // Initial GL resource setup for preview
    if (!setup_gl_resources(gl_resources, preview_width, preview_height)) {
        std::cerr << "Failed to initialize initial GL resources!" << std::endl;
        // Shutdown sequence...
        return 1;
    }

    if(show_thumbs) start_thumbnail_worker();

    // --- Main Loop ---
    bool running = true;
    last_frame_ticks = SDL_GetTicks();

    while (running) {
        Uint64 current_ticks = SDL_GetTicks();
        float delta_time = (current_ticks - last_frame_ticks) / 1000.0f;
        delta_time = std::min(delta_time, 0.1f); // Clamp delta time
        last_frame_ticks = current_ticks;

        file_dropped_this_frame = false;

        // --- Event Handling ---
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            ImGui_ImplSDL3_ProcessEvent(&event);
            if (event.type == SDL_EVENT_QUIT) running = false;
            if (event.type == SDL_EVENT_WINDOW_RESIZED || event.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED) { /* Handle resize */ }

            // --- Keyboard Shortcuts ---
            if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_SPACE && event.key.down) {
                    playing = !playing.load();
                    status_message = playing ? "Playback Started" : "Playback Paused";
            }
            // Split Node (Ctrl+B)
            else if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_B && (SDL_GetModState() & SDL_KMOD_CTRL) && selected_node && event.key.down) {
                if (selected_node->type == NodeType::Media) { // Only split media nodes for now
                        MediaNode* media_node = static_cast<MediaNode*>(selected_node);
                        float cut_time = playhead_time;
                        float node_start = media_node->start_time;
                        float node_end = media_node->start_time + media_node->duration;

                        if (cut_time > node_start + 0.01f && cut_time < node_end - 0.01f) { // Check if cut is within bounds
                            float time_into_node = cut_time - node_start;

                            // Create the new node (second part)
                            std::shared_ptr<MediaNode> new_split_node = std::make_shared<MediaNode>(media_node->is_audio);
                            *new_split_node = *media_node; // Copy properties

                            // Adjust timings and media start
                            new_split_node->id = 0; // Get a new ID (or handle ID copying carefully in Node constructor/copy)
                            static size_t next_id_split = 10000; new_split_node->id = next_id_split++; // Simple unique ID for now
                            new_split_node->start_time = cut_time;
                            new_split_node->duration = node_end - cut_time;
                            new_split_node->media_start = media_node->media_start + time_into_node;
                            new_split_node->name = media_node->name + " (Split)";
                            new_split_node->parent = media_node->parent; // Keep same parent initially
                            new_split_node->children.clear(); // Split node shouldn't inherit children directly

                            // Modify the original node (first part)
                            media_node->duration = time_into_node;

                            // Add the new node to the timeline (find correct position if parented)
                            if (media_node->parent) {
                                media_node->parent->AddChild(new_split_node); // Add as sibling
                            } else {
                                timeline_root_nodes.push_back(new_split_node); // Add as root node
                            }

                            // Handle linked node splitting
                            std::shared_ptr<Node> linked_shared = media_node->linked_node.lock();
                            if (linked_shared && linked_shared->type == NodeType::Media) {
                                MediaNode* linked_media_node = static_cast<MediaNode*>(linked_shared.get());
                                float linked_node_end = linked_media_node->start_time + linked_media_node->duration;

                                // Assume linked node has same start/duration for split logic (needs refinement)
                                std::shared_ptr<MediaNode> new_linked_split = std::make_shared<MediaNode>(linked_media_node->is_audio);
                                *new_linked_split = *linked_media_node; // Copy properties

                                new_linked_split->id = 0; static size_t next_id_split_link = 20000; new_linked_split->id = next_id_split_link++;
                                new_linked_split->start_time = cut_time;
                                new_linked_split->duration = linked_node_end - cut_time; // Adjust based on original end
                                new_linked_split->media_start = linked_media_node->media_start + time_into_node; // Assuming sync
                                new_linked_split->name = linked_media_node->name + " (Split)";
                                new_linked_split->parent = linked_media_node->parent;
                                new_linked_split->children.clear();

                                linked_media_node->duration = time_into_node;

                                // Add new linked node
                                if (linked_media_node->parent) {
                                    linked_media_node->parent->AddChild(new_linked_split);
                                } else {
                                    timeline_root_nodes.push_back(new_linked_split);
                                }

                                // Re-link the split pairs
                                new_split_node->linked_node = new_linked_split;
                                new_linked_split->linked_node = new_split_node;
                                media_node->linked_node = linked_shared; // Original pair remains linked
                                linked_media_node->linked_node = media_node->shared_from_this(); // Original pair remains linked


                            } else {
                                // Original node didn't have a valid link, or split node doesn't need one
                                new_split_node->linked_node.reset();
                                media_node->linked_node.reset(); // Break link of original too? Maybe not if linked node wasn't split.
                            }


                            selected_node = new_split_node.get(); // Select the new (second) part
                            timeline_needs_redraw = true;
                            status_message = "Split node " + media_node->name;
                            std::cout << status_message << std::endl;
                        } else {
                            status_message = "Cut position not within node bounds.";
                        }
                    }
                }
                 // Delete Node (Delete Key)
                else if (event.key.key == SDLK_DELETE && selected_node && event.type == SDL_EVENT_KEY_DOWN && event.key.down) {
                    std::string deleted_name = selected_node->name;
                    Node* node_to_delete = selected_node;
                    selected_node = nullptr; // Deselect first

                    // Find and remove the node from the hierarchy
                    bool removed = false;
                    if (node_to_delete->parent) {
                         auto& children = node_to_delete->parent->children;
                         auto it = std::remove_if(children.begin(), children.end(),
                                                 [&](const std::shared_ptr<Node>& p) { return p.get() == node_to_delete; });
                         if (it != children.end()) {
                             children.erase(it, children.end());
                             removed = true;
                         }
                    } else {
                         // It's a root node
                         auto it = std::remove_if(timeline_root_nodes.begin(), timeline_root_nodes.end(),
                                                 [&](const std::shared_ptr<Node>& p) { return p.get() == node_to_delete; });
                         if (it != timeline_root_nodes.end()) {
                             timeline_root_nodes.erase(it, timeline_root_nodes.end());
                             removed = true;
                         }
                    }

                    // Unlink if it's a MediaNode with a link
                    if (node_to_delete->type == NodeType::Media) {
                        MediaNode* mn = static_cast<MediaNode*>(node_to_delete);
                        auto linked_shared = mn->linked_node.lock();
                        if(linked_shared && linked_shared->type == NodeType::Media) {
                             static_cast<MediaNode*>(linked_shared.get())->linked_node.reset();
                        }
                         mn->linked_node.reset(); // Clear weak ptr on deleted node too
                    }


                    if (removed) {
                         status_message = "Deleted node: " + deleted_name;
                         std::cout << status_message << std::endl;
                         timeline_needs_redraw = true;
                         // The shared_ptr going out of scope should trigger Node destructor if no other references exist
                    } else {
                        status_message = "Failed to find node to delete.";
                        std::cerr << status_message << std::endl;
                    }
                }

            // --- File Drop ---
            if (event.type == SDL_EVENT_DROP_FILE) {
                if (event.drop.data) {
                    dropped_file_path = event.drop.data;
                    // SDL_free(event.drop.data); // Free the dropped path memory
                    file_dropped_this_frame = true;
                     status_message = "Processing dropped file...";
                }
            }

        } // End Event Loop

        // --- Process Dropped File ---
        if (file_dropped_this_frame) {
            std::cout << "File dropped: " << dropped_file_path << std::endl;
            AddMediaToTimeline(dropped_file_path, playhead_time, 0); // Add to layer 0 at playhead
            dropped_file_path.clear();
        }

        // --- Update Playback ---
        if (playing) {
            playhead_time += delta_time;
            float actual_project_duration = calculate_project_duration(timeline_root_nodes);
            if (playhead_time >= actual_project_duration) {
                playhead_time = actual_project_duration;
                playing = false; // Stop at end
                status_message = "Playback Reached End";
            }
             timeline_needs_redraw = true; // Playhead moved
        }

        // --- Update Video Previews (Decoding/Texture Upload) ---
        // Only update if playing or if redraw needed and time changed significantly
        if (playing || (timeline_needs_redraw && std::abs(playhead_time - last_preview_update_time) > PREVIEW_UPDATE_INTERVAL)) {
            update_video_previews(gl_resources, timeline_root_nodes, playhead_time);
            last_preview_update_time = playhead_time;
        }

        // --- Process Thumbnail Results ---
        if (show_thumbs) ProcessThumbnailResults(gl_resources, 2); // Process a couple per frame

        // --- Start ImGui Frame ---
        ImGui_ImplOpenGL3_NewFrame(); ImGui_ImplSDL3_NewFrame(); ImGui::NewFrame();
        RenderDockSpace(); // Setup docking environment

        // --- UI Windows ---

        // Controls Window
        ImGui::Begin("Controls");
        ApplyWindowBackgroundGradients();
        // Play/Pause Button
        if (ImGui::Button(playing ? "Pause (Space)" : "Play (Space)", ImVec2(120, 0))) {
            playing = !playing.load();
            status_message = playing ? "Playback Started" : "Playback Paused";
        }
        ImGui::SameLine();
        float current_project_duration = calculate_project_duration(timeline_root_nodes);
        ImGui::Text("Time: %.2f / %.2f", playhead_time, current_project_duration);
        // Seek Slider
        ImGui::SetNextItemWidth(-1); // Use full width
        if (ImGui::SliderFloat("##Seek", &playhead_time, 0.0f, current_project_duration, "%.2f s")) {
            playing = false; // Stop playback on manual seek
            timeline_needs_redraw = true;
            last_preview_update_time = -1.0f; // Force preview update
        }
        ImGui::Separator();
         // Export Section
         ImGui::InputText("Output File", output_path, sizeof(output_path)); // Use global output_path
         ImGui::InputInt("Export FPS", &export_fps); export_fps = std::max(1, export_fps);
         ImGui::InputInt2("Export Res", &render_width); render_width = std::max(16, render_width); render_height = std::max(16, render_height);
         if (ImGui::Button("Export Video")) {
             SDL_GL_MakeCurrent(window, gl_context); // Ensure context active
             std::filesystem::path out_p(output_path);
             if (!out_p.has_filename()) { status_message = "Output path is not a valid filename!"; }
             else {
                 float export_duration = calculate_project_duration(timeline_root_nodes);
                 if (export_duration <= 0) { status_message = "Cannot export empty timeline!"; }
                 else {
                     status_message = "Exporting..."; playing = false; // Stop playback
                     ImGui::Render(); // Render UI once to show status message
                     ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData()); SDL_GL_SwapWindow(window); // Display message

                     bool success = start_video_export(output_path, render_width, render_height, export_fps, export_duration, timeline_root_nodes, media_library, window);
                     status_message = success ? "Export finished successfully!" : "Export failed!";
                 }
             }
         }
         ImGui::Text("Status: %s", status_message.c_str()); ImGui::Separator();
         // Project IO
         if (ImGui::Button("Save Project")) {
             const char* filters[] = { "*.zest" };
             const char* save_path = tinyfd_saveFileDialog("Save Project", "project.zest", 1, filters, "Zest Project Files (*.zest)");
             if (save_path) {
                 if (SaveProject(save_path, media_library, timeline_root_nodes)) {
                     status_message = "Project saved: " + std::string(save_path);
                 } else { status_message = "Failed to save project!"; }
             }
         }
         ImGui::SameLine();
         if (ImGui::Button("Load Project")) {
             const char* filters[] = { "*.zest" };
             const char* load_path = tinyfd_openFileDialog("Load Project", "", 1, filters, "Zest Project Files (*.zest)", 0);
             if (load_path) {
                 playing = false; // Stop playback
                 selected_node = nullptr;
                 // Create temporary holders for loaded data
                 std::map<std::string, Clip> loaded_library;
                 std::vector<std::shared_ptr<Node>> loaded_nodes;

                 if (LoadProject(load_path, loaded_library, loaded_nodes)) {
                     // --- Cleanup old state ---
                     timeline_root_nodes.clear(); // Clear nodes first (releases shared_ptrs)
                     media_library.clear();
                     cleanup_gl_resources(gl_resources); // Clean GL textures
                     cleanup_video_resources(gl_resources); // Clean FFmpeg contexts
                     gl_resources.preloaded_audio.clear(); // Clear audio cache
                      // Stop/clear thumbnail worker state
                     if(show_thumbs) {
                        stop_thumbnail_worker(); // Stop current worker
                        // Clear queues (should be done by stop worker)
                        // Clear GL resource maps for thumbnails
                        gl_resources.clip_thumbnail_textures.clear();
                        gl_resources.generated_thumbnails_map.clear();
                        start_thumbnail_worker(); // Restart worker
                     }


                     // --- Apply loaded state ---
                     media_library = std::move(loaded_library);
                     timeline_root_nodes = std::move(loaded_nodes);

                     // --- Reload resources for the new library/nodes ---
                     if (!setup_gl_resources(gl_resources, preview_width, preview_height)) {
                          status_message = "Error: Failed to re-initialize GL resources after load!";
                          // Handle critical error
                     }
                     status_message = "Reloading resources..."; ImGui::Render(); ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData()); SDL_GL_SwapWindow(window);

                     for (auto& [path, clip] : media_library) {
                         load_resources_for_clip(gl_resources, clip); // Load for library clips
                         if(show_thumbs) QueueClipThumbnails(gl_resources, clip); // Queue thumbs
                     }

                     playhead_time = 0.0f; // Reset playhead
                     timeline_zoom_factor = 1.0f; // Reset zoom
                     timeline_needs_redraw = true;
                     status_message = "Project loaded: " + std::string(load_path);
                 } else {
                     status_message = "Failed to load project!";
                 }
             }
         }

        ImGui::End(); // End Controls

        // Timeline Editor
        float current_view_duration = calculate_project_duration(timeline_root_nodes); // Use actual duration for view range
        DrawTimelineEditor(timeline_root_nodes, playhead_time, current_view_duration, timeline_zoom_factor, timeline_needs_redraw, selected_node, gl_resources);

        // Video Preview
         if (timeline_needs_redraw) { // Update preview if timeline changed
             UpdatePreview(gl_resources, timeline_root_nodes, preview_width, preview_height, playhead_time);
             timeline_needs_redraw = false; // Reset flag
         }
        RenderPreviewWindow(gl_resources.render_tex, preview_width, preview_height);

        // Inspector
        ImGui::Begin("Inspector"); ApplyWindowBackgroundGradients();
        if (selected_node) {
            ImGui::Text("Selected Node: %s (ID: %zu)", selected_node->name.c_str(), selected_node->id);
            if (selected_node->type == NodeType::Media) {
                MediaNode* mn = static_cast<MediaNode*>(selected_node);
                if (mn->source_clip) ImGui::Text("Source: %s", mn->source_clip->path.c_str());
                if (ImGui::InputFloat("Media Start", &mn->media_start, 0.01f, 0.1f, "%.3f")) {
                    timeline_needs_redraw = true;
                }
                mn->media_start = std::max(0.0f, mn->media_start);
                 // Display linked node info
                 auto linked = mn->linked_node.lock();
                 if(linked) ImGui::Text("Linked Node ID: %zu (%s)", linked->id, linked->name.c_str());
                 else ImGui::Text("Linked Node: None");

            } else if (selected_node->type == NodeType::Group) {
                ImGui::Text("Type: Group Node");
            }
             ImGui::Separator();

            // Common Node Properties
            if (ImGui::InputText("Name", &selected_node->name[0], 64)) {
                timeline_needs_redraw = true;
            }
            if (ImGui::Checkbox("Visible", &selected_node->visible)) {
                timeline_needs_redraw = true;
            }
            if (ImGui::InputFloat("Start Time", &selected_node->start_time, 0.01f, 0.1f, "%.3f")) {
                timeline_needs_redraw = true;
            }
            selected_node->start_time = std::max(0.0f, selected_node->start_time);
            if (ImGui::InputFloat("Duration", &selected_node->duration, 0.01f, 0.1f, "%.3f")) {
                timeline_needs_redraw = true;
            }
            selected_node->duration = std::max(0.01f, selected_node->duration);
            if (ImGui::InputInt("Layer", &selected_node->layer)) {
                timeline_needs_redraw = true;
            }
            selected_node->layer = std::max(0, selected_node->layer);

            // Blend Mode Combo
            const char* blend_modes[] = { "Normal", "Additive", "Multiply", "Screen", /* ... add others ... */ };
            int current_mode = static_cast<int>(selected_node->blend_mode);
            if (ImGui::Combo("Blend Mode", &current_mode, blend_modes, IM_ARRAYSIZE(blend_modes))) {
                selected_node->blend_mode = static_cast<BlendMode>(current_mode);
                timeline_needs_redraw = true;
            }
            ImGui::Separator();

            // Transform Properties (using fallback values directly for sliders)
            ImGui::Text("Transform (Fallback Values)");
            if (ImGui::DragFloat("Position X", &selected_node->pos_x, 0.01f)) {
                timeline_needs_redraw = true;
            }
            if (ImGui::DragFloat("Position Y", &selected_node->pos_y, 0.01f)) {
                timeline_needs_redraw = true;
            }
            if (ImGui::DragFloat("Scale", &selected_node->scale, 0.01f, 0.0f)) {
                timeline_needs_redraw = true;
            }
            selected_node->scale = std::max(0.0f, selected_node->scale);
            if (ImGui::DragFloat("Rotation", &selected_node->rotation, 0.5f, 0.0f, 360.0f)) {
                timeline_needs_redraw = true;
            }
            if (ImGui::DragFloat("Opacity", &selected_node->opacity, 0.01f, 0.0f, 1.0f)) {
                timeline_needs_redraw = true;
            }

            // Keyframe Editors
             ImGui::SeparatorText("Keyframes");
             DrawKeyframeTrackEditor("Opacity Keys", selected_node->opacity_track, timeline_needs_redraw);
             DrawKeyframeTrackEditor("Position X Keys", selected_node->pos_x_track, timeline_needs_redraw);
             DrawKeyframeTrackEditor("Position Y Keys", selected_node->pos_y_track, timeline_needs_redraw);
             DrawKeyframeTrackEditor("Rotation Keys", selected_node->rotation_track, timeline_needs_redraw);
             DrawKeyframeTrackEditor("Scale Keys", selected_node->scale_track, timeline_needs_redraw);

            // Sync linked node timing if this node was modified
             if (timeline_needs_redraw && selected_node->type == NodeType::Media) {
                 MediaNode* mn = static_cast<MediaNode*>(selected_node);
                 auto linked_shared = mn->linked_node.lock();
                  if(linked_shared) {
                     // Sync start and duration
                     linked_shared->start_time = mn->start_time;
                     linked_shared->duration = mn->duration;
                      // Optionally sync media_start if desired? Usually not.
                      // linked_shared->media_start = mn->media_start;
                      if(linked_shared->type == NodeType::Media) {
                           static_cast<MediaNode*>(linked_shared.get())->media_start = mn->media_start;
                      }
                  }
             }


        } else {
            ImGui::Text("No node selected.");
        }
        ImGui::End(); // End Inspector


        // Media Library Window (Simple List)
        ImGui::Begin("Media Library"); ApplyWindowBackgroundGradients();
        ImGui::Text("Loaded Media (%zu items):", media_library.size()); ImGui::Separator();
         if (ImGui::BeginChild("LibraryScroll")) {
             ImGuiListClipper clipper; clipper.Begin(media_library.size());
             auto lib_it = media_library.begin();
             while (clipper.Step()) {
                 std::advance(lib_it, clipper.DisplayStart); // Advance iterator
                 for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
                     Clip& clip = lib_it->second;
                     ImGui::PushID(clip.path.c_str()); // Use path for ID
                     ImGui::Selectable(clip.name.c_str());
                     if (ImGui::BeginDragDropSource()) {
                         ImGui::SetDragDropPayload("DND_MEDIA_PATH", clip.path.c_str(), clip.path.size() + 1);
                         ImGui::Text("Drag %s", clip.name.c_str());
                         ImGui::EndDragDropSource();
                     }
                      ImGui::SameLine(250); ImGui::TextDisabled("%s (%.2fs)", clip.type == ClipType::Video ? "Video" : (clip.type == ClipType::Image ? "Image" : "Audio"), clip.source_duration);
                     ImGui::PopID();
                     ++lib_it; // Increment iterator
                 }
             }
              // clipper.End(); // Clipper destructor handles End()
         }
         ImGui::EndChild();
        ImGui::End(); // End Media Library

        // --- Rendering ---
        ImGui::Render();
        int display_w, display_h; SDL_GetWindowSizeInPixels(window, &display_w, &display_h); glViewport(0, 0, display_w, display_h);
        ImVec4 clear_color = ImGui::GetStyle().Colors[ImGuiCol_WindowBg];
        glClearColor(clear_color.x * clear_color.w, clear_color.y * clear_color.w, clear_color.z * clear_color.w, clear_color.w);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        // Handle ImGui viewports
        if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable) {
            SDL_Window* backup_current_window = SDL_GL_GetCurrentWindow(); SDL_GLContext backup_context = SDL_GL_GetCurrentContext();
            ImGui::UpdatePlatformWindows(); ImGui::RenderPlatformWindowsDefault();
            SDL_GL_MakeCurrent(backup_current_window, backup_context);
        }
        SDL_GL_SwapWindow(window);

    } // End main loop

    // --- Cleanup ---
    std::cout << "Cleaning up resources..." << std::endl;
    stop_thumbnail_worker();
    if (ImGui::GetIO().UserData) { IM_DELETE((GradientData*)ImGui::GetIO().UserData); ImGui::GetIO().UserData = nullptr; }
    timeline_root_nodes.clear(); // Important: Clear nodes before cleaning resources they might reference
    media_library.clear();
    cleanup_gl_resources(gl_resources);
    cleanup_video_resources(gl_resources); // Must be after GL cleanup if textures are linked
    gl_resources.preloaded_audio.clear();
    ImGui_ImplOpenGL3_Shutdown(); ImGui_ImplSDL3_Shutdown(); ImGui::DestroyContext();
    SDL_GL_DestroyContext(gl_context);
    SDL_DestroyWindow(window);
    SDL_Quit();
    avformat_network_deinit();
    std::cout << "Exiting application." << std::endl;
    return 0;
}


// --- UI Function Implementations ---

void SetupImGuiStyle() {
    // ... (implementation unchanged)
}

GLuint CreateGradientTexture(ImVec4 top, ImVec4 bottom, int height) {
     // ... (implementation unchanged)
    return 0; // Placeholder
}

void ApplyWindowBackgroundGradients() {
    // ... (implementation unchanged)
}

void RenderDockSpace() {
     // ... (implementation unchanged - maybe add menu items for project io?)
     // Example modification for menu:
     static ImGuiDockNodeFlags dockspace_flags = ImGuiDockNodeFlags_PassthruCentralNode;
     ImGuiWindowFlags window_flags = ImGuiWindowFlags_MenuBar | ImGuiWindowFlags_NoDocking;
     // ... (rest of setup) ...
     ImGui::Begin("DockSpace Demo", nullptr, window_flags);
     // ... (PopStyleVar) ...
     ImGuiIO& io = ImGui::GetIO();
     if (io.ConfigFlags & ImGuiConfigFlags_DockingEnable) {
         ImGuiID dockspace_id = ImGui::GetID("MyDockSpace");
         ImGui::DockSpace(dockspace_id, ImVec2(0.0f, 0.0f), dockspace_flags);
     }

     if (ImGui::BeginMenuBar()) {
         if (ImGui::BeginMenu("File")) {
             if (ImGui::MenuItem("Load Project...")) {
                 // Trigger Load Project Logic (using button logic for now)
                 const char* filters[] = { "*.zest" };
                 const char* load_path = tinyfd_openFileDialog("Load Project", "", 1, filters, "Zest Project Files (*.zest)", 0);
                 if (load_path) {
                      // Simulate button press or call load function directly
                       std::map<std::string, Clip> loaded_library;
                       std::vector<std::shared_ptr<Node>> loaded_nodes;
                        if (LoadProject(load_path, loaded_library, loaded_nodes)) {
                           // ... (perform cleanup and apply loaded state as in button logic) ...
                           status_message = "Project Loaded via Menu";
                        } else status_message = "Failed to load project!";
                 }
             }
             if (ImGui::MenuItem("Save Project...")) {
                 // Trigger Save Project Logic
                 const char* filters[] = { "*.zest" };
                 const char* save_path = tinyfd_saveFileDialog("Save Project", "project.zest", 1, filters, "Zest Project Files (*.zest)");
                 if (save_path) {
                      if (SaveProject(save_path, media_library, timeline_root_nodes)) status_message = "Project saved via Menu";
                      else status_message = "Failed to save project!";
                 }
             }
              if (ImGui::MenuItem("Export Video...")) { /* Trigger export logic maybe? */ }
             ImGui::Separator();
             if (ImGui::MenuItem("Exit")) {
                // Push an SDL_QUIT event to be handled by the main loop
                SDL_Event quit_event;
                quit_event.type = SDL_EVENT_QUIT;
                SDL_PushEvent(&quit_event);
            }
            ImGui::EndMenu();
         }
         // ... (Other menus) ...
         ImGui::EndMenuBar();
     }
     ImGui::End();
}


// --- Updated UpdatePreview Function ---
void UpdatePreview(GLResources& res, const std::vector<std::shared_ptr<Node>>& root_nodes, int width, int height, float current_time) {
    // update_video_previews is already called in main loop based on timing
    // This function now just triggers the render_frame to the preview FBO
    render_frame(res, current_time, root_nodes, width, height);
}


// --- RenderPreviewWindow (Mostly Unchanged) ---
void RenderPreviewWindow(GLuint preview_tex, int preview_width, int preview_height) {
    // ... (implementation mostly unchanged)
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
    if (preview_tex != 0) {
         // UV Coordinates: OpenGL FBOs often have Y=0 at bottom. ImGui expects Y=0 at top.
         // Flip the V coordinate for display in ImGui: (0,1) to (1,0).
         ImGui::Image((ImTextureID)(intptr_t)preview_tex, display_size, ImVec2(0, 1), ImVec2(1, 0));
    } else { /* Placeholder drawing */ }
    ImGui::End();
}


void DrawTimelineEditor(
    std::vector<std::shared_ptr<Node>>& root_nodes,
    float& playhead_time,
    float& view_duration, // The duration the timeline view covers
    float& zoom_factor,   // Controls pixels per second
    std::atomic<bool>& needs_redraw,
    Node*& selected_node,
    GLResources& res)
{
    ImGui::Begin("Timeline Editor");
    ApplyWindowBackgroundGradients();

    // Constants & Styling
    const float label_width = 120.0f;
    const float timeline_area_width = ImGui::GetContentRegionAvail().x - label_width;
    const float layer_height = 45.0f;
    const float layer_padding = 4.0f;
    const float handle_width = 8.0f;
    const float thumbnail_margin = 2.0f;
    const float waveform_height_scale = 0.8f;
    const float ruler_height = 20.0f; // Space above timeline for ruler + playhead head
    const ImU32 col_video_top = IM_COL32(180, 90, 80, 255);
    const ImU32 col_video_bottom = IM_COL32(140, 70, 60, 255);
    const ImU32 col_audio_top = IM_COL32(80, 90, 180, 255);
    const ImU32 col_audio_bottom = IM_COL32(60, 70, 140, 255);
    const ImU32 col_image_top = IM_COL32(90, 180, 80, 255);
    const ImU32 col_image_bottom = IM_COL32(70, 140, 60, 255);
    const ImU32 col_group_top = IM_COL32(160, 160, 80, 255);
    const ImU32 col_group_bottom = IM_COL32(130, 130, 60, 255);
    const ImU32 col_border = IM_COL32(200, 200, 200, 100);
    const ImU32 col_selection = IM_COL32(255, 165, 0, 255);
    const ImU32 col_text = IM_COL32(240, 240, 240, 230);
    const ImU32 col_text_shadow = IM_COL32(0, 0, 0, 150);
    const ImU32 col_waveform = IM_COL32(230, 230, 250, 200);
    const ImU32 col_ruler_major = IM_COL32(255, 255, 255, 90);
    const ImU32 col_ruler_minor = IM_COL32(255, 255, 255, 50);
    const ImU32 col_playhead = IM_COL32(255, 70, 70, 255);
    const ImU32 col_label_bg = IM_COL32(50, 50, 55, 255);
    const ImU32 col_timeline_bg = IM_COL32(40, 40, 45, 255);
    const ImU32 col_grid_line = IM_COL32(60, 60, 65, 150);
    const ImU32 col_separator_line = IM_COL32(60, 60, 65, 255);

    // Determine layer range
    int min_layer = 0, max_layer = 0;
    bool has_nodes = !root_nodes.empty();
    if (has_nodes) {
        min_layer = root_nodes[0]->layer; // Initialize with first node's layer
        max_layer = root_nodes[0]->layer;
    }
    std::function<void(Node*)> find_layers = [&](Node* node) {
        if (!node) return;
        min_layer = std::min(min_layer, node->layer);
        max_layer = std::max(max_layer, node->layer);
        for (const auto& child : node->children) find_layers(child.get());
    };
    for (const auto& root : root_nodes) find_layers(root.get());
    int num_layers = has_nodes ? (max_layer - min_layer) + 1 : 1; // Ensure at least 1 layer visually

    // Timeline dimensions
    float total_timeline_height = num_layers * (layer_height + layer_padding);
    float pixels_per_second = 100.0f * zoom_factor;
    pixels_per_second = std::max(1.0f, pixels_per_second); // Ensure minimum pixels/sec
    zoom_factor = std::max(0.01f, zoom_factor); // Prevent zoom <= 0

    // --- Timeline Controls (Zoom) ---
    if (ImGui::Button("Zoom In")) { zoom_factor *= 1.2f; needs_redraw = true; } ImGui::SameLine();
    if (ImGui::Button("Zoom Out")) { zoom_factor /= 1.2f; needs_redraw = true; } ImGui::SameLine();
    ImGui::Text("Zoom: %.2fx", zoom_factor);

    view_duration = calculate_project_duration(root_nodes); // Recalculate view duration based on nodes

    // --- Draw Areas Setup ---
    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    ImVec2 window_content_start = ImGui::GetCursorScreenPos(); // Top-left below zoom controls
    ImVec2 labels_area_min = ImVec2(window_content_start.x, window_content_start.y + ruler_height);
    ImVec2 labels_area_max = ImVec2(labels_area_min.x + label_width, labels_area_min.y + total_timeline_height);
    ImVec2 timeline_canvas_min = ImVec2(labels_area_min.x + label_width, labels_area_min.y); // Where the scrollable child starts
    ImVec2 timeline_canvas_max = ImVec2(timeline_canvas_min.x + timeline_area_width, timeline_canvas_min.y + total_timeline_height);
    ImVec2 ruler_area_min = ImVec2(timeline_canvas_min.x, window_content_start.y);
    ImVec2 ruler_area_max = ImVec2(timeline_canvas_max.x, labels_area_min.y);

    // Backgrounds (Draw static parts)
    draw_list->AddRectFilled(labels_area_min, labels_area_max, col_label_bg);
    draw_list->AddRectFilled(ruler_area_min, ruler_area_max, col_timeline_bg); // Ruler background
    draw_list->AddLine(ImVec2(timeline_canvas_min.x, labels_area_min.y), ImVec2(timeline_canvas_min.x, labels_area_max.y), col_separator_line, 1.0f); // Separator

    // --- Layer Labels & Grid (Static Area) ---
    for (int i = 0; i < num_layers; ++i) {
        int layer_index = max_layer - i; // Layer number (higher value is higher up visually)
        float y_base = labels_area_min.y + i * (layer_height + layer_padding);
        ImVec2 label_min = ImVec2(labels_area_min.x, y_base);
        ImVec2 label_max = ImVec2(labels_area_max.x, y_base + layer_height);
        ImVec2 line_start = ImVec2(timeline_canvas_min.x, y_base + layer_height + layer_padding / 2.0f);
        ImVec2 line_end = ImVec2(timeline_canvas_max.x, line_start.y);

        draw_list->AddRectFilled(label_min, label_max, col_label_bg); // Redraw label bg just in case
        std::string layer_text = "L " + std::to_string(layer_index);
        ImVec2 text_size = ImGui::CalcTextSize(layer_text.c_str());
        draw_list->AddText(ImVec2(label_min.x + (label_width - text_size.x) * 0.5f, label_min.y + (layer_height - text_size.y) * 0.5f), col_text, layer_text.c_str());

        // Draw horizontal grid line across the timeline canvas area background
        if (i < num_layers) { // Draw line below every layer including the last one
             draw_list->AddLine(line_start, line_end, col_grid_line);
        }
    }

    // --- Timeline Content Child Window (Handles Scrolling) ---
    ImGui::SetCursorScreenPos(timeline_canvas_min); // Position the child window
    ImGui::PushStyleColor(ImGuiCol_ChildBg, col_timeline_bg);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    // Add vertical scrollbar flag if needed later: ImGuiWindowFlags_HorizontalScrollbar | ImGuiWindowFlags_AlwaysVerticalScrollbar
    ImGui::BeginChild("TimelineContent", ImVec2(timeline_area_width, total_timeline_height), false, ImGuiWindowFlags_HorizontalScrollbar | ImGuiWindowFlags_NoMove);
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();

    // Get child window properties *after* BeginChild
    ImDrawList* child_draw_list = ImGui::GetWindowDrawList();
    ImVec2 timeline_content_origin = ImGui::GetCursorScreenPos(); // Top-left corner of the scrollable content area (relative to screen)
    float current_scroll_x = ImGui::GetScrollX();
    float current_scroll_y = ImGui::GetScrollY(); // Should be 0 if no vertical scrollbar

    // Make content large enough to scroll horizontally
    ImGui::Dummy(ImVec2(view_duration * pixels_per_second, total_timeline_height - ImGui::GetStyle().ScrollbarSize)); // Adjust height for potential scrollbar

    // --- Ruler (Draw inside Child, relative to its origin, background already drawn above) ---
    float time_step = 1.0f;
    if (pixels_per_second < 30) time_step = 5.0f;
    if (pixels_per_second < 10) time_step = 10.0f;
    if (pixels_per_second > 150) time_step = 0.5f;
    if (pixels_per_second > 300) time_step = 0.25f;
    if (pixels_per_second > 600) time_step = 0.1f;

    int start_tick_idx = static_cast<int>(current_scroll_x / pixels_per_second / time_step);
    int end_tick_idx = static_cast<int>((current_scroll_x + timeline_area_width) / pixels_per_second / time_step) + 2;

    for (int i = start_tick_idx; i <= end_tick_idx; ++i) {
        float time = i * time_step;
        float x_abs = time * pixels_per_second;
        float x_rel = timeline_content_origin.x + x_abs - current_scroll_x;

        if (x_rel < timeline_content_origin.x - 5 || x_rel > timeline_content_origin.x + timeline_area_width + 5) continue;

        // Draw major tick line (on ruler area, drawn outside child)
        draw_list->AddLine(ImVec2(x_rel, ruler_area_min.y + ruler_height * 0.5f), ImVec2(x_rel, ruler_area_max.y), col_ruler_major);
        char time_label[16]; snprintf(time_label, sizeof(time_label), "%.2f", time);
        draw_list->AddText(ImVec2(x_rel + 3, ruler_area_min.y + 2), col_text, time_label); // Draw text on ruler area

        // Draw minor ticks (on ruler area)
        int minor_ticks = (time_step <= 0.1f) ? 0 : ((time_step == 0.25f) ? 1 : 4);
         if (minor_ticks > 0) {
            float minor_step = time_step / (minor_ticks + 1.0f);
             for (int j = 1; j <= minor_ticks; ++j) {
                 float minor_time = time + j * minor_step; // Ticks between major ones
                 float minor_x_abs = minor_time * pixels_per_second;
                 float minor_x_rel = timeline_content_origin.x + minor_x_abs - current_scroll_x;
                 if (minor_x_rel < timeline_content_origin.x - 1 || minor_x_rel > timeline_content_origin.x + timeline_area_width + 1) continue;
                 draw_list->AddLine(ImVec2(minor_x_rel, ruler_area_min.y + ruler_height * 0.75f), ImVec2(minor_x_rel, ruler_area_max.y), col_ruler_minor);
             }
         }
         // Draw vertical grid line through timeline content area
         child_draw_list->AddLine(ImVec2(x_rel, timeline_content_origin.y), ImVec2(x_rel, timeline_content_origin.y + total_timeline_height), col_grid_line);

    }


    // --- Draw Nodes (Inside Child Window) ---
    static int dragging_node_id = -1;
    static int resizing_node_id = -1;
    static bool resizing_left = false;
    static float drag_offset_x = 0.0f; // Offset relative to node start in pixels
    bool clicked_on_item_this_frame = false; // Renamed to avoid conflict

    std::function<void(Node*)> draw_node_recursive_child =
        [&](Node* node) {
        if (!node) return;

        // Calculate node screen position relative to child origin, considering scroll
        float node_start_x_abs = node->start_time * pixels_per_second;
        float node_end_x_abs = node_start_x_abs + node->duration * pixels_per_second;
        float node_start_x_rel = timeline_content_origin.x + node_start_x_abs - current_scroll_x;
        float node_end_x_rel = timeline_content_origin.x + node_end_x_abs - current_scroll_x;

        int layer_draw_index = max_layer - node->layer;
        float node_y_rel = timeline_content_origin.y + layer_draw_index * (layer_height + layer_padding);

        ImVec2 node_min = ImVec2(node_start_x_rel, node_y_rel);
        ImVec2 node_max = ImVec2(node_end_x_rel, node_y_rel + layer_height);

        // Cull nodes completely outside the *child window's visible area*
        if (node_max.x < timeline_content_origin.x || node_min.x > timeline_content_origin.x + timeline_area_width) {
            for (const auto& child : node->children) draw_node_recursive_child(child.get());
            return;
        }

        // Node Background Color based on Type
        ImU32 col_top = IM_COL32(100, 100, 100, 255), col_bottom = IM_COL32(80, 80, 80, 255);
         // ... (Color selection logic based on node type - unchanged) ...
        if (node->type == NodeType::Media) {
             MediaNode* mn = static_cast<MediaNode*>(node);
             if (mn->is_audio) { col_top = col_audio_top; col_bottom = col_audio_bottom; }
             else if (mn->source_clip && mn->source_clip->type == ClipType::Image) { col_top = col_image_top; col_bottom = col_image_bottom; }
             else { col_top = col_video_top; col_bottom = col_video_bottom; } // Default to video color
        } else if (node->type == NodeType::Group) {
            col_top = col_group_top; col_bottom = col_group_bottom;
        }


        // Draw Node Body
        child_draw_list->AddRectFilledMultiColor(node_min, node_max, col_top, col_top, col_bottom, col_bottom);
        child_draw_list->AddRect(node_min, node_max, col_border, 2.0f);

        // --- Draw Thumbnails / Waveform ---
        // ... (Thumbnail and Waveform drawing logic - unchanged, uses child_draw_list, node_min, node_max) ...
         if (node->type == NodeType::Media) {
             MediaNode* mn = static_cast<MediaNode*>(node);
             if (mn->source_clip) {
                 // Video/Image Thumbnails
                 if ((mn->source_clip->type == ClipType::Video || mn->source_clip->type == ClipType::Image) && show_thumbs) {
                     auto thumb_it = res.clip_thumbnail_textures.find(mn->source_clip->path);
                     if (thumb_it != res.clip_thumbnail_textures.end() && !thumb_it->second.empty()) {
                         const auto& thumbnails = thumb_it->second;
                         float thumb_draw_h = layer_height - 2 * thumbnail_margin;
                         float thumb_draw_w = thumb_draw_h * (THUMBNAIL_WIDTH / (float)THUMBNAIL_HEIGHT);

                         auto gen_map_it = res.generated_thumbnails_map.find(mn->source_clip->path);

                         float node_visible_start_x = std::max(node_min.x, timeline_content_origin.x);
                         float node_visible_end_x = std::min(node_max.x, timeline_content_origin.x + timeline_area_width);
                         float node_visible_width = node_visible_end_x - node_visible_start_x;

                         int first_thumb_idx = static_cast<int>(((node_visible_start_x - node_min.x) / (thumb_draw_w + thumbnail_margin)));
                         int last_thumb_idx = static_cast<int>(((node_visible_end_x - node_min.x) / (thumb_draw_w + thumbnail_margin))) +1;
                         first_thumb_idx = std::max(0, first_thumb_idx);


                         for (int k = first_thumb_idx; k < last_thumb_idx ; ++k)
                         {
                              float thumb_start_x_node = k * (thumb_draw_w + thumbnail_margin); // X relative to node start
                              float thumb_start_x_rel = node_min.x + thumb_start_x_node; // X relative to canvas

                              // Find the best thumbnail texture index (closest timestamp <= media_start + time_at_thumb_start)
                              float time_at_thumb_start = thumb_start_x_node / pixels_per_second;
                              float target_media_time = mn->media_start + time_at_thumb_start;
                              GLuint tex_id = 0;

                              if (gen_map_it != res.generated_thumbnails_map.end()) {
                                   float best_diff = std::numeric_limits<float>::max();
                                   auto best_match = gen_map_it->second.end();
                                   for(auto it = gen_map_it->second.begin(); it != gen_map_it->second.end(); ++it) {
                                       float diff = target_media_time - it->first; // Timestamp is the key
                                       if (diff >= -0.01f && diff < best_diff) { // Find closest at or slightly before
                                           best_diff = diff;
                                           best_match = it;
                                       }
                                   }
                                   // Fallback to first if no match found before target
                                   if(best_match == gen_map_it->second.end() && !gen_map_it->second.empty()) {
                                        best_match = gen_map_it->second.begin();
                                   }
                                   if (best_match != gen_map_it->second.end()) {
                                       tex_id = best_match->second;
                                   }
                              }


                             if (tex_id != 0) {
                                 ImVec2 thumb_min_pos(thumb_start_x_rel + thumbnail_margin, node_min.y + thumbnail_margin);
                                 ImVec2 thumb_max_pos(thumb_start_x_rel + thumb_draw_w + thumbnail_margin, node_max.y - thumbnail_margin);

                                 // Clip thumbnail drawing to node bounds and canvas bounds
                                 thumb_min_pos.x = std::max(thumb_min_pos.x, node_min.x);
                                 thumb_max_pos.x = std::min(thumb_max_pos.x, node_max.x);
                                 thumb_min_pos.x = std::max(thumb_min_pos.x, timeline_content_origin.x); // Clip left edge of canvas
                                 thumb_max_pos.x = std::min(thumb_max_pos.x, timeline_content_origin.x + timeline_area_width); // Clip right edge

                                 if (thumb_max_pos.x > thumb_min_pos.x && thumb_max_pos.y > thumb_min_pos.y) {
                                      child_draw_list->AddImage((ImTextureID)(intptr_t)tex_id, thumb_min_pos, thumb_max_pos, ImVec2(0,0), ImVec2(1,1));
                                 }
                             } else { /* Optional: Draw loading placeholder */ }
                         }
                     }
                 }
                 // Audio Waveform
                 else if (mn->is_audio && mn->source_clip && !mn->source_clip->waveform.empty()) {
                     const auto& wf = mn->source_clip->waveform;
                     int wf_samples = wf.size();
                     if(mn->source_clip->source_duration > 0 && wf_samples > 0) {
                         float samples_per_sec_wf = wf_samples / mn->source_clip->source_duration;
                         float center_y = node_min.y + layer_height / 2.0f;
                         float max_h = (layer_height / 2.0f) * waveform_height_scale;

                         int start_sample_idx_in_clip = static_cast<int>(mn->media_start * samples_per_sec_wf);
                         int end_sample_idx_in_clip = static_cast<int>((mn->media_start + node->duration) * samples_per_sec_wf);

                         // Optimize drawing by only iterating over visible pixel columns
                         float pixels_per_sample = pixels_per_second / samples_per_sec_wf;
                         int start_pixel_x = static_cast<int>(node_min.x);
                         int end_pixel_x = static_cast<int>(node_max.x);

                         for(int px = start_pixel_x; px < end_pixel_x; ++px) {
                             // Find corresponding sample index
                             float time_at_px = (px - node_min.x) / pixels_per_second;
                             int s = static_cast<int>((mn->media_start + time_at_px) * samples_per_sec_wf);

                             if (s >= 0 && s < wf_samples) {
                                 float x = node_min.x + time_at_px * pixels_per_second;
                                 // Cull samples outside visible range more precisely
                                 if (x < timeline_content_origin.x - 1 || x > timeline_content_origin.x + timeline_area_width + 1) continue;

                                 float amp = wf[s];
                                 float h = max_h * amp;
                                 child_draw_list->AddLine(ImVec2(x, center_y - h), ImVec2(x, center_y + h), col_waveform);
                             }
                         }
                     }
                 }
             }
         }


        // Draw Node Name (clipped to node rect)
        child_draw_list->PushClipRect(node_min, node_max, true);
        ImVec2 text_pos = ImVec2(node_min.x + 5, node_min.y + 3);
        child_draw_list->AddText(ImVec2(text_pos.x + 1, text_pos.y + 1), col_text_shadow, node->name.c_str());
        child_draw_list->AddText(text_pos, col_text, node->name.c_str());
        child_draw_list->PopClipRect();

        // --- Interaction (using InvisibleButton placed over the node) ---
        ImGui::SetCursorScreenPos(node_min); // Position button relative to child window origin
        std::string node_id_str = "node" + std::to_string(node->id);
        ImGui::InvisibleButton(node_id_str.c_str(), ImVec2(node_max.x - node_min.x, node_max.y - node_min.y));

        bool hovered = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem); // Allow hover even if dragging/resizing another item
        bool active = ImGui::IsItemActive();
        // Use mouse position relative to the *start* of the virtual content area for time calculations
        ImVec2 mouse_pos_abs = ImVec2(
            (ImGui::GetMousePos().x - timeline_content_origin.x) + current_scroll_x,
            (ImGui::GetMousePos().y - timeline_content_origin.y) + current_scroll_y
        );

        // Selection
        if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && dragging_node_id == -1 && resizing_node_id == -1) {
            selected_node = node;
            clicked_on_item_this_frame = true;
            needs_redraw = true;
        }

        // Dragging
        if (active && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
            if (dragging_node_id == -1 && resizing_node_id == -1) {
                dragging_node_id = node->id;
                selected_node = node;
                drag_offset_x = mouse_pos_abs.x - node_start_x_abs; // Offset from absolute start
            }
            if (dragging_node_id == (int)node->id) {
                float new_start_x_abs = mouse_pos_abs.x - drag_offset_x;
                float new_start_time = new_start_x_abs / pixels_per_second;
                float delta_time = new_start_time - node->start_time;
                node->start_time = std::max(0.0f, new_start_time);
                needs_redraw = true;
                // Drag linked node
                 if (node->type == NodeType::Media) {
                     MediaNode* mn = static_cast<MediaNode*>(node);
                     auto linked = mn->linked_node.lock();
                     if (linked) {
                         linked->start_time = std::max(0.0f, linked->start_time + delta_time);
                     }
                 }
            }
        } else if (dragging_node_id == (int)node->id) {
            dragging_node_id = -1;
        }

        // Resizing Handles (relative screen positions)
        ImVec2 left_handle_min = ImVec2(node_min.x, node_min.y);
        ImVec2 left_handle_max = ImVec2(node_min.x + handle_width, node_max.y);
        ImVec2 right_handle_min = ImVec2(node_max.x - handle_width, node_min.y);
        ImVec2 right_handle_max = ImVec2(node_max.x, node_max.y);

        ImGui::SetCursorScreenPos(left_handle_min);
        ImGui::InvisibleButton((node_id_str + "L").c_str(), ImVec2(handle_width, layer_height));
        bool left_active = ImGui::IsItemActive();
        bool left_hovered = ImGui::IsItemHovered();
        if (left_hovered || (resizing_node_id == (int)node->id && resizing_left)) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);

        ImGui::SetCursorScreenPos(right_handle_min);
        ImGui::InvisibleButton((node_id_str + "R").c_str(), ImVec2(handle_width, layer_height));
        bool right_active = ImGui::IsItemActive();
        bool right_hovered = ImGui::IsItemHovered();
        if (right_hovered || (resizing_node_id == (int)node->id && !resizing_left)) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);


        // Start Resizing
        if ((left_active || right_active) && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
            if (resizing_node_id == -1 && dragging_node_id == -1) {
                resizing_node_id = node->id;
                resizing_left = left_active;
                selected_node = node;
            }
        }

        // Perform Resizing
        if (resizing_node_id == (int)node->id) {
            float mouse_time = mouse_pos_abs.x / pixels_per_second;
            mouse_time = std::max(0.0f, mouse_time);
            float min_duration_time = 1.0f / pixels_per_second; // Minimum 1 pixel duration

            if (resizing_left) {
                float original_end_time = node->start_time + node->duration;
                float new_start_time = std::min(mouse_time, original_end_time - min_duration_time);
                new_start_time = std::max(0.0f, new_start_time);

                float delta_time = new_start_time - node->start_time;
                float new_duration = original_end_time - new_start_time;

                node->start_time = new_start_time;
                node->duration = new_duration;

                if (node->type == NodeType::Media) {
                    MediaNode* mn = static_cast<MediaNode*>(node);
                     mn->media_start = std::max(0.0f, mn->media_start + delta_time);
                    // Sync linked node
                     auto linked = mn->linked_node.lock();
                     if(linked) {
                        linked->start_time = node->start_time;
                        linked->duration = node->duration;
                        if(linked->type == NodeType::Media) {
                            static_cast<MediaNode*>(linked.get())->media_start = std::max(0.0f, static_cast<MediaNode*>(linked.get())->media_start + delta_time);
                        }
                     }
                }
            } else { // Resizing right handle
                float new_duration = mouse_time - node->start_time;
                node->duration = std::max(min_duration_time, new_duration);
                 // Sync linked node
                 if (node->type == NodeType::Media) {
                     MediaNode* mn = static_cast<MediaNode*>(node);
                     auto linked = mn->linked_node.lock();
                     if (linked) {
                         linked->duration = node->duration;
                     }
                 }
            }
            needs_redraw = true;
        }

        // Stop Resizing
        if (resizing_node_id == (int)node->id && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            resizing_node_id = -1;
        }

        // Selection Highlight (using child_draw_list)
        if (node == selected_node) {
            child_draw_list->AddRect(node_min, node_max, col_selection, 2.0f, 0, 2.0f);
        }

        // Process Children recursively
        for (const auto& child : node->children) {
            draw_node_recursive_child(child.get());
        }

    }; // End of lambda draw_node_recursive_child

    // Start drawing nodes within the child window
    for (const auto& root : root_nodes) {
        draw_node_recursive_child(root.get());
    }

    // --- Playhead (Draw within child window, clipped) ---
    float playhead_x_abs = playhead_time * pixels_per_second;
    float playhead_x_rel = timeline_content_origin.x + playhead_x_abs - current_scroll_x;

    // Only draw if visible within child bounds
    if (playhead_x_rel >= timeline_content_origin.x && playhead_x_rel <= timeline_content_origin.x + timeline_area_width) {
        child_draw_list->AddLine(ImVec2(playhead_x_rel, timeline_content_origin.y),
                                 ImVec2(playhead_x_rel, timeline_content_origin.y + total_timeline_height),
                                 col_playhead, 2.0f);
    }

    // End Child Window
    ImGui::EndChild();

    // --- Draw Playhead Triangle Head (Outside child window, on ruler area) ---
    float playhead_x_canvas = timeline_canvas_min.x + playhead_x_abs - current_scroll_x;
    if (playhead_x_canvas >= timeline_canvas_min.x && playhead_x_canvas <= timeline_canvas_max.x) {
        draw_list->AddTriangleFilled(ImVec2(playhead_x_canvas, ruler_area_max.y),             // Point rests on timeline top
                                     ImVec2(playhead_x_canvas - 6, ruler_area_max.y - 10),  // Top left point
                                     ImVec2(playhead_x_canvas + 6, ruler_area_max.y - 10),  // Top right point
                                     col_playhead);
    }

    // --- Playhead/Background Interaction (Clicking on Ruler/Canvas) ---
    // Use the ruler area for clicking/dragging the playhead
    ImGui::SetCursorScreenPos(ruler_area_min);
    ImGui::InvisibleButton("ruler_interaction_area", ImVec2(timeline_area_width, ruler_height));
    bool ruler_hovered = ImGui::IsItemHovered();
    bool ruler_active = ImGui::IsItemActive();

    if (ruler_hovered || ruler_active) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);

    if (ruler_active && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
        ImVec2 mouse_pos_canvas = ImVec2(ImGui::GetMousePos().x - timeline_canvas_min.x, ImGui::GetMousePos().y - timeline_canvas_min.y); // Relative to canvas start
        playhead_time = (mouse_pos_canvas.x + current_scroll_x) / pixels_per_second;
        playhead_time = std::clamp(playhead_time, 0.0f, view_duration);
        playing = false;
        needs_redraw = true;
        // No need to assign last_preview_update_time here
    } else if (ruler_hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        ImVec2 mouse_pos_canvas = ImVec2(ImGui::GetMousePos().x, ImGui::GetMousePos().y);
        mouse_pos_canvas.x -= timeline_canvas_min.x;
        mouse_pos_canvas.y -= timeline_canvas_min.y;
        playhead_time = (mouse_pos_canvas.x + current_scroll_x) / pixels_per_second;
        playhead_time = std::clamp(playhead_time, 0.0f, view_duration);
        playing = false;
        needs_redraw = true;
        // No need to assign last_preview_update_time here
    }

    // Deselect node if clicking on timeline background (child window area)
    ImGui::SetCursorScreenPos(timeline_canvas_min);
    ImGui::InvisibleButton("timeline_bg_deselect", ImVec2(timeline_area_width, total_timeline_height));
    if (!clicked_on_item_this_frame && ImGui::IsItemClicked(ImGuiMouseButton_Left) && ImGui::IsWindowHovered()) {
         selected_node = nullptr;
         needs_redraw = true;
    }


    // --- Timeline Drag and Drop Target (covers the child window area) ---
     ImGui::SetCursorScreenPos(timeline_canvas_min); // DND target covers the whole canvas
     if (ImGui::BeginDragDropTarget()) {
          if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("DND_MEDIA_PATH")) {
             std::string dropped_path((const char*)payload->Data, payload->DataSize -1);
             ImVec2 mouse_pos_canvas = ImVec2(ImGui::GetMousePos().x, ImGui::GetMousePos().y);
             mouse_pos_canvas.x -= timeline_canvas_min.x;
             mouse_pos_canvas.y -= timeline_canvas_min.y;
             float drop_time = (mouse_pos_canvas.x + current_scroll_x) / pixels_per_second;
             drop_time = std::max(0.0f, drop_time);
             int layer_draw_idx = static_cast<int>(mouse_pos_canvas.y / (layer_height + layer_padding));
             int target_layer = max_layer - layer_draw_idx;
             target_layer = std::clamp(target_layer, min_layer, max_layer);

             AddMediaToTimeline(dropped_path, drop_time, target_layer);
         }
         ImGui::EndDragDropTarget();
     }

    ImGui::End(); // End Timeline Editor Window
}

// --- DrawKeyframeTrackEditor ---
template<typename T>
void DrawKeyframeTrackEditor(const std::string& label, KeyframeTrack<T>& track, std::atomic<bool>& changed_flag) {
    if (ImGui::TreeNode(label.c_str())) {
        int to_remove = -1;
        bool changed_here = false;

        for (size_t i = 0; i < track.keyframes.size(); ++i) {
            auto& kf = track.keyframes[i];
            ImGui::PushID(static_cast<int>(i));

            ImGui::SetNextItemWidth(80);
            if (ImGui::DragFloat("Time", &kf.time, 0.01f, 0.0f, 9999.0f, "%.3f")) changed_here = true;

            ImGui::SameLine(); ImGui::SetNextItemWidth(120);
            // Use DragScalar for floats, adapt if T is different
            if constexpr (std::is_same_v<T, float>) {
                if (ImGui::DragScalar("Value", ImGuiDataType_Float, &kf.value, 0.01f)) changed_here = true;
            } else {
                 // Need specific editor for other types (e.g., Vec2, Color)
                 ImGui::Text("Value: [Type Editor TBD]");
            }


            ImGui::SameLine(); ImGui::SetNextItemWidth(100);
            const char* interp_labels[] = {"Linear", "EaseInOut", "Hold"};
            int interp_idx = static_cast<int>(kf.interp);
            if (ImGui::Combo("Interp", &interp_idx, interp_labels, IM_ARRAYSIZE(interp_labels))) {
                kf.interp = static_cast<InterpolationType>(interp_idx);
                changed_here = true;
            }

            ImGui::SameLine();
            if (ImGui::Button("X")) { to_remove = static_cast<int>(i); }

            ImGui::PopID();
        }

        if (to_remove >= 0) {
            track.keyframes.erase(track.keyframes.begin() + to_remove);
            // Sort keyframes after removal/modification? Should maintain sort order.
            changed_here = true;
        }

        if (ImGui::Button("Add Keyframe")) {
            // Add at current playhead time if possible? Or default?
             float new_time = playhead_time; // Use global playhead
             // Find appropriate default value (e.g., current evaluated value or fallback)
             // This requires access to the node itself. Pass node pointer maybe?
             // For now, use default T{}
             T default_value = T{};
             if constexpr (std::is_same_v<T, float>) {
                  // Get default from selected_node if available? Complicates function signature.
                  // default_value = selected_node ? selected_node->Evaluate...(new_time) : T{}; // Example
             }


             track.keyframes.push_back(Keyframe<T>{new_time, default_value, InterpolationType::Linear});
            // Sort keyframes after adding
            std::sort(track.keyframes.begin(), track.keyframes.end(), [](const Keyframe<T>& a, const Keyframe<T>& b){
                return a.time < b.time;
            });
            changed_here = true;
        }

        if (changed_here) {
             changed_flag = true; // Signal that something changed
             // Sort keyframes if time was modified
             std::sort(track.keyframes.begin(), track.keyframes.end(), [](const Keyframe<T>& a, const Keyframe<T>& b){
                 return a.time < b.time;
             });
        }

        ImGui::TreePop();
    }
}