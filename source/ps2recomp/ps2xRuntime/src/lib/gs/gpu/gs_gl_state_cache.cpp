// Tekken 4 port (perf): see gs_gl_state_cache.h.
#include "gs_gl_state_cache.h"

#include <array>
#include <cstdlib>
#include <cstring>
#include <unordered_map>
#include <vector>

namespace GSGL
{
    namespace GLStateCache
    {
        namespace
        {
            constexpr GLenum kFramebufferTarget = 0x8D40, kReadFramebuffer = 0x8CA8, kDrawFramebuffer = 0x8CA9;
            constexpr GLenum kArrayBufferTarget = 0x8892;
            constexpr GLenum kTex2D = 0x0DE1;
            constexpr GLenum kTextureUnit0 = 0x84C0;
            constexpr int kUnits = 16;

            Api g_real;

            // Every cached value starts unknown (valid = false): the first call always reaches GL.
            template <typename T>
            struct Slot
            {
                bool valid = false;
                T value{};
                bool same(const T &v) const { return valid && std::memcmp(&value, &v, sizeof(T)) == 0; }
                void set(const T &v) { valid = true, value = v; }
            };

            // capabilities the renderer toggles (others pass through uncached)
            struct Caps
            {
                Slot<bool> blend, depth, scissor, cull;
                Slot<bool> *find(GLenum cap)
                {
                    switch (cap)
                    {
                    case 0x0BE2: return &blend;
                    case 0x0B71: return &depth;
                    case 0x0C11: return &scissor;
                    case 0x0B44: return &cull;
                    default: return nullptr;
                    }
                }
            } g_caps;

            Slot<GLuint> g_readFb, g_drawFb, g_program, g_vao, g_arrayBuffer;
            Slot<GLenum> g_activeUnit, g_depthFunc;
            Slot<GLboolean> g_depthMask;
            Slot<std::array<GLint, 4>> g_viewport, g_scissor;
            Slot<std::array<GLboolean, 4>> g_colorMask;
            Slot<std::array<GLenum, 2>> g_blendEq;
            Slot<std::array<GLenum, 4>> g_blendFunc;
            Slot<std::array<float, 4>> g_blendColor;
            Slot<GLint> g_unpackAlign, g_packAlign;
            std::array<Slot<GLuint>, kUnits> g_tex2d;

            // texture object parameters (min, mag, wrap s, wrap t) by texture id
            std::unordered_map<GLuint, std::array<Slot<GLint>, 4>> g_texParams;
            int texParamIndex(GLenum pname)
            {
                switch (pname)
                {
                case 0x2801: return 0; // MIN_FILTER
                case 0x2800: return 1; // MAG_FILTER
                case 0x2802: return 2; // WRAP_S
                case 0x2803: return 3; // WRAP_T
                default: return -1;
                }
            }

            // uniform values by program, then location
            struct UniformValue
            {
                uint32_t type = 0; // 0 = unknown
                uint32_t words[4] = {0, 0, 0, 0};
            };
            std::unordered_map<GLuint, std::vector<UniformValue>> g_uniforms;
            std::vector<UniformValue> *g_currentUniforms = nullptr;

            void selectProgramUniforms(GLuint program)
            {
                g_currentUniforms = program ? &g_uniforms[program] : nullptr;
            }

            // true when the call must reach GL (and the cache now holds the new value)
            bool uniformChanged(GLint location, uint32_t type, const void *values, size_t bytes)
            {
                if (location < 0 || !g_currentUniforms || location >= 4096)
                    return location >= 0;
                std::vector<UniformValue> &u = *g_currentUniforms;
                if (static_cast<size_t>(location) >= u.size())
                    u.resize(static_cast<size_t>(location) + 1u);
                UniformValue &slot = u[static_cast<size_t>(location)];
                UniformValue next;
                next.type = type;
                std::memcpy(next.words, values, bytes);
                if (slot.type == type && std::memcmp(slot.words, next.words, sizeof(next.words)) == 0)
                    return false;
                slot = next;
                return true;
            }

            GLuint boundTexture()
            {
                const GLenum unit = g_activeUnit.valid ? g_activeUnit.value : kTextureUnit0;
                const size_t index = unit - kTextureUnit0;
                if (index >= g_tex2d.size() || !g_tex2d[index].valid)
                    return ~0u; // unknown
                return g_tex2d[index].value;
            }

            void Enable(GLenum cap)
            {
                Slot<bool> *slot = g_caps.find(cap);
                if (slot && slot->same(true))
                    return;
                if (slot)
                    slot->set(true);
                g_real.Enable(cap);
            }
            void Disable(GLenum cap)
            {
                Slot<bool> *slot = g_caps.find(cap);
                if (slot && slot->same(false))
                    return;
                if (slot)
                    slot->set(false);
                g_real.Disable(cap);
            }
            void BindFramebuffer(GLenum target, GLuint fb)
            {
                if (target == kFramebufferTarget)
                {
                    if (g_readFb.same(fb) && g_drawFb.same(fb))
                        return;
                    g_readFb.set(fb), g_drawFb.set(fb);
                }
                else if (target == kReadFramebuffer)
                {
                    if (g_readFb.same(fb))
                        return;
                    g_readFb.set(fb);
                }
                else if (target == kDrawFramebuffer)
                {
                    if (g_drawFb.same(fb))
                        return;
                    g_drawFb.set(fb);
                }
                g_real.BindFramebuffer(target, fb);
            }
            void DeleteFramebuffers(GLsizei n, const GLuint *ids)
            {
                for (GLsizei i = 0; i < n; ++i)
                {
                    if (g_readFb.same(ids[i]))
                        g_readFb.valid = false;
                    if (g_drawFb.same(ids[i]))
                        g_drawFb.valid = false;
                }
                g_real.DeleteFramebuffers(n, ids);
            }
            void Viewport(GLint x, GLint y, GLsizei w, GLsizei h)
            {
                const std::array<GLint, 4> v{x, y, w, h};
                if (g_viewport.same(v))
                    return;
                g_viewport.set(v);
                g_real.Viewport(x, y, w, h);
            }
            void Scissor(GLint x, GLint y, GLsizei w, GLsizei h)
            {
                const std::array<GLint, 4> v{x, y, w, h};
                if (g_scissor.same(v))
                    return;
                g_scissor.set(v);
                g_real.Scissor(x, y, w, h);
            }
            void DepthFunc(GLenum f)
            {
                if (g_depthFunc.same(f))
                    return;
                g_depthFunc.set(f);
                g_real.DepthFunc(f);
            }
            void DepthMask(GLboolean m)
            {
                if (g_depthMask.same(m))
                    return;
                g_depthMask.set(m);
                g_real.DepthMask(m);
            }
            void ColorMask(GLboolean r, GLboolean g, GLboolean b, GLboolean a)
            {
                const std::array<GLboolean, 4> v{r, g, b, a};
                if (g_colorMask.same(v))
                    return;
                g_colorMask.set(v);
                g_real.ColorMask(r, g, b, a);
            }
            void BlendEquationSeparate(GLenum rgb, GLenum alpha)
            {
                const std::array<GLenum, 2> v{rgb, alpha};
                if (g_blendEq.same(v))
                    return;
                g_blendEq.set(v);
                g_real.BlendEquationSeparate(rgb, alpha);
            }
            void BlendFuncSeparate(GLenum a, GLenum b, GLenum c, GLenum d)
            {
                const std::array<GLenum, 4> v{a, b, c, d};
                if (g_blendFunc.same(v))
                    return;
                g_blendFunc.set(v);
                g_real.BlendFuncSeparate(a, b, c, d);
            }
            void BlendColor(float r, float g, float b, float a)
            {
                const std::array<float, 4> v{r, g, b, a};
                if (g_blendColor.same(v))
                    return;
                g_blendColor.set(v);
                g_real.BlendColor(r, g, b, a);
            }
            void UseProgram(GLuint program)
            {
                if (g_program.same(program))
                    return;
                g_program.set(program);
                selectProgramUniforms(program);
                g_real.UseProgram(program);
            }
            void LinkProgram(GLuint program)
            {
                g_uniforms.erase(program); // locations and values start over
                if (g_program.same(program))
                    selectProgramUniforms(program);
                g_real.LinkProgram(program);
            }
            void ProgramBinary(GLuint program, GLenum format, const void *binary, GLsizei length)
            {
                g_uniforms.erase(program);
                if (g_program.same(program))
                    selectProgramUniforms(program);
                g_real.ProgramBinary(program, format, binary, length);
            }
            void DeleteProgram(GLuint program)
            {
                g_uniforms.erase(program);
                if (g_program.same(program))
                    g_program.valid = false, g_currentUniforms = nullptr;
                g_real.DeleteProgram(program);
            }
            void Uniform1i(GLint loc, GLint v)
            {
                if (uniformChanged(loc, 1, &v, sizeof(v)))
                    g_real.Uniform1i(loc, v);
            }
            void Uniform1f(GLint loc, float v)
            {
                if (uniformChanged(loc, 2, &v, sizeof(v)))
                    g_real.Uniform1f(loc, v);
            }
            void Uniform2f(GLint loc, float a, float b)
            {
                const float v[2] = {a, b};
                if (uniformChanged(loc, 3, v, sizeof(v)))
                    g_real.Uniform2f(loc, a, b);
            }
            void Uniform4f(GLint loc, float a, float b, float c, float d)
            {
                const float v[4] = {a, b, c, d};
                if (uniformChanged(loc, 4, v, sizeof(v)))
                    g_real.Uniform4f(loc, a, b, c, d);
            }
            void Uniform4i(GLint loc, GLint a, GLint b, GLint c, GLint d)
            {
                const GLint v[4] = {a, b, c, d};
                if (uniformChanged(loc, 5, v, sizeof(v)))
                    g_real.Uniform4i(loc, a, b, c, d);
            }
            void Uniform1ui(GLint loc, GLuint v)
            {
                if (uniformChanged(loc, 6, &v, sizeof(v)))
                    g_real.Uniform1ui(loc, v);
            }
            void Uniform4ui(GLint loc, GLuint a, GLuint b, GLuint c, GLuint d)
            {
                const GLuint v[4] = {a, b, c, d};
                if (uniformChanged(loc, 7, v, sizeof(v)))
                    g_real.Uniform4ui(loc, a, b, c, d);
            }
            void Uniform1uiv(GLint loc, GLsizei count, const GLuint *v)
            {
                // arrays: forget the location's cached value, always pass through
                if (g_currentUniforms && loc >= 0 && static_cast<size_t>(loc) < g_currentUniforms->size())
                    (*g_currentUniforms)[static_cast<size_t>(loc)].type = 0;
                g_real.Uniform1uiv(loc, count, v);
            }
            void ActiveTexture(GLenum unit)
            {
                if (g_activeUnit.same(unit))
                    return;
                g_activeUnit.set(unit);
                g_real.ActiveTexture(unit);
            }
            void BindTexture(GLenum target, GLuint id)
            {
                const GLenum unit = g_activeUnit.valid ? g_activeUnit.value : kTextureUnit0;
                const size_t index = unit - kTextureUnit0;
                if (target == kTex2D && index < g_tex2d.size())
                {
                    if (g_activeUnit.valid && g_tex2d[index].same(id))
                        return;
                    if (g_activeUnit.valid)
                        g_tex2d[index].set(id);
                }
                g_real.BindTexture(target, id);
            }
            void TexParameteri(GLenum target, GLenum pname, GLint value)
            {
                const int index = texParamIndex(pname);
                const GLuint tex = boundTexture();
                if (target == kTex2D && index >= 0 && tex != ~0u && tex != 0u)
                {
                    Slot<GLint> &slot = g_texParams[tex][static_cast<size_t>(index)];
                    if (slot.same(value))
                        return;
                    slot.set(value);
                }
                g_real.TexParameteri(target, pname, value);
            }
            void DeleteTextures(GLsizei n, const GLuint *ids)
            {
                for (GLsizei i = 0; i < n; ++i)
                {
                    g_texParams.erase(ids[i]);
                    for (Slot<GLuint> &unit : g_tex2d) // GL unbinds a deleted texture from every unit
                        if (unit.same(ids[i]))
                            unit.set(0u);
                }
                g_real.DeleteTextures(n, ids);
            }
            void BindVertexArray(GLuint vao)
            {
                if (g_vao.same(vao))
                    return;
                g_vao.set(vao);
                g_real.BindVertexArray(vao);
            }
            void DeleteVertexArrays(GLsizei n, const GLuint *ids)
            {
                for (GLsizei i = 0; i < n; ++i)
                    if (g_vao.same(ids[i]))
                        g_vao.set(0u);
                g_real.DeleteVertexArrays(n, ids);
            }
            void BindBuffer(GLenum target, GLuint buffer)
            {
                if (target == kArrayBufferTarget)
                {
                    if (g_arrayBuffer.same(buffer))
                        return;
                    g_arrayBuffer.set(buffer);
                }
                g_real.BindBuffer(target, buffer);
            }
            void DeleteBuffers(GLsizei n, const GLuint *ids)
            {
                for (GLsizei i = 0; i < n; ++i)
                    if (g_arrayBuffer.same(ids[i]))
                        g_arrayBuffer.set(0u);
                g_real.DeleteBuffers(n, ids);
            }
            void PixelStorei(GLenum pname, GLint value)
            {
                Slot<GLint> *slot = pname == 0x0CF5 ? &g_unpackAlign : pname == 0x0D05 ? &g_packAlign : nullptr;
                if (slot && slot->same(value))
                    return;
                if (slot)
                    slot->set(value);
                g_real.PixelStorei(pname, value);
            }
        }

        bool Enabled()
        {
            static const bool on = []
            {
                const char *value = std::getenv("PS2X_GL_STATE_CACHE");
                return !value || *value != '0';
            }();
            return on;
        }

        Api Install(const Api &real)
        {
            g_real = real;
            Api api = real;
            if (real.ActiveTexture)
            {
                real.ActiveTexture(kTextureUnit0); // a known texture unit for the binding cache
                g_activeUnit.set(kTextureUnit0);
            }
            const auto shim = [](auto &field, auto function)
            {
                if (field)
                    field = function;
            };
            shim(api.Enable, &Enable);
            shim(api.Disable, &Disable);
            shim(api.BindFramebuffer, &BindFramebuffer);
            shim(api.DeleteFramebuffers, &DeleteFramebuffers);
            shim(api.Viewport, &Viewport);
            shim(api.Scissor, &Scissor);
            shim(api.DepthFunc, &DepthFunc);
            shim(api.DepthMask, &DepthMask);
            shim(api.ColorMask, &ColorMask);
            shim(api.BlendEquationSeparate, &BlendEquationSeparate);
            shim(api.BlendFuncSeparate, &BlendFuncSeparate);
            shim(api.BlendColor, &BlendColor);
            shim(api.UseProgram, &UseProgram);
            shim(api.LinkProgram, &LinkProgram);
            shim(api.ProgramBinary, &ProgramBinary);
            shim(api.DeleteProgram, &DeleteProgram);
            shim(api.Uniform1i, &Uniform1i);
            shim(api.Uniform1f, &Uniform1f);
            shim(api.Uniform2f, &Uniform2f);
            shim(api.Uniform4f, &Uniform4f);
            shim(api.Uniform4i, &Uniform4i);
            shim(api.Uniform1ui, &Uniform1ui);
            shim(api.Uniform4ui, &Uniform4ui);
            shim(api.Uniform1uiv, &Uniform1uiv);
            shim(api.ActiveTexture, &ActiveTexture);
            shim(api.BindTexture, &BindTexture);
            shim(api.TexParameteri, &TexParameteri);
            shim(api.DeleteTextures, &DeleteTextures);
            shim(api.BindVertexArray, &BindVertexArray);
            shim(api.DeleteVertexArrays, &DeleteVertexArrays);
            shim(api.BindBuffer, &BindBuffer);
            shim(api.DeleteBuffers, &DeleteBuffers);
            shim(api.PixelStorei, &PixelStorei);
            return api;
        }
    }
}
