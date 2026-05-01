#pragma once
#include <string>
#include <unordered_map>
#include <vector>
#include <cstdint>

struct GlyphInfo {
    wchar_t character = L'?';
    float confidence = 0.0f;
    int stableCount = 0;
};

struct GlyphVote {
    std::unordered_map<wchar_t, float> votes;
};

class TextLearning
{
private:
    std::unordered_map<uint32_t, GlyphInfo> m_FontMap;
    std::unordered_map<uint32_t, GlyphVote> m_Votes;

    std::wstring m_lastText;
    int m_stableFrames;

    float CompareStrings(const std::wstring& a, const std::wstring& b);
    void AttemptLearning(const std::vector<uint32_t>& glyphs, const std::wstring& bestMatch);
    void CommitIfStable(uint32_t hash);

    bool IsStable(const std::wstring& current);
    float ComputeConfidence(uint32_t hash) const;

public:
    TextLearning();

    void Process(const std::vector<uint32_t>& glyphs, const std::wstring& detected);

    wchar_t GetChar(uint32_t hash) const;
    int GetMappingsCount() const;
    void AddMapping(uint32_t hash, wchar_t ch);
};
