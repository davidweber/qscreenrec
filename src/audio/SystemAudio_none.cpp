#include "SystemAudio.h"

// Built when no system-audio backend is available (e.g. Linux without libpulse headers).
bool SystemAudio::isSupported() { return false; }
std::unique_ptr<SystemAudio> SystemAudio::create() { return nullptr; }
