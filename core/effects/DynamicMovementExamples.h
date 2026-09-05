//
// DynamicMovementExamples.h — the 8 classic Dynamic Movement example scripts,
// verbatim from AVS (e_dynamicmovement.h, BSD-3, Copyright 2005 Nullsoft, Inc.).
// Bundled content for our scripted DynamicMovement effect and additional
// validation corpus for the EEL VM.
//
#pragma once

namespace viz {

struct DynamicMovementExample {
    const char* name;
    const char* init;
    const char* frame;
    const char* beat;
    const char* point;
    bool rectangular;   // point script uses x/y (true) or d/r (false)
    bool wrap;
    int gridW;          // grid vertex count, x
    int gridH;          // grid vertex count, y
};

inline constexpr DynamicMovementExample kDynamicMovementExamples[] = {
    { "Random Rotate",
      "", "",
      "dr = (rand(100) / 100) * $PI;\nd = d * .95;",
      "r = r + dr;",
      false, true, 2, 2 },
    { "Random Direction",
      "speed=.05;dr = (rand(200) / 100) * $PI;",
      "dx = cos(dr) * speed;\ndy = sin(dr) * speed;",
      "dr = (rand(200) / 100) * $PI;",
      "x = x + dx;\ny = y + dy;",
      true, true, 2, 2 },
    { "In and Out",
      "speed=.2;c=0;",
      "",
      "c = c + ($PI/2);\ndd = 1 - (sin(c) * speed);",
      "d = d * dd;",
      false, true, 2, 2 },
    { "Unspun Kaleida",
      "c=200;f=0;dt=0;dl=0;beatdiv=8",
      "f = f + 1;\nt = ((f * $pi * 2)/c)/beatdiv;\ndt = dl + t;\n"
      "dr = 4+(cos(dt)*2);",
      "c=f;f=0;dl=dt",
      "r=cos(r*dr);",
      false, true, 33, 33 },
    { "Roiling Gridley",
      "c=200;f=0;dt=0;dl=0;beatdiv=8",
      "f = f + 1;\nt = ((f * $pi * 2)/c)/beatdiv;\ndt = dl + t;\n"
      "dx = 14+(cos(dt)*8);\ndy = 10+(sin(dt*2)*4);",
      "c=f;f=0;dl=dt",
      "x=x+(sin(y*dx) * .03);\ny=y-(cos(x*dy) * .03);",
      true, true, 32, 32 },
    { "6-Way Outswirl",
      "c=200;f=0;dt=0;dl=0;beatdiv=8",
      "f = f + 1;\nt = ((f * $pi * 2)/c)/beatdiv;\ndt = dl + t;\n"
      "dr = 18+(cos(dt)*12);",
      "c=f;f=0;dl=dt",
      "d=d*(1+(cos(r*6) * .05));\nr=r-(sin(d*dr) * .05);\nd = d * .98;",
      false, false, 32, 32 },
    { "Wavy",
      "c=200;f=0;dx=0;dl=0;beatdiv=16;speed=.05",
      "f = f + 1;\nt = ( (f * 2 * 3.1415) / c ) / beatdiv;\ndx = dl + t;",
      "c = f;\nf = 0;\ndl = dx;",
      "y = y + ((sin((x+dx) * $PI))*speed);\nx = x + .025",
      true, true, 6, 6 },
    { "Smooth Rotoblitter",
      "c=200;f=0;dt=0;dl=0;beatdiv=4;speed=.15",
      "f = f + 1;\nt = ((f * $pi * 2)/c)/beatdiv;\ndt = dl + t;\n"
      "dr = cos(dt)*speed*2;\ndd = 1 - (sin(dt)*speed);",
      "c=f;f=0;dl=dt",
      "r = r + dr;\nd = d * dd;",
      false, true, 2, 2 },
};

inline constexpr int kNumDynamicMovementExamples =
    (int)(sizeof(kDynamicMovementExamples) / sizeof(kDynamicMovementExamples[0]));

} // namespace viz
