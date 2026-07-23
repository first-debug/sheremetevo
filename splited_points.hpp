#ifndef POINTS_HPP
#define POINTS_HPP

#include "opencv2/core/types.hpp"
#include <vector>

typedef struct {
    std::vector<cv::Point2d> pixels_cam1;
    std::vector<cv::Point2d> geo_cam1;
    std::vector<cv::Point2d> pixels_cam2;
    std::vector<cv::Point2d> geo_cam2;
    std::vector<cv::Point2d> pixels_cam3;
    std::vector<cv::Point2d> geo_cam3;
    std::vector<cv::Point2d> pixels_cam4;
    std::vector<cv::Point2d> geo_cam4;
} points_struct;

void init_points(points_struct &);

#endif // POINTS_HPP

