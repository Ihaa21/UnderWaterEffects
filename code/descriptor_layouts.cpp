
//
// NOTE: Material
//

#define MATERIAL_DESCRIPTOR_LAYOUT(set_number)                          \
    layout(set = set_number, binding = 0) uniform sampler2D ColorTexture; \
    layout(set = set_number, binding = 1) uniform sampler2D NormalTexture; \

//
// NOTE: Scene
//

#include "shader_light_types.cpp"

struct water_entry
{
    mat4 WVTransform;
    mat4 WVPTransform;
};

struct instance_entry
{
    mat4 WVTransform;
    mat4 WVPTransform;
    vec4 Color;
    float SpecularPower;
    float RimBound;
    float RimThreshold;
};

#define SCENE_DESCRIPTOR_LAYOUT(set_number)                             \
    layout(set = set_number, binding = 0) uniform scene_buffer          \
    {                                                                   \
        vec3 CameraPos;                                                 \
        uint NumPointLights;                                            \
    } SceneBuffer;                                                      \
                                                                        \
    layout(set = set_number, binding = 1) buffer instance_buffer        \
    {                                                                   \
        instance_entry InstanceBuffer[];                                \
    };                                                                  \
                                                                        \
    layout(set = set_number, binding = 2) buffer point_light_buffer     \
    {                                                                   \
        point_light PointLights[];                                      \
    };                                                                  \
                                                                        \
    layout(set = set_number, binding = 3) buffer point_light_transforms \
    {                                                                   \
        mat4 PointLightTransforms[];                                    \
    };                                                                  \
                                                                        \
    layout(set = set_number, binding = 4) buffer directional_light_buffer \
    {                                                                   \
        directional_light DirectionalLight;                             \
    };                                                                  \
                                                                        \
    layout(set = set_number, binding = 5) buffer water_buffer           \
    {                                                                   \
        water_entry WaterBuffer[];                                      \
    };                                                                  \
                                                                        \
    layout(set = set_number, binding = 6) uniform snow_buffer           \
    {                                                                   \
        vec3 SnowFallDir;                                               \
    } SnowBuffer;                                                       \
                                                                        \
    
