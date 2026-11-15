#version 450

// Surface, water, sky, distortion, overlay. Fixed when creating a pipeline,
// allowing the driver to eliminate unrelated work and varyings.
layout(constant_id = 0) const uint materialShader = 0u;

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec2 inUV;
layout(location = 2) in vec4 inColor;
layout(location = 3) in float inAlpha;
layout(location = 4) in vec2 inMaskUV;
layout(location = 5) in vec3 inNormal;

struct InstanceData
{
    mat4 clipFromModel;
    vec4 tint;
    vec4 parameters;
    vec4 uvRow0;
    vec4 uvRow1;
    vec4 maskRow0;
    vec4 maskRow1;
    mat4 viewFromModel;
    mat4 normalFromModel;
    vec4 diffuse;
    vec4 ambient;
    vec4 emission;
    vec4 specular;
    vec4 sunPosition;
    vec4 sunAmbient;
    vec4 sunDiffuse;
    vec4 sunSpecular;
    uvec4 lighting;
    vec4 clipPlane;
};
struct PointLight
{
    vec4 position;
    vec4 ambient;
    vec4 diffuse;
    vec4 specular;
    vec4 attenuation;
};
layout(std430, set = 2, binding = 0) readonly buffer InstanceBuffer
{
    InstanceData instances[];
};
layout(std430, set = 2, binding = 1) readonly buffer LightBuffer
{
    PointLight lights[];
};

layout(location = 0) out vec2 uv;
layout(location = 1) out vec4 color;
layout(location = 2) out flat vec2 alphaPolicy;
layout(location = 3) out vec2 maskUV;
layout(location = 4) out flat float materialKind;
layout(location = 5) out vec3 specularColor;
layout(location = 6) out flat vec4 skyDiffuse;
layout(location = 7) out flat vec4 skyMoon;
layout(location = 8) out flat vec4 skyAtmosphere;
layout(location = 9) out flat float opacity;
layout(location = 10) out vec4 screenPosition;
layout(location = 11) out vec3 viewPosition;
layout(location = 12) out vec3 viewNormal;
layout(location = 13) out float clipDistance;

vec3 safeNormal(vec3 value)
{
    return value * inversesqrt(max(dot(value, value), 1e-12));
}
float specularTerm(vec3 normal, vec3 direction, vec3 eye, float shininess)
{
    if (dot(normal, direction) <= 0.0 || shininess <= 0.0)
        return 0.0;
    return pow(max(dot(normal, safeNormal(direction + eye)), 0.0), max(shininess, 1e-4));
}

void main()
{
    InstanceData data = instances[gl_InstanceIndex];
    gl_Position = data.clipFromModel * vec4(inPosition, 1.0);
    if (materialShader == 2u)
        gl_Position.z = gl_Position.w;
    else if (materialShader == 1u)
        gl_Position.z = min(gl_Position.z, gl_Position.w);
    uv = vec2(dot(data.uvRow0.xyz, vec3(inUV, 1.0)), dot(data.uvRow1.xyz, vec3(inUV, 1.0)));
    maskUV = vec2(dot(data.maskRow0.xyz, vec3(inMaskUV, 1.0)), dot(data.maskRow1.xyz, vec3(inMaskUV, 1.0)));
    alphaPolicy = data.parameters.xy;
    materialKind = data.parameters.w;
    if (materialShader == 1u)
        uv = (inPosition.xy + data.emission.xy) * (3.0 / (8192.0 * 5.0));

    vec4 vertexColor = vec4(inColor.rgb, inAlpha < 0.0 ? inColor.a : inAlpha);
    uint mode = data.lighting.w;
    vec4 diffuse = (mode == 2u || mode == 4u) ? vertexColor : data.diffuse;
    vec3 ambient = (mode == 2u || mode == 3u) ? vertexColor.rgb : data.ambient.rgb;
    vec3 emission = (mode == 1u ? vertexColor.rgb : data.emission.rgb) * data.emission.a;
    vec3 specular = (mode == 5u ? vertexColor.rgb : data.specular.rgb) * data.ambient.a;
    skyDiffuse = data.diffuse;
    skyMoon = data.emission;
    skyAtmosphere = data.ambient;
    opacity = data.parameters.z;
    color = materialShader == 2u ? vertexColor * data.tint : diffuse * data.tint;
    specularColor = vec3(0.0);
    screenPosition = gl_Position;
    if (materialShader == 4u)
    {
        viewPosition = vec3(0.0);
        viewNormal = vec3(0.0, 0.0, 1.0);
        clipDistance = 1.0;
        return;
    }
    viewPosition = (data.viewFromModel * vec4(inPosition, 1.0)).xyz;
    viewNormal = safeNormal(mat3(data.normalFromModel) * inNormal);
    clipDistance = (data.lighting.z & 4u) != 0u ? dot(vec4(viewPosition, 1.0), data.clipPlane) : 1.0;
    if (materialShader == 1u || materialShader == 2u || (data.lighting.z & 1u) == 0u)
        return;

    vec3 position = viewPosition;
    vec3 normal = viewNormal;
    vec3 eye = safeNormal(-position);
    vec3 direction = safeNormal(data.sunPosition.xyz);
    vec3 lit = ambient * data.sunAmbient.rgb + emission;
    lit += diffuse.rgb * data.sunDiffuse.rgb * max(dot(normal, direction), 0.0);
    vec3 highlights = specular * data.sunSpecular.rgb * specularTerm(normal, direction, eye, data.specular.w);
    for (uint i = 0u; i < data.lighting.y; ++i)
    {
        PointLight light = lights[data.lighting.x + i];
        vec3 delta = light.position.xyz - position;
        float distance = length(delta);
        bool classic = (data.lighting.z & 2u) != 0u;
        if (!classic && distance >= light.attenuation.w)
            continue;
        vec3 toLight = delta / max(distance, 1e-6);
        float attenuation = 1.0 / max(dot(light.attenuation.xyz, vec3(1.0, distance, distance * distance)), 1e-6);
        if (!classic)
        {
            float fade = clamp((distance / max(light.attenuation.w, 1e-6) - 0.75) / 0.25, 0.0, 1.0);
            fade = 1.0 - fade * fade;
            attenuation *= fade * fade;
        }
        lit += attenuation
            * (ambient * light.ambient.rgb + diffuse.rgb * light.diffuse.rgb * max(dot(normal, toLight), 0.0));
        highlights += attenuation * specular * light.specular.rgb * specularTerm(normal, toLight, eye, data.specular.w);
    }
    color.rgb = max(lit, vec3(0.0)) * data.tint.rgb;
    specularColor = max(highlights, vec3(0.0)) * data.tint.rgb;
}
