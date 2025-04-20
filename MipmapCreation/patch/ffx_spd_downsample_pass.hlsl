// SPD pass
// SRV  0 : SPD_InputDownsampleSrc          : r_input_downsample_src
// UAV  0 : SPD_InternalGlobalAtomic        : rw_internal_global_atomic
// UAV  1 : SPD_InputDownsampleSrcMidMip    : rw_input_downsample_src_mid_mip
// UAV  2 : SPD_InputDownsampleSrcMips      : rw_input_downsample_src_mips
// CB   0 : cbSPD
#define FFX_SPD_BIND_SRV_INPUT_DOWNSAMPLE_SRC               0

#define FFX_SPD_BIND_UAV_INTERNAL_GLOBAL_ATOMIC             0
#define FFX_SPD_BIND_UAV_INPUT_DOWNSAMPLE_SRC_MID_MIPMAP    1
#define FFX_SPD_BIND_UAV_INPUT_DOWNSAMPLE_SRC_MIPS          2

#define FFX_SPD_BIND_CB_SPD                                 0

#define FFX_GPU 1
#define FFX_HLSL 1
#include "ffx_core.h"   // fidelityfx/ffx_core.h


#pragma dxc diagnostic push
#pragma dxc diagnostic ignored "-Wambig-lit-shift"

#include "ffx_spd_resources.h"
#pragma dxc diagnostic pop
#pragma warning(disable: 3205)  // conversion from larger type to smaller


cbuffer cbSPD : register(b0)
{
    FfxUInt32       mips;
    FfxUInt32       numWorkGroups;
    FfxUInt32x2     workGroupOffset;
    FfxFloat32x2    invInputSize;
    FfxFloat32x2    padding;
};
SamplerState s_LinearClamp : register(s0);
Texture2DArray<FfxFloat32x4> r_input_downsample_src: register(t0);
// SpdGlobalAtomicBuffer
globallycoherent
RWStructuredBuffer<FfxUInt32>  rw_internal_global_atomic : register(u0);
globallycoherent
RWTexture2DArray<FfxFloat32x4> rw_input_downsample_src_mid_mip : register(u1);
RWTexture2DArray<FfxFloat32x4> rw_input_downsample_src_mips[SPD_MAX_MIP_LEVELS+1] : register(u2);

FfxUInt32 Mips() { return mips; }
FfxUInt32 NumWorkGroups() { return numWorkGroups; }
FfxUInt32x2  WorkGroupOffset() { return workGroupOffset; }
FfxFloat32x2 InvInputSize() { return invInputSize; }

// No Half
FfxFloat32x4 SampleSrcImage(FfxInt32x2 uv, FfxUInt32 slice)
{
    FfxFloat32x2 textureCoord = FfxFloat32x2(uv) * InvInputSize() + InvInputSize();
    FfxFloat32x4 result = r_input_downsample_src.SampleLevel(s_LinearClamp, FfxFloat32x3(textureCoord, slice), 0);
    return FfxFloat32x4(ffxSrgbFromLinear(result.x), ffxSrgbFromLinear(result.y), ffxSrgbFromLinear(result.z), result.w);
}
FfxFloat32x4 LoadSrcImage(FfxInt32x2 uv, FfxUInt32 slice)
{
    return rw_input_downsample_src_mips[0][FfxUInt32x3(uv, slice)];
}
void StoreSrcMip(FfxFloat32x4 value, FfxInt32x2 uv, FfxUInt32 slice, FfxUInt32 mip)
{
    rw_input_downsample_src_mips[mip][FfxUInt32x3(uv, slice)] = value;
}
FfxFloat32x4 LoadMidMip(FfxInt32x2 uv, FfxUInt32 slice)
{ 
    return rw_input_downsample_src_mid_mip[FfxUInt32x3(uv, slice)];
}
void StoreMidMip(FfxFloat32x4 value, FfxInt32x2 uv, FfxUInt32 slice)
{
    rw_input_downsample_src_mid_mip[FfxUInt32x3(uv, slice)] = value;
}

// Atomic
void IncreaseAtomicCounter(FFX_PARAMETER_IN FfxUInt32 slice, FFX_PARAMETER_INOUT FfxUInt32 counter)
{
    //InterlockedAdd(rw_internal_global_atomic[0].counter[slice], 1, counter);
    InterlockedAdd(rw_internal_global_atomic[slice], 1, counter);
}
void ResetAtomicCounter(FFX_PARAMETER_IN FfxUInt32 slice)
{
    //rw_internal_global_atomic[0].counter[slice] = 0;
    rw_internal_global_atomic[slice] = 0;
}


#include "spd/ffx_spd_downsample.h"

#define SPD_ROOTSIG \
    "DescriptorTable(UAV(u0, numDescriptors=17)), " \
    "DescriptorTable(SRV(t0, numDescriptors=14)), " \
    "CBV(b0), " \
    "StaticSampler(s0, filter = FILTER_MIN_MAG_LINEAR_MIP_POINT, " \
    "addressU = TEXTURE_ADDRESS_CLAMP, " \
    "addressV = TEXTURE_ADDRESS_CLAMP, " \
    "addressW = TEXTURE_ADDRESS_CLAMP, " \
    "comparisonFunc = COMPARISON_NEVER, " \
    "borderColor = STATIC_BORDER_COLOR_TRANSPARENT_BLACK)"

[RootSignature(SPD_ROOTSIG)]
[numthreads(256, 1, 1)]
void mainCS(uint LocalThreadIndex : SV_GroupIndex, uint3 WorkGroupId : SV_GroupID)
{
    DOWNSAMPLE(LocalThreadIndex, WorkGroupId);
}
