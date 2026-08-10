#include "d3d9_renderer.h"
#include "d3d9_texture.h"
#include "d3d9_util.h"
#include "shaders/blit.vs.dxbc.h"
#include "shaders/blit.ps.dxbc.h"

#include <cfloat>
#include <utility>

namespace dxup {

  D3D9ImmediateRenderer::D3D9ImmediateRenderer(ID3D11Device1* device, ID3D11DeviceContext1* context, D3D9State* state)
    : m_device{ device }
    , m_context{ context }
    , m_state{ state }
    , m_upVertexBuffer{ device, D3D11_BIND_VERTEX_BUFFER }
    , m_upIndexBuffer{ device, D3D11_BIND_INDEX_BUFFER }
    , m_fanIndexBuffer{ device, D3D11_BIND_INDEX_BUFFER }
    , m_fanIndexed{ false }
    , m_vsConstants{ device, context }
    , m_psConstants{ device, context } {
  
    D3D11_SAMPLER_DESC blitSampler;
    blitSampler.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    blitSampler.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    blitSampler.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    blitSampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    blitSampler.MipLODBias = 0;
    blitSampler.MaxAnisotropy = 1;
    blitSampler.ComparisonFunc = D3D11_COMPARISON_NEVER;
    blitSampler.BorderColor[0] = 1.0f;
    blitSampler.BorderColor[1] = 1.0f;
    blitSampler.BorderColor[2] = 1.0f;
    blitSampler.BorderColor[3] = 1.0f;
    blitSampler.MinLOD = -FLT_MAX;
    blitSampler.MaxLOD = FLT_MAX;
    HRESULT result = m_device->CreateSamplerState(&blitSampler, &m_blitSampler);
    if (FAILED(result))
      log::warn("D3D9ImmediateRenderer: failed to create blit sampler state.");

    result = m_device->CreateVertexShader(g_blit_vs, sizeof(g_blit_vs), nullptr, &m_blitVS);
    if (FAILED(result))
      log::warn("D3D9ImmediateRenderer: failed to create blit vs.");

    result = m_device->CreatePixelShader(g_blit_ps, sizeof(g_blit_ps), nullptr, &m_blitPS);
    if (FAILED(result))
      log::warn("D3D9ImmediateRenderer: failed to create blit ps.");
  }

  void D3D9ImmediateRenderer::endFrame() {
    m_fanIndexBuffer.endFrame();
    m_upIndexBuffer.endFrame();
    m_upVertexBuffer.endFrame();

    m_vsConstants.endFrame();
    m_psConstants.endFrame();
  }

  HRESULT D3D9ImmediateRenderer::Clear(DWORD Count, const D3DRECT* pRects, DWORD Flags, D3DCOLOR Color, float Z, DWORD Stencil) {
    if (Count >= 1) {
      bool fullRectClear = pRects->x1 == 0 &&
                           pRects->x2 == (LONG) m_state->viewport.Width &&
                           pRects->y1 == 0 &&
                           pRects->y2 == (LONG) m_state->viewport.Height;
        
      if (!fullRectClear) {
        log::warn("Clear called with rects. Discarding clear.");
        return D3D_OK;
      }
    }

    FLOAT color[4];
    convert::color(Color, color);

    if (config::getBool(config::RandomClearColour)) {
      for (uint32_t i = 0; i < 4; i++)
        color[i] = ((float)(rand() % 255)) / 255.0f;
    }

    if (Flags & D3DCLEAR_TARGET) {
      for (uint32_t i = 0; i < 4; i++)
      {
        if (m_state->renderTargets[i] == nullptr)
          continue;

        ID3D11RenderTargetView* rtv = m_state->renderTargets[i]->GetD3D11RenderTarget(m_state->renderState[D3DRS_SRGBWRITEENABLE] == TRUE);
        if (rtv)
          m_context->ClearRenderTargetView(rtv, color);
      }
    }

    ID3D11DepthStencilView* dsv = nullptr;
    if (m_state->depthStencil != nullptr)
      dsv = m_state->depthStencil->GetD3D11DepthStencil();

    if ((Flags & D3DCLEAR_STENCIL || Flags & D3DCLEAR_ZBUFFER) && dsv != nullptr) {
      uint32_t clearFlags = Flags & D3DCLEAR_STENCIL ? D3D11_CLEAR_STENCIL : 0;
      clearFlags |= Flags & D3DCLEAR_ZBUFFER ? D3D11_CLEAR_DEPTH : 0;

      m_context->ClearDepthStencilView(dsv, clearFlags, std::clamp(Z, 0.0f, 1.0f), Stencil);
    }
    return D3D_OK;
  }

  HRESULT D3D9ImmediateRenderer::drawTriangleFan(bool indexed, D3DPRIMITIVETYPE PrimitiveType, UINT StartIndex, UINT PrimitiveCount, UINT BaseVertexIndex) {
    const uint32_t newPrimitiveCount = PrimitiveCount * 3;
    const uint32_t length = newPrimitiveCount * sizeof(uint16_t);

    m_fanIndexBuffer.reserve(length);

    uint16_t* data = nullptr;
    m_fanIndexBuffer.map(m_context, (void**)&data, length);

    if (indexed && m_state->indexBuffer != nullptr) {
      D3D11_MAPPED_SUBRESOURCE res;
      ID3D11Resource* originalIndexBuffer = m_state->indexBuffer->GetDXUPResource()->GetStaging();
      m_context->Map(originalIndexBuffer, 0, D3D11_MAP_READ, 0, &res);

      uint16_t* originalIndices = reinterpret_cast<uint16_t*>(res.pData);

      for (UINT i = 0; i < PrimitiveCount; i++) {
        data[3 * i + 0] = originalIndices[StartIndex + i + 1];
        data[3 * i + 1] = originalIndices[StartIndex + i + 2];
        data[3 * i + 2] = originalIndices[StartIndex + 0];
      }

      m_context->Unmap(originalIndexBuffer, 0);
    }
    else {
      for (UINT i = 0; i < PrimitiveCount; i++) {
        data[3 * i + 0] = i + 1;
        data[3 * i + 1] = i + 2;
        data[3 * i + 2] = 0;
      }
    }

    uint32_t offset = m_fanIndexBuffer.unmap(m_context, length);

    m_fanIndexed = indexed;

    m_context->IASetIndexBuffer(m_fanIndexBuffer.getBuffer(), DXGI_FORMAT_R16_UINT, offset);
    HRESULT result = DrawIndexedPrimitive(D3DPT_TRIANGLELIST, BaseVertexIndex, 0, PrimitiveCount + 2, 0, newPrimitiveCount);
    m_state->dirtyFlags |= dirtyFlags::indexBuffer;
    return result;
  }

  HRESULT D3D9ImmediateRenderer::DrawPrimitive(D3DPRIMITIVETYPE PrimitiveType, UINT StartVertex, UINT PrimitiveCount) {
    if (!preDraw()) {
      log::warn("Invalid internal render state achieved.");
      postDraw();
      return D3D_OK; // Lies!
    }

    if (PrimitiveType == D3DPT_TRIANGLEFAN)
      return this->drawTriangleFan(false, PrimitiveType, 0, PrimitiveCount, StartVertex);

    D3D_PRIMITIVE_TOPOLOGY topology;
    UINT drawCount = convert::primitiveData(PrimitiveType, PrimitiveCount, topology);

    m_context->IASetPrimitiveTopology(topology);
    m_context->Draw(drawCount, StartVertex);

    postDraw();

    return D3D_OK;
  }
  HRESULT D3D9ImmediateRenderer::DrawPrimitiveUP(D3DPRIMITIVETYPE PrimitiveType, UINT PrimitiveCount, CONST void* pVertexStreamZeroData, UINT VertexStreamZeroStride) {
    if (pVertexStreamZeroData == nullptr)
      return log::d3derr(D3DERR_INVALIDCALL, "DrawPrimitiveUP: pVertexStreamZeroData was nullptr.");

    if (!preDraw()) {
      log::warn("Invalid internal render state achieved.");
      postDraw();
      return D3D_OK; // Lies!
    }

    if (PrimitiveType == D3DPT_TRIANGLEFAN)
      return this->drawTriangleFan(false, PrimitiveType, 0, PrimitiveCount, 0);

    D3D_PRIMITIVE_TOPOLOGY topology;
    UINT drawCount = convert::primitiveData(PrimitiveType, PrimitiveCount, topology);
    UINT length = drawCount * VertexStreamZeroStride;

    m_upVertexBuffer.reserve(length);
    uint32_t offset = m_upVertexBuffer.update(m_context, pVertexStreamZeroData, length);

    ID3D11Buffer* buffer = m_upVertexBuffer.getBuffer();
    m_context->IASetVertexBuffers(0, 1, &buffer, &VertexStreamZeroStride, &offset);

    m_context->IASetPrimitiveTopology(topology);
    m_context->Draw(drawCount, 0);

    m_state->dirtyFlags |= dirtyFlags::vertexBuffers;

    postDraw();

    return D3D_OK;
  }
  HRESULT D3D9ImmediateRenderer::DrawIndexedPrimitiveUP(D3DPRIMITIVETYPE PrimitiveType, UINT MinVertexIndex, UINT NumVertices, UINT PrimitiveCount, const void* pIndexData, D3DFORMAT IndexDataFormat, const void* pVertexStreamZeroData, UINT VertexStreamZeroStride) {
    if (pVertexStreamZeroData == nullptr)
      return log::d3derr(D3DERR_INVALIDCALL, "DrawIndexedPrimitiveUP: pVertexStreamZeroData was nullptr.");

    if (pIndexData == nullptr)
      return log::d3derr(D3DERR_INVALIDCALL, "DrawIndexedPrimitiveUP: pIndexData was nullptr.");

    if (!preDraw()) {
      log::warn("Invalid internal render state achieved.");
      postDraw();
      return D3D_OK; // Lies!
    }

    if (PrimitiveType == D3DPT_TRIANGLEFAN)
      return this->drawTriangleFan(true, PrimitiveType, 0, PrimitiveCount, 0);

    D3D_PRIMITIVE_TOPOLOGY topology;
    UINT drawCount = convert::primitiveData(PrimitiveType, PrimitiveCount, topology);
    UINT length = (MinVertexIndex + NumVertices) * VertexStreamZeroStride;

    m_upVertexBuffer.reserve(length);
    uint32_t offset = m_upVertexBuffer.update(m_context, pVertexStreamZeroData, length);

    ID3D11Buffer* buffer = m_upVertexBuffer.getBuffer();
    m_context->IASetVertexBuffers(0, 1, &buffer, &VertexStreamZeroStride, &offset);

    length *= IndexDataFormat == D3DFMT_INDEX32 ? 4 : 2;

    m_upIndexBuffer.reserve(length);
    m_upIndexBuffer.update(m_context, pIndexData, length);
    m_context->IASetIndexBuffer(m_upIndexBuffer.getBuffer(), IndexDataFormat == D3DFMT_INDEX32 ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_R16_UINT, 0);

    m_context->IASetPrimitiveTopology(topology);
    m_context->DrawIndexed(drawCount, 0, 0);

    m_state->dirtyFlags |= dirtyFlags::vertexBuffers;
    m_state->dirtyFlags |= dirtyFlags::indexBuffer;

    postDraw();

    return D3D_OK;
  }
  HRESULT D3D9ImmediateRenderer::DrawIndexedPrimitive(D3DPRIMITIVETYPE PrimitiveType, INT BaseVertexIndex, UINT MinVertexIndex, UINT NumVertices, UINT startIndex, UINT primCount) {
    if (!preDraw()) {
      log::warn("Invalid internal render state achieved.");
      postDraw();
      return D3D_OK; // Lies!
    }

    if (PrimitiveType == D3DPT_TRIANGLEFAN)
      return this->drawTriangleFan(true, PrimitiveType, startIndex, primCount, BaseVertexIndex);

    D3D_PRIMITIVE_TOPOLOGY topology;
    UINT drawCount = convert::primitiveData(PrimitiveType, primCount, topology);

    m_context->IASetPrimitiveTopology(topology);
    m_context->DrawIndexed(drawCount, startIndex, BaseVertexIndex);

    postDraw();
    return D3D_OK;
  }

  //

  void D3D9ImmediateRenderer::blit(Direct3DSurface9* dst, Direct3DSurface9* src) {
    D3DSURFACE_DESC desc;
    src->GetDesc(&desc);

    D3D11_VIEWPORT viewport;
    viewport.TopLeftX = 0.0f;
    viewport.TopLeftY = 0.0f;
    viewport.Width = (float)desc.Width;
    viewport.Height = (float)desc.Height;
    viewport.MinDepth = 0.0f;
    viewport.MaxDepth = 1.0f;
    m_context->RSSetViewports(1, &viewport);
    m_state->dirtyFlags |= dirtyFlags::viewport;

    m_context->PSSetSamplers(0, 1, &m_blitSampler);
    m_state->dirtySamplers |= 1;

    m_context->RSSetState(nullptr);
    m_state->dirtyFlags |= dirtyFlags::rasterizer;

    m_context->OMSetDepthStencilState(nullptr, 0);
    m_state->dirtyFlags |= dirtyFlags::depthStencilState;

    // TODO! Do I need to do any SRGB-ness here.
    ID3D11RenderTargetView* dstRTV = dst->GetD3D11RenderTarget(false);
    m_context->OMSetRenderTargets(1, &dstRTV, nullptr);
    m_state->dirtyFlags |= dirtyFlags::renderTargets;
    
    ID3D11ShaderResourceView* srcSRV = src->GetDXUPResource()->GetSRV(false);
    m_context->PSSetShaderResources(0, 1, &srcSRV);
    m_state->dirtyFlags |= dirtyFlags::textures;

    m_context->VSSetShader(m_blitVS.ptr(), nullptr, 0);
    m_state->dirtyFlags |= dirtyFlags::vertexShader;

    m_context->PSSetShader(m_blitPS.ptr(), nullptr, 0);
    m_state->dirtyFlags |= dirtyFlags::pixelShader;

    m_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    m_context->IASetInputLayout(nullptr);
    m_state->dirtyFlags |= dirtyFlags::vertexDecl;

    m_context->Draw(3, 0);
  }

  void D3D9ImmediateRenderer::updateScissorRect() {
    m_context->RSSetScissorRects(1, (D3D11_RECT*)&m_state->scissorRect);
  }

  void D3D9ImmediateRenderer::updateViewport() {
    D3D11_VIEWPORT viewport;
    viewport.TopLeftX = (FLOAT)m_state->viewport.X;
    viewport.TopLeftY = (FLOAT)m_state->viewport.Y;
    viewport.MinDepth = m_state->viewport.MinZ;
    viewport.MaxDepth = m_state->viewport.MaxZ;
    viewport.Width = (FLOAT)m_state->viewport.Width;
    viewport.Height = (FLOAT)m_state->viewport.Height;
    m_context->RSSetViewports(1, &viewport);
  }

  static D3DMATRIX mulMatrix(const D3DMATRIX& a, const D3DMATRIX& b) {
    D3DMATRIX m;
    for (uint32_t row = 0; row < 4; row++) {
      for (uint32_t col = 0; col < 4; col++) {
        float sum = 0.0f;
        for (uint32_t k = 0; k < 4; k++)
          sum += (&a._11)[row * 4 + k] * (&b._11)[k * 4 + col];
        (&m._11)[row * 4 + col] = sum;
      }
    }
    return m;
  }

  HRESULT D3D9ImmediateRenderer::getFFPVS(const ffp::FFPVertexInputs& inputs, ID3D11VertexShader** outShader, ID3DBlob** outBytecode) {
    *outShader = nullptr;
    if (outBytecode != nullptr)
      *outBytecode = nullptr;

    for (FFPVertexShaderEntry& entry : m_ffpVSs) {
      if (entry.inputs == inputs) {
        *outShader = entry.shader.ptr();

        if (outBytecode != nullptr) {
          *outBytecode = entry.bytecode.ptr();
          (*outBytecode)->AddRef();
        }
        return D3D_OK;
      }
    }

    FFPVertexShaderEntry entry;
    entry.inputs = inputs;

    HRESULT result = ffp::compileVertexShader(m_device, inputs, &entry.shader, &entry.bytecode);
    if (FAILED(result))
      return result;

    m_ffpVSs.push_back(std::move(entry));

    FFPVertexShaderEntry& cached = m_ffpVSs.back();
    *outShader = cached.shader.ptr();

    if (outBytecode != nullptr) {
      *outBytecode = cached.bytecode.ptr();
      (*outBytecode)->AddRef();
    }
    return D3D_OK;
  }

  ID3D11PixelShader* D3D9ImmediateRenderer::getFFPPS(bool hasTexture) {
    Com<ID3D11PixelShader>& ps = hasTexture ? m_ffpPSTextured : m_ffpPS;

    if (ps == nullptr) {
      ID3D11PixelShader* shader = nullptr;
      HRESULT result = ffp::compilePixelShader(m_device, hasTexture, &shader);
      if (FAILED(result)) {
        log::warn("getFFPPS: failed to compile FFP pixel shader.");
        return nullptr;
      }
      ps = shader;
    }

    return ps.ptr();
  }

  void D3D9ImmediateRenderer::injectFFPConstants() {
    D3D9ShaderConstants& vs = m_state->vsConstants;

    const D3DMATERIAL9& mat = m_state->material;
    const bool lighting = m_state->renderState[D3DRS_LIGHTING] != FALSE;
    const D3DCOLOR ambientState = (D3DCOLOR)m_state->renderState[D3DRS_AMBIENT];

    // c0-c3: world * view * proj
    D3DMATRIX wvp = mulMatrix(mulMatrix(m_state->worldMatrix, m_state->viewMatrix), m_state->projMatrix);
    float* m = (float*)&vs.floatConstants[0];
    m[0] = wvp._11; m[1] = wvp._12; m[2] = wvp._13; m[3] = wvp._14;
    m[4] = wvp._21; m[5] = wvp._22; m[6] = wvp._23; m[7] = wvp._24;
    m[8] = wvp._31; m[9] = wvp._32; m[10] = wvp._33; m[11] = wvp._34;
    m[12] = wvp._41; m[13] = wvp._42; m[14] = wvp._43; m[15] = wvp._44;

    // c4: light direction in world space (points in the direction light travels)
    Vector<float, 4> lightDir = { 0.0f, 0.0f, -1.0f, 0.0f };
    for (uint32_t i = 0; i < m_state->lights.size(); i++) {
      if (m_state->lightEnabled[i] && m_state->lights[i].Type == D3DLIGHT_DIRECTIONAL) {
        lightDir = { m_state->lights[i].Direction.x, m_state->lights[i].Direction.y, m_state->lights[i].Direction.z, 0.0f };
        break;
      }
    }
    vs.floatConstants[4] = lightDir;

    // c5: material diffuse
    vs.floatConstants[5] = { mat.Diffuse.r, mat.Diffuse.g, mat.Diffuse.b, mat.Diffuse.a };

    // c6: ambient, c7: light color
    Vector<float, 4> ambient = { 1.0f, 1.0f, 1.0f, 0.0f };
    Vector<float, 4> lightColor = { 0.0f, 0.0f, 0.0f, 0.0f };

    if (lighting) {
      float a[4];
      convert::color(ambientState, a);
      ambient = { a[0] * mat.Ambient.r, a[1] * mat.Ambient.g, a[2] * mat.Ambient.b, 0.0f };

      bool foundLight = false;
      for (uint32_t i = 0; i < m_state->lights.size(); i++) {
        if (m_state->lightEnabled[i]) {
          lightColor = { m_state->lights[i].Diffuse.r, m_state->lights[i].Diffuse.g, m_state->lights[i].Diffuse.b, 0.0f };
          foundLight = true;
          break;
        }
      }

      if (!foundLight)
        lightColor = { 1.0f, 1.0f, 1.0f, 0.0f };
    }

    vs.floatConstants[6] = ambient;
    vs.floatConstants[7] = lightColor;

    // c8-c11: world matrix (for normal transformation)
    float* w = (float*)&vs.floatConstants[8];
    w[0] = m_state->worldMatrix._11; w[1] = m_state->worldMatrix._12; w[2] = m_state->worldMatrix._13; w[3] = m_state->worldMatrix._14;
    w[4] = m_state->worldMatrix._21; w[5] = m_state->worldMatrix._22; w[6] = m_state->worldMatrix._23; w[7] = m_state->worldMatrix._24;
    w[8] = m_state->worldMatrix._31; w[9] = m_state->worldMatrix._32; w[10] = m_state->worldMatrix._33; w[11] = m_state->worldMatrix._34;
    w[12] = m_state->worldMatrix._41; w[13] = m_state->worldMatrix._42; w[14] = m_state->worldMatrix._43; w[15] = m_state->worldMatrix._44;
  }

  void D3D9ImmediateRenderer::updateVertexShaderAndInputLayout() {
    if (m_state->vertexDecl == nullptr)
      return;

    const bool ffp = m_state->vertexShader == nullptr;

    auto& elements = m_state->vertexDecl->GetD3D11Descs();

    ID3D11VertexShader* vertexShader = nullptr;
    const void* bytecode = nullptr;
    UINT bytecodeSize = 0;
    Com<ID3DBlob> ffpBytecode;

    if (ffp) {
      ffp::FFPVertexInputs inputs = ffp::deriveVertexInputs(elements);

      if (!inputs.position)
        return;

      if (FAILED(getFFPVS(inputs, &vertexShader, &ffpBytecode)))
        return;

      bytecode = ffpBytecode->GetBufferPointer();
      bytecodeSize = ffpBytecode->GetBufferSize();
    } else {
      vertexShader = m_state->vertexShader->GetD3D11Shader();
      auto* vertexShdrBytecode = m_state->vertexShader->GetTranslation();
      bytecode = vertexShdrBytecode->getBytecode();
      bytecodeSize = vertexShdrBytecode->getByteSize();
    }

    ID3D11InputLayout* layout = nullptr;
    if (!ffp)
      layout = m_state->vertexShader->GetLinkedInput(m_state->vertexDecl.ptr());

    if (layout == nullptr) {
      HRESULT result = m_device->CreateInputLayout(&elements[0], elements.size(), bytecode, bytecodeSize, &layout);

      if (FAILED(result)) {
        log::warn("updateVertexShaderAndInputLayout: failed to create input layout.");
        return;
      }

      if (!ffp)
        m_state->vertexShader->LinkInput(layout, m_state->vertexDecl.ptr());
    }

    m_state->dirtyFlags &= ~dirtyFlags::vertexDecl;
    m_state->dirtyFlags &= ~dirtyFlags::vertexShader;

    m_context->IASetInputLayout(layout);
    m_context->VSSetShader(vertexShader, nullptr, 0);

    if (layout != nullptr)
      layout->Release();
  }
  void D3D9ImmediateRenderer::updateDepthStencilState() {
    D3D11_DEPTH_STENCIL_DESC desc;
    desc.BackFace.StencilDepthFailOp = convert::stencilOp(m_state->renderState[D3DRS_CCW_STENCILZFAIL]);
    desc.BackFace.StencilFailOp = convert::stencilOp(m_state->renderState[D3DRS_CCW_STENCILFAIL]);
    desc.BackFace.StencilPassOp = convert::stencilOp(m_state->renderState[D3DRS_CCW_STENCILPASS]);
    desc.BackFace.StencilFunc = convert::func(m_state->renderState[D3DRS_CCW_STENCILFUNC]);

    desc.DepthEnable = (m_state->renderState[D3DRS_ZENABLE] == D3DZB_FALSE) ? FALSE : TRUE;
    desc.DepthFunc = convert::func(m_state->renderState[D3DRS_ZFUNC]);
    desc.DepthWriteMask = m_state->renderState[D3DRS_ZWRITEENABLE] == TRUE ? D3D11_DEPTH_WRITE_MASK_ALL : D3D11_DEPTH_WRITE_MASK_ZERO;

    desc.FrontFace.StencilDepthFailOp = convert::stencilOp(m_state->renderState[D3DRS_STENCILZFAIL]);
    desc.FrontFace.StencilFailOp = convert::stencilOp(m_state->renderState[D3DRS_STENCILFAIL]);
    desc.FrontFace.StencilPassOp = convert::stencilOp(m_state->renderState[D3DRS_STENCILPASS]);
    desc.FrontFace.StencilFunc = convert::func(m_state->renderState[D3DRS_STENCILFUNC]);

    desc.StencilEnable = m_state->renderState[D3DRS_STENCILENABLE] == TRUE ? TRUE : FALSE;
    desc.StencilReadMask = (UINT8)(m_state->renderState[D3DRS_STENCILMASK] & 0x000000FF); // I think we can do this.
    desc.StencilWriteMask = (UINT8)(m_state->renderState[D3DRS_STENCILWRITEMASK] & 0x000000FF);

    size_t hash = m_caches.depthStencil.hash(desc);
    ID3D11DepthStencilState* state = m_caches.depthStencil.lookupObject(hash);

    if (state == nullptr) {
      Com<ID3D11DepthStencilState> comState;

      HRESULT result = m_device->CreateDepthStencilState(&desc, &comState);
      if (FAILED(result)) {
        log::fail("Failed to create depth stencil state.");
        return;
      }

      m_caches.depthStencil.pushState(hash, desc, comState.ptr());
      state = comState.ptr();
    }

    m_context->OMSetDepthStencilState(state, (UINT)m_state->renderState[D3DRS_STENCILREF]);

    m_state->dirtyFlags &= ~dirtyFlags::depthStencilState;
  }
  void D3D9ImmediateRenderer::updateRasterizer() {
    D3D11_RASTERIZER_DESC1 desc;
    desc.AntialiasedLineEnable = false;
    desc.CullMode = convert::cullMode(m_state->renderState[D3DRS_CULLMODE]);
    desc.DepthBias = (INT)reinterpret::dwordToFloat(m_state->renderState[D3DRS_DEPTHBIAS]);
    desc.DepthBiasClamp = D3D11_DEFAULT_DEPTH_BIAS_CLAMP;
    desc.DepthClipEnable = true;
    desc.FillMode = convert::fillMode(m_state->renderState[D3DRS_FILLMODE]);
    desc.ForcedSampleCount = 0;
    desc.FrontCounterClockwise = false;
    desc.MultisampleEnable = false;
    desc.ScissorEnable = m_state->renderState[D3DRS_SCISSORTESTENABLE] == TRUE ? TRUE : FALSE;
    desc.SlopeScaledDepthBias = reinterpret::dwordToFloat(m_state->renderState[D3DRS_SLOPESCALEDEPTHBIAS]);

    size_t hash = m_caches.rasterizer.hash(desc);
    ID3D11RasterizerState1* state = m_caches.rasterizer.lookupObject(hash);

    if (state == nullptr) {
      Com<ID3D11RasterizerState1> comState;

      HRESULT result = m_device->CreateRasterizerState1(&desc, &comState);
      if (FAILED(result)) {
        log::fail("Failed to create rasterizer state.");
        return;
      }

      m_caches.rasterizer.pushState(hash, desc, comState.ptr());
      state = comState.ptr();
    }

    m_context->RSSetState(state);

    m_state->dirtyFlags &= ~dirtyFlags::rasterizer;
  }
  void D3D9ImmediateRenderer::updateBlendState() {
    D3D11_BLEND_DESC1 desc;
    desc.AlphaToCoverageEnable = false;
    desc.IndependentBlendEnable = false;

    bool separateAlpha = m_state->renderState[D3DRS_SEPARATEALPHABLENDENABLE] == TRUE;

    // Change me if we do independent blending at some point.
    for (uint32_t i = 0; i < 1; i++) {
      desc.RenderTarget[i].BlendEnable = m_state->renderState[D3DRS_ALPHABLENDENABLE] == TRUE;

      desc.RenderTarget[i].BlendOp = convert::blendOp(m_state->renderState[D3DRS_BLENDOP]);
      desc.RenderTarget[i].BlendOpAlpha = separateAlpha ? convert::blendOp(m_state->renderState[D3DRS_BLENDOPALPHA]) : D3D11_BLEND_OP_ADD;

      desc.RenderTarget[i].DestBlend = convert::blend(m_state->renderState[D3DRS_DESTBLEND]);
      desc.RenderTarget[i].DestBlendAlpha = separateAlpha ? convert::blend(m_state->renderState[D3DRS_DESTBLENDALPHA]) : D3D11_BLEND_ZERO;

      desc.RenderTarget[i].LogicOp = D3D11_LOGIC_OP_NOOP;
      desc.RenderTarget[i].LogicOpEnable = false;

      uint32_t writeIndex;
      switch (i) {
      default:
      case 0: writeIndex = D3DRS_COLORWRITEENABLE; break;
      case 1: writeIndex = D3DRS_COLORWRITEENABLE1; break;
      case 2: writeIndex = D3DRS_COLORWRITEENABLE2; break;
      case 3: writeIndex = D3DRS_COLORWRITEENABLE3; break;
      }

      desc.RenderTarget[i].RenderTargetWriteMask = m_state->renderState[writeIndex];

      desc.RenderTarget[i].SrcBlend = convert::blend(m_state->renderState[D3DRS_SRCBLEND]);
      desc.RenderTarget[i].SrcBlendAlpha = separateAlpha ? convert::blend(m_state->renderState[D3DRS_SRCBLENDALPHA]) : D3D11_BLEND_ONE;
    }

    size_t hash = m_caches.blendState.hash(desc);
    ID3D11BlendState1* state = m_caches.blendState.lookupObject(hash);

    if (state == nullptr) {
      Com<ID3D11BlendState1> comState;

      HRESULT result = m_device->CreateBlendState1(&desc, &comState);
      if (FAILED(result)) {
        log::fail("Failed to create blend state.");
        return;
      }

      m_caches.blendState.pushState(hash, desc, comState.ptr());
      state = comState.ptr();
    }

    float blendFactor[4];
    convert::color((D3DCOLOR)m_state->renderState[D3DRS_BLENDFACTOR], blendFactor);
    m_context->OMSetBlendState(state, blendFactor, 0xFFFFFFFF);
  }
  void D3D9ImmediateRenderer::updateSampler(uint32_t sampler) {
    auto& samplerState = m_state->samplerStates[sampler];

    D3D11_SAMPLER_DESC desc;
    desc.AddressU = convert::textureAddressMode(samplerState[D3DSAMP_ADDRESSU]);
    desc.AddressV = convert::textureAddressMode(samplerState[D3DSAMP_ADDRESSV]);
    desc.AddressW = convert::textureAddressMode(samplerState[D3DSAMP_ADDRESSW]);
    convert::color(samplerState[D3DSAMP_BORDERCOLOR], desc.BorderColor);
    desc.ComparisonFunc = D3D11_COMPARISON_NEVER;
    desc.Filter = convert::filter(samplerState[D3DSAMP_MAGFILTER], samplerState[D3DSAMP_MINFILTER], samplerState[D3DSAMP_MIPFILTER]);
    desc.MaxAnisotropy = std::clamp((UINT)samplerState[D3DSAMP_MAXANISOTROPY], 0u, 16u);
    desc.MipLODBias = std::clamp(reinterpret::dwordToFloat(samplerState[D3DSAMP_MIPMAPLODBIAS]), -16.0f, 15.99f);
    desc.MaxLOD = (FLOAT)samplerState[D3DSAMP_MAXMIPLEVEL];
    desc.MinLOD = -FLT_MAX;

    size_t hash = m_caches.sampler.hash(desc);
    ID3D11SamplerState* state = m_caches.sampler.lookupObject(hash);

    if (state == nullptr) {
      Com<ID3D11SamplerState> comState;

      HRESULT result = m_device->CreateSamplerState(&desc, &comState);
      if (FAILED(result)) {
        log::fail("Failed to create sampler state.");
        return;
      }

      m_caches.sampler.pushState(hash, desc, comState.ptr());
      state = comState.ptr();
    }

    if (sampler < 16)
      m_context->PSSetSamplers(sampler, 1, &state);
    else {
      sampler -= 16;
      m_context->VSSetSamplers(sampler, 1, &state);
    }

    m_state->dirtySamplers &= ~(1ull << sampler);
  }
  void D3D9ImmediateRenderer::updateSamplers() {
    for (uint32_t i = 0; i < 20; i++) {
      if (m_state->dirtySamplers & (1ull << i))
        updateSampler(i);
    }
  }
  void D3D9ImmediateRenderer::updateTextures() {
    std::array<ID3D11ShaderResourceView*, 20> srvs;

    for (uint32_t i = 0; i < 20; i++) {
      IDirect3DBaseTexture9* pTexture = m_state->textures[i];
      bool srgb = m_state->samplerStates[i][D3DSAMP_SRGBTEXTURE] == TRUE;

      srvs[i] = nullptr;

      if (pTexture != nullptr) {
        switch (pTexture->GetType()) {

        case D3DRTYPE_TEXTURE: {
          Direct3DTexture9* tex = reinterpret_cast<Direct3DTexture9*>(pTexture);
          srvs[i] = tex->GetDXUPResource()->GetSRV(srgb);
          break;
        }

        case D3DRTYPE_CUBETEXTURE: {
          Direct3DCubeTexture9* tex = reinterpret_cast<Direct3DCubeTexture9*>(pTexture);
          srvs[i] = tex->GetDXUPResource()->GetSRV(srgb);
          break;
        }

        default: log::warn("updateTextures: unknown resource type as a texture."); break;

        }
      }
    }

    m_context->PSSetShaderResources(0, 16, &srvs[0]);
    m_context->VSSetShaderResources(0, 4, &srvs[16]);
  }
  void D3D9ImmediateRenderer::updateRenderTargets() {
    std::array<ID3D11RenderTargetView*, 4> rtvs = { nullptr, nullptr, nullptr, nullptr };
    for (uint32_t i = 0; i < 4; i++)
    {
      if (m_state->renderTargets[i] != nullptr) {
        rtvs[i] = m_state->renderTargets[i]->GetD3D11RenderTarget(m_state->renderState[D3DRS_SRGBWRITEENABLE] == TRUE);
        m_state->renderTargets[i]->GetDXUPResource()->MarkDirty(0, 0); // Mark dirty so we copy to staging when read on the CPU again.
        if (rtvs[i] == nullptr)
          log::warn("No render target view for bound render target surface.");
      }
    }

    ID3D11DepthStencilView* dsv = nullptr;
    if (m_state->depthStencil != nullptr) {
      dsv = m_state->depthStencil->GetD3D11DepthStencil();
      if (dsv == nullptr)
        log::warn("No depth stencil view for bound depth stencil surface.");
    }

    m_context->OMSetRenderTargets(4, &rtvs[0], dsv);

    m_state->dirtyFlags &= ~dirtyFlags::renderTargets;
  }
  void D3D9ImmediateRenderer::updatePixelShader() {
    ID3D11PixelShader* ps = nullptr;

    if (m_state->pixelShader != nullptr)
      ps = m_state->pixelShader->GetD3D11Shader();
    else {
      bool hasTexture = false;

      if (m_state->vertexDecl != nullptr) {
        ffp::FFPVertexInputs inputs = ffp::deriveVertexInputs(m_state->vertexDecl->GetD3D11Descs());
        hasTexture = inputs.tex0 && m_state->textures[0] != nullptr;
      }

      ps = getFFPPS(hasTexture);
    }

    m_context->PSSetShader(ps, nullptr, 0);

    m_state->dirtyFlags &= ~dirtyFlags::pixelShader;
  }
  void D3D9ImmediateRenderer::updateVertexBuffer() {
    std::array<ID3D11Buffer*, 16> buffers;
    for (uint32_t i = 0; i < 16; i++) {
      Direct3DVertexBuffer9* buffer = m_state->vertexBuffers[i].ptr();
      if (buffer != nullptr)
        buffers[i] = buffer->GetDXUPResource()->GetResourceAs<ID3D11Buffer>();
      else
        buffers[i] = nullptr;
    }
    m_context->IASetVertexBuffers(0, 16, buffers.data(), m_state->vertexStrides.data(), m_state->vertexOffsets.data());
    m_state->dirtyFlags &= ~dirtyFlags::vertexBuffers;
  }
  void D3D9ImmediateRenderer::updateIndexBuffer() {
    DXGI_FORMAT format = DXGI_FORMAT_R16_UINT;

    ID3D11Buffer* buffer = nullptr;
    if (m_state->indexBuffer != nullptr) {
      if (m_state->indexBuffer->GetD3D9Desc().Format == D3DFMT_INDEX32)
        format = DXGI_FORMAT_R32_UINT;

      buffer = m_state->indexBuffer->GetDXUPResource()->GetResourceAs<ID3D11Buffer>();
    }

    m_context->IASetIndexBuffer(buffer, format, 0);
    m_state->dirtyFlags &= ~dirtyFlags::indexBuffer;
  }
  void D3D9ImmediateRenderer::updateVertexConstants() {
    if (m_state->vertexShader == nullptr)
      injectFFPConstants();

    m_vsConstants.update(m_state->vsConstants);
  }
  void D3D9ImmediateRenderer::updatePixelConstants() {
    m_psConstants.update(m_state->psConstants);
    m_state->dirtyFlags &= ~dirtyFlags::psConstants;
  }

  //

  void D3D9ImmediateRenderer::undirtyContext() {
    if (m_state->dirtyFlags & dirtyFlags::viewport)
      updateViewport();

    if (m_state->dirtyFlags & dirtyFlags::scissorRect)
      updateScissorRect();

    if (m_state->dirtyFlags & dirtyFlags::vertexBuffers)
      updateVertexBuffer();

    if (m_state->dirtyFlags & dirtyFlags::indexBuffer)
      updateIndexBuffer();

    if (m_state->dirtyFlags & dirtyFlags::vsConstants)
      updateVertexConstants();
    else if (m_state->vertexShader == nullptr && m_state->vertexDecl != nullptr)
      updateVertexConstants();

    if (m_state->dirtyFlags & dirtyFlags::psConstants)
      updatePixelConstants();

    if (m_state->dirtyFlags & dirtyFlags::vertexDecl || m_state->dirtyFlags & dirtyFlags::vertexShader)
      updateVertexShaderAndInputLayout();

    if (m_state->dirtySamplers != 0)
      updateSamplers();

    if (m_state->dirtyFlags & dirtyFlags::textures)
      updateTextures();

    if (m_state->dirtyFlags & dirtyFlags::renderTargets)
      updateRenderTargets();

    if (m_state->dirtyFlags & dirtyFlags::rasterizer)
      updateRasterizer();

    if (m_state->dirtyFlags & dirtyFlags::blendState)
      updateBlendState();

    if (m_state->dirtyFlags & dirtyFlags::depthStencilState)
      updateDepthStencilState();

    if (m_state->dirtyFlags & dirtyFlags::pixelShader)
      updatePixelShader();
  }

  //

  void D3D9ImmediateRenderer::handleDepthStencilDiscard() {
    if (m_state->depthStencil != nullptr && m_state->depthStencil->GetD3D9Desc().Discard) {
      ID3D11DepthStencilView* dsv = m_state->depthStencil->GetD3D11DepthStencil();
      if (dsv)
        m_context->ClearDepthStencilView(dsv, D3D11_CLEAR_DEPTH, 0.0f, 0);
    }
  }

  //

  bool D3D9ImmediateRenderer::canDraw() {
    return !( (m_state->dirtyFlags & dirtyFlags::vertexDecl) || (m_state->dirtyFlags & dirtyFlags::vertexShader) );
  }
  bool D3D9ImmediateRenderer::preDraw() {
    undirtyContext();
    return canDraw();
  }
  void D3D9ImmediateRenderer::postDraw() {
    // Nothing here yet!
  }

}