#pragma once
#include <string>

struct Clip {
    std::string name;
    int start_time;
    int duration;
    int layer;
    std::string path;
};