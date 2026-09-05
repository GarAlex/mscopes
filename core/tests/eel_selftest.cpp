//
// eel_selftest.cpp — unit tests for the EEL VM, plus a compatibility pass over
// the 14 classic Superscope example scripts (compile + run, assert finiteness).
//
// Build:  clang++ -std=c++17 -O2 -I core -I core/script -I core/effects \
//           core/script/EelVM.cpp core/script/eel_selftest.cpp -o build/eel_selftest
//
#include "EelVM.h"
#include "SuperscopeExamples.h"
#include "DynamicMovementExamples.h"

#include <cmath>
#include <cstdio>
#include <string>

static int g_failures = 0;

static void expect(const char* src, const char* varName, double want, double tol = 1e-9)
{
    viz::eel::VM vm;
    viz::eel::Program p = viz::eel::compile(vm, src);
    if (!p.ok()) {
        printf("FAIL compile: \"%s\" -> %s\n", src, p.error().c_str());
        g_failures++;
        return;
    }
    p.run();
    double got = *vm.var(varName);
    if (std::fabs(got - want) > tol) {
        printf("FAIL eval: \"%s\" -> %s=%.12g, want %.12g\n", src, varName, got, want);
        g_failures++;
    }
}

static void expectError(const char* src)
{
    viz::eel::VM vm;
    viz::eel::Program p = viz::eel::compile(vm, src);
    if (p.ok()) {
        printf("FAIL: expected compile error for \"%s\"\n", src);
        g_failures++;
    }
}

int main()
{
    // arithmetic & precedence
    expect("x=1+2*3", "x", 7);
    expect("x=(1+2)*3", "x", 9);
    expect("x=2^10", "x", 1024);
    expect("x=2^3^2", "x", 512);              // right-assoc
    expect("x=-3+5", "x", 2);
    expect("x=10/4", "x", 2.5);
    expect("x=10%3", "x", 1);
    expect("x=10%0", "x", 0);                 // EEL: mod by zero -> 0
    expect("x=5/0", "x", 0);                  // EEL: div by zero -> 0
    expect("x=.5*2", "x", 1);
    expect("x=1; x=x+1; x=x*4", "x", 8);

    // variables auto-create as 0; case-insensitive
    expect("x=unset_var+5", "x", 5);
    expect("Foo=3; x=fOO*2", "x", 6);

    // constants
    expect("x=$PI", "x", 3.141592653589793, 1e-12);
    expect("x=$e", "x", 2.718281828459045, 1e-12);

    // functions
    expect("x=abs(-4.5)", "x", 4.5);
    expect("x=min(3,7)+max(3,7)", "x", 10);
    expect("x=sqr(5)", "x", 25);
    expect("x=sqrt(81)", "x", 9);
    expect("x=pow(2,8)", "x", 256);
    expect("x=sign(-9)", "x", -1);
    expect("x=floor(3.7)+ceil(3.2)", "x", 7);
    expect("x=sin(0)+cos(0)", "x", 1);
    expect("x=atan2(1,1)", "x", 0.7853981633974483, 1e-12);
    expect("x=sqrt(-1)", "x", 0);             // guarded
    expect("x=log(0)", "x", 0);               // guarded

    // logic
    expect("x=if(above(2,1),5,7)", "x", 5);
    expect("x=if(below(2,1),5,7)", "x", 7);
    expect("x=equal(4,4)+equal(4,5)", "x", 1);
    expect("x=band(1,2)+bor(0,3)+bnot(0)", "x", 3);
    expect("x=if(0,9,3)", "x", 3);
    expect("x=2>1", "x", 1);
    expect("x=1>=2", "x", 0);
    expect("x=3==3", "x", 1);
    expect("x=3!=3", "x", 0);
    expect("x= 1<2 ? 10 : 20", "x", 10);

    // statements: empty, stray expressions, comments, CRLF
    expect(";;x=2;;", "x", 2);
    expect("sin(1); x=2", "x", 2);            // stray expression discarded
    expect("// comment\nx=4", "x", 4);
    expect("/* multi\nline */ x=6", "x", 6);
    expect("a=1;\r\nb=2;\r\nx=a+b", "x", 3);

    // exec
    expect("x=exec2(a=5,a*2)", "x", 10);
    expect("x=exec3(a=1,b=2,a+b)", "x", 3);

    // rand bounds: 0 <= rand(k) < k, integer for k>=1
    {
        viz::eel::VM vm;
        auto p = viz::eel::compile(vm, "x=rand(50)");
        bool okAll = p.ok();
        for (int i = 0; i < 1000 && okAll; ++i) {
            p.run();
            double v = *vm.var("x");
            if (v < 0 || v >= 50 || v != std::floor(v)) okAll = false;
        }
        if (!okAll) { printf("FAIL: rand(50) out of bounds/non-integer\n"); g_failures++; }
    }

    // errors surface
    expectError("x=");
    expectError("x=nosuchfn(3)");
    expectError("x=(1+2");
    expectError("3=x");

    // host hooks
    {
        viz::eel::VM vm;
        vm.getosc = [](double band, double, double) { return band * 2; };
        auto p = viz::eel::compile(vm, "x=getosc(0.25,0.1,0)");
        p.run();
        if (std::fabs(*vm.var("x") - 0.5) > 1e-12) {
            printf("FAIL: getosc hook\n"); g_failures++;
        }
    }

    // reg00-99: process-global, shared across VM instances
    expect("reg07=42; x=reg07", "x", 42);
    {
        viz::eel::VM a, b;
        viz::eel::compile(a, "reg42=7").run();
        viz::eel::compile(b, "x=reg42*2").run();
        if (std::fabs(*b.var("x") - 14) > 1e-12) {
            printf("FAIL: reg42 not shared across VMs\n"); g_failures++;
        }
        viz::eel::compile(a, "reg42=0").run();     // reset for later tests
    }

    // megabuf: per-VM; gmegabuf: shared across VMs; assign() works on both
    expect("megabuf(5)=9; x=megabuf(5)+megabuf(6)", "x", 9);
    expect("i=100000; megabuf(i)=3; x=megabuf(100000)", "x", 3);
    expect("x=megabuf(-1)+megabuf(99999999)", "x", 0);     // out of range reads 0
    expect("assign(y,4); x=y", "x", 4);
    expect("assign(megabuf(2),8); x=megabuf(2)", "x", 8);
    {
        viz::eel::VM a, b;
        viz::eel::compile(a, "megabuf(0)=5; gmegabuf(1)=6").run();
        viz::eel::compile(b, "x=megabuf(0); y=gmegabuf(1)").run();
        if (*b.var("x") != 0 || *b.var("y") != 6) {
            printf("FAIL: megabuf isolation / gmegabuf sharing\n"); g_failures++;
        }
        viz::eel::compile(b, "gmegabuf(1)=0").run();
    }

    // extensions beyond ns-eel1
    expect("x=lerp(2,10,0.25)", "x", 4);
    expect("x=clamp(5,0,3)+clamp(-1,0,3)+clamp(2,0,3)", "x", 5);
    expect("x=frac(3.75)", "x", 0.75);
    expect("x=smoothstep(0,1,0.5)", "x", 0.5);
    expect("x=smoothstep(0,1,-2)+smoothstep(0,1,5)", "x", 1);
    {   // noise: deterministic, bounded, varies
        viz::eel::VM vm;
        auto p = viz::eel::compile(vm, "a=noise(1.3,2.7); b=noise(1.3,2.7); c=noise(9.1,4.2)");
        p.run();
        double a = *vm.var("a"), b = *vm.var("b"), c = *vm.var("c");
        if (a != b || a < 0 || a > 1 || c < 0 || c > 1 || a == c) {
            printf("FAIL: noise semantics (a=%g c=%g)\n", a, c); g_failures++;
        }
    }
    expect("c=hsv(0,1,1); x=getr(c)", "x", 1);              // pure red
    expect("c=hsv(0,1,1); x=getg(c)+getb(c)", "x", 0);
    expect("c=hsv(1/3,1,1); x=getg(c)", "x", 1);            // pure green
    expect("c=hsv(0,0,0.5); x=getr(c)", "x", 128.0/255.0, 1e-6);  // gray

    // shared-state reset (preset determinism)
    {
        viz::eel::VM vm;
        viz::eel::compile(vm, "reg33=5; gmegabuf(7)=9").run();
        viz::eel::VM::resetShared();
        viz::eel::VM vm2;
        viz::eel::compile(vm2, "x=reg33+gmegabuf(7)").run();
        if (*vm2.var("x") != 0) {
            printf("FAIL: resetShared left state (x=%g)\n", *vm2.var("x"));
            g_failures++;
        }
    }

    // ------------------------------------------------------------------
    //  Corpus: the 14 classic Superscope examples must compile and run.
    // ------------------------------------------------------------------
    for (int e = 0; e < viz::kNumSuperscopeExamples; ++e) {
        const auto& ex = viz::kSuperscopeExamples[e];
        viz::eel::VM vm;
        *vm.var("w") = 640; *vm.var("h") = 360;

        struct { const char* label; const char* src; } scripts[] = {
            {"init", ex.init}, {"frame", ex.frame}, {"beat", ex.beat}, {"point", ex.point}};

        viz::eel::Program progs[4];
        bool ok = true;
        for (int s = 0; s < 4; ++s) {
            progs[s] = viz::eel::compile(vm, scripts[s].src);
            if (!progs[s].ok()) {
                printf("FAIL corpus \"%s\" %s: %s\n", ex.name, scripts[s].label,
                       progs[s].error().c_str());
                g_failures++; ok = false;
            }
        }
        if (!ok) continue;

        // simulate: init, then 8 frames with a beat every 4th, 64 points each
        progs[0].run();
        bool finite = true;
        for (int f = 0; f < 8 && finite; ++f) {
            *vm.var("b") = (f % 4 == 0) ? 1.0 : 0.0;
            if (f % 4 == 0) progs[2].run();
            progs[1].run();
            int n = (int)*vm.var("n"); if (n < 1) n = 1; if (n > 512) n = 512;
            for (int idx = 0; idx < n; ++idx) {
                *vm.var("i") = (n == 1) ? 0.0 : (double)idx / (n - 1);
                *vm.var("v") = std::sin(idx * 0.3 + f);
                progs[3].run();
                double x = *vm.var("x"), y = *vm.var("y");
                if (!std::isfinite(x) || !std::isfinite(y)) finite = false;
            }
        }
        if (!finite) {
            printf("FAIL corpus \"%s\": non-finite x/y\n", ex.name);
            g_failures++;
        }
    }

    // ------------------------------------------------------------------
    //  Corpus: the 8 classic Dynamic Movement examples must compile & run.
    // ------------------------------------------------------------------
    for (int e = 0; e < viz::kNumDynamicMovementExamples; ++e) {
        const auto& ex = viz::kDynamicMovementExamples[e];
        viz::eel::VM vm;
        *vm.var("w") = 640; *vm.var("h") = 360; *vm.var("alpha") = 0.5;

        struct { const char* label; const char* src; } scripts[] = {
            {"init", ex.init}, {"frame", ex.frame}, {"beat", ex.beat}, {"point", ex.point}};
        viz::eel::Program progs[4];
        bool ok = true;
        for (int s = 0; s < 4; ++s) {
            progs[s] = viz::eel::compile(vm, scripts[s].src);
            if (!progs[s].ok()) {
                printf("FAIL DM corpus \"%s\" %s: %s\n", ex.name, scripts[s].label,
                       progs[s].error().c_str());
                g_failures++; ok = false;
            }
        }
        if (!ok) continue;

        progs[0].run();
        bool finite = true;
        for (int f = 0; f < 6 && finite; ++f) {
            *vm.var("b") = (f % 3 == 0) ? 1.0 : 0.0;
            if (f % 3 == 0) progs[2].run();
            progs[1].run();
            for (int gy = 0; gy < ex.gridH && finite; ++gy) {
                for (int gx = 0; gx < ex.gridW && finite; ++gx) {
                    double px = (double)gx / (ex.gridW - 1) * 2 - 1;
                    double py = (double)gy / (ex.gridH - 1) * 2 - 1;
                    *vm.var("x") = px; *vm.var("y") = py;
                    *vm.var("d") = std::sqrt(px*px + py*py) / std::sqrt(2.0);
                    *vm.var("r") = std::atan2(py, px) + 1.5707963;
                    progs[3].run();
                    if (!std::isfinite(*vm.var("x")) || !std::isfinite(*vm.var("y")) ||
                        !std::isfinite(*vm.var("d")) || !std::isfinite(*vm.var("r")))
                        finite = false;
                }
            }
        }
        if (!finite) {
            printf("FAIL DM corpus \"%s\": non-finite outputs\n", ex.name);
            g_failures++;
        }
    }

    if (g_failures == 0) {
        printf("EEL selftest: ALL PASS (unit tests + %d superscope + %d dynamic-movement scripts)\n",
               viz::kNumSuperscopeExamples, viz::kNumDynamicMovementExamples);
        return 0;
    }
    printf("EEL selftest: %d FAILURE(S)\n", g_failures);
    return 1;
}
