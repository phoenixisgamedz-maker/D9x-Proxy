#define _CRT_SECURE_NO_WARNINGS
#include "Direct3DDeviceProxy.h"
#include "Shared.h"
#include <algorithm>
#include <unordered_map>
#include <vector>
#include <string>

// ================= DIAGNOSTIC =================
static int g_diag_DIP = 0, g_diag_DP = 0, g_diag_DPUP = 0;
static int g_diag_text_DIP = 0, g_diag_text_DP = 0, g_diag_text_DPUP = 0;
static int g_candidateHits = 0;

// ================= GLOBAL =================
static std::unordered_map<uint32_t, GlyphEntry> g_FontMap;
static std::unordered_map<uint32_t, int> g_UnknownGlyphCount;
static std::vector<GlyphInstance> g_TextBuffer;
static std::vector<std::wstring> g_KnownPhrases = {
    L"PRESS START BUTTON",
    L"HD COMPATIBLE FOR OPTIMAL GAMING",
    L"ELECTRONIC ARTS INC",
    L"ALL RIGHTS RESERVED",
    L"START",
    L"OPTIONS",
    L"BACK",
    L"SELECT",
    L"LOADING",
    L"SAVING",
    L"NEW GAME",
    L"LOAD GAME",
    L"EXIT",
    L"YES",
    L"NO",
    L"CONTINUE"
};

// ================= HELPERS =================
bool HookedIDirect3DDevice9::IsLikelyTextTexture(IDirect3DBaseTexture9* tex)
{
    if (!tex) return false;

    IDirect3DTexture9* t = nullptr;
    if (FAILED(tex->QueryInterface(__uuidof(IDirect3DTexture9), (void**)&t)))
        return false;

    D3DSURFACE_DESC d{};
    t->GetLevelDesc(0, &d);
    t->Release();

    return (d.Width <= 512 && d.Height <= 512) &&
        (d.Format == D3DFMT_A8R8G8B8 || d.Format == D3DFMT_A4R4G4B4);
}

bool HookedIDirect3DDevice9::HasTextTexture()
{
    for (int i = 0; i < 3; i++)
        if (IsLikelyTextTexture(m_texStages[i])) return true;
    return false;
}

// ================= HASH =================
uint32_t HookedIDirect3DDevice9::HashGlyph(BYTE* pixels, int pitch, int x, int y, int w, int h)
{
    uint32_t hash = 2166136261u;

    for (int yy = 0; yy < h; yy++)
    {
        for (int xx = 0; xx < w; xx++)
        {
            DWORD* px = (DWORD*)(pixels + (y + yy) * pitch + (x + xx) * 4);

            BYTE r = (*px >> 16) & 0xFF;
            BYTE g = (*px >> 8) & 0xFF;
            BYTE b = (*px) & 0xFF;
            BYTE a = (*px >> 24) & 0xFF;

            BYTE gray = (r * 30 + g * 59 + b * 11) / 100;
            BYTE combined = (gray ^ (a >> 2));

            hash ^= combined;
            hash *= 16777619;
        }
    }
    return hash;
}

// ================= SAVE BMP =================
void HookedIDirect3DDevice9::SaveGlyphToBMP(uint32_t hash, BYTE* pixels, int pitch, int x, int y, int w, int h)
{
    CreateDirectoryA("glyphs", NULL);

    char filename[256];
    sprintf_s(filename, "glyphs\\glyph_%08X.bmp", hash);

    FILE* f = nullptr;
    errno_t err = fopen_s(&f, filename, "wb");
    if (err != 0 || !f) return;

    BITMAPFILEHEADER bf = { 0 };
    BITMAPINFOHEADER bi = { 0 };

    bf.bfType = 0x4D42;
    bf.bfOffBits = sizeof(bf) + sizeof(bi);
    bf.bfSize = bf.bfOffBits + w * h * 3;

    bi.biSize = sizeof(bi);
    bi.biWidth = w;
    bi.biHeight = h;
    bi.biPlanes = 1;
    bi.biBitCount = 24;

    fwrite(&bf, sizeof(bf), 1, f);
    fwrite(&bi, sizeof(bi), 1, f);

    for (int yy = h - 1; yy >= 0; yy--)
    {
        for (int xx = 0; xx < w; xx++)
        {
            DWORD* px = (DWORD*)(pixels + (y + yy) * pitch + (x + xx) * 4);

            BYTE r = (*px >> 16) & 0xFF;
            BYTE g = (*px >> 8) & 0xFF;
            BYTE bVal = (*px) & 0xFF;

            fputc(bVal, f);
            fputc(g, f);
            fputc(r, f);
        }
    }

    fclose(f);
}

// ================= PROCESS GLYPH =================
GlyphUV HookedIDirect3DDevice9::ProcessGlyph(IDirect3DTexture9* tex9, VertexUV* verts)
{
    GlyphUV g = { 0 };
    if (!tex9 || !verts) return g;

    g.minU = g.maxU = verts[0].u;
    g.minV = g.maxV = verts[0].v;

    for (int i = 1; i < 4; i++)
    {
        if (verts[i].u < g.minU) g.minU = verts[i].u;
        if (verts[i].u > g.maxU) g.maxU = verts[i].u;
        if (verts[i].v < g.minV) g.minV = verts[i].v;
        if (verts[i].v > g.maxV) g.maxV = verts[i].v;
    }

    g.minU = max(0.0f, min(1.0f, g.minU));
    g.maxU = max(0.0f, min(1.0f, g.maxU));
    g.minV = max(0.0f, min(1.0f, g.minV));
    g.maxV = max(0.0f, min(1.0f, g.maxV));

    D3DSURFACE_DESC desc;
    if (FAILED(tex9->GetLevelDesc(0, &desc))) return g;

    g.texX = (int)(g.minU * desc.Width);
    g.texY = (int)(g.minV * desc.Height);
    g.width = (int)((g.maxU - g.minU) * desc.Width);
    g.height = (int)((g.maxV - g.minV) * desc.Height);

    if (g.texX < 0 || g.texY < 0 ||
        g.texX + g.width > desc.Width ||
        g.texY + g.height > desc.Height)
    {
        return g;
    }

    if (g.width < 8 || g.height < 8 || g.width > 64 || g.height > 64)
        return g;

    IDirect3DSurface9* surface = nullptr;
    if (FAILED(tex9->GetSurfaceLevel(0, &surface))) return g;

    D3DLOCKED_RECT lock;
    if (FAILED(surface->LockRect(&lock, NULL, D3DLOCK_READONLY)))
    {
        surface->Release();
        return g;
    }

    BYTE* pixels = (BYTE*)lock.pBits;

    int alphaSum = 0;
    for (int yy = 0; yy < g.height; yy++)
    {
        for (int xx = 0; xx < g.width; xx++)
        {
            DWORD* px = (DWORD*)(pixels + (g.texY + yy) * lock.Pitch + (g.texX + xx) * 4);
            BYTE a = (*px >> 24) & 0xFF;
            alphaSum += a;
        }
    }

    if (alphaSum < (g.width * g.height * 5))
    {
        surface->UnlockRect();
        surface->Release();
        return g;
    }

    g.hash = HashGlyph(pixels, lock.Pitch, g.texX, g.texY, g.width, g.height);

    surface->UnlockRect();
    surface->Release();
    return g;
}

// ================= TEXT =================
wchar_t HookedIDirect3DDevice9::GetCharFromHash(uint32_t hash)
{
    auto it = g_FontMap.find(hash);
    if (it != g_FontMap.end() && it->second.count > 0)
        return it->second.character;

    if (g_UnknownGlyphCount[hash]++ < 5)
        LogF("❓ UNKNOWN GLYPH: 0x%08X", hash);

    return L'?';
}

void HookedIDirect3DDevice9::AddGlyph(uint32_t hash, float x, float y, int h)
{
    GlyphInstance glyph;
    glyph.hash = hash;
    glyph.character = GetCharFromHash(hash);
    glyph.screenX = x;
    glyph.screenY = y;
    glyph.height = h;
    g_TextBuffer.push_back(glyph);
}

// ================= COMPARE STRINGS =================
float HookedIDirect3DDevice9::CompareStrings(const std::wstring& a, const std::wstring& b)
{
    if (a.empty() || b.empty()) return 0.0f;

    int match = 0;
    int total = 0;

    for (size_t i = 0; i < a.size(); i++)
    {
        wchar_t ca = a[i];

        if (ca == L'?' || ca == 0)
            continue;

        bool found = false;

        for (int shift = -2; shift <= 2; shift++)
        {
            int j = (int)i + shift;
            if (j < 0 || j >= (int)b.size()) continue;

            if (ca == b[j])
            {
                found = true;
                break;
            }
        }

        int weight = (ca >= L'A' && ca <= L'Z') ? 2 : 1;
        if (found) match += weight;
        total += weight;
    }

    return (total == 0) ? 0.0f : (float)match / (float)total;
}

// ================= ATTEMPT LEARNING =================
void HookedIDirect3DDevice9::AttemptLearning(const std::vector<DetectedGlyph>& glyphs, const std::wstring& text)
{
    if (glyphs.empty() || text.empty()) return;

    float bestScore = 0.0f;
    std::wstring bestMatch;

    for (const auto& phrase : g_KnownPhrases)
    {
        float score = CompareStrings(text, phrase);
        if (score > bestScore)
        {
            bestScore = score;
            bestMatch = phrase;
        }
    }

    if (bestScore > 0.6f)
    {
        LogF("🔥 LEARN: '%S' -> '%S' (score=%.2f)", text.c_str(), bestMatch.c_str(), bestScore);

        for (size_t i = 0; i < glyphs.size(); i++)
        {
            if (glyphs[i].hash == 0) continue;

            bool foundMatch = false;

            for (int shift = -2; shift <= 2; shift++)
            {
                int j = (int)i + shift;
                if (j < 0 || j >= (int)bestMatch.size()) continue;

                wchar_t expected = bestMatch[j];

                auto& entry = g_FontMap[glyphs[i].hash];

                if (entry.count == 0)
                {
                    entry.character = expected;
                    entry.count = 1;
                    foundMatch = true;
                    LogF("   MAPPING: 0x%08X -> '%lc' (new)", glyphs[i].hash, expected);
                    break;
                }
                else if (entry.character == expected)
                {
                    entry.count++;
                    foundMatch = true;
                    break;
                }
                else if (entry.count < 3)
                {
                    entry.character = expected;
                    entry.count = 1;
                    foundMatch = true;
                    LogF("   MAPPING: 0x%08X -> '%lc' (conflict)", glyphs[i].hash, expected);
                    break;
                }
            }
        }
    }
    else if (bestScore > 0.3f)
    {
        LogF("⚠️ LOW SCORE: '%S' vs '%S' (%.2f)", text.c_str(), bestMatch.c_str(), bestScore);
    }
}

// ================= SAVE FONT MAP =================
void HookedIDirect3DDevice9::SaveFontMap()
{
    FILE* f = nullptr;
    fopen_s(&f, "fontmap.txt", "w");
    if (f)
    {
        for (auto& pair : g_FontMap)
        {
            fprintf(f, "%08X %lc %d\n", pair.first, pair.second.character, pair.second.count);
        }
        fclose(f);
        LogF("💾 Saved %zu glyph mappings", g_FontMap.size());
    }
}

// ================= LOAD FONT MAP =================
void HookedIDirect3DDevice9::LoadFontMap()
{
    FILE* f = nullptr;
    fopen_s(&f, "fontmap.txt", "r");
    if (f)
    {
        uint32_t hash;
        wchar_t ch;
        int count;
        while (fscanf_s(f, "%08X %lc %d\n", &hash, &ch, 1, &count) == 3)
        {
            g_FontMap[hash] = { ch, count };
        }
        fclose(f);
        LogF("📖 Loaded %zu glyph mappings", g_FontMap.size());
    }
}

// ================= FLUSH TEXT =================
void HookedIDirect3DDevice9::FlushText()
{
    if (g_TextBuffer.empty()) return;

    std::sort(g_TextBuffer.begin(), g_TextBuffer.end(),
        [](const GlyphInstance& a, const GlyphInstance& b)
        {
            float t = (a.height > b.height) ? a.height * 0.5f : b.height * 0.5f;
            float dy = (a.screenY > b.screenY) ? a.screenY - b.screenY : b.screenY - a.screenY;
            if (dy < t)
                return a.screenX < b.screenX;
            return a.screenY < b.screenY;
        });

    std::vector<DetectedGlyph> currentLineGlyphs;
    std::wstring currentLineText;
    float lastY = g_TextBuffer[0].screenY;
    float lastX = g_TextBuffer[0].screenX;
    float threshold = g_TextBuffer[0].height * 0.5f;

    for (size_t idx = 0; idx < g_TextBuffer.size(); idx++)
    {
        const GlyphInstance& g = g_TextBuffer[idx];
        float dy = (g.screenY > lastY) ? g.screenY - lastY : lastY - g.screenY;

        if (dy > threshold)
        {
            if (!currentLineText.empty())
            {
                LogF("📝 TEXT: %S", currentLineText.c_str());
                AttemptLearning(currentLineGlyphs, currentLineText);
            }
            currentLineGlyphs.clear();
            currentLineText.clear();
            lastY = g.screenY;
            lastX = g.screenX;
            threshold = g.height * 0.5f;
        }

        if (!currentLineText.empty())
        {
            float dx = (g.screenX > lastX) ? g.screenX - lastX : lastX - g.screenX;
            if (dx > g.height * 0.5f)
            {
                currentLineText += L' ';
                currentLineGlyphs.push_back({ 0, g.screenX, g.screenY });
            }
        }

        currentLineGlyphs.push_back({ g.hash, g.screenX, g.screenY });
        currentLineText += g.character;
        lastX = g.screenX;
    }

    if (!currentLineText.empty())
    {
        LogF("📝 TEXT: %S", currentLineText.c_str());
        AttemptLearning(currentLineGlyphs, currentLineText);
    }

    g_TextBuffer.clear();
}

// ================= CONSTRUCTOR =================
HookedIDirect3DDevice9::HookedIDirect3DDevice9(IDirect3DDevice9* pReal)
    : m_pReal(pReal), m_refCount(1)
{
    ZeroMemory(m_texStages, sizeof(m_texStages));
    LoadFontMap();
    g_devicesCreated++;
    g_devicesAlive++;
    LogF("[DEVICE] Created (total=%d alive=%d)", g_devicesCreated, g_devicesAlive);
}

// ================= DESTRUCTOR =================
HookedIDirect3DDevice9::~HookedIDirect3DDevice9()
{
    SaveFontMap();
    for (int i = 0; i < 3; i++)
        if (m_texStages[i]) m_texStages[i]->Release();
    g_devicesAlive--;
    LogF("[DEVICE] Destroyed (alive=%d)", g_devicesAlive);
}

// ================= IUNKNOWN =================
HRESULT STDMETHODCALLTYPE HookedIDirect3DDevice9::QueryInterface(REFIID riid, void** ppv)
{
    if (!ppv) return E_POINTER;
    if (riid == __uuidof(IDirect3DDevice9) || riid == __uuidof(IUnknown)) {
        *ppv = this; AddRef(); return S_OK;
    }
    return m_pReal->QueryInterface(riid, ppv);
}

ULONG STDMETHODCALLTYPE HookedIDirect3DDevice9::AddRef()
{
    InterlockedIncrement(&m_refCount);
    return m_pReal->AddRef();
}

ULONG STDMETHODCALLTYPE HookedIDirect3DDevice9::Release()
{
    ULONG realRef = m_pReal->Release();
    LONG ref = InterlockedDecrement(&m_refCount);
    if (ref == 0) delete this;
    return realRef;
}

// ================= RESET =================
HRESULT STDMETHODCALLTYPE HookedIDirect3DDevice9::Reset(D3DPRESENT_PARAMETERS* p)
{
    g_resets++;
    for (int i = 0; i < 3; i++)
    {
        if (m_texStages[i]) m_texStages[i]->Release();
        m_texStages[i] = nullptr;
    }
    g_candidateHits = 0;
    LogF("[DEVICE] Reset (%d)", g_resets);
    return m_pReal->Reset(p);
}

// ================= PRESENT =================
HRESULT STDMETHODCALLTYPE HookedIDirect3DDevice9::Present(const RECT* a, const RECT* b, HWND c, const RGNDATA* d)
{
    FlushText();
    return m_pReal->Present(a, b, c, d);
}

// ================= SET TEXTURE =================
HRESULT STDMETHODCALLTYPE HookedIDirect3DDevice9::SetTexture(DWORD stage, IDirect3DBaseTexture9* tex)
{
    g_TEX++;
    if (stage < 3)
    {
        if (m_texStages[stage]) m_texStages[stage]->Release();
        m_texStages[stage] = tex;
        if (m_texStages[stage]) m_texStages[stage]->AddRef();
    }
    return m_pReal->SetTexture(stage, tex);
}

HRESULT STDMETHODCALLTYPE HookedIDirect3DDevice9::GetTexture(DWORD stage, IDirect3DBaseTexture9** tex)
{
    return m_pReal->GetTexture(stage, tex);
}

// ================= DRAW INDEXED PRIMITIVE =================
HRESULT STDMETHODCALLTYPE HookedIDirect3DDevice9::DrawIndexedPrimitive(
    D3DPRIMITIVETYPE type, INT base, UINT minIndex, UINT numVertices,
    UINT startIndex, UINT primCount)
{
    static thread_local bool inHook = false;
    if (inHook)
        return m_pReal->DrawIndexedPrimitive(type, base, minIndex, numVertices, startIndex, primCount);

    inHook = true;

    g_DIP++; g_drawCallsPerFrame++;

    if (type != D3DPT_TRIANGLELIST || primCount < 2 || primCount > 1000)
    {
        inHook = false;
        return m_pReal->DrawIndexedPrimitive(type, base, minIndex, numVertices, startIndex, primCount);
    }

    static int sample = 0;
    if ((sample++ % 3) != 0)
    {
        inHook = false;
        return m_pReal->DrawIndexedPrimitive(type, base, minIndex, numVertices, startIndex, primCount);
    }

    DWORD alpha = FALSE, alphaTest = FALSE;
    m_pReal->GetRenderState(D3DRS_ALPHABLENDENABLE, &alpha);
    m_pReal->GetRenderState(D3DRS_ALPHATESTENABLE, &alphaTest);

    bool alphaOK = alpha || alphaTest;
    if (!alphaOK)
    {
        inHook = false;
        return m_pReal->DrawIndexedPrimitive(type, base, minIndex, numVertices, startIndex, primCount);
    }

    IDirect3DVertexBuffer9* vb = nullptr;
    UINT stride = 0, offset = 0;

    if (FAILED(m_pReal->GetStreamSource(0, &vb, &offset, &stride)) || !vb)
    {
        inHook = false;
        return m_pReal->DrawIndexedPrimitive(type, base, minIndex, numVertices, startIndex, primCount);
    }

    if (stride < 16 || stride > 128)
    {
        vb->Release();
        inHook = false;
        return m_pReal->DrawIndexedPrimitive(type, base, minIndex, numVertices, startIndex, primCount);
    }

    BYTE* vbData = nullptr;
    if (FAILED(vb->Lock(0, 0, (void**)&vbData, D3DLOCK_READONLY)))
    {
        vb->Release();
        inHook = false;
        return m_pReal->DrawIndexedPrimitive(type, base, minIndex, numVertices, startIndex, primCount);
    }

    IDirect3DIndexBuffer9* ib = nullptr;
    BYTE* ibData = nullptr;
    D3DINDEXBUFFER_DESC ibDesc{};

    if (FAILED(m_pReal->GetIndices(&ib)) || !ib ||
        FAILED(ib->GetDesc(&ibDesc)) ||
        FAILED(ib->Lock(0, 0, (void**)&ibData, D3DLOCK_READONLY)))
    {
        if (ib) ib->Release();
        vb->Unlock();
        vb->Release();
        inHook = false;
        return m_pReal->DrawIndexedPrimitive(type, base, minIndex, numVertices, startIndex, primCount);
    }

    auto GetIndex = [&](UINT i) -> UINT
        {
            return (ibDesc.Format == D3DFMT_INDEX16)
                ? ((WORD*)ibData)[i]
                : ((DWORD*)ibData)[i];
        };

    int uvOffset = -1;
    int floatsPerVertex = stride / 4;

    IDirect3DVertexDeclaration9* decl = nullptr;
    if (SUCCEEDED(m_pReal->GetVertexDeclaration(&decl)) && decl)
    {
        D3DVERTEXELEMENT9 elems[32];
        UINT num = 32;

        if (SUCCEEDED(decl->GetDeclaration(elems, &num)))
        {
            for (UINT i = 0; i < num; i++)
            {
                if (elems[i].Usage == D3DDECLUSAGE_TEXCOORD && elems[i].UsageIndex == 0)
                {
                    uvOffset = elems[i].Offset / 4;
                    break;
                }
            }
        }
        decl->Release();
    }

    if (uvOffset < 0)
        uvOffset = (stride >= 24) ? (floatsPerVertex - 2) : 4;

    if (uvOffset < 0 || uvOffset + 1 >= floatsPerVertex)
    {
        ib->Unlock(); ib->Release();
        vb->Unlock(); vb->Release();
        inHook = false;
        return m_pReal->DrawIndexedPrimitive(type, base, minIndex, numVertices, startIndex, primCount);
    }

    UINT indexCount = primCount * 3;

    for (UINT i = 0; i + 2 < indexCount; i += 3)
    {
        UINT ids[3] = {
            GetIndex(startIndex + i + 0),
            GetIndex(startIndex + i + 1),
            GetIndex(startIndex + i + 2)
        };

        UINT unique[4];
        int uCount = 0;

        for (int k = 0; k < 3; k++)
        {
            bool found = false;
            for (int j = 0; j < uCount; j++)
                if (unique[j] == ids[k]) found = true;

            if (!found && uCount < 4)
                unique[uCount++] = ids[k];
        }

        if (uCount < 3) continue;

        VertexUV verts[4];

        for (int v = 0; v < uCount; v++)
        {
            UINT idx = unique[v] + base;
            if (idx >= numVertices) continue;

            BYTE* ptr = vbData + stride * idx;
            float* f = (float*)ptr;

            verts[v].x = f[0];
            verts[v].y = f[1];
            verts[v].z = f[2];
            verts[v].rhw = f[3];
            verts[v].u = f[uvOffset];
            verts[v].v = f[uvOffset + 1];
        }

        float w = (verts[0].x > verts[1].x) ? verts[0].x - verts[1].x : verts[1].x - verts[0].x;
        float h = (verts[0].y > verts[2].y) ? verts[0].y - verts[2].y : verts[2].y - verts[0].y;

        if (w < 2 || h < 2 || w > 800 || h > 400) continue;

        IDirect3DTexture9* tex = nullptr;
        GlyphUV glyph;
        ZeroMemory(&glyph, sizeof(glyph));

        for (int s = 0; s < 3; s++)
        {
            if (m_texStages[s] &&
                SUCCEEDED(m_texStages[s]->QueryInterface(__uuidof(IDirect3DTexture9), (void**)&tex)))
            {
                glyph = ProcessGlyph(tex, verts);
                if (glyph.width > 0) break;

                tex->Release();
                tex = nullptr;
            }
        }

        if (tex && glyph.width > 0)
        {
            float cx = 0, cy = 0;
            for (int v = 0; v < uCount; v++)
            {
                cx += verts[v].x;
                cy += verts[v].y;
            }

            AddGlyph(glyph.hash, cx / uCount, cy / uCount, glyph.height);
            tex->Release();
        }
    }

    ib->Unlock();
    ib->Release();

    vb->Unlock();
    vb->Release();

    inHook = false;

    return m_pReal->DrawIndexedPrimitive(type, base, minIndex, numVertices, startIndex, primCount);
}

// ================= DRAW PRIMITIVE =================
HRESULT STDMETHODCALLTYPE HookedIDirect3DDevice9::DrawPrimitive(D3DPRIMITIVETYPE type, UINT start, UINT count)
{
    g_DP++; g_drawCallsPerFrame++;
    if (count > g_maxPrim) g_maxPrim = count;
    return m_pReal->DrawPrimitive(type, start, count);
}

// ================= DRAW PRIMITIVE UP =================
HRESULT STDMETHODCALLTYPE HookedIDirect3DDevice9::DrawPrimitiveUP(
    D3DPRIMITIVETYPE type, UINT count, const void* vertices, UINT stride)
{
    g_DP++; g_drawCallsPerFrame++;
    return m_pReal->DrawPrimitiveUP(type, count, vertices, stride);
}

// ================= DRAW INDEXED PRIMITIVE UP =================
HRESULT STDMETHODCALLTYPE HookedIDirect3DDevice9::DrawIndexedPrimitiveUP(
    D3DPRIMITIVETYPE type, UINT min, UINT num, UINT count,
    const void* indices, D3DFORMAT idxFmt, const void* vertices, UINT stride)
{
    g_DIP++; g_drawCallsPerFrame++;
    return m_pReal->DrawIndexedPrimitiveUP(type, min, num, count, indices, idxFmt, vertices, stride);
}

// ================= SET FVF =================
HRESULT STDMETHODCALLTYPE HookedIDirect3DDevice9::SetFVF(DWORD fvf) { return m_pReal->SetFVF(fvf); }
HRESULT STDMETHODCALLTYPE HookedIDirect3DDevice9::GetFVF(DWORD* fvf) { return m_pReal->GetFVF(fvf); }

// ================= RENDER TARGET =================
HRESULT STDMETHODCALLTYPE HookedIDirect3DDevice9::SetRenderTarget(DWORD i, IDirect3DSurface9* p) { g_RT++; return m_pReal->SetRenderTarget(i, p); }
HRESULT STDMETHODCALLTYPE HookedIDirect3DDevice9::GetRenderTarget(DWORD i, IDirect3DSurface9** pp) { return m_pReal->GetRenderTarget(i, pp); }

// ================= DEPTH STENCIL =================
HRESULT STDMETHODCALLTYPE HookedIDirect3DDevice9::SetDepthStencilSurface(IDirect3DSurface9* p) { return m_pReal->SetDepthStencilSurface(p); }
HRESULT STDMETHODCALLTYPE HookedIDirect3DDevice9::GetDepthStencilSurface(IDirect3DSurface9** pp) { return m_pReal->GetDepthStencilSurface(pp); }

// ================= RENDER STATE =================
HRESULT STDMETHODCALLTYPE HookedIDirect3DDevice9::SetRenderState(D3DRENDERSTATETYPE s, DWORD v) { return m_pReal->SetRenderState(s, v); }
HRESULT STDMETHODCALLTYPE HookedIDirect3DDevice9::GetRenderState(D3DRENDERSTATETYPE s, DWORD* v) { return m_pReal->GetRenderState(s, v); }

// ================= TRANSFORM =================
HRESULT STDMETHODCALLTYPE HookedIDirect3DDevice9::SetTransform(D3DTRANSFORMSTATETYPE s, const D3DMATRIX* m) { return m_pReal->SetTransform(s, m); }
HRESULT STDMETHODCALLTYPE HookedIDirect3DDevice9::GetTransform(D3DTRANSFORMSTATETYPE s, D3DMATRIX* m) { return m_pReal->GetTransform(s, m); }
HRESULT STDMETHODCALLTYPE HookedIDirect3DDevice9::MultiplyTransform(D3DTRANSFORMSTATETYPE s, const D3DMATRIX* m) { return m_pReal->MultiplyTransform(s, m); }

// ================= VIEWPORT =================
HRESULT STDMETHODCALLTYPE HookedIDirect3DDevice9::SetViewport(const D3DVIEWPORT9* p) { return m_pReal->SetViewport(p); }
HRESULT STDMETHODCALLTYPE HookedIDirect3DDevice9::GetViewport(D3DVIEWPORT9* p) { return m_pReal->GetViewport(p); }

// ================= MATERIAL =================
HRESULT STDMETHODCALLTYPE HookedIDirect3DDevice9::SetMaterial(const D3DMATERIAL9* m) { return m_pReal->SetMaterial(m); }
HRESULT STDMETHODCALLTYPE HookedIDirect3DDevice9::GetMaterial(D3DMATERIAL9* m) { return m_pReal->GetMaterial(m); }

// ================= LIGHT =================
HRESULT STDMETHODCALLTYPE HookedIDirect3DDevice9::SetLight(DWORD i, const D3DLIGHT9* l) { return m_pReal->SetLight(i, l); }
HRESULT STDMETHODCALLTYPE HookedIDirect3DDevice9::GetLight(DWORD i, D3DLIGHT9* l) { return m_pReal->GetLight(i, l); }
HRESULT STDMETHODCALLTYPE HookedIDirect3DDevice9::LightEnable(DWORD i, BOOL e) { return m_pReal->LightEnable(i, e); }
HRESULT STDMETHODCALLTYPE HookedIDirect3DDevice9::GetLightEnable(DWORD i, BOOL* e) { return m_pReal->GetLightEnable(i, e); }

// ================= CLIP PLANE =================
HRESULT STDMETHODCALLTYPE HookedIDirect3DDevice9::SetClipPlane(DWORD i, const float* p) { return m_pReal->SetClipPlane(i, p); }
HRESULT STDMETHODCALLTYPE HookedIDirect3DDevice9::GetClipPlane(DWORD i, float* p) { return m_pReal->GetClipPlane(i, p); }

// ================= SCISSOR RECT =================
HRESULT STDMETHODCALLTYPE HookedIDirect3DDevice9::SetScissorRect(const RECT* r) { return m_pReal->SetScissorRect(r); }
HRESULT STDMETHODCALLTYPE HookedIDirect3DDevice9::GetScissorRect(RECT* r) { return m_pReal->GetScissorRect(r); }

// ================= SOFTWARE VERTEX PROCESSING =================
HRESULT STDMETHODCALLTYPE HookedIDirect3DDevice9::SetSoftwareVertexProcessing(BOOL b) { return m_pReal->SetSoftwareVertexProcessing(b); }
BOOL STDMETHODCALLTYPE HookedIDirect3DDevice9::GetSoftwareVertexProcessing() { return m_pReal->GetSoftwareVertexProcessing(); }

// ================= NPATCH =================
HRESULT STDMETHODCALLTYPE HookedIDirect3DDevice9::SetNPatchMode(float f) { return m_pReal->SetNPatchMode(f); }
float STDMETHODCALLTYPE HookedIDirect3DDevice9::GetNPatchMode() { return m_pReal->GetNPatchMode(); }

// ================= VERTEX SHADER =================
HRESULT STDMETHODCALLTYPE HookedIDirect3DDevice9::SetVertexShader(IDirect3DVertexShader9* vs) { g_VS++; return m_pReal->SetVertexShader(vs); }
HRESULT STDMETHODCALLTYPE HookedIDirect3DDevice9::GetVertexShader(IDirect3DVertexShader9** vs) { return m_pReal->GetVertexShader(vs); }
HRESULT STDMETHODCALLTYPE HookedIDirect3DDevice9::SetVertexShaderConstantF(UINT r, const float* c, UINT n) { return m_pReal->SetVertexShaderConstantF(r, c, n); }
HRESULT STDMETHODCALLTYPE HookedIDirect3DDevice9::GetVertexShaderConstantF(UINT r, float* c, UINT n) { return m_pReal->GetVertexShaderConstantF(r, c, n); }
HRESULT STDMETHODCALLTYPE HookedIDirect3DDevice9::SetVertexShaderConstantI(UINT r, const int* c, UINT n) { return m_pReal->SetVertexShaderConstantI(r, c, n); }
HRESULT STDMETHODCALLTYPE HookedIDirect3DDevice9::GetVertexShaderConstantI(UINT r, int* c, UINT n) { return m_pReal->GetVertexShaderConstantI(r, c, n); }
HRESULT STDMETHODCALLTYPE HookedIDirect3DDevice9::SetVertexShaderConstantB(UINT r, const BOOL* c, UINT n) { return m_pReal->SetVertexShaderConstantB(r, c, n); }
HRESULT STDMETHODCALLTYPE HookedIDirect3DDevice9::GetVertexShaderConstantB(UINT r, BOOL* c, UINT n) { return m_pReal->GetVertexShaderConstantB(r, c, n); }
HRESULT STDMETHODCALLTYPE HookedIDirect3DDevice9::CreateVertexShader(const DWORD* f, IDirect3DVertexShader9** s) { return m_pReal->CreateVertexShader(f, s); }

// ================= PIXEL SHADER =================
HRESULT STDMETHODCALLTYPE HookedIDirect3DDevice9::SetPixelShader(IDirect3DPixelShader9* ps) { g_PS++; return m_pReal->SetPixelShader(ps); }
HRESULT STDMETHODCALLTYPE HookedIDirect3DDevice9::GetPixelShader(IDirect3DPixelShader9** ps) { return m_pReal->GetPixelShader(ps); }
HRESULT STDMETHODCALLTYPE HookedIDirect3DDevice9::SetPixelShaderConstantF(UINT r, const float* c, UINT n) { return m_pReal->SetPixelShaderConstantF(r, c, n); }
HRESULT STDMETHODCALLTYPE HookedIDirect3DDevice9::GetPixelShaderConstantF(UINT r, float* c, UINT n) { return m_pReal->GetPixelShaderConstantF(r, c, n); }
HRESULT STDMETHODCALLTYPE HookedIDirect3DDevice9::SetPixelShaderConstantI(UINT r, const int* c, UINT n) { return m_pReal->SetPixelShaderConstantI(r, c, n); }
HRESULT STDMETHODCALLTYPE HookedIDirect3DDevice9::GetPixelShaderConstantI(UINT r, int* c, UINT n) { return m_pReal->GetPixelShaderConstantI(r, c, n); }
HRESULT STDMETHODCALLTYPE HookedIDirect3DDevice9::SetPixelShaderConstantB(UINT r, const BOOL* c, UINT n) { return m_pReal->SetPixelShaderConstantB(r, c, n); }
HRESULT STDMETHODCALLTYPE HookedIDirect3DDevice9::GetPixelShaderConstantB(UINT r, BOOL* c, UINT n) { return m_pReal->GetPixelShaderConstantB(r, c, n); }
HRESULT STDMETHODCALLTYPE HookedIDirect3DDevice9::CreatePixelShader(const DWORD* f, IDirect3DPixelShader9** s) { return m_pReal->CreatePixelShader(f, s); }

// ================= VERTEX DECLARATION =================
HRESULT STDMETHODCALLTYPE HookedIDirect3DDevice9::SetVertexDeclaration(IDirect3DVertexDeclaration9* d) { return m_pReal->SetVertexDeclaration(d); }
HRESULT STDMETHODCALLTYPE HookedIDirect3DDevice9::GetVertexDeclaration(IDirect3DVertexDeclaration9** d) { return m_pReal->GetVertexDeclaration(d); }
HRESULT STDMETHODCALLTYPE HookedIDirect3DDevice9::CreateVertexDeclaration(const D3DVERTEXELEMENT9* e, IDirect3DVertexDeclaration9** d) { return m_pReal->CreateVertexDeclaration(e, d); }

// ================= STREAM SOURCE =================
HRESULT STDMETHODCALLTYPE HookedIDirect3DDevice9::SetStreamSource(UINT n, IDirect3DVertexBuffer9* b, UINT o, UINT s) { g_STREAM++; return m_pReal->SetStreamSource(n, b, o, s); }
HRESULT STDMETHODCALLTYPE HookedIDirect3DDevice9::GetStreamSource(UINT n, IDirect3DVertexBuffer9** b, UINT* o, UINT* s) { return m_pReal->GetStreamSource(n, b, o, s); }
HRESULT STDMETHODCALLTYPE HookedIDirect3DDevice9::SetStreamSourceFreq(UINT n, UINT f) { return m_pReal->SetStreamSourceFreq(n, f); }
HRESULT STDMETHODCALLTYPE HookedIDirect3DDevice9::GetStreamSourceFreq(UINT n, UINT* f) { return m_pReal->GetStreamSourceFreq(n, f); }
HRESULT STDMETHODCALLTYPE HookedIDirect3DDevice9::SetIndices(IDirect3DIndexBuffer9* i) { g_IDX++; return m_pReal->SetIndices(i); }
HRESULT STDMETHODCALLTYPE HookedIDirect3DDevice9::GetIndices(IDirect3DIndexBuffer9** i) { return m_pReal->GetIndices(i); }
HRESULT STDMETHODCALLTYPE HookedIDirect3DDevice9::ProcessVertices(UINT a, UINT b, UINT c, IDirect3DVertexBuffer9* d, IDirect3DVertexDeclaration9* e, DWORD f) { return m_pReal->ProcessVertices(a, b, c, d, e, f); }

// ================= CREATION =================
HRESULT STDMETHODCALLTYPE HookedIDirect3DDevice9::CreateTexture(UINT w, UINT h, UINT l, DWORD u, D3DFORMAT f, D3DPOOL p, IDirect3DTexture9** t, HANDLE* sh) { return m_pReal->CreateTexture(w, h, l, u, f, p, t, sh); }
HRESULT STDMETHODCALLTYPE HookedIDirect3DDevice9::CreateVolumeTexture(UINT w, UINT h, UINT d, UINT l, DWORD u, D3DFORMAT f, D3DPOOL p, IDirect3DVolumeTexture9** v, HANDLE* sh) { return m_pReal->CreateVolumeTexture(w, h, d, l, u, f, p, v, sh); }
HRESULT STDMETHODCALLTYPE HookedIDirect3DDevice9::CreateCubeTexture(UINT e, UINT l, DWORD u, D3DFORMAT f, D3DPOOL p, IDirect3DCubeTexture9** c, HANDLE* sh) { return m_pReal->CreateCubeTexture(e, l, u, f, p, c, sh); }
HRESULT STDMETHODCALLTYPE HookedIDirect3DDevice9::CreateRenderTarget(UINT w, UINT h, D3DFORMAT f, D3DMULTISAMPLE_TYPE m, DWORD mq, BOOL l, IDirect3DSurface9** s, HANDLE* sh) { return m_pReal->CreateRenderTarget(w, h, f, m, mq, l, s, sh); }
HRESULT STDMETHODCALLTYPE HookedIDirect3DDevice9::CreateDepthStencilSurface(UINT w, UINT h, D3DFORMAT f, D3DMULTISAMPLE_TYPE m, DWORD mq, BOOL d, IDirect3DSurface9** s, HANDLE* sh) { return m_pReal->CreateDepthStencilSurface(w, h, f, m, mq, d, s, sh); }
HRESULT STDMETHODCALLTYPE HookedIDirect3DDevice9::CreateOffscreenPlainSurface(UINT w, UINT h, D3DFORMAT f, D3DPOOL p, IDirect3DSurface9** s, HANDLE* sh) { return m_pReal->CreateOffscreenPlainSurface(w, h, f, p, s, sh); }
HRESULT STDMETHODCALLTYPE HookedIDirect3DDevice9::CreateVertexBuffer(UINT l, DWORD u, DWORD f, D3DPOOL p, IDirect3DVertexBuffer9** b, HANDLE* sh) { return m_pReal->CreateVertexBuffer(l, u, f, p, b, sh); }
HRESULT STDMETHODCALLTYPE HookedIDirect3DDevice9::CreateIndexBuffer(UINT l, DWORD u, D3DFORMAT f, D3DPOOL p, IDirect3DIndexBuffer9** b, HANDLE* sh) { return m_pReal->CreateIndexBuffer(l, u, f, p, b, sh); }
HRESULT STDMETHODCALLTYPE HookedIDirect3DDevice9::CreateQuery(D3DQUERYTYPE t, IDirect3DQuery9** q) { return m_pReal->CreateQuery(t, q); }

// ================= STATE BLOCK =================
HRESULT STDMETHODCALLTYPE HookedIDirect3DDevice9::CreateStateBlock(D3DSTATEBLOCKTYPE t, IDirect3DStateBlock9** s) { return m_pReal->CreateStateBlock(t, s); }
HRESULT STDMETHODCALLTYPE HookedIDirect3DDevice9::BeginStateBlock() { return m_pReal->BeginStateBlock(); }
HRESULT STDMETHODCALLTYPE HookedIDirect3DDevice9::EndStateBlock(IDirect3DStateBlock9** s) { return m_pReal->EndStateBlock(s); }

// ================= SURFACE OPS =================
HRESULT STDMETHODCALLTYPE HookedIDirect3DDevice9::UpdateSurface(IDirect3DSurface9* src, const RECT* sr, IDirect3DSurface9* dst, const POINT* dp) { return m_pReal->UpdateSurface(src, sr, dst, dp); }
HRESULT STDMETHODCALLTYPE HookedIDirect3DDevice9::UpdateTexture(IDirect3DBaseTexture9* src, IDirect3DBaseTexture9* dst) { return m_pReal->UpdateTexture(src, dst); }
HRESULT STDMETHODCALLTYPE HookedIDirect3DDevice9::GetRenderTargetData(IDirect3DSurface9* rt, IDirect3DSurface9* dst) { return m_pReal->GetRenderTargetData(rt, dst); }
HRESULT STDMETHODCALLTYPE HookedIDirect3DDevice9::GetFrontBufferData(UINT i, IDirect3DSurface9* dst) { return m_pReal->GetFrontBufferData(i, dst); }
HRESULT STDMETHODCALLTYPE HookedIDirect3DDevice9::StretchRect(IDirect3DSurface9* src, const RECT* sr, IDirect3DSurface9* dst, const RECT* dr, D3DTEXTUREFILTERTYPE f) { return m_pReal->StretchRect(src, sr, dst, dr, f); }
HRESULT STDMETHODCALLTYPE HookedIDirect3DDevice9::ColorFill(IDirect3DSurface9* s, const RECT* r, D3DCOLOR c) { return m_pReal->ColorFill(s, r, c); }

// ================= TEXTURE STAGE =================
HRESULT STDMETHODCALLTYPE HookedIDirect3DDevice9::GetTextureStageState(DWORD s, D3DTEXTURESTAGESTATETYPE t, DWORD* v) { return m_pReal->GetTextureStageState(s, t, v); }
HRESULT STDMETHODCALLTYPE HookedIDirect3DDevice9::SetTextureStageState(DWORD s, D3DTEXTURESTAGESTATETYPE t, DWORD v) { return m_pReal->SetTextureStageState(s, t, v); }
HRESULT STDMETHODCALLTYPE HookedIDirect3DDevice9::GetSamplerState(DWORD s, D3DSAMPLERSTATETYPE t, DWORD* v) { return m_pReal->GetSamplerState(s, t, v); }
HRESULT STDMETHODCALLTYPE HookedIDirect3DDevice9::SetSamplerState(DWORD s, D3DSAMPLERSTATETYPE t, DWORD v) { return m_pReal->SetSamplerState(s, t, v); }

// ================= CLIP =================
HRESULT STDMETHODCALLTYPE HookedIDirect3DDevice9::SetClipStatus(const D3DCLIPSTATUS9* c) { return m_pReal->SetClipStatus(c); }
HRESULT STDMETHODCALLTYPE HookedIDirect3DDevice9::GetClipStatus(D3DCLIPSTATUS9* c) { return m_pReal->GetClipStatus(c); }

// ================= CURSOR =================
HRESULT STDMETHODCALLTYPE HookedIDirect3DDevice9::SetCursorProperties(UINT x, UINT y, IDirect3DSurface9* s) { return m_pReal->SetCursorProperties(x, y, s); }
void STDMETHODCALLTYPE HookedIDirect3DDevice9::SetCursorPosition(int x, int y, DWORD f) { m_pReal->SetCursorPosition(x, y, f); }
BOOL STDMETHODCALLTYPE HookedIDirect3DDevice9::ShowCursor(BOOL b) { return m_pReal->ShowCursor(b); }

// ================= GAMMA =================
void STDMETHODCALLTYPE HookedIDirect3DDevice9::SetGammaRamp(UINT i, DWORD f, const D3DGAMMARAMP* g) { m_pReal->SetGammaRamp(i, f, g); }
void STDMETHODCALLTYPE HookedIDirect3DDevice9::GetGammaRamp(UINT i, D3DGAMMARAMP* g) { m_pReal->GetGammaRamp(i, g); }

// ================= PALETTE =================
HRESULT STDMETHODCALLTYPE HookedIDirect3DDevice9::SetPaletteEntries(UINT n, const PALETTEENTRY* e) { return m_pReal->SetPaletteEntries(n, e); }
HRESULT STDMETHODCALLTYPE HookedIDirect3DDevice9::GetPaletteEntries(UINT n, PALETTEENTRY* e) { return m_pReal->GetPaletteEntries(n, e); }
HRESULT STDMETHODCALLTYPE HookedIDirect3DDevice9::SetCurrentTexturePalette(UINT n) { return m_pReal->SetCurrentTexturePalette(n); }
HRESULT STDMETHODCALLTYPE HookedIDirect3DDevice9::GetCurrentTexturePalette(UINT* n) { return m_pReal->GetCurrentTexturePalette(n); }

// ================= SWAP CHAIN =================
HRESULT STDMETHODCALLTYPE HookedIDirect3DDevice9::CreateAdditionalSwapChain(D3DPRESENT_PARAMETERS* p, IDirect3DSwapChain9** s) { return m_pReal->CreateAdditionalSwapChain(p, s); }
HRESULT STDMETHODCALLTYPE HookedIDirect3DDevice9::GetSwapChain(UINT i, IDirect3DSwapChain9** s) { return m_pReal->GetSwapChain(i, s); }
UINT STDMETHODCALLTYPE HookedIDirect3DDevice9::GetNumberOfSwapChains() { return m_pReal->GetNumberOfSwapChains(); }

// ================= BACK BUFFER =================
HRESULT STDMETHODCALLTYPE HookedIDirect3DDevice9::GetBackBuffer(UINT i, UINT b, D3DBACKBUFFER_TYPE t, IDirect3DSurface9** s) { return m_pReal->GetBackBuffer(i, b, t, s); }
HRESULT STDMETHODCALLTYPE HookedIDirect3DDevice9::GetRasterStatus(UINT i, D3DRASTER_STATUS* s) { return m_pReal->GetRasterStatus(i, s); }

// ================= SCENE =================
HRESULT STDMETHODCALLTYPE HookedIDirect3DDevice9::BeginScene() { return m_pReal->BeginScene(); }
HRESULT STDMETHODCALLTYPE HookedIDirect3DDevice9::EndScene() { return m_pReal->EndScene(); }
HRESULT STDMETHODCALLTYPE HookedIDirect3DDevice9::Clear(DWORD a, const D3DRECT* b, DWORD c, D3DCOLOR d, float e, DWORD f) { return m_pReal->Clear(a, b, c, d, e, f); }

// ================= CAPS =================
HRESULT STDMETHODCALLTYPE HookedIDirect3DDevice9::TestCooperativeLevel() { return m_pReal->TestCooperativeLevel(); }
UINT STDMETHODCALLTYPE HookedIDirect3DDevice9::GetAvailableTextureMem() { return m_pReal->GetAvailableTextureMem(); }
HRESULT STDMETHODCALLTYPE HookedIDirect3DDevice9::EvictManagedResources() { return m_pReal->EvictManagedResources(); }
HRESULT STDMETHODCALLTYPE HookedIDirect3DDevice9::GetDeviceCaps(D3DCAPS9* c) { return m_pReal->GetDeviceCaps(c); }
HRESULT STDMETHODCALLTYPE HookedIDirect3DDevice9::GetDirect3D(IDirect3D9** d) { return m_pReal->GetDirect3D(d); }
HRESULT STDMETHODCALLTYPE HookedIDirect3DDevice9::GetCreationParameters(D3DDEVICE_CREATION_PARAMETERS* p) { return m_pReal->GetCreationParameters(p); }
HRESULT STDMETHODCALLTYPE HookedIDirect3DDevice9::GetDisplayMode(UINT i, D3DDISPLAYMODE* m) { return m_pReal->GetDisplayMode(i, m); }
HRESULT STDMETHODCALLTYPE HookedIDirect3DDevice9::ValidateDevice(DWORD* p) { return m_pReal->ValidateDevice(p); }
HRESULT STDMETHODCALLTYPE HookedIDirect3DDevice9::SetDialogBoxMode(BOOL b) { return m_pReal->SetDialogBoxMode(b); }

// ================= PATCH =================
HRESULT STDMETHODCALLTYPE HookedIDirect3DDevice9::DrawRectPatch(UINT h, const float* n, const D3DRECTPATCH_INFO* i) { return m_pReal->DrawRectPatch(h, n, i); }
HRESULT STDMETHODCALLTYPE HookedIDirect3DDevice9::DrawTriPatch(UINT h, const float* n, const D3DTRIPATCH_INFO* i) { return m_pReal->DrawTriPatch(h, n, i); }
HRESULT STDMETHODCALLTYPE HookedIDirect3DDevice9::DeletePatch(UINT h) { return m_pReal->DeletePatch(h); }