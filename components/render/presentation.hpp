#ifndef OPENMW_COMPONENTS_RENDER_PRESENTATION_H
#define OPENMW_COMPONENTS_RENDER_PRESENTATION_H

#include "backend.hpp"

namespace Render
{
    // Viewer adapter for shared window services, independent of graphics API.
    class Presentation
    {
    public:
        virtual ~Presentation() = default;
        virtual void setPresentMode(PresentMode mode) = 0;
        virtual void setStatsLevel(int) {}
        virtual CapturedImage capture(bool includeGui = true) = 0;
    };
}

#endif
