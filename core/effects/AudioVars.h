//
// AudioVars.h — the audio features (audio/Features.h) as script variables.
//
// Every scripted effect (Superscope, Dynamic Movement, the scripted
// transforms, the pixel shader) sets these each frame, beside AVS's own
// variables and bass / mid / treb / bpm / beatphase:
//
//   kick snare hat              drum envelopes: 1 at a hit, decaying
//   onset                       onset strength, 0..1
//   level                       overall loudness, 0..1 (automatic gain)
//   bass_att mid_att treb_att   slow averages of lows, mids and highs, 0..1
//
// A preset whose scripts assign one of these names keeps it: before these
// existed, that name was simply the preset's own variable, and AVS presets
// must behave as they always did. Without features (the Music plugin, test
// frames) kick follows the beat and the rest follow bass / mid / treble.
//
#pragma once
#include "VizFrame.h"
#include "EelVM.h"
#include <initializer_list>

namespace viz {

class AudioVars {
public:
    void bind(eel::VM& vm, std::initializer_list<const eel::Program*> programs)
    {
        static const char* const names[N] = {
            "kick", "snare", "hat", "onset", "level", "bass_att", "mid_att", "treb_att" };
        for (int i = 0; i < N; ++i) {
            _v[i].slot = vm.var(names[i]);
            _v[i].own = false;
            for (const eel::Program* p : programs)
                if (p && p->assigns(_v[i].slot)) _v[i].own = true;
        }
    }

    void set(const VizFrame& a) const
    {
        double v[N];
        if (a.hasFeatures) {
            v[KICK] = a.kick; v[SNARE] = a.snare; v[HAT] = a.hat; v[ONSET] = a.onset;
            v[LEVEL] = a.level; v[BASS_ATT] = a.bassAtt; v[MID_ATT] = a.midAtt; v[TREB_ATT] = a.trebleAtt;
        } else {
            v[KICK] = a.beat ? 1.0 : 0.0; v[SNARE] = v[HAT] = v[ONSET] = 0.0;
            v[LEVEL] = (a.bass + a.mid + a.treble) / 3.0;
            v[BASS_ATT] = a.bass; v[MID_ATT] = a.mid; v[TREB_ATT] = a.treble;
        }
        for (int i = 0; i < N; ++i)
            if (_v[i].slot && !_v[i].own) *_v[i].slot = v[i];
    }

private:
    enum { KICK, SNARE, HAT, ONSET, LEVEL, BASS_ATT, MID_ATT, TREB_ATT, N };
    struct V { double* slot = nullptr; bool own = false; };
    V _v[N];
};

} // namespace viz
