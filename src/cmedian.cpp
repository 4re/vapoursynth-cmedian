
/**
 *  CMedian - VapourSynth Constant Time Median Filter
 *
 *  CMedian is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation; either
 *  version 2.1 of the License, or (at your option) any later version.
 *
 *  VapourSynth is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public
 *  License along with VapourSynth; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 **/

#include <VapourSynth.h>
#include <VSHelper.h>

#include <cstdint>
#include <string>
#include <algorithm>
#include <emmintrin.h>

static const size_t sseBytes = 16;

constexpr uint32_t getCoarseBits(uint32_t bits)
{
    return bits >> 1;
}

typedef struct CMedian
{
    VSNodeRef *node;
    const VSVideoInfo *vi;

    int radius;
    bool planes[3];
} CMedian;

typedef struct ColPair
{
    int beg;
    int end;
} ColPair;

template <typename>
static inline __m128i sse_adds(const __m128i &a, const __m128i &b);

template <>
inline __m128i sse_adds<uint8_t>(const __m128i &a, const __m128i &b)
{
    return _mm_adds_epu8(a, b);
}

template <>
inline __m128i sse_adds<uint16_t>(const __m128i &a, const __m128i &b)
{
    return _mm_adds_epu16(a, b);
}

template <typename>
static inline __m128i sse_subs(const __m128i &a, const __m128i &b);

template <>
inline __m128i sse_subs<uint8_t>(const __m128i &a, const __m128i &b)
{
    return _mm_subs_epu8(a, b);
}

template <>
inline __m128i sse_subs<uint16_t>(const __m128i &a, const __m128i &b)
{
    return _mm_subs_epu16(a, b);
}

template <typename T, size_t size>
static inline void sse_histAdd(T *a, const T *b)
{
    constexpr uint32_t pixelStep = sseBytes / sizeof(T);
    for (uint32_t i = 0; i < size; i += pixelStep) {
        __m128i sa = _mm_load_si128(reinterpret_cast<__m128i*>(a + i));
        __m128i sb = _mm_load_si128(reinterpret_cast<const __m128i*>(b + i));
        __m128i sum = sse_adds<T>(sa, sb);
        _mm_store_si128(reinterpret_cast<__m128i*>(a + i), sum);
    }
}

template <typename T, size_t size>
static inline void sse_histSub(T *a, const T *b)
{
    constexpr uint32_t pixelStep = sseBytes / sizeof(T);
    for (uint32_t i = 0; i < size; i += pixelStep) {
        __m128i sa = _mm_load_si128(reinterpret_cast<__m128i*>(a + i));
        __m128i sb = _mm_load_si128(reinterpret_cast<const __m128i*>(b + i));
        __m128i sum = sse_subs<T>(sa, sb);
        _mm_store_si128(reinterpret_cast<__m128i*>(a + i), sum);
    }
}

template <typename T, size_t size>
static inline void sse_histZero(T *a)
{
    constexpr uint32_t pixelStep = sseBytes / sizeof(T);
    const __m128i zero = _mm_setzero_si128();
    for (uint32_t i = 0; i < size; i += pixelStep) {
        _mm_store_si128(reinterpret_cast<__m128i*>(a + i), zero);
    }
}

template <typename T, size_t size>
static inline void sse_set(T *a, const int val)
{
    constexpr uint32_t pixelStep = sseBytes / sizeof(T);
    const __m128i v = _mm_set1_epi32(val);
    for (uint32_t i = 0; i < size; i += pixelStep) {
        _mm_store_si128(reinterpret_cast<__m128i*>(a + i), v);
    }
}

template <typename T, typename CountType, size_t bits>
static inline void cmedian_kernel(const T *srcp, const int srcStride, T *dstp, const int dstStride,
            const int w, const int h, CMedian *d, int plane,
            int offsBeg, int offsEnd, CountType *hCoarse, CountType *hFine, CountType *kCoarse, CountType *kFine, ColPair *colPair)
{
    const int radius = d->radius;

    const uint32_t coarseBits = getCoarseBits(bits);
    const uint32_t sCoarse = 1 << coarseBits;
    const uint32_t sCoarse2 = 1 << (bits - coarseBits);
    const uint32_t sFine = 1 << bits;

    const uint32_t lshift = sizeof(T) * 8 - (coarseBits);

    // set column hist beg & end
    const int hBeg = std::max(0, offsBeg - radius);
    const int hEnd = std::min(w, offsEnd + radius);

    // initialize column hist
    for (int x = hBeg; x < hEnd; ++x) {
        sse_histZero<CountType, sCoarse>(hCoarse + (x << coarseBits));
        sse_histZero<CountType, sFine>(hFine + (x << bits));
        for (int y = 0; y < radius; ++y) {
            T val = srcp[x + srcStride * y];
            ++hCoarse[(x << coarseBits) + (val >> (bits - coarseBits))];
            ++hFine[(x << bits) + (val << lshift >> lshift)];
        }
    }

    for (int y = 0; y < h; ++y) {

        sse_set<int, (sCoarse * 2)>(reinterpret_cast<int*>(colPair), -1);

        // add column hist of next rows
        if (y + radius < h) {
            for (int x = hBeg; x < hEnd; ++x) {
                T val = srcp[x + radius * srcStride];
                ++hCoarse[(x << coarseBits) + (val >> (bits - coarseBits))];
                ++hFine[(x << bits) + (val << lshift >> lshift)];
            }
        }

        // initialize the kernel hist
        sse_histZero<CountType, sCoarse>(kCoarse);
        sse_histZero<CountType, sFine>(kFine);
        for (int x = hBeg; x < std::min(w, offsBeg + radius); ++x)
            sse_histAdd<CountType, sCoarse>(kCoarse, hCoarse + (x << coarseBits));


        for (int x = offsBeg; x < std::min(w, offsEnd); ++x) {

            // set kernel hist beg & end
            const int beg = std::max(0, x - radius);
            const int end = std::min(w, x + radius + 1);

            // add coarse on the right side
            if (x + radius < w)
                sse_histAdd<CountType, sCoarse>(kCoarse, hCoarse + ((end - 1) << coarseBits));

            // get median number
            const uint32_t bound = ((std::min(w, x + radius + 1) - std::max(0, x - radius)) *
                (std::min(h, y + radius + 1) - std::max(0, y - radius))) / 2;

            // get coarseIdx
            uint32_t sum = 0;
            uint32_t coarseIdx = 0;
            for (; coarseIdx < sCoarse; ++coarseIdx) {
                sum += kCoarse[coarseIdx];
                if (sum > bound)
                    break;
            }
            sum -= kCoarse[coarseIdx];
            assert(coarseIdx < sCoarse);

            // partially update fine
            const int fineOffs = coarseIdx << (bits - coarseBits);
            if (colPair[coarseIdx].end <= beg) {
                // no overlap so we just zero the fine and recalulate by current area that kernel is on
                sse_histZero<CountType, sCoarse2>(kFine + fineOffs);
                for (int i = beg; i < end; ++i)
                    sse_histAdd<CountType, sCoarse2>(kFine + fineOffs, hFine + (i << bits) + fineOffs);
            } else {
                // overlap so we do multiple addition and subtraction to reach current area that kernel is on
                for (int i = colPair[coarseIdx].end; i < end; ++i)
                    sse_histAdd<CountType, sCoarse2>(kFine + fineOffs, hFine + (i << bits) + fineOffs);
                for (int i = colPair[coarseIdx].beg; i < beg; ++i)
                    sse_histSub<CountType, sCoarse2>(kFine + fineOffs, hFine + (i << bits) + fineOffs);
            }
            // update colPair
            colPair[coarseIdx].beg = beg;
            colPair[coarseIdx].end = end;

            // get fineIdx
            uint32_t fineIdx = fineOffs;
            for (; fineIdx < sFine; ++fineIdx) {
                sum += kFine[fineIdx];
                if (sum > bound)
                    break;
            }
            assert(fineIdx < sFine);
            dstp[x] = fineIdx;


            // sub coarse on the left side
            if (x - radius >= 0)
                sse_histSub<CountType, sCoarse>(kCoarse, hCoarse + (beg << coarseBits));
        }

        // sub column hist of previous rows
        if (y - radius >= 0) {
            for (int x = hBeg; x < hEnd; ++x) {
                T val = srcp[x - radius * srcStride];
                --hCoarse[(x << coarseBits) + (val >> (bits - coarseBits))];
                --hFine[(x << bits) + (val << lshift >> lshift)];
            }
        }

        srcp += srcStride;
        dstp += dstStride;
    }
}

template <typename T, size_t bits>
static inline void cmedian(const T *srcp, const int srcStride, T *dstp, const int dstStride, const int w, const int h, CMedian *d, int plane, void *hCoarse, void *hFine, void *kCoarse, void *kFine, ColPair *colPair)
{
    static_assert(bits % 2 == 0, "bits must be even number");

    // split frames to process
    const int step = (bits == 8) ? 256 : (bits == 10) ? 192 : (bits == 16) ? 128 : 256;
    for (int i = 0; i < w; i += step) {
        if (d->radius < 8)
            cmedian_kernel<T, uint8_t, bits>(srcp, srcStride, dstp, dstStride, w, h, d, plane, i, i + step,
                reinterpret_cast<uint8_t*>(hCoarse), reinterpret_cast<uint8_t*>(hFine),
                reinterpret_cast<uint8_t*>(kCoarse), reinterpret_cast<uint8_t*>(kFine), colPair);
        else
            cmedian_kernel<T, uint16_t, bits>(srcp, srcStride, dstp, dstStride, w, h, d, plane, i, i + step,
                reinterpret_cast<uint16_t*>(hCoarse), reinterpret_cast<uint16_t*>(hFine),
                reinterpret_cast<uint16_t*>(kCoarse), reinterpret_cast<uint16_t*>(kFine), colPair);
    }
}

static void VS_CC cmedianInit(VSMap *in, VSMap *out, void **instanceData, VSNode* node, VSCore *core, const VSAPI *vsapi)
{
    CMedian *d = reinterpret_cast<CMedian*> (*instanceData);
    vsapi->setVideoInfo(d->vi, 1, node);
}

static const VSFrameRef *VS_CC cmedianGetFrame(int n, int activationReason, void **instanceData, void **frameData, VSFrameContext *frameCtx, VSCore *core, const VSAPI *vsapi)
{
    CMedian *d = reinterpret_cast<CMedian*> (*instanceData);

    if (activationReason == arInitial) {

        vsapi->requestFrameFilter(n, d->node, frameCtx);

    } else if (activationReason == arAllFramesReady) {

        auto src = vsapi->getFrameFilter(n, d->node, frameCtx);
        auto dst = vsapi->copyFrame(src, core);

        ///////////////////////////////////////////////////////////////////////
        const uint32_t coarseBits = getCoarseBits(d->vi->format->bitsPerSample);
        const uint32_t sCoarse = 1 << coarseBits;
        const uint32_t sFine = 1 << d->vi->format->bitsPerSample;

        void *hCoarse = nullptr, *hFine = nullptr, *kCoarse = nullptr, *kFine = nullptr;

        if (d->radius < 8) {
            hCoarse = vs_aligned_malloc<void>(sizeof(uint8_t) * d->vi->width * sCoarse, 32);
            hFine = vs_aligned_malloc<void>(sizeof(uint8_t) * d->vi->width * sFine, 32);
            kCoarse = vs_aligned_malloc<void>(sizeof(uint8_t) * sCoarse, 32);
            kFine = vs_aligned_malloc<void>(sizeof(uint8_t) * sFine, 32);
        } else {
            hCoarse = vs_aligned_malloc<void>(sizeof(uint16_t) * d->vi->width * sCoarse, 32);
            hFine = vs_aligned_malloc<void>(sizeof(uint16_t) * d->vi->width * sFine, 32);
            kCoarse = vs_aligned_malloc<void>(sizeof(uint16_t) * sCoarse, 32);
            kFine = vs_aligned_malloc<void>(sizeof(uint16_t) * sFine, 32);
        }

        ColPair *colPair = vs_aligned_malloc<ColPair>(sizeof(ColPair) * sCoarse, 32);
        ///////////////////////////////////////////////////////////////////////

        for (int plane = 0; plane < d->vi->format->numPlanes; ++plane) {

            if (!d->planes[plane])
                continue;

            auto srcp = vsapi->getReadPtr(src, plane);
            auto srcStride = vsapi->getStride(src, plane) / d->vi->format->bytesPerSample;
            auto dstp = vsapi->getWritePtr(dst, plane);
            auto dstStride = vsapi->getStride(dst, plane) / d->vi->format->bytesPerSample;
            auto width = vsapi->getFrameWidth(src, plane);
            auto height = vsapi->getFrameHeight(src, plane);

            if (d->vi->format->sampleType == stInteger) {
                if (d->vi->format->bitsPerSample == 8)
                    cmedian<uint8_t, 8>(srcp, srcStride, dstp, dstStride, width, height, d, plane, hCoarse, hFine, kCoarse, kFine, colPair);
                else if (d->vi->format->bitsPerSample == 10)
                    cmedian<uint16_t, 10>(reinterpret_cast<const uint16_t*>(srcp), srcStride, reinterpret_cast<uint16_t*>(dstp), dstStride, width, height, d, plane, hCoarse, hFine, kCoarse, kFine, colPair);
                else if (d->vi->format->bitsPerSample == 16)
                    cmedian<uint16_t, 16>(reinterpret_cast<const uint16_t*>(srcp), srcStride, reinterpret_cast<uint16_t*>(dstp), dstStride, width, height, d, plane, hCoarse, hFine, kCoarse, kFine, colPair);
            }
        }

        vs_aligned_free(hCoarse);
        vs_aligned_free(hFine);
        vs_aligned_free(kCoarse);
        vs_aligned_free(kFine);
        vs_aligned_free(colPair);

        vsapi->freeFrame(src);
        return dst;
    }
    return nullptr;
}

static void VS_CC cmedianFree(void *instanceData, VSCore *core, const VSAPI *vsapi)
{
    CMedian *d = reinterpret_cast<CMedian*> (instanceData);
    vsapi->freeNode(d->node);
    delete d;
}

static void VS_CC cmedianCreate(const VSMap *in, VSMap *out, void *userData, VSCore *core, const VSAPI *vsapi)
{
    CMedian *d = new CMedian();

    int err;

    try {

        d->node = vsapi->propGetNode(in, "clip", 0, 0);
        d->vi = vsapi->getVideoInfo(d->node);

        if (d->vi->format->sampleType != stInteger)
            throw std::string("only integer sample type support");

        if (d->vi->format->bitsPerSample != 8 &&
            d->vi->format->bitsPerSample != 10 &&
            d->vi->format->bitsPerSample != 16)
            throw std::string("only 8, 10, 16 bits support");

        d->radius = int64ToIntS(vsapi->propGetInt(in, "radius", 0, &err));
        if (err) d->radius = 1;

        if (d->radius < 1 || d->radius > 127)
            throw std::string("radius must be 1 ... 127");


        for (int i = 0; i < 3; ++i)
            d->planes[i] = false;

        int m = vsapi->propNumElements(in, "planes");

        if (m <= 0) {
            for (int i = 0; i < 3; ++i)
                d->planes[i] = true;
        } else {
            for (int i = 0; i < m; ++i) {
                int p = int64ToIntS(vsapi->propGetInt(in, "planes", i, &err));
                if (p < 0 || p > d->vi->format->numPlanes - 1)
                    throw std::string("planes index out of bound");
                d->planes[p] = true;
            }
        }

    } catch (std::string &errorMsg) {
        vsapi->freeNode(d->node);
        vsapi->setError(out, std::string("CMedian: ").append(errorMsg).c_str());
        return;
    }

    vsapi->createFilter(in, out, "cmedian", cmedianInit, cmedianGetFrame, cmedianFree, fmParallel, 0, d, core);
}

VS_EXTERNAL_API(void) VapourSynthPluginInit(VSConfigPlugin configFunc, VSRegisterFunction registerFunc, VSPlugin *plugin)
{
    configFunc("com.mio.cmedian", "cmedian", "VapourSynth Constant Time Median Filter", VAPOURSYNTH_API_VERSION, 1, plugin);
    registerFunc("Median", "clip:clip;"
        "radius:int:opt;"
        "planes:int[]:opt;",
        cmedianCreate, nullptr, plugin);
}
