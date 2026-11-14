#include "statsoverlay.hpp"
#include "../../profile.hpp"

#include <components/myguiplatform/myguirendermanager.hpp>

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
        MyGUI::TextBox* text = nullptr;
        Render::MeshHandle mesh;
        std::vector<Render::Vertex> vertices;
        std::vector<std::uint32_t> indices;
        std::vector<Render::Draw> draws;
        std::array<double, 240> history{};
        std::size_t cursor = 0, count = 0, drawCount = 0, meshUploads = 0, imageUploads = 0;
        std::array<double, 5> phases{};
        std::chrono::steady_clock::time_point nextText{};
        explicit Impl(Render::Backend& b)
            : backend(b)
        {
        }
        void shutdown()
        {
            if (text && MyGUI::Gui::getInstancePtr())
                MyGUI::Gui::getInstance().destroyWidget(text);
            text = nullptr;
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
        mImpl->level = level;
        mImpl->nextText = {};
        if (!level)
            mImpl->shutdown();
    }
    void StatsOverlay::shutdown()
    {
        mImpl->shutdown();
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
        if (!s.text)
        {
            s.text = MyGUI::Gui::getInstance().createWidget<MyGUI::TextBox>(
                "NormalText", { 18, 14, 700, 400 }, MyGUI::Align::Left | MyGUI::Align::Top, "Notification");
            s.text->setTextAlign(MyGUI::Align::Left | MyGUI::Align::Top);
            s.text->setNeedMouseFocus(false);
            s.text->setNeedKeyFocus(false);
            s.text->setTextColour(MyGUI::Colour::White);
        }
        const auto now = std::chrono::steady_clock::now();
        if (now < s.nextText)
            return;
        s.nextText = now + std::chrono::milliseconds(150);
        const double mean = s.count ? std::accumulate(s.history.begin(), s.history.begin() + s.count, 0.) / s.count : 0;
        const double worst = s.count ? *std::max_element(s.history.begin(), s.history.begin() + s.count) : 0;
        std::ostringstream out;
        out << std::fixed << std::setprecision(1) << "Vulkan  " << (mean > 0 ? 1000 / mean : 0) << " FPS  " << mean
            << " ms  max " << worst << " ms";
        if (s.level >= 2)
        {
            out << "\nSubmission intervals: 16.7 / 33.3 / 66.7 ms (red = over 33.3 ms)";
            out << "\n\n\n\n\n\n\n\nCPU ready " << s.phases[0] << "  cull " << s.phases[1] << "  prepare "
                << s.phases[2] << " ms";
            out << "\nCPU submit " << s.phases[3] << "  retire " << s.phases[4] << " ms  (not GPU duration)";
            out << "\nDraw items " << s.drawCount << "  mesh uploads " << s.meshUploads << "  image uploads "
                << s.imageUploads;
        }
        if (s.level >= 3)
            out << "\nMeshes " << meshes << " (" << meshBytes / 1048576 << " MiB)  textures " << textures << " ("
                << textureBytes / 1048576 << " MiB)";
        if (s.level >= 4)
            OMW::forEachUserStatsValue([&](const OMW::UserStats& stat) {
                double value = 0;
                if (engine.getAveragedAttribute(stat.mTaken, value))
                    out << "\n" << stat.mLabel << "  " << value * 1000 << " ms";
            });
        if (const auto* manager = MyGUIPlatform::RenderManager::getInstancePtr())
        {
            const auto view = manager->getViewSize();
            s.text->setCoord(18, 14, std::max(1, std::min(700, view.width - 36)), std::max(1, view.height - 28));
        }
        s.text->setCaption(out.str());
    }
    std::span<const Render::Draw> StatsOverlay::graph()
    {
        auto& s = *mImpl;
        s.draws.clear();
        if (s.level < 2)
            return {};
        const auto* manager = MyGUIPlatform::RenderManager::getInstancePtr();
        if (!manager)
            return {};
        const auto size = manager->getViewSize();
        if (size.width < 1 || size.height < 1)
            return {};
        const float width = static_cast<float>(size.width), height = static_cast<float>(size.height);
        s.vertices.clear();
        s.indices.clear();
        s.rect(16, 62, 482, 106, { 0, 0, 0, 180 }, width, height);
        for (float ms : { 16.6667f, 33.3333f, 66.6667f })
            s.rect(16, 166 - ms * 1.5f, 480, 1, { 110, 110, 110, 180 }, width, height);
        for (std::size_t i = 0; i < s.history.size(); ++i)
        {
            const auto start = (s.cursor + s.history.size() - s.count) % s.history.size();
            const double ms = i < s.count ? s.history[(start + i) % s.history.size()] : 0;
            const float h = static_cast<float>(std::clamp(ms * 1.5, 0., 100.));
            s.rect(18 + 2 * static_cast<float>(i), 166 - h, 1.5f, h,
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
