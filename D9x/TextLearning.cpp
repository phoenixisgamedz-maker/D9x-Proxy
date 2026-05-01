#define NOMINMAX
#include "TextLearning.h"
#include "Shared.h"
#include <algorithm>
#include <cwctype>

TextLearning::TextLearning()
    : m_stableFrames(0)
{
}

float TextLearning::CompareStrings(const std::wstring& a, const std::wstring& b)
{
    if (a.empty() || b.empty()) return 0.0f;

    int total = 0;
    int match = 0;
    size_t len = (a.size() < b.size()) ? a.size() : b.size();  // بديل يدوي لـ std::min

    for (size_t i = 0; i < len; ++i)
    {
        wchar_t ca = a[i];
        wchar_t cb = b[i];

        if (ca == L'?' && cb == L'?') continue;

        if (ca != L'?' && cb != L'?')
        {
            ++total;
            if (ca == cb) ++match;
        }
    }

    return (total == 0) ? 0.0f : (float)match / (float)total;
}

float TextLearning::ComputeConfidence(uint32_t hash) const
{
    auto it = m_FontMap.find(hash);
    return (it != m_FontMap.end()) ? it->second.confidence : 0.0f;
}

bool TextLearning::IsStable(const std::wstring& current)
{
    if (current == m_lastText)
    {
        ++m_stableFrames;
    }
    else
    {
        m_stableFrames = 0;
        m_lastText = current;
    }
    return m_stableFrames >= 2;
}

void TextLearning::Process(const std::vector<uint32_t>& glyphs, const std::wstring& detected)
{
    if (glyphs.empty() || detected.empty()) return;

    // فك تشفير النص الحالي
    std::wstring decoded;
    decoded.reserve(glyphs.size());

    for (size_t idx = 0; idx < glyphs.size(); ++idx)
    {
        uint32_t hash = glyphs[idx];
        auto it = m_FontMap.find(hash);
        if (it != m_FontMap.end())
        {
            decoded += it->second.character;
        }
        else
        {
            decoded += L'?';
        }
    }

    float bestScore = CompareStrings(decoded, detected);
    bool stable = IsStable(decoded);

    // ================= DIAGNOSTIC =================
    LogF("[DEBUG] decoded='%S'", decoded.c_str());
    LogF("[DEBUG] detected='%S'", detected.c_str());
    LogF("[DEBUG] score=%.2f stable=%d mappings=%d",
        bestScore, m_stableFrames, (int)m_FontMap.size());

    // Bootstrap (تعلم أولي حتى مع معرفة قليلة)
    if (m_FontMap.empty() && bestScore > 0.25f)
    {
        LogF("[LEARN] BOOTSTRAP: '%S' vs '%S' (score=%.2f)", decoded.c_str(), detected.c_str(), bestScore);
        AttemptLearning(glyphs, detected);
        return;
    }

    // تعلم عادي مع الاستقرار
    if (stable && bestScore > 0.55f)
    {
        LogF("[LEARN] STABLE: '%S' vs '%S' (score=%.2f, frames=%d)", decoded.c_str(), detected.c_str(), bestScore, m_stableFrames);
        AttemptLearning(glyphs, detected);
    }
    // تعلم بثقة عالية بدون شرط الاستقرار
    else if (bestScore > 0.7f)
    {
        LogF("[LEARN] HIGH CONFIDENCE: '%S' vs '%S' (score=%.2f)", decoded.c_str(), detected.c_str(), bestScore);
        AttemptLearning(glyphs, detected);
    }
}

void TextLearning::AttemptLearning(const std::vector<uint32_t>& glyphs, const std::wstring& bestMatch)
{
    LogF("[DEBUG] LEARNING START len=%d", (int)glyphs.size());

    size_t len = (glyphs.size() < bestMatch.size()) ? glyphs.size() : bestMatch.size();

    for (size_t i = 0; i < len; ++i)
    {
        uint32_t hash = glyphs[i];
        if (hash == 0) continue;

        // ================= ALIGNMENT =================
        for (int shift = -1; shift <= 1; ++shift)
        {
            int j = (int)i + shift;
            if (j < 0 || j >= (int)bestMatch.size()) continue;

            wchar_t targetChar = bestMatch[j];
            if (!iswprint(targetChar) || targetChar == L' ') continue;

            // تسجيل صوت
            m_Votes[hash].votes[targetChar] += 1.0f;

            // مكافأة إضافية إذا كان متطابقاً مع التعلم السابق
            auto it = m_FontMap.find(hash);
            if (it != m_FontMap.end() && it->second.character == targetChar)
            {
                m_Votes[hash].votes[targetChar] += 0.5f;
            }
        }

        CommitIfStable(hash);
    }
}

void TextLearning::CommitIfStable(uint32_t hash)
{
    auto itVote = m_Votes.find(hash);
    if (itVote == m_Votes.end()) return;

    float total = 0.0f;
    float maxWeight = 0.0f;
    wchar_t bestChar = L'?';

    for (auto& pair : itVote->second.votes)
    {
        total += pair.second;
        if (pair.second > maxWeight)
        {
            maxWeight = pair.second;
            bestChar = pair.first;
        }
    }

    if (total == 0.0f) return;

    float confidence = maxWeight / total;

    // ================= DIAGNOSTIC =================
    LogF("[DEBUG] hash=0x%08X total=%.2f max=%.2f conf=%.2f",
        hash, total, maxWeight, confidence);

    bool shouldCommit = false;

    if (confidence > 0.75f && maxWeight > 3.0f)
    {
        shouldCommit = true;
    }
    else if (confidence > 0.6f && maxWeight > 6.0f)
    {
        shouldCommit = true;
    }
    else if (m_FontMap[hash].character != bestChar && confidence > 0.8f && maxWeight > 4.0f)
    {
        shouldCommit = true;
        LogF("[LEARN] CORRECTING: 0x%08X -> '%lc' (was '%lc')", hash, bestChar, m_FontMap[hash].character);
    }

    if (shouldCommit)
    {
        GlyphInfo& info = m_FontMap[hash];
        if (info.character != bestChar)
        {
            info.character = bestChar;
            info.confidence = confidence;
            info.stableCount = 0;
        }
        else if (info.character == bestChar && confidence > info.confidence)
        {
            info.confidence = confidence;
            ++info.stableCount;
        }

        if (info.stableCount > 10)
        {
            itVote->second.votes.clear();
            itVote->second.votes[bestChar] = maxWeight;
        }
    }
}

wchar_t TextLearning::GetChar(uint32_t hash) const
{
    auto it = m_FontMap.find(hash);
    if (it != m_FontMap.end())
    {
        return it->second.character;
    }
    return L'?';
}

int TextLearning::GetMappingsCount() const
{
    return (int)m_FontMap.size();
}

void TextLearning::AddMapping(uint32_t hash, wchar_t ch)
{
    GlyphInfo& info = m_FontMap[hash];
    info.character = ch;
    info.confidence = 1.0f;
    info.stableCount = 10;
}
