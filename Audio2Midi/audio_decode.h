#pragma once
#include <atomic>
#include <string>
#include <vector>

//decode any audio file -> interleaved stereo float pcm at the given rate
//tries ffmpeg first - exe folder then PATH - then Windows Media Foundation
bool DecodeAudioStereo(const std::wstring& path, int rate, std::vector<float>& out,
                       const std::atomic<bool>& cancel, std::wstring& err);
