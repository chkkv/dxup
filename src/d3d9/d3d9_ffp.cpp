#include "d3d9_ffp.h"
#include "d3d9_util.h"
#include "../util/d3dcompiler_helpers.h"
#include "../util/log.h"
#include <cstring>

namespace dxup {

  namespace ffp {

    bool fvfHasPosition(DWORD fvf) {
      return (fvf & D3DFVF_POSITION_MASK) != 0;
    }

    FFPVertexInputs deriveVertexInputs(const std::vector<D3D11_INPUT_ELEMENT_DESC>& d3d11Descs) {
      FFPVertexInputs inputs;

      for (const D3D11_INPUT_ELEMENT_DESC& desc : d3d11Descs) {
        if (std::strcmp(desc.SemanticName, "POSITION") == 0)
          inputs.position = true;
        else if (std::strcmp(desc.SemanticName, "POSITIONT") == 0) {
          inputs.position = true;
          inputs.rhw = true;
        }
        else if (std::strcmp(desc.SemanticName, "NORMAL") == 0 && desc.SemanticIndex == 0)
          inputs.normal = true;
        else if (std::strcmp(desc.SemanticName, "COLOR") == 0 && desc.SemanticIndex == 0)
          inputs.diffuse = true;
        else if (std::strcmp(desc.SemanticName, "TEXCOORD") == 0 && desc.SemanticIndex == 0)
          inputs.tex0 = true;
      }

      return inputs;
    }

    void buildFVFDeclaration(DWORD fvf, std::vector<D3D11_INPUT_ELEMENT_DESC>& d3d11Descs, std::vector<D3DVERTEXELEMENT9>& d3d9Descs) {
      d3d11Descs.clear();
      d3d9Descs.clear();

      UINT offset = 0;

      auto addElement = [&](D3DDECLUSAGE usage, UINT usageIndex, D3DDECLTYPE type, UINT size) {
        D3DVERTEXELEMENT9 d3d9Element;
        d3d9Element.Stream = 0;
        d3d9Element.Offset = offset;
        d3d9Element.Type = type;
        d3d9Element.Method = D3DDECLMETHOD_DEFAULT;
        d3d9Element.Usage = usage;
        d3d9Element.UsageIndex = usageIndex;
        d3d9Descs.push_back(d3d9Element);

        D3D11_INPUT_ELEMENT_DESC d3d11Desc;
        d3d11Desc.SemanticName = convert::declUsage(true, false, usage).c_str();
        d3d11Desc.SemanticIndex = usageIndex;
        d3d11Desc.Format = convert::declType(type);
        d3d11Desc.InputSlot = 0;
        d3d11Desc.AlignedByteOffset = offset;
        d3d11Desc.InputSlotClass = D3D11_INPUT_PER_VERTEX_DATA;
        d3d11Desc.InstanceDataStepRate = 0;
        d3d11Descs.push_back(d3d11Desc);

        offset += size;
      };

      if (fvf & D3DFVF_XYZW)
        addElement(D3DDECLUSAGE_POSITIONT, 0, D3DDECLTYPE_FLOAT4, 16);
      else if (fvf & D3DFVF_XYZRHW)
        addElement(D3DDECLUSAGE_POSITIONT, 0, D3DDECLTYPE_FLOAT4, 16);
      else if (fvf & D3DFVF_XYZ)
        addElement(D3DDECLUSAGE_POSITION, 0, D3DDECLTYPE_FLOAT3, 12);

      if (fvf & D3DFVF_NORMAL)
        addElement(D3DDECLUSAGE_NORMAL, 0, D3DDECLTYPE_FLOAT3, 12);

      if (fvf & D3DFVF_PSIZE)
        addElement(D3DDECLUSAGE_PSIZE, 0, D3DDECLTYPE_FLOAT1, 4);

      if (fvf & D3DFVF_DIFFUSE)
        addElement(D3DDECLUSAGE_COLOR, 0, D3DDECLTYPE_D3DCOLOR, 4);

      if (fvf & D3DFVF_SPECULAR)
        addElement(D3DDECLUSAGE_COLOR, 1, D3DDECLTYPE_D3DCOLOR, 4);

      for (UINT i = 0; i < 8; i++) {
        if (fvf & (D3DFVF_TEX1 << i))
          addElement(D3DDECLUSAGE_TEXCOORD, i, D3DDECLTYPE_FLOAT2, 8);
      }

      D3DVERTEXELEMENT9 end;
      end.Stream = 0xFF;
      end.Offset = 0;
      end.Type = D3DDECLTYPE_UNUSED;
      end.Method = 0;
      end.Usage = 0;
      end.UsageIndex = 0;
      d3d9Descs.push_back(end);
    }

    static const char g_ffpVS[] =
"row_major float4x4 cWVP        : register(c0);\n"
"float4   cLightDir   : register(c4);\n"
"float4   cMatDiffuse : register(c5);\n"
"float4   cAmbient    : register(c6);\n"
"float4   cLightColor : register(c7);\n"
"row_major float4x4 cWorld      : register(c8);\n"
"\n"
"struct VSIn {\n"
"#if HAS_RHW\n"
"  float4 pos : POSITIONT0;\n"
"#else\n"
"  float3 pos : POSITION0;\n"
"#endif\n"
"#if HAS_NORMAL\n"
"  float3 normal : NORMAL0;\n"
"#endif\n"
"#if HAS_DIFFUSE\n"
"  float4 color0 : COLOR0;\n"
"#endif\n"
"#if HAS_TEX0\n"
"  float2 tex0 : TEXCOORD0;\n"
"#endif\n"
"};\n"
"\n"
"struct VSOut {\n"
"  float4 position : SV_Position;\n"
"  float4 color    : COLOR0;\n"
"#if HAS_TEX0\n"
"  float2 tex0     : TEXCOORD0;\n"
"#endif\n"
"};\n"
"\n"
"VSOut main(VSIn input) {\n"
"  VSOut output;\n"
"\n"
"#if HAS_RHW\n"
"  output.position = input.pos;\n"
"#else\n"
"  output.position = mul(float4(input.pos, 1.0f), cWVP);\n"
"#endif\n"
"\n"
"#if HAS_DIFFUSE\n"
"  output.color = input.color0;\n"
"#else\n"
"  output.color = cMatDiffuse;\n"
"#endif\n"
"\n"
"#if HAS_NORMAL\n"
"  float3 normalW = normalize(mul(input.normal, (float3x3)cWorld));\n"
"  float nDotL = saturate(dot(normalW, -normalize(cLightDir.xyz)));\n"
"  output.color.rgb *= (cAmbient.rgb + nDotL * cLightColor.rgb);\n"
"#endif\n"
"\n"
"#if HAS_TEX0\n"
"  output.tex0 = input.tex0;\n"
"#endif\n"
"\n"
"  return output;\n"
"}\n";

    static const char g_ffpPS[] =
"Texture2D    cTex0  : register(t0);\n"
"SamplerState cSamp0 : register(s0);\n"
"\n"
"struct PSIn {\n"
"  float4 position : SV_Position;\n"
"  float4 color    : COLOR0;\n"
"#if HAS_TEX0\n"
"  float2 tex0     : TEXCOORD0;\n"
"#endif\n"
"};\n"
"\n"
"float4 main(PSIn input) : SV_Target {\n"
"#if HAS_TEX0\n"
"  return cTex0.Sample(cSamp0, input.tex0) * input.color;\n"
"#else\n"
"  return input.color;\n"
"#endif\n"
"}\n";

    static HRESULT compileShader(ID3D11Device1* device, const char* source, const D3D_SHADER_MACRO* defines, const char* target, bool vertex, ID3DBlob** outBytecode, void** outShader) {
      *outShader = nullptr;
      if (outBytecode != nullptr)
        *outBytecode = nullptr;

      ID3DBlob* code = nullptr;
      ID3DBlob* errors = nullptr;
      HRESULT result = D3D_OK;

      if (!d3dcompiler::compile(&result, source, std::strlen(source), "dxup_ffp", defines, "main", target, D3DCOMPILE_OPTIMIZATION_LEVEL3, &code, &errors))
        return D3DERR_INVALIDCALL;

      if (FAILED(result)) {
        if (errors != nullptr) {
          log::warn("FFP shader compile failed: %s", (char*)errors->GetBufferPointer());
          errors->Release();
        }
        return result;
      }

      if (vertex)
        result = device->CreateVertexShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, (ID3D11VertexShader**)outShader);
      else
        result = device->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, (ID3D11PixelShader**)outShader);

      if (FAILED(result)) {
        code->Release();
        return result;
      }

      if (outBytecode != nullptr)
        *outBytecode = code;
      else
        code->Release();

      return D3D_OK;
    }

    HRESULT compileVertexShader(ID3D11Device1* device, const FFPVertexInputs& inputs, ID3D11VertexShader** outShader, ID3DBlob** outBytecode) {
      D3D_SHADER_MACRO defines[] = {
        { "HAS_RHW",    inputs.rhw      ? "1" : "0" },
        { "HAS_NORMAL", inputs.normal   ? "1" : "0" },
        { "HAS_DIFFUSE", inputs.diffuse ? "1" : "0" },
        { "HAS_TEX0",   inputs.tex0     ? "1" : "0" },
        { nullptr, nullptr }
      };

      return compileShader(device, g_ffpVS, defines, "vs_4_0", true, outBytecode, (void**)outShader);
    }

    HRESULT compilePixelShader(ID3D11Device1* device, bool hasTexture, ID3D11PixelShader** outShader) {
      D3D_SHADER_MACRO defines[] = {
        { "HAS_TEX0", hasTexture ? "1" : "0" },
        { nullptr, nullptr }
      };

      return compileShader(device, g_ffpPS, defines, "ps_4_0", false, nullptr, (void**)outShader);
    }

  }

}
