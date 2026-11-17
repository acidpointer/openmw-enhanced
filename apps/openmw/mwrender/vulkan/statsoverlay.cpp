#include "statsoverlay.hpp"
#include "../../profile.hpp"

#include <components/myguiplatform/myguirendermanager.hpp>

#include <MyGUI_EditBox.h>
#include <MyGUI_Gui.h>
#include <MyGUI_TextBox.h>
#include <osg/Stats>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <iomanip>
#include <numeric>
#include <sstream>

namespace MWRender::Vulkan
{
    struct StatsOverlay::Impl
    {
        Render::Backend& backend;
        int level = 0;
        MyGUI::Widget* panel = nullptr;
        MyGUI::TextBox* header = nullptr;
        MyGUI::TextBox* details = nullptr;
        MyGUI::IntCoord chart;
        Render::MeshHandle mesh;
        std::vector<Render::Vertex> vertices;
        std::vector<std::uint32_t> indices;
        std::vector<Render::Draw> draws;
        std::array<double, 240> history{};
        std::size_t cursor = 0, count = 0, drawCount = 0, meshUploads = 0, imageUploads = 0;
        std::array<double, 5> phases{};
        PrepareStats prepare;
        PrepareStats peakPrepare;
        double peakPrepareMs = 0;
        double lastPrepareMs = 0;
        std::chrono::steady_clock::time_point nextText{};
        explicit Impl(Render::Backend& b)
            : backend(b)
        {
        }
        void shutdown()
        {
            if (panel && MyGUI::Gui::getInstancePtr())
                MyGUI::Gui::getInstance().destroyWidget(panel);
            panel = nullptr;
            header = details = nullptr;
            chart = MyGUI::IntCoord();
        }
        ~Impl()
        {
            shutdown();
            if (mesh)
                backend.releaseMesh(mesh);
        }
        void rect(float x, float y, float w, float h, std::array<std::uint8_t, 4> color, float width, float height)
        {
            const auto first = static_cast<std::uint32_t>(vertices.size());
            for (const auto point : { std::array<float, 2>{ x, y }, std::array<float, 2>{ x + w, y },
                     std::array<float, 2>{ x + w, y + h }, std::array<float, 2>{ x, y + h } })
            {
                Render::Vertex v{};
                v.mPosition = { point[0] * 2 / width - 1, 1 - point[1] * 2 / height, 0 };
                v.mColor = color;
                vertices.push_back(v);
            }
            indices.insert(indices.end(), { first, first + 1, first + 2, first, first + 2, first + 3 });
        }
    };
    StatsOverlay::StatsOverlay(Render::Backend& backend)
        : mImpl(std::make_unique<Impl>(backend))
    {
    }
    StatsOverlay::~StatsOverlay() = default;
    void StatsOverlay::setLevel(int level)
    {
        mImpl->backend.setEffectDiagnostics(level >= 2);
        mImpl->peakPrepareMs = mImpl->lastPrepareMs;
        mImpl->peakPrepare = mImpl->prepare;
        mImpl->level = level;
        mImpl->nextText = {};
        if (!level)
            mImpl->shutdown();
    }
    void StatsOverlay::shutdown()
    {
        mImpl->backend.setEffectDiagnostics(false);
        mImpl->shutdown();
    }
    void StatsOverlay::prepared(double ms, const PrepareStats& prepare)
    {
        auto& s = *mImpl;
        s.prepare = prepare;
        s.lastPrepareMs = ms;
        if (ms >= s.peakPrepareMs)
        {
            s.peakPrepareMs = ms;
            s.peakPrepare = prepare;
        }
    }
    void StatsOverlay::submitted(double ms, const std::array<double, 5>& phases, std::size_t draws,
        std::size_t meshUploads, std::size_t imageUploads)
    {
        auto& s = *mImpl;
        if (ms > 0 && std::isfinite(ms))
        {
            s.history[s.cursor] = ms;
            s.cursor = (s.cursor + 1) % s.history.size();
            s.count = std::min(s.count + 1, s.history.size());
        }
        s.phases = phases;
        s.drawCount = draws;
        s.meshUploads = meshUploads;
        s.imageUploads = imageUploads;
    }
    void StatsOverlay::update(osg::Stats& engine, std::size_t meshes, std::size_t textures, std::uint64_t meshBytes,
        std::uint64_t textureBytes)
    {
        auto& s = *mImpl;
        if (!s.level || !MyGUI::Gui::getInstancePtr())
            return;
        if (!s.panel)
        {
            s.panel = MyGUI::Gui::getInstance().createWidget<MyGUI::Widget>(
                "FullBlackBG", { 8, 8, 720, 400 }, MyGUI::Align::Left | MyGUI::Align::Top, "Notification");
            s.panel->setNeedMouseFocus(false);
            s.panel->setNeedKeyFocus(false);
            const auto label = [&] {
                auto* text = s.panel->createWidget<MyGUI::EditBox>(
                    "MW_TextEditClient", { 12, 12, 696, 100 }, MyGUI::Align::Left | MyGUI::Align::Top);
                text->setFontName("Default");
                text->setEditReadOnly(true);
                text->setEditStatic(true);
                text->setEditMultiLine(true);
                text->setEditWordWrap(true);
                text->setVisibleHScroll(false);
                text->setVisibleVScroll(false);
                text->setTextAlign(MyGUI::Align::Left | MyGUI::Align::Top);
                text->setNeedMouseFocus(false);
                text->setNeedKeyFocus(false);
                text->setTextColour(MyGUI::Colour::White);
                return text;
            };
            s.header = label();
            s.details = label();
        }
        const auto now = std::chrono::steady_clock::now();
        if (now < s.nextText)
            return;
        s.nextText = now + std::chrono::milliseconds(150);
        const double mean = s.count ? std::accumulate(s.history.begin(), s.history.begin() + s.count, 0.) / s.count : 0;
        const double worst = s.count ? *std::max_element(s.history.begin(), s.history.begin() + s.count) : 0;
        std::ostringstream heading, out;
        heading << std::fixed << std::setprecision(1) << "Vulkan  " << (mean > 0 ? 1000 / mean : 0) << " FPS  " << mean
                << " ms  max " << worst << " ms";
        out << std::fixed << std::setprecision(2);
        if (s.level >= 2)
        {
            heading << "\nSubmission intervals: 16.7 / 33.3 / 66.7 ms (red > 33.3 ms)";
            out << "CPU ready " << s.phases[0] << "  cull " << s.phases[1] << "  prepare " << s.phases[2] << " ms";
            out << "\nCPU submit " << s.phases[3] << "  retire " << s.phases[4] << " ms  (not GPU duration)";
            out << "\nPrepare peak " << s.peakPrepareMs << " ms: scene " << s.peakPrepare.mScene << "  effects "
                << s.peakPrepare.mEffects << "  overlay " << s.peakPrepare.mOverlay;
            out << "\nPeak assets: mesh " << s.peakPrepare.mMeshes << "  texture " << s.peakPrepare.mTextures
                << "  target " << s.peakPrepare.mTargets << " ms (included above)";
            out << "\nPeak particles " << s.peakPrepare.mParticles << " ms (lock wait " << s.peakPrepare.mParticleWait
                << " ms, included in scene)";
            out << "\nDraw items " << s.drawCount << "  mesh uploads " << s.meshUploads << "  image uploads "
                << s.imageUploads;
            const auto diagnostics = s.backend.effectDiagnostics();
            if (!diagnostics.mGpuAvailable)
                out << "\nGPU timings unavailable / pending";
            else
                for (const auto& timing : diagnostics.mGpu)
                    out << "\nGPU " << timing.mName << "  " << timing.mMilliseconds << " ms";
            out << "\nShader compilations " << s.prepare.mShaderCompilations << "  pipeline misses "
                << diagnostics.mPipelineMisses;
            out << "\nEffect targets " << diagnostics.mTargetCreations << "  descriptor updates "
                << diagnostics.mDescriptorUpdates << " (cumulative)";
            out << "\nPeak mesh builds " << s.peakPrepare.mMeshBuilds << "  stream updates "
                << s.peakPrepare.mMeshUpdates;
        }
        if (s.level >= 3)
        {
            out << "\nMeshes " << meshes << " (" << meshBytes / 1048576 << " MiB)  textures " << textures << " ("
                << textureBytes / 1048576 << " MiB)";
            out << "\nPeak native states " << s.peakPrepare.mStateBindings << "  unique transforms "
                << s.peakPrepare.mTransforms;
        }
        if (s.level >= 4)
            OMW::forEachUserStatsValue([&](const OMW::UserStats& stat) {
                double value = 0;
                if (engine.getAveragedAttribute(stat.mTaken, value))
                    out << "\n" << stat.mLabel << "  " << value * 1000 << " ms";
            });
        s.peakPrepareMs = 0;
        if (const auto* manager = MyGUIPlatform::RenderManager::getInstancePtr())
        {
            const auto view = manager->getViewSize();
            const int width = std::max(1, std::min(720, view.width - 16));
            const int textWidth = std::max(1, width - 24);
            s.header->setCoord(12, 12, textWidth, 200);
            s.header->setCaption(heading.str());
            const int headerHeight = s.header->getTextSize().height;
            s.header->setSize(textWidth, headerHeight);
            int y = 12 + headerHeight;
            s.chart = MyGUI::IntCoord();
            if (s.level >= 2)
            {
                const int chartHeight = std::max(16, std::min(112, view.height / 3));
                s.chart = { 20, 8 + y + 8, textWidth, chartHeight };
                y += 8 + chartHeight + 12;
            }
            s.details->setVisible(s.level >= 2);
            s.details->setCoord(12, y, textWidth, std::max(1, view.height));
            s.details->setCaption(out.str());
            if (s.level >= 2)
                y += s.details->getTextSize().height;
            s.panel->setSize(width, std::max(1, std::min(view.height - 16, y + 12)));
        }
    }
    std::span<const Render::Draw> StatsOverlay::graph()
    {
        auto& s = *mImpl;
        s.draws.clear();
        if (s.level < 2 || !s.panel || s.chart.width < 1 || s.chart.height < 1)
            return {};
        const auto* manager = MyGUIPlatform::RenderManager::getInstancePtr();
        if (!manager)
            return {};
        const auto size = manager->getViewSize();
        if (size.width < 1 || size.height < 1)
            return {};
        const float width = static_cast<float>(size.width), height = static_cast<float>(size.height);
        const float x = s.chart.left, y = s.chart.top, w = s.chart.width, h = s.chart.height;
        const float baseline = y + h - 4;
        const float scale = (h - 8) / 66.6667f;
        s.vertices.clear();
        s.indices.clear();
        s.rect(x, y, w, h, { 18, 18, 18, 255 }, width, height);
        for (float ms : { 16.6667f, 33.3333f, 66.6667f })
            s.rect(x, baseline - ms * scale, w, 1, { 110, 110, 110, 255 }, width, height);
        for (std::size_t i = 0; i < s.history.size(); ++i)
        {
            const auto start = (s.cursor + s.history.size() - s.count) % s.history.size();
            const double ms = i < s.count ? s.history[(start + i) % s.history.size()] : 0;
            const float barHeight = static_cast<float>(std::clamp(ms * scale, 0., double(h - 8)));
            s.rect(x + w * static_cast<float>(i) / s.history.size(), baseline - barHeight, w * .8f / s.history.size(),
                barHeight,
                ms > 33.3333 ? std::array<std::uint8_t, 4>{ 255, 70, 40, 240 }
                             : std::array<std::uint8_t, 4>{ 80, 240, 150, 240 },
                width, height);
        }
        if (s.mesh)
            s.backend.updateMesh(s.mesh, s.vertices, {});
        else
            s.mesh = s.backend.createMesh(s.vertices, s.indices);
        Render::Draw draw;
        draw.mMesh = s.mesh;
        draw.mDoubleSided = true;
        draw.mDepthTest = draw.mDepthWrite = false;
        draw.mBlend = Render::BlendMode::Alpha;
        s.draws.push_back(draw);
        return s.draws;
    }
}
