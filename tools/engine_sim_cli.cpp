// Offline engine sound renderer.
//
// Loads an engine script, holds the crankshaft at fixed speeds with the dyno
// and writes steady on-load / off-load takes as mono 16-bit WAV files. Runs
// the synthesizer synchronously (no audio thread, no real-time pacing), so the
// output is deterministic for a given script and seed.

#include "../include/engine.h"
#include "../include/simulator.h"
#include "../include/units.h"
#include "../scripting/include/compiler.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace {

constexpr int AudioSampleRate = 44100;
constexpr double FrameDt = 1 / 60.0;

struct Options {
    std::string script;
    std::string outDir = ".";
    std::string kind = "engine";
    std::vector<double> rpms;
    int points = 7;
    double idleRpm = 0;
    double topFraction = 0.93;
    double settle = 2.0;
    double duration = 3.0;
    double peakDb = -1.0;
    int simFrequency = 0;
    unsigned int seed = 1;
    bool probe = false;
    bool dry = false;
    bool fuelCut = false;
};

struct Take {
    double rpm;
    bool onLoad;
    std::vector<float> samples;
    double meanRpm = 0, minRpm = 0, maxRpm = 0;
    double meanDynoTorque = 0;
};

void usage() {
    std::cerr <<
        "usage: engine-sim-cli --script <main.mr> [options]\n"
        "  --probe              print natural idle, redline and suggested rpm points, then exit\n"
        "  --kind <name>        file name prefix (default: engine)\n"
        "  --out <dir>          output directory (default: .)\n"
        "  --rpms a,b,c         explicit rpm points (default: idle + geometric spacing)\n"
        "  --idle <rpm>         idle rpm to use instead of the measured one\n"
        "  --points <n>         points above idle when --rpms is not given (default: 7)\n"
        "  --top <fraction>     highest point as a fraction of redline (default: 0.93)\n"
        "  --settle <s>         settling time before each take (default: 2.0)\n"
        "  --duration <s>       take length (default: 3.0)\n"
        "  --peak-db <dB>       peak level of the loudest take (default: -1.0)\n"
        "  --sim-freq <hz>      override the script's simulation frequency\n"
        "  --seed <n>           noise seed (default: 1)\n"
        "  --dry                bypass the exhaust impulse response (convolution off)\n"
        "  --fuel-cut           off-load takes with the throttle open and the ignition off\n";
}

bool parseArgs(int argc, char **argv, Options *opt) {
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> std::string {
            if (i + 1 >= argc) { usage(); std::exit(2); }
            return argv[++i];
        };

        if (a == "--script") opt->script = next();
        else if (a == "--out") opt->outDir = next();
        else if (a == "--kind") opt->kind = next();
        else if (a == "--idle") opt->idleRpm = std::stod(next());
        else if (a == "--points") opt->points = std::stoi(next());
        else if (a == "--top") opt->topFraction = std::stod(next());
        else if (a == "--settle") opt->settle = std::stod(next());
        else if (a == "--duration") opt->duration = std::stod(next());
        else if (a == "--peak-db") opt->peakDb = std::stod(next());
        else if (a == "--sim-freq") opt->simFrequency = std::stoi(next());
        else if (a == "--seed") opt->seed = (unsigned int)std::stoul(next());
        else if (a == "--probe") opt->probe = true;
        else if (a == "--dry") opt->dry = true;
        else if (a == "--fuel-cut") opt->fuelCut = true;
        else if (a == "--rpms") {
            std::stringstream ss(next());
            std::string item;
            while (std::getline(ss, item, ',')) opt->rpms.push_back(std::stod(item));
        }
        else { usage(); return false; }
    }

    if (opt->script.empty()) { usage(); return false; }
    return true;
}

// Minimal RIFF reader for the 16-bit PCM impulse responses in assets/sound-library.
bool readWav16(const std::string &path, std::vector<int16_t> *out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;

    char riff[12];
    f.read(riff, 12);
    if (!f || std::memcmp(riff, "RIFF", 4) != 0 || std::memcmp(riff + 8, "WAVE", 4) != 0) return false;

    uint16_t channels = 1, bits = 16;
    while (f) {
        char id[4];
        uint32_t size;
        f.read(id, 4);
        f.read(reinterpret_cast<char *>(&size), 4);
        if (!f) break;

        if (std::memcmp(id, "fmt ", 4) == 0) {
            std::vector<char> fmt(size);
            f.read(fmt.data(), size);
            std::memcpy(&channels, fmt.data() + 2, 2);
            std::memcpy(&bits, fmt.data() + 14, 2);
        }
        else if (std::memcmp(id, "data", 4) == 0) {
            if (bits != 16) return false;
            std::vector<int16_t> raw(size / 2);
            f.read(reinterpret_cast<char *>(raw.data()), size);
            out->clear();
            for (size_t i = 0; i + channels <= raw.size(); i += channels) out->push_back(raw[i]);
            return true;
        }
        else {
            f.seekg(size + (size & 1), std::ios::cur);
        }
    }

    return false;
}

bool writeWav16(const std::string &path, const std::vector<float> &samples, double gain) {
    std::ofstream f(path, std::ios::binary);
    if (!f) return false;

    const uint32_t dataSize = (uint32_t)(samples.size() * 2);
    auto u32 = [&](uint32_t v) { f.write(reinterpret_cast<const char *>(&v), 4); };
    auto u16 = [&](uint16_t v) { f.write(reinterpret_cast<const char *>(&v), 2); };

    f.write("RIFF", 4); u32(36 + dataSize); f.write("WAVE", 4);
    f.write("fmt ", 4); u32(16); u16(1); u16(1); u32(AudioSampleRate); u32(AudioSampleRate * 2); u16(2); u16(16);
    f.write("data", 4); u32(dataSize);

    for (float s : samples) {
        const long v = std::lround(s * gain);
        u16((uint16_t)(int16_t)std::clamp(v, (long)INT16_MIN, (long)INT16_MAX));
    }

    return (bool)f;
}

class Renderer {
public:
    bool load(const Options &opt) {
        es_script::Compiler compiler;
        compiler.initialize();
        if (!compiler.compile(opt.script.c_str())) {
            std::cerr << "error: failed to compile " << opt.script << " (see error_log.log)\n";
            compiler.destroy();
            return false;
        }

        const es_script::Compiler::Output output = compiler.execute();
        compiler.destroy();

        m_engine = output.engine;
        if (m_engine == nullptr || output.vehicle == nullptr || output.transmission == nullptr) {
            std::cerr << "error: script did not set an engine, vehicle and transmission\n";
            return false;
        }

        m_engine->calculateDisplacement();
        m_simulator = m_engine->createSimulator(output.vehicle, output.transmission);
        m_simulator->setSimulationFrequency(
            opt.simFrequency > 0 ? opt.simFrequency : (int)m_engine->getSimulationFrequency());

        // Pin the leveler at unity: gain is applied once per engine when writing files
        Synthesizer::AudioParameters audio = m_simulator->synthesizer().getAudioParameters();
        audio.inputSampleNoise = (float)m_engine->getInitialJitter();
        audio.airNoise = (float)m_engine->getInitialNoise();
        audio.dF_F_mix = (float)m_engine->getInitialHighFrequencyGain();
        audio.levelerMinGain = 1.0f;
        audio.levelerMaxGain = 1.0f;
        audio.volume = 1.0f;
        if (opt.dry) audio.convolution = 0.0f;
        m_simulator->synthesizer().setAudioParameters(audio);
        m_simulator->synthesizer().m_levelingFilter.p_minLevel = 1.0f;
        m_simulator->synthesizer().m_levelingFilter.p_maxLevel = 1.0f;

        for (int i = 0; i < m_engine->getExhaustSystemCount(); ++i) {
            ImpulseResponse *response = m_engine->getExhaustSystem(i)->getImpulseResponse();
            std::vector<int16_t> ir;
            if (!readWav16(response->getFilename(), &ir)) {
                std::cerr << "error: can't read impulse response " << response->getFilename() << "\n";
                return false;
            }

            m_simulator->synthesizer().initializeImpulseResponse(
                ir.data(), (unsigned int)ir.size(), (float)response->getVolume(), i);
        }

        // Drop the silence the synthesizer pre-fills its output buffer with
        Synthesizer &synth = m_simulator->synthesizer();
        std::vector<int16_t> discard(synth.m_audioBuffer.size());
        synth.readAudioOutput((int)discard.size(), discard.data());

        m_engine->getIgnitionModule()->m_enabled = true;
        crank();
        return true;
    }

    // Spins the engine up with the starter so it turns in its normal direction.
    // Starting from rest on the dyno would pick the direction arbitrarily (the
    // dyno follows the sign of the current crank velocity) and can run the
    // engine backwards, with valve and ignition timing reversed.
    void crank() {
        const double target = 0.8 * units::toRpm(m_engine->getStarterSpeed());
        m_simulator->m_starterMotor.m_enabled = true;
        for (int i = 0; i < (int)(3.0 / FrameDt) && m_engine->getRpm() < target; ++i) {
            run(FrameDt, nullptr, nullptr, nullptr);
        }
        m_simulator->m_starterMotor.m_enabled = false;
        std::fprintf(stderr, "cranked to %.0f rpm (%s)\n",
            m_engine->getRpm(), m_engine->isSpinningCw() ? "cw" : "ccw");
    }

    Engine *engine() const { return m_engine; }

    void setControls(bool dyno, double rpm, double speedControl) {
        m_simulator->m_dyno.m_enabled = dyno;
        m_simulator->m_dyno.m_hold = dyno;
        m_simulator->m_dyno.m_rotationSpeed = units::rpm(rpm);
        m_engine->setSpeedControl(speedControl);
    }

    // Advances the simulation by at least `seconds` of audio, appending samples
    // to `out` (if given) and the crank speed of every frame to `rpm` (if given).
    void run(double seconds, std::vector<float> *out, std::vector<double> *rpm, std::vector<double> *torque) {
        const size_t target = (size_t)std::ceil(seconds * AudioSampleRate);
        size_t produced = 0;
        while (produced < target) {
            m_simulator->startFrame(FrameDt);
            while (m_simulator->simulateStep()) { /* void */ }
            m_simulator->endFrame();

            produced += renderAvailable(out, target - produced);
            if (rpm != nullptr) rpm->push_back(m_engine->getRpm());
            if (torque != nullptr) torque->push_back(m_simulator->getFilteredDynoTorque());
        }
    }

    // Moves the dyno set point gradually to avoid a speed step during settling
    void ramp(double fromRpm, double toRpm, double speedControl, double seconds) {
        const int frames = std::max(1, (int)(seconds / FrameDt));
        for (int i = 1; i <= frames; ++i) {
            setControls(true, fromRpm + (toRpm - fromRpm) * i / frames, speedControl);
            run(FrameDt, nullptr, nullptr, nullptr);
        }
    }

private:
    // Single-threaded equivalent of Synthesizer::renderAudio(), producing
    // unclamped float samples.
    size_t renderAvailable(std::vector<float> *out, size_t want) {
        Synthesizer &synth = m_simulator->synthesizer();
        const int n = (int)synth.m_inputChannels[0].data.size();
        if (n == 0) return 0;

        for (int i = 0; i < synth.m_inputChannelCount; ++i) {
            synth.m_inputChannels[i].data.read(n, synth.m_inputChannels[i].transferBuffer);
            synth.m_filters[i].airNoiseLowPass.setCutoffFrequency(
                synth.m_audioParameters.airNoiseFrequencyCutoff, synth.m_audioSampleRate);
            synth.m_filters[i].jitterFilter.setJitterScale(synth.m_audioParameters.inputSampleNoise);
        }
        synth.m_inputSamplesRead = n;
        synth.m_processed = true;

        for (int i = 0; i < n; ++i) {
            const float s = synth.renderAudioRaw(i);
            if (out != nullptr && (size_t)i < want) out->push_back(s);
        }

        return (size_t)n;
    }

    Engine *m_engine = nullptr;
    Simulator *m_simulator = nullptr;
};

double roundTo(double v, double step) { return std::round(v / step) * step; }

void stats(const std::vector<double> &v, double *mean, double *lo, double *hi) {
    *mean = 0; *lo = 1e300; *hi = -1e300;
    for (double x : v) { *mean += x; *lo = std::min(*lo, x); *hi = std::max(*hi, x); }
    if (!v.empty()) *mean /= v.size();
}

double measureIdle(Renderer &r) {
    // Spin up on the dyno, release it with the throttle closed and let it settle
    r.setControls(true, 1200, 0.0);
    r.run(2.0, nullptr, nullptr, nullptr);
    r.setControls(false, 0, 0.0);
    r.run(4.0, nullptr, nullptr, nullptr);

    std::vector<double> rpm;
    r.run(2.0, nullptr, &rpm, nullptr);
    double mean, lo, hi;
    stats(rpm, &mean, &lo, &hi);
    return mean;
}

std::vector<double> defaultPoints(double idle, double redline, int points, double topFraction) {
    std::vector<double> rpms = { roundTo(idle, 50) };
    const double top = redline * topFraction;
    const double ratio = std::pow(top / idle, 1.0 / points);
    for (int i = 1; i <= points; ++i) {
        rpms.push_back(roundTo(idle * std::pow(ratio, i), i == points ? 50 : 100));
    }
    return rpms;
}

} /* namespace */

int main(int argc, char **argv) {
    Options opt;
    if (!parseArgs(argc, argv, &opt)) return 2;
    std::srand(opt.seed);

    Renderer renderer;
    if (!renderer.load(opt)) return 1;

    Engine *engine = renderer.engine();
    const double redline = units::toRpm(engine->getRedline());

    double idle = opt.idleRpm;
    if (opt.probe || (opt.rpms.empty() && idle <= 0)) {
        idle = measureIdle(renderer);
        std::cerr << "engine: " << engine->getName()
            << "\ncylinders: " << engine->getCylinderCount()
            << "\nsimulation frequency: " << engine->getSimulationFrequency()
            << "\nnatural idle: " << std::lround(idle) << " rpm"
            << "\nredline: " << std::lround(redline) << " rpm\n";
    }

    std::vector<double> rpms = opt.rpms.empty()
        ? defaultPoints(opt.idleRpm > 0 ? opt.idleRpm : idle, redline, opt.points, opt.topFraction)
        : opt.rpms;

    if (opt.probe) {
        std::cerr << "suggested rpms:";
        for (size_t i = 0; i < rpms.size(); ++i) std::cerr << (i ? "," : " ") << rpms[i];
        std::cerr << "\n";
        return 0;
    }

    std::vector<Take> takes;
    double previousRpm = rpms.front();
    renderer.setControls(true, previousRpm, 1.0);
    renderer.run(opt.settle, nullptr, nullptr, nullptr);

    for (double rpm : rpms) {
        for (bool onLoad : { true, false }) {
            // Off load is a closed throttle by default; with --fuel-cut it is an
            // open throttle with the ignition off (an unthrottled diesel on overrun)
            const bool fuelCut = !onLoad && opt.fuelCut;
            const double speedControl = (onLoad || fuelCut) ? 1.0 : 0.0;
            engine->getIgnitionModule()->m_enabled = !fuelCut;
            renderer.ramp(previousRpm, rpm, speedControl, std::min(0.5, opt.settle));
            renderer.run(std::max(0.0, opt.settle - 0.5), nullptr, nullptr, nullptr);
            previousRpm = rpm;

            Take take;
            take.rpm = rpm;
            take.onLoad = onLoad;
            std::vector<double> rpmTrace, torqueTrace;
            renderer.run(opt.duration, &take.samples, &rpmTrace, &torqueTrace);
            stats(rpmTrace, &take.meanRpm, &take.minRpm, &take.maxRpm);
            double lo, hi;
            stats(torqueTrace, &take.meanDynoTorque, &lo, &hi);

            std::fprintf(stderr, "%6.0f %-3s  rpm %.0f [%.0f..%.0f]  dyno %.1f N*m\n",
                rpm, onLoad ? "on" : "off", take.meanRpm, take.minRpm, take.maxRpm, take.meanDynoTorque);
            takes.push_back(std::move(take));
        }
    }

    float peak = 0;
    for (const Take &t : takes) {
        for (float s : t.samples) peak = std::max(peak, std::abs(s));
    }
    if (peak <= 0) {
        std::cerr << "error: all takes are silent\n";
        return 1;
    }

    const double gain = std::pow(10.0, opt.peakDb / 20) * INT16_MAX / peak;
    std::fprintf(stderr, "gain: %.6g (raw peak %.1f)\n", gain, peak);

    const std::string manifestPath = opt.outDir + "/" + opt.kind + "_takes.csv";
    std::ofstream manifest(manifestPath);
    manifest << "file,target_rpm,load,mean_rpm,min_rpm,max_rpm,dyno_torque_nm,peak_dbfs,rms_dbfs\n";

    for (const Take &t : takes) {
        char name[256];
        std::snprintf(name, sizeof(name), "%s_%.0f_%s.wav", opt.kind.c_str(), t.rpm, t.onLoad ? "on" : "off");
        const std::string path = opt.outDir + "/" + name;
        if (!writeWav16(path, t.samples, gain)) {
            std::cerr << "error: can't write " << path << "\n";
            return 1;
        }

        double p = 0, sq = 0;
        for (float s : t.samples) { p = std::max(p, (double)std::abs(s * gain)); sq += (s * gain) * (s * gain); }
        const double rms = std::sqrt(sq / t.samples.size());
        manifest << name << "," << t.rpm << "," << (t.onLoad ? "on" : "off") << ","
            << std::lround(t.meanRpm) << "," << std::lround(t.minRpm) << "," << std::lround(t.maxRpm) << ","
            << std::lround(t.meanDynoTorque) << ","
            << 20 * std::log10(p / INT16_MAX) << "," << 20 * std::log10(rms / INT16_MAX) << "\n";
    }

    std::cerr << "wrote " << takes.size() << " takes and " << manifestPath << "\n";
    return 0;
}
