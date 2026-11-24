#pragma once

// The native renderer's backends (Metal on macOS, OpenGL on macOS and the
// Vita through vitaGL): runtime/native_renderer.h picks one (native_renderer.cpp).

#include "runtime/native_renderer.h"

#include <cstdlib>
#include <functional>
#include <utility>
#include <mutex>

namespace native_renderer::detail
{
    void setInEmulator(bool emulator); // the backend's probe (native_renderer::inEmulator)

    struct FrameOutput
    {
        std::mutex mutex;
        std::vector<uint8_t> pixels;
        uint32_t width = 0, height = 0;
        uint64_t revision = 0;
        // The frame before, when the host had not taken it yet: the game (59.94 Hz) and the screen (60 Hz)
        // drift past each other, and replacing an unread frame dropped pictures in waves. SOTC_FRAME_QUEUE=0: off.
        std::vector<uint8_t> pending;
        uint32_t pendingWidth = 0, pendingHeight = 0;
        uint64_t pendingRevision = 0, taken = 0;

        // Call with the mutex held, before writing a new frame into pixels.
        void keepUnread()
        {
            static const bool off = [] { const char *v = std::getenv("SOTC_FRAME_QUEUE"); return v && *v == '0'; }();
            if (off || !revision || taken >= revision)
                return;
            std::swap(pending, pixels);
            pendingWidth = width;
            pendingHeight = height;
            pendingRevision = revision;
        }

        FrameRead read(std::vector<uint8_t> &rgba, uint32_t &w, uint32_t &h, uint64_t *seen)
        {
            const std::lock_guard lock(mutex);
            if (!revision)
                return FrameRead::Missing;
            w = width;
            h = height;
            if (seen && *seen == revision)
                return FrameRead::Unchanged;
            if (seen && pendingRevision > *seen && pendingWidth == width && pendingHeight == height)
            {
                std::swap(rgba, pending);
                *seen = taken = pendingRevision;
                pendingRevision = 0;
                return FrameRead::Updated;
            }
            rgba = pixels;
            if (seen)
            {
                *seen = taken = revision;
                pendingRevision = 0;
            }
            return FrameRead::Updated;
        }
    };

    class Backend
    {
    public:
        virtual ~Backend() = default;
        virtual bool hasMesh(uint64_t id) = 0;
        virtual void uploadMesh(uint64_t id, const Vertex *vertices, uint32_t count, bool perFrame) = 0;
        // native_renderer::frameMesh: by default a scratch buffer handed to
        // uploadMesh at the commit (the GL backend writes into its frame arena).
        virtual Vertex *frameMesh(uint32_t count)
        {
            if (m_frameScratch.size() < count)
                m_frameScratch.resize(count);
            return m_frameScratch.data();
        }
        virtual uint32_t *frameMeshIndices(uint32_t count, uint32_t &base)
        {
            base = 0;
            if (m_indexScratch.size() < count)
                m_indexScratch.resize(count);
            return m_indexScratch.data();
        }
        virtual void commitFrameMesh(uint64_t id, uint32_t count, uint32_t indexCount)
        {
            if (!indexCount)
            {
                uploadMesh(id, m_frameScratch.data(), count, true);
                return;
            }
            m_expanded.resize(indexCount); // the triangle list
            for (uint32_t i = 0; i < indexCount; ++i)
                m_expanded[i] = m_frameScratch[m_indexScratch[i]];
            uploadMesh(id, m_expanded.data(), indexCount, true);
        }
        virtual void cancelFrameMesh() {}
        // native_renderer::gpuSkinning: none by default (the CPU poses).
        virtual bool canSkin() { return false; }
        virtual void uploadSkinnedMesh(uint64_t, const SkinVertex *, uint32_t, const uint32_t *, uint32_t) {}
        virtual uint32_t framePalette(const float *, uint32_t) { return 0; }
        // Rewrite a current recording frame's palette; no GPU skinning by default.
        virtual void rewriteFramePalette(uint32_t, const float *, uint32_t) {}
        virtual bool hasTexture(uint64_t id) = 0;
        virtual void uploadTexture(uint64_t id, uint32_t width, uint32_t height, const uint8_t *rgba, uint32_t levels) = 0;
        virtual void uploadCompressed(uint64_t, uint32_t, uint32_t, const uint8_t *, size_t, uint32_t, uint8_t, uint8_t) {}
        virtual void beginFrame() = 0;
        virtual void draw(uint64_t mesh, const float clip[16], const DrawState &state) = 0;
        virtual void setFog(const Fog &fog) = 0;
        virtual void setBloom(const Bloom &bloom) = 0;
        virtual void addScreenBlur(const ScreenBlur &blur) = 0;
        virtual void setGlowComposite(const GlowComposite &glow) = 0;
        virtual void setDepthOfField(const DepthOfField &dof) = 0;
        virtual void endFrame(uint32_t width, uint32_t height) = 0;
        // The frame ending next is not shown (its uploads still made): the
        // last one stays on screen (holdIncomplete). Backends that cannot: no-op.
        virtual void holdFrame() {}
        virtual FrameRead readFrame(std::vector<uint8_t> &rgba, uint32_t &width, uint32_t &height, uint64_t *seen) = 0;
        bool latestFrame(std::vector<uint8_t> &rgba, uint32_t &width, uint32_t &height)
        {
            return readFrame(rgba, width, height, nullptr) == FrameRead::Updated;
        }
        // A backend that draws on a thread of its own: returns once every
        // frame handed over is drawn.
        virtual void finishFrames() {}
        virtual bool resetStateFrame() { return false; }
        virtual bool captureState(std::vector<uint8_t> &, std::string &error) const
        { error = "This native backend cannot capture temporal histories."; return false; }
        virtual bool validateState(const std::vector<uint8_t> &, std::string &error) const
        { error = "This native backend cannot validate temporal histories."; return false; }
        virtual bool restoreState(const std::vector<uint8_t> &, std::string &error)
        { error = "This native backend cannot restore temporal histories."; return false; }
        // It fills the frame log's render / present times itself (else the
        // front times endFrame).
        virtual bool timesItself() { return false; }

    private:
        std::vector<Vertex> m_frameScratch, m_expanded;
        std::vector<uint32_t> m_indexScratch;
    };

    // nullptr when the platform or the device does not have it.
    Backend *createMetalBackend();
    // makeCurrent: makes the OpenGL context current (on the thread that will draw).
    Backend *createGlBackend(std::function<void()> makeCurrent);
    // The host's present hook (native_renderer::setPresentHook), for the GL backend.
    const std::function<void(uint32_t, uint32_t, uint32_t)> &presentHook();
}
