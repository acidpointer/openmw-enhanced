#ifndef OPENMW_COMPONENTS_SCENEUTIL_SCREENCOVERAGE_H
#define OPENMW_COMPONENTS_SCENEUTIL_SCREENCOVERAGE_H

#include <algorithm>
#include <cmath>
#include <optional>

#include <osg/BoundingBox>
#include <osg/Matrix>
#include <osg/Vec4>

namespace SceneUtil
{
    // Project conservative drawable bounds. Anything crossing the eye plane or
    // lacking finite bounds remains unknown and must use the full-screen path.
    inline std::optional<osg::Vec4f> screenCoverage(
        const osg::BoundingBox& bounds, const osg::Matrix& modelView, const osg::Matrix& projection)
    {
        if (!bounds.valid())
            return std::nullopt;
        osg::Vec4f result(1, 1, 0, 0);
        const osg::Matrixd transform = osg::Matrixd(modelView) * osg::Matrixd(projection);
        for (unsigned int i = 0; i < 8; ++i)
        {
            const auto corner = bounds.corner(i);
            const osg::Vec4d clip = osg::Vec4d(corner.x(), corner.y(), corner.z(), 1) * transform;
            if (!std::isfinite(clip.x()) || !std::isfinite(clip.y()) || !std::isfinite(clip.w()) || clip.w() <= 1e-6)
                return std::nullopt;
            const double x = std::clamp(clip.x() / clip.w() * 0.5 + 0.5, 0.0, 1.0);
            const double y = std::clamp(clip.y() / clip.w() * 0.5 + 0.5, 0.0, 1.0);
            result.x() = std::min(result.x(), static_cast<float>(x));
            result.y() = std::min(result.y(), static_cast<float>(y));
            result.z() = std::max(result.z(), static_cast<float>(x));
            result.w() = std::max(result.w(), static_cast<float>(y));
        }
        return result;
    }
}

#endif
