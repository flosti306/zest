// cv_utils.hpp
#pragma once

#include <opencv2/opencv.hpp> // Main OpenCV header
#include <vector>
#include <string>

// Dependencies for GLuint and DecodedFrame
#include <glad/glad.h>      // For GLuint (should be included before any other GL headers)
#include "video_export.hpp" // For DecodedFrame (assuming DecodedFrame is defined here or in a header it includes)
                            // Or, if DecodedFrame is in shared.hpp, include that.
                            // Make sure the path is correct relative to cv_utils.hpp

// Function Declarations
cv::Mat DecodedFrameToCvMat(const DecodedFrame& frame_data);
GLuint GrabCutMaskToRGBTexture(const cv::Mat& grabcut_mask_cv, int& out_width, int& out_height, bool make_fg_white = true);