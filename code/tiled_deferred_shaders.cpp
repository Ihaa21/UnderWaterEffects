#version 450

#extension GL_ARB_separate_shader_objects : enable
#extension GL_GOOGLE_include_directive : enable

#include "descriptor_layouts.cpp"
#include "toon_blinn_phong_lighting.cpp"

//
// NOTE: Math
//

struct plane
{
    vec3 Normal;
    float Distance;
};

struct frustum
{
    // NOTE: Left, Right, Top, Bottom
    plane Planes[4];
};

plane PlaneCreate(vec3 P0, vec3 P1, vec3 P2)
{
    plane Result;

    vec3 V0 = P1 - P0;
    vec3 V1 = P2 - P0;
    Result.Normal = normalize(cross(V0, V1));
    Result.Distance = dot(Result.Normal, P0);
    
    return Result;
}

bool SphereInsidePlane(vec3 SphereCenter, float SphereRadius, plane Plane)
{
    bool Result = dot(Plane.Normal, SphereCenter) - Plane.Distance < -SphereRadius;
    return Result;
}

bool SphereInsideFrustum(vec3 SphereCenter, float SphereRadius, frustum Frustum, float NearZ, float FarZ)
{
    bool Result = true;

    if (SphereCenter.z + SphereRadius < NearZ || SphereCenter.z - SphereRadius > FarZ)
    {
        Result = false;
    }

    for (int PlaneId = 0; PlaneId < 4; ++PlaneId)
    {
        if (SphereInsidePlane(SphereCenter, SphereRadius, Frustum.Planes[PlaneId]))
        {
            Result = false;
        }
    }
    
    return Result;
}

vec4 ClipToView(mat4 InverseProjection, vec4 ClipPos)
{
    vec4 Result = InverseProjection * ClipPos;
    Result = Result / Result.w;
    return Result;
}

vec4 ScreenToView(mat4 InverseProjection, vec2 ScreenSize, vec4 ScreenPos)
{
    vec2 Ndc = 2.0f * (ScreenPos.xy / ScreenSize) - vec2(1.0f);
    vec4 Result = ClipToView(InverseProjection, vec4(Ndc, ScreenPos.zw));
    return Result;
}

//
// NOTE: Descriptor Sets
//

#define TILE_DIM_IN_PIXELS 8

layout(set = 0, binding = 0) uniform tiled_deferred_globals
{
    mat4 InverseProjection;
    vec2 ScreenSize;
    uvec2 GridSize;
};

layout(set = 0, binding = 1) buffer grid_frustums
{
    frustum GridFrustums[];
};

// NOTE: Opaque Data
layout(set = 0, binding = 2, rg32ui) uniform uimage2D LightGrid_O;
layout(set = 0, binding = 3) buffer light_index_list_opaque
{
    uint LightIndexList_O[];
};
layout(set = 0, binding = 4) buffer light_index_counter_opaque
{
    uint LightIndexCounter_O;
};

// NOTE: Transparent Data
layout(set = 0, binding = 5, rg32ui) uniform uimage2D LightGrid_T;
layout(set = 0, binding = 6) buffer light_index_list_transparent
{
    uint LightIndexList_T[];
};
layout(set = 0, binding = 7) buffer light_index_counter_transparent
{
    uint LightIndexCounter_T;
};

// NOTE: GBuffer Data
layout(set = 0, binding = 8) uniform sampler2D GBufferPositionTexture;
layout(set = 0, binding = 9) uniform sampler2D GBufferNormalTexture;
layout(set = 0, binding = 10) uniform usampler2D GBufferMaterialTexture;
layout(set = 0, binding = 11) uniform sampler2D GBufferDepthTexture;

SCENE_DESCRIPTOR_LAYOUT(1)
MATERIAL_DESCRIPTOR_LAYOUT(2)

layout(set = 3, binding = 0) uniform sampler2D PerlinNoise;
layout(set = 3, binding = 1) uniform sampler2D Distortion;
layout(set = 3, binding = 2) uniform water_inputs
{
    float Time;
} WaterInputs;

//
// NOTE: Grid Frustum Shader
//

#if GRID_FRUSTUM

layout(local_size_x = TILE_DIM_IN_PIXELS, local_size_y = TILE_DIM_IN_PIXELS, local_size_z = 1) in;

void main()
{
    uvec2 GridPos = uvec2(gl_GlobalInvocationID.xy);
    if (GridPos.x < GridSize.x && GridPos.y < GridSize.y)
    {
        // NOTE: Compute four corner points of tile
        vec3 CameraPos = vec3(0);
        vec4 BotLeft = vec4((GridPos + vec2(0, 0)) * vec2(TILE_DIM_IN_PIXELS), 0, 1);
        vec4 BotRight = vec4((GridPos + vec2(1, 0)) * vec2(TILE_DIM_IN_PIXELS), 0, 1);
        vec4 TopLeft = vec4((GridPos + vec2(0, 1)) * vec2(TILE_DIM_IN_PIXELS), 0, 1);
        vec4 TopRight = vec4((GridPos + vec2(1, 1)) * vec2(TILE_DIM_IN_PIXELS), 0, 1);
     
        // NOTE: Transform corner points to far plane in view space (we assume a counter clock wise winding order)
        BotLeft = ScreenToView(InverseProjection, ScreenSize, BotLeft);
        BotRight = ScreenToView(InverseProjection, ScreenSize, BotRight);
        TopLeft = ScreenToView(InverseProjection, ScreenSize, TopLeft);
        TopRight = ScreenToView(InverseProjection, ScreenSize, TopRight);
   
        // NOTE: Build the frustum planes and store
        frustum Frustum;
        Frustum.Planes[0] = PlaneCreate(CameraPos, BotLeft.xyz, TopLeft.xyz);
        Frustum.Planes[1] = PlaneCreate(CameraPos, TopRight.xyz, BotRight.xyz);
        Frustum.Planes[2] = PlaneCreate(CameraPos, TopLeft.xyz, TopRight.xyz);
        Frustum.Planes[3] = PlaneCreate(CameraPos, BotRight.xyz, BotLeft.xyz);
        
        // NOTE: Write out to buffer
        uint WriteIndex = GridPos.y * GridSize.x + GridPos.x;
        GridFrustums[WriteIndex] = Frustum;
    }
}

#endif

//
// NOTE: Light Culling Shader
//

#if LIGHT_CULLING

shared frustum SharedFrustum;
shared uint SharedMinDepth;
shared uint SharedMaxDepth;

// NOTE: Opaque
shared uint SharedGlobalLightId_O;
shared uint SharedCurrLightId_O;
shared uint SharedLightIds_O[1024];

// NOTE: Transparent
shared uint SharedGlobalLightId_T;
shared uint SharedCurrLightId_T;
shared uint SharedLightIds_T[1024];

void LightAppendOpaque(uint LightId)
{
    uint WriteArrayId = atomicAdd(SharedCurrLightId_O, 1);
    if (WriteArrayId < 1024)
    {
        SharedLightIds_O[WriteArrayId] = LightId;
    }
}

void LightAppendTransparent(uint LightId)
{
    uint WriteArrayId = atomicAdd(SharedCurrLightId_T, 1);
    if (WriteArrayId < 1024)
    {
        SharedLightIds_T[WriteArrayId] = LightId;
    }
}

layout(local_size_x = TILE_DIM_IN_PIXELS, local_size_y = TILE_DIM_IN_PIXELS, local_size_z = 1) in;

void main()
{    
    uint NumThreadsPerGroup = TILE_DIM_IN_PIXELS * TILE_DIM_IN_PIXELS;

    // NOTE: Skip threads that go past the screen
    if (!(gl_GlobalInvocationID.x < ScreenSize.x && gl_GlobalInvocationID.y < ScreenSize.y))
    {
        return;
    }
    
    // NOTE: Setup shared variables
    if (gl_LocalInvocationIndex == 0)
    {
        SharedFrustum = GridFrustums[uint(gl_WorkGroupID.y) * GridSize.x + uint(gl_WorkGroupID.x)];
        SharedMinDepth = 0xFFFFFFFF;
        SharedMaxDepth = 0;
        SharedCurrLightId_O = 0;
        SharedCurrLightId_T = 0;
    }

    barrier();
    
    // NOTE: Calculate min/max depth in grid tile (since our depth values are between 0 and 1, we can reinterpret them as ints and
    // comparison will still work correctly)
    ivec2 ReadPixelId = ivec2(gl_GlobalInvocationID.xy);
    uint PixelDepth = floatBitsToInt(texelFetch(GBufferDepthTexture, ReadPixelId, 0).x);
    atomicMin(SharedMinDepth, PixelDepth);
    atomicMax(SharedMaxDepth, PixelDepth);

    barrier();

    // NOTE: Convert depth bounds to frustum planes in view space
    float MinDepth = uintBitsToFloat(SharedMinDepth);
    float MaxDepth = uintBitsToFloat(SharedMaxDepth);

    MinDepth = ClipToView(InverseProjection, vec4(0, 0, MinDepth, 1)).z;
    MaxDepth = ClipToView(InverseProjection, vec4(0, 0, MaxDepth, 1)).z;

    float NearClipDepth = ClipToView(InverseProjection, vec4(0, 0, 1, 1)).z;
    plane MinPlane = { vec3(0, 0, 1), MaxDepth };
    
    // NOTE: Cull lights against tiles frustum (each thread culls one light at a time)
    for (uint LightId = gl_LocalInvocationIndex; LightId < SceneBuffer.NumPointLights; LightId += NumThreadsPerGroup)
    {
        point_light Light = PointLights[LightId];
        if (SphereInsideFrustum(Light.Pos, Light.MaxDistance, SharedFrustum, NearClipDepth, MinDepth))
        {
            LightAppendTransparent(LightId);

            if (!SphereInsidePlane(Light.Pos, Light.MaxDistance, MinPlane))
            {
                LightAppendOpaque(LightId);
            }
        }
    }

    barrier();

    // NOTE: Get space and light index lists
    if (gl_LocalInvocationIndex == 0)
    {
        ivec2 WritePixelId = ivec2(gl_WorkGroupID.xy);

        // NOTE: Without the ifs, we get a lot of false positives, might be quicker to skip the atomic? Idk if this matters a lot
        if (SharedCurrLightId_O != 0)
        {
            SharedGlobalLightId_O = atomicAdd(LightIndexCounter_O, SharedCurrLightId_O);
            imageStore(LightGrid_O, WritePixelId, ivec4(SharedGlobalLightId_O, SharedCurrLightId_O, 0, 0));
        }
        if (SharedCurrLightId_T != 0)
        {
            SharedGlobalLightId_T = atomicAdd(LightIndexCounter_T, SharedCurrLightId_T);
            imageStore(LightGrid_T, WritePixelId, ivec4(SharedGlobalLightId_T, SharedCurrLightId_T, 0, 0));
        }
    }

    barrier();

    // NOTE: Write opaque
    for (uint LightId = gl_LocalInvocationIndex; LightId < SharedCurrLightId_O; LightId += NumThreadsPerGroup)
    {
        LightIndexList_O[SharedGlobalLightId_O + LightId] = SharedLightIds_O[LightId];
    }

    // NOTE: Write transparent
    for (uint LightId = gl_LocalInvocationIndex; LightId < SharedCurrLightId_T; LightId += NumThreadsPerGroup)
    {
        LightIndexList_T[SharedGlobalLightId_T + LightId] = SharedLightIds_T[LightId];
    }
}

#endif

//
// NOTE: GBuffer Vertex
//

#if GBUFFER_VERT

layout(location = 0) in vec3 InPos;
layout(location = 1) in vec3 InNormal;
layout(location = 2) in vec2 InUv;

layout(location = 0) out vec3 OutViewPos;
layout(location = 1) out vec3 OutViewNormal;
layout(location = 2) out vec2 OutUv;
layout(location = 3) out flat uint OutInstanceId;

void main()
{
    instance_entry Entry = InstanceBuffer[gl_InstanceIndex];
    
    gl_Position = Entry.WVPTransform * vec4(InPos, 1);
    OutViewPos = (Entry.WVTransform * vec4(InPos, 1)).xyz;
    OutViewNormal = (Entry.WVTransform * vec4(InNormal, 0)).xyz;
    OutUv = InUv;
    OutInstanceId = gl_InstanceIndex;
}

#endif

//
// NOTE: GBuffer Fragment
//

#if GBUFFER_FRAG

layout(location = 0) in vec3 InViewPos;
layout(location = 1) in vec3 InViewNormal;
layout(location = 2) in vec2 InUv;
layout(location = 3) in flat uint InInstanceId;

layout(location = 0) out vec4 OutViewPos;
layout(location = 1) out vec4 OutViewNormal;
layout(location = 2) out uvec2 OutMaterial;

void main()
{
    OutViewPos = vec4(InViewPos, 0);
    OutViewNormal = vec4(normalize(InViewNormal), 0);
    OutMaterial = uvec2(InInstanceId, 0);
}

#endif

//
// NOTE: GBuffer Snow Vertex
//

// NOTE: Snow inputs
vec3 SnowColor = vec3(0.75, 0.75, 1);
float SnowHeight = 0.05;
float SnowAmount = 0.25;

#if GBUFFER_SNOW_VERT

layout(location = 0) in vec3 InPos;
layout(location = 1) in vec3 InNormal;
layout(location = 2) in vec2 InUv;

layout(location = 0) out vec3 OutViewPos;
layout(location = 1) out vec3 OutViewNormal;
layout(location = 2) out vec2 OutUv;
layout(location = 3) out flat uint OutInstanceId;

void main()
{
    instance_entry Entry = InstanceBuffer[gl_InstanceIndex];

    vec3 ViewNormal = (Entry.WVTransform * vec4(InNormal, 0)).xyz;
    float SnowAngle = -dot(SnowBuffer.SnowFallDir, ViewNormal);

    vec3 SnowPos = InPos;
    if (SnowAngle > SnowAmount)
    {
        SnowPos += InNormal * SnowAngle * SnowHeight;
    }
    
    gl_Position = Entry.WVPTransform * vec4(SnowPos, 1);
    OutViewPos = (Entry.WVTransform * vec4(SnowPos, 1)).xyz;
    OutViewNormal = ViewNormal;
    OutUv = InUv;
    OutInstanceId = gl_InstanceIndex;
}

#endif

//
// NOTE: GBuffer Snow Fragment
//

#if GBUFFER_SNOW_FRAG

layout(location = 0) in vec3 InViewPos;
layout(location = 1) in vec3 InViewNormal;
layout(location = 2) in vec2 InUv;
layout(location = 3) in flat uint InInstanceId;

layout(location = 0) out vec4 OutViewPos;
layout(location = 1) out vec4 OutViewNormal;
layout(location = 2) out uvec2 OutMaterial;

void main()
{
    OutViewPos = vec4(InViewPos, 0);
    OutViewNormal = vec4(normalize(InViewNormal), 0);
    OutMaterial = uvec2(InInstanceId, 1);
}

#endif

//
// NOTE: Directional Light Vert
//

#if TILED_DEFERRED_LIGHTING_VERT

layout(location = 0) in vec3 InPos;

void main()
{
    gl_Position = vec4(2.0*InPos, 1);
}

#endif

//
// NOTE: Tiled Deferred Lighting
//

#if TILED_DEFERRED_LIGHTING_FRAG

layout(location = 0) out vec4 OutColor;

void main()
{
    vec3 CameraPos = vec3(0, 0, 0);
    ivec2 PixelPos = ivec2(gl_FragCoord.xy);

    uvec2 MaterialId = texelFetch(GBufferMaterialTexture, PixelPos, 0).xy;
    if (MaterialId.x == 0xFFFFFFFF)
    {
        return;
    }
    
    instance_entry Entry = InstanceBuffer[MaterialId.x];
    
    vec3 SurfacePos = texelFetch(GBufferPositionTexture, PixelPos, 0).xyz;
    vec3 SurfaceNormal = texelFetch(GBufferNormalTexture, PixelPos, 0).xyz;
    vec3 SurfaceColor = Entry.Color.rgb;
    vec3 View = normalize(CameraPos - SurfacePos);

    if (MaterialId.y == 1)
    {
        // NOTE: We have snow on this object
        float SnowAngle = -dot(SnowBuffer.SnowFallDir, SurfaceNormal);
        if (SnowAngle > SnowAmount)
        {
            SurfaceColor = SnowColor;
            Entry.SpecularPower = 32;
            Entry.RimBound = 0.05;
            Entry.RimThreshold = 0.01;
            
            // TODO: Modify lighting properties since we have snow
        }
    }
    
    vec3 Color = vec3(0);

#if 0
    // NOTE: Calculate lighting for point lights
    ivec2 GridPos = PixelPos / ivec2(TILE_DIM_IN_PIXELS);
    uvec2 LightIndexMetaData = imageLoad(LightGrid_O, GridPos).xy; // NOTE: Stores the pointer + # of elements
    for (int i = 0; i < LightIndexMetaData.y; ++i)
    {
        uint LightId = LightIndexList_O[LightIndexMetaData.x + i];
        point_light CurrLight = PointLights[LightId];
        vec3 LightDir = normalize(SurfacePos - CurrLight.Pos);
        Color += ToonBlinnPhongLighting(View, SurfaceColor, SurfaceNormal, 32, LightDir, PointLightAttenuate(SurfacePos, CurrLight));
    }
#endif
    
    // NOTE: Calculate lighting for directional lights
    {
        Color += ToonBlinnPhongLighting(View, SurfaceColor, SurfaceNormal, Entry.SpecularPower, Entry.RimBound, Entry.RimThreshold,
                                        DirectionalLight.Dir, DirectionalLight.Color);
        Color += DirectionalLight.AmbientLight * SurfaceColor;
    }

    OutColor = vec4(Color, 1);
}

#endif

//
// NOTE: Water Shaders
//

#if WATER_VERTEX

layout(location = 0) in vec3 InPos;
layout(location = 1) in vec3 InNormal;
layout(location = 2) in vec2 InUv;

layout(location = 0) out vec3 OutViewPos;
layout(location = 1) out vec3 OutViewNormal;
layout(location = 2) out vec2 OutUv;
layout(location = 3) out flat uint OutInstanceId;

void main()
{
    water_entry Entry = WaterBuffer[gl_InstanceIndex];
    
    gl_Position = Entry.WVPTransform * vec4(InPos, 1);
    OutViewPos = (Entry.WVTransform * vec4(InPos, 1)).xyz;
    OutViewNormal = (Entry.WVTransform * vec4(InNormal, 0)).xyz;
    OutUv = InUv;
    OutInstanceId = gl_InstanceIndex;
}

#endif

#if WATER_FRAGMENT

layout(location = 0) in vec3 InViewPos;
layout(location = 1) in vec3 InViewNormal;
layout(location = 2) in vec2 InUv;
layout(location = 3) in flat uint InInstanceId;

layout(location = 0) out vec4 OutColor;

float LinearizeDepth(float Depth, float NearZ, float FarZ)
{
    return NearZ * FarZ / (NearZ + Depth * (FarZ - NearZ));
}

void main()
{
    // NOTE: Shader inputs
    vec4 ShallowColor = vec4(0.325, 0.807, 0.971, 0.725);
    vec4 DeepColor = vec4(0.086, 0.407, 1, 0.9);
    float MaxDepth = 1;
    float WaterNoiseCutOff = 0.777;
    float FoamMinDistance = 0.04;
    float FoamMaxDistance = 0.4;
    vec2 NoiseScrollAmount = vec2(0.003);
    float SurfaceDistortAmount = 0.27;
    float SurfaceAA = 0.005;

    // TODO: Make these inputs
    float NearZ = 0.01f;
    float FarZ = 1000.0f;

    ivec2 PixelCoords = ivec2(gl_FragCoord.xy);
    vec4 WaterColor = vec4(0);
    
    // NOTE: Calculate base water color
    float DepthValue = texelFetch(GBufferDepthTexture, PixelCoords, 0).x;
    DepthValue = LinearizeDepth(DepthValue, NearZ, FarZ);
    float DepthDifference = DepthValue - InViewPos.z;
    WaterColor += mix(ShallowColor, DeepColor, clamp(DepthDifference / MaxDepth, 0, 1));

    // NOTE: Shoreline effect, change the surface noise to be smaller when we are in shallow water
    vec3 BackgroundNormal = texelFetch(GBufferNormalTexture, PixelCoords, 0).xyz;
    float FoamDistance = mix(FoamMaxDistance, FoamMinDistance, clamp(dot(InViewNormal, BackgroundNormal), 0, 1));
    float FoamDepthDifference = clamp(DepthDifference / FoamDistance, 0, 1);
    float SurfaceNoiseCutOff = FoamDepthDifference * WaterNoiseCutOff;

    // NOTE: Distorted water effect
    vec2 Distortion = SurfaceDistortAmount * (2 * texture(Distortion, InUv).xy - 1);

    // NOTE: Calculate waves
    vec2 ModifiedUv = InUv + WaterInputs.Time * NoiseScrollAmount + Distortion; 
    float NoiseSample = texture(PerlinNoise, ModifiedUv).x;
    float SurfaceNoise = smoothstep(SurfaceNoiseCutOff - SurfaceAA, SurfaceNoiseCutOff + SurfaceAA, NoiseSample);
    WaterColor += SurfaceNoise;
    
    OutColor = WaterColor;
}

#endif
