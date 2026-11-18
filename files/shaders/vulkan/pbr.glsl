#include "pbrenvironment.glsl"
#include "srgb.glsl"

layout(set = 3, binding = 0) uniform sampler2D pbrNormal;
layout(set = 3, binding = 1) uniform sampler2D pbrSpecular;
layout(set = 3, binding = 2) uniform sampler2D pbrProperties;
layout(set = 3, binding = 3) uniform sampler2D pbrEmissive;

vec3 pbrUnit(vec3 value)
{
    return value * inversesqrt(max(dot(value, value), 1e-12));
}

// The frame comes from the actual transformed UVs and skinned view-space
// position. Its sign follows mirrored UVs independently of viewport winding.
vec3 mappedNormal(vec3 normal, vec2 du, vec2 dv, vec3 dx, vec3 dy, vec3 sampleNormal)
{
    float determinant = du.x * dv.y - du.y * dv.x;
    float orientation = determinant < 0.0 ? -1.0 : 1.0;
    vec3 tangent = orientation * (dx * dv.y - dy * du.y);
    tangent -= normal * dot(normal, tangent);
    vec3 referenceBitangent = orientation * (dy * du.x - dx * dv.x);
    if (abs(determinant) <= 1e-20 || dot(tangent, tangent) <= 1e-20)
        return normal;
    tangent *= inversesqrt(dot(tangent, tangent));
    vec3 bitangent = cross(normal, tangent);
    bitangent *= dot(bitangent, referenceBitangent) < 0.0 ? -1.0 : 1.0;
    vec3 result = tangent * sampleNormal.x + bitangent * sampleNormal.y + normal * sampleNormal.z;
    return dot(result, result) > 1e-10 ? pbrUnit(result) : normal;
}

struct PbrSurface
{
    vec3 normal;
    vec3 eye;
    vec3 albedo;
    vec3 reflectance;
    vec3 grazingReflectance;
    float roughness;
    float alphaSquared;
    float smithView;
    float ndotv;
    bool hasSpecular;
};

vec3 pbrFresnel(PbrSurface surface, float cosine)
{
    float x = 1.0 - clamp(cosine, 0.0, 1.0);
    float x2 = x * x;
    return mix(surface.reflectance, surface.grazingReflectance, x2 * x2 * x);
}

vec3 pbrDirect(PbrSurface surface, vec3 direction, vec3 diffuse, vec3 specular)
{
    float ndotl = dot(surface.normal, direction);
    if (ndotl <= 0.0)
        return vec3(0.0);
    vec3 lambert = ndotl * diffuse * surface.albedo;
    if (!surface.hasSpecular)
        return lambert;
    vec3 halfway = pbrUnit(direction + surface.eye);
    float ndoth = clamp(dot(surface.normal, halfway), 0.0, 1.0);
    vec3 fresnel = pbrFresnel(surface, dot(direction, halfway));
    float h2 = ndoth * ndoth;
    float denominator = (1.0 - h2) + h2 * surface.alphaSquared;
    float distribution = surface.alphaSquared / (denominator * denominator);
    // Fast height-correlated Smith visibility. Light colors use OpenMW's
    // familiar Lambert normalization, so the common 1/pi cancels out.
    float visibility = 0.5 / max(ndotl * surface.smithView
        + surface.ndotv * mix(ndotl, 1.0, surface.roughness), 1e-5);
    return lambert * (1.0 - fresnel) + specular * fresnel * (ndotl * distribution * visibility);
}

float roughnessFromShininess(float shininess)
{
    // Legacy Blinn-Phong exponent to a perceptual microfacet roughness.
    return sqrt(sqrt(2.0 / (clamp(isnan(shininess) ? 0.0 : shininess, 0.0, 2048.0) + 2.0)));
}

vec3 normalFromMap(vec3 texel, uint flags)
{
    vec3 value = texel * 2.0 - 1.0;
    if ((flags & 512u) != 0u)
        value.z = sqrt(max(1.0 - dot(value.xy, value.xy), 0.0));
    return value;
}

vec3 materialProperties(vec3 values)
{
    return clamp(mix(values, vec3(1.0, 1.0, 0.0), isnan(values)), 0.0, 1.0);
}

vec4 bakePbrTerrain()
{
    InstanceData data = instances[pbrInstance];
    uint flags = data.lighting.z;
    bool hasTerrainGloss = (flags & 1024u) != 0u;
    float coverage = materialKind > 0.5 ? texture(blendMap, maskUV).a : 1.0;
    if ((flags & 16384u) != 0u)
    {
        vec2 slope = vec2(0.5);
        if ((flags & 32u) != 0u)
        {
            vec3 texel = texture(pbrNormal, uv).rgb;
            if ((flags & 32768u) != 0u)
                texel = encodeSrgb(texel);
            vec3 normal = normalFromMap(texel, flags);
            if (dot(normal, normal) > 1e-10)
                slope = pbrUnit(normal).xy * 0.5 + 0.5;
        }
        // Terrain's diffuse alpha is a specular coefficient only when the
        // layer has opted into the legacy terrain specular convention.
        float strength = hasTerrainGloss ? texture(diffuseMap, uv).a
            : pbrDefaultDielectricWeight(data.uvRow0.w, data.uvRow1.w, (flags & 128u) != 0u);
        return vec4(slope, strength, coverage);
    }
    float shininess = hasTerrainGloss ? 128.0 : data.specular.w;
    float roughness = data.uvRow0.w >= 0.0 ? data.uvRow0.w : roughnessFromShininess(shininess);
    vec3 properties = vec3(1.0, roughness, data.uvRow1.w);
    if ((flags & 128u) != 0u)
    {
        properties = texture(pbrProperties, uv).rgb;
        if ((flags & 131072u) != 0u)
            properties = encodeSrgb(properties);
    }
    return vec4(materialProperties(properties), coverage);
}

vec4 samplePbr(sampler2D map, mat2 primary, mat2 secondary, uint alternate)
{
    return alternate != 0u ? textureGrad(map, maskUV, secondary[0], secondary[1])
                          : textureGrad(map, uv, primary[0], primary[1]);
}

vec3 shadePbr(vec4 base, inout vec3 normal, mat2 primary, mat2 secondary, vec3 dx, vec3 dy)
{
    InstanceData data = instances[pbrInstance];
    uint flags = data.lighting.z;
    uint alternate = flags >> 19u;
    vec3 baseColor = (flags & 2048u) != 0u ? decodeSrgb(base.rgb) : base.rgb;
    vec3 albedo = baseColor * color.rgb;
    float bakedStrength = -1.0;
    float normalVariance = 0.0;
    if ((flags & 32u) != 0u)
    {
        bool useSecondary = (alternate & 1u) != 0u;
        vec2 coordinates = useSecondary ? maskUV : uv;
        mat2 gradient = useSecondary ? secondary : primary;
        vec3 texel = textureGrad(pbrNormal, coordinates, gradient[0], gradient[1]).xyz;
        if ((flags & 32768u) != 0u)
            texel = encodeSrgb(texel);
        if ((flags & 262144u) != 0u)
            bakedStrength = texel.b;
        vec3 mapped = normalFromMap(texel, flags);
        float lengthSquared = dot(mapped, mapped);
        if ((flags & 512u) == 0u && lengthSquared > 1e-10)
            normalVariance = 0.25 * max(1.0 - lengthSquared, 0.0);
        normal = mappedNormal(normal, gradient[0], gradient[1], dx, dy, mapped);
    }
    float shininess = data.specular.w;
    bool hasPropertiesMap = (flags & 128u) != 0u;
    float defaultWeight = pbrDefaultDielectricWeight(data.uvRow0.w, data.uvRow1.w, hasPropertiesMap);
    // The terrain composite carries a weight for each texel. Its packed
    // roughness texture alone cannot distinguish glossy layers from matte ones.
    vec3 dielectricWeight = vec3(pbrTerrainDielectricWeight(defaultWeight, bakedStrength));
    if (bakedStrength < 0.0 && defaultWeight == 0.0 && shininess > 0.0)
        dielectricWeight = clamp(specularColor, 0.0, 1.0);
    if ((flags & 64u) != 0u)
    {
        vec4 legacy = samplePbr(pbrSpecular, primary, secondary, alternate & 2u);
        if ((flags & 65536u) != 0u)
            legacy.rgb = encodeSrgb(legacy.rgb);
        shininess = 255.0 * legacy.a;
        dielectricWeight = clamp(legacy.rgb * data.ambient.a, 0.0, 1.0);
    }
    else if ((flags & 1024u) != 0u)
    {
        shininess = 128.0;
        dielectricWeight = vec3(base.a);
    }
    float roughness = data.uvRow0.w;
    float metalness = data.uvRow1.w;
    float occlusion = 1.0;
    if (hasPropertiesMap)
    {
        vec3 packed = textureGrad(pbrProperties, uv, primary[0], primary[1]).rgb;
        if ((flags & 131072u) != 0u)
            packed = encodeSrgb(packed);
        packed = materialProperties(packed);
        occlusion = packed.r;
        roughness = packed.g;
        metalness = packed.b;
    }
    // Analytically filtered hemispherical environment. Its first directional
    // band fades with lobe width, with uniform ambient in unprobed interiors.
    vec3 up = fog.worldUp.xyz * fog.hemisphere.w;
    float skyDiffuse = clamp(dot(normal, up), -1.0, 1.0);
    vec3 ambientAlbedo = baseColor * data.tint.rgb * sunDiffuseColor * (1.0 - metalness);
    vec3 indirect = ambientAlbedo * (1.0 + fog.hemisphere.rgb * skyDiffuse);
    bool hasSpecular = metalness > 0.0 || any(greaterThan(dielectricWeight, vec3(0.0)));
    PbrSurface surface = PbrSurface(normal, vec3(0.0), albedo * (1.0 - metalness),
        vec3(0.0), vec3(0.0), 0.0, 0.0, 0.0, 0.0, hasSpecular);
    if (hasSpecular)
    {
        if (roughness < 0.0)
            roughness = roughnessFromShininess(shininess);
        // Broaden the lobe as averaged RGB normals shorten in coarse mips.
        float alpha = clamp(roughness * roughness + normalVariance, 0.18 * 0.18, 1.0);
        surface.eye = pbrUnit(-viewPosition);
        surface.reflectance = mix(0.04 * dielectricWeight, albedo, metalness);
        surface.grazingReflectance = mix(dielectricWeight, vec3(1.0), metalness);
        surface.roughness = alpha;
        surface.alphaSquared = alpha * alpha;
        surface.ndotv = clamp(dot(normal, surface.eye), 0.0, 1.0);
        surface.smithView = mix(surface.ndotv, 1.0, alpha);
        float skySpecular = clamp(dot(reflect(-surface.eye, normal), up), -1.0, 1.0) * (1.0 - alpha);
        PbrEnvironmentBrdf dfg = pbrEnvironmentBrdf(sqrt(alpha), surface.ndotv);
        vec3 indirectF = clamp(surface.reflectance * dfg.scale + surface.grazingReflectance * dfg.bias, 0.0, 1.0);
        indirect = indirect * (1.0 - indirectF) + indirectF * (1.0 + fog.hemisphere.rgb * skySpecular);
    }
    vec3 result = occlusion * data.sunAmbient.rgb * indirect + sunSpecularColor * data.tint.rgb;

    vec3 direction = pbrUnit(data.sunPosition.xyz);
    if (any(notEqual(data.sunDiffuse.rgb, vec3(0))) || any(notEqual(data.sunSpecular.rgb, vec3(0))))
    {
        float visibility = receivesShadows && shadows.flags.x > 0u ? sunVisibility() : 1.0;
        result += visibility * pbrDirect(surface, direction, data.sunDiffuse.rgb, data.sunSpecular.rgb);
    }
    bool classic = (flags & 2u) != 0u;
    for (uint i = 0u; i < data.lighting.y; ++i)
    {
        uint index = data.lighting.x + i;
        vec4 falloff = lights[index].attenuation;
        vec3 delta = lights[index].position.xyz - viewPosition;
        float distanceSquared = dot(delta, delta);
        if (!classic && (falloff.w <= 0.0 || distanceSquared >= falloff.w * falloff.w))
            continue;
        float distance = sqrt(distanceSquared);
        float attenuation = 1.0 / max(dot(falloff.xyz, vec3(1.0, distance, distanceSquared)), 1e-6);
        if (!classic)
        {
            float fade = clamp(4.0 * distance / falloff.w - 3.0, 0.0, 1.0);
            fade = 1.0 - fade * fade;
            attenuation *= fade * fade;
        }
        result += attenuation * (occlusion * ambientAlbedo * lights[index].ambient.rgb
            + pbrDirect(surface, delta / max(distance, 1e-6), lights[index].diffuse.rgb, lights[index].specular.rgb));
    }
    if ((flags & 256u) != 0u)
    {
        vec3 glow = samplePbr(pbrEmissive, primary, secondary, alternate & 8u).rgb;
        result += (flags & 8192u) != 0u ? glow : decodeSrgb(glow);
    }
    result = max(result, vec3(0.0));
    return (flags & 4096u) != 0u ? encodeSrgb(result) : result;
}
