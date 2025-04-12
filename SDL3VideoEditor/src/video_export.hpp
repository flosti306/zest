#pragma once

#include <SDL_opengl.h>
#include <cstdio>
#include <vector>
#include <functional>
#include <iostream>
#include <algorithm>
#include <SDL.h>
#include <SDL_render.h>
#include <string>
#include "shared.hpp"


struct Clip;

bool start_video_export(const std::string& output_path, 
                       int width, int height, int fps,
                       int duration_frames,
                       const std::vector<Clip>& clips,
                       SDL_Window* window);
