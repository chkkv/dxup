#include "d3d9_renderer.h"
#include "d3d9_texture.h"
#include "d3d9_util.h"
#include "shaders/blit.vs.dxbc.h"
#include "shaders/blit.ps.dxbc.h"

#include <cfloat>
#include <cmath>
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

    log::warn("drawTriangleFan: indexed=%d prim=%d start=%d len=%d indexBuffer=%p", indexed ? 1 : 0, PrimitiveCount, StartIndex, length, (void*)m_state->indexBuffer.ptr());
    m_fanIndexBuffer.reserve(length);

    uint16_t* data = nullptr;
    m_fanIndexBuffer.map(m_context, (void**)&data, length);

    if (indexed && m_state->indexBuffer != nullptr) {
      D3D11_MAPPED_SUBRESOURCE res;
      ID3D11Resource* originalIndexBuffer = m_state->indexBuffer->GetDXUPResource()->GetStaging();

      // If there is no staging buffer available (e.g. the resource was created
      // in a pool/usage combination that does not allocate one), fall back to
      // unindexed fan expansion rather than crashing on a null Map target.
      if (originalIndexBuffer == nullptr) {
        for (UINT i = 0; i < PrimitiveCount; i++) {
          data[3 * i + 0] = i + 1;
          data[3 * i + 1] = i + 2;
          data[3 * i + 2] = 0;
        }
        uint32_t fallbackOffset = m_fanIndexBuffer.unmap(m_context, length);
        m_fanIndexed = indexed;
        m_context->IASetIndexBuffer(m_fanIndexBuffer.getBuffer(), DXGI_FORMAT_R16_UINT, fallbackOffset);
        HRESULT fallbackResult = DrawIndexedPrimitive(D3DPT_TRIANGLELIST, BaseVertexIndex, 0, PrimitiveCount + 2, 0, newPrimitiveCount);
        m_state->dirtyFlags |= dirtyFlags::indexBuffer;
        return fallbackResult;
      }

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

  // 3x3 matrix inverse (top-left of a 4x4 row-major matrix), for normal transformation.
  static void inverseMatrix3(float* inv, const float* m) {
    float det = m[0] * (m[4] * m[8] - m[5] * m[7])
              - m[1] * (m[3] * m[8] - m[5] * m[6])
              + m[2] * (m[3] * m[7] - m[4] * m[6]);
    if (det == 0.0f) {
      inv[0] = 1.0f; inv[1] = 0.0f; inv[2] = 0.0f;
      inv[3] = 0.0f; inv[4] = 1.0f; inv[5] = 0.0f;
      inv[6] = 0.0f; inv[7] = 0.0f; inv[8] = 1.0f;
      return;
    }
    float invDet = 1.0f / det;
    inv[0] =  (m[4] * m[8] - m[5] * m[7]) * invDet;
    inv[1] = -(m[1] * m[8] - m[2] * m[7]) * invDet;
    inv[2] =  (m[1] * m[5] - m[2] * m[4]) * invDet;
    inv[3] = -(m[3] * m[8] - m[5] * m[6]) * invDet;
    inv[4] =  (m[0] * m[8] - m[2] * m[6]) * invDet;
    inv[5] = -(m[0] * m[5] - m[2] * m[3]) * invDet;
    inv[6] =  (m[3] * m[7] - m[4] * m[6]) * invDet;
    inv[7] = -(m[0] * m[7] - m[1] * m[6]) * invDet;
    inv[8] =  (m[0] * m[4] - m[1] * m[3]) * invDet;
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

    // c4: light direction in view space (directional, w=0) or position in view space (point/spot, w=1)
    // c12: spot params (cosPhi, cosTheta, falloff, isSpot)
    // c13: attenuation (a0, a1, a2, range)
    // c14: spot axis direction in view space
    // c15: light ambient color
    // c16: material ambient
    // c17: material emissive
    // c18-c20: normal matrix (inverse of view*world, 3x3)
    Vector<float, 4> lightDir = { 0.0f, 0.0f, -1.0f, 0.0f };
    Vector<float, 4> spotParams = { -1.0f, -1.0f, 0.0f, 0.0f };
    Vector<float, 4> spotDir = { 0.0f, 0.0f, -1.0f, 0.0f };
    Vector<float, 4> attenParams = { 1.0f, 0.0f, 0.0f, 0.0f };
    Vector<float, 4> lightAmbient = { 0.0f, 0.0f, 0.0f, 0.0f };

    const D3DLIGHT9* light = nullptr;
    for (uint32_t i = 0; i < m_state->lights.size(); i++) {
      if (m_state->lightEnabled[i]) {
        light = &m_state->lights[i];
        break;
      }
    }

    D3DMATRIX worldView = mulMatrix(m_state->worldMatrix, m_state->viewMatrix);

    // Lights are specified in world space; only the view matrix transforms them
    // into view space (D3D9 fixed-function lighting happens in view space).
    const D3DMATRIX& view = m_state->viewMatrix;

    if (light != nullptr) {
      if (light->Type == D3DLIGHT_DIRECTIONAL) {
        D3DVECTOR dir = light->Direction;
        // Transform direction to view space (no translation).
        Vector<float, 4> dirV = {
          view._11 * dir.x + view._12 * dir.y + view._13 * dir.z,
          view._21 * dir.x + view._22 * dir.y + view._23 * dir.z,
          view._31 * dir.x + view._32 * dir.y + view._33 * dir.z,
          0.0f
        };
        float len = std::sqrt(dirV[0] * dirV[0] + dirV[1] * dirV[1] + dirV[2] * dirV[2]);
        if (len > 0.0f) {
          dirV[0] /= len; dirV[1] /= len; dirV[2] /= len;
        }
        lightDir = dirV;
      } else {
        D3DVECTOR pos = light->Position;
        Vector<float, 4> posV = {
          view._11 * pos.x + view._12 * pos.y + view._13 * pos.z + view._14,
          view._21 * pos.x + view._22 * pos.y + view._23 * pos.z + view._24,
          view._31 * pos.x + view._32 * pos.y + view._33 * pos.z + view._34,
          1.0f
        };
        lightDir = posV;

        D3DVECTOR axis = light->Direction;
        Vector<float, 4> axisV = {
          view._11 * axis.x + view._12 * axis.y + view._13 * axis.z,
          view._21 * axis.x + view._22 * axis.y + view._23 * axis.z,
          view._31 * axis.x + view._32 * axis.y + view._33 * axis.z,
          0.0f
        };
        float alen = std::sqrt(axisV[0] * axisV[0] + axisV[1] * axisV[1] + axisV[2] * axisV[2]);
        if (alen > 0.0f) {
          axisV[0] /= alen; axisV[1] /= alen; axisV[2] /= alen;
        }
        spotDir = axisV;

        float a0 = light->Attenuation0;
        float a1 = light->Attenuation1;
        float a2 = light->Attenuation2;
        if (a0 == 0.0f && a1 == 0.0f && a2 == 0.0f)
          a0 = 1.0f;
        attenParams = { a0, a1, a2, light->Range };

        if (light->Type == D3DLIGHT_SPOT) {
          float cosPhi = std::cos(light->Phi / 2.0f);
          float cosTheta = std::cos(light->Theta / 2.0f);
          spotParams = { cosPhi, cosTheta, light->Falloff, 1.0f };
        }
      }

      lightAmbient = { light->Ambient.r, light->Ambient.g, light->Ambient.b, 0.0f };
    }
    vs.floatConstants[4] = lightDir;
    vs.floatConstants[12] = spotParams;
    vs.floatConstants[13] = attenParams;
    vs.floatConstants[14] = spotDir;
    vs.floatConstants[15] = lightAmbient;
    vs.floatConstants[16] = { mat.Ambient.r, mat.Ambient.g, mat.Ambient.b, mat.Ambient.a };
    vs.floatConstants[17] = { mat.Emissive.r, mat.Emissive.g, mat.Emissive.b, 0.0f };

    // c8-c11: world * view (for vertex position in view space)
    float* wv = (float*)&vs.floatConstants[8];
    wv[0] = worldView._11; wv[1] = worldView._12; wv[2] = worldView._13; wv[3] = worldView._14;
    wv[4] = worldView._21; wv[5] = worldView._22; wv[6] = worldView._23; wv[7] = worldView._24;
    wv[8] = worldView._31; wv[9] = worldView._32; wv[10] = worldView._33; wv[11] = worldView._34;
    wv[12] = worldView._41; wv[13] = worldView._42; wv[14] = worldView._43; wv[15] = worldView._44;

    // c18-c20: normal matrix = inverse(view * world), 3x3
    float wvF[16];
    float nm[9];
    std::memcpy(wvF, wv, sizeof(wvF));
    float wv33[9] = {
      wvF[0], wvF[1], wvF[2],
      wvF[4], wvF[5], wvF[6],
      wvF[8], wvF[9], wvF[10]
    };
    inverseMatrix3(nm, wv33);
    float* nmC = (float*)&vs.floatConstants[18];
    nmC[0] = nm[0]; nmC[1] = nm[1]; nmC[2] = nm[2];
    nmC[4] = nm[3]; nmC[5] = nm[4]; nmC[6] = nm[5];
    nmC[8] = nm[6]; nmC[9] = nm[7]; nmC[10] = nm[8];

    // c5: material diffuse
    vs.floatConstants[5] = { mat.Diffuse.r, mat.Diffuse.g, mat.Diffuse.b, mat.Diffuse.a };

    // c6: global ambient (scene ambient), c7: light color (w = lighting switch)
    Vector<float, 4> globalAmbient = { 0.0f, 0.0f, 0.0f, 0.0f };
    Vector<float, 4> lightColor = { 0.0f, 0.0f, 0.0f, 0.0f };

    if (lighting) {
      float a[4];
      convert::color(ambientState, a);
      globalAmbient = { a[0], a[1], a[2], 0.0f };

      if (light != nullptr)
        lightColor = { light->Diffuse.r, light->Diffuse.g, light->Diffuse.b, 1.0f };
      else
        lightColor = { 0.0f, 0.0f, 0.0f, 1.0f };
    }

    vs.floatConstants[6] = globalAmbient;
    vs.floatConstants[7] = lightColor;
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
      // D3D9's POSITIONT (pre-transformed vertex) maps to D3D11's POSITION semantic.
      // The translated shader declares its input as POSITION (SV_Position), so the
      // input layout must use POSITION as well, otherwise CreateInputLayout fails
      // with E_INVALIDARG due to a semantic name mismatch.
      std::vector<D3D11_INPUT_ELEMENT_DESC> layoutElements(elements.begin(), elements.end());
      for (D3D11_INPUT_ELEMENT_DESC& elem : layoutElements) {
        if (std::strcmp(elem.SemanticName, "POSITIONT") == 0)
          elem.SemanticName = "POSITION";
      }

      HRESULT result = m_device->CreateInputLayout(&layoutElements[0], layoutElements.size(), bytecode, bytecodeSize, &layout);

      // If the input layout fails to create, the shader may declare more components
      // than the vertex format provides (e.g. shader reads v2.xyzw but the declaration
      // only has FLOAT2). D3D11 requires layout component count >= shader component count.
      // Widen the element formats to 4 components and retry.
      if (FAILED(result) && !ffp) {
        std::vector<D3D11_INPUT_ELEMENT_DESC> widened(layoutElements.begin(), layoutElements.end());
        for (D3D11_INPUT_ELEMENT_DESC& elem : widened) {
          switch (elem.Format) {
          case DXGI_FORMAT_R32_FLOAT: elem.Format = DXGI_FORMAT_R32G32B32A32_FLOAT; break;
          case DXGI_FORMAT_R32G32_FLOAT: elem.Format = DXGI_FORMAT_R32G32B32A32_FLOAT; break;
          case DXGI_FORMAT_R32G32B32_FLOAT: elem.Format = DXGI_FORMAT_R32G32B32A32_FLOAT; break;
          case DXGI_FORMAT_R16G16_UNORM: elem.Format = DXGI_FORMAT_R16G16B16A16_UNORM; break;
          case DXGI_FORMAT_R16G16_SNORM: elem.Format = DXGI_FORMAT_R16G16B16A16_SNORM; break;
          case DXGI_FORMAT_R8G8B8A8_UNORM: break;
          case DXGI_FORMAT_B8G8R8A8_UNORM: break;
          default: break;
          }
        }
        HRESULT retry = m_device->CreateInputLayout(widened.data(), widened.size(), bytecode, bytecodeSize, &layout);
        if (SUCCEEDED(retry)) {
          result = retry;
          log::warn("updateVertexShaderAndInputLayout: widened element formats and retried input layout.");
        }
      }

      if (FAILED(result)) {
        log::warn("updateVertexShaderAndInputLayout: failed to create input layout. hr=%08x elements=%d bytecode=%d", (unsigned)result, (int)elements.size(), (int)bytecodeSize);
        for (uint32_t ei = 0; ei < elements.size(); ei++)
          log::warn("  elem[%d]: %s idx=%d fmt=%d off=%d", ei, elements[ei].SemanticName, elements[ei].SemanticIndex, (int)elements[ei].Format, (int)elements[ei].AlignedByteOffset);
        FILE* dbgf = fopen("shader_dump_vs.dxbc", "wb");
        if (dbgf) { fwrite(bytecode, 1, bytecodeSize, dbgf); fclose(dbgf); log::warn("  dumped shader to shader_dump_vs.dxbc"); }
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
    else if (m_state->pixelShader == nullptr && m_state->vertexDecl != nullptr)
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