#ifndef OPENMW_COMPONENTS_RENDER_CAPTURE_H
#define OPENMW_COMPONENTS_RENDER_CAPTURE_H

namespace Render
{
    class CaptureBoundary
    {
    public:
        constexpr CaptureBoundary(bool requested, bool includeGui)
            : mRequested(requested)
            , mIncludeGui(includeGui)
        {
        }
        constexpr bool before(bool gui, bool offscreen)
        {
            if (!mRequested || mRecorded || mIncludeGui || !gui || offscreen)
                return false;
            mRecorded = true;
            return true;
        }
        constexpr bool atEnd()
        {
            if (!mRequested || mRecorded)
                return false;
            mRecorded = true;
            return true;
        }

    private:
        bool mRequested, mIncludeGui;
        bool mRecorded = false;
    };
}

#endif
