#include "gloam/audio.hpp"

namespace gloam::audio {

auto mix_at(const NoiseField& field, const Level& level, Coord listener, Dir facing, Coord source,
            std::int32_t emission) -> Mix {
  Mix mix{};
  mix.gain = gain_from_loudness(field.at(level, listener), emission);
  // Pan is computed even at silence. It costs four integer operations, and a
  // sink that wants to know where a silent voice WOULD have been — a mix
  // debugger, the diagnostic binary — gets a straight answer instead of a
  // centred one that means two different things.
  mix.pan = pan_from_bearing(facing, listener, source);
  return mix;
}

auto mix_reciprocal(const NoiseField& from_listener, const Level& level, Coord listener,
                    Dir facing, Coord source, std::int32_t emission) -> Mix {
  Mix mix{};
  // THE FIELD IS ROOTED AT THE LISTENER AND READ AT THE SOURCE — the opposite
  // ends from `mix_at`, and the same answer. `audio.hpp` carries the symmetry
  // argument and its precondition; the short version is that per-edge
  // attenuation is stored in twin pairs, so the quietest path between two cells
  // costs the same measured from either end.
  //
  // This is what makes a tick with sixteen simultaneous stings cost ONE
  // propagation instead of sixteen. Measured at §11's reference scale (32x32
  // cells, 16 monsters, every one of them stinging on the same tick):
  // 13.6 ms per-monster, 0.97 ms shared, against a 4 ms tick budget. The
  // per-monster reading was over budget by 3.4x, so this is a correctness fix
  // for §11 rather than a tidy-up.
  mix.gain = gain_from_loudness(from_listener.at(level, source), emission);
  mix.pan = pan_from_bearing(facing, listener, source);
  return mix;
}

auto mix_for(const Level& level, Coord listener, Dir facing, Coord source, std::int32_t emission,
             const Tuning& tuning) -> Mix {
  // THE SAME `propagate_noise` MONSTERS HEAR THROUGH, from the other end.
  //
  // `noise.hpp` states the rule this line obeys: "the audio mix calls the SAME
  // propagation, evaluated from the party's position instead of the monster's
  // ... do not add a second propagation path for audio." The field is built
  // from the SOURCE and read at the LISTENER, which is exactly what
  // `advance` does for a monster's senses with the two roles swapped.
  const auto field = propagate_noise(level, source, emission, tuning);
  return mix_at(field, level, listener, facing, source, emission);
}

}  // namespace gloam::audio
