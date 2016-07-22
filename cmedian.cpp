
/**
 *  CMedian
 *
 *  DO WHAT THE FUCK YOU WANT TO PUBLIC LICENSE
 *  Version 2, December 2004
 *
 *  This program is free software. It comes without any warranty, to
 *  the extent permitted by applicable law. You can redistribute it
 *  and/or modify it under the terms of the Do What The Fuck You Want
 *  To Public License, Version 2, as published by Sam Hocevar. See
 *  http://sam.zoy.org/wtfpl/COPYING for more details.
 *
 **/

#include "VapourSynth/VapourSynth.h"
#include "VapourSynth/VSHelper.h"

#include <cstdint>
#include <string>
#include <algorithm>
#include <emmintrin.h>

constexpr uint32_t getCoarseBits(uint32_t bits)
{
    if (bits == 8)
        return 4;
    if (bits == 10)
        return 5;
    if (bits == 16)
        return 8;
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
static inline __m128i simdAdds(const __m128i &a, const __m128i &b);

template <>
inline __m128i simdAdds<uint8_t>(const __m128i &a, const __m128i &b)
{
    return _mm_adds_epu8(a, b);
}

template <>
inline __m128i simdAdds<uint16_t>(const __m128i &a, const __m128i &b)
{
    return _mm_adds_epu16(a, b);
}

template <typename>
static inline __m128i simdSubs(const __m128i &a, const __m128i &b);

template <>
inline __m128i simdSubs<uint8_t>(const __m128i &a, const __m128i &b)
{
    return _mm_subs_epu8(a, b);
}

template <>
inline __m128i simdSubs<uint16_t>(const __m128i &a, const __m128i &b)
{
    return _mm_subs_epu16(a, b);
}

template <typename T, size_t size>
static inline void histAdd(T *a, const T *b)
{
    for (uint32_t i = 0; i < (size / (16 / sizeof(T))); ++i) {
        __m128i sa = _mm_load_si128(reinterpret_cast<__m128i*>(a) + i);
        __m128i sb = _mm_load_si128(reinterpret_cast<const __m128i*>(b) + i);
        __m128i sum = simdAdds<T>(sa, sb);
        _mm_store_si128(reinterpret_cast<__m128i*>(a) + i, sum);
    }
}

template <typename T, size_t size>
static inline void histSub(T *a, const T *b)
{
    for (uint32_t i = 0; i < (size / (16 / sizeof(T))); ++i) {
        __m128i sa = _mm_load_si128(reinterpret_cast<__m128i*>(a) + i);
        __m128i sb = _mm_load_si128(reinterpret_cast<const __m128i*>(b) + i);
        __m128i sum = simdSubs<T>(sa, sb);
        _mm_store_si128(reinterpret_cast<__m128i*>(a) + i, sum);
    }
}

template <typename T, size_t size>
static inline void histZero(T *a)
{
    const __m128i zero = _mm_setzero_si128();
    for (uint32_t i = 0; i < (size / (16 / sizeof(T))); ++i) {
        _mm_store_si128(reinterpret_cast<__m128i*>(a) + i, zero);
    }
}

template <size_t size>
static inline void simd_set(int *a, const int val)
{
    const __m128i v = _mm_set1_epi32(val);
    for (uint32_t i = 0; i < (size / 4); ++i) {
        _mm_store_si128(reinterpret_cast<__m128i*>(a) + i, v);
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
        histZero<CountType, sCoarse>(hCoarse + (x << coarseBits));
        histZero<CountType, sFine>(hFine + (x << bits));
        for (int y = 0; y < radius; ++y) {
            T val = srcp[x + srcStride * y];
            ++hCoarse[(x << coarseBits) + (val >> (bits - coarseBits))];
            ++hFine[(x << bits) + (val << lshift >> lshift)];
        }
    }

    for (int y = 0; y < h; ++y) {

        simd_set<(sCoarse * 2)>(reinterpret_cast<int*>(colPair), -1);

        // add column hist of next rows
        if (y + radius < h) {
            for (int x = hBeg; x < hEnd; ++x) {
                T val = srcp[x + radius * srcStride];
                ++hCoarse[(x << coarseBits) + (val >> (bits - coarseBits))];
                ++hFine[(x << bits) + (val << lshift >> lshift)];
            }
        }

        // initialize the kernel hist
        histZero<CountType, sCoarse>(kCoarse);
        histZero<CountType, sFine>(kFine);
        for (int x = hBeg; x < std::min(w, offsBeg + radius); ++x)
            histAdd<CountType, sCoarse>(kCoarse, hCoarse + (x << coarseBits));


        for (int x = offsBeg; x < std::min(w, offsEnd); ++x) {

            // set kernel hist beg & end
            const int beg = std::max(0, x - radius);
            const int end = std::min(w, x + radius + 1);

            // add coarse on the right side
            if (x + radius < w)
                histAdd<CountType, sCoarse>(kCoarse, hCoarse + ((end - 1) << coarseBits));

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
                histZero<CountType, sCoarse2>(kFine + fineOffs);
                for (int i = beg; i < end; ++i)
                    histAdd<CountType, sCoarse2>(kFine + fineOffs, hFine + (i << bits) + fineOffs);
            } else {
                // overlap so we do multiple addition and subtraction to reach current area that kernel is on
                for (int i = colPair[coarseIdx].end; i < end; ++i)
                    histAdd<CountType, sCoarse2>(kFine + fineOffs, hFine + (i << bits) + fineOffs);
                for (int i = colPair[coarseIdx].beg; i < beg; ++i)
                    histSub<CountType, sCoarse2>(kFine + fineOffs, hFine + (i << bits) + fineOffs);
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
                histSub<CountType, sCoarse>(kCoarse, hCoarse + (beg << coarseBits));
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
static inline void cmedian(const T *srcp, const int srcStride, T *dstp, const int dstStride, const int w, const int h, CMedian *d, int plane)
{
    static_assert(bits % 2 == 0, "bits must be even number");

    const uint32_t coarseBits = getCoarseBits(bits);
    const uint32_t sCoarse = 1 << coarseBits;
    const uint32_t sFine = 1 << bits;

    void *hCoarse = nullptr;
    void *hFine = nullptr;
    void *kCoarse = nullptr;
    void *kFine = nullptr;

    if (d->radius < 8) {
        hCoarse = vs_aligned_malloc<void>(sizeof(uint8_t) * w * sCoarse, 32);
        hFine = vs_aligned_malloc<void>(sizeof(uint8_t) * w * sFine, 32);
        kCoarse = vs_aligned_malloc<void>(sizeof(uint8_t) * sCoarse, 32);
        kFine = vs_aligned_malloc<void>(sizeof(uint8_t) * sFine, 32);
    } else {
        hCoarse = vs_aligned_malloc<void>(sizeof(uint16_t) * w * sCoarse, 32);
        hFine = vs_aligned_malloc<void>(sizeof(uint16_t) * w * sFine, 32);
        kCoarse = vs_aligned_malloc<void>(sizeof(uint16_t) * sCoarse, 32);
        kFine = vs_aligned_malloc<void>(sizeof(uint16_t) * sFine, 32);
    }

    ColPair *colPair = vs_aligned_malloc<ColPair>(sizeof(ColPair) * sCoarse, 32);

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

    vs_aligned_free(hCoarse);
    vs_aligned_free(hFine);
    vs_aligned_free(kCoarse);
    vs_aligned_free(kFine);
    vs_aligned_free(colPair);
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
                    cmedian<uint8_t, 8>(srcp, srcStride, dstp, dstStride, width, height, d, plane);
                else if (d->vi->format->bitsPerSample == 10)
                    cmedian<uint16_t, 10>(reinterpret_cast<const uint16_t*>(srcp), srcStride, reinterpret_cast<uint16_t*>(dstp), dstStride, width, height, d, plane);
                else if (d->vi->format->bitsPerSample == 16)
                    cmedian<uint16_t, 16>(reinterpret_cast<const uint16_t*>(srcp), srcStride, reinterpret_cast<uint16_t*>(dstp), dstStride, width, height, d, plane);
            }
        }

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
            throw std::string("only support interger sample type");

        if (d->vi->format->bitsPerSample != 8 &&
            d->vi->format->bitsPerSample != 10 &&
            d->vi->format->bitsPerSample != 16)
            throw std::string("only support 8, 10, 16 bits");

        d->radius = vsapi->propGetInt(in, "radius", 0, &err);
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
                int p = vsapi->propGetInt(in, "planes", i, &err);
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
