#include "particles.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>

namespace RenderAssets
{
    std::uint32_t importParticles(const osgParticle::ParticleSystem& source, const osg::Matrixd& viewFromModel,
        std::vector<Render::Vertex>& vertices, std::vector<std::uint32_t>& indices, std::vector<int>& order)
    {
        osgParticle::ParticleSystem::ScopedReadLock lock(*source.getReadWriteMutex());
        order.clear();
        order.reserve(source.numParticles());
        const int detail = std::max(1, source.getLevelOfDetail());
        for (int i = 0; i < source.numParticles(); i += detail)
            if (source.getParticle(i)->isAlive())
            {
                const auto center = source.getParticle(i)->getPosition() * viewFromModel;
                if (center.valid()
                    && (source.getSortMode() == osgParticle::ParticleSystem::NO_SORT
                        || source.getVisibilityDistance() <= 0
                        || (-center.z() >= 0 && -center.z() <= source.getVisibilityDistance())))
                    order.push_back(i);
            }
        if (order.size() > std::numeric_limits<std::uint32_t>::max() / 6)
            throw std::length_error("Too many visible particles");
        if (source.getSortMode() != osgParticle::ParticleSystem::NO_SORT)
            std::stable_sort(order.begin(), order.end(), [&](int a, int b) {
                const auto zA = (source.getParticle(a)->getPosition() * viewFromModel).z();
                const auto zB = (source.getParticle(b)->getPosition() * viewFromModel).z();
                return source.getSortMode() == osgParticle::ParticleSystem::SORT_BACK_TO_FRONT ? zA < zB : zA > zB;
            });
        std::size_t capacity = vertices.size() / 4;
        if (order.size() > capacity)
        {
            capacity = std::max(order.size(), std::max(std::size_t{ 16 }, capacity * 2));
            capacity = std::min(capacity, std::size_t(std::numeric_limits<std::uint32_t>::max() / 6));
            vertices.resize(capacity * 4);
            indices.resize(capacity * 6);
            constexpr std::array<unsigned, 6> triangle{ 0, 1, 2, 0, 2, 3 };
            for (std::size_t i = 0; i < capacity; ++i)
                for (unsigned j = 0; j < triangle.size(); ++j)
                    indices[i * 6 + j] = static_cast<std::uint32_t>(i * 4 + triangle[j]);
        }
        const bool localScale
            = source.getParticleScaleReferenceFrame() == osgParticle::ParticleSystem::LOCAL_COORDINATES;
        const auto axis = [&](const osg::Vec3& value) { return osg::Matrixd::transform3x3(value, viewFromModel); };
        const bool billboard = source.getParticleAlignment() == osgParticle::ParticleSystem::BILLBOARD;
        osg::Vec3d baseX(source.getAlignVectorX()), baseY(source.getAlignVectorY());
        if (billboard)
        {
            const double lengthX2 = osg::Matrixd::transform3x3(viewFromModel, baseX).length2();
            const double lengthY2 = osg::Matrixd::transform3x3(viewFromModel, baseY).length2();
            if (lengthX2 < 1e-20 || lengthY2 < 1e-20)
                return 0;
            baseX /= localScale ? std::sqrt(lengthX2) : lengthX2;
            baseY /= localScale ? std::sqrt(lengthY2) : lengthY2;
        }
        std::uint32_t count = 0;
        for (int index : order)
        {
            const auto& particle = *source.getParticle(index);
            auto center = particle.getPosition() * viewFromModel;
            const float size = particle.getCurrentSize() * std::sqrt(static_cast<float>(detail));
            const auto color = particle.getCurrentColor();
            if (!center.valid() || !std::isfinite(size) || size <= 0 || !color.valid()
                || !std::isfinite(particle.getCurrentAlpha()) || !particle.getAngle().valid())
                continue;
            const auto angle = particle.getAngle();
            const osg::Quat rotation(
                angle.x(), osg::Vec3(1, 0, 0), angle.y(), osg::Vec3(0, 1, 0), angle.z(), osg::Vec3(0, 0, 1));
            const auto rotatedAxis = [&](const osg::Vec3d& value) {
                const auto rotated = rotation * value;
                return osg::Matrixd::transform3x3(
                           billboard ? osg::Matrixd::transform3x3(viewFromModel, rotated) : rotated, viewFromModel)
                    * size;
            };
            auto right = rotatedAxis(baseX);
            auto up = rotatedAxis(baseY);
            if (particle.getShape() == osgParticle::Particle::LINE)
            {
                const auto velocity = particle.getVelocity();
                const auto length = velocity.length();
                if (length < 1e-12)
                    continue;
                up = axis(velocity) * (size * .5 / length);
                center += up;
                right = up ^ osg::Vec3d(0, 0, 1);
                right.normalize();
                right *= size * .025;
            }
            const std::array corners{ center - right - up, center + right - up, center + right + up,
                center - right + up };
            const float s = particle.getSTexCoord(), t = particle.getTTexCoord();
            const float ds = particle.getSTexTile(), dt = particle.getTTexTile();
            const std::array<std::array<float, 2>, 4> uv{ { { s, t }, { s + ds, t }, { s + ds, t + dt },
                { s, t + dt } } };
            for (unsigned j = 0; j < corners.size(); ++j)
            {
                auto& vertex = vertices[count * 4 + j];
                std::copy_n(corners[j].ptr(), 3, vertex.mPosition.begin());
                vertex.mUV = uv[j];
                for (unsigned channel = 0; channel < 4; ++channel)
                    vertex.mColor[channel]
                        = static_cast<std::uint8_t>(std::clamp(color[channel], 0.f, 1.f) * 255.f + .5f);
                vertex.mAlpha = std::clamp(color.a() * particle.getCurrentAlpha(), 0.f, 1.f);
            }
            ++count;
        }
        return count * 6;
    }
}
