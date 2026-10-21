#include "../include/convolution_filter.h"

#include <assert.h>
#include <string.h>

ConvolutionFilter::ConvolutionFilter() {
    m_shiftRegister = nullptr;
    m_impulseResponse = nullptr;

    m_shiftOffset = 0;
    m_sampleCount = 0;
}

ConvolutionFilter::~ConvolutionFilter() {
    assert(m_shiftRegister == nullptr);
    assert(m_impulseResponse == nullptr);
}

// ATG_ENGINE_SIM_FAST_CONVOLUTION keeps the shift register twice over (every
// sample at offset and offset + samples), so the window is contiguous, and sums
// with independent accumulators the compiler can vectorize. The result differs
// from the sequential sum by float rounding only, so it is opt-in (the wasm build)
// and the native renders stay bit-identical.
#ifdef ATG_ENGINE_SIM_FAST_CONVOLUTION
static constexpr int ShiftRegisterCopies = 2;
#else
static constexpr int ShiftRegisterCopies = 1;
#endif

void ConvolutionFilter::initialize(int samples) {
    m_sampleCount = samples;
    m_shiftOffset = 0;
    m_shiftRegister = new float[(size_t)samples * ShiftRegisterCopies];
    m_impulseResponse = new float[samples];

    memset(m_shiftRegister, 0, sizeof(float) * samples * ShiftRegisterCopies);
    memset(m_impulseResponse, 0, sizeof(float) * samples);
}

void ConvolutionFilter::destroy() {
    delete[] m_shiftRegister;
    delete[] m_impulseResponse;

    m_shiftRegister = nullptr;
    m_impulseResponse = nullptr;
}

float ConvolutionFilter::f(float sample) {
#ifdef ATG_ENGINE_SIM_FAST_CONVOLUTION
    m_shiftRegister[m_shiftOffset] = sample;
    m_shiftRegister[m_shiftOffset + m_sampleCount] = sample;

    const float *window = m_shiftRegister + m_shiftOffset;
    const float *ir = m_impulseResponse;
    constexpr int Lanes = 8;
    float acc[Lanes] = {};
    int i = 0;
    for (; i + Lanes <= m_sampleCount; i += Lanes) {
        for (int j = 0; j < Lanes; ++j) {
            acc[j] += ir[i + j] * window[i + j];
        }
    }

    float result = 0;
    for (; i < m_sampleCount; ++i) result += ir[i] * window[i];
    for (int j = 0; j < Lanes; ++j) result += acc[j];

    m_shiftOffset = (m_shiftOffset - 1 + m_sampleCount) % m_sampleCount;

    return result;
#else
    m_shiftRegister[m_shiftOffset] = sample;

    float result = 0;
    for (int i = 0; i < m_sampleCount - m_shiftOffset; ++i) {
        result += m_impulseResponse[i] * m_shiftRegister[i + m_shiftOffset];
    }

    for (int i = m_sampleCount - m_shiftOffset; i < m_sampleCount; ++i) {
        result += m_impulseResponse[i] * m_shiftRegister[i - (m_sampleCount - m_shiftOffset)];
    }

    m_shiftOffset = (m_shiftOffset - 1 + m_sampleCount) % m_sampleCount;

    return result;
#endif /* ATG_ENGINE_SIM_FAST_CONVOLUTION */
}
