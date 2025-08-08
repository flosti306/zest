// cv_utils.hpp
#pragma once

#include <opencv2/opencv.hpp> // Main OpenCV header
#include <opencv2/tracking/tracking.hpp>
#include <opencv2/opencv_modules.hpp>
#include <vector>
#include <string>

// Dependencies for GLuint and DecodedFrame
#include <glad/glad.h>      // For GLuint (should be included before any other GL headers)
#include "video_export.hpp" // For DecodedFrame (assuming DecodedFrame is defined here or in a header it includes)
                            // Or, if DecodedFrame is in shared.hpp, include that.
                            // Make sure the path is correct relative to cv_utils.hpp

struct TransformData;

// Function Declarations
cv::Mat DecodedFrameToCvMat(const DecodedFrame& frame_data);
GLuint GrabCutMaskToRGBTexture(const cv::Mat& grabcut_mask_cv, int& out_width, int& out_height, bool make_fg_white = true);

// Initializes a specific OpenCV tracker
cv::Ptr<cv::Tracker> InitializeTrackerByName(const std::string& tracker_name = "CSRT");

// Updates the tracker with a new frame and returns the tracked position
bool UpdateTracker(cv::Ptr<cv::Tracker> tracker, const cv::Mat& frame, cv::Rect2f& tracked_box);

// Calculates the transformation between the initial and current box
TransformData CalculateTransformFromBoxes(const cv::Rect2f& initial_box, const cv::Rect2f& current_box);