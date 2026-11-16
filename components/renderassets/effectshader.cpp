#include "effectshader.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <fstream>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <regex>
#include <sstream>
#include <stdexcept>

#ifndef _WIN32
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <poll.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace RenderAssets
{
    bool effectRequiresOffscreen(std::span<const std::uint32_t> words)
    {
        if (words.size() < 5 || words[0] != 0x07230203)
            return true;
        bool fragment = false;
        for (std::size_t offset = 5; offset < words.size();)
        {
            const auto count = words[offset] >> 16;
            const auto op = words[offset] & 0xffff;
            if (!count || count > words.size() - offset)
                return true;
            if (op == 15 && count >= 4 && words[offset + 1] == 4)
                fragment = true;
            // FragCoord, FrontFacing and signed Y derivatives change when the
            // window's negative-height viewport replaces an offscreen viewport.
            const auto sensitive = [](unsigned builtin) { return builtin == 15 || builtin == 17; };
            if ((op == 71 && count >= 4 && words[offset + 2] == 11 && sensitive(words[offset + 3]))
                || (op == 72 && count >= 5 && words[offset + 3] == 11 && sensitive(words[offset + 4])) || op == 208
                || op == 211 || op == 214)
                return true;
            offset += count;
        }
        return !fragment;
    }

    bool effectMayDiscard(std::span<const std::uint32_t> words)
    {
        if (words.size() < 5 || words[0] != 0x07230203)
            return true;
        bool fragment = false;
        for (std::size_t offset = 5; offset < words.size();)
        {
            const auto count = words[offset] >> 16;
            const auto op = words[offset] & 0xffff;
            if (!count || count > words.size() - offset)
                return true;
            if (op == 15 && count >= 4 && words[offset + 1] == 4) // Fragment OpEntryPoint
                fragment = true;
            // OpKill, OpTerminateInvocation, OpDemoteToHelperInvocationEXT.
            if (op == 252 || op == 4416 || op == 5380)
                return true;
            // BuiltIn SampleMask can suppress stores without a kill opcode.
            // Include member decorations for interface blocks as well.
            if ((op == 71 && count >= 4 && words[offset + 2] == 11 && words[offset + 3] == 20)
                || (op == 72 && count >= 5 && words[offset + 3] == 11 && words[offset + 4] == 20))
                return true;
            offset += count;
        }
        return !fragment;
    }

    std::string translateEffectShader(std::string_view source, bool fragment, std::span<const std::string> samplers)
    {
        const std::map<std::string, std::string> macros{ { "OMW_REVERSE_Z", "1" }, { "OMW_NATIVE_DEPTH", "1" },
            { "OMW_USE_BINDINGS", "1" }, { "OMW_MULTIVIEW", "0" }, { "omw_In", "in" }, { "omw_Out", "out" },
            { "omw_Vertex", "_omw_Vertex" }, { "omw_FragColor", "_omw_FragColor" }, { "omw_Texture1D", "texture" },
            { "omw_Texture2D", "texture" }, { "omw_Texture2DArray", "texture" }, { "omw_Texture3D", "texture" } };
        std::istringstream input{ std::string(source) };
        std::string result = "#version 450 core\n";
        // Preserve legacy GLSL spellings used by existing effect bodies.
        result += "#define OMW_NATIVE_DEPTH 1\n";
        result += "#define texture2D texture\n#define texture3D texture\n#define texture2DLod textureLod\n";
        result += fragment ? "#define varying in\n" : "#define varying out\n#define attribute in\n";
        bool version = false;
        std::string line;
        while (std::getline(input, line))
        {
            const auto start = line.find_first_not_of(" \t\r");
            const auto text = start == std::string::npos ? std::string_view{} : std::string_view(line).substr(start);
            if (text.starts_with("#version"))
            {
                if (version)
                    throw std::invalid_argument("Multiple shader version directives");
                version = true;
                continue;
            }
            if (text == "out vec4 omw_FragColor;")
                continue;
            if (text.starts_with("#define "))
            {
                std::istringstream define{ std::string(text.substr(8)) };
                std::string name;
                define >> name;
                if (const auto found = macros.find(name); found != macros.end())
                {
                    result += "#define " + name + " " + found->second + "\n";
                    if (fragment && name == "omw_FragColor")
                        result += "layout(location=0) out vec4 _omw_FragColor;\n";
                    continue;
                }
            }
            result += line + '\n';
        }
        if (!version)
            throw std::invalid_argument("Effect has no generated GLSL version header");
        // Assign descriptor layouts in the shader itself. CLI overrides use
        // resource names that can differ for anonymous blocks between stages.
        // Every linked stage must use exactly the same namespace and bindings.
        const std::regex declaration(R"(uniform\s+[iu]?sampler\w*\s+(\w+))");
        std::string bound;
        std::size_t offset = 0;
        for (std::sregex_iterator match(result.begin(), result.end(), declaration), end; match != end; ++match)
        {
            const auto name = (*match)[1].str();
            const auto found = std::find(samplers.begin(), samplers.end(), name);
            if (found == samplers.end())
                throw std::invalid_argument("Unmapped effect sampler: " + name);
            const auto position = static_cast<std::size_t>(match->position());
            bound.append(result, offset, position - offset);
            bound += "layout(set=0,binding=" + std::to_string(found - samplers.begin()) + ") ";
            bound += match->str();
            offset = position + match->length();
        }
        bound.append(result, offset, std::string::npos);
        const std::string engineBlock = "layout(std140) uniform _data";
        const std::string nativeBlock
            = "layout(std140,set=0,binding=" + std::to_string(samplers.size() + 1) + ") uniform _data";
        for (auto pos = bound.find(engineBlock); pos != std::string::npos;
            pos = bound.find(engineBlock, pos + nativeBlock.size()))
            bound.replace(pos, engineBlock.size(), nativeBlock);
        if (fragment)
            bound = std::regex_replace(bound, std::regex(R"(\bgl_FragColor\b)"), "_omw_FragColor");
        return bound;
    }

    namespace
    {
        struct Type
        {
            unsigned op = 0;
            std::vector<std::uint32_t> args;
        };
        struct Decoration
        {
            std::optional<unsigned> binding, set, offset;
            unsigned arrayStride = 0, matrixStride = 0;
            bool rowMajor = false, block = false;
        };
        std::string spirvString(std::span<const std::uint32_t> words)
        {
            const auto* text = reinterpret_cast<const char*>(words.data());
            const auto* end = static_cast<const char*>(std::memchr(text, 0, words.size_bytes()));
            if (!end)
                throw std::invalid_argument("Unterminated SPIR-V name");
            return { text, end };
        }
        std::vector<std::uint32_t> readShader(const std::filesystem::path& path)
        {
            std::ifstream input(path, std::ios::binary | std::ios::ate);
            const auto size = input.tellg();
            if (size < 20 || size > 16 * 1024 * 1024 || size % 4)
                throw std::runtime_error("Invalid compiled effect size");
            std::vector<std::uint32_t> result(static_cast<std::size_t>(size) / 4);
            input.seekg(0);
            if (!input.read(reinterpret_cast<char*>(result.data()), size))
                throw std::runtime_error("Cannot read compiled effect");
            return result;
        }
    }

    std::vector<EffectResource> reflectEffectShader(std::span<const std::uint32_t> words)
    {
        if (words.size() < 5 || words[0] != 0x07230203 || !words[3] || words[3] > 1024 * 1024)
            throw std::invalid_argument("Invalid effect SPIR-V header");
        std::map<unsigned, Type> types;
        std::map<unsigned, std::uint32_t> constants;
        std::map<unsigned, std::string> names;
        std::map<std::pair<unsigned, unsigned>, std::string> memberNames;
        std::map<unsigned, Decoration> decorations;
        std::map<std::pair<unsigned, unsigned>, Decoration> memberDecorations;
        std::map<unsigned, std::pair<unsigned, unsigned>> variables;
        for (std::size_t pos = 5; pos < words.size();)
        {
            const auto size = words[pos] >> 16;
            const auto op = words[pos] & 0xffff;
            if (!size || size > words.size() - pos)
                throw std::invalid_argument("Truncated effect instruction");
            const auto args = words.subspan(pos + 1, size - 1);
            const auto need = [&](std::size_t count) {
                if (args.size() < count)
                    throw std::invalid_argument("Invalid effect instruction");
            };
            if (op == 5)
            {
                need(2);
                names[args[0]] = spirvString(args.subspan(1));
            }
            else if (op == 6)
            {
                need(3);
                memberNames[{ args[0], args[1] }] = spirvString(args.subspan(2));
            }
            else if (op >= 20 && op <= 32)
            {
                need(1);
                types[args[0]] = { op, { args.begin() + 1, args.end() } };
            }
            else if (op == 43)
            {
                need(3);
                constants[args[1]] = args[2];
            }
            else if (op == 59)
            {
                need(3);
                variables[args[1]] = { args[0], args[2] };
            }
            else if (op == 71 || op == 72)
            {
                const bool member = op == 72;
                need(member ? 3 : 2);
                auto& dec = member ? memberDecorations[{ args[0], args[1] }] : decorations[args[0]];
                const auto kind = args[member ? 2 : 1];
                if (kind == 2)
                    dec.block = true;
                else if (kind == 4)
                    dec.rowMajor = true;
                else if (kind == 6 || kind == 7 || kind == 33 || kind == 34 || kind == 35)
                {
                    need(member ? 4 : 3);
                    const auto value = args[member ? 3 : 2];
                    if (kind == 6)
                        dec.arrayStride = value;
                    if (kind == 7)
                        dec.matrixStride = value;
                    if (kind == 33)
                        dec.binding = value;
                    if (kind == 34)
                        dec.set = value;
                    if (kind == 35)
                        dec.offset = value;
                }
            }
            pos += size;
        }
        const auto type = [&](unsigned id) -> const Type& {
            const auto found = types.find(id);
            if (found == types.end())
                throw std::invalid_argument("Unknown effect uniform type");
            return found->second;
        };
        std::function<unsigned(
            unsigned, const Decoration&, unsigned, std::string, std::vector<EffectMember>&, unsigned)>
            flatten;
        flatten = [&](unsigned id, const Decoration& dec, unsigned offset, std::string name,
                      std::vector<EffectMember>& output, unsigned level) -> unsigned {
            if (level > 24 || offset > 65536 || output.size() > 4096)
                throw std::invalid_argument("Effect uniform layout exceeds limits");
            const auto& t = type(id);
            if (t.op == 30)
            {
                unsigned size = 0;
                for (unsigned i = 0; i < t.args.size(); ++i)
                {
                    const auto& md = memberDecorations[{ id, i }];
                    if (!md.offset)
                        throw std::invalid_argument("Effect member has no offset");
                    if (*md.offset > 65536 || offset > 65536 - *md.offset)
                        throw std::invalid_argument("Effect member offset exceeds limits");
                    const auto memberName = memberNames[{ id, i }];
                    size = std::max(size,
                        *md.offset
                            + flatten(t.args[i], md, offset + *md.offset,
                                name.empty() ? memberName : name + '.' + memberName, output, level + 1));
                }
                return (size + 15) / 16 * 16;
            }
            unsigned count = 1, stride = 0;
            if (t.op == 28)
            {
                if (t.args.size() != 2 || !constants.contains(t.args[1]))
                    throw std::invalid_argument("Invalid effect array");
                count = constants.at(t.args[1]);
                stride = decorations[id].arrayStride;
                id = t.args[0];
                if (!stride || !count || count > 4096 || type(id).op == 30 || type(id).op == 28)
                    throw std::invalid_argument("Unsupported effect uniform array layout");
            }
            EffectMember member;
            member.mName = std::move(name);
            member.mOffset = offset;
            member.mCount = count;
            member.mArrayStride = stride;
            if (type(id).op == 24)
            {
                const auto& m = type(id);
                if (m.args.size() != 2 || !dec.matrixStride || dec.rowMajor)
                    throw std::invalid_argument("Unsupported effect matrix layout");
                member.mColumns = m.args[1];
                member.mMatrixStride = dec.matrixStride;
                id = m.args[0];
            }
            if (type(id).op == 23)
            {
                const auto& v = type(id);
                if (v.args.size() != 2)
                    throw std::invalid_argument("Invalid effect vector");
                member.mRows = v.args[1];
                id = v.args[0];
            }
            const auto& scalar = type(id);
            if (scalar.op == 22 && scalar.args == std::vector<std::uint32_t>{ 32 })
                member.mScalar = EffectMember::Scalar::Float;
            else if (scalar.op == 21 && scalar.args.size() == 2 && scalar.args[0] == 32)
                member.mScalar = scalar.args[1] ? EffectMember::Scalar::Int : EffectMember::Scalar::UInt;
            else if (scalar.op == 20)
                member.mScalar = EffectMember::Scalar::Int;
            else
                throw std::invalid_argument("Effect requires unsupported scalar type");
            if (!member.mRows || member.mRows > 4 || !member.mColumns || member.mColumns > 4)
                throw std::invalid_argument("Invalid effect uniform shape");
            std::uint64_t bytes
                = member.mColumns > 1 ? std::uint64_t(member.mMatrixStride) * member.mColumns : member.mRows * 4;
            if (stride)
            {
                if (stride < bytes)
                    throw std::invalid_argument("Effect array elements overlap");
                bytes = std::uint64_t(stride) * count;
            }
            if (bytes > 65536 || offset + bytes > 65536)
                throw std::invalid_argument("Effect uniform block too large");
            output.push_back(std::move(member));
            return static_cast<unsigned>(bytes);
        };
        std::vector<EffectResource> result;
        for (const auto& [id, variable] : variables)
        {
            const auto storage = variable.second;
            if (storage != 0 && storage != 2)
            {
                if (storage == 9 || storage == 12)
                    throw std::invalid_argument("Effect storage buffers/push constants are unsupported");
                continue;
            }
            const auto& dec = decorations[id];
            if (!dec.binding || !dec.set || *dec.set != 0)
                throw std::invalid_argument("Effect descriptor requires set zero and binding");
            const auto& pointer = type(variable.first);
            if (pointer.op != 32 || pointer.args.size() != 2)
                throw std::invalid_argument("Invalid effect pointer");
            const auto& resource = type(pointer.args[1]);
            EffectResource output;
            output.mBinding.mBinding = *dec.binding;
            output.mName = names[id];
            if (storage == 0)
            {
                if (resource.op != 27 || resource.args.size() != 1)
                    throw std::invalid_argument("Only combined effect samplers are supported");
                const auto& image = type(resource.args[0]);
                // Dim2D=1 or Dim3D=2; non-array, non-MS sampled images.
                if (image.op != 25 || image.args.size() < 7 || (image.args[1] != 1 && image.args[1] != 2)
                    || image.args[3] || image.args[4] || image.args[5] != 1)
                    throw std::invalid_argument("Effect requires an unsupported texture dimension or storage texture");
            }
            else
            {
                if (resource.op != 30 || !decorations[pointer.args[1]].block)
                    throw std::invalid_argument("Effect uniform descriptor is not a block");
                output.mBinding.mKind = Render::EffectBindingKind::UniformBuffer;
                output.mName = names[pointer.args[1]];
                output.mBinding.mSize = flatten(pointer.args[1], {}, 0, {}, output.mMembers, 0);
            }
            result.push_back(std::move(output));
        }
        return result;
    }

    std::vector<std::string> effectCompilerArguments(
        const std::filesystem::path& compiler, const std::vector<std::string>& samplers)
    {
        // Inputs precede options. Descriptor layouts are explicit in the
        // translated source; only standalone globals need the compiler's block.
        std::vector<std::string> arguments{ compiler.string(), "effect.vert", "effect.frag", "--target-env",
            "vulkan1.1", "-V", "-R", "-l", "-Os", "--auto-map-bindings", "--auto-map-locations",
            "--set-default-uniform-block", "_NativeGlobals", "0", std::to_string(samplers.size()) };
        return arguments;
    }

    EffectShader compileEffectShader(const std::filesystem::path& compiler, std::string_view vertex,
        std::string_view fragment, const std::vector<std::string>& samplers, std::stop_token stop)
    {
        if (stop.stop_requested())
            throw std::runtime_error("Effect shader compilation cancelled");
#ifdef _WIN32
        throw std::runtime_error("Native effect shader compiler launching is not implemented on Windows");
#else
        // Shader work runs off the render thread, with one compiler process at
        // a time so enabling a large effect chain cannot saturate all CPU cores.
        static std::mutex compilerMutex;
        static std::condition_variable_any compilerAvailable;
        static bool compilerBusy = false;
        {
            std::unique_lock lock(compilerMutex);
            if (!compilerAvailable.wait(lock, stop, [] { return !compilerBusy; }) || stop.stop_requested())
                throw std::runtime_error("Effect shader compilation cancelled");
            compilerBusy = true;
        }
        struct ReleaseCompiler
        {
            ~ReleaseCompiler()
            {
                {
                    const std::lock_guard lock(compilerMutex);
                    compilerBusy = false;
                }
                compilerAvailable.notify_one();
            }
        } releaseCompiler;
        std::string pattern = (std::filesystem::temp_directory_path() / "openmw-vulkan-fx-XXXXXX").string();
        if (!mkdtemp(pattern.data()))
            throw std::runtime_error("Cannot create effect compiler directory");
        struct Directory
        {
            std::filesystem::path path;
            ~Directory()
            {
                std::error_code error;
                std::filesystem::remove_all(path, error);
            }
        } directory{ pattern };
        const auto write = [&](const char* name, const std::string& text) {
            std::ofstream output(directory.path / name);
            output << text;
            if (!output)
                throw std::runtime_error("Cannot write translated effect");
        };
        write("effect.vert", translateEffectShader(vertex, false, samplers));
        write("effect.frag", translateEffectShader(fragment, true, samplers));
        auto arguments = effectCompilerArguments(compiler, samplers);
        std::vector<char*> argv;
        for (auto& arg : arguments)
            argv.push_back(arg.data());
        argv.push_back(nullptr);
        int pipefd[2];
        if (pipe2(pipefd, O_CLOEXEC))
            throw std::runtime_error("Cannot create shader compiler pipe");
        const auto child = fork();
        if (child == 0)
        {
            if (chdir(pattern.c_str()) || dup2(pipefd[1], STDOUT_FILENO) < 0 || dup2(pipefd[1], STDERR_FILENO) < 0)
                _exit(126);
            close(pipefd[0]);
            close(pipefd[1]);
            execv(argv[0], argv.data());
            _exit(127);
        }
        close(pipefd[1]);
        if (child < 0)
        {
            close(pipefd[0]);
            throw std::runtime_error("Cannot launch effect compiler");
        }
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        std::string log;
        int status = 0;
        bool finished = false;
        for (;;)
        {
            pollfd fd{ pipefd[0], POLLIN, 0 };
            const int ready = poll(&fd, 1, 50);
            if (ready > 0 && fd.revents)
            {
                char buffer[4096];
                const auto count = read(pipefd[0], buffer, sizeof(buffer));
                if (count > 0 && log.size() < 65536)
                    log.append(buffer, std::min<std::size_t>(count, 65536 - log.size()));
                if (count == 0 && finished)
                    break;
            }
            if (!finished)
            {
                const auto waited = waitpid(child, &status, WNOHANG);
                if (waited == child)
                    finished = true;
                else if (waited < 0 && errno != EINTR)
                {
                    finished = true;
                    status = 1 << 8;
                }
            }
            if (stop.stop_requested() || std::chrono::steady_clock::now() >= deadline)
            {
                if (!finished)
                {
                    kill(child, SIGKILL);
                    while (waitpid(child, &status, 0) < 0 && errno == EINTR)
                    {
                    }
                }
                close(pipefd[0]);
                throw std::runtime_error(stop.stop_requested() ? "Effect shader compilation cancelled"
                                                               : "Effect shader compilation timed out: " + log);
            }
            if (finished && ready == 0)
                break;
        }
        close(pipefd[0]);
        if (!WIFEXITED(status) || WEXITSTATUS(status))
            throw std::runtime_error("Effect GLSL translation failed: " + log);
        EffectShader result;
        result.mVertex = readShader(directory.path / "vert.spv");
        result.mFragment = readShader(directory.path / "frag.spv");
        result.mMayDiscard = effectMayDiscard(result.mFragment);
        result.mRequiresOffscreen = effectRequiresOffscreen(result.mFragment);
        const auto reflect = [&](const auto& code) {
            auto resources = reflectEffectShader(code);
            for (auto& resource : resources)
                if (resource.mBinding.mKind == Render::EffectBindingKind::UniformBuffer)
                {
                    if (resource.mBinding.mBinding == samplers.size())
                        resource.mName = "_NativeGlobals";
                    else if (resource.mBinding.mBinding == samplers.size() + 1)
                        resource.mName = "_data";
                    else
                        throw std::invalid_argument("Unexpected effect uniform buffer binding");
                }
            return resources;
        };
        result.mResources = reflect(result.mVertex);
        result.mVertexTextureReads = std::any_of(result.mResources.begin(), result.mResources.end(),
            [](const auto& resource) { return resource.mBinding.mKind == Render::EffectBindingKind::Texture; });
        for (auto& resource : reflect(result.mFragment))
        {
            const auto existing = std::find_if(result.mResources.begin(), result.mResources.end(),
                [&](const auto& r) { return r.mBinding.mBinding == resource.mBinding.mBinding; });
            if (existing == result.mResources.end())
                result.mResources.push_back(std::move(resource));
            else
            {
                if (existing->mBinding.mKind != resource.mBinding.mKind || existing->mName != resource.mName)
                    throw std::runtime_error("Linked effect stages disagree on a descriptor");
                existing->mBinding.mSize = std::max(existing->mBinding.mSize, resource.mBinding.mSize);
                for (auto& member : resource.mMembers)
                {
                    const auto found = std::find_if(existing->mMembers.begin(), existing->mMembers.end(),
                        [&](const auto& m) { return m.mName == member.mName; });
                    if (found == existing->mMembers.end())
                        existing->mMembers.push_back(std::move(member));
                    else if (*found != member)
                        throw std::runtime_error("Linked effect stages disagree on a uniform layout");
                }
            }
        }
        return result;
#endif
    }
}
