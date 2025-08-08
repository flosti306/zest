// cv_utils.cpp
#include "cv_utils.hpp"
#include "effects.hpp"
#include <glad/glad.h> // For GLuint and OpenGL calls
#include <iostream>

cv::Mat DecodedFrameToCvMat(const DecodedFrame& frame_data) {
    if (frame_data.pixels.empty() || frame_data.width <= 0 || frame_data.height <= 0) {
        return cv::Mat();
    }
    // Create a Mat sharing the data (no copy). Ensure lifetime of frame_data.pixels!
    // Or, for safety, make a copy:
    // cv::Mat(frame_data.height, frame_data.width, CV_8UC3, (void*)frame_data.pixels.data()).copyTo(output_mat);
    // For GrabCut, BGR is often preferred, so we might need conversion if pixels are RGB
    cv::Mat rgb_mat(frame_data.height, frame_data.width, CV_8UC3, (void*)frame_data.pixels.data());
    cv::Mat bgr_mat;
    cv::cvtColor(rgb_mat, bgr_mat, cv::COLOR_RGB2BGR);
    return bgr_mat;
}

GLuint GrabCutMaskToRGBTexture(const cv::Mat& grabcut_mask_cv, int& out_width, int& out_height, bool make_fg_white) {
    if (grabcut_mask_cv.empty() || grabcut_mask_cv.type() != CV_8UC1) {
        std::cerr << "Invalid GrabCut mask format for texture conversion." << std::endl;
        return 0;
    }

    out_width = grabcut_mask_cv.cols;
    out_height = grabcut_mask_cv.rows;

    cv::Mat display_mask_rgb(out_height, out_width, CV_8UC3);

    for (int y = 0; y < out_height; ++y) {
        for (int x = 0; x < out_width; ++x) {
            uchar val = grabcut_mask_cv.at<uchar>(y, x);
            cv::Vec3b& pixel = display_mask_rgb.at<cv::Vec3b>(y, x);
            if (val == cv::GC_FGD || val == cv::GC_PR_FGD) { // Foreground or Probable Foreground
                pixel = make_fg_white ? cv::Vec3b(255, 255, 255) : cv::Vec3b(0,0,0); // White for FG
            } else { // Background or Probable Background
                pixel = make_fg_white ? cv::Vec3b(0, 0, 0) : cv::Vec3b(255,255,255);       // Black for BG
            }
        }
    }
    
    // Flip if OpenCV origin (top-left) differs from GL texture upload convention (bottom-left)
    // cv::flip(display_mask_rgb, display_mask_rgb, 0); // Often needed

    GLuint tex_id = 0;
    glGenTextures(1, &tex_id);
    glBindTexture(GL_TEXTURE_2D, tex_id);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, out_width, out_height, 0, GL_BGR, GL_UNSIGNED_BYTE, display_mask_rgb.data); // Use GL_BGR if display_mask_rgb is BGR
    // If display_mask_rgb was converted to RGB: glTexImage2D(..., GL_RGB, ..., display_mask_rgb.data);

    GLenum err = glGetError();
    if (err != GL_NO_ERROR) {
        std::cerr << "OpenGL Error creating GrabCut mask texture: " << err << std::endl;
        glDeleteTextures(1, &tex_id);
        return 0;
    }

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);

    return tex_id;
}

cv::Ptr<cv::Tracker> InitializeTrackerByName(const std::string& tracker_name) {
    if (tracker_name == "CSRT") return cv::TrackerCSRT::create();
    if (tracker_name == "KCF") return cv::TrackerKCF::create();
    // Add other trackers like MOSSE, etc. if needed
    std::cerr << "Warning: Unknown tracker name '" << tracker_name << "'. Defaulting to CSRT." << std::endl;
    return cv::TrackerCSRT::create();
}

bool UpdateTracker(cv::Ptr<cv::Tracker> tracker, const cv::Mat& frame, cv::Rect2f& tracked_box) {
    if (!tracker || frame.empty()) return false;

    // 1. Create a temporary integer-based Rect for the API call.
    cv::Rect tracked_box_int = tracked_box;

    // 2. Call the update function with the correct integer-based type.
    bool success = tracker->update(frame, tracked_box_int);

    // 3. If tracking was successful, convert the result back to a float-based Rect.
    if (success) {
        tracked_box = tracked_box_int;
    }

    return success;
}

TransformData CalculateTransformFromBoxes(const cv::Rect2f& initial_box, const cv::Rect2f& current_box) {
    TransformData t_data;

    if (initial_box.width == 0 || initial_box.height == 0) return t_data;

    // Calculate scale (non-uniform)
    t_data.scale.x = current_box.width / initial_box.width;
    t_data.scale.y = current_box.height / initial_box.height;

    // Calculate translation (center to center)
    cv::Point2f initial_center = initial_box.tl() + cv::Point2f(initial_box.width * 0.5f, initial_box.height * 0.5f);
    cv::Point2f current_center = current_box.tl() + cv::Point2f(current_box.width * 0.5f, current_box.height * 0.5f);
    
    // Normalize translation to [-0.5, 0.5] range (assuming input is normalized later)
    t_data.translate.x = current_center.x - initial_center.x;
    t_data.translate.y = current_center.y - initial_center.y;

    // Note: Simple trackers don't provide reliable rotation. We leave it at 0.
    t_data.rotation = 0.0f;

    return t_data;
}