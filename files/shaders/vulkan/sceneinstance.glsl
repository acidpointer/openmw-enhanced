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
