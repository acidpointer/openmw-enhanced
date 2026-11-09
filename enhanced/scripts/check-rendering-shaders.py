#!/usr/bin/env python3
"""Static GLSL checks; no engine build, GL context, or executable test run.

Requires glslangValidator. Optional reviewed mod sources also receive a C++20
constant-evaluation check of the actual compatibility transform (GCC required).
Scene checks cover representative feature sets; legacy EXT_gpu_shader4 bodies
are additionally checked as core GLSL 330 because glslang lacks that extension.
Files are retained in a fresh temporary directory for failure inspection.
"""
from pathlib import Path
import argparse
import re
import subprocess
import tempfile
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--godrays', type=Path)
parser.add_argument('--depth-of-field', type=Path)
parser.add_argument('--cxx', default='c++')
args = parser.parse_args()
if bool(args.godrays) != bool(args.depth_of_field):
    parser.error('supply both reviewed mod sources, or neither')
root = Path(__file__).resolve().parents[2]
work = Path(tempfile.mkdtemp(prefix='openmw-render-static-'))
compat_paths = []
if args.godrays:
    src = (root / 'components/fx/shadercompatibility.cpp').read_text()

    def raw(name):
        return re.search('constexpr std::string_view ' + name + ' = R"\\w*\\((.*?)\\)\\w*";', src, re.S).group(1)

    def fnv(s):
        h = 14695981039346656037
        for b in s.encode():
            h = (h ^ b) * 1099511628211 & (1 << 64) - 1
        return h
    god = args.godrays.read_text(encoding='utf-8-sig')
    dof = args.depth_of_field.read_text(encoding='utf-8-sig')
    rays = re.search('"        vec2 screendir = \\(Tex - sunview\\);", R"GLSL\\((.*?)\\)GLSL"', src, re.S).group(1)
    dofzero = re.search('"float c = color.a \\* Rfixed;", R"GLSL\\((.*?)\\)GLSL"', src, re.S).group(1)
    g = god.replace('        vec2 screendir = (Tex - sunview);', rays)
    d = dof.replace('float c = color.a * Rfixed;', dofzero).replace(
        'float c, fog = 0.0;', 'float c = 0.0, fog = 0.0;')
    c = g.replace('shared {', 'shared {\n' + raw('maskMapping'))
    c = c.replace('fragment stretch {', raw('maskTargets') + '\nfragment stretch(target=Enhanced_GodrayDepth) {')
    c = c.replace('fragment blurRHalf {', 'fragment blurRHalf(target=Enhanced_GodrayBlur, rt1=Enhanced_GodrayDepth) {')
    c = c.replace('fragment rays {', 'fragment rays(rt1=Enhanced_GodrayBlur) {')
    c = c.replace('omw_FragColor = vec4(0, 0, 0, depth);', 'omw_FragColor = vec4(depth);')
    c = c.replace('omw_FragColor = vec4(0.0,0.0,0.0,alpha);', 'omw_FragColor = vec4(alpha);')
    c = c.replace('omw_GetLastPass(coords).a', 'omw_Texture2D(Enhanced_GodrayBlur, enhancedSceneToMask(coords)).r')
    a = c.index('fragment stretch(')
    b = c.index('fragment rays(')
    m = c[a:b].replace('vec2 Tex = omw_TexCoord;', 'vec2 Tex = enhancedMaskToScene(omw_TexCoord);')
    for coord in ['Tex', 'Tex + radial', 'Tex - radial', 'Tex + 2.0*radial', 'Tex - 2.0*radial']:
        m = m.replace('omw_GetLastPass(' + coord + ').a', 'omw_Texture2D(Enhanced_GodrayDepth, enhancedSceneToMask(' + coord + ')).r')
    c = c[:a] + m + c[b:]
    header = '#include <optional>\n    #include <string>\n    #include <string_view>\n    #include <utility>\n    namespace Fx { struct CompatibleShader { std::string mSource; std::string_view mRevision; }; }\n    '
    cpp = src.replace('#include "shadercompatibility.hpp"', header)
    cpp = cpp.replace('std::uint64_t fingerprint(', 'constexpr std::uint64_t fingerprint(')
    cpp = cpp.replace('bool replaceOnce(', 'constexpr bool replaceOnce(')
    cpp = cpp.replace('std::optional<CompatibleShader> optimizeLegacyShader(', 'constexpr std::optional<CompatibleShader> optimizeLegacyShader(')
    for i, (name, original, compact, expected) in enumerate([('godrays', god, False, g), ('godrays', god, True, c), ('depth_of_field', dof, True, d)]):
        if ')INPUT"' in original:
            raise ValueError("shader source contains the checker raw-string delimiter")
        cpp += '\nstatic_assert([] { auto result = Fx::optimizeLegacyShader("' + name + '", R"INPUT(' + original + ')INPUT", ' + str(compact).lower() + '); return result && result->mSource.size() == ' + str(len(expected.encode())) + ' && fingerprint(result->mSource) == ' + str(fnv(expected)) + 'ull; }());\n'
        (work / (name + ('-compact' if compact and name == 'godrays' else '') + '.omwfx')).write_text(expected)
    Path(str(work / 'compat-static.cpp')).write_text(cpp)
    r = subprocess.run([args.cxx, '-std=c++20', '-fsyntax-only', '-fconstexpr-ops-limit=1000000000', str(work / 'compat-static.cpp')], capture_output=True, text=True)
    if r.returncode:
        raise SystemExit(r.stdout + r.stderr)
    print('C++ constant evaluation verifies both reviewed shaders and compact/noncompact variants against generated GLSL sources.')
    compat_paths = [work / 'godrays.omwfx', work / 'godrays-compact.omwfx', work / 'depth_of_field.omwfx']
api = (root / 'components/fx/pass.cpp').read_text()
header = re.search('std::string header = R"GLSL\\((.*?)\\)GLSL";', api, re.S).group(1)
state = (root / 'components/fx/stateupdater.hpp').read_text()
types = {n: dict(Mat4='mat4', Vec4='vec4', Vec2='vec2', Float='float', Int='int', Bool='bool')[t] for n, t in re.findall('struct (\\w+) : Std140::(\\w+)\\s*\\{\\s*static constexpr std::string_view sName = "\\w+";', state)}
names = {n: s for n, s in re.findall('struct (\\w+) : Std140::\\w+\\s*\\{\\s*static constexpr std::string_view sName = "(\\w+)";', state)}
order = re.search('using UniformData = Std140::UBO<(.*?)>;', state, re.S).group(1)
struct = 'struct _omw_data {\n' + ''.join((types[n] + ' ' + names[n] + ';\n' for n in re.findall('\\w+', order))) + '};\n'

def blocks(s):
    result = []
    pattern = re.compile('\\b(shared|fragment|vertex|technique|uniform_\\w+|sampler_\\w+|render_target)(?:\\s+(\\w+))?(?:\\([^)]*\\))?\\s*\\{')
    pos = 0
    while (m := pattern.search(s, pos)):
        i = start = m.end()
        depth = 1
        while depth:
            if s.startswith('//', i):
                i = s.index('\n', i)
                continue
            if s.startswith('/*', i):
                i = s.index('*/', i + 2) + 2
                continue
            if s[i] == '"':
                i += 1
                while s[i] != '"':
                    i += 2 if s[i] == '\\' else 1
                i += 1
                continue
            depth += (s[i] == '{') - (s[i] == '}')
            i += 1
        result.append((m.group(1), m.group(2), s[start:i - 1]))
        pos = i
    return result
paths = [root / 'files/data/shaders/screen_space_shadows.omwfx', root / 'files/data/shaders/internal_distortion.omwfx'] + compat_paths
count = 0
for path in paths:
    s = path.read_text()
    bs = blocks(s)
    shared = '\n'.join((b for k, n, b in bs if k == 'shared'))
    for ver in (120, 330):
        for multi in (0,) if ver == 120 else (0, 1):
            for rz in (0, 1):
                for normals in (0, 1) if 'screen_space_shadows' in path.name else (0,):
                    for mode in ('static', 'uniform'):
                        defs = {'@version': str(ver), '@profile': 'compatibility' if ver >= 330 else '', '@extensions': '#extension GL_OVR_multiview2 : require' if multi else '', '@uboStruct': struct, '@apiVersion': '1', '@reverseZ': str(rz), '@radialFog': '1', '@exponentialFog': '1', '@hdr': '0', '@normals': str(normals), '@useBindings': str(int(ver >= 330)), '@multiview': str(multi), '@in': 'in' if ver >= 330 else 'varying', '@out': 'out' if ver >= 330 else 'varying', '@position': 'gl_Position', '@texture1D': 'texture' if ver >= 330 else 'texture1D', '@texture2DArray': 'texture', '@texture2D': 'texture' if ver >= 330 else 'texture2D', '@texture3D': 'texture' if ver >= 330 else 'texture3D', '@vertex': '_omw_Vertex' if ver >= 330 else 'gl_Vertex', '@fragColor': '_omw_FragColor' if ver >= 330 else 'gl_FragColor', '@fragBinding': 'out vec4 _omw_FragColor;' if ver >= 330 else '', '@builtinSampler': 'sampler2DArray' if multi else 'sampler2D', '@pointLightCount': '256', '@ubo': str(int(ver >= 330))}
                        h = header
                        for key in sorted(defs, key=len, reverse=True):
                            h = h.replace(key, defs[key])
                        assert '@' not in h
                        decl = ''
                        for k, n, b in bs:
                            if k.startswith('uniform_'):
                                ty = k.removeprefix('uniform_')
                                default = re.search('\\bdefault\\s*=\\s*(.*?);', b).group(1)
                                if n == 'N' and mode == 'static':
                                    default = '50'
                                if mode == 'uniform' or re.search('\\bstatic\\s*=\\s*false', b):
                                    decl += 'uniform ' + ty + ' ' + n + ';\n'
                                else:
                                    decl += 'const ' + ty + ' ' + n + ' = ' + default + ';\n'
                            elif k.startswith('sampler_'):
                                decl += 'uniform sampler' + k.removeprefix('sampler_').upper() + ' ' + n + ';\n'
                            elif k == 'render_target':
                                decl += 'uniform sampler2D ' + n + ';\n'
                        for k, n, b in bs:
                            if k not in ('fragment', 'vertex'):
                                continue
                            file = work / (path.stem + '-' + n + '-' + str(ver) + '-' + str(multi) + str(rz) + str(normals) + '-' + mode + ('.frag' if k == 'fragment' else '.vert'))
                            file.write_text(h + decl + shared + '\n' + b)
                            r = subprocess.run(['glslangValidator', '-S', 'frag' if k == 'fragment' else 'vert', str(file)], capture_output=True, text=True)
                            if r.returncode:
                                raise SystemExit(r.stdout + r.stderr)
                            count += 1
print(count, 'current FX shader variants passed static GLSL validation, including constants/uniforms and saved N=50.')
base = root / 'files/shaders'

def expand(path, stack=()):
    assert path not in stack
    text = path.read_text()

    def inc(m):
        name = m.group(1)
        p = path.parent / name
        if not p.exists():
            p = base / name
        return expand(p, (*stack, path))
    text = re.sub('^\\s*#include\\s+"([^"]+)".*$', inc, text, flags=re.M)
    return re.sub('^\\s*@link.*$', '', text, flags=re.M)
count = 0
for name in ['objects.frag', 'terrain.frag', 'groundcover.frag', 'water.frag', 'bs/default.frag', 'bs/nolighting.frag']:
    text = expand(base / 'compatibility' / name)
    for modern in (False, True):
        for normals in (0, 1):
            for normalmap in (0, 1):
                for batch in (0, 1) if name == 'terrain.frag' and (not normalmap) else (0,):
                    d = {'useGPUShader4': str(int(modern)), 'normalMap': str(normalmap), 'specularMap': str(normalmap), 'forcePPL': '1', 'diffuseMap': '1', 'blendMap': '1', 'terrainLayerBatch': str(batch), 'disableNormals': '0', 'lightingMethodClustered': '0', 'lightingMethod': '1', 'reverseZ': '1', 'maxLights': '8', 'startLight': '1', 'endLight': '8', 'numViews': '2', 'waterRefraction': '1', 'useOVR_multiview': '0', 'softParticles': '1', 'shadows_enabled': '0', 'material': '0', 'skyBlending': '0'}
                    s = re.sub('@(\\w+)', lambda m: d.get(m.group(1), '0'), text)
                    s = s.replace('#version 120', '#version 120\n#define OMW_WRITE_NORMALS ' + str(normals), 1)
                    if modern:
                        s = s.replace('#version 120', '#version 330 compatibility', 1).replace('#extension GL_EXT_gpu_shader4: require', '')
                    file = work / (name.replace('/', '-') + '-n' + str(normals) + '-m' + str(normalmap) + '-b' + str(batch) + '.frag')
                    file.write_text(s)
                    r = subprocess.run(['glslangValidator', '-S', 'frag', str(file)], capture_output=True, text=True)
                    if r.returncode:
                        raise SystemExit(r.stdout + r.stderr)
                    count += 1
print(count, 'scene fragment variants pass static validation, including normals output and gated terrain batches.')
print('Static-check artifacts:', work)
