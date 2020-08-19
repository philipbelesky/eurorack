// Copyright 2017 Emilie Gillet.
//
// Author: Emilie Gillet (emilie.o.gillet@gmail.com)
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.
//
// See http://creativecommons.org/licenses/MIT/ for more information.
//
// -----------------------------------------------------------------------------
//
// Multi-stage envelope

#include "stages/segment_generator.h"

#include "stages/settings.h"
#include "stmlib/dsp/dsp.h"
#include "stmlib/dsp/parameter_interpolator.h"
#include "stmlib/dsp/units.h"

#include <cassert>
#include <cmath>
#include <algorithm>

#include "stages/resources.h"
#include "stmlib/utils/gate_flags.h"
#include "stmlib/utils/random.h"

namespace stages {

using namespace stmlib;
using namespace std;
using namespace segment;

// Duration of the "tooth" in the output when a trigger is received while the
// output is high.
const int kRetrigDelaySamples = 32;

// S&H delay (for all those sequencers whose CV and GATE outputs are out of
// sync).
const size_t kSampleAndHoldDelay = kSampleRate * 2 / 1000;  // 2 milliseconds

void SegmentGenerator::Init(Settings* settings) {
  process_fn_ = &SegmentGenerator::ProcessMultiSegment;

  settings_ = settings;

  phase_ = 0.0f;

  zero_ = 0.0f;
  half_ = 0.5f;
  one_ = 1.0f;

  start_ = 0.0f;
  value_ = 0.0f;
  lp_ = 0.0f;

  monitored_segment_ = 0;
  active_segment_ = 0;
  retrig_delay_ = 0;
  primary_ = 0;

  Segment s;
  s.start = &zero_;
  s.end = &zero_;
  s.time = &zero_;
  s.curve = &half_;
  s.portamento = &zero_;
  s.phase = NULL;
  s.if_rising = 0;
  s.if_falling = 0;
  s.if_complete = 0;
  s.bipolar = false;
  s.retrig = true;
  s.shift_register = Random::GetSample();
  s.register_value = Random::GetFloat();
  fill(&segments_[0], &segments_[kMaxNumSegments + 1], s);

  Parameters p;
  p.primary = 0.0f;
  p.secondary = 0.0f;
  fill(&parameters_[0], &parameters_[kMaxNumSegments], p);

  ramp_extractor_.Init(
      kSampleRate,
      1000.0f / kSampleRate);
  ramp_division_quantizer_.Init();
  delay_line_.Init();
  gate_delay_.Init();

  num_segments_ = 0;
}

inline float SegmentGenerator::WarpPhase(float t, float curve) const {
  curve -= 0.5f;
  const bool flip = curve < 0.0f;
  if (flip) {
    t = 1.0f - t;
  }
  const float a = 128.0f * curve * curve;
  t = (1.0f + a) * t / (1.0f + a * t);
  if (flip) {
    t = 1.0f - t;
  }
  return t;
}

inline float SegmentGenerator::RateToFrequency(float rate) const {
  int32_t i = static_cast<int32_t>(rate * 2048.0f);
  CONSTRAIN(i, 0, LUT_ENV_FREQUENCY_SIZE);
  return lut_env_frequency[i];
}

inline float SegmentGenerator::PortamentoRateToLPCoefficient(float rate) const {
  int32_t i = static_cast<int32_t>(rate * 512.0f);
  return lut_portamento_coefficient[i];
}

static void advance_tm(
    const size_t steps,
    const float prob,
    uint16_t& shift_register,
    float& register_value,
    bool bipolar) {
  uint16_t sr = shift_register;
  uint16_t copied_bit = (sr << (steps - 1)) & (1 << 15);
  // Ensure registers lock at extremes. Threshold established through trial and error, though
  // depend on power supply and such.
  // Tested at audio rates. Still allows you to let trickles of changes through if you want.
  float p = prob < 0.001f ? 0.0f : prob > 0.999f ? 1.1f : prob;
  uint16_t mutated = copied_bit ^ ((Random::GetFloat() < p) << 15);
  sr = (sr >> 1) | mutated;
  shift_register = sr;
  register_value = (float)(shift_register) / 65535.0f;
  if (bipolar) {
    register_value = (10.0f / 8.0f) * (register_value - 0.5f);
  }
}

void SegmentGenerator::ProcessMultiSegment(
    const GateFlags* gate_flags, SegmentGenerator::Output* out, size_t size) {
  float phase = phase_;
  float start = start_;
  float lp = lp_;
  float value = value_;

  while (size--) {
    const Segment& segment = segments_[active_segment_];

    if (segment.time) {
      phase += RateToFrequency(*segment.time);
    }

    bool complete = phase >= 1.0f;
    if (complete) {
      phase = 1.0f;
    }
    value = Crossfade(
        start,
        *segment.end,
        WarpPhase(segment.phase ? *segment.phase : phase, *segment.curve));

    ONE_POLE(lp, value, PortamentoRateToLPCoefficient(*segment.portamento));

    // Decide what to do next.
    int go_to_segment = -1;
    if ((*gate_flags & GATE_FLAG_RISING) && segment.retrig) {
      go_to_segment = segment.if_rising;
    } else if (*gate_flags & GATE_FLAG_FALLING) {
      go_to_segment = segment.if_falling;
    } else if (complete) {
      go_to_segment = segment.if_complete;
    }

    if (go_to_segment != -1) {
      if (segment.advance_tm) {
        const size_t steps = size_t(15 * parameters_[active_segment_].secondary + 1);
        const float prob = parameters_[active_segment_].primary;
        advance_tm(
            steps, prob,
            (&segments_[active_segment_])->shift_register,
            (&segments_[active_segment_])->register_value,
            segment.bipolar);
      }
      phase = 0.0f;
      const Segment& destination = segments_[go_to_segment];
      start = destination.start
          ? *destination.start
          : (go_to_segment == active_segment_ ? start : value);
      active_segment_ = go_to_segment;
    }

    out->value = lp;
    out->phase = phase;
    out->segment = active_segment_;
    ++gate_flags;
    ++out;
  }
  phase_ = phase;
  start_ = start;
  lp_ = lp;
  value_ = value;
}

void SegmentGenerator::ProcessDecayEnvelope(
    const GateFlags* gate_flags, SegmentGenerator::Output* out, size_t size) {
  const float frequency = RateToFrequency(parameters_[0].primary);
  while (size--) {
    if ((*gate_flags & GATE_FLAG_RISING) && (active_segment_ != 0 || segments_[0].retrig)) {
      phase_ = 0.0f;
      active_segment_ = 0;
    }

    phase_ += frequency;
    if (phase_ >= 1.0f) {
      phase_ = 1.0f;
      active_segment_ = 1;
    }
    lp_ = value_ = 1.0f - WarpPhase(phase_, parameters_[0].secondary);
    out->value = lp_;
    out->phase = phase_;
    out->segment = active_segment_;
    ++gate_flags;
    ++out;
  }
}

void SegmentGenerator::ProcessTimedPulseGenerator(
    const GateFlags* gate_flags, SegmentGenerator::Output* out, size_t size) {
  const float frequency = RateToFrequency(parameters_[0].secondary);

  ParameterInterpolator primary(&primary_, parameters_[0].primary, size);
  while (size--) {
    if ((*gate_flags & GATE_FLAG_RISING) && (active_segment_ != 0 || segments_[0].retrig)) {
      retrig_delay_ = active_segment_ == 0 ? kRetrigDelaySamples : 0;
      phase_ = 0.0f;
      active_segment_ = 0;
    }
    if (retrig_delay_) {
      --retrig_delay_;
    }
    phase_ += frequency;
    if (phase_ >= 1.0f) {
      phase_ = 1.0f;
      active_segment_ = 1;
    }

    const float p = primary.Next();
    lp_ = value_ = active_segment_ == 0 && !retrig_delay_ ? p : 0.0f;
    out->value = lp_;
    out->phase = phase_;
    out->segment = active_segment_;
    ++gate_flags;
    ++out;
  }
}

void SegmentGenerator::ProcessGateGenerator(
    const GateFlags* gate_flags, SegmentGenerator::Output* out, size_t size) {
  ParameterInterpolator primary(&primary_, parameters_[0].primary, size);
  while (size--) {
    active_segment_ = *gate_flags & GATE_FLAG_HIGH ? 0 : 1;

    const float p = primary.Next();
    lp_ = value_ = active_segment_ == 0 ? p : 0.0f;
    out->value = lp_;
    out->phase = 0.5f;
    out->segment = active_segment_;
    ++gate_flags;
    ++out;
  }
}

void SegmentGenerator::ProcessSampleAndHold(
    const GateFlags* gate_flags, SegmentGenerator::Output* out, size_t size) {
  const float coefficient = PortamentoRateToLPCoefficient(
      parameters_[0].secondary);
  ParameterInterpolator primary(&primary_, parameters_[0].primary, size);

  while (size--) {
    const float p = primary.Next();
    gate_delay_.Write(*gate_flags);
    if (gate_delay_.Read(kSampleAndHoldDelay) & GATE_FLAG_RISING) {
      value_ = p;
    }
    active_segment_ = *gate_flags & GATE_FLAG_HIGH ? 0 : 1;

    ONE_POLE(lp_, value_, coefficient);
    out->value = lp_;
    out->phase = 0.5f;
    out->segment = active_segment_;
    ++gate_flags;
    ++out;
  }
}

void SegmentGenerator::ProcessTrackAndHold(
    const GateFlags* gate_flags, SegmentGenerator::Output* out, size_t size) {
  const float coefficient = PortamentoRateToLPCoefficient(
      parameters_[0].secondary);
  ParameterInterpolator primary(&primary_, parameters_[0].primary, size);

  while (size--) {
    const float p = primary.Next();
    gate_delay_.Write(*gate_flags);
    if (gate_delay_.Read(kSampleAndHoldDelay) & GATE_FLAG_HIGH) {
      value_ = p;
    }
    active_segment_ = *gate_flags & GATE_FLAG_HIGH ? 0 : 1;

    ONE_POLE(lp_, value_, coefficient);
    out->value = lp_;
    out->phase = 0.5f;
    out->segment = active_segment_;
    ++gate_flags;
    ++out;
  }
}

void SegmentGenerator::ProcessClockedSampleAndHold(
    const GateFlags* gate_flags, SegmentGenerator::Output* out, size_t size) {
  const float frequency = RateToFrequency(parameters_[0].secondary);
  ParameterInterpolator primary(&primary_, parameters_[0].primary, size);
  while (size--) {
    phase_ += frequency;
    if (phase_ >= 1.0f) {
      phase_ -= 1.0f;

      const float reset_time = phase_ / frequency;
      value_ = primary.subsample(1.0f - reset_time);
    }
    primary.Next();
    active_segment_ = phase_ < 0.5f ? 0 : 1;
    out->value = value_;
    out->phase = phase_;
    out->segment = active_segment_;
    ++out;
  }
}

Ratio divider_ratios[] = {
  { 0.249999f, 4 },
  { 0.333333f, 3 },
  { 0.499999f, 2 },
  { 0.999999f, 1 },
  { 1.999999f, 1 },
  { 2.999999f, 1 },
  { 3.999999f, 1 },
};

Ratio divider_ratios_slow[] = {
  { 0.124999f, 8 },
  { 0.142856f, 7 },
  { 0.166666f, 6 },
  { 0.199999f, 5 },
  { 0.249999f, 4 },
  { 0.333333f, 3 },
  { 0.499999f, 2 },
  { 0.999999f, 1 },
};

Ratio divider_ratios_fast[] = {
  { 0.999999f, 1 },
  { 1.999999f, 1 },
  { 2.999999f, 1 },
  { 3.999999f, 1 },
  { 4.999999f, 1 },
  { 5.999999f, 1 },
  { 6.999999f, 1 },
  { 7.999999f, 1 },
};

void SegmentGenerator::ProcessTapLFO(
    const GateFlags* gate_flags, SegmentGenerator::Output* out, size_t size) {
  float ramp[12];
  Ratio r;
  switch (segments_[0].range) {
    case segment::RANGE_DEFAULT:
      r = ramp_division_quantizer_.Lookup(
          divider_ratios, parameters_[0].primary * 1.03f, 7);
      break;
    case segment::RANGE_SLOW:
      r = ramp_division_quantizer_.Lookup(
          divider_ratios_slow, parameters_[0].primary * 1.03f, 8);
      break;
    case segment::RANGE_FAST:
      r = ramp_division_quantizer_.Lookup(
          divider_ratios_fast, parameters_[0].primary * 1.03f, 8);
      break;
  }

  ramp_extractor_.Process(r, gate_flags, ramp, size);
  for (size_t i = 0; i < size; ++i) {
    out[i].phase = ramp[i];
  }
  ShapeLFO(parameters_[0].secondary, out, size, segments_[0].bipolar);
  active_segment_ = out[size - 1].segment;
}

void SegmentGenerator::ProcessFreeRunningLFO(
    const GateFlags* gate_flags, SegmentGenerator::Output* out, size_t size) {
  float f = 96.0f * (parameters_[0].primary - 0.5f);
  CONSTRAIN(f, -128.0f, 127.0f);

  float frequency = SemitonesToRatio(f) * 2.0439497f / kSampleRate;

  active_segment_ = 0;
  switch (segments_[active_segment_].range) {
    case segment::RANGE_SLOW:
      frequency /= 16;
      break;
    case segment::RANGE_FAST:
      frequency *= 64;
      frequency = min(frequency, 7040.0f / kSampleRate); // A8, things seems to get weird after this...
      break;
    default:
      // It's good where it is
      break;
  }

  if (settings_->state().multimode == MULTI_MODE_STAGES_SLOW_LFO) {
    frequency /= 8.0f;
  }

  for (size_t i = 0; i < size; ++i) {
    phase_ += frequency;
    if (phase_ >= 1.0f) {
      phase_ -= 1.0f;
    }
    out[i].phase = phase_;
  }
  ShapeLFO(parameters_[0].secondary, out, size, segments_[0].bipolar);
  active_segment_ = out[size - 1].segment;
}

void SegmentGenerator::ProcessDelay(
    const GateFlags* gate_flags, SegmentGenerator::Output* out, size_t size) {
  const float max_delay = static_cast<float>(kMaxDelay - 1);

  float delay_time = SemitonesToRatio(
      2.0f * (parameters_[0].secondary - 0.5f) * 36.0f) * 0.5f * kSampleRate;
  float clock_frequency = 1.0f;
  float delay_frequency = 1.0f / delay_time;

  if (delay_time >= max_delay) {
    clock_frequency = max_delay * delay_frequency;
    delay_time = max_delay;
  }
  ParameterInterpolator primary(&primary_, parameters_[0].primary, size);

  active_segment_ = 0;
  while (size--) {
    phase_ += clock_frequency;
    ONE_POLE(lp_, primary.Next(), clock_frequency);
    if (phase_ >= 1.0f) {
      phase_ -= 1.0f;
      delay_line_.Write(lp_);
    }

    aux_ += delay_frequency;
    if (aux_ >= 1.0f) {
      aux_ -= 1.0f;
    }
    active_segment_ = aux_ < 0.5f ? 0 : 1;

    ONE_POLE(
        value_,
        delay_line_.Read(delay_time - phase_),
        clock_frequency);
    out->value = value_;
    out->phase = aux_;
    out->segment = active_segment_;
    ++out;
  }
}

void SegmentGenerator::ProcessPortamento(
    const GateFlags* gate_flags, SegmentGenerator::Output* out, size_t size) {
  const float coefficient = PortamentoRateToLPCoefficient(
      parameters_[0].secondary);
  ParameterInterpolator primary(&primary_, parameters_[0].primary, size);

  active_segment_ = 0;
  while (size--) {
    value_ = primary.Next();
    ONE_POLE(lp_, value_, coefficient);
    out->value = lp_;
    out->phase = 0.5f;
    out->segment = active_segment_;
    ++out;
  }
}

void SegmentGenerator::ProcessRandom(
    const GateFlags* gate_flags, SegmentGenerator::Output* out, size_t size) {
  const float coefficient = PortamentoRateToLPCoefficient(
      parameters_[0].secondary);
  float f = 96.0f * (parameters_[0].primary - 0.5f);
  CONSTRAIN(f, -128.0f, 127.0f);

  float frequency = SemitonesToRatio(f) * 2.0439497f / kSampleRate;

  active_segment_ = 0;
  while (size--) {
    phase_ += frequency;
    if (phase_ >= 1.0f) {
      phase_ -= 1.0f;
      value_ = Random::GetFloat();
      if (segments_[0].bipolar) {
        value_ = 10.0f / 8.0f * (value_ - 0.5f);
      }
      active_segment_ = 1;
    }
    ONE_POLE(lp_, value_, coefficient);
    out->value = lp_;
    out->phase = 0.5f;
    out->segment = active_segment_;
    ++out;
  }
}

void SegmentGenerator::ProcessTuring(
    const GateFlags* gate_flags, SegmentGenerator::Output* out, size_t size) {
  const size_t steps = size_t(15 * parameters_[0].secondary + 1);
  ParameterInterpolator primary(&primary_, parameters_[0].primary, size);

  Segment* seg = &segments_[0];
  while (size--) {
    float prob = primary.Next();
    if (*gate_flags & GATE_FLAG_RISING) {
      advance_tm(steps, prob, seg->shift_register, seg->register_value, seg->bipolar);
      value_ = seg->register_value;
    }
    active_segment_ = *gate_flags & GATE_FLAG_HIGH ? 0 : 1;
    out->value = segments_[0].register_value;
    out->phase = 0.5f;
    out->segment = active_segment_;
    ++out;
    ++gate_flags;
  }
}

void SegmentGenerator::ProcessLogistic(
    const GateFlags* gate_flags, SegmentGenerator::Output* out, size_t size) {
  const float coefficient = PortamentoRateToLPCoefficient(
      parameters_[0].secondary);
  float r = 0.5f * parameters_[0].primary + 3.5f;
  if (value_ <= 0.0f) {
    value_ = Random::GetFloat();
  }

  while (size--) {
    if(*gate_flags & GATE_FLAG_RISING) {
      value_ *= r * (1 - value_);
    }
    active_segment_ = *gate_flags & GATE_FLAG_HIGH ? 0 : 1;

    ONE_POLE(lp_, value_, coefficient);
    out->value = segments_[0].bipolar ? 10.0f / 8.0f * (lp_ - 0.5) : lp_;
    out->phase = 0.5f;
    out->segment = active_segment_;
    ++out;
    ++gate_flags;
  }
}

void SegmentGenerator::ProcessZero(
    const GateFlags* gate_flags, SegmentGenerator::Output* out, size_t size) {

  value_ = 0.0f;
  active_segment_ = 1;
  while (size--) {
    out->value = 0.0f;
    out->phase = 0.5f;
    out->segment = 1;
    ++out;
  }
}

void SegmentGenerator::ProcessSlave(
    const GateFlags* gate_flags, SegmentGenerator::Output* out, size_t size) {
  while (size--) {
    active_segment_ = out->segment == monitored_segment_ ? 0 : 1;
    out->value = active_segment_ ? 0.0f : 1.0f - out->phase;
    ++out;
  }
}

/* static */
void SegmentGenerator::ShapeLFO(
    float shape,
    SegmentGenerator::Output* in_out,
    size_t size,
    bool bipolar) {
  shape -= 0.5f;
  shape = 2.0f + 9.999999f * shape / (1.0f + 3.0f * fabs(shape));

  const float slope = min(shape * 0.5f, 0.5f);
  const float plateau_width = max(shape - 3.0f, 0.0f);
  const float sine_amount = max(
      shape < 2.0f ? shape - 1.0f : 3.0f - shape, 0.0f);

  const float slope_up = 1.0f / slope;
  const float slope_down = 1.0f / (1.0f - slope);
  const float plateau = 0.5f * (1.0f - plateau_width);
  const float normalization = 1.0f / plateau;
  const float phase_shift = plateau_width * 0.25f;

  while (size--) {
    float phase = in_out->phase + phase_shift;
    if (phase > 1.0f) {
      phase -= 1.0f;
    }
    float triangle = phase < slope
        ? slope_up * phase
        : 1.0f - (phase - slope) * slope_down;
    triangle -= 0.5f;
    CONSTRAIN(triangle, -plateau, plateau);
    triangle = triangle * normalization;
    float sine = InterpolateWrap(lut_sine, phase + 0.75f, 1024.0f);
    const float amplitude = bipolar ? (10.0f / 16.0f) : 0.5f;
    const float offset = bipolar ? 0.0f : 0.5f;
    in_out->value = amplitude * Crossfade(triangle, sine, sine_amount) + offset;
    in_out->segment = phase < 0.5f ? 0 : 1;
    ++in_out;
  }
}

inline bool is_step(Configuration config) {
  // Looping Turing types are holds
  return config.type == TYPE_STEP
    || (config.type == TYPE_TURING && !config.loop);
}

void SegmentGenerator::Configure(
    bool has_trigger,
    const Configuration* segment_configuration,
    int num_segments) {
  if (num_segments == 1) {
    ConfigureSingleSegment(has_trigger, segment_configuration[0]);
    return;
  }
  num_segments_ = num_segments;

  // assert(has_trigger);

  process_fn_ = &SegmentGenerator::ProcessMultiSegment;

  // A first pass to collect loop points, and check for STEP segments.
  int loop_start = -1;
  int loop_end = -1;
  bool has_step_segments = false;
  int last_segment = num_segments - 1;
  int first_ramp_segment = -1;

  for (int i = 0; i <= last_segment; ++i) {
    has_step_segments = has_step_segments || is_step(segment_configuration[i]);
    if (segment_configuration[i].loop) {
      if (loop_start == -1) {
        loop_start = i;
      }
      loop_end = i;
    }
    if (segment_configuration[i].type == TYPE_RAMP) {
      if (first_ramp_segment == -1) {
        first_ramp_segment = i;
      }
    }
  }

  // Check if there are step segments inside the loop.
  bool has_step_segments_inside_loop = false;
  if (loop_start != -1) {
    for (int i = loop_start; i <= loop_end; ++i) {
      if (is_step(segment_configuration[i])) {
        has_step_segments_inside_loop = true;
        break;
      }
    }
  }

  for (int i = 0; i <= last_segment; ++i) {
    Segment* s = &segments_[i];
    s->bipolar = segment_configuration[i].bipolar;
    s->retrig = true;
    s->advance_tm = false;
    if (segment_configuration[i].type == TYPE_RAMP) {
      s->retrig = !s->bipolar; // For ramp, bipolar means don't retrig.
      s->start = (num_segments == 1) ? &one_ : NULL;
      s->time = &parameters_[i].primary;
      s->curve = &parameters_[i].secondary;
      s->portamento = &zero_;
      s->phase = NULL;

      if (i == last_segment) {
        s->end = &zero_;
      } else if (segment_configuration[i + 1].type == TYPE_TURING) {
        s->end = &segments_[i+1].register_value;
      } else if (segment_configuration[i + 1].type != TYPE_RAMP) {
        s->end = &parameters_[i + 1].primary;
      } else if (i == first_ramp_segment) {
        s->end = &one_;
      } else {
        s->end = &parameters_[i].secondary;
        // The whole "reuse the curve from other segment" thing
        // is a bit too complicated...
        //
        // for (int j = i + 1; j <= last_segment; ++j) {
        //   if (segment_configuration[j].type == TYPE_RAMP) {
        //     if (j == last_segment ||
        //         segment_configuration[j + 1].type != TYPE_RAMP) {
        //       s->curve = &parameters_[j].secondary;
        //       break;
        //     }
        //   }
        // }
        s->curve = &half_;
      }
    } else {
      s->start = s->end = &parameters_[i].primary;
      s->curve = &half_;
      if (segment_configuration[i].type == TYPE_STEP) {
        s->portamento = &parameters_[i].secondary;
        s->time = NULL;
        // Sample if there is a loop of length 1 on this segment. Otherwise
        // track.
        s->phase = i == loop_start && i == loop_end ? &zero_ : &one_;
      } else if (segment_configuration[i].type == TYPE_TURING) {
        s->start = s->end = &s->register_value;
        s->advance_tm = true;
        s->portamento = &zero_;
        s->time = NULL;
        s->phase = &zero_;
      } else {
        s->portamento = &zero_;
        // Hold if there's a loop of length 1 of this segment. Otherwise, use
        // the programmed time.
        s->time = i == loop_start && i == loop_end
            ? NULL : &parameters_[i].secondary;
        s->phase = &one_;  // Track the changes on the slider.
      }
    }

    s->if_complete = i == loop_end ? loop_start : i + 1;
    s->if_falling = loop_end == -1 || loop_end == last_segment || has_step_segments ? -1 : loop_end + 1;
    s->if_rising = 0;

    if (has_step_segments) {
      if (!has_step_segments_inside_loop && i >= loop_start && i <= loop_end) {
        s->if_rising = (loop_end + 1) % num_segments;
      } else {
        // Just go to the next stage.
        // s->if_rising = (i == loop_end) ? loop_start : (i + 1) % num_segments;

        // Find the next STEP segment.
        bool follow_loop = loop_end != -1;
        int next_step = i;
        while (!is_step(segment_configuration[next_step])) {
          ++next_step;
          if (follow_loop && next_step == loop_end + 1) {
            next_step = loop_start;
            follow_loop = false;
          }
          if (next_step >= num_segments) {
            next_step = num_segments - 1;
            break;
          }
        }
        s->if_rising = next_step == loop_end
            ? loop_start
            : (next_step + 1) % num_segments;
      }
    }
  }

  Segment* sentinel = &segments_[num_segments];
  sentinel->end = sentinel->start = segments_[num_segments - 1].end;
  sentinel->time = &zero_;
  sentinel->curve = &half_;
  sentinel->portamento = &zero_;
  sentinel->if_rising = 0;
  sentinel->if_falling = -1;
  sentinel->if_complete = loop_end == last_segment ? 0 : -1;

  // After changing the state of the module, we go to the sentinel.
  active_segment_ = num_segments;
}

/* static */
SegmentGenerator::ProcessFn SegmentGenerator::process_fn_table_[16] = {
  // RAMP
  &SegmentGenerator::ProcessZero,
  &SegmentGenerator::ProcessFreeRunningLFO,
  &SegmentGenerator::ProcessDecayEnvelope,
  &SegmentGenerator::ProcessTapLFO,

  // STEP
  &SegmentGenerator::ProcessPortamento,
  &SegmentGenerator::ProcessPortamento,
  &SegmentGenerator::ProcessSampleAndHold,
  &SegmentGenerator::ProcessSampleAndHold,

  // HOLD
  &SegmentGenerator::ProcessDelay,
  &SegmentGenerator::ProcessDelay,
  // &SegmentGenerator::ProcessClockedSampleAndHold,
  &SegmentGenerator::ProcessTimedPulseGenerator,
  &SegmentGenerator::ProcessGateGenerator,

  // These types can't normally be accessed, but are what random segments default
  // to in basic mode.
  &SegmentGenerator::ProcessZero,
  &SegmentGenerator::ProcessZero,
  &SegmentGenerator::ProcessZero,
  &SegmentGenerator::ProcessZero,
};

// Seems really silly to have to separate tables with just a single difference but meh
SegmentGenerator::ProcessFn SegmentGenerator::advanced_process_fn_table_[16] = {
  // RAMP
  &SegmentGenerator::ProcessZero,
  &SegmentGenerator::ProcessFreeRunningLFO,
  &SegmentGenerator::ProcessDecayEnvelope,
  &SegmentGenerator::ProcessTapLFO,

  // STEP
  &SegmentGenerator::ProcessPortamento,
  &SegmentGenerator::ProcessPortamento,
  &SegmentGenerator::ProcessSampleAndHold,
  &SegmentGenerator::ProcessTrackAndHold,

  // HOLD
  &SegmentGenerator::ProcessDelay,
  &SegmentGenerator::ProcessDelay,
  // &SegmentGenerator::ProcessClockedSampleAndHold,
  &SegmentGenerator::ProcessTimedPulseGenerator,
  &SegmentGenerator::ProcessGateGenerator,

  // TURING
  &SegmentGenerator::ProcessRandom,
  &SegmentGenerator::ProcessRandom,
  &SegmentGenerator::ProcessTuring,
  &SegmentGenerator::ProcessLogistic,
};


}  // namespace stages
