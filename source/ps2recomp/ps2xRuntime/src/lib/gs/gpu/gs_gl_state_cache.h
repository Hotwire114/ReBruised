// Tekken 4 port (perf): redundant GL state call elimination for the render-target renderer's context.
//
// The renderer sets its whole pipeline state for every batch (~40 GL calls: enables, blend, depth, program uniforms,
// texture bindings and parameters, buffers). Most of it repeats the previous batch. GLStateCache::Install replaces the
// state-setting entry points of an Api copy with shims that remember the last value of each piece of state and drop
// a call that would not change it. Every GL call of that context goes through the same Api (one thread), so the cache
// always knows the real state; deletions (textures, programs, buffers, vertex arrays, framebuffers) forget what they
// held. The draws, uploads and readbacks themselves pass through untouched. PS2X_GL_STATE_CACHE=0 turns it off.
#pragma once

#include "gs_gl.h"

namespace GSGL
{
    namespace GLStateCache
    {
        // Returns a copy of `real` with the caching shims installed (the shims call `real`'s functions). Process-wide:
        // one GL context (the GS worker's) may use it.
        Api Install(const Api &real);
        bool Enabled();
    }
}
