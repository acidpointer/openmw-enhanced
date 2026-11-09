#ifndef OPENMW_COMPONENTS_SCENEUTIL_PAGINGANALYSIS_H
#define OPENMW_COMPONENTS_SCENEUTIL_PAGINGANALYSIS_H

#include <osg/Geometry>
#include <osg/LOD>
#include <osg/Sequence>
#include <osg/Switch>

#include <algorithm>
#include <functional>
#include <unordered_map>
#include <utility>
#include <vector>

namespace SceneUtil
{
    // Analyse the same selected children as the paging copy operation. State
    // keys include the inherited path as well as the drawable's local state;
    // neither sibling traversal nor selector traversal may leak material state.
    class PagingAnalysis : public osg::NodeVisitor
    {
        using StateKey = std::vector<const osg::StateSet*>;
        struct StateHash
        {
            std::size_t operator()(const StateKey& key) const
            {
                std::size_t result = 0;
                for (const auto* state : key)
                    result ^= std::hash<const void*>{}(state) + 0x9e3779b9 + (result << 6) + (result >> 2);
                return result;
            }
        };
        struct ScopedState
        {
            StateKey& mState;
            std::size_t mSize;
            ScopedState(StateKey& state, const osg::Node& node)
                : mState(state)
                , mSize(state.size())
            {
                if (node.getStateSet())
                    state.push_back(node.getStateSet());
            }
            ~ScopedState() { mState.resize(mSize); }
        };

    public:
        using StateSetCounter = std::unordered_map<StateKey, unsigned int, StateHash>;
        struct Result
        {
            StateSetCounter mStateSetCounter;
            unsigned int mNumVerts = 0;
        };
        explicit PagingAnalysis(osg::Node::NodeMask mask)
            : osg::NodeVisitor(TRAVERSE_ALL_CHILDREN)
        {
            setTraversalMask(mask);
        }

        void apply(osg::Node& node) override
        {
            ScopedState state(mState, node);
            if (auto* sw = node.asSwitch())
            {
                for (unsigned int i = 0; i < sw->getNumChildren(); ++i)
                    if (sw->getValue(i))
                        sw->getChild(i)->accept(*this);
            }
            else if (auto* lod = dynamic_cast<osg::LOD*>(&node))
            {
                for (unsigned int i = 0; i < lod->getNumChildren(); ++i)
                    if (std::max(lod->getRangeList()[i].first, mDistances.first)
                        < std::min(lod->getRangeList()[i].second, mDistances.second))
                        lod->getChild(i)->accept(*this);
            }
            else if (auto* sequence = dynamic_cast<osg::Sequence*>(&node))
            {
                const auto index = sequence->getValue() < 0 ? 0u : unsigned(sequence->getValue());
                if (index < sequence->getNumChildren())
                    sequence->getChild(index)->accept(*this);
            }
            else
                traverse(node);
        }
        void apply(osg::Geometry& geometry) override
        {
            ScopedState state(mState, geometry);
            if (const auto* vertices = geometry.getVertexArray())
                mResult.mNumVerts += vertices->getNumElements();
            ++mResult.mStateSetCounter[mState];
            ++mGlobalStateSetCounter[mState];
        }
        Result retrieveResult() { return std::exchange(mResult, {}); }
        void addInstance(const Result& result)
        {
            for (const auto& [key, count] : result.mStateSetCounter)
                mGlobalStateSetCounter[key] += count;
        }
        float getMergeBenefit(const Result& result) const
        {
            if (result.mStateSetCounter.empty())
                return 1;
            float benefit = 0;
            for (const auto& [key, count] : result.mStateSetCounter)
                if (const auto it = mGlobalStateSetCounter.find(key); it != mGlobalStateSetCounter.end())
                    benefit += it->second;
            return benefit / result.mStateSetCounter.size();
        }
        osg::LOD::MinMaxPair mDistances{ 0, 0 };

    private:
        StateKey mState;
        Result mResult;
        StateSetCounter mGlobalStateSetCounter;
    };
}
#endif
