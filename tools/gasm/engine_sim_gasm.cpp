// Engine Sim as a gasm guest (step 1: simulator + synthesizer, no UI).
//
// Loads an engine script from the assets, runs the simulation for one frame of
// time per gasm frame and pushes the synthesizer's output as audio. The
// synthesizer runs synchronously on the frame (no audio thread), as in
// tools/engine_sim_cli.cpp. A small framebuffer shows rpm and throttle.
//
// Assets: mount the repository's es/, assets/ and tools/ under the same names
//   gasm-run engine-sim.wasm --asset-dir es=es --asset-dir assets=assets --asset-dir tools=tools
//
// Params:
//   script=<asset>   engine script (default tools/engine-sounds/r4.mr)
//   mode=play|bench  play: pad UP opens the throttle, A cranks (default)
//                    bench: the dyno holds rpm=<n> at wide-open throttle
//   rpm=<n>          bench rpm (default 3000)
//   sim_freq=<hz>    override the script's simulation frequency
//   capture=<n>      save the first n frames of audio as storage key capture.wav
//                    (16-bit mono; the Node runner's --storage-dir writes it out)

#include "gasm.h"

#include "../../include/engine.h"
#include "../../include/simulator.h"
#include "../../include/units.h"
#include "../../scripting/include/compiler.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

constexpr int AudioSampleRate = 44100;
constexpr double FrameRate = 60.0;
constexpr int Width = 160;
constexpr int Height = 40;

void log(const std::string &s) { gasm_log(s.c_str(), (uint32_t)s.size()); }

std::string param(const char *name, const std::string &fallback) {
    char buf[256];
    const int32_t n = gasm_param(name, (uint32_t)std::strlen(name), buf, sizeof(buf));
    return (n >= 0 && n <= (int32_t)sizeof(buf)) ? std::string(buf, (size_t)n) : fallback;
}

bool readAsset(const std::string &name, std::string *contents) {
    const int32_t size = gasm_asset_size(name.c_str(), (uint32_t)name.size());
    if (size < 0) return false;
    if (contents == nullptr) return true;
    contents->resize((size_t)size);
    return gasm_asset_read(name.c_str(), (uint32_t)name.size(), contents->data(), (uint32_t)size) == size;
}

// 16-bit PCM mono (or first channel) from a RIFF/WAVE asset
bool readWav16(const std::string &name, std::vector<int16_t> *out) {
    std::string data;
    if (!readAsset(name, &data) || data.size() < 12) return false;
    if (std::memcmp(data.data(), "RIFF", 4) != 0 || std::memcmp(data.data() + 8, "WAVE", 4) != 0) return false;

    uint16_t channels = 1, bits = 16;
    size_t pos = 12;
    while (pos + 8 <= data.size()) {
        uint32_t size;
        std::memcpy(&size, data.data() + pos + 4, 4);
        const char *id = data.data() + pos;
        const char *body = data.data() + pos + 8;
        if (pos + 8 + size > data.size()) size = (uint32_t)(data.size() - pos - 8);

        if (std::memcmp(id, "fmt ", 4) == 0 && size >= 16) {
            std::memcpy(&channels, body + 2, 2);
            std::memcpy(&bits, body + 14, 2);
        }
        else if (std::memcmp(id, "data", 4) == 0) {
            if (bits != 16 || channels == 0) return false;
            out->clear();
            for (size_t i = 0; i + 2 * channels <= size; i += 2 * channels) {
                int16_t s;
                std::memcpy(&s, body + i, 2);
                out->push_back(s);
            }
            return true;
        }
        pos += 8 + size + (size & 1);
    }
    return false;
}

struct State {
    Engine *engine = nullptr;
    Simulator *simulator = nullptr;
    bool bench = false;
    double benchRpm = 3000;
    double speedControl = 0;
    std::vector<float> audio;
    uint8_t fb[Width * Height * 4] = {};
    uint64_t frames = 0;
    uint64_t steps = 0;
    uint64_t captureFrames = 0;
    std::vector<int16_t> capture;
} g;

void saveCapture() {
    const uint32_t dataSize = (uint32_t)(g.capture.size() * 2);
    std::string wav;
    auto u32 = [&](uint32_t v) { wav.append(reinterpret_cast<const char *>(&v), 4); };
    auto u16 = [&](uint16_t v) { wav.append(reinterpret_cast<const char *>(&v), 2); };
    wav += "RIFF"; u32(36 + dataSize); wav += "WAVE";
    wav += "fmt "; u32(16); u16(1); u16(1); u32(AudioSampleRate); u32(AudioSampleRate * 2); u16(2); u16(16);
    wav += "data"; u32(dataSize);
    wav.append(reinterpret_cast<const char *>(g.capture.data()), dataSize);
    const char key[] = "capture.wav";
    gasm_storage_set(key, sizeof(key) - 1, wav.data(), (uint32_t)wav.size());
    log("engine-sim: saved " + std::to_string(g.capture.size()) + " samples to capture.wav");
}

bool load() {
    piranha::setFileReader(readAsset);

    const std::string script = param("script", "tools/engine-sounds/r4.mr");
    es_script::Compiler compiler;
    compiler.initialize();
    if (!compiler.compile(script.c_str())) {
        log("engine-sim: failed to compile " + script);
        compiler.destroy();
        return false;
    }
    const es_script::Compiler::Output output = compiler.execute();
    compiler.destroy();

    if (output.engine == nullptr || output.vehicle == nullptr || output.transmission == nullptr) {
        log("engine-sim: the script set no engine, vehicle and transmission");
        return false;
    }

    g.engine = output.engine;
    g.engine->calculateDisplacement();
    g.simulator = g.engine->createSimulator(output.vehicle, output.transmission);

    const int simFreq = std::atoi(param("sim_freq", "0").c_str());
    g.simulator->setSimulationFrequency(simFreq > 0 ? simFreq : (int)g.engine->getSimulationFrequency());
    // The synthesizer is drained every frame, so one frame of input is the steady latency
    g.simulator->setTargetSynthesizerLatency(1.0 / FrameRate);

    Synthesizer &synth = g.simulator->synthesizer();
    Synthesizer::AudioParameters audio = synth.getAudioParameters();
    audio.inputSampleNoise = (float)g.engine->getInitialJitter();
    audio.airNoise = (float)g.engine->getInitialNoise();
    audio.dF_F_mix = (float)g.engine->getInitialHighFrequencyGain();
    // The leveler aims peaks at levelerTarget, but its smoothed gain overshoots
    // on transients; the app's 30000 (-0.8 dBFS) clips, so leave headroom
    audio.levelerTarget = 12000.0f;
    synth.setAudioParameters(audio);

    for (int i = 0; i < g.engine->getExhaustSystemCount(); ++i) {
        ImpulseResponse *response = g.engine->getExhaustSystem(i)->getImpulseResponse();
        std::vector<int16_t> ir;
        if (!readWav16(response->getFilename(), &ir)) {
            log("engine-sim: can't read impulse response " + response->getFilename());
            return false;
        }
        synth.initializeImpulseResponse(ir.data(), (unsigned int)ir.size(), (float)response->getVolume(), i);
    }

    // Drop the silence the synthesizer pre-fills its output buffer with
    std::vector<int16_t> discard(synth.m_audioBuffer.size());
    synth.readAudioOutput((int)discard.size(), discard.data());

    g.engine->getIgnitionModule()->m_enabled = true;
    log("engine-sim: " + g.engine->getName() + ", " + std::to_string(g.engine->getCylinderCount())
        + " cylinders, " + std::to_string(g.simulator->getSimulationFrequency()) + " Hz");
    return true;
}

// Runs the synthesizer on the input the last frame produced (see the CLI's
// renderAvailable): unclamped samples in int16 units
void renderAudio() {
    Synthesizer &synth = g.simulator->synthesizer();
    const int n = (int)synth.m_inputChannels[0].data.size();
    if (n == 0) return;

    for (int i = 0; i < synth.m_inputChannelCount; ++i) {
        synth.m_inputChannels[i].data.read(n, synth.m_inputChannels[i].transferBuffer);
        synth.m_filters[i].airNoiseLowPass.setCutoffFrequency(
            synth.m_audioParameters.airNoiseFrequencyCutoff, synth.m_audioSampleRate);
        synth.m_filters[i].jitterFilter.setJitterScale(synth.m_audioParameters.inputSampleNoise);
    }
    synth.m_inputSamplesRead = n;
    synth.m_processed = true;

    for (int i = 0; i < n; ++i) {
        const float s = synth.renderAudioRaw(i) / 32768.0f;
        g.audio.push_back(std::clamp(s, -1.0f, 1.0f));
    }
}

void controls() {
    Dynamometer &dyno = g.simulator->m_dyno;
    if (g.bench) {
        dyno.m_enabled = dyno.m_hold = g.engine->getRpm() > 100;
        dyno.m_rotationSpeed = units::rpm(g.benchRpm);
        g.simulator->m_starterMotor.m_enabled = g.engine->getRpm() < 100;
        g.speedControl = 1.0;
    }
    else {
        const uint32_t pad = gasm_input_pad(0);
        const double target = (pad & GASM_BTN_UP) ? 1.0 : 0.0;
        g.speedControl += (target - g.speedControl) * 0.5;
        g.simulator->m_starterMotor.m_enabled = (pad & GASM_BTN_A) != 0;
    }
    g.engine->setSpeedControl(g.speedControl);
}

void draw() {
    const double rpm = g.engine->getRpm();
    const double redline = units::toRpm(g.engine->getRedline());
    const int rpmBar = (int)std::clamp(rpm / redline * Width, 0.0, (double)Width);
    const int throttleBar = (int)std::clamp(g.speedControl * Width, 0.0, (double)Width);
    for (int y = 0; y < Height; ++y) {
        for (int x = 0; x < Width; ++x) {
            uint8_t *p = &g.fb[(y * Width + x) * 4];
            const bool top = y < 28, inBar = top ? x < rpmBar : x < throttleBar;
            p[0] = inBar ? (top ? (rpm > redline * 0.9 ? 239 : 119) : 244) : 14;
            p[1] = inBar ? (top ? (rpm > redline * 0.9 ? 69 : 206) : 128) : 16;
            p[2] = inBar ? (top ? (rpm > redline * 0.9 ? 69 : 224) : 42) : 18;
            p[3] = 255;
        }
    }
    gasm_video_present(g.fb, Width, Height, Width * 4);
}

} /* namespace */

GASM_EXPORT("gasm_abi_version") int32_t abi_version(void) { return GASM_ABI_VERSION; }

GASM_EXPORT("gasm_init") int32_t init(void) {
    gasm_set_frame_rate(FrameRate);
    gasm_audio_config(AudioSampleRate, 1);
    g.bench = param("mode", "play") == "bench";
    g.benchRpm = std::atof(param("rpm", "3000").c_str());
    g.captureFrames = (uint64_t)std::atoll(param("capture", "0").c_str());
    return load() ? 0 : 1;
}

GASM_EXPORT("gasm_frame") void frame(void) {
    controls();

    g.simulator->startFrame(1.0 / FrameRate);
    while (g.simulator->simulateStep()) { /* void */ }
    g.steps += g.simulator->getFrameIterationCount();
    g.simulator->endFrame();

    g.audio.clear();
    renderAudio();
    if (!g.audio.empty()) gasm_audio_push(g.audio.data(), (uint32_t)g.audio.size());
    if (g.frames < g.captureFrames) {
        for (float s : g.audio) g.capture.push_back((int16_t)std::lround(s * 32767.0f));
        if (g.frames + 1 == g.captureFrames) saveCapture();
    }

    draw();

    if (++g.frames % 60 == 0) {
        char line[160];
        std::snprintf(line, sizeof(line), "engine-sim: frame %llu  rpm %.0f  steps/frame %.0f",
            (unsigned long long)g.frames, g.engine->getRpm(), (double)g.steps / g.frames);
        log(line);
    }
}
