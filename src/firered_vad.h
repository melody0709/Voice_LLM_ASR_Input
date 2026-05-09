#pragma once
// FireRedVAD — streaming VAD using onnxruntime + kaldi_native_fbank
// Model: fireredvad_stream_vad_with_cache.onnx
// Input: 80-dim fbank (25ms window, 10ms shift) + CMVN normalization

#include <cmath>
#include <cstring>
#include <memory>
#include <string>
#include <vector>
#include <algorithm>

#include "onnxruntime_cxx_api.h"
#include "kaldi-native-fbank/csrc/online-feature.h"
#include "utils.h"

namespace firered_vad {

// ---------------------------------------------------------------------------
// CMVN constants (from models/cmvn.ark, dim=80)
// ---------------------------------------------------------------------------
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

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------
constexpr int kSampleRate = 16000;
constexpr int kFbankBins = 80;

// ---------------------------------------------------------------------------
// kaldi_native_fbank options for FireRedVAD
// ---------------------------------------------------------------------------
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

// ---------------------------------------------------------------------------
// FireRedVad config
// ---------------------------------------------------------------------------
struct FireRedVadConfig {
    std::string modelPath;   // path to fireredvad_stream_vad_with_cache.onnx
    float threshold = 0.5f;  // speech probability threshold
};

// ---------------------------------------------------------------------------
// FireRedVad — streaming VAD using onnxruntime
// ---------------------------------------------------------------------------
class FireRedVad {
public:
    ~FireRedVad() = default;

    static std::unique_ptr<FireRedVad> Create(const FireRedVadConfig& cfg) {
        auto vad = std::unique_ptr<FireRedVad>(new FireRedVad());
        vad->threshold_ = cfg.threshold;

        // onnxruntime env + session
        vad->env_ = std::make_unique<Ort::Env>(ORT_LOGGING_LEVEL_WARNING, "FireRedVad");
        Ort::SessionOptions so;
        so.SetIntraOpNumThreads(1);
        so.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);

        // Convert path to wstring for Session constructor
        std::wstring wpath = Utf8ToWide(cfg.modelPath);
        vad->session_ = std::make_unique<Ort::Session>(*vad->env_, wpath.c_str(), so);

        // Get input/output names (store as std::string, keep c_str() pointers)
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

        // Init cache: [8, 1, 128, 19]
        vad->cacheData_.resize(8 * 1 * 128 * 19, 0.0f);

        // Init fbank extractor
        vad->fbank_ = std::make_unique<knf::OnlineGenericBaseFeature<knf::FbankComputer>>(MakeFbankOpts());
        vad->fbankFrame_ = 0;

        return vad;
    }

    void Reset() {
        std::fill(cacheData_.begin(), cacheData_.end(), 0.0f);
        fbank_ = std::make_unique<knf::OnlineGenericBaseFeature<knf::FbankComputer>>(MakeFbankOpts());
        fbankFrame_ = 0;
    }

    // Process a chunk of raw PCM float samples (normalized -1..1).
    // Returns true if speech is detected in any frame.
    bool Process(const float* samples, int nSamples) {
        // FireRedVAD expects int16-scale values (not normalized float)
        scaledBuf_.resize(nSamples);
        for (int i = 0; i < nSamples; i++)
            scaledBuf_[i] = samples[i] * 32768.0f;

        // Feed waveform to fbank extractor
        fbank_->AcceptWaveform(kSampleRate, scaledBuf_.data(), nSamples);

        int nReady = fbank_->NumFramesReady();
        bool anySpeech = false;
        while (fbankFrame_ < nReady) {
            const float* frame = fbank_->GetFrame(fbankFrame_++);
            // Apply CMVN in-place on a copy
            float feat[kFbankBins];
            for (int d = 0; d < kFbankBins; d++)
                feat[d] = (frame[d] - kCmvnMeans[d]) * kCmvnIstd[d];
            float prob = ProcessOneFrame(feat);
            if (prob >= threshold_) anySpeech = true;
        }
        return anySpeech;
    }

    // Get the last probability value (for debugging/UI)
    float LastProb() const { return lastProb_; }

private:
    FireRedVad() = default;

    float ProcessOneFrame(const float* featFrame) {
        Ort::MemoryInfo memInfo = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);

        // feat: [1, 1, 80]
        int64_t featShape[] = {1, 1, kFbankBins};
        auto featTensor = Ort::Value::CreateTensor<float>(memInfo,
            const_cast<float*>(featFrame), kFbankBins, featShape, 3);

        // caches_in: [8, 1, 128, 19]
        int64_t cacheShape[] = {8, 1, 128, 19};
        auto cacheTensor = Ort::Value::CreateTensor<float>(memInfo,
            cacheData_.data(), cacheData_.size(), cacheShape, 4);

        std::vector<Ort::Value> inputs;
        inputs.push_back(std::move(featTensor));
        inputs.push_back(std::move(cacheTensor));

        auto outputs = session_->Run(Ort::RunOptions{nullptr},
            inputNames_.data(), inputs.data(), 2,
            outputNames_.data(), 2);

        // probs: [1, 1, 1]
        float prob = outputs[0].GetTensorMutableData<float>()[0];
        lastProb_ = prob;

        // Copy caches_out back to cacheData_
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
    std::vector<float> cacheData_; // [8*1*128*19]
    std::unique_ptr<knf::OnlineGenericBaseFeature<knf::FbankComputer>> fbank_;
    int fbankFrame_ = 0;
    std::vector<float> scaledBuf_;

    std::string inputNameStr_;
    std::string cachesInNameStr_;
    std::string outputNameStr_;
    std::string cachesOutNameStr_;
    std::vector<const char*> inputNames_;
    std::vector<const char*> outputNames_;

    float threshold_ = 0.5f;
    float lastProb_ = 0.0f;
};

} // namespace firered_vad
