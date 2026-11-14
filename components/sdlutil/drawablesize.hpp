#ifndef OPENMW_COMPONENTS_SDLUTIL_DRAWABLESIZE_H
#define OPENMW_COMPONENTS_SDLUTIL_DRAWABLESIZE_H

#include <SDL_video.h>
#include <SDL_vulkan.h>

namespace SDLUtil
{
    inline void getDrawableSize(SDL_Window* window, int& width, int& height)
    {
        if (SDL_GetWindowFlags(window) & SDL_WINDOW_VULKAN)
            SDL_Vulkan_GetDrawableSize(window, &width, &height);
        else
            SDL_GL_GetDrawableSize(window, &width, &height);
    }
}

#endif
