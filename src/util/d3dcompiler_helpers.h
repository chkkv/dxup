#pragma once

#include <d3dcompiler.h>

namespace dxup {

  namespace d3dcompiler {

    bool disassemble(HRESULT* result, LPCVOID pSrcData, SIZE_T SrcDataSize, UINT Flags, LPCSTR szComments, ID3DBlob** ppDisassembly);

    bool compile(HRESULT* result, LPCVOID pSrcData, SIZE_T SrcDataSize, LPCSTR pSourceName, const D3D_SHADER_MACRO* pDefines, LPCSTR pEntrypoint, LPCSTR pTarget, UINT Flags, ID3DBlob** ppCode, ID3DBlob** ppErrorMsgs);

  }

  namespace d3dx {

    bool dissasembleShader(HRESULT* result, LPCVOID pShader, BOOL EnableColorCode, LPCSTR pComments, ID3DBlob** ppDisassembly);

  }

}