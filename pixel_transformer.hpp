#ifndef PIXEL_TRANSFORMER_H
#define PIXEL_TRANSFORMER_H

#include <opencv2/opencv.hpp>
#include <vector>
#include <array>
#include <utility>
#include <cmath>

class PixelGeoTransformer {
private:
    std::array<double, 9> H_flat;
    bool is_valid = false;
    std::vector<cv::Point2d> src_points;
    std::vector<cv::Point2d> dst_points;

    void calculate_homography();
public:
    PixelGeoTransformer(const std::vector<cv::Point2d>& points_pixels,
                        const std::vector<cv::Point2d>& points_geo);
    /**
     * @brief Преобразует одну пиксельную точку в географическую.
     * Максимально оптимизировано: нет вызовов OpenCV, только базовая математика.
     */
    std::pair<double, double> pixel_to_geo(double x_pixel, double y_pixel) const;
    /**
     * @brief Пакетное преобразование множества точек.
     * В C++ никогда не стоит преобразовывать тысячи точек по одной в цикле.
     */
    std::vector<std::pair<double, double>> batch_pixel_to_geo(
        const std::vector<cv::Point2d>& pixels) const;
    // Геттер для проверки состояния
    bool isValid() const;
};

#endif // PIXEL_TRANSFORMER_H
