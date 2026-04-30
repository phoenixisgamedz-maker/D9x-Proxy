#pragma once
#include <string>
#include <unordered_map>
#include <vector>

class TextLearning
{
private:
    std::unordered_map<uint32_t, wchar_t> m_charMap;
    std::unordered_map<std::wstring, int> m_learnedCount;
    std::wstring m_lastText;
    int m_stableFrames;

    float CompareText(const std::wstring& a, const std::wstring& b);
    float ComputeConfidence();

public:
    TextLearning();

    const wchar_t* FindBestMatch(const std::wstring& detected,
        const std::vector<std::wstring>& knownPhrases,
        float& outScore);

    bool TryLearn(const std::wstring& detected,
        const std::wstring& matchedPhrase,
        float score);

    void AddMapping(uint32_t hash, wchar_t ch);
    wchar_t GetChar(uint32_t hash);
    int GetMappingsCount() const;
};