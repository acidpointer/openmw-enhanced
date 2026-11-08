#ifndef OPENMW_COMPONENTS_SCENEUTIL_LIGHTBOUNDSINDEX_H
#define OPENMW_COMPONENTS_SCENEUTIL_LIGHTBOUNDSINDEX_H

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <span>
#include <vector>

#include <osg/BoundingSphere>

namespace SceneUtil
{
    // Broad phase only: callers must retain their original sphere-intersection
    // predicate. Indices refer to the input order, independently of tree order.
    class LightBoundsIndex
    {
    public:
        void build(std::span<const osg::BoundingSphere> spheres)
        {
            build(spheres, [](const osg::BoundingSphere& sphere) -> const auto& { return sphere; });
        }

        template <class Range, class GetSphere>
        void build(const Range& spheres, GetSphere getSphere)
        {
            mSize = spheres.size();
            mItems.clear();
            mNodes.clear();
            mUnbounded.clear();
            mItems.reserve(mSize);
            for (std::size_t i = 0; i < mSize; ++i)
            {
                Bounds bounds;
                if (makeBounds(getSphere(spheres[i]), bounds))
                    mItems.push_back({ bounds, i });
                else
                    mUnbounded.push_back(i);
            }
            if (!mItems.empty())
            {
                mNodes.reserve(mItems.size() / 2 + 1);
                buildNode(0, mItems.size());
            }
        }

        // Returns false rather than producing an incomplete candidate list if
        // the budget is exceeded. Dense queries can then use a linear scan.
        // The result is unique, but not sorted by input index.
        bool queryCandidates(const osg::BoundingSphere& sphere, std::vector<std::size_t>& result,
            std::size_t budget = std::numeric_limits<std::size_t>::max()) const
        {
            result.clear();
            Bounds bounds;
            if (!makeBounds(sphere, bounds))
            {
                if (mSize > budget)
                    return false;
                for (std::size_t i = 0; i < mSize; ++i)
                    result.push_back(i);
                return true;
            }
            if (mUnbounded.size() > budget)
                return false;
            result.insert(result.end(), mUnbounded.begin(), mUnbounded.end());
            return mNodes.empty() || queryNode(0, bounds, result, budget);
        }

    private:
        struct Bounds
        {
            std::array<double, 3> mMin;
            std::array<double, 3> mMax;

            bool intersects(const Bounds& other) const
            {
                for (std::size_t axis = 0; axis < 3; ++axis)
                    if (mMax[axis] < other.mMin[axis] || other.mMax[axis] < mMin[axis])
                        return false;
                return true;
            }

            void expandBy(const Bounds& other)
            {
                for (std::size_t axis = 0; axis < 3; ++axis)
                {
                    mMin[axis] = std::min(mMin[axis], other.mMin[axis]);
                    mMax[axis] = std::max(mMax[axis], other.mMax[axis]);
                }
            }
        };

        struct Item
        {
            Bounds mBounds;
            std::size_t mIndex;
        };

        struct Node
        {
            Bounds mBounds;
            std::size_t mBegin;
            std::size_t mEnd;
            std::size_t mLeft = 0;
            std::size_t mRight = 0;
        };

        static bool makeBounds(const osg::BoundingSphere& sphere, Bounds& bounds)
        {
            const double radius = sphere.radius();
            // Extreme or malformed mod data must retain the legacy predicate's
            // behavior, including its float overflow/NaN behavior. Do not prune it.
            constexpr double maxMagnitude = 1e15;
            if (!std::isfinite(radius) || radius < 0 || radius > maxMagnitude)
                return false;
            for (std::size_t axis = 0; axis < 3; ++axis)
            {
                const double center = sphere.center()[axis];
                if (!std::isfinite(center) || std::abs(center) > maxMagnitude)
                    return false;
                // A mathematically tight box can reject a tangent intersection
                // accepted by OSG's float arithmetic. Pad well beyond its rounding
                // error, including near zero, and calculate the boxes in double.
                const double padding
                    = 16 * std::numeric_limits<float>::epsilon() * std::max({ 1.0, std::abs(center), radius });
                bounds.mMin[axis] = center - radius - padding;
                bounds.mMax[axis] = center + radius + padding;
            }
            return true;
        }

        std::size_t buildNode(std::size_t begin, std::size_t end)
        {
            Bounds bounds = mItems[begin].mBounds;
            for (std::size_t i = begin + 1; i < end; ++i)
                bounds.expandBy(mItems[i].mBounds);
            const std::size_t node = mNodes.size();
            mNodes.push_back({ bounds, begin, end });
            if (end - begin <= 8)
                return node;

            std::size_t axis = 0;
            for (std::size_t i = 1; i < 3; ++i)
                if (bounds.mMax[i] - bounds.mMin[i] > bounds.mMax[axis] - bounds.mMin[axis])
                    axis = i;
            const std::size_t middle = begin + (end - begin) / 2;
            std::nth_element(mItems.begin() + begin, mItems.begin() + middle, mItems.begin() + end,
                [axis](const Item& a, const Item& b) {
                    const double left = a.mBounds.mMin[axis] + a.mBounds.mMax[axis];
                    const double right = b.mBounds.mMin[axis] + b.mBounds.mMax[axis];
                    return left < right || (left == right && a.mIndex < b.mIndex);
                });
            const std::size_t left = buildNode(begin, middle);
            const std::size_t right = buildNode(middle, end);
            mNodes[node].mLeft = left;
            mNodes[node].mRight = right;
            return node;
        }

        bool queryNode(
            std::size_t nodeIndex, const Bounds& bounds, std::vector<std::size_t>& result, std::size_t budget) const
        {
            const Node& node = mNodes[nodeIndex];
            if (!node.mBounds.intersects(bounds))
                return true;
            if (node.mLeft != 0)
                return queryNode(node.mLeft, bounds, result, budget) && queryNode(node.mRight, bounds, result, budget);
            for (std::size_t i = node.mBegin; i < node.mEnd; ++i)
            {
                if (!mItems[i].mBounds.intersects(bounds))
                    continue;
                if (result.size() == budget)
                    return false;
                result.push_back(mItems[i].mIndex);
            }
            return true;
        }

        std::size_t mSize = 0;
        std::vector<Item> mItems;
        std::vector<Node> mNodes;
        std::vector<std::size_t> mUnbounded;
    };
}

#endif
