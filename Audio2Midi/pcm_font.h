#pragma once
#include <string>
#include <vector>

//silent preset for the spectrogram channel - presets 0 and 1 are positive and negative impulses
constexpr int kPcmSilentPreset = 2;

//impulse sf2 for PCM v2 - one zone per key with amplitude keyAmp key
bool WritePcmImpulseFont(const std::wstring& path, const std::vector<double>& keyAmp, int sampleRate, std::wstring& err);
