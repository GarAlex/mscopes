# Third-party notices

MScopes itself is licensed under the BSD 3-Clause License (see `LICENSE`).
It contains or derives from the following works, each under its own terms.

## vis_avs — Nullsoft Advanced Visualization Studio

The classic effects in `core/effects/` are our own C++ implementations of
the algorithms in Nullsoft's AVS, as published in the open-source
[`grandchild/vis_avs`](https://github.com/grandchild/vis_avs) tree, and the
`.avs` preset reader follows its file format. Each ported class names the
`e_*.cpp` it derives from in the comment above its declaration. The classic
example scripts for Superscope and Dynamic Movement (`SuperscopeExamples.h`,
`DynamicMovementExamples.h`) are reproduced from that source.

    Copyright 2005 Nullsoft, Inc. All rights reserved.

    Redistribution and use in source and binary forms, with or without
    modification, are permitted provided that the following conditions are met:

      * Redistributions of source code must retain the above copyright notice,
        this list of conditions and the following disclaimer.
      * Redistributions in binary form must reproduce the above copyright notice,
        this list of conditions and the following disclaimer in the documentation
        and/or other materials provided with the distribution.
      * Neither the name of Nullsoft nor the names of its contributors may be used
        to endorse or promote products derived from this software without specific
        prior written permission.

    THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
    AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
    IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
    ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
    LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
    CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
    SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
    INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
    CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
    ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
    POSSIBILITY OF SUCH DAMAGE.

"Winamp", "Nullsoft" and "AVS" are the marks of their owners. MScopes is an
independent project and is not affiliated with or endorsed by them.

## Apple iTunes Visual Plug-in SDK

`third_party/itunes-visual-sdk/` (`iTunesAPI.h`, `iTunesVisualAPI.h`,
`iTunesAPI.cpp`) is Apple sample code, © Apple Inc., used to build the Music
visual plugin. It is distributed under Apple's sample-code license, whose full
text is retained at the top of each file: use, modification and
redistribution are permitted provided the notice is kept and Apple's name is
not used to endorse derived products.

## Preset content

The app ships only presets written for it. Community `.avs` presets are the
work of their authors and are neither included in this repository nor in
the app; see `docs/PRESETS.md`.
