#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

namespace defb
{

// Small, dependency-free radix-2 FFT (in-place, forward only).
// Used by the analysis stage so the DSP core can be built and tested
// without any framework or external FFT library.
class Fft
{
public:
    explicit Fft (int order = 11) { reset (order); }

    void reset (int order)
    {
        order_ = order;
        size_  = 1 << order;
        rev_.resize ((size_t) size_);

        for (int i = 0; i < size_; ++i)
        {
            unsigned int x = (unsigned int) i;
            unsigned int r = 0;
            for (int b = 0; b < order_; ++b)
            {
                r = (r << 1) | (x & 1u);
                x >>= 1;
            }
            rev_[(size_t) i] = (int) r;
        }

        const int half = size_ / 2;
        cos_.resize ((size_t) half);
        sin_.resize ((size_t) half);
        for (int i = 0; i < half; ++i)
        {
            const double a = -2.0 * kPi * (double) i / (double) size_;
            cos_[(size_t) i] = (float) std::cos (a);
            sin_[(size_t) i] = (float) std::sin (a);
        }
    }

    int size() const noexcept { return size_; }

    void forward (float* re, float* im) const
    {
        for (int i = 0; i < size_; ++i)
        {
            const int j = rev_[(size_t) i];
            if (j > i)
            {
                std::swap (re[i], re[j]);
                std::swap (im[i], im[j]);
            }
        }

        for (int len = 2; len <= size_; len <<= 1)
        {
            const int half = len >> 1;
            const int step = size_ / len;

            for (int base = 0; base < size_; base += len)
            {
                for (int k = 0; k < half; ++k)
                {
                    const int tw = k * step;
                    const float wr = cos_[(size_t) tw];
                    const float wi = sin_[(size_t) tw];
                    const int a = base + k;
                    const int b = a + half;

                    const float xr = re[b] * wr - im[b] * wi;
                    const float xi = re[b] * wi + im[b] * wr;

                    re[b] = re[a] - xr;
                    im[b] = im[a] - xi;
                    re[a] += xr;
                    im[a] += xi;
                }
            }
        }
    }

    static constexpr double kPi = 3.14159265358979323846;

private:
    int order_ = 0;
    int size_  = 0;
    std::vector<int> rev_;
    std::vector<float> cos_, sin_;
};

} // namespace defb
