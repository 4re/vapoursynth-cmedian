# CMedian - VapourSynth Constant Time Median Filter #

*   implements the constant time median filter algorithm [Link](https://nomis80.org/ctmf.html)

## Build ##

*   compiler with c++11 support
*   CPU with SSE2 support

## Usage ##

    cmedian.Median(src, radius=1, planes=[0, 1, 2])

*   the default setting, apply a median filter of radius=1
***

## Parameter ##

    cmedian.Median(clip clip[, radius=1, int[] planes=[0, 1, 2]])

*   clip: the input clip
    *   all color family support
    *   8, 10, 16 integer type support

***
*   radius: the kernel radius, which will result a (2 * radius + 1) * (2 * radius + 1) square.
    *   default: 1
    *   range: 1 ... 127

***
*   planes: apply median filter for planes
    *   default: all planes

***

## Performance ##

    This is the implementation of constant time median filter.
    the runtime complexity is O(1) for any 'radius'.
    but note that it uses the histogram data structure for implementing the algorithm,
    the cost on computing histogram grows exponentially as the bitdepth increases.
    so you may find that 16 bit input is much slower than 8 bit input.

    for radius= 1 ... 7, it is optimazed by using 8 bit size counter
    for radius= 8 ... 127, it uses 16 bit size counter which should be slower.

    and for radius=1, you can use rgvs.RemoveGrain(4) instead of this filter, it's much faster duo to its specific algorithm.

    CPU: E3 1230-V2 4C8T

        YUV420P8 1920x1080
            cmedian.Median(radius=1, planes=[0, 1, 2])      47.2 fps
            cmedian.Median(radius=8, planes=[0, 1, 2])      42.8 fps

        YUV420P10 1920x1080
            cmedian.Median(radius=1, planes=[0, 1, 2])      27.1 fps
            cmedian.Median(radius=8, planes=[0, 1, 2])      24.5 fps

        YUV420P16 1920x1080
            cmedian.Median(radius=1, planes=[0, 1, 2])      5.9 fps
            cmedian.Median(radius=8, planes=[0, 1, 2])      2.7 fps

## License ##

    CMedian

    DO WHAT THE FUCK YOU WANT TO PUBLIC LICENSE
    Version 2, December 2004

    This program is free software. It comes without any warranty, to
    the extent permitted by applicable law. You can redistribute it
    and/or modify it under the terms of the Do What The Fuck You Want
    To Public License, Version 2, as published by Sam Hocevar. See
    http://sam.zoy.org/wtfpl/COPYING for more details.
