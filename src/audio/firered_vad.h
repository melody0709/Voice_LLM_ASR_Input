#pragma once

#include <cmath>
#include <cstring>
#include <deque>
#include <memory>
#include <string>
#include <utility>
#include <vector>
#include <algorithm>

#include "onnxruntime_cxx_api.h"
#include "kaldi-native-fbank/csrc/online-feature.h"
#include "utils.h"

namespace firered_vad {

static const float kCmvnMeans[80] = {
    10.42295174919564f, 10.862097411631494f, 11.764544378124809f, 12.490164701573908f,
    13.25983008289003f, 13.89594383242307f,  14.364940238918987f, 14.593948347480778f,
    14.749723601612253f, 14.668315348346496f, 14.730796723156509f, 14.775052459167833f,
    14.9890519821556f,   15.178004932637085f, 15.253520314586988f, 15.328637048782031f,
    15.334018588850057f, 15.288641702136166f, 15.4276616890477f,   15.246266155846598f,
    15.092573799088989f, 15.290421940704482f, 15.07575008669762f,  15.186772872540853f,
    15.088673242416798f, 15.170797396442111f, 15.070178088017926f, 15.150795340269006f,
    15.108532832116397f, 15.115345080167454f, 15.141279987705998f, 15.131832359605129f,
    15.145195868641611f, 15.19151892676777f,  15.235478667211774f, 15.306369752614641f,
    15.373021476906201f, 15.416394625766584f, 15.459857436373436f, 15.39143273165164f,
    15.46357624247469f,  15.399661212735632f, 15.462907917820873f, 15.441629120393843f,
    15.484969525295984f, 15.552401775001249f, 15.638091925650645f, 15.705489346158819f,
    15.767008852632651f, 15.855123781367105f, 15.867269782501769f, 15.891537408746947f,
    15.9231448295521f,   15.978382613315533f, 16.014801667676718f, 16.048674939996204f,
    16.082029914992358f, 16.09680075379873f,  16.093736693349236f, 16.07247919506059f,
    16.075509664672943f, 16.02227087563821f,  15.976762101902347f, 15.89786454765505f,
    15.812741644368487f, 15.711205109067762f, 15.604198886052728f, 15.553519438005933f,
    15.51025275187747f,  15.460023817226517f, 15.4156843628003f,   15.37602764551613f,
    15.328348980305998f, 15.295370796331634f, 15.185470194591382f, 15.017044975516262f,
    14.905080029850632f, 14.623806569017782f, 14.138093813776406f, 13.313870348004635f,
};
static const float kCmvnIstd[80] = {
    0.2494980879825924f,  0.23563235243542163f, 0.23145152525802104f, 0.2332233926481505f,
    0.23182660283718737f, 0.22853356937894798f, 0.2243486976577694f,  0.21898920450844725f,
    0.21832438092730974f, 0.22082592767700662f, 0.2229673556813116f,  0.22288416257259386f,
    0.22234810686081127f, 0.22100642502031184f, 0.21994202276343874f, 0.22005444019015313f,
    0.22070092118977014f, 0.22150809748461409f, 0.22236667273698002f, 0.22305291750035372f,
    0.22335341587062665f, 0.22438905727453648f, 0.22547701626910854f, 0.2269007560258811f,
    0.22823023223045188f, 0.22931472070164832f, 0.23046728075908798f, 0.23083553439603108f,
    0.23143382733873202f, 0.2322065940520882f,  0.2325798897870885f,  0.2336197007969686f,
    0.23437240620327746f, 0.23508252486137127f, 0.23578078965798868f, 0.235892002292441f,
    0.2360209777141303f,  0.23663799549538955f, 0.2374987640063862f,  0.2379845180109627f,
    0.23899377757763487f, 0.239748152899516f,   0.24030835895648858f, 0.2409769361661754f,
    0.24143248909906587f, 0.24135465696291541f, 0.24079937967447773f, 0.24047405396120294f,
    0.23995525139180407f, 0.23952287673801784f, 0.2394808893818449f,  0.2393650908183211f,
    0.23929339134427347f, 0.23902199338028696f, 0.23857873289710127f, 0.23814701563685442f,
    0.23804621120577427f, 0.23824193788738984f, 0.2386009552212688f,  0.23915406501238878f,
    0.23922540730102645f, 0.2393830803524144f,  0.2397336021407156f,  0.2396056154523928f,
    0.24028502694537057f, 0.2406181323375118f,  0.2406792985927079f,  0.24096201908324874f,
    0.24043605546026958f, 0.24021526735270676f, 0.23972514279402155f, 0.23871998076352863f,
    0.2374413126648784f,  0.23619509194955604f, 0.23337281484306663f, 0.2268023271445609f,
    0.2257750261856693f,  0.22503847248255957f, 0.2263113742246566f,  0.2289949344716713f,
};

constexpr int kSampleRate = 16000;
constexpr int kFbankBins = 80;
constexpr int kFrameShift = 160;
constexpr int kFrameLength = 400;
constexpr int kFramesPerSecond = 100;

static knf::FbankOptions MakeFbankOpts() {
    knf::FbankOptions opts;
    opts.frame_opts.samp_freq = kSampleRate;
    opts.frame_opts.frame_length_ms = 25.0f;
    opts.frame_opts.frame_shift_ms = 10.0f;
    opts.frame_opts.dither = 0.0f;
    opts.frame_opts.snip_edges = true;
    opts.mel_opts.num_bins = kFbankBins;
    opts.use_energy = false;
    opts.use_log_fbank = true;
    opts.use_power = true;
    return opts;
}

enum class VadState {
    SILENCE = 0,
    POSSIBLE_SPEECH = 1,
    SPEECH = 2,
    POSSIBLE_SILENCE = 3
};

struct FireRedVadConfig {
    std::string modelPath;
    float threshold = 0.2f;
    int smoothWindowSize = 5;
    int padStartMs = 80;
    int minSpeechMs = 50;
    int maxSpeechFrame = 2000;
    int minSilenceMs = 300;
};

struct StreamVadPostprocessor {
    float speechThreshold;
    int smoothWindowSize;
    int padStartFrame;
    int minSpeechFrame;
    int maxSpeechFrame;
    int minSilenceFrame;

    VadState state = VadState::SILENCE;
    std::deque<float> smoothWindow;
    float smoothSum = 0.0f;
    int frameCnt = 0;
    int speechCnt = 0;
    int silenceCnt = 0;
    bool hitMaxSpeech = false;
    int lastSpeechStartFrame = -1;
    int lastSpeechEndFrame = -1;

    std::vector<std::pair<int, int>> segments;

    StreamVadPostprocessor(float threshold, int smoothWin, int padStart,
                           int minSpeech, int maxSpeech, int minSilence)
        : speechThreshold(threshold),
          smoothWindowSize(std::max(1, smoothWin)),
          padStartFrame(std::max(smoothWindowSize, padStart)),
          minSpeechFrame(minSpeech),
          maxSpeechFrame(maxSpeech),
          minSilenceFrame(minSilence) {}

    void Reset() {
        state = VadState::SILENCE;
        smoothWindow.clear();
        smoothSum = 0.0f;
        frameCnt = 0;
        speechCnt = 0;
        silenceCnt = 0;
        hitMaxSpeech = false;
        lastSpeechStartFrame = -1;
        lastSpeechEndFrame = -1;
        segments.clear();
    }

    float SmoothProb(float prob) {
        smoothWindow.push_back(prob);
        smoothSum += prob;
        if (static_cast<int>(smoothWindow.size()) > smoothWindowSize) {
            smoothSum -= smoothWindow.front();
            smoothWindow.pop_front();
        }
        return smoothSum / static_cast<float>(smoothWindow.size());
    }

    void ProcessOneFrame(float rawProb) {
        frameCnt++;
        float smoothedProb = SmoothProb(rawProb);
        bool isSpeech = (smoothedProb >= speechThreshold);

        if (hitMaxSpeech) {
            int startFrame = frameCnt - 1;
            if (lastSpeechEndFrame >= 0)
                startFrame = std::max(startFrame, lastSpeechEndFrame + 1);
            lastSpeechStartFrame = startFrame;
            hitMaxSpeech = false;
        }

        switch (state) {
        case VadState::SILENCE:
            if (isSpeech) {
                state = VadState::POSSIBLE_SPEECH;
                speechCnt = 1;
            } else {
                silenceCnt++;
                speechCnt = 0;
            }
            break;

        case VadState::POSSIBLE_SPEECH:
            if (isSpeech) {
                speechCnt++;
                if (speechCnt >= minSpeechFrame) {
                    state = VadState::SPEECH;
                    int startFrame = std::max(0,
                        (frameCnt - 1) - speechCnt + 1 - padStartFrame);
                    if (lastSpeechEndFrame >= 0)
                        startFrame = std::max(startFrame, lastSpeechEndFrame + 1);
                    lastSpeechStartFrame = startFrame;
                    silenceCnt = 0;
                }
            } else {
                state = VadState::SILENCE;
                silenceCnt = 1;
                speechCnt = 0;
            }
            break;

        case VadState::SPEECH:
            speechCnt++;
            if (isSpeech) {
                silenceCnt = 0;
                if (speechCnt >= maxSpeechFrame) {
                    hitMaxSpeech = true;
                    segments.push_back({lastSpeechStartFrame, frameCnt - 1});
                    lastSpeechEndFrame = frameCnt - 1;
                    lastSpeechStartFrame = -1;
                    speechCnt = 0;
                }
            } else {
                state = VadState::POSSIBLE_SILENCE;
                silenceCnt = 1;
            }
            break;

        case VadState::POSSIBLE_SILENCE:
            speechCnt++;
            if (isSpeech) {
                state = VadState::SPEECH;
                silenceCnt = 0;
                if (speechCnt >= maxSpeechFrame) {
                    hitMaxSpeech = true;
                    segments.push_back({lastSpeechStartFrame, frameCnt - 1});
                    lastSpeechEndFrame = frameCnt - 1;
                    lastSpeechStartFrame = -1;
                    speechCnt = 0;
                }
            } else {
                silenceCnt++;
                if (silenceCnt >= minSilenceFrame) {
                    state = VadState::SILENCE;
                    segments.push_back({lastSpeechStartFrame, frameCnt - 1});
                    lastSpeechEndFrame = frameCnt - 1;
                    lastSpeechStartFrame = -1;
                    speechCnt = 0;
                }
            }
            break;
        }
    }

    void Flush() {
        if (lastSpeechStartFrame >= 0) {
            segments.push_back({lastSpeechStartFrame, frameCnt - 1});
            lastSpeechStartFrame = -1;
        }
    }

    bool IsInSpeech() const {
        return state == VadState::SPEECH || state == VadState::POSSIBLE_SPEECH;
    }
};

class FireRedVad {
public:
    ~FireRedVad() = default;

    static std::unique_ptr<FireRedVad> Create(const FireRedVadConfig& cfg) {
        auto vad = std::unique_ptr<FireRedVad>(new FireRedVad());
        vad->threshold_ = cfg.threshold;

        vad->env_ = std::make_unique<Ort::Env>(ORT_LOGGING_LEVEL_WARNING, "FireRedVad");
        Ort::SessionOptions so;
        so.SetIntraOpNumThreads(1);
        so.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);

        std::wstring wpath = Utf8ToWide(cfg.modelPath);
        vad->session_ = std::make_unique<Ort::Session>(*vad->env_, wpath.c_str(), so);

        Ort::AllocatorWithDefaultOptions alloc;
        {
            auto n = vad->session_->GetInputNameAllocated(0, alloc);
            vad->inputNameStr_ = n.get();
        }
        {
            auto n = vad->session_->GetInputNameAllocated(1, alloc);
            vad->cachesInNameStr_ = n.get();
        }
        {
            auto n = vad->session_->GetOutputNameAllocated(0, alloc);
            vad->outputNameStr_ = n.get();
        }
        {
            auto n = vad->session_->GetOutputNameAllocated(1, alloc);
            vad->cachesOutNameStr_ = n.get();
        }

        vad->inputNames_ = {vad->inputNameStr_.c_str(), vad->cachesInNameStr_.c_str()};
        vad->outputNames_ = {vad->outputNameStr_.c_str(), vad->cachesOutNameStr_.c_str()};

        vad->cacheData_.resize(8 * 1 * 128 * 19, 0.0f);

        vad->fbank_ = std::make_unique<knf::OnlineGenericBaseFeature<knf::FbankComputer>>(MakeFbankOpts());
        vad->fbankFrame_ = 0;

        vad->postprocessor_ = std::make_unique<StreamVadPostprocessor>(
            cfg.threshold, cfg.smoothWindowSize, cfg.padStartMs / 10,
            cfg.minSpeechMs / 10, cfg.maxSpeechFrame, cfg.minSilenceMs / 10);

        return vad;
    }

    void Reset() {
        std::fill(cacheData_.begin(), cacheData_.end(), 0.0f);
        fbank_ = std::make_unique<knf::OnlineGenericBaseFeature<knf::FbankComputer>>(MakeFbankOpts());
        fbankFrame_ = 0;
        postprocessor_->Reset();
        anySpeechSeen_ = false;
    }

    void Process(const float* samples, int nSamples) {
        scaledBuf_.resize(nSamples);
        for (int i = 0; i < nSamples; i++)
            scaledBuf_[i] = samples[i] * 32768.0f;

        fbank_->AcceptWaveform(kSampleRate, scaledBuf_.data(), nSamples);

        int nReady = fbank_->NumFramesReady();
        while (fbankFrame_ < nReady) {
            const float* frame = fbank_->GetFrame(fbankFrame_++);
            float feat[kFbankBins];
            for (int d = 0; d < kFbankBins; d++)
                feat[d] = (frame[d] - kCmvnMeans[d]) * kCmvnIstd[d];
            float prob = ProcessOneFrame(feat);
            postprocessor_->ProcessOneFrame(prob);
            if (prob >= threshold_) anySpeechSeen_ = true;
        }
    }

    bool HasSpeech() const {
        return anySpeechSeen_ || !postprocessor_->segments.empty();
    }

    bool IsInSpeech() const {
        return postprocessor_->IsInSpeech();
    }

    void Flush() {
        postprocessor_->Flush();
    }

    std::vector<float> GetConcatenatedSamples(const float* samples, int nSamples) const {
        std::vector<float> out;
        if (postprocessor_->segments.empty()) return out;

        for (auto& seg : postprocessor_->segments) {
            int startSample = std::max(0, seg.first * kFrameShift);
            int endSample = std::min(nSamples, (seg.second + 1) * kFrameShift);
            if (endSample > startSample) {
                out.insert(out.end(), samples + startSample, samples + endSample);
            }
        }
        return out;
    }

    float LastProb() const { return lastProb_; }

private:
    FireRedVad() = default;

    float ProcessOneFrame(const float* featFrame) {
        Ort::MemoryInfo memInfo = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);

        int64_t featShape[] = {1, 1, kFbankBins};
        auto featTensor = Ort::Value::CreateTensor<float>(memInfo,
            const_cast<float*>(featFrame), kFbankBins, featShape, 3);

        int64_t cacheShape[] = {8, 1, 128, 19};
        auto cacheTensor = Ort::Value::CreateTensor<float>(memInfo,
            cacheData_.data(), cacheData_.size(), cacheShape, 4);

        std::vector<Ort::Value> inputs;
        inputs.push_back(std::move(featTensor));
        inputs.push_back(std::move(cacheTensor));

        auto outputs = session_->Run(Ort::RunOptions{nullptr},
            inputNames_.data(), inputs.data(), 2,
            outputNames_.data(), 2);

        float prob = outputs[0].GetTensorMutableData<float>()[0];
        lastProb_ = prob;

        auto& cacheOut = outputs[1];
        const float* cacheOutData = cacheOut.GetTensorData<float>();
        auto outShape = cacheOut.GetTensorTypeAndShapeInfo().GetShape();
        size_t cacheOutSize = 1;
        for (auto s : outShape) cacheOutSize *= static_cast<size_t>(s);
        std::memcpy(cacheData_.data(), cacheOutData, cacheOutSize * sizeof(float));

        return prob;
    }

    std::unique_ptr<Ort::Env> env_;
    std::unique_ptr<Ort::Session> session_;
    std::vector<float> cacheData_;
    std::unique_ptr<knf::OnlineGenericBaseFeature<knf::FbankComputer>> fbank_;
    int fbankFrame_ = 0;
    std::vector<float> scaledBuf_;
    std::unique_ptr<StreamVadPostprocessor> postprocessor_;

    std::string inputNameStr_;
    std::string cachesInNameStr_;
    std::string outputNameStr_;
    std::string cachesOutNameStr_;
    std::vector<const char*> inputNames_;
    std::vector<const char*> outputNames_;

    float threshold_ = 0.5f;
    float lastProb_ = 0.0f;
    bool anySpeechSeen_ = false;
};

} // namespace firered_vad
