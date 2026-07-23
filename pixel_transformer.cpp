#include <opencv2/opencv.hpp>
#include <cmath>
#include "pixel_transformer.hpp"

void PixelGeoTransformer::calculate_homography() {
    if (src_points.size() < 4 || dst_points.size() < 4) {
        throw std::invalid_argument("Для вычисления матрицы гомографии требуется минимум 4 точки.");
    }
    if (src_points.size() != dst_points.size()) {
        throw std::invalid_argument("Количество пиксельных и географических точек должно совпадать.");
    }

    // Вычисляем матрицу.
    // Примечание: для реальных данных лучше использовать cv::RANSAC
    cv::Mat H_mat = cv::findHomography(src_points, dst_points, cv::RANSAC);

    if (H_mat.empty()) {
        throw std::runtime_error("Не удалось вычислить матрицу гомографии.");
    }

    // Приводим к double (CV_64F) для сохранения точности координат (lat/lon)
    cv::Mat H_double;
    H_mat.convertTo(H_double, CV_64F);

    // Разворачиваем матрицу 3x3 в плоский массив
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            H_flat[i * 3 + j] = H_double.at<double>(i, j);
        }
    }
    is_valid = true;
}

PixelGeoTransformer::PixelGeoTransformer(const std::vector<cv::Point2d>& points_pixels,
                    const std::vector<cv::Point2d>& points_geo)
    : src_points(points_pixels), dst_points(points_geo) {
    calculate_homography();
}

std::pair<double, double> PixelGeoTransformer::pixel_to_geo(double x_pixel, double y_pixel) const {
    if (!is_valid) {
        throw std::runtime_error("Матрица гомографии не инициализирована.");
    }

    // H_flat layout:
    // [0] [1] [2]
    // [3] [4] [5]
    // [6] [7] [8]

    double w = H_flat[6] * x_pixel + H_flat[7] * y_pixel + H_flat[8];

    // Защита от деления на ноль (точка уходит в бесконечность)
    if (std::abs(w) < 1e-9) {
        throw std::runtime_error("Ошибка проективного преобразования: деление на ноль.");
    }

    double inv_w = 1.0 / w;
    double lon = (H_flat[0] * x_pixel + H_flat[1] * y_pixel + H_flat[2]) * inv_w;
    double lat = (H_flat[3] * x_pixel + H_flat[4] * y_pixel + H_flat[5]) * inv_w;

    return {lon, lat};
}

std::vector<std::pair<double, double>> PixelGeoTransformer::batch_pixel_to_geo(
    const std::vector<cv::Point2d>& pixels) const {

    if (!is_valid) {
        throw std::runtime_error("Матрица гомографии не инициализирована.");
    }

    std::vector<std::pair<double, double>> result;
    result.reserve(pixels.size()); // Предварительное выделение памяти

    for (const auto& p : pixels) {
        double w = H_flat[6] * p.x + H_flat[7] * p.y + H_flat[8];
        if (std::abs(w) < 1e-9) w = 1e-9; // Fallback

        double inv_w = 1.0 / w;
        double lon = (H_flat[0] * p.x + H_flat[1] * p.y + H_flat[2]) * inv_w;
        double lat = (H_flat[3] * p.x + H_flat[4] * p.y + H_flat[5]) * inv_w;

        result.emplace_back(lon, lat);
    }
    return result;
}

// Геттер для проверки состояния
bool PixelGeoTransformer::isValid() const { return is_valid; }
