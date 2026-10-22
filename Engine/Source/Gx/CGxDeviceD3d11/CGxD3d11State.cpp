#include <cmath>
#include <cstdint>
#include <cstring>
#include "Engine/Source/Gx/CGxDeviceD3d11/CGxDeviceD3d11.h"
#include "Engine/Source/Gx/Buffer.h"
#include "Engine/Source/Gx/CGxBuf.h"
#include "Engine/Source/Gx/CGxPool.h"
#include "Engine/Source/Gx/CGxShaderPermute.h"
#include "Engine/Source/Gx/Gx.h"
#include "Engine/Source/Storm/H/storm.h"
#include "Engine/Source/Tempest/C4Vector.h"
const uint32_t CGxDeviceD3d11::s_gxAttribToSemanticIndex[GxVAs_Last] = { 0, 0, 0, 0, 0, 1, 0, 1, 2, 3, 4, 5, 6, 7 };

const DXGI_FORMAT CGxDeviceD3d11::s_gxAttribToDxgiFormat[8] = { DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UINT, DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R32G32_FLOAT, DXGI_FORMAT_R32G32B32_FLOAT, DXGI_FORMAT_R16G16_SINT, DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R32G32B32A32_FLOAT };

const char* CGxDeviceD3d11::s_gxAttribToSemanticName[GxVAs_Last] = { "POSITION", "BLENDWEIGHT", "BLENDINDICES", "NORMAL", "COLOR", "COLOR", "TEXCOORD", "TEXCOORD", "TEXCOORD", "TEXCOORD", "TEXCOORD", "TEXCOORD", "TEXCOORD", "TEXCOORD" };

const D3D11_BLEND CGxDeviceD3d11::s_srcBlend[13] = { D3D11_BLEND_ONE, D3D11_BLEND_ONE, D3D11_BLEND_SRC_ALPHA, D3D11_BLEND_SRC_ALPHA, D3D11_BLEND_DEST_COLOR, D3D11_BLEND_DEST_COLOR, D3D11_BLEND_DEST_COLOR, D3D11_BLEND_INV_SRC_ALPHA, D3D11_BLEND_INV_SRC_ALPHA, D3D11_BLEND_SRC_ALPHA, D3D11_BLEND_ONE, D3D11_BLEND_BLEND_FACTOR, D3D11_BLEND_INV_DEST_COLOR };

const D3D11_BLEND CGxDeviceD3d11::s_srcBlendAlpha[13] = { D3D11_BLEND_ONE, D3D11_BLEND_ONE, D3D11_BLEND_SRC_ALPHA, D3D11_BLEND_SRC_ALPHA, D3D11_BLEND_DEST_ALPHA, D3D11_BLEND_DEST_ALPHA, D3D11_BLEND_DEST_ALPHA, D3D11_BLEND_INV_SRC_ALPHA, D3D11_BLEND_INV_SRC_ALPHA, D3D11_BLEND_SRC_ALPHA, D3D11_BLEND_ONE, D3D11_BLEND_BLEND_FACTOR, D3D11_BLEND_ONE };

const D3D11_BLEND CGxDeviceD3d11::s_dstBlend[13] = { D3D11_BLEND_ZERO, D3D11_BLEND_ZERO, D3D11_BLEND_INV_SRC_ALPHA, D3D11_BLEND_ONE, D3D11_BLEND_ZERO, D3D11_BLEND_SRC_COLOR, D3D11_BLEND_ONE, D3D11_BLEND_ONE, D3D11_BLEND_ZERO, D3D11_BLEND_ZERO, D3D11_BLEND_ONE, D3D11_BLEND_INV_BLEND_FACTOR, D3D11_BLEND_ONE };

const D3D11_BLEND CGxDeviceD3d11::s_dstBlendAlpha[13] = { D3D11_BLEND_ZERO, D3D11_BLEND_ZERO, D3D11_BLEND_INV_SRC_ALPHA, D3D11_BLEND_ONE, D3D11_BLEND_ZERO, D3D11_BLEND_SRC_ALPHA, D3D11_BLEND_ONE, D3D11_BLEND_ONE, D3D11_BLEND_ZERO, D3D11_BLEND_ZERO, D3D11_BLEND_ONE, D3D11_BLEND_INV_BLEND_FACTOR, D3D11_BLEND_ZERO };

const D3D11_COMPARISON_FUNC CGxDeviceD3d11::s_cmpFunc[4] = { D3D11_COMPARISON_LESS_EQUAL, D3D11_COMPARISON_EQUAL, D3D11_COMPARISON_GREATER_EQUAL, D3D11_COMPARISON_LESS };

const D3D11_FILTER CGxDeviceD3d11::s_filterModes[GxTexFilters_Last] = { D3D11_FILTER_MIN_MAG_MIP_POINT, D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT, D3D11_FILTER_MIN_MAG_MIP_POINT, D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT, D3D11_FILTER_MIN_MAG_MIP_LINEAR, D3D11_FILTER_ANISOTROPIC };

const D3D11_FILTER CGxDeviceD3d11::s_compareFilterModes[GxTexFilters_Last] = { D3D11_FILTER_COMPARISON_MIN_MAG_MIP_POINT, D3D11_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT, D3D11_FILTER_COMPARISON_MIN_MAG_MIP_POINT, D3D11_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT, D3D11_FILTER_COMPARISON_MIN_MAG_MIP_LINEAR, D3D11_FILTER_COMPARISON_ANISOTROPIC };

const D3D11_TEXTURE_ADDRESS_MODE CGxDeviceD3d11::s_wrapModes[3] = { D3D11_TEXTURE_ADDRESS_CLAMP, D3D11_TEXTURE_ADDRESS_WRAP, D3D11_TEXTURE_ADDRESS_MIRROR };

const D3D11_PRIMITIVE_TOPOLOGY CGxDeviceD3d11::s_primitiveTopology[5] = { D3D11_PRIMITIVE_TOPOLOGY_POINTLIST, D3D11_PRIMITIVE_TOPOLOGY_LINELIST, D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP };

const D3D11_PRIMITIVE_TOPOLOGY CGxDeviceD3d11::s_patchTopology[5] = { D3D11_PRIMITIVE_TOPOLOGY_1_CONTROL_POINT_PATCHLIST, D3D11_PRIMITIVE_TOPOLOGY_2_CONTROL_POINT_PATCHLIST, D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED, D3D11_PRIMITIVE_TOPOLOGY_3_CONTROL_POINT_PATCHLIST, D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED };

// INLINED
inline bool CGxDeviceD3d11::InputLayoutKey::operator==(const InputLayoutKey& key) {
    return this->m_format == key.m_format && this->m_mask == key.m_mask;
}

// OFFSET: 0x0086D510/0x1405A1E40
void CGxDeviceD3d11::IStateReset() {
    this->m_curInputLayout = NULL;
    this->m_curIndexBuffer = NULL;
    this->m_curVertexBuffers[0] = NULL;
    this->m_vertexStrides[0] = 0;
    this->m_curVertexBuffers[1] = NULL;
    this->m_vertexStrides[1] = 0;
    this->m_curRasterizerState = NULL;
    this->m_curBlendState = NULL;
    this->m_curDepthState = NULL;
    this->m_instanceCount = 1;
    this->m_stateDirty = (this->m_stateDirty & 0xFFFFFF00) | 0x3F;
}

// OFFSET: 0x0086D570/0x1405A1EA0
void CGxDeviceD3d11::ISetIndexBuffer() {
    ID3D11Buffer* indexBuffer = static_cast<ID3D11Buffer*>(this->m_primIndexBuf->m_pool->m_apiSpecific);

    if (this->m_curIndexBuffer != indexBuffer) {
        this->m_d3dContext->IASetIndexBuffer(indexBuffer, DXGI_FORMAT_R16_UINT, 0);
        this->m_curIndexBuffer = indexBuffer;
    }
}

// OFFSET: 0x0086D5B0/0x1405A1F00
void CGxDeviceD3d11::ISetVertexBuffer() {
    ID3D11Buffer* vertexBuffer = static_cast<ID3D11Buffer*>(this->m_primVertexBufs[0]->m_pool->m_apiSpecific);
    UINT offset = 0;

    if (this->m_curVertexBuffers[0] != vertexBuffer) {
        this->m_curVertexBuffers[0] = vertexBuffer;
        this->m_d3dContext->IASetVertexBuffers(0, 1, &this->m_curVertexBuffers[0], &this->m_vertexStrides[0], &offset);
    }
}

// OFFSET: 0x0086D600/0x1405A1F60
void CGxDeviceD3d11::ISetVertexBuffers() {
    ID3D11Buffer* vertexBuffer0 = static_cast<ID3D11Buffer*>(this->m_primVertexBufs[0]->m_pool->m_apiSpecific);
    ID3D11Buffer* vertexBuffer1 = this->m_primVertexBufs[1] ? static_cast<ID3D11Buffer*>(this->m_primVertexBufs[1]->m_pool->m_apiSpecific) : NULL;
    UINT offsets[2] = { 0, 0 };

    if (this->m_curVertexBuffers[0] != vertexBuffer0 || this->m_curVertexBuffers[1] != vertexBuffer1) {
        this->m_curVertexBuffers[0] = vertexBuffer0;
        this->m_curVertexBuffers[1] = vertexBuffer1;
        this->m_d3dContext->IASetVertexBuffers(0, 2, this->m_curVertexBuffers, this->m_vertexStrides, offsets);
    }
}

// OFFSET: 0x0086D680/0x1405A1FF0
void CGxDeviceD3d11::IStateSyncScissorRect() {
    if (!this->m_scissorRectDirty) {
        return;
    }

    const CRect& window = this->DeviceCurWindow();
    D3D11_RECT rect;

    rect.left = static_cast<LONG>(this->m_scissorRect.minX * window.maxX + 0.5f);
    rect.right = static_cast<LONG>(this->m_scissorRect.maxX * window.maxX + 1.0f);

    if (this->m_textureTarget[GxBuffers_Color].m_apiSpecific || this->m_textureTarget[GxBuffers_Depth].m_apiSpecific) {
        rect.top = static_cast<LONG>(this->m_scissorRect.minY * window.maxY + 0.5f);
        rect.bottom = static_cast<LONG>(this->m_scissorRect.maxY * window.maxY + 1.0f);
    } else {
        rect.top = static_cast<LONG>((1.0f - this->m_scissorRect.maxY) * window.maxY + 0.5f);
        rect.bottom = static_cast<LONG>((1.0f - this->m_scissorRect.minY) * window.maxY + 1.0f);
    }

    if (rect.left < 0) {
        rect.left = 0;
    }

    if (rect.top < 0) {
        rect.top = 0;
    }

    LONG width = static_cast<LONG>(window.maxX);

    if (rect.right > width) {
        rect.right = width;
    }

    LONG height = static_cast<LONG>(window.maxY);

    if (rect.bottom > height) {
        rect.bottom = height;
    }

    this->m_d3dContext->RSSetScissorRects(1, &rect);
    this->m_scissorRectDirty = 0;
}

// OFFSET: 0x0086F480/0x1405A4C40
void CGxDeviceD3d11::IUpdateInputLayout() {
    if (!(this->m_stateDirty & StateDirty_InputLayout)) {
        return;
    }

    this->m_stateDirty &= ~StateDirty_InputLayout;

    uint32_t format = this->m_primVertexFormat;
    CGxShaderPermute* permute = static_cast<CGxShaderPermute*>(this->m_appRenderStates[GxRs_VertexShader].m_value.m_data.p);

    // The vertex shader can legitimately be unbound (IBindVertexShader accepts null), in which case there is
    // no input signature to match the layout against.
    if (!permute) {
        return;
    }

    InputLayoutKey key;
    key.m_format = format;
    key.m_mask = permute->unk_000C;

    uint32_t hashval = format ^ ((format >> 2) + key.m_mask + (format << 6) + 0x9E3779B9);
    InputLayoutRec* rec = this->m_inputLayouts.Ptr(hashval, key);

    if (rec) {
        if (this->m_curInputLayout != rec->m_layout) {
            this->m_stateDirty |= StateDirty_InputLayoutApply;
            this->m_curInputLayout = rec->m_layout;
        }

        return;
    }

    const VertexBufDesc& desc = Buffer::s_vertexBufDesc[format];
    D3D11_INPUT_ELEMENT_DESC elements[GxVAs_Last];
    uint32_t count = 0;

    for (uint32_t i = 0; i < desc.attribCount; i++) {
        const CGxVertexAttrib& attrib = desc.attribs[i];

        if (!((1 << CGxDevice::IVertexAttribToShAttrib(attrib.m_attrib)) & permute->unk_000C)) {
            continue;
        }

        D3D11_INPUT_ELEMENT_DESC& element = elements[count++];
        element.SemanticName = s_gxAttribToSemanticName[attrib.m_attrib];
        element.SemanticIndex = s_gxAttribToSemanticIndex[attrib.m_attrib];
        element.Format = s_gxAttribToDxgiFormat[attrib.m_type];
        element.AlignedByteOffset = attrib.m_offset;

        UINT instanced = attrib.m_stream != 0;
        element.InstanceDataStepRate = instanced;
        element.InputSlotClass = static_cast<D3D11_INPUT_CLASSIFICATION>(instanced);
        element.InputSlot = instanced;
    }

    uint8_t* data = permute->Load();

    if (!data) {
        return;
    }

    ID3D11InputLayout* layout;
    uint32_t signatureSize = *reinterpret_cast<uint32_t*>(data + 28);

    if (signatureSize && signatureSize <= permute->unk_0010 &&
        SUCCEEDED(this->m_d3dDevice->CreateInputLayout(elements, count, data + 32, signatureSize, &layout))) {
        rec = this->m_inputLayouts.New(hashval, key, 0, 0);
        rec->m_layout = layout;
        this->m_stateDirty |= StateDirty_InputLayoutApply;
        this->m_curInputLayout = layout;
    }

    SMemFree(data, __FILE__, __LINE__, 0);
}

// OFFSET: 0x0086F650/0x1405A4F20
void CGxDeviceD3d11::ISetInputLayout() {
    this->IUpdateInputLayout();

    if (!(this->m_stateDirty & StateDirty_InputLayoutApply)) {
        return;
    }

    this->m_stateDirty &= ~StateDirty_InputLayoutApply;
    this->m_d3dContext->IASetInputLayout(this->m_curInputLayout);

    uint32_t stride = Buffer::s_vertexBufDesc[this->m_primVertexFormats[0]].size;

    if (this->m_vertexStrides[0] != stride) {
        this->m_curVertexBuffers[0] = NULL;
        this->m_vertexStrides[0] = stride;
    }
}

// OFFSET: 0x0086F6B0/0x1405A4F90
void CGxDeviceD3d11::ISetInputLayoutInstanced() {
    this->IUpdateInputLayout();

    if (!(this->m_stateDirty & StateDirty_InputLayoutApply)) {
        return;
    }

    this->m_stateDirty &= ~StateDirty_InputLayoutApply;
    this->m_d3dContext->IASetInputLayout(this->m_curInputLayout);

    uint32_t stride = Buffer::s_vertexBufDesc[this->m_primVertexFormats[0]].size;

    if (this->m_vertexStrides[0] != stride) {
        this->m_curVertexBuffers[0] = NULL;
        this->m_vertexStrides[0] = stride;
    }

    if (this->m_primVertexFormats[1] < GxVertexBufferFormats_Last) {
        stride = Buffer::s_vertexBufDesc[this->m_primVertexFormats[1]].size;

        if (this->m_vertexStrides[1] != stride) {
            this->m_curVertexBuffers[1] = NULL;
            this->m_vertexStrides[1] = stride;
        }
    }
}

// OFFSET: 0x0086F740/0x1405A5030
void CGxDeviceD3d11::IStateSyncRasterizer() {
    if (this->m_stateDirty & StateDirty_Rasterizer) {
        this->m_stateDirty &= ~StateDirty_Rasterizer;

        int32_t multisample = this->m_appRenderStates[GxRs_Multisample].m_value.m_data.i[0];
        int32_t scissor = this->m_appRenderStates[GxRs_ScissorTest].m_value.m_data.i[0];
        int32_t fill = this->MasterEnable(GxMasterEnable_PolygonFill);
        int32_t cull = this->m_appRenderStates[GxRs_Culling].m_value.m_data.i[0];
        int32_t polygonOffset = this->m_appRenderStates[GxRs_PolygonOffset].m_value.m_data.i[0];
        uint32_t hashval = multisample | (2 * (scissor | (2 * (fill | (2 * (cull | (4 * polygonOffset)))))));

        RasterizerStateRec* rec = this->m_rasterizerStates.Ptr(hashval, HASHKEY_NONE());

        if (rec) {
            if (this->m_curRasterizerState == rec->m_state) {
                goto apply;
            }

            this->m_curRasterizerState = rec->m_state;
        } else {
            D3D11_CULL_MODE cullModes[3] = { D3D11_CULL_NONE, D3D11_CULL_BACK, D3D11_CULL_FRONT };

            D3D11_RASTERIZER_DESC desc;
            desc.FillMode = static_cast<D3D11_FILL_MODE>(fill + 2);
            desc.CullMode = cullModes[cull];
            desc.FrontCounterClockwise = TRUE;
            desc.DepthBias = static_cast<INT>(CGxDevice::s_polygonOffsetValues[polygonOffset] * -16777216.0f);
            desc.DepthBiasClamp = 0.0f;
            desc.SlopeScaledDepthBias = 0.0f;
            desc.DepthClipEnable = TRUE;
            desc.ScissorEnable = scissor;
            desc.MultisampleEnable = multisample;
            desc.AntialiasedLineEnable = FALSE;

            ID3D11RasterizerState* state;

            if (FAILED(this->m_d3dDevice->CreateRasterizerState(&desc, &state))) {
                goto apply;
            }

            rec = this->m_rasterizerStates.New(hashval, HASHKEY_NONE(), 0, 0);
            rec->m_state = state;
            this->m_curRasterizerState = state;
        }

        this->m_stateDirty |= StateDirty_RasterizerApply;
    }

apply:
    if (this->m_stateDirty & StateDirty_RasterizerApply) {
        this->m_stateDirty &= ~StateDirty_RasterizerApply;
        this->m_d3dContext->RSSetState(this->m_curRasterizerState);
    }
}

// OFFSET: 0x0086F8C0/0x1405A5240
void CGxDeviceD3d11::IStateSyncBlend() {
    if (this->m_stateDirty & StateDirty_Blend) {
        this->m_stateDirty &= ~StateDirty_Blend;

        int32_t mode = this->m_appRenderStates[GxRs_BlendingMode].m_value.m_data.i[0];
        uint32_t colorWrite = this->m_appRenderStates[GxRs_ColorWrite].m_value.m_data.u[0];
        uint32_t hashval = colorWrite | (16 * mode);

        BlendStateRec* rec = this->m_blendStates.Ptr(hashval, HASHKEY_NONE());

        if (rec) {
            if (this->m_curBlendState != rec->m_state) {
                this->m_curBlendState = rec->m_state;
                this->m_stateDirty |= StateDirty_BlendApply;
            }
        } else {
            D3D11_BLEND_DESC desc;
            desc.AlphaToCoverageEnable = FALSE;
            desc.IndependentBlendEnable = FALSE;
            desc.RenderTarget[0].BlendEnable = mode != 0 && mode != 1;
            desc.RenderTarget[0].SrcBlend = s_srcBlend[mode];
            desc.RenderTarget[0].DestBlend = s_dstBlend[mode];
            desc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
            desc.RenderTarget[0].SrcBlendAlpha = s_srcBlendAlpha[mode];
            desc.RenderTarget[0].DestBlendAlpha = s_dstBlendAlpha[mode];
            desc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
            desc.RenderTarget[0].RenderTargetWriteMask = static_cast<UINT8>(colorWrite);

            for (uint32_t i = 1; i < 8; i++) {
                desc.RenderTarget[i].BlendEnable = FALSE;
                desc.RenderTarget[i].SrcBlend = D3D11_BLEND_ONE;
                desc.RenderTarget[i].DestBlend = D3D11_BLEND_ZERO;
                desc.RenderTarget[i].BlendOp = D3D11_BLEND_OP_ADD;
                desc.RenderTarget[i].SrcBlendAlpha = D3D11_BLEND_ONE;
                desc.RenderTarget[i].DestBlendAlpha = D3D11_BLEND_ZERO;
                desc.RenderTarget[i].BlendOpAlpha = D3D11_BLEND_OP_ADD;
                desc.RenderTarget[i].RenderTargetWriteMask = 0;
            }

            ID3D11BlendState* state;

            if (SUCCEEDED(this->m_d3dDevice->CreateBlendState(&desc, &state))) {
                rec = this->m_blendStates.New(hashval, HASHKEY_NONE(), 0, 0);
                rec->m_state = state;
                this->m_curBlendState = state;
                this->m_stateDirty |= StateDirty_BlendApply;
            }
        }
    }

    if (this->m_stateDirty & StateDirty_BlendApply) {
        this->m_stateDirty &= ~StateDirty_BlendApply;

        float factor = this->m_appRenderStates[GxRs_BlendFactor].m_value.m_data.f[0];
        float factors[4] = { factor, factor, factor, factor };
        this->m_d3dContext->OMSetBlendState(this->m_curBlendState, factors, 0xFFFFFFFF);
    }
}

// OFFSET: 0x0086FA90/0x1405A5490
void CGxDeviceD3d11::IStateSyncDepth() {
    if (this->m_stateDirty & StateDirty_Depth) {
        this->m_stateDirty &= ~StateDirty_Depth;

        int32_t write = this->m_appRenderStates[GxRs_DepthWrite].m_value.m_data.i[0];
        int32_t test = this->m_appRenderStates[GxRs_DepthTest].m_value.m_data.i[0];
        int32_t func = this->m_appRenderStates[GxRs_DepthFunc].m_value.m_data.i[0];
        uint32_t hashval = func | (4 * (write | (2 * test)));

        DepthStateRec* rec = this->m_depthStates.Ptr(hashval, HASHKEY_NONE());

        if (rec) {
            if (this->m_curDepthState != rec->m_state) {
                this->m_stateDirty |= StateDirty_DepthApply;
                this->m_curDepthState = rec->m_state;
            }
        } else {
            D3D11_DEPTH_STENCIL_DESC desc;
            desc.DepthEnable = (test | write) != 0;
            desc.DepthWriteMask = static_cast<D3D11_DEPTH_WRITE_MASK>(write);
            desc.DepthFunc = test ? s_cmpFunc[func] : D3D11_COMPARISON_ALWAYS;
            desc.StencilEnable = FALSE;
            desc.StencilReadMask = 0xFF;
            desc.StencilWriteMask = 0xFF;
            desc.FrontFace.StencilFailOp = D3D11_STENCIL_OP_KEEP;
            desc.FrontFace.StencilDepthFailOp = D3D11_STENCIL_OP_KEEP;
            desc.FrontFace.StencilPassOp = D3D11_STENCIL_OP_KEEP;
            desc.FrontFace.StencilFunc = D3D11_COMPARISON_ALWAYS;
            desc.BackFace.StencilFailOp = D3D11_STENCIL_OP_KEEP;
            desc.BackFace.StencilDepthFailOp = D3D11_STENCIL_OP_KEEP;
            desc.BackFace.StencilPassOp = D3D11_STENCIL_OP_KEEP;
            desc.BackFace.StencilFunc = D3D11_COMPARISON_ALWAYS;

            ID3D11DepthStencilState* state;

            if (SUCCEEDED(this->m_d3dDevice->CreateDepthStencilState(&desc, &state))) {
                rec = this->m_depthStates.New(hashval, HASHKEY_NONE(), 0, 0);
                rec->m_state = state;
                this->m_stateDirty |= StateDirty_DepthApply;
                this->m_curDepthState = state;
            }
        }
    }

    if (this->m_stateDirty & StateDirty_DepthApply) {
        this->m_stateDirty &= ~StateDirty_DepthApply;
        this->m_d3dContext->OMSetDepthStencilState(this->m_curDepthState, 0);
    }
}

// OFFSET: 0x0086FBF0/0x1405A5680
ID3D11SamplerState* CGxDeviceD3d11::IGetSamplerState(CGxTexFlags flags) {
    uint32_t bits = *reinterpret_cast<uint32_t*>(&flags);
    uint32_t hashval = (bits & 0xCF) | (16 * (bits & 0x30)) | (((bits & 0x1F000) | ((bits >> 2) & 0xC0)) >> 2);

    SamplerStateRec* rec = this->m_samplerStates.Ptr(hashval, HASHKEY_NONE());

    if (rec) {
        return rec->m_state;
    }

    uint32_t filter = bits & 7;
    uint32_t compare = (bits >> 3) & 1;

    D3D11_SAMPLER_DESC desc;
    desc.Filter = compare ? s_compareFilterModes[filter] : s_filterModes[filter];
    desc.AddressU = s_wrapModes[(bits >> 4) & 3];
    desc.AddressV = s_wrapModes[(bits & 0xFF) >> 6];
    desc.AddressW = s_wrapModes[(bits >> 8) & 3];
    desc.MipLODBias = 0.0f;
    desc.MaxAnisotropy = (bits >> 12) & 0x1F;
    desc.ComparisonFunc = static_cast<D3D11_COMPARISON_FUNC>(compare + 1);
    desc.BorderColor[0] = 0.0f;
    desc.BorderColor[1] = 0.0f;
    desc.BorderColor[2] = 0.0f;
    desc.BorderColor[3] = 0.0f;
    desc.MinLOD = 0.0f;
    desc.MaxLOD = D3D11_FLOAT32_MAX;

    ID3D11SamplerState* state;

    if (FAILED(this->m_d3dDevice->CreateSamplerState(&desc, &state))) {
        return NULL;
    }

    rec = this->m_samplerStates.New(hashval, HASHKEY_NONE(), 0, 0);
    rec->m_state = state;

    return state;
}

// OFFSET: 0x0086FD30/0x1405A5830
void CGxDeviceD3d11::ISetVertexTexture(uint32_t slot, CGxTex* texId) {
    if (texId) {
        this->IResourceTouch(texId);
        this->ITexMarkAsUpdated(texId);

        if (!texId->m_needsCreation && (texId->m_apiSpecificData || texId->m_apiSpecificData2)) {
            ID3D11ShaderResourceView* srv = static_cast<TexViews*>(texId->m_apiSpecificData)->m_srv;
            ID3D11SamplerState* sampler = this->IGetSamplerState(texId->m_flags);
            this->m_d3dContext->VSSetShaderResources(slot, 1, &srv);
            this->m_d3dContext->VSSetSamplers(slot, 1, &sampler);
            return;
        }
    }

    ID3D11ShaderResourceView* srv = NULL;
    ID3D11SamplerState* sampler = NULL;
    this->m_d3dContext->VSSetShaderResources(slot, 1, &srv);
    this->m_d3dContext->VSSetSamplers(slot, 1, &sampler);
}

// OFFSET: 0x0086FDF0/0x1405A5910
void CGxDeviceD3d11::ISetDomainTexture(uint32_t slot, CGxTex* texId) {
    if (texId) {
        this->IResourceTouch(texId);
        this->ITexMarkAsUpdated(texId);

        if (!texId->m_needsCreation && (texId->m_apiSpecificData || texId->m_apiSpecificData2)) {
            ID3D11ShaderResourceView* srv = static_cast<TexViews*>(texId->m_apiSpecificData)->m_srv;
            ID3D11SamplerState* sampler = this->IGetSamplerState(texId->m_flags);
            this->m_d3dContext->DSSetShaderResources(slot, 1, &srv);
            this->m_d3dContext->DSSetSamplers(slot, 1, &sampler);
        }
    }

    ID3D11ShaderResourceView* srv = NULL;
    ID3D11SamplerState* sampler = NULL;
    this->m_d3dContext->DSSetShaderResources(slot, 1, &srv);
    this->m_d3dContext->DSSetSamplers(slot, 1, &sampler);
}

// OFFSET: 0x0086FEB0/0x1405A59F0
void CGxDeviceD3d11::ISetTexture(uint32_t slot, CGxTex* texId) {
    if (texId) {
        this->IResourceTouch(texId);
        this->ITexMarkAsUpdated(texId);

        if (!texId->m_needsCreation && (texId->m_apiSpecificData || texId->m_apiSpecificData2)) {
            ID3D11ShaderResourceView* srv = static_cast<TexViews*>(texId->m_apiSpecificData)->m_srv;
            ID3D11SamplerState* sampler = this->IGetSamplerState(texId->m_flags);
            this->m_d3dContext->PSSetShaderResources(slot, 1, &srv);
            this->m_d3dContext->PSSetSamplers(slot, 1, &sampler);
            return;
        }
    }

    ID3D11ShaderResourceView* srv = NULL;
    ID3D11SamplerState* sampler = NULL;
    this->m_d3dContext->PSSetShaderResources(slot, 1, &srv);
    this->m_d3dContext->PSSetSamplers(slot, 1, &sampler);
}

// OFFSET: 0x0086FF70/0x1405A5AC0
void CGxDeviceD3d11::IRsSendToHw(EGxRenderState which) {
    CGxStateBom& value = this->m_appRenderStates[which].m_value;

    switch (which) {
        case GxRs_BlendingMode:
        case GxRs_ColorWrite:
            this->m_stateDirty |= StateDirty_Blend;
            break;

        case GxRs_DepthTest:
        case GxRs_DepthFunc:
        case GxRs_DepthWrite:
            this->m_stateDirty |= StateDirty_Depth;
            break;

        case GxRs_PolygonOffset:
        case GxRs_Culling:
        case GxRs_Multisample:
        case GxRs_ScissorTest:
            this->m_stateDirty |= StateDirty_Rasterizer;
            break;

        case GxRs_Texture0:
        case GxRs_Texture1:
        case GxRs_Texture2:
        case GxRs_Texture3:
        case GxRs_Texture4:
        case GxRs_Texture5:
        case GxRs_Texture6:
        case GxRs_Texture7:
        case GxRs_Texture8:
        case GxRs_Texture9:
        case GxRs_Texture10:
        case GxRs_Texture11:
        case GxRs_Texture12:
        case GxRs_Texture13:
        case GxRs_Texture14:
        case GxRs_Texture15:
            this->ISetTexture(which - GxRs_Texture0, static_cast<CGxTex*>(value.m_data.p));
            break;

        case GxRs_VertexTexture0:
        case GxRs_VertexTexture1:
        case GxRs_VertexTexture2:
        case GxRs_VertexTexture3:
            this->ISetVertexTexture(which - GxRs_VertexTexture0, static_cast<CGxTex*>(value.m_data.p));
            break;

        case GxRs_Unk41:
            this->ISetDomainTexture(0, static_cast<CGxTex*>(value.m_data.p));
            break;

        case GxRs_VertexShader:
            this->m_stateDirty |= StateDirty_InputLayout;
            this->IBindVertexShader(static_cast<CGxShaderPermute*>(value.m_data.p));
            break;

        case GxRs_HullShader:
            this->IBindHullShader(static_cast<CGxShaderPermute*>(value.m_data.p));
            break;

        case GxRs_DomainShader:
            this->IBindDomainShader(static_cast<CGxShaderPermute*>(value.m_data.p));
            break;

        case GxRs_GeometryShader:
            this->IBindGeometryShader(static_cast<CGxShaderPermute*>(value.m_data.p));
            break;

        case GxRs_PixelShader:
            this->IBindPixelShader(static_cast<CGxShaderPermute*>(value.m_data.p));
            break;

        case GxRs_BlendFactor:
            this->m_stateDirty |= StateDirty_BlendApply;
            break;

        default:
            break;
    }
}

// OFFSET: 0x008700E0/0x1405A5C10
void CGxDeviceD3d11::IStateSync() {
    this->IRsSync(0);
    this->IShaderConstantsFlush();

    if (this->m_viewportDirty) {
        this->IXformSetViewport();
    }

    if (this->m_appMasterEnables != this->m_hwMasterEnables) {
        int32_t enable;

        if (this->NeedsUpdate(this->m_appMasterEnables, this->m_hwMasterEnables, 0, 0, GxMasterEnable_PolygonFill, enable)) {
            this->m_stateDirty |= StateDirty_Rasterizer;
        }

        this->m_hwMasterEnables = this->m_appMasterEnables;
    }

    this->IStateSyncScissorRect();
    this->IStateSyncRasterizer();
    this->IStateSyncBlend();
    this->IStateSyncDepth();
}

// OFFSET: 0x00870160/0x14058C3D0
void CGxDeviceD3d11::CapsWindowSize(CRect& rect) {
    rect = this->DeviceCurWindow();
}

// OFFSET: 0x00870190
void CGxDeviceD3d11::CapsWindowSizeInScreenCoords(CRect& rect) {
    if (this->IDevIsWindowed()) {
        const CRect& window = this->DeviceCurWindow();
        POINT points[2];
        points[0].x = 0;
        points[0].y = 0;
        points[1].x = static_cast<LONG>(window.maxX);
        points[1].y = static_cast<LONG>(window.maxY);
        MapWindowPoints(this->m_hwnd, NULL, points, 2);
        rect.minY = static_cast<float>(points[0].y);
        rect.minX = static_cast<float>(points[0].x);
        rect.maxY = static_cast<float>(points[1].y);
        rect.maxX = static_cast<float>(points[1].x);
    } else {
        rect = this->DeviceCurWindow();
    }
}

// OFFSET: 0x00870250/0x1405A5DA0
void CGxDeviceD3d11::SceneClear(uint32_t mask, CImVector color) {
    CGxDevice::SceneClear(mask, color);

    if (!this->m_context) {
        return;
    }

    if (mask & 0xF) {
        C4Vector clearColor(color);

        for (uint32_t i = 0; i < this->m_targets.m_numRenderTargets; i++) {
            if (this->m_targets.m_renderTargets[i] && ((1 << i) & mask)) {
                this->m_d3dContext->ClearRenderTargetView(this->m_targets.m_renderTargets[i], reinterpret_cast<const float*>(&clearColor));
            }
        }
    }

    if ((mask & 0x10) && this->m_targets.m_curDepthView) {
        this->m_d3dContext->ClearDepthStencilView(this->m_targets.m_curDepthView, D3D11_CLEAR_DEPTH, 1.0f, 0);
    }
}

// OFFSET: 0x00870310/0x1405A5E80
void CGxDeviceD3d11::ScenePresent() {
    if (!this->m_context) {
        return;
    }

    int32_t scrShotClick = this->m_scrShotClick;

    CGxDevice::ScenePresent();
    this->ICursorDraw();

    if (scrShotClick) {
        this->DeviceScreenShot();
    }

    this->WaitForFPSCap();

    HRESULT result = this->m_swapChain->Present(this->m_format.m_vsync, 0);

    if (FAILED(result)) {
        GxLog("Present Error: 0x%08X\n", result);

        if (result == DXGI_ERROR_DEVICE_REMOVED || result == DXGI_ERROR_DEVICE_RESET) {
            CGxFormat format = this->m_format;
            GxWindowProc windowProc = this->m_windowProc;
            HWND hwnd = this->m_hwnd;

            this->DeviceDestroy();

            if (this->m_ownhwnd) {
                this->DeviceCreate(windowProc, format);
            } else {
                this->DeviceCreate(hwnd, format);
            }

            this->NotifyOnDeviceRestored();
        }
    }
}

// OFFSET: 0x00870400/0x1405A5F90
void CGxDeviceD3d11::SceneClearToViewport(uint32_t mask, CImVector color) {
    CGxDevice::SceneClearToViewport(mask, color);

    if (!this->m_context) {
        return;
    }

    if (this->m_viewportDirty) {
        this->IXformSetViewport();
    }

    uint32_t permute = 0;

    for (uint32_t i = 0; i < this->m_targets.m_numRenderTargets; i++) {
        if (this->m_targets.m_renderTargets[i] && ((1 << i) & mask)) {
            permute = i;
        }
    }

    g_theGxDevicePtr->RsPush();

    int32_t polygonFill = g_theGxDevicePtr->MasterEnable(GxMasterEnable_PolygonFill);
    g_theGxDevicePtr->MasterEnableSet(GxMasterEnable_PolygonFill, 1);
    GxRsSet(GxRs_ColorWrite, (mask & 0xF) ? 0xF : 0);
    GxRsSet(GxRs_DepthWrite, static_cast<int32_t>((mask >> 4) & 1));
    GxRsSet(GxRs_DepthTest, 0);
    GxRsSet(GxRs_BlendingMode, 0);
    GxRsSet(GxRs_Lighting, 0);
    GxRsSet(GxRs_Culling, 0);
    GxRsSet(GxRs_ScissorTest, 0);
    g_theGxDevicePtr->RsSet(GxRs_VertexShader, &this->m_clearVertexShader->m_permutes[0]);
    g_theGxDevicePtr->RsSet(GxRs_PixelShader, &this->m_clearPixelShader->m_permutes[permute]);

    float constants[4] = { 0.0f, 0.0f, 0.0f, 0.0f };

    switch (this->m_targets.m_colorTargetFormat) {
        case GxTex_R16:
        case GxTex_Gr1616F:
        case GxTex_R24X8:
        case GxTex_R32F:
            constants[0] = 1.0f;
            constants[1] = 1.0f;
            constants[2] = 1.0f;
            constants[3] = 0.0f;
            break;

        case GxTex_Abgr16161616F:
        case GxTex_Abgr32323232F:
            constants[0] = 1.0f;
            constants[1] = 1.0f;
            constants[2] = 1.0f;
            constants[3] = 1.0f;
            break;

        default:
            constants[0] = color.r * (1.0f / 255.0f);
            constants[1] = color.g * (1.0f / 255.0f);
            constants[2] = color.b * (1.0f / 255.0f);
            constants[3] = color.a * (1.0f / 255.0f);
            break;
    }

    g_theGxDevicePtr->ShaderConstantsSet(GxSh_Pixel, 0, constants, 1);

    float vertices[12] = {
        -1.0f, 1.0f, 1.0f,
        1.0f, 1.0f, 1.0f,
        -1.0f, -1.0f, 1.0f,
        1.0f, -1.0f, 1.0f
    };

    uint16_t indices[4] = { 0, 2, 1, 3 };

    CGxBuf* vertexBuf = g_theGxDevicePtr->BufStream(GxPoolTarget_Vertex, 12, 4);
    memcpy(GxBufLock(vertexBuf), vertices, sizeof(vertices));
    GxBufUnlock(vertexBuf, 0);

    CGxBuf* indexBuf = g_theGxDevicePtr->BufStream(GxPoolTarget_Index, 2, 4);
    memcpy(GxBufLock(indexBuf), indices, sizeof(indices));
    GxBufUnlock(indexBuf, 0);

    g_theGxDevicePtr->PrimVertexPtr(vertexBuf, GxVBF_P);
    g_theGxDevicePtr->PrimIndexPtr(indexBuf);

    CGxBatch batch;
    batch.m_primType = GxPrim_TriangleStrip;
    batch.m_start = 0;
    batch.m_count = 4;
    batch.m_minIndex = 0;
    batch.m_maxIndex = 3;
    g_theGxDevicePtr->Draw(&batch, 1);
    g_theGxDevicePtr->MasterEnableSet(GxMasterEnable_PolygonFill, polygonFill);
    g_theGxDevicePtr->RsPop();
}

// OFFSET: 0x00870890
int32_t CGxDeviceD3d11::XformIsValidViewport(float minX, float maxX, float minY, float maxY, float minZ, float maxZ) {
    const CRect& window = this->DeviceCurWindow();
    float x = static_cast<float>(static_cast<int32_t>(window.maxX * minX + 0.5f));
    float y = static_cast<float>(static_cast<int32_t>((1.0f - maxY) * window.maxY + 0.5f));
    float width = static_cast<float>(static_cast<uint32_t>(static_cast<int64_t>(window.maxX * maxX - x + 0.5)));
    float viewY;

    if (this->m_textureTarget[GxBuffers_Color].m_apiSpecific || this->m_textureTarget[GxBuffers_Depth].m_apiSpecific) {
        viewY = static_cast<float>(static_cast<int32_t>(window.maxY * minY));
    } else {
        viewY = y;
    }

    if (width <= 0.0f) {
        return 0;
    }

    float height = static_cast<float>(static_cast<uint32_t>(static_cast<int64_t>((1.0f - minY) * window.maxY - y + 0.5)));

    return height > 0.0f && minZ >= 0.0f && maxZ <= 1.0f && minZ <= maxZ && x >= 0.0f && viewY >= 0.0f && width + x <= window.maxX && height + viewY <= window.maxY;
}

// OFFSET: 0x00870A00/0x1405A6670
void CGxDeviceD3d11::IXformSetViewport() {
    const CRect& window = this->DeviceCurWindow();
    D3D11_VIEWPORT viewport;

    viewport.TopLeftX = static_cast<float>(static_cast<int32_t>(this->m_viewport.m_x.m_l * window.maxX + 0.5f));
    viewport.TopLeftY = static_cast<float>(static_cast<int32_t>((1.0f - this->m_viewport.m_y.m_h) * window.maxY + 0.5f));
    viewport.Width = static_cast<float>(static_cast<uint32_t>(this->m_viewport.m_x.m_h * window.maxX - viewport.TopLeftX + 0.5f));
    viewport.Height = static_cast<float>(static_cast<uint32_t>((1.0f - this->m_viewport.m_y.m_l) * window.maxY - viewport.TopLeftY + 0.5f));
    viewport.MinDepth = this->m_viewport.m_z.m_l;
    viewport.MaxDepth = this->m_viewport.m_z.m_h;

    if (this->m_frameBuffer != g_theGxDevicePtr->BackBuffer()) {
        viewport.TopLeftY = static_cast<float>(static_cast<int32_t>(window.maxY * this->m_viewport.m_y.m_l));
    }

    this->m_d3dContext->RSSetViewports(1, &viewport);
    this->m_viewportDirty = 0;
}

// OFFSET: 0x00870B50
void CGxDeviceD3d11::XformSetProjection(const C44Matrix& matrix) {
    CGxDevice::XformSetProjection(matrix);

    C44Matrix projNative = matrix;

    if (!(fabs(projNative.c3 - 1.0f) < 0.00000023841858f) && !(fabs(projNative.c3) < 0.00000023841858f)) {
        projNative *= 1.0f / projNative.c3;
    }

    if (projNative.d3 == 0.0f) {
        float minZ = -(projNative.d2 / (projNative.c2 + 1.0f));
        float maxZ = -(projNative.d2 / (projNative.c2 - 1.0f));
        projNative.c2 = maxZ / (maxZ - minZ);
        projNative.d2 = maxZ * minZ / (minZ - maxZ);
    } else {
        float invC2 = 1.0f / projNative.c2;
        float minZ = (-1.0f - projNative.d2) * invC2;
        float maxZ = (1.0f - projNative.d2) * invC2;
        projNative.c2 = 1.0f / (maxZ - minZ);
        projNative.d2 = minZ / (minZ - maxZ);
    }

    if (!(this->m_appMasterEnables & (1 << GxMasterEnable_NormalProjection)) && projNative.d3 != 1.0f) {
        C44Matrix scale;
        scale.a0 = 0.2f;
        scale.b1 = 0.2f;
        scale.c2 = 0.2f;
        projNative = projNative * scale;
    }

    this->m_xforms[GxXform_Projection].m_dirty = 1;
    this->m_projNative = projNative;
}

// OFFSET: 0x00870E50/0x14058CC40
void CGxDeviceD3d11::XformSetView(const C44Matrix& matrix) {
    CGxDevice::XformSetView(matrix);
}

// OFFSET: 0x00870E60
void CGxDeviceD3d11::SetInstanceCount(uint32_t count) {
    this->m_instanceCount = count;
}

// OFFSET: 0x00870E70/0x1405A6C20
void CGxDeviceD3d11::ResetInstancing() {
    if (this->m_curVertexBuffers[1]) {
        this->m_curVertexBuffers[1] = NULL;
        this->m_vertexStrides[1] = 0;

        UINT offset = 0;
        this->m_d3dContext->IASetVertexBuffers(1, 1, &this->m_curVertexBuffers[1], &this->m_vertexStrides[1], &offset);
    }

    this->m_instanceCount = 1;
    this->m_primVertexFormats[1] = GxVertexBufferFormats_Last;
    this->m_primVertexBufs[1] = NULL;
}

// OFFSET: 0x00870EE0
void CGxDeviceD3d11::PrimVertexPtr(CGxBuf* buf, EGxVertexBufferFormat format) {
    if (this->m_primVertexFormat != format) {
        this->m_stateDirty |= StateDirty_InputLayout;
    }

    CGxDevice::PrimVertexPtr(buf, format);
}

// OFFSET: 0x00870F10
void CGxDeviceD3d11::PrimVertexPtrs(CGxBuf** bufs, const EGxVertexBufferFormat* formats, EGxVertexBufferFormat format) {
    if (this->m_primVertexFormat != format) {
        this->m_stateDirty |= StateDirty_InputLayout;
    }

    CGxDevice::PrimVertexPtrs(bufs, formats, format);
}

// OFFSET: 0x00870F40/0x1405A6CE0
void CGxDeviceD3d11::DrawNonIndexed(const CGxBatch& batch) {
    if (!this->m_context || this->m_intF5C) {
        return;
    }

    // No vertex shader bound means every draw below is invalid input. D3D11 does not reject it, it
    // hangs the device (DEVICE_DRAW_VERTEX_SHADER_NOT_SET), so drop the draw instead.
    CGxShaderPermute* vsPermute = static_cast<CGxShaderPermute*>(this->m_appRenderStates[GxRs_VertexShader].m_value.m_data.p);

    if (!vsPermute || !vsPermute->Valid()) {
        return;
    }


    this->IStateSync();
    this->ISetInputLayout();
    this->ISetVertexBuffer();

    if (this->m_appRenderStates[GxRs_HullShader].m_value.m_data.p && this->m_appRenderStates[GxRs_DomainShader].m_value.m_data.p) {
        this->m_d3dContext->IASetPrimitiveTopology(s_patchTopology[batch.m_primType]);
    } else {
        this->m_d3dContext->IASetPrimitiveTopology(s_primitiveTopology[batch.m_primType]);
    }

    this->m_d3dContext->Draw(batch.m_count, this->m_primVertexBufs[0]->m_index + batch.m_start);
}

// OFFSET: 0x00870FE0/0x1405A6DA0
void CGxDeviceD3d11::Draw(const CGxBatch& batch) {
    if (!this->m_context || this->m_intF5C) {
        return;
    }

    // No vertex shader bound means every draw below is invalid input. D3D11 does not reject it, it
    // hangs the device (DEVICE_DRAW_VERTEX_SHADER_NOT_SET), so drop the draw instead.
    CGxShaderPermute* vsPermute = static_cast<CGxShaderPermute*>(this->m_appRenderStates[GxRs_VertexShader].m_value.m_data.p);

    if (!vsPermute || !vsPermute->Valid()) {
        return;
    }


    this->IStateSync();
    this->ISetInputLayout();
    this->ISetIndexBuffer();
    this->ISetVertexBuffer();

    if (this->m_appRenderStates[GxRs_HullShader].m_value.m_data.p && this->m_appRenderStates[GxRs_DomainShader].m_value.m_data.p) {
        this->m_d3dContext->IASetPrimitiveTopology(s_patchTopology[batch.m_primType]);
    } else {
        this->m_d3dContext->IASetPrimitiveTopology(s_primitiveTopology[batch.m_primType]);
    }

    // m_index is a byte offset (see IBufLock); D3D11 wants item offsets. Guard itemSize 0: the stream
    // buffers are created with itemSize 0 and their offset is always 0 too.
    CGxBuf* indexBuf = this->m_primIndexBuf;
    CGxBuf* vertexBuf = this->m_primVertexBufs[0];
    UINT indexOffset = indexBuf->m_itemSize ? indexBuf->m_index / indexBuf->m_itemSize : 0;
    UINT vertexOffset = vertexBuf->m_itemSize ? vertexBuf->m_index / vertexBuf->m_itemSize : 0;
    this->m_d3dContext->DrawIndexed(batch.m_count, batch.m_start + indexOffset, vertexOffset);
}

// OFFSET: 0x00871090/0x1405A6E70
void CGxDeviceD3d11::DrawInstancedNonIndexed(const CGxBatch& batch) {
    if (!this->m_context || this->m_intF5C) {
        return;
    }

    // No vertex shader bound means every draw below is invalid input. D3D11 does not reject it, it
    // hangs the device (DEVICE_DRAW_VERTEX_SHADER_NOT_SET), so drop the draw instead.
    CGxShaderPermute* vsPermute = static_cast<CGxShaderPermute*>(this->m_appRenderStates[GxRs_VertexShader].m_value.m_data.p);

    if (!vsPermute || !vsPermute->Valid()) {
        return;
    }


    this->IStateSync();
    this->ISetInputLayoutInstanced();
    this->ISetVertexBuffers();

    if (this->m_appRenderStates[GxRs_HullShader].m_value.m_data.p && this->m_appRenderStates[GxRs_DomainShader].m_value.m_data.p) {
        this->m_d3dContext->IASetPrimitiveTopology(s_patchTopology[batch.m_primType]);
    } else {
        this->m_d3dContext->IASetPrimitiveTopology(s_primitiveTopology[batch.m_primType]);
    }

    this->m_d3dContext->DrawInstanced(batch.m_count, this->m_instanceCount, this->m_primVertexBufs[0]->m_index, this->m_primVertexBufs[1] ? this->m_primVertexBufs[1]->m_index : 0);
}

// OFFSET: 0x00871150/0x1405A6F50
void CGxDeviceD3d11::DrawInstanced(const CGxBatch& batch) {
    if (!this->m_context || this->m_intF5C) {
        return;
    }

    // No vertex shader bound means every draw below is invalid input. D3D11 does not reject it, it
    // hangs the device (DEVICE_DRAW_VERTEX_SHADER_NOT_SET), so drop the draw instead.
    CGxShaderPermute* vsPermute = static_cast<CGxShaderPermute*>(this->m_appRenderStates[GxRs_VertexShader].m_value.m_data.p);

    if (!vsPermute || !vsPermute->Valid()) {
        return;
    }


    this->IStateSync();
    this->ISetInputLayoutInstanced();
    this->ISetIndexBuffer();
    this->ISetVertexBuffers();

    if (this->m_appRenderStates[GxRs_HullShader].m_value.m_data.p && this->m_appRenderStates[GxRs_DomainShader].m_value.m_data.p) {
        this->m_d3dContext->IASetPrimitiveTopology(s_patchTopology[batch.m_primType]);
    } else {
        this->m_d3dContext->IASetPrimitiveTopology(s_primitiveTopology[batch.m_primType]);
    }

    CGxBuf* indexBuf = this->m_primIndexBuf;
    CGxBuf* vertexBuf = this->m_primVertexBufs[0];
    CGxBuf* vertexBuf1 = this->m_primVertexBufs[1];
    UINT indexOffset = indexBuf->m_itemSize ? indexBuf->m_index / indexBuf->m_itemSize : 0;
    UINT vertexOffset = vertexBuf->m_itemSize ? vertexBuf->m_index / vertexBuf->m_itemSize : 0;
    UINT vertexOffset1 = (vertexBuf1 && vertexBuf1->m_itemSize) ? vertexBuf1->m_index / vertexBuf1->m_itemSize : 0;
    this->m_d3dContext->DrawIndexedInstanced(batch.m_count, this->m_instanceCount, batch.m_start + indexOffset, vertexOffset, vertexOffset1);
}

// NOT IN BINARY: this client's only draw entry point (GxDraw calls Draw(batch, count)); the D3d11
// backend implements the per-batch overrides, so dispatch through them.
void CGxDeviceD3d11::Draw(CGxBatch* batch, int32_t count) {
    for (int32_t i = 0; i < count; i++) {
        this->Draw(batch[i]);
    }
}

// NOT IN BINARY: cata declares these on every backend but never implements them; the engine does not
// call them. Present so the class is concrete.
void CGxDeviceD3d11::DeviceUnk81(uint32_t a, uint32_t b) {
}

void CGxDeviceD3d11::DeviceUnk82() {
}

void CGxDeviceD3d11::DeviceUnk83(uint32_t a, uint32_t b) {
}
