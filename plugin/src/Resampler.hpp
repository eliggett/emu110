// Copyright (c) 2026 Elliott H. Liggett
// SPDX-License-Identifier: GPL-3.0-or-later
/*
    Fixed-ratio polyphase resampler, 32 kHz core -> host rate.

    PLUGIN-PLAN.md section 11 asks for a windowed-sinc polyphase and warns specifically
    against linear interpolation, which "would quietly undo a lot" of the accuracy work.
    That is why this file exists: the emulation is bit-identical to MAME, and throwing
    that away in the last stage of the signal path would be absurd.

    The ratio is rational and known -- 2:3 to 48 kHz, 320:441 to 44.1 kHz -- so the filter
    is designed once at setup and the audio path is a fixed-length dot product with no
    allocation and no division.

    Output j is taken at input position j*M/L, so it needs inputs up to floor(j*M/L).  The
    caller asks inputsFor() how many core frames a block needs, renders exactly that many,
    and hands them over.  Nothing is buffered between blocks except the filter history,
    which is what makes the result independent of the host's buffer size.
*/

#ifndef VOLTAIRE_RESAMPLER_HPP
#define VOLTAIRE_RESAMPLER_HPP

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <numeric>
#include <vector>

namespace voltaire {

class Resampler
{
public:
    /// Taps per phase.  48 with a Kaiser beta of 8 puts the stopband near -100 dB,
    /// comfortably below the 16-bit floor the rest of the chain works to.
    static constexpr unsigned kTaps = 48;

    void setup(double coreRate, double hostRate)
    {
        const long cr = long(coreRate + 0.5), hr = long(hostRate + 0.5);
        const long g = std::gcd(cr, hr);
        m_L = unsigned(hr / g);            // interpolate by L
        m_M = unsigned(cr / g);            // then decimate by M

        // The cutoff has to protect both Nyquists, expressed on the upsampled grid.
        const double cutoff = 0.5 / double(std::max(m_L, m_M)) * 0.93;

        const unsigned n = m_L * kTaps;
        m_taps.assign(n, 0.0f);
        const double beta = 8.0;
        const double i0b = besselI0(beta);
        for (unsigned i = 0; i < n; i ++)
        {
            const double x = double(i) - double(n - 1) / 2.0;
            const double s = (std::abs(x) < 1e-9) ? 2.0 * cutoff
                    : std::sin(2.0 * M_PI * cutoff * x) / (M_PI * x);
            const double r = 2.0 * double(i) / double(n - 1) - 1.0;
            const double w = besselI0(beta * std::sqrt(std::max(0.0, 1.0 - r * r))) / i0b;
            // Phase p, delay k: y = sum_k h[p*kTaps + k] * x[i - k].
            m_taps[size_t(i % m_L) * kTaps + (i / m_L)] = float(s * w * double(m_L));
        }

        reset();
    }

    void reset()
    {
        m_hist.assign(kTaps, 0.0f);
        m_pos = 0;
        m_inNext = 0;
        m_outNext = 0;
    }

    /// Core frames this block needs in order to produce `hostFrames` outputs.
    uint32_t inputsFor(uint32_t hostFrames) const
    {
        if (hostFrames == 0)
            return 0;
        const uint64_t last = ((m_outNext + hostFrames - 1) * m_M) / m_L;
        return uint32_t(last + 1 > m_inNext ? (last + 1 - m_inNext) : 0);
    }

    /// Latency in HOST frames, for the host's delay compensation.
    uint32_t latency() const
    { return uint32_t((uint64_t(kTaps / 2) * m_L) / m_M); }

    /// `in` must hold exactly inputsFor(outFrames) core samples.
    void process(const float *in, uint32_t inFrames, float *out, uint32_t outFrames)
    {
        uint32_t c = 0;
        for (uint32_t i = 0; i < outFrames; i ++)
        {
            const uint64_t need = (m_outNext * m_M) / m_L;
            while (m_inNext <= need && c < inFrames)
                push(in[c ++]);

            const float *h = &m_taps[size_t((m_outNext * m_M) % m_L) * kTaps];
            float acc = 0.0f;
            unsigned idx = (m_pos + kTaps - 1) % kTaps;      // newest first
            for (unsigned k = 0; k < kTaps; k ++)
            {
                acc += h[k] * m_hist[idx];
                idx = (idx + kTaps - 1) % kTaps;
            }
            out[i] = acc;
            m_outNext ++;
        }
    }

    unsigned L() const { return m_L; }
    unsigned M() const { return m_M; }

private:
    void push(float v)
    {
        m_hist[m_pos] = v;
        m_pos = (m_pos + 1) % kTaps;
        m_inNext ++;
    }

    static double besselI0(double x)
    {
        double sum = 1.0, term = 1.0;
        for (int k = 1; k < 50; k ++)
        {
            term *= (x / 2.0) * (x / 2.0) / (double(k) * double(k));
            sum += term;
            if (term < 1e-17 * sum) break;
        }
        return sum;
    }

    std::vector<float> m_taps, m_hist;
    unsigned m_L = 1, m_M = 1;
    unsigned m_pos = 0;
    uint64_t m_inNext = 0, m_outNext = 0;
};

} // namespace voltaire

#endif // VOLTAIRE_RESAMPLER_HPP
