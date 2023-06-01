/*
 * Copyright (c) 2018, 2019, Red Hat, Inc. All rights reserved.
 * Copyright Amazon.com Inc. or its affiliates. All Rights Reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 *
 */

#include "precompiled.hpp"

#include "gc/shenandoah/heuristics/shenandoahAdaptiveHeuristics.hpp"
#include "gc/shenandoah/shenandoahCollectionSet.hpp"
#include "gc/shenandoah/shenandoahFreeSet.hpp"
#include "gc/shenandoah/shenandoahHeap.inline.hpp"
#include "gc/shenandoah/shenandoahGeneration.hpp"
#include "gc/shenandoah/shenandoahHeapRegion.inline.hpp"
#include "gc/shenandoah/shenandoahOldGeneration.hpp"
#include "gc/shenandoah/shenandoahYoungGeneration.hpp"
#include "logging/log.hpp"
#include "logging/logTag.hpp"
#include "utilities/quickSort.hpp"

// These constants are used to adjust the margin of error for the moving
// average of the allocation rate and cycle time. The units are standard
// deviations.
const double ShenandoahAdaptiveHeuristics::FULL_PENALTY_SD = 0.2;
const double ShenandoahAdaptiveHeuristics::DEGENERATE_PENALTY_SD = 0.1;

// These are used to decide if we want to make any adjustments at all
// at the end of a successful concurrent cycle.
const double ShenandoahAdaptiveHeuristics::LOWEST_EXPECTED_AVAILABLE_AT_END = -0.5;
const double ShenandoahAdaptiveHeuristics::HIGHEST_EXPECTED_AVAILABLE_AT_END = 0.5;

// These values are the confidence interval expressed as standard deviations.
// At the minimum confidence level, there is a 25% chance that the true value of
// the estimate (average cycle time or allocation rate) is not more than
// MINIMUM_CONFIDENCE standard deviations away from our estimate. Similarly, the
// MAXIMUM_CONFIDENCE interval here means there is a one in a thousand chance
// that the true value of our estimate is outside the interval. These are used
// as bounds on the adjustments applied at the outcome of a GC cycle.
const double ShenandoahAdaptiveHeuristics::MINIMUM_CONFIDENCE = 0.319; // 25%
const double ShenandoahAdaptiveHeuristics::MAXIMUM_CONFIDENCE = 3.291; // 99.9%

ShenandoahAdaptiveHeuristics::ShenandoahAdaptiveHeuristics(ShenandoahGeneration* generation) :
  ShenandoahHeuristics(generation),
  _margin_of_error_sd(ShenandoahAdaptiveInitialConfidence),
  _spike_threshold_sd(ShenandoahAdaptiveInitialSpikeThreshold),
  _last_trigger(OTHER),
  _available(Moving_Average_Samples, ShenandoahAdaptiveDecayFactor) { }

ShenandoahAdaptiveHeuristics::~ShenandoahAdaptiveHeuristics() {}

void ShenandoahAdaptiveHeuristics::choose_collection_set_from_regiondata(ShenandoahCollectionSet* cset,
                                                                         RegionData* data, size_t size,
                                                                         size_t actual_free) {
  size_t garbage_threshold = ShenandoahHeapRegion::region_size_bytes() * ShenandoahGarbageThreshold / 100;
  size_t ignore_threshold = ShenandoahHeapRegion::region_size_bytes() * ShenandoahIgnoreGarbageThreshold / 100;
  ShenandoahHeap* heap = ShenandoahHeap::heap();

  // The logic for cset selection in adaptive is as follows:
  //
  //   1. We cannot get cset larger than available free space. Otherwise we guarantee OOME
  //      during evacuation, and thus guarantee full GC. In practice, we also want to let
  //      application to allocate something. This is why we limit CSet to some fraction of
  //      available space. In non-overloaded heap, max_cset would contain all plausible candidates
  //      over garbage threshold.
  //
  //   2. We should not get cset too low so that free threshold would not be met right
  //      after the cycle. Otherwise we get back-to-back cycles for no reason if heap is
  //      too fragmented. In non-overloaded non-fragmented heap min_garbage would be around zero.
  //
  // Therefore, we start by sorting the regions by garbage. Then we unconditionally add the best candidates
  // before we meet min_garbage. Then we add all candidates that fit with a garbage threshold before
  // we hit max_cset. When max_cset is hit, we terminate the cset selection. Note that in this scheme,
  // ShenandoahGarbageThreshold is the soft threshold which would be ignored until min_garbage is hit.

  // In generational mode, the sort order within the data array is not strictly descending amounts of garbage.  In
  // particular, regions that have reached tenure age will be sorted into this array before younger regions that contain
  // more garbage.  This represents one of the reasons why we keep looking at regions even after we decide, for example,
  // to exclude one of the regions because it might require evacuation of too much live data.
  // TODO: Split it in the separate methods for clarity.
  bool is_generational = heap->mode()->is_generational();
  bool is_global = _generation->is_global();
  size_t capacity = heap->young_generation()->max_capacity();

  // cur_young_garbage represents the amount of memory to be reclaimed from young-gen.  In the case that live objects
  // are known to be promoted out of young-gen, we count this as cur_young_garbage because this memory is reclaimed
  // from young-gen and becomes available to serve future young-gen allocation requests.
  size_t cur_young_garbage = 0;

  // Better select garbage-first regions
  QuickSort::sort<RegionData>(data, (int)size, compare_by_garbage, false);

  if (is_generational) {
    for (size_t idx = 0; idx < size; idx++) {
      ShenandoahHeapRegion* r = data[idx]._region;
      if (cset->is_preselected(r->index())) {
        assert(r->age() >= InitialTenuringThreshold, "Preselected regions must have tenure age");
        // Entire region will be promoted, This region does not impact young-gen or old-gen evacuation reserve.
        // This region has been pre-selected and its impact on promotion reserve is already accounted for.

        // r->used() is r->garbage() + r->get_live_data_bytes()
        // Since all live data in this region is being evacuated from young-gen, it is as if this memory
        // is garbage insofar as young-gen is concerned.  Counting this as garbage reduces the need to
        // reclaim highly utilized young-gen regions just for the sake of finding min_garbage to reclaim
        // within youn-gen memory.

        cur_young_garbage += r->garbage();
        cset->add_region(r);
      }
    }
    if (is_global) {
      size_t max_young_cset    = (size_t) (heap->get_young_evac_reserve() / ShenandoahEvacWaste);
      size_t young_cur_cset = 0;
      size_t max_old_cset    = (size_t) (heap->get_old_evac_reserve() / ShenandoahOldEvacWaste);
      size_t old_cur_cset = 0;
      size_t free_target = (capacity * ShenandoahMinFreeThreshold) / 100 + max_young_cset;
      size_t min_garbage = (free_target > actual_free) ? (free_target - actual_free) : 0;

      log_info(gc, ergo)("Adaptive CSet Selection for GLOBAL. Max Young Evacuation: " SIZE_FORMAT
                         "%s, Max Old Evacuation: " SIZE_FORMAT "%s, Actual Free: " SIZE_FORMAT "%s.",
                         byte_size_in_proper_unit(max_young_cset),    proper_unit_for_byte_size(max_young_cset),
                         byte_size_in_proper_unit(max_old_cset),    proper_unit_for_byte_size(max_old_cset),
                         byte_size_in_proper_unit(actual_free), proper_unit_for_byte_size(actual_free));

      for (size_t idx = 0; idx < size; idx++) {
        ShenandoahHeapRegion* r = data[idx]._region;
        if (cset->is_preselected(r->index())) {
          continue;
        }
        bool add_region = false;
        if (r->is_old()) {
          size_t new_cset = old_cur_cset + r->get_live_data_bytes();
          if ((new_cset <= max_old_cset) && (r->garbage() > garbage_threshold)) {
            add_region = true;
            old_cur_cset = new_cset;
          }
        } else if (r->age() < InitialTenuringThreshold) {
          size_t new_cset = young_cur_cset + r->get_live_data_bytes();
          size_t region_garbage = r->garbage();
          size_t new_garbage = cur_young_garbage + region_garbage;
          bool add_regardless = (region_garbage > ignore_threshold) && (new_garbage < min_garbage);
          if ((new_cset <= max_young_cset) && (add_regardless || (region_garbage > garbage_threshold))) {
            add_region = true;
            young_cur_cset = new_cset;
            cur_young_garbage = new_garbage;
          }
        }
        // Note that we do not add aged regions if they were not pre-selected.  The reason they were not preselected
        // is because there is not sufficient room in old-gen to hold their to-be-promoted live objects.

        if (add_region) {
          cset->add_region(r);
        }
      }
    } else {
      // This is young-gen collection or a mixed evacuation.  If this is mixed evacuation, the old-gen candidate regions
      // have already been added.
      size_t max_cset    = (size_t) (heap->get_young_evac_reserve() / ShenandoahEvacWaste);
      size_t cur_cset = 0;
      size_t free_target = (capacity * ShenandoahMinFreeThreshold) / 100 + max_cset;
      size_t min_garbage = (free_target > actual_free) ? (free_target - actual_free) : 0;

      log_info(gc, ergo)("Adaptive CSet Selection for YOUNG. Max Evacuation: " SIZE_FORMAT "%s, Actual Free: " SIZE_FORMAT "%s.",
                         byte_size_in_proper_unit(max_cset),    proper_unit_for_byte_size(max_cset),
                         byte_size_in_proper_unit(actual_free), proper_unit_for_byte_size(actual_free));

      for (size_t idx = 0; idx < size; idx++) {
        ShenandoahHeapRegion* r = data[idx]._region;
        if (cset->is_preselected(r->index())) {
          continue;
        }
        if  (r->age() < InitialTenuringThreshold) {
          size_t new_cset = cur_cset + r->get_live_data_bytes();
          size_t region_garbage = r->garbage();
          size_t new_garbage = cur_young_garbage + region_garbage;
          bool add_regardless = (region_garbage > ignore_threshold) && (new_garbage < min_garbage);
          assert(r->is_young(), "Only young candidates expected in the data array");
          if ((new_cset <= max_cset) && (add_regardless || (region_garbage > garbage_threshold))) {
            cur_cset = new_cset;
            cur_young_garbage = new_garbage;
            cset->add_region(r);
          }
        }
        // Note that we do not add aged regions if they were not pre-selected.  The reason they were not preselected
        // is because there is not sufficient room in old-gen to hold their to-be-promoted live objects or because
        // they are to be promoted in place.
      }
    }
  } else {
    // Traditional Shenandoah (non-generational)
    size_t capacity    = ShenandoahHeap::heap()->max_capacity();
    size_t max_cset    = (size_t)((1.0 * capacity / 100 * ShenandoahEvacReserve) / ShenandoahEvacWaste);
    size_t free_target = (capacity * ShenandoahMinFreeThreshold) / 100 + max_cset;
    size_t min_garbage = (free_target > actual_free) ? (free_target - actual_free) : 0;

    log_info(gc, ergo)("Adaptive CSet Selection. Target Free: " SIZE_FORMAT "%s, Actual Free: "
                     SIZE_FORMAT "%s, Max Evacuation: " SIZE_FORMAT "%s, Min Garbage: " SIZE_FORMAT "%s",
                     byte_size_in_proper_unit(free_target), proper_unit_for_byte_size(free_target),
                     byte_size_in_proper_unit(actual_free), proper_unit_for_byte_size(actual_free),
                     byte_size_in_proper_unit(max_cset),    proper_unit_for_byte_size(max_cset),
                     byte_size_in_proper_unit(min_garbage), proper_unit_for_byte_size(min_garbage));

    size_t cur_cset = 0;
    size_t cur_garbage = 0;

    for (size_t idx = 0; idx < size; idx++) {
      ShenandoahHeapRegion* r = data[idx]._region;

      size_t new_cset    = cur_cset + r->get_live_data_bytes();
      size_t new_garbage = cur_garbage + r->garbage();

      if (new_cset > max_cset) {
        break;
      }

      if ((new_garbage < min_garbage) || (r->garbage() > garbage_threshold)) {
        cset->add_region(r);
        cur_cset = new_cset;
        cur_garbage = new_garbage;
      }
    }
  }

  size_t collected_old = cset->get_old_bytes_reserved_for_evacuation();
  size_t collected_promoted = cset->get_young_bytes_to_be_promoted();
  size_t collected_young = cset->get_young_bytes_reserved_for_evacuation();

  log_info(gc, ergo)("Chosen CSet evacuates young: " SIZE_FORMAT "%s (of which at least: " SIZE_FORMAT "%s are to be promoted), "
                     "old: " SIZE_FORMAT "%s",
                     byte_size_in_proper_unit(collected_young),    proper_unit_for_byte_size(collected_young),
                     byte_size_in_proper_unit(collected_promoted), proper_unit_for_byte_size(collected_promoted),
                     byte_size_in_proper_unit(collected_old),      proper_unit_for_byte_size(collected_old));
}

void ShenandoahAdaptiveHeuristics::record_cycle_start() {
  ShenandoahHeuristics::record_cycle_start();
  _allocation_rate.allocation_counter_reset();
}

void ShenandoahAdaptiveHeuristics::record_success_concurrent(bool abbreviated) {
  ShenandoahHeuristics::record_success_concurrent(abbreviated);

  size_t available = MIN2(_generation->available(), ShenandoahHeap::heap()->free_set()->available());

  double z_score = 0.0;
  double available_sd = _available.sd();
  if (available_sd > 0) {
    double available_avg = _available.avg();
    z_score = (double(available) - available_avg) / available_sd;
    log_debug(gc, ergo)("%s Available: " SIZE_FORMAT " %sB, z-score=%.3f. Average available: %.1f %sB +/- %.1f %sB.",
                        _generation->name(),
                        byte_size_in_proper_unit(available),     proper_unit_for_byte_size(available),
                        z_score,
                        byte_size_in_proper_unit(available_avg), proper_unit_for_byte_size(available_avg),
                        byte_size_in_proper_unit(available_sd),  proper_unit_for_byte_size(available_sd));
  }

  _available.add(double(available));

  // In the case when a concurrent GC cycle completes successfully but with an
  // unusually small amount of available memory we will adjust our trigger
  // parameters so that they are more likely to initiate a new cycle.
  // Conversely, when a GC cycle results in an above average amount of available
  // memory, we will adjust the trigger parameters to be less likely to initiate
  // a GC cycle.
  //
  // The z-score we've computed is in no way statistically related to the
  // trigger parameters, but it has the nice property that worse z-scores for
  // available memory indicate making larger adjustments to the trigger
  // parameters. It also results in fewer adjustments as the application
  // stabilizes.
  //
  // In order to avoid making endless and likely unnecessary adjustments to the
  // trigger parameters, the change in available memory (with respect to the
  // average) at the end of a cycle must be beyond these threshold values.
  if (z_score < LOWEST_EXPECTED_AVAILABLE_AT_END ||
      z_score > HIGHEST_EXPECTED_AVAILABLE_AT_END) {
    // The sign is flipped because a negative z-score indicates that the
    // available memory at the end of the cycle is below average. Positive
    // adjustments make the triggers more sensitive (i.e., more likely to fire).
    // The z-score also gives us a measure of just how far below normal. This
    // property allows us to adjust the trigger parameters proportionally.
    //
    // The `100` here is used to attenuate the size of our adjustments. This
    // number was chosen empirically. It also means the adjustments at the end of
    // a concurrent cycle are an order of magnitude smaller than the adjustments
    // made for a degenerated or full GC cycle (which themselves were also
    // chosen empirically).
    adjust_last_trigger_parameters(z_score / -100);
  }
}

void ShenandoahAdaptiveHeuristics::record_success_degenerated() {
  ShenandoahHeuristics::record_success_degenerated();
  // Adjust both trigger's parameters in the case of a degenerated GC because
  // either of them should have triggered earlier to avoid this case.
  adjust_margin_of_error(DEGENERATE_PENALTY_SD);
  adjust_spike_threshold(DEGENERATE_PENALTY_SD);
}

void ShenandoahAdaptiveHeuristics::record_success_full() {
  ShenandoahHeuristics::record_success_full();
  // Adjust both trigger's parameters in the case of a full GC because
  // either of them should have triggered earlier to avoid this case.
  adjust_margin_of_error(FULL_PENALTY_SD);
  adjust_spike_threshold(FULL_PENALTY_SD);
}

static double saturate(double value, double min, double max) {
  return MAX2(MIN2(value, max), min);
}

// Return a conservative estimate of how much memory can be allocated before we need to start GC. The estimate is based
// on memory that is currently available within young generation plus all of the memory that will be added to the young
// generation at the end of the current cycle (as represented by young_regions_to_be_reclaimed) and on the anticipated
// amount of time required to perform a GC.
size_t ShenandoahAdaptiveHeuristics::bytes_of_allocation_runway_before_gc_trigger(size_t young_regions_to_be_reclaimed) {
  assert(_generation->is_young(), "Only meaningful for young-gen heuristic");

  size_t max_capacity = _generation->max_capacity();
  size_t capacity = _generation->soft_max_capacity();
  size_t usage = _generation->used();
  size_t available = (capacity > usage)? capacity - usage: 0;
  size_t allocated = _generation->bytes_allocated_since_gc_start();

  size_t available_young_collected = ShenandoahHeap::heap()->collection_set()->get_young_available_bytes_collected();
  size_t anticipated_available =
    available + young_regions_to_be_reclaimed * ShenandoahHeapRegion::region_size_bytes() - available_young_collected;
  size_t allocation_headroom = anticipated_available;
  size_t spike_headroom = capacity * ShenandoahAllocSpikeFactor / 100;
  size_t penalties      = capacity * _gc_time_penalties / 100;

  double rate = _allocation_rate.sample(allocated);

  // At what value of available, would avg and spike triggers occur?
  //  if allocation_headroom < avg_cycle_time * avg_alloc_rate, then we experience avg trigger
  //  if allocation_headroom < avg_cycle_time * rate, then we experience spike trigger if is_spiking
  //
  // allocation_headroom =
  //     0, if penalties > available or if penalties + spike_headroom > available
  //     available - penalties - spike_headroom, otherwise
  //
  // so we trigger if available - penalties - spike_headroom < avg_cycle_time * avg_alloc_rate, which is to say
  //                  available < avg_cycle_time * avg_alloc_rate + penalties + spike_headroom
  //            or if available < penalties + spike_headroom
  //
  // since avg_cycle_time * avg_alloc_rate > 0, the first test is sufficient to test both conditions
  //
  // thus, evac_slack_avg is MIN2(0,  available - avg_cycle_time * avg_alloc_rate + penalties + spike_headroom)
  //
  // similarly, evac_slack_spiking is MIN2(0, available - avg_cycle_time * rate + penalties + spike_headroom)
  // but evac_slack_spiking is only relevant if is_spiking, as defined below.

  double avg_cycle_time = _gc_cycle_time_history->davg() + (_margin_of_error_sd * _gc_cycle_time_history->dsd());

  // TODO: Consider making conservative adjustments to avg_cycle_time, such as: (avg_cycle_time *= 2) in cases where
  // we expect a longer-than-normal GC duration.  This includes mixed evacuations, evacuation that perform promotion
  // including promotion in place, and OLD GC bootstrap cycles.  It has been observed that these cycles sometimes
  // require twice or more the duration of "normal" GC cycles.  We have experimented with this approach.  While it
  // does appear to reduce the frequency of degenerated cycles due to late triggers, it also has the effect of reducing
  // evacuation slack so that there is less memory available to be transferred to OLD.  The result is that we
  // throttle promotion and it takes too long to move old objects out of the young generation.

  double avg_alloc_rate = _allocation_rate.upper_bound(_margin_of_error_sd);
  size_t evac_slack_avg;
  if (anticipated_available > avg_cycle_time * avg_alloc_rate + penalties + spike_headroom) {
    evac_slack_avg = anticipated_available - (avg_cycle_time * avg_alloc_rate + penalties + spike_headroom);
  } else {
    // we have no slack because it's already time to trigger
    evac_slack_avg = 0;
  }

  bool is_spiking = _allocation_rate.is_spiking(rate, _spike_threshold_sd);
  size_t evac_slack_spiking;
  if (is_spiking) {
    if (anticipated_available > avg_cycle_time * rate + penalties + spike_headroom) {
      evac_slack_spiking = anticipated_available - (avg_cycle_time * rate + penalties + spike_headroom);
    } else {
      // we have no slack because it's already time to trigger
      evac_slack_spiking = 0;
    }
  } else {
    evac_slack_spiking = evac_slack_avg;
  }

  size_t threshold = min_free_threshold();
  size_t evac_min_threshold = (anticipated_available > threshold)? anticipated_available - threshold: 0;
  return MIN3(evac_slack_spiking, evac_slack_avg, evac_min_threshold);
}

bool ShenandoahAdaptiveHeuristics::should_start_gc() {
  size_t capacity = _generation->soft_max_capacity();
  size_t available = _generation->soft_available();
  size_t allocated = _generation->bytes_allocated_since_gc_start();

  log_debug(gc)("should_start_gc (%s)? available: " SIZE_FORMAT ", soft_max_capacity: " SIZE_FORMAT
                ", allocated: " SIZE_FORMAT,
                _generation->name(), available, capacity, allocated);

  // The collector reserve may eat into what the mutator is allowed to use. Make sure we are looking
  // at what is available to the mutator when deciding whether to start a GC.
  size_t usable = ShenandoahHeap::heap()->free_set()->available();
  if (usable < available) {
    log_debug(gc)("Usable (" SIZE_FORMAT "%s) is less than available (" SIZE_FORMAT "%s)",
                  byte_size_in_proper_unit(usable),    proper_unit_for_byte_size(usable),
                  byte_size_in_proper_unit(available), proper_unit_for_byte_size(available));
    available = usable;
  }

  // Track allocation rate even if we decide to start a cycle for other reasons.
  double rate = _allocation_rate.sample(allocated);
  _last_trigger = OTHER;

  // OLD generation is maintained to be as small as possible.  Depletion-of-free-pool triggers do not apply to old generation.
  if (!_generation->is_old()) {
    size_t min_threshold = min_free_threshold();
    if (available < min_threshold) {
      log_info(gc)("Trigger (%s): Free (" SIZE_FORMAT "%s) is below minimum threshold (" SIZE_FORMAT "%s)",
                   _generation->name(),
                   byte_size_in_proper_unit(available), proper_unit_for_byte_size(available),
                   byte_size_in_proper_unit(min_threshold),       proper_unit_for_byte_size(min_threshold));
      return true;
    }

    // Check if we need to learn a bit about the application
    const size_t max_learn = ShenandoahLearningSteps;
    if (_gc_times_learned < max_learn) {
      size_t init_threshold = capacity / 100 * ShenandoahInitFreeThreshold;
      if (available < init_threshold) {
        log_info(gc)("Trigger (%s): Learning " SIZE_FORMAT " of " SIZE_FORMAT ". Free ("
                     SIZE_FORMAT "%s) is below initial threshold (" SIZE_FORMAT "%s)",
                     _generation->name(), _gc_times_learned + 1, max_learn,
                     byte_size_in_proper_unit(available), proper_unit_for_byte_size(available),
                     byte_size_in_proper_unit(init_threshold),      proper_unit_for_byte_size(init_threshold));
        return true;
      }
    }

    //  Rationale:
    //    The idea is that there is an average allocation rate and there are occasional abnormal bursts (or spikes) of
    //    allocations that exceed the average allocation rate.  What do these spikes look like?
    //
    //    1. At certain phase changes, we may discard large amounts of data and replace it with large numbers of newly
    //       allocated objects.  This "spike" looks more like a phase change.  We were in steady state at M bytes/sec
    //       allocation rate and now we're in a "reinitialization phase" that looks like N bytes/sec.  We need the "spike"
    //       accomodation to give us enough runway to recalibrate our "average allocation rate".
    //
    //   2. The typical workload changes.  "Suddenly", our typical workload of N TPS increases to N+delta TPS.  This means
    //       our average allocation rate needs to be adjusted.  Once again, we need the "spike" accomodation to give us
    //       enough runway to recalibrate our "average allocation rate".
    //
    //    3. Though there is an "average" allocation rate, a given workload's demand for allocation may be very bursty.  We
    //       allocate a bunch of LABs during the 5 ms that follow completion of a GC, then we perform no more allocations for
    //       the next 150 ms.  It seems we want the "spike" to represent the maximum divergence from average within the
    //       period of time between consecutive evaluation of the should_start_gc() service.  Here's the thinking:
    //
    //       a) Between now and the next time I ask whether should_start_gc(), we might experience a spike representing
    //          the anticipated burst of allocations.  If that would put us over budget, then we should start GC immediately.
    //       b) Between now and the anticipated depletion of allocation pool, there may be two or more bursts of allocations.
    //          If there are more than one of these bursts, we can "approximate" that these will be separated by spans of
    //          time with very little or no allocations so the "average" allocation rate should be a suitable approximation
    //          of how this will behave.
    //
    //    For cases 1 and 2, we need to "quickly" recalibrate the average allocation rate whenever we detect a change
    //    in operation mode.  We want some way to decide that the average rate has changed.  Make average allocation rate
    //    computations an independent effort.


    // Check if allocation headroom is still okay. This also factors in:
    //   1. Some space to absorb allocation spikes (ShenandoahAllocSpikeFactor)
    //   2. Accumulated penalties from Degenerated and Full GC

    size_t allocation_headroom = available;
    size_t spike_headroom = capacity / 100 * ShenandoahAllocSpikeFactor;
    size_t penalties      = capacity / 100 * _gc_time_penalties;

    allocation_headroom -= MIN2(allocation_headroom, penalties);
    allocation_headroom -= MIN2(allocation_headroom, spike_headroom);

    double avg_cycle_time = _gc_cycle_time_history->davg() + (_margin_of_error_sd * _gc_cycle_time_history->dsd());
    double avg_alloc_rate = _allocation_rate.upper_bound(_margin_of_error_sd);
    log_debug(gc)("%s: average GC time: %.2f ms, allocation rate: %.0f %s/s",
                  _generation->name(),
                  avg_cycle_time * 1000, byte_size_in_proper_unit(avg_alloc_rate), proper_unit_for_byte_size(avg_alloc_rate));

    if (avg_cycle_time > allocation_headroom / avg_alloc_rate) {

      log_info(gc)("Trigger (%s): Average GC time (%.2f ms) is above the time for average allocation rate (%.0f %sB/s)"
                   " to deplete free headroom (" SIZE_FORMAT "%s) (margin of error = %.2f)",
                   _generation->name(), avg_cycle_time * 1000,
                   byte_size_in_proper_unit(avg_alloc_rate), proper_unit_for_byte_size(avg_alloc_rate),
                   byte_size_in_proper_unit(allocation_headroom), proper_unit_for_byte_size(allocation_headroom),
                   _margin_of_error_sd);

      log_info(gc, ergo)("Free headroom: " SIZE_FORMAT "%s (free) - " SIZE_FORMAT "%s (spike) - "
                         SIZE_FORMAT "%s (penalties) = " SIZE_FORMAT "%s",
                         byte_size_in_proper_unit(available),           proper_unit_for_byte_size(available),
                         byte_size_in_proper_unit(spike_headroom),      proper_unit_for_byte_size(spike_headroom),
                         byte_size_in_proper_unit(penalties),           proper_unit_for_byte_size(penalties),
                         byte_size_in_proper_unit(allocation_headroom), proper_unit_for_byte_size(allocation_headroom));

      _last_trigger = RATE;
      return true;
    }

    bool is_spiking = _allocation_rate.is_spiking(rate, _spike_threshold_sd);
    if (is_spiking && avg_cycle_time > allocation_headroom / rate) {
      log_info(gc)("Trigger (%s): Average GC time (%.2f ms) is above the time for instantaneous allocation rate (%.0f %sB/s)"
                   " to deplete free headroom (" SIZE_FORMAT "%s) (spike threshold = %.2f)",
                   _generation->name(), avg_cycle_time * 1000,
                   byte_size_in_proper_unit(rate), proper_unit_for_byte_size(rate),
                   byte_size_in_proper_unit(allocation_headroom), proper_unit_for_byte_size(allocation_headroom),
                   _spike_threshold_sd);
      _last_trigger = SPIKE;
      return true;
    }

    ShenandoahHeap* heap = ShenandoahHeap::heap();
    if (heap->mode()->is_generational()) {
      // Get through promotions and mixed evacuations as quickly as possible.  These cycles sometimes require significantly
      // more time than traditional young-generation cycles so start them up as soon as possible.  This is a "mitigation"
      // for the reality that old-gen and young-gen activities are not truly "concurrent".  If there is old-gen work to
      // be done, we start up the young-gen GC threads so they can do some of this old-gen work.  As implemented, promotion
      // gets priority over old-gen marking.

      size_t promo_potential = heap->get_promotion_potential();
      size_t promo_in_place_potential = heap->get_promotion_in_place_potential();
      ShenandoahOldHeuristics* old_heuristics = (ShenandoahOldHeuristics*) heap->old_generation()->heuristics();
      size_t mixed_candidates = old_heuristics->unprocessed_old_collection_candidates();
      if (promo_potential > 0) {
        // Detect unsigned arithmetic underflow
        assert(promo_potential < heap->capacity(), "Sanity");
        log_info(gc)("Trigger (%s): expedite promotion of " SIZE_FORMAT "%s",
                     _generation->name(), byte_size_in_proper_unit(promo_potential), proper_unit_for_byte_size(promo_potential));
        return true;
      } else if (promo_in_place_potential > 0) {
        // Detect unsigned arithmetic underflow
        assert(promo_in_place_potential < heap->capacity(), "Sanity");
        log_info(gc)("Trigger (%s): expedite promotion in place of " SIZE_FORMAT "%s", _generation->name(),
                     byte_size_in_proper_unit(promo_in_place_potential),
                     proper_unit_for_byte_size(promo_in_place_potential));
        return true;
      } else if (mixed_candidates > 0) {
        // We need to run young GC in order to open up some free heap regions so we can finish mixed evacuations.
        log_info(gc)("Trigger (%s): expedite mixed evacuation of " SIZE_FORMAT " regions",
                     _generation->name(), mixed_candidates);
        return true;
      }
    }
  }
  return ShenandoahHeuristics::should_start_gc();
}

void ShenandoahAdaptiveHeuristics::adjust_last_trigger_parameters(double amount) {
  switch (_last_trigger) {
    case RATE:
      adjust_margin_of_error(amount);
      break;
    case SPIKE:
      adjust_spike_threshold(amount);
      break;
    case OTHER:
      // nothing to adjust here.
      break;
    default:
      ShouldNotReachHere();
  }
}

void ShenandoahAdaptiveHeuristics::adjust_margin_of_error(double amount) {
  _margin_of_error_sd = saturate(_margin_of_error_sd + amount, MINIMUM_CONFIDENCE, MAXIMUM_CONFIDENCE);
  log_debug(gc, ergo)("Margin of error now %.2f", _margin_of_error_sd);
}

void ShenandoahAdaptiveHeuristics::adjust_spike_threshold(double amount) {
  _spike_threshold_sd = saturate(_spike_threshold_sd - amount, MINIMUM_CONFIDENCE, MAXIMUM_CONFIDENCE);
  log_debug(gc, ergo)("Spike threshold now: %.2f", _spike_threshold_sd);
}

ShenandoahAllocationRate::ShenandoahAllocationRate() :
  _last_sample_time(os::elapsedTime()),
  _last_sample_value(0),
  _interval_sec(1.0 / ShenandoahAdaptiveSampleFrequencyHz),
  _rate(int(ShenandoahAdaptiveSampleSizeSeconds * ShenandoahAdaptiveSampleFrequencyHz), ShenandoahAdaptiveDecayFactor),
  _rate_avg(int(ShenandoahAdaptiveSampleSizeSeconds * ShenandoahAdaptiveSampleFrequencyHz), ShenandoahAdaptiveDecayFactor) {
}

double ShenandoahAllocationRate::sample(size_t allocated) {
  double now = os::elapsedTime();
  double rate = 0.0;
  if (now - _last_sample_time > _interval_sec) {
    if (allocated >= _last_sample_value) {
      rate = instantaneous_rate(now, allocated);
      _rate.add(rate);
      _rate_avg.add(_rate.avg());
    }

    _last_sample_time = now;
    _last_sample_value = allocated;
  }
  return rate;
}

double ShenandoahAllocationRate::upper_bound(double sds) const {
  // Here we are using the standard deviation of the computed running
  // average, rather than the standard deviation of the samples that went
  // into the moving average. This is a much more stable value and is tied
  // to the actual statistic in use (moving average over samples of averages).
  return _rate.davg() + (sds * _rate_avg.dsd());
}

void ShenandoahAllocationRate::allocation_counter_reset() {
  _last_sample_time = os::elapsedTime();
  _last_sample_value = 0;
}

bool ShenandoahAllocationRate::is_spiking(double rate, double threshold) const {
  if (rate <= 0.0) {
    return false;
  }

  double sd = _rate.sd();
  if (sd > 0) {
    // There is a small chance that that rate has already been sampled, but it
    // seems not to matter in practice.
    double z_score = (rate - _rate.avg()) / sd;
    if (z_score > threshold) {
      return true;
    }
  }
  return false;
}

double ShenandoahAllocationRate::instantaneous_rate(double time, size_t allocated) const {
  size_t last_value = _last_sample_value;
  double last_time = _last_sample_time;
  size_t allocation_delta = (allocated > last_value) ? (allocated - last_value) : 0;
  double time_delta_sec = time - last_time;
  return (time_delta_sec > 0)  ? (allocation_delta / time_delta_sec) : 0;
}
