#pragma once
#include <atomic>
#include <functional>
#include <string>

struct A2MResult
{
    long long soundNotes = 0;
    long long specNotes = 0;
    double seconds = 0.0;
    std::wstring fontPath;
};

//progress gets 0 to 100
using A2MProgressFn = std::function<void(int)>;

//PCM direct v2 - 1 tick per sample with a2m-pcm 2 and a2m-spec 1 markers
bool A2MConvert(const std::wstring& inPath, const std::wstring& outPath,
                const std::atomic<bool>& cancel, const A2MProgressFn& progress,
                A2MResult& res, std::wstring& err);

//input folder -> stem_midiver folder holding stem_midiver mid sf2 and a2mfreq
std::wstring A2MOutputPathFor(const std::wstring& inPath);
