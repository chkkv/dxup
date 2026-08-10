#pragma once

#include "d3d9_includes.h"
#include <vector>

namespace dxup {

  namespace ffp {

    struct FFPVertexInputs {
      bool position = false;
      bool rhw = false;
      bool normal = false;
      bool diffuse = false;
      bool tex0 = false;

      bool operator == (const FFPVertexInputs& other) const {
        return position == other.position
          && rhw == other.rhw
          && normal == other.normal
          && diffuse == other.diffuse
          && tex0 == other.tex0;
      }
    };

    void buildFVFDeclaration(DWORD fvf, std::vector<D3D11_INPUT_ELEMENT_DESC>& d3d11Descs, std::vector<D3DVERTEXELEMENT9>& d3d9Descs);

    FFPVertexInputs deriveVertexInputs(const std::vector<D3D11_INPUT_ELEMENT_DESC>& d3d11Descs);

    HRESULT compileVertexShader(ID3D11Device1* device, const FFPVertexInputs& inputs, ID3D11VertexShader** outShader, ID3DBlob** outBytecode);

    HRESULT compilePixelShader(ID3D11Device1* device, bool hasTexture, ID3D11PixelShader** outShader);

    bool fvfHasPosition(DWORD fvf);

  }

}
