#define _CRT_SECURE_NO_WARNINGS
#define NOMINMAX
#include "Direct3DDeviceProxy.h"
#include "Shared.h"
#include <vector>
#include <unordered_map>
#include <cmath>
#include <algorithm>

extern void SyncBuffers();
extern void PushGlyph(uint32_t hash, float cx, float cy, float w, float h);
extern void UpdateStats(int pushCount);

struct HookGuard {
    bool& flag;
    HookGuard(bool& f) : flag(f) { flag = true; }
    ~HookGuard() { flag = false; }
};

static std::vector<GlyphInstance> g_TextBuffer;
static int g_debugCallCount = 0;
static int g_debugPushCount = 0;

// ================== STRUCTURES ==================
struct FrameGlyph {
    float cx, cy, w, h;
    uint32_t hash;
};

struct DrawCandidate {
    std::vector<FrameGlyph> glyphs;
    int glyphCount = 0;
};

enum AtlasType {
    ATLAS_ARIAL,
    ATLAS_BODY,
    ATLAS_TITLE
};

// ================== GLOBALS ==================
static std::vector<DrawCandidate> g_frameCandidates;   // يكتب فيه DIP
static std::vector<DrawCandidate> g_presentCandidates; // يقرأ منه Present
static int g_discardCount = 0;
static AtlasType g_currentAtlasType = ATLAS_ARIAL;
static int g_atlasWidth = 0, g_atlasHeight = 0;
static std::unordered_map<uint32_t, float> g_temporalFreq;
static std::unordered_map<uint32_t, bool> g_stableGlyphs;
static float g_uvVariance = 0.0f;
static int g_uvSampleCount = 0;
static int g_emptyFrames = 0;
static int g_sessionId = 0;
static int g_prevGlyphCount = 0;
static bool g_debugMode = false;  // Production mode

// ================== Stable Candidate Selection ==================
static std::unordered_map<int, int> g_primFreq;
static int g_selectedPrim = 0;

static inline uint32_t Q(float v, AtlasType type)
{
    switch (type)
    {
        case ATLAS_ARIAL: return (uint32_t)(v * 64.0f);
        case ATLAS_BODY:  return (uint32_t)(v * 128.0f);
        case ATLAS_TITLE: return (uint32_t)(v * 32.0f);
        default: return (uint32_t)(v * 64.0f);
    }
}

static void UpdateAtlasTypeByUV(float wUV, float hUV)
{
    if (g_uvSampleCount < 50)
    {
        g_uvVariance += (wUV + hUV) * 0.5f;
        g_uvSampleCount++;
    }
    else if (g_uvSampleCount == 50)
    {
        g_uvVariance /= 50.0f;
        if (g_uvVariance < 0.03f)
            g_currentAtlasType = ATLAS_BODY;
        else
            g_currentAtlasType = ATLAS_ARIAL;
        g_uvSampleCount++;
        LogF("[ATLAS] Detected type: %s (variance=%.4f)",
             g_currentAtlasType == ATLAS_BODY ? "BODY" : "ARIAL", g_uvVariance);
    }
}

bool HookedIDirect3DDevice9::IsLikelyTextTexture(IDirect3DBaseTexture9* tex)
{
    if (!tex) return false;
    IDirect3DTexture9* t = nullptr;
    if (FAILED(tex->QueryInterface(__uuidof(IDirect3DTexture9), (void**)&t)))
        return false;
    D3DSURFACE_DESC d{};
    t->GetLevelDesc(0, &d);
    t->Release();
    return (d.Format == D3DFMT_A8R8G8B8 || d.Format == D3DFMT_A4R4G4B4);
}

bool HookedIDirect3DDevice9::HasTextTexture()
{
    for (int i = 0; i < 3; i++)
        if (m_texStages[i] && IsLikelyTextTexture(m_texStages[i]))
            return true;
    return false;
}

wchar_t HookedIDirect3DDevice9::GetCharFromHash(uint32_t hash) { return L'?'; }
void HookedIDirect3DDevice9::AddGlyph(uint32_t hash, float x, float y, int h) {}
float HookedIDirect3DDevice9::CompareStrings(const std::wstring& a, const std::wstring& b) { return 0.0f; }
void HookedIDirect3DDevice9::AttemptLearning(const std::vector<DetectedGlyph>& glyphs, const std::wstring& text) {}
void HookedIDirect3DDevice9::SaveFontMap() {}
void HookedIDirect3DDevice9::LoadFontMap() {}
void HookedIDirect3DDevice9::FlushText() { g_TextBuffer.clear(); }

// ================== CONSTRUCTOR / DESTRUCTOR ==================
HookedIDirect3DDevice9::HookedIDirect3DDevice9(IDirect3DDevice9* pReal)
    : m_pReal(pReal), m_refCount(1)
{
    ZeroMemory(m_texStages, sizeof(m_texStages));
    g_devicesCreated++;
    g_devicesAlive++;
    LogF("[DEVICE] Created (total=%d alive=%d)", g_devicesCreated, g_devicesAlive);
}

HookedIDirect3DDevice9::~HookedIDirect3DDevice9()
{
    for (int i = 0; i < 3; i++)
        if (m_texStages[i]) m_texStages[i]->Release();
    g_devicesAlive--;
    LogF("[DEVICE] Destroyed (alive=%d)", g_devicesAlive);
}

// ================== IUNKNOWN ==================
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
    if (InterlockedDecrement(&m_refCount) == 0)
        delete this;
    return realRef;
}

// ================== RESET ==================
HRESULT STDMETHODCALLTYPE HookedIDirect3DDevice9::Reset(D3DPRESENT_PARAMETERS* p)
{
    g_resets++;
    for (int i = 0; i < 3; i++) {
        if (m_texStages[i]) m_texStages[i]->Release();
        m_texStages[i] = nullptr;
    }

    g_frameCandidates.clear();
    g_presentCandidates.clear();
    g_discardCount = 0;
    g_currentAtlasType = ATLAS_ARIAL;
    g_temporalFreq.clear();
    g_stableGlyphs.clear();
    g_uvVariance = 0.0f;
    g_uvSampleCount = 0;
    g_emptyFrames = 0;
    g_sessionId = 0;
    g_prevGlyphCount = 0;
    g_selectedPrim = 0;
    g_primFreq.clear();

    return m_pReal->Reset(p);
}

// ================== PRESENT ==================
HRESULT STDMETHODCALLTYPE HookedIDirect3DDevice9::Present(
    const RECT* pSourceRect,
    const RECT* pDestRect,
    HWND hDestWindowOverride,
    const RGNDATA* pDirtyRegion)
{
    // 🔥 Swap buffers (حل Frame Boundary Bug)
    g_presentCandidates.swap(g_frameCandidates);
    g_frameCandidates.clear();
    
    LogF("[DEBUG] Candidates this frame: %d", (int)g_presentCandidates.size());

    // ================== Stable Candidate Selection ==================
    int bestCount = 0;
    int bestPrim = 0;

    for (auto& it : g_primFreq)
    {
        if (it.second > bestCount)
        {
            bestCount = it.second;
            bestPrim = it.first;
        }
    }

    if (bestCount > 0 && bestPrim != g_selectedPrim)
    {
        g_selectedPrim = bestPrim;
        LogF("[PRIM] Selected stable PrimitiveCount: %d (frequency: %d)", bestPrim, bestCount);
    }

    g_primFreq.clear();

    // ================== Session Detection ==================
    int totalGlyphsBefore = 0;
    for (auto& candidate : g_presentCandidates)
        totalGlyphsBefore += candidate.glyphCount;

    if (totalGlyphsBefore == 0)
    {
        g_emptyFrames++;
    }
    else
    {
        bool newSession = false;

        if (g_emptyFrames > 60)
            newSession = true;
        else if (g_prevGlyphCount > 0 && totalGlyphsBefore > 0)
        {
            float ratio = (float)totalGlyphsBefore / (float)g_prevGlyphCount;
            if (ratio < 0.3f || ratio > 3.0f)
                newSession = true;
        }

        if (newSession)
        {
            g_sessionId++;
            LogF("[SESSION] New session #%d (prev=%d curr=%d empty=%d)",
                 g_sessionId, g_prevGlyphCount, totalGlyphsBefore, g_emptyFrames);
            g_temporalFreq.clear();
            g_stableGlyphs.clear();
        }
        g_emptyFrames = 0;
    }
    g_prevGlyphCount = totalGlyphsBefore;

    // ================== Process Top Candidates ==================
    if (!g_presentCandidates.empty())
    {
        std::sort(g_presentCandidates.begin(), g_presentCandidates.end(),
            [](const DrawCandidate& a, const DrawCandidate& b) {
                return a.glyphCount > b.glyphCount;
            });

        if (g_presentCandidates.size() > 3)
            g_presentCandidates.resize(3);

        std::vector<FrameGlyph> mergedGlyphs;
        for (auto& candidate : g_presentCandidates)
        {
            mergedGlyphs.insert(mergedGlyphs.end(),
                candidate.glyphs.begin(),
                candidate.glyphs.end());
        }

        std::sort(mergedGlyphs.begin(), mergedGlyphs.end(),
            [](const FrameGlyph& a, const FrameGlyph& b) {
                float threshold = (a.h < b.h) ? a.h * 0.3f : b.h * 0.3f;
                float dy = fabs(a.cy - b.cy);
                if (dy > threshold)
                    return a.cy < b.cy;
                return a.cx < b.cx;
            });

        int totalGlyphs = (int)mergedGlyphs.size();
        LogF("[BEST] Session=%d DrawCalls=%d TotalGlyphs=%d Discard=%d",
             g_sessionId, (int)g_presentCandidates.size(), totalGlyphs, g_discardCount);

        if (totalGlyphs > 5)
        {
            LogF("[TEXT-LINE] Session %d: text block detected (%d glyphs)", g_sessionId, totalGlyphs);
        }

        // Deduplication
        std::vector<FrameGlyph> finalGlyphs;
        for (auto& g : mergedGlyphs)
        {
            bool duplicate = false;
            for (auto& f : finalGlyphs)
            {
                if (g.hash == f.hash &&
                    fabs(g.cx - f.cx) < 1.0f &&
                    fabs(g.cy - f.cy) < 1.0f)
                {
                    duplicate = true;
                    break;
                }
            }
            if (!duplicate)
                finalGlyphs.push_back(g);
        }

        int uniqueCount = (int)finalGlyphs.size();
        LogF("[TEST] Sending %d unique glyphs to ASI (was %d)", uniqueCount, totalGlyphs);

        for (auto& g : finalGlyphs)
        {
            PushGlyph(g.hash, g.cx, g.cy, g.w, g.h);
        }

        g_presentCandidates.clear();
        g_discardCount = 0;
    }

    // Soft Decay for temporalFreq
    for (auto it = g_temporalFreq.begin(); it != g_temporalFreq.end(); )
    {
        it->second -= 0.5f;
        if (it->second < -3.0f)
            it = g_temporalFreq.erase(it);
        else
            ++it;
    }

    for (auto it = g_stableGlyphs.begin(); it != g_stableGlyphs.end(); )
    {
        if (g_temporalFreq.find(it->first) == g_temporalFreq.end())
            it = g_stableGlyphs.erase(it);
        else
            ++it;
    }

    SyncBuffers();
    return m_pReal->Present(pSourceRect, pDestRect, hDestWindowOverride, pDirtyRegion);
}

// ================== SET TEXTURE ==================
HRESULT STDMETHODCALLTYPE HookedIDirect3DDevice9::SetTexture(DWORD stage, IDirect3DBaseTexture9* tex)
{
    g_TEX++;
    if (stage < 3) {
        if (m_texStages[stage]) m_texStages[stage]->Release();
        m_texStages[stage] = tex;
        if (tex) tex->AddRef();

        if (tex) {
            IDirect3DTexture9* t = nullptr;
            if (SUCCEEDED(tex->QueryInterface(__uuidof(IDirect3DTexture9), (void**)&t))) {
                D3DSURFACE_DESC d{};
                t->GetLevelDesc(0, &d);
                g_atlasWidth = d.Width;
                g_atlasHeight = d.Height;

                if (d.Width == 512 && d.Height == 512)
                    g_currentAtlasType = ATLAS_TITLE;
                else if (d.Width == 256 && d.Height == 256)
                    g_currentAtlasType = ATLAS_BODY;
                else
                    g_currentAtlasType = ATLAS_ARIAL;
                t->Release();
            }
        }
    }
    return m_pReal->SetTexture(stage, tex);
}

HRESULT STDMETHODCALLTYPE HookedIDirect3DDevice9::GetTexture(DWORD stage, IDirect3DBaseTexture9** tex)
{
    return m_pReal->GetTexture(stage, tex);
}

// ================== DRAW INDEXED PRIMITIVE ==================
HRESULT STDMETHODCALLTYPE HookedIDirect3DDevice9::DrawIndexedPrimitive(
    D3DPRIMITIVETYPE Type,
    INT BaseVertexIndex,
    UINT MinVertexIndex,
    UINT NumVertices,
    UINT StartIndex,
    UINT PrimitiveCount)
{
    static thread_local bool inHook = false;
    if (inHook)
        return m_pReal->DrawIndexedPrimitive(Type, BaseVertexIndex, MinVertexIndex, NumVertices, StartIndex, PrimitiveCount);

    HookGuard guard(inHook);
    g_debugCallCount++;

    if (Type != D3DPT_TRIANGLELIST || !HasTextTexture() || NumVertices < 4)
        return m_pReal->DrawIndexedPrimitive(Type, BaseVertexIndex, MinVertexIndex, NumVertices, StartIndex, PrimitiveCount);

    // ================== Stable Candidate Registration ==================
    bool isCandidateRange = (PrimitiveCount > 20 && PrimitiveCount < 200);

    if (isCandidateRange)
        g_primFreq[PrimitiveCount]++;

    // 🎯 Phase 1: قبل الاستقرار → خذ أي مرشح
    if (g_selectedPrim == 0)
    {
        if (!isCandidateRange)
            return m_pReal->DrawIndexedPrimitive(Type, BaseVertexIndex, MinVertexIndex, NumVertices, StartIndex, PrimitiveCount);
    }
    else
    {
        // 🎯 Phase 2: بعد الاستقرار → خذ فقط الأفضل
        if (PrimitiveCount != g_selectedPrim)
            return m_pReal->DrawIndexedPrimitive(Type, BaseVertexIndex, MinVertexIndex, NumVertices, StartIndex, PrimitiveCount);
    }

    // ================== Vertex Declaration ==================
    IDirect3DVertexDeclaration9* decl = nullptr;
    if (FAILED(m_pReal->GetVertexDeclaration(&decl)) || !decl)
        return m_pReal->DrawIndexedPrimitive(Type, BaseVertexIndex, MinVertexIndex, NumVertices, StartIndex, PrimitiveCount);

    D3DVERTEXELEMENT9 elems[64];
    UINT num = 64;
    int posOffset = -1, uvOffset = -1;

    if (SUCCEEDED(decl->GetDeclaration(elems, &num)))
    {
        for (UINT i = 0; i < num; i++)
        {
            if (elems[i].Usage == D3DDECLUSAGE_POSITION || elems[i].Usage == D3DDECLUSAGE_POSITIONT)
                posOffset = elems[i].Offset;
            if (elems[i].Usage == D3DDECLUSAGE_TEXCOORD && elems[i].UsageIndex == 0)
                uvOffset = elems[i].Offset;
        }
    }
    decl->Release();
    if (posOffset < 0 || uvOffset < 0)
        return m_pReal->DrawIndexedPrimitive(Type, BaseVertexIndex, MinVertexIndex, NumVertices, StartIndex, PrimitiveCount);

    // ================== Logging ==================
    static int drawCallLogCounter = 0;
    if (++drawCallLogCounter % 500 == 1)
    {
        IDirect3DVertexBuffer9* tempVb = nullptr;
        UINT tempStride = 0, tempOffset = 0;
        m_pReal->GetStreamSource(0, &tempVb, &tempOffset, &tempStride);
        LogF("[LAYOUT] posOffset=%d uvOffset=%d stride=%d PrimCount=%d NumVerts=%d",
             posOffset, uvOffset, tempStride, PrimitiveCount, NumVertices);
        if (tempVb) tempVb->Release();
    }

    // ================== Vertex / Index Buffers ==================
    IDirect3DVertexBuffer9* vb = nullptr;
    UINT stride = 0, streamOffset = 0;
    if (FAILED(m_pReal->GetStreamSource(0, &vb, &streamOffset, &stride)) || !vb)
        return m_pReal->DrawIndexedPrimitive(Type, BaseVertexIndex, MinVertexIndex, NumVertices, StartIndex, PrimitiveCount);

    IDirect3DIndexBuffer9* ib = nullptr;
    if (FAILED(m_pReal->GetIndices(&ib)) || !ib)
    {
        vb->Release();
        return m_pReal->DrawIndexedPrimitive(Type, BaseVertexIndex, MinVertexIndex, NumVertices, StartIndex, PrimitiveCount);
    }

    D3DVERTEXBUFFER_DESC vbDesc;
    vb->GetDesc(&vbDesc);

    D3DINDEXBUFFER_DESC ibDesc;
    ib->GetDesc(&ibDesc);

    BYTE* vbData = nullptr;
    BYTE* ibData = nullptr;

    if (FAILED(vb->Lock(0, 0, (void**)&vbData, D3DLOCK_READONLY)) ||
        FAILED(ib->Lock(0, 0, (void**)&ibData, D3DLOCK_READONLY)))
    {
        if (vbData) vb->Unlock();
        if (ibData) ib->Unlock();
        ib->Release(); vb->Release();
        return m_pReal->DrawIndexedPrimitive(Type, BaseVertexIndex, MinVertexIndex, NumVertices, StartIndex, PrimitiveCount);
    }

    UINT indexSize = (ibDesc.Format == D3DFMT_INDEX16) ? 2 : 4;
    UINT maxIndex = ibDesc.Size / indexSize;

    if (StartIndex >= maxIndex || maxIndex == 0)
    {
        ib->Unlock(); vb->Unlock();
        ib->Release(); vb->Release();
        return m_pReal->DrawIndexedPrimitive(Type, BaseVertexIndex, MinVertexIndex, NumVertices, StartIndex, PrimitiveCount);
    }

    UINT indexCount = PrimitiveCount * 3;
    UINT safeIndexCount = std::min(indexCount, maxIndex - StartIndex);

    static std::unordered_map<uint32_t, int> frameFreq;
    frameFreq.clear();

    DrawCandidate candidate;
    candidate.glyphCount = 0;

    for (UINT i = 0; i + 5 < safeIndexCount; i += 6)
    {
        float minX = 1e9f, maxX = -1e9f, minY = 1e9f, maxY = -1e9f;
        float minU = 1e9f, maxU = -1e9f, minV = 1e9f, maxV = -1e9f;
        bool valid = false;

        for (int k = 0; k < 6; k++)
        {
            UINT idx = StartIndex + i + k;
            if (idx >= maxIndex) continue;

            UINT vi = (indexSize == 2) ? ((WORD*)ibData)[idx] : ((DWORD*)ibData)[idx];
            vi += BaseVertexIndex;

            if (vi * stride >= vbDesc.Size) continue;

            BYTE* v = vbData + streamOffset + vi * stride;
            float* pos = (float*)(v + posOffset);
            float* uv = (float*)(v + uvOffset);

            valid = true;

            if (pos[0] < minX) minX = pos[0];
            if (pos[0] > maxX) maxX = pos[0];
            if (pos[1] < minY) minY = pos[1];
            if (pos[1] > maxY) maxY = pos[1];
            if (uv[0] < minU) minU = uv[0];
            if (uv[0] > maxU) maxU = uv[0];
            if (uv[1] < minV) minV = uv[1];
            if (uv[1] > maxV) maxV = uv[1];
        }

        if (!valid) continue;

        float w = maxX - minX;
        float h = maxY - minY;
        float wUV = maxU - minU;
        float hUV = maxV - minV;

        UpdateAtlasTypeByUV(wUV, hUV);

        float minSize = (g_currentAtlasType == ATLAS_TITLE) ? 0.2f : ((g_currentAtlasType == ATLAS_BODY) ? 0.1f : 0.05f);
        if (w < minSize || h < minSize) { g_discardCount++; continue; }

        if (wUV < 0.0001f || hUV < 0.0001f) { g_discardCount++; continue; }
        float uvMax = (g_currentAtlasType == ATLAS_TITLE) ? 0.98f : 0.99f;
        if (wUV > uvMax || hUV > uvMax) { g_discardCount++; continue; }

        float cx = (minX + maxX) * 0.5f;
        float cy = (minY + maxY) * 0.5f;

        float uC = (minU + maxU) * 0.5f;
        float vC = (minV + maxV) * 0.5f;

        uint32_t hash = 2166136261u;
        hash = (hash * 16777619) ^ Q(uC, g_currentAtlasType);
        hash = (hash * 16777619) ^ Q(vC, g_currentAtlasType);
        hash = (hash * 16777619) ^ Q(w, g_currentAtlasType);
        hash = (hash * 16777619) ^ Q(h, g_currentAtlasType);

        uint32_t texSig = (g_atlasWidth << 16) | g_atlasHeight;
        hash ^= texSig;

        frameFreq[hash]++;
        if (frameFreq[hash] > 50) { g_discardCount++; continue; }

        float& freq = g_temporalFreq[hash];
        freq += 2.0f;

        // 🧠 Warmup Phase (أول 2 ثواني تقريباً أو أول 50 glyph فريد)
        bool warmup = (g_sessionId < 2 || g_temporalFreq.size() < 50);

        if (!warmup)
        {
            if (freq > 2.5f)
                g_stableGlyphs[hash] = true;
            if (!g_stableGlyphs[hash])
                continue;
        }

        if (freq > 100.0f) freq = 100.0f;

        candidate.glyphs.push_back({ cx, cy, w, h, hash });
        candidate.glyphCount++;

        if (candidate.glyphCount <= 50)
            LogF("[DIAG] PUSH #%d: hash=0x%08X pos(%.1f,%.1f) size(%.1fx%.1f)",
                 candidate.glyphCount, hash, cx, cy, w, h);
    }

    if (candidate.glyphCount > 0)
    {
        LogF("[CANDIDATE] PUSHED: %d glyphs", candidate.glyphCount);
        g_frameCandidates.push_back(std::move(candidate));
    }
    else
    {
        LogF("[CANDIDATE] EMPTY");
    }

    ib->Unlock();
    vb->Unlock();
    ib->Release();
    vb->Release();

    return m_pReal->DrawIndexedPrimitive(Type, BaseVertexIndex, MinVertexIndex, NumVertices, StartIndex, PrimitiveCount);
}

// ================== DRAW PRIMITIVE ==================
HRESULT STDMETHODCALLTYPE HookedIDirect3DDevice9::DrawPrimitive(D3DPRIMITIVETYPE type, UINT start, UINT count)
{
    g_DP++;
    g_drawCallsPerFrame++;
    if (count > g_maxPrim) g_maxPrim = count;
    return m_pReal->DrawPrimitive(type, start, count);
}

HRESULT STDMETHODCALLTYPE HookedIDirect3DDevice9::DrawPrimitiveUP(
    D3DPRIMITIVETYPE type, UINT count, const void* vertices, UINT stride)
{
    g_DP++;
    g_drawCallsPerFrame++;
    return m_pReal->DrawPrimitiveUP(type, count, vertices, stride);
}

HRESULT STDMETHODCALLTYPE HookedIDirect3DDevice9::DrawIndexedPrimitiveUP(
    D3DPRIMITIVETYPE type, UINT min, UINT num, UINT count,
    const void* indices, D3DFORMAT idxFmt, const void* vertices, UINT stride)
{
    g_DIP++;
    g_drawCallsPerFrame++;
    return m_pReal->DrawIndexedPrimitiveUP(type, min, num, count, indices, idxFmt, vertices, stride);
}

// ================== SET FVF ==================
HRESULT STDMETHODCALLTYPE HookedIDirect3DDevice9::SetFVF(DWORD fvf) { return m_pReal->SetFVF(fvf); }
HRESULT STDMETHODCALLTYPE HookedIDirect3DDevice9::GetFVF(DWORD* fvf) { return m_pReal->GetFVF(fvf); }

// ================== RENDER TARGET ==================
HRESULT STDMETHODCALLTYPE HookedIDirect3DDevice9::SetRenderTarget(DWORD i, IDirect3DSurface9* p) { g_RT++; return m_pReal->SetRenderTarget(i, p); }
HRESULT STDMETHODCALLTYPE HookedIDirect3DDevice9::GetRenderTarget(DWORD i, IDirect3DSurface9** pp) { return m_pReal->GetRenderTarget(i, pp); }

// ================== DEPTH STENCIL ==================
HRESULT STDMETHODCALLTYPE HookedIDirect3DDevice9::SetDepthStencilSurface(IDirect3DSurface9* p) { return m_pReal->SetDepthStencilSurface(p); }
HRESULT STDMETHODCALLTYPE HookedIDirect3DDevice9::GetDepthStencilSurface(IDirect3DSurface9** pp) { return m_pReal->GetDepthStencilSurface(pp); }

// ================== RENDER STATE ==================
HRESULT STDMETHODCALLTYPE HookedIDirect3DDevice9::SetRenderState(D3DRENDERSTATETYPE s, DWORD v) { return m_pReal->SetRenderState(s, v); }
HRESULT STDMETHODCALLTYPE HookedIDirect3DDevice9::GetRenderState(D3DRENDERSTATETYPE s, DWORD* v) { return m_pReal->GetRenderState(s, v); }

// ================== TRANSFORM ==================
HRESULT STDMETHODCALLTYPE HookedIDirect3DDevice9::SetTransform(D3DTRANSFORMSTATETYPE s, const D3DMATRIX* m) { return m_pReal->SetTransform(s, m); }
HRESULT STDMETHODCALLTYPE HookedIDirect3DDevice9::GetTransform(D3DTRANSFORMSTATETYPE s, D3DMATRIX* m) { return m_pReal->GetTransform(s, m); }
HRESULT STDMETHODCALLTYPE HookedIDirect3DDevice9::MultiplyTransform(D3DTRANSFORMSTATETYPE s, const D3DMATRIX* m) { return m_pReal->MultiplyTransform(s, m); }

// ================== VIEWPORT ==================
HRESULT STDMETHODCALLTYPE HookedIDirect3DDevice9::SetViewport(const D3DVIEWPORT9* p) { return m_pReal->SetViewport(p); }
HRESULT STDMETHODCALLTYPE HookedIDirect3DDevice9::GetViewport(D3DVIEWPORT9* p) { return m_pReal->GetViewport(p); }

// ================== MATERIAL ==================
HRESULT STDMETHODCALLTYPE HookedIDirect3DDevice9::SetMaterial(const D3DMATERIAL9* m) { return m_pReal->SetMaterial(m); }
HRESULT STDMETHODCALLTYPE HookedIDirect3DDevice9::GetMaterial(D3DMATERIAL9* m) { return m_pReal->GetMaterial(m); }

// ================== LIGHT ==================
HRESULT STDMETHODCALLTYPE HookedIDirect3DDevice9::SetLight(DWORD i, const D3DLIGHT9* l) { return m_pReal->SetLight(i, l); }
HRESULT STDMETHODCALLTYPE HookedIDirect3DDevice9::GetLight(DWORD i, D3DLIGHT9* l) { return m_pReal->GetLight(i, l); }
HRESULT STDMETHODCALLTYPE HookedIDirect3DDevice9::LightEnable(DWORD i, BOOL e) { return m_pReal->LightEnable(i, e); }
HRESULT STDMETHODCALLTYPE HookedIDirect3DDevice9::GetLightEnable(DWORD i, BOOL* e) { return m_pReal->GetLightEnable(i, e); }

// ================== CLIP PLANE ==================
HRESULT STDMETHODCALLTYPE HookedIDirect3DDevice9::SetClipPlane(DWORD i, const float* p) { return m_pReal->SetClipPlane(i, p); }
HRESULT STDMETHODCALLTYPE HookedIDirect3DDevice9::GetClipPlane(DWORD i, float* p) { return m_pReal->GetClipPlane(i, p); }

// ================== SCISSOR RECT ==================
HRESULT STDMETHODCALLTYPE HookedIDirect3DDevice9::SetScissorRect(const RECT* r) { return m_pReal->SetScissorRect(r); }
HRESULT STDMETHODCALLTYPE HookedIDirect3DDevice9::GetScissorRect(RECT* r) { return m_pReal->GetScissorRect(r); }

// ================== SOFTWARE VERTEX PROCESSING ==================
HRESULT STDMETHODCALLTYPE HookedIDirect3DDevice9::SetSoftwareVertexProcessing(BOOL b) { return m_pReal->SetSoftwareVertexProcessing(b); }
BOOL STDMETHODCALLTYPE HookedIDirect3DDevice9::GetSoftwareVertexProcessing() { return m_pReal->GetSoftwareVertexProcessing(); }

// ================== NPATCH ==================
HRESULT STDMETHODCALLTYPE HookedIDirect3DDevice9::SetNPatchMode(float f) { return m_pReal->SetNPatchMode(f); }
float STDMETHODCALLTYPE HookedIDirect3DDevice9::GetNPatchMode() { return m_pReal->GetNPatchMode(); }

// ================== VERTEX SHADER ==================
HRESULT STDMETHODCALLTYPE HookedIDirect3DDevice9::SetVertexShader(IDirect3DVertexShader9* vs) { g_VS++; return m_pReal->SetVertexShader(vs); }
HRESULT STDMETHODCALLTYPE HookedIDirect3DDevice9::GetVertexShader(IDirect3DVertexShader9** vs) { return m_pReal->GetVertexShader(vs); }
HRESULT STDMETHODCALLTYPE HookedIDirect3DDevice9::SetVertexShaderConstantF(UINT r, const float* c, UINT n) { return m_pReal->SetVertexShaderConstantF(r, c, n); }
HRESULT STDMETHODCALLTYPE HookedIDirect3DDevice9::GetVertexShaderConstantF(UINT r, float* c, UINT n) { return m_pReal->GetVertexShaderConstantF(r, c, n); }
HRESULT STDMETHODCALLTYPE HookedIDirect3DDevice9::SetVertexShaderConstantI(UINT r, const int* c, UINT n) { return m_pReal->SetVertexShaderConstantI(r, c, n); }
HRESULT STDMETHODCALLTYPE HookedIDirect3DDevice9::GetVertexShaderConstantI(UINT r, int* c, UINT n) { return m_pReal->GetVertexShaderConstantI(r, c, n); }
HRESULT STDMETHODCALLTYPE HookedIDirect3DDevice9::SetVertexShaderConstantB(UINT r, const BOOL* c, UINT n) { return m_pReal->SetVertexShaderConstantB(r, c, n); }
HRESULT STDMETHODCALLTYPE HookedIDirect3DDevice9::GetVertexShaderConstantB(UINT r, BOOL* c, UINT n) { return m_pReal->GetVertexShaderConstantB(r, c, n); }
HRESULT STDMETHODCALLTYPE HookedIDirect3DDevice9::CreateVertexShader(const DWORD* f, IDirect3DVertexShader9** s) { return m_pReal->CreateVertexShader(f, s); }

// ================== PIXEL SHADER ==================
HRESULT STDMETHODCALLTYPE HookedIDirect3DDevice9::SetPixelShader(IDirect3DPixelShader9* ps) { g_PS++; return m_pReal->SetPixelShader(ps); }
HRESULT STDMETHODCALLTYPE HookedIDirect3DDevice9::GetPixelShader(IDirect3DPixelShader9** ps) { return m_pReal->GetPixelShader(ps); }
HRESULT STDMETHODCALLTYPE HookedIDirect3DDevice9::SetPixelShaderConstantF(UINT r, const float* c, UINT n) { return m_pReal->SetPixelShaderConstantF(r, c, n); }
HRESULT STDMETHODCALLTYPE HookedIDirect3DDevice9::GetPixelShaderConstantF(UINT r, float* c, UINT n) { return m_pReal->GetPixelShaderConstantF(r, c, n); }
HRESULT STDMETHODCALLTYPE HookedIDirect3DDevice9::SetPixelShaderConstantI(UINT r, const int* c, UINT n) { return m_pReal->SetPixelShaderConstantI(r, c, n); }
HRESULT STDMETHODCALLTYPE HookedIDirect3DDevice9::GetPixelShaderConstantI(UINT r, int* c, UINT n) { return m_pReal->GetPixelShaderConstantI(r, c, n); }
HRESULT STDMETHODCALLTYPE HookedIDirect3DDevice9::SetPixelShaderConstantB(UINT r, const BOOL* c, UINT n) { return m_pReal->SetPixelShaderConstantB(r, c, n); }
HRESULT STDMETHODCALLTYPE HookedIDirect3DDevice9::GetPixelShaderConstantB(UINT r, BOOL* c, UINT n) { return m_pReal->GetPixelShaderConstantB(r, c, n); }
HRESULT STDMETHODCALLTYPE HookedIDirect3DDevice9::CreatePixelShader(const DWORD* f, IDirect3DPixelShader9** s) { return m_pReal->CreatePixelShader(f, s); }

// ================== VERTEX DECLARATION ==================
HRESULT STDMETHODCALLTYPE HookedIDirect3DDevice9::SetVertexDeclaration(IDirect3DVertexDeclaration9* d) { return m_pReal->SetVertexDeclaration(d); }
HRESULT STDMETHODCALLTYPE HookedIDirect3DDevice9::GetVertexDeclaration(IDirect3DVertexDeclaration9** d) { return m_pReal->GetVertexDeclaration(d); }
HRESULT STDMETHODCALLTYPE HookedIDirect3DDevice9::CreateVertexDeclaration(const D3DVERTEXELEMENT9* e, IDirect3DVertexDeclaration9** d) { return m_pReal->CreateVertexDeclaration(e, d); }

// ================== STREAM SOURCE ==================
HRESULT STDMETHODCALLTYPE HookedIDirect3DDevice9::SetStreamSource(UINT n, IDirect3DVertexBuffer9* b, UINT o, UINT s) { g_STREAM++; return m_pReal->SetStreamSource(n, b, o, s); }
HRESULT STDMETHODCALLTYPE HookedIDirect3DDevice9::GetStreamSource(UINT n, IDirect3DVertexBuffer9** b, UINT* o, UINT* s) { return m_pReal->GetStreamSource(n, b, o, s); }
HRESULT STDMETHODCALLTYPE HookedIDirect3DDevice9::SetStreamSourceFreq(UINT n, UINT f) { return m_pReal->SetStreamSourceFreq(n, f); }
HRESULT STDMETHODCALLTYPE HookedIDirect3DDevice9::GetStreamSourceFreq(UINT n, UINT* f) { return m_pReal->GetStreamSourceFreq(n, f); }
HRESULT STDMETHODCALLTYPE HookedIDirect3DDevice9::SetIndices(IDirect3DIndexBuffer9* i) { g_IDX++; return m_pReal->SetIndices(i); }
HRESULT STDMETHODCALLTYPE HookedIDirect3DDevice9::GetIndices(IDirect3DIndexBuffer9** i) { return m_pReal->GetIndices(i); }
HRESULT STDMETHODCALLTYPE HookedIDirect3DDevice9::ProcessVertices(UINT a, UINT b, UINT c, IDirect3DVertexBuffer9* d, IDirect3DVertexDeclaration9* e, DWORD f) { return m_pReal->ProcessVertices(a, b, c, d, e, f); }

// ================== CREATION ==================
HRESULT STDMETHODCALLTYPE HookedIDirect3DDevice9::CreateTexture(UINT w, UINT h, UINT l, DWORD u, D3DFORMAT f, D3DPOOL p, IDirect3DTexture9** t, HANDLE* sh) { return m_pReal->CreateTexture(w, h, l, u, f, p, t, sh); }
HRESULT STDMETHODCALLTYPE HookedIDirect3DDevice9::CreateVolumeTexture(UINT w, UINT h, UINT d, UINT l, DWORD u, D3DFORMAT f, D3DPOOL p, IDirect3DVolumeTexture9** v, HANDLE* sh) { return m_pReal->CreateVolumeTexture(w, h, d, l, u, f, p, v, sh); }
HRESULT STDMETHODCALLTYPE HookedIDirect3DDevice9::CreateCubeTexture(UINT e, UINT l, DWORD u, D3DFORMAT f, D3DPOOL p, IDirect3DCubeTexture9** c, HANDLE* sh) { return m_pReal->CreateCubeTexture(e, l, u, f, p, c, sh); }
HRESULT STDMETHODCALLTYPE HookedIDirect3DDevice9::CreateRenderTarget(UINT w, UINT h, D3DFORMAT f, D3DMULTISAMPLE_TYPE m, DWORD mq, BOOL l, IDirect3DSurface9** s, HANDLE* sh) { return m_pReal->CreateRenderTarget(w, h, f, m, mq, l, s, sh); }
HRESULT STDMETHODCALLTYPE HookedIDirect3DDevice9::CreateDepthStencilSurface(UINT w, UINT h, D3DFORMAT f, D3DMULTISAMPLE_TYPE m, DWORD mq, BOOL d, IDirect3DSurface9** s, HANDLE* sh) { return m_pReal->CreateDepthStencilSurface(w, h, f, m, mq, d, s, sh); }
HRESULT STDMETHODCALLTYPE HookedIDirect3DDevice9::CreateOffscreenPlainSurface(UINT w, UINT h, D3DFORMAT f, D3DPOOL p, IDirect3DSurface9** s, HANDLE* sh) { return m_pReal->CreateOffscreenPlainSurface(w, h, f, p, s, sh); }
HRESULT STDMETHODCALLTYPE HookedIDirect3DDevice9::CreateVertexBuffer(UINT l, DWORD u, DWORD f, D3DPOOL p, IDirect3DVertexBuffer9** b, HANDLE* sh) { return m_pReal->CreateVertexBuffer(l, u, f, p, b, sh); }
HRESULT STDMETHODCALLTYPE HookedIDirect3DDevice9::CreateIndexBuffer(UINT l, DWORD u, D3DFORMAT f, D3DPOOL p, IDirect3DIndexBuffer9** b, HANDLE* sh) { return m_pReal->CreateIndexBuffer(l, u, f, p, b, sh); }
HRESULT STDMETHODCALLTYPE HookedIDirect3DDevice9::CreateQuery(D3DQUERYTYPE t, IDirect3DQuery9** q) { return m_pReal->CreateQuery(t, q); }

// ================== STATE BLOCK ==================
HRESULT STDMETHODCALLTYPE HookedIDirect3DDevice9::CreateStateBlock(D3DSTATEBLOCKTYPE t, IDirect3DStateBlock9** s) { return m_pReal->CreateStateBlock(t, s); }
HRESULT STDMETHODCALLTYPE HookedIDirect3DDevice9::BeginStateBlock() { return m_pReal->BeginStateBlock(); }
HRESULT STDMETHODCALLTYPE HookedIDirect3DDevice9::EndStateBlock(IDirect3DStateBlock9** s) { return m_pReal->EndStateBlock(s); }

// ================== SURFACE OPS ==================
HRESULT STDMETHODCALLTYPE HookedIDirect3DDevice9::UpdateSurface(IDirect3DSurface9* src, const RECT* sr, IDirect3DSurface9* dst, const POINT* dp) { return m_pReal->UpdateSurface(src, sr, dst, dp); }
HRESULT STDMETHODCALLTYPE HookedIDirect3DDevice9::UpdateTexture(IDirect3DBaseTexture9* src, IDirect3DBaseTexture9* dst) { return m_pReal->UpdateTexture(src, dst); }
HRESULT STDMETHODCALLTYPE HookedIDirect3DDevice9::GetRenderTargetData(IDirect3DSurface9* rt, IDirect3DSurface9* dst) { return m_pReal->GetRenderTargetData(rt, dst); }
HRESULT STDMETHODCALLTYPE HookedIDirect3DDevice9::GetFrontBufferData(UINT i, IDirect3DSurface9* dst) { return m_pReal->GetFrontBufferData(i, dst); }
HRESULT STDMETHODCALLTYPE HookedIDirect3DDevice9::StretchRect(IDirect3DSurface9* src, const RECT* sr, IDirect3DSurface9* dst, const RECT* dr, D3DTEXTUREFILTERTYPE f) { return m_pReal->StretchRect(src, sr, dst, dr, f); }
HRESULT STDMETHODCALLTYPE HookedIDirect3DDevice9::ColorFill(IDirect3DSurface9* s, const RECT* r, D3DCOLOR c) { return m_pReal->ColorFill(s, r, c); }

// ================== TEXTURE STAGE ==================
HRESULT STDMETHODCALLTYPE HookedIDirect3DDevice9::GetTextureStageState(DWORD s, D3DTEXTURESTAGESTATETYPE t, DWORD* v) { return m_pReal->GetTextureStageState(s, t, v); }
HRESULT STDMETHODCALLTYPE HookedIDirect3DDevice9::SetTextureStageState(DWORD s, D3DTEXTURESTAGESTATETYPE t, DWORD v) { return m_pReal->SetTextureStageState(s, t, v); }
HRESULT STDMETHODCALLTYPE HookedIDirect3DDevice9::GetSamplerState(DWORD s, D3DSAMPLERSTATETYPE t, DWORD* v) { return m_pReal->GetSamplerState(s, t, v); }
HRESULT STDMETHODCALLTYPE HookedIDirect3DDevice9::SetSamplerState(DWORD s, D3DSAMPLERSTATETYPE t, DWORD v) { return m_pReal->SetSamplerState(s, t, v); }

// ================== CLIP ==================
HRESULT STDMETHODCALLTYPE HookedIDirect3DDevice9::SetClipStatus(const D3DCLIPSTATUS9* c) { return m_pReal->SetClipStatus(c); }
HRESULT STDMETHODCALLTYPE HookedIDirect3DDevice9::GetClipStatus(D3DCLIPSTATUS9* c) { return m_pReal->GetClipStatus(c); }

// ================== CURSOR ==================
HRESULT STDMETHODCALLTYPE HookedIDirect3DDevice9::SetCursorProperties(UINT x, UINT y, IDirect3DSurface9* s) { return m_pReal->SetCursorProperties(x, y, s); }
void STDMETHODCALLTYPE HookedIDirect3DDevice9::SetCursorPosition(int x, int y, DWORD f) { m_pReal->SetCursorPosition(x, y, f); }
BOOL STDMETHODCALLTYPE HookedIDirect3DDevice9::ShowCursor(BOOL b) { return m_pReal->ShowCursor(b); }

// ================== GAMMA ==================
void STDMETHODCALLTYPE HookedIDirect3DDevice9::SetGammaRamp(UINT i, DWORD f, const D3DGAMMARAMP* g) { m_pReal->SetGammaRamp(i, f, g); }
void STDMETHODCALLTYPE HookedIDirect3DDevice9::GetGammaRamp(UINT i, D3DGAMMARAMP* g) { m_pReal->GetGammaRamp(i, g); }

// ================== PALETTE ==================
HRESULT STDMETHODCALLTYPE HookedIDirect3DDevice9::SetPaletteEntries(UINT n, const PALETTEENTRY* e) { return m_pReal->SetPaletteEntries(n, e); }
HRESULT STDMETHODCALLTYPE HookedIDirect3DDevice9::GetPaletteEntries(UINT n, PALETTEENTRY* e) { return m_pReal->GetPaletteEntries(n, e); }
HRESULT STDMETHODCALLTYPE HookedIDirect3DDevice9::SetCurrentTexturePalette(UINT n) { return m_pReal->SetCurrentTexturePalette(n); }
HRESULT STDMETHODCALLTYPE HookedIDirect3DDevice9::GetCurrentTexturePalette(UINT* n) { return m_pReal->GetCurrentTexturePalette(n); }

// ================== SWAP CHAIN ==================
HRESULT STDMETHODCALLTYPE HookedIDirect3DDevice9::CreateAdditionalSwapChain(D3DPRESENT_PARAMETERS* p, IDirect3DSwapChain9** s) { return m_pReal->CreateAdditionalSwapChain(p, s); }
HRESULT STDMETHODCALLTYPE HookedIDirect3DDevice9::GetSwapChain(UINT i, IDirect3DSwapChain9** s) { return m_pReal->GetSwapChain(i, s); }
UINT STDMETHODCALLTYPE HookedIDirect3DDevice9::GetNumberOfSwapChains() { return m_pReal->GetNumberOfSwapChains(); }

// ================== BACK BUFFER ==================
HRESULT STDMETHODCALLTYPE HookedIDirect3DDevice9::GetBackBuffer(UINT i, UINT b, D3DBACKBUFFER_TYPE t, IDirect3DSurface9** s) { return m_pReal->GetBackBuffer(i, b, t, s); }
HRESULT STDMETHODCALLTYPE HookedIDirect3DDevice9::GetRasterStatus(UINT i, D3DRASTER_STATUS* s) { return m_pReal->GetRasterStatus(i, s); }

// ================== SCENE ==================
HRESULT STDMETHODCALLTYPE HookedIDirect3DDevice9::BeginScene() { return m_pReal->BeginScene(); }
HRESULT STDMETHODCALLTYPE HookedIDirect3DDevice9::EndScene() { return m_pReal->EndScene(); }
HRESULT STDMETHODCALLTYPE HookedIDirect3DDevice9::Clear(DWORD a, const D3DRECT* b, DWORD c, D3DCOLOR d, float e, DWORD f) { return m_pReal->Clear(a, b, c, d, e, f); }

// ================== CAPS ==================
HRESULT STDMETHODCALLTYPE HookedIDirect3DDevice9::TestCooperativeLevel() { return m_pReal->TestCooperativeLevel(); }
UINT STDMETHODCALLTYPE HookedIDirect3DDevice9::GetAvailableTextureMem() { return m_pReal->GetAvailableTextureMem(); }
HRESULT STDMETHODCALLTYPE HookedIDirect3DDevice9::EvictManagedResources() { return m_pReal->EvictManagedResources(); }
HRESULT STDMETHODCALLTYPE HookedIDirect3DDevice9::GetDeviceCaps(D3DCAPS9* c) { return m_pReal->GetDeviceCaps(c); }
HRESULT STDMETHODCALLTYPE HookedIDirect3DDevice9::GetDirect3D(IDirect3D9** d) { return m_pReal->GetDirect3D(d); }
HRESULT STDMETHODCALLTYPE HookedIDirect3DDevice9::GetCreationParameters(D3DDEVICE_CREATION_PARAMETERS* p) { return m_pReal->GetCreationParameters(p); }
HRESULT STDMETHODCALLTYPE HookedIDirect3DDevice9::GetDisplayMode(UINT i, D3DDISPLAYMODE* m) { return m_pReal->GetDisplayMode(i, m); }
HRESULT STDMETHODCALLTYPE HookedIDirect3DDevice9::ValidateDevice(DWORD* p) { return m_pReal->ValidateDevice(p); }
HRESULT STDMETHODCALLTYPE HookedIDirect3DDevice9::SetDialogBoxMode(BOOL b) { return m_pReal->SetDialogBoxMode(b); }

// ================== PATCH ==================
HRESULT STDMETHODCALLTYPE HookedIDirect3DDevice9::DrawRectPatch(UINT h, const float* n, const D3DRECTPATCH_INFO* i) { return m_pReal->DrawRectPatch(h, n, i); }
HRESULT STDMETHODCALLTYPE HookedIDirect3DDevice9::DrawTriPatch(UINT h, const float* n, const D3DTRIPATCH_INFO* i) { return m_pReal->DrawTriPatch(h, n, i); }
HRESULT STDMETHODCALLTYPE HookedIDirect3DDevice9::DeletePatch(UINT h) { return m_pReal->DeletePatch(h); }
