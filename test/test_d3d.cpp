/*
 * DXUP D3D9 test application.
 *
 * Draws a rubik's cube (3x3x3, standard face colours) using the D3D9
 * fixed-function pipeline (FVF + SetTransform + DrawPrimitiveUP) and
 * rotates it clockwise around the Y axis.
 *
 * Build (mingw-w64):
 *   x86_64-w64-mingw32-gcc test_d3d.c -o test_d3d.exe -ld3d9 -lgdi32 -luser32 -O2 -mwindows
 *   i686-w64-mingw32-gcc   test_d3d.c -o test_d3d.exe -ld3d9 -lgdi32 -luser32 -O2 -mwindows
 */

#include <windows.h>
#include <d3d9.h>
#include <math.h>
#include <string.h>

typedef struct {
  float x, y, z;
  DWORD color;
} Vertex;

static IDirect3D9* g_pD3D = NULL;
static IDirect3DDevice9* g_pDevice = NULL;
static HWND g_hWnd = NULL;

static Vertex g_verts[972];
static int g_triCount = 0;

/* ---- Matrix helpers (row-major, D3DX-compatible) ---- */

static void matIdentity(D3DMATRIX* m) {
  m->_12 = m->_13 = m->_14 = 0.0f;
  m->_21 = m->_23 = m->_24 = 0.0f;
  m->_31 = m->_32 = m->_34 = 0.0f;
  m->_41 = m->_42 = m->_43 = 0.0f;
  m->_11 = m->_22 = m->_33 = m->_44 = 1.0f;
}

static void matMul(D3DMATRIX* out, const D3DMATRIX* a, const D3DMATRIX* b) {
  D3DMATRIX r;
  int row, col, k;
  for (row = 0; row < 4; row++) {
    for (col = 0; col < 4; col++) {
      float sum = 0.0f;
      for (k = 0; k < 4; k++)
        sum += (&a->_11)[row * 4 + k] * (&b->_11)[k * 4 + col];
      (&r._11)[row * 4 + col] = sum;
    }
  }
  *out = r;
}

static void matRotY(D3DMATRIX* m, float angle) {
  float c = cosf(angle);
  float s = sinf(angle);
  matIdentity(m);
  m->_11 = c;  m->_13 = -s;
  m->_31 = s;  m->_33 = c;
}

static void vecNormalize(D3DVECTOR* v) {
  float len = sqrtf(v->x * v->x + v->y * v->y + v->z * v->z);
  if (len > 0.0f) {
    v->x /= len;
    v->y /= len;
    v->z /= len;
  }
}

static void vecCross(D3DVECTOR* out, const D3DVECTOR* a, const D3DVECTOR* b) {
  out->x = a->y * b->z - a->z * b->y;
  out->y = a->z * b->x - a->x * b->z;
  out->z = a->x * b->y - a->y * b->x;
}

static float vecDot(const D3DVECTOR* a, const D3DVECTOR* b) {
  return a->x * b->x + a->y * b->y + a->z * b->z;
}

static void matLookAtLH(D3DMATRIX* m, const D3DVECTOR* eye, const D3DVECTOR* at, const D3DVECTOR* up) {
  D3DVECTOR zaxis, xaxis, yaxis;

  zaxis.x = at->x - eye->x;
  zaxis.y = at->y - eye->y;
  zaxis.z = at->z - eye->z;
  vecNormalize(&zaxis);

  vecCross(&xaxis, up, &zaxis);
  vecNormalize(&xaxis);

  vecCross(&yaxis, &zaxis, &xaxis);

  m->_11 = xaxis.x; m->_12 = yaxis.x; m->_13 = zaxis.x; m->_14 = 0.0f;
  m->_21 = xaxis.y; m->_22 = yaxis.y; m->_23 = zaxis.y; m->_24 = 0.0f;
  m->_31 = xaxis.z; m->_32 = yaxis.z; m->_33 = zaxis.z; m->_34 = 0.0f;
  m->_41 = -vecDot(&xaxis, eye);
  m->_42 = -vecDot(&yaxis, eye);
  m->_43 = -vecDot(&zaxis, eye);
  m->_44 = 1.0f;
}

static void matPerspectiveFovLH(D3DMATRIX* m, float fovY, float aspect, float zn, float zf) {
  float h = 1.0f / tanf(fovY * 0.5f);
  float w = h / aspect;
  matIdentity(m);
  m->_11 = w;
  m->_22 = h;
  m->_33 = zf / (zf - zn);
  m->_34 = 1.0f;
  m->_43 = -zn * zf / (zf - zn);
  m->_44 = 0.0f;
}

/* ---- Rubik's cube ---- */

static DWORD faceColor(int coord, int axis, int sign) {
  if (coord == sign) {
    if (axis == 0) return sign > 0 ? D3DCOLOR_XRGB(0, 0, 255)     : D3DCOLOR_XRGB(0, 170, 0);
    if (axis == 1) return sign > 0 ? D3DCOLOR_XRGB(255, 255, 255) : D3DCOLOR_XRGB(255, 255, 0);
    if (axis == 2) return sign > 0 ? D3DCOLOR_XRGB(230, 0, 0)     : D3DCOLOR_XRGB(255, 120, 0);
  }
  return D3DCOLOR_XRGB(0, 0, 0);
}

static void addQuad(Vertex* verts, int* triCount, float cx, float cy, float cz, float h, int axis, int sign, DWORD color) {
  float corner[4][3];
  int order[6] = { 0, 1, 2, 0, 2, 3 };
  int i;

  switch (axis) {
  case 0: /* X */
    corner[0][0] = sign * h; corner[0][1] = -h; corner[0][2] = -h;
    corner[1][0] = sign * h; corner[1][1] = -h; corner[1][2] =  h;
    corner[2][0] = sign * h; corner[2][1] =  h; corner[2][2] =  h;
    corner[3][0] = sign * h; corner[3][1] =  h; corner[3][2] = -h;
    break;
  case 1: /* Y */
    corner[0][0] = -h; corner[0][1] = sign * h; corner[0][2] = -h;
    corner[1][0] = -h; corner[1][1] = sign * h; corner[1][2] =  h;
    corner[2][0] =  h; corner[2][1] = sign * h; corner[2][2] =  h;
    corner[3][0] =  h; corner[3][1] = sign * h; corner[3][2] = -h;
    break;
  default: /* Z */
    corner[0][0] = -h; corner[0][1] = -h; corner[0][2] = sign * h;
    corner[1][0] = -h; corner[1][1] =  h; corner[1][2] = sign * h;
    corner[2][0] =  h; corner[2][1] =  h; corner[2][2] = sign * h;
    corner[3][0] =  h; corner[3][1] = -h; corner[3][2] = sign * h;
    break;
  }

  if (sign < 0) {
    int rev[6] = { 0, 2, 1, 0, 3, 2 };
    for (i = 0; i < 6; i++)
      order[i] = rev[i];
  }

  for (i = 0; i < 6; i++) {
    int c = order[i];
    verts[*triCount * 3 + i].x = cx + corner[c][0];
    verts[*triCount * 3 + i].y = cy + corner[c][1];
    verts[*triCount * 3 + i].z = cz + corner[c][2];
    verts[*triCount * 3 + i].color = color;
  }
  (*triCount)++;
}

static void buildRubiksCube(void) {
  int i, j, k;
  g_triCount = 0;

  for (i = -1; i <= 1; i++) {
    for (j = -1; j <= 1; j++) {
      for (k = -1; k <= 1; k++) {
        float cx = (float)i, cy = (float)j, cz = (float)k;
        addQuad(g_verts, &g_triCount, cx, cy, cz, 0.45f, 0, +1, faceColor(i, 0, +1));
        addQuad(g_verts, &g_triCount, cx, cy, cz, 0.45f, 0, -1, faceColor(i, 0, -1));
        addQuad(g_verts, &g_triCount, cx, cy, cz, 0.45f, 1, +1, faceColor(j, 1, +1));
        addQuad(g_verts, &g_triCount, cx, cy, cz, 0.45f, 1, -1, faceColor(j, 1, -1));
        addQuad(g_verts, &g_triCount, cx, cy, cz, 0.45f, 2, +1, faceColor(k, 2, +1));
        addQuad(g_verts, &g_triCount, cx, cy, cz, 0.45f, 2, -1, faceColor(k, 2, -1));
      }
    }
  }
}

/* ---- Rendering ---- */

static void renderFrame(void) {
  float angle = (float)GetTickCount() / 1000.0f;
  RECT rc;
  float aspect;
  D3DMATRIX proj, view, world;
  D3DVECTOR eye = { 0.0f, 2.2f, 4.5f };
  D3DVECTOR at  = { 0.0f, 0.0f, 0.0f };
  D3DVECTOR up  = { 0.0f, 1.0f, 0.0f };

  GetClientRect(g_hWnd, &rc);
  aspect = rc.right > 0 && rc.bottom > 0 ? (float)rc.right / (float)rc.bottom : 1.0f;

  g_pDevice->Clear(0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, D3DCOLOR_XRGB(35, 35, 45), 1.0f, 0);

  g_pDevice->SetRenderState(D3DRS_LIGHTING, FALSE);
  g_pDevice->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
  g_pDevice->SetRenderState(D3DRS_ZENABLE, TRUE);
  g_pDevice->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);

  matPerspectiveFovLH(&proj, 60.0f * 3.14159265f / 180.0f, aspect, 0.1f, 100.0f);
  g_pDevice->SetTransform(D3DTS_PROJECTION, &proj);

  matLookAtLH(&view, &eye, &at, &up);
  g_pDevice->SetTransform(D3DTS_VIEW, &view);

  /* Clockwise rotation when viewed from above (+Y). */
  matRotY(&world, -angle);
  g_pDevice->SetTransform(D3DTS_WORLD, &world);

  g_pDevice->SetFVF(D3DFVF_XYZ | D3DFVF_DIFFUSE);
  g_pDevice->DrawPrimitiveUP(D3DPT_TRIANGLELIST, g_triCount, g_verts, sizeof(Vertex));

  g_pDevice->Present(NULL, NULL, NULL, NULL);
}

/* ---- Window ---- */

static LRESULT CALLBACK wndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
  switch (msg) {
  case WM_KEYDOWN:
    if (wParam == VK_ESCAPE) {
      DestroyWindow(hWnd);
      return 0;
    }
    break;
  case WM_DESTROY:
    PostQuitMessage(0);
    return 0;
  }
  return DefWindowProc(hWnd, msg, wParam, lParam);
}

static HRESULT initD3D(HWND hWnd) {
  D3DPRESENT_PARAMETERS pp;

  g_pD3D = Direct3DCreate9(D3D_SDK_VERSION);
  if (g_pD3D == NULL)
    return E_FAIL;

  memset(&pp, 0, sizeof(pp));
  pp.Windowed = TRUE;
  pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
  pp.BackBufferFormat = D3DFMT_UNKNOWN;
  pp.BackBufferCount = 1;
  pp.EnableAutoDepthStencil = TRUE;
  pp.AutoDepthStencilFormat = D3DFMT_D16;

  if (g_pD3D->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hWnd,
        D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &g_pDevice) != D3D_OK) {
    if (g_pD3D->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hWnd,
          D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, &g_pDevice) != D3D_OK) {
      return E_FAIL;
    }
  }

  buildRubiksCube();
  return S_OK;
}

static void cleanup(void) {
  if (g_pDevice) g_pDevice->Release();
  if (g_pD3D) g_pD3D->Release();
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
  WNDCLASSA wc;
  MSG msg;

  (void)hPrevInstance;
  (void)lpCmdLine;
  (void)nCmdShow;

  memset(&wc, 0, sizeof(wc));
  wc.style = CS_HREDRAW | CS_VREDRAW;
  wc.lpfnWndProc = wndProc;
  wc.hInstance = hInstance;
  wc.hCursor = LoadCursor(NULL, IDC_ARROW);
  wc.lpszClassName = "DXUPTEST";
  if (!RegisterClassA(&wc))
    return 1;

  g_hWnd = CreateWindowA("DXUPTEST", "DXUP D3D9 Test - Rubik's Cube",
    WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 640, 480,
    NULL, NULL, hInstance, NULL);
  if (g_hWnd == NULL)
    return 1;

  if (FAILED(initD3D(g_hWnd))) {
    MessageBoxA(g_hWnd, "Failed to create D3D9 device", "DXUP Test", MB_ICONERROR);
    return 1;
  }

  ShowWindow(g_hWnd, SW_SHOW);
  UpdateWindow(g_hWnd);

  while (TRUE) {
    while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
      if (msg.message == WM_QUIT) {
        cleanup();
        return (int)msg.wParam;
      }
      TranslateMessage(&msg);
      DispatchMessageA(&msg);
    }
    renderFrame();
  }

  cleanup();
  return 0;
}
