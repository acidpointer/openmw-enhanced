#ifndef OPENMW_COMPONENTS_RENDER_MATH_H
#define OPENMW_COMPONENTS_RENDER_MATH_H

#include <components/renderassets/model.hpp>

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace Render
{
    inline Matrix multiply(const Matrix& left, const Matrix& right)
    {
        Matrix result{};
        for (std::size_t column = 0; column < 4; ++column)
            for (std::size_t row = 0; row < 4; ++row)
                for (std::size_t k = 0; k < 4; ++k)
                    result[column * 4 + row] += left[k * 4 + row] * right[column * 4 + k];
        return result;
    }

    inline void validateTransform(const Matrix& matrix)
    {
        if (!std::all_of(matrix.begin(), matrix.end(), [](float value) { return std::isfinite(value); })
            || matrix[3] != 0 || matrix[7] != 0 || matrix[11] != 0 || matrix[15] != 1)
            throw std::invalid_argument("Model transform must be finite and affine");
    }

    inline void validateBounds(const Bounds& bounds)
    {
        for (std::size_t i = 0; i < 3; ++i)
            if (!std::isfinite(bounds.mMin[i]) || !std::isfinite(bounds.mMax[i]) || bounds.mMin[i] > bounds.mMax[i])
                throw std::invalid_argument("Model bounds must be finite and ordered");
    }

    inline Bounds transformBounds(const Bounds& bounds, const Matrix& matrix)
    {
        Bounds result{};
        for (std::size_t row = 0; row < 3; ++row)
        {
            result.mMin[row] = matrix[12 + row];
            result.mMax[row] = matrix[12 + row];
            for (std::size_t column = 0; column < 3; ++column)
            {
                const float a = matrix[column * 4 + row] * bounds.mMin[column];
                const float b = matrix[column * 4 + row] * bounds.mMax[column];
                result.mMin[row] += std::min(a, b);
                result.mMax[row] += std::max(a, b);
            }
        }
        return result;
    }

    inline bool reversesWinding(const Matrix& m)
    {
        const double determinant = double(m[0]) * (double(m[5]) * m[10] - double(m[9]) * m[6])
            - double(m[4]) * (double(m[1]) * m[10] - double(m[9]) * m[2])
            + double(m[8]) * (double(m[1]) * m[6] - double(m[5]) * m[2]);
        return determinant < 0;
    }

    class Frustum
    {
    public:
        explicit Frustum(const Matrix& matrix)
        {
            if (!std::all_of(matrix.begin(), matrix.end(), [](float value) { return std::isfinite(value); }))
                throw std::invalid_argument("View projection must be finite");
            for (std::size_t i = 0; i < 4; ++i)
            {
                mPlanes[0][i] = double(matrix[4 * i + 3]) + matrix[4 * i];
                mPlanes[1][i] = double(matrix[4 * i + 3]) - matrix[4 * i];
                mPlanes[2][i] = double(matrix[4 * i + 3]) + matrix[4 * i + 1];
                mPlanes[3][i] = double(matrix[4 * i + 3]) - matrix[4 * i + 1];
                // The renderer uses zero-to-one clip depth.
                mPlanes[4][i] = matrix[4 * i + 2];
                mPlanes[5][i] = double(matrix[4 * i + 3]) - matrix[4 * i + 2];
            }
        }

        bool intersects(const Bounds& bounds) const
        {
            for (const auto& plane : mPlanes)
            {
                double distance = plane[3];
                double magnitude = std::abs(plane[3]);
                for (std::size_t i = 0; i < 3; ++i)
                {
                    const auto term = plane[i] * (plane[i] >= 0 ? bounds.mMax[i] : bounds.mMin[i]);
                    distance += term;
                    magnitude += std::abs(term);
                }
                // Retain borderline geometry despite floating-point roundoff.
                if (distance < -1e-6 * std::max(1.0, magnitude))
                    return false;
            }
            return true;
        }

    private:
        std::array<std::array<double, 4>, 6> mPlanes;
    };
}

#endif
