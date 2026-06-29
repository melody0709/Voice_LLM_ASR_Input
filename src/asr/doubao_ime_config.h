#pragma once

#include "doubao_ime_asr.h"
#include "globals.h"

inline doubao_ime_asr::DoubaoImeConfig BuildDoubaoImeConfigFromConfig(const Config& config) {
    doubao_ime_asr::DoubaoImeConfig dcfg;
    dcfg.deviceId = config.doubaoImeDeviceId;
    dcfg.cdid = config.doubaoImeCdid;
    dcfg.token = config.doubaoImeToken;
    dcfg.sampleRate = 16000;
    dcfg.channels = 1;
    dcfg.frameMs = 20;
    dcfg.enablePunctuation = true;
    return dcfg;
}
