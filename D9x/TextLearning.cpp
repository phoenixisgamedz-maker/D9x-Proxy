#include "TextLearning.h"
#include <algorithm>
#include <cwctype>

// ================= COMPARE TEXT (Tolerant) =================
float TextLearning::CompareText(const std::wstring& a, const std::wstring& b)
{
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

            if (j < 0 || j >= (int)b.size())
                continue;

            wchar_t cb = b[j];

            if (cb == L' ')
                continue;

            if (ca == cb)
            {
                found = true;
                break;
            }
        }

        int weight = iswalpha(ca) ? 2 : 1;

        if (found)
            match += weight;

        total += weight;
    }

    if (total == 0)
        return 0.0f;

    return (float)match / (float)total;
}

// ================= CONFIDENCE =================
float TextLearning::ComputeConfidence()
{
    float sum = 0;
    int n = 0;

    for (auto& pair : m_learnedCount)
    {
        sum += (float)pair.second;
        n++;
    }

    if (n == 0)
        return 0.0f;

    return sum / n;
}

// ================= CONSTRUCTOR =================
TextLearning::TextLearning()
    : m_stableFrames(0)
{
}

// ================= BEST MATCH =================
const wchar_t* TextLearning::FindBestMatch(const std::wstring& detected,
    const std::vector<std::wstring>& knownPhrases,
    float& outScore)
{
    float bestScore = 0.0f;
    const wchar_t* bestMatch = nullptr;

    for (auto& phrase : knownPhrases)
    {
        float score = CompareText(detected, phrase);

        if (score > bestScore)
        {
            bestScore = score;
            bestMatch = phrase.c_str();
        }
    }

    outScore = bestScore;
    return bestMatch;
}

// ================= TRY LEARN =================
bool TextLearning::TryLearn(const std::wstring& detected,
    const std::wstring& matchedPhrase,
    float score)
{
    // Temporal Stability
    if (detected == m_lastText)
        m_stableFrames++;
    else
    {
        m_stableFrames = 0;
        m_lastText = detected;
    }

    // Dynamic Threshold
    float confidence = ComputeConfidence();
    float threshold = 0.6f;

    if (confidence < 0.3f)
        threshold += confidence * 0.05f;
    else
        threshold += 0.3f * 0.05f;

    // Learning Decision
    if (m_stableFrames >= 2 && score > threshold)
    {
        int& count = m_learnedCount[matchedPhrase];

        if (count < 3)
        {
            count++;
            return true;
        }
    }

    return false;
}

// ================= ADD MAPPING =================
void TextLearning::AddMapping(uint32_t hash, wchar_t ch)
{
    m_charMap[hash] = ch;
}

// ================= GET CHAR =================
wchar_t TextLearning::GetChar(uint32_t hash)
{
    auto it = m_charMap.find(hash);
    if (it != m_charMap.end())
        return it->second;
    return L'?';
}

// ================= GET MAPPINGS COUNT =================
int TextLearning::GetMappingsCount() const
{
    return (int)m_charMap.size();
}