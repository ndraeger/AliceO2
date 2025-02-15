// Copyright 2019-2020 CERN and copyright holders of ALICE O2.
// See https://alice-o2.web.cern.ch/copyright for details of the copyright holders.
// All rights not expressly granted are reserved.
//
// This software is distributed under the terms of the GNU General Public
// License v3 (GPL Version 3), copied verbatim in the file "COPYING".
//
// In applying this license CERN does not waive the privileges and immunities
// granted to it by virtue of its status as an Intergovernmental Organization
// or submit itself to any jurisdiction.
#include "Framework/TimesliceIndex.h"
#include "Framework/Signpost.h"
#include <fairmq/Channel.h>

O2_DECLARE_DYNAMIC_LOG(timeslice_index);

namespace o2::framework
{

TimesliceIndex::TimesliceIndex(size_t maxLanes, std::vector<InputChannelInfo>& channels)
  : mMaxLanes{maxLanes},
    mChannels{channels}
{
}

void TimesliceIndex::resize(size_t s)
{
  mVariables.resize(s);
  mPublishedVariables.resize(s);
  mDirty.resize(s, false);
}

void TimesliceIndex::associate(TimesliceId timestamp, TimesliceSlot slot)
{
  assert(mVariables.size() > slot.index);
  mVariables[slot.index].put({0, static_cast<uint64_t>(timestamp.value)});
  mVariables[slot.index].commit();
  mDirty[slot.index] = true;
  O2_SIGNPOST_ID_GENERATE(tid, timeslice_index);
  O2_SIGNPOST_EVENT_EMIT(timeslice_index, tid, "associate", "Associating timestamp %zu to slot %zu", timestamp.value, slot.index);
}

TimesliceSlot TimesliceIndex::findOldestSlot(TimesliceId timestamp) const
{
  size_t lane = timestamp.value % mMaxLanes;
  TimesliceSlot oldest{lane};
  auto oldPVal = std::get_if<uint64_t>(&mVariables[oldest.index].get(0));
  if (oldPVal == nullptr) {
    return oldest;
  }
  uint64_t oldTimestamp = *oldPVal;

  for (size_t i = lane + mMaxLanes; i < mVariables.size(); i += mMaxLanes) {
    auto newPVal = std::get_if<uint64_t>(&mVariables[i].get(0));
    if (newPVal == nullptr) {
      return TimesliceSlot{i};
    }
    uint64_t newTimestamp = *newPVal;

    if (oldTimestamp > newTimestamp) {
      oldest = TimesliceSlot{i};
      oldTimestamp = newTimestamp;
    }
  }
  return oldest;
}

std::tuple<TimesliceIndex::ActionTaken, TimesliceSlot> TimesliceIndex::replaceLRUWith(data_matcher::VariableContext& newContext, TimesliceId timestamp)
{
  auto oldestSlot = findOldestSlot(timestamp);
  O2_SIGNPOST_ID_GENERATE(tid, timeslice_index);
  if (TimesliceIndex::isValid(oldestSlot) == false) {
    mVariables[oldestSlot.index] = newContext;
    auto debugTimestamp = std::get_if<uint64_t>(&mVariables[oldestSlot.index].get(0));
    O2_SIGNPOST_EVENT_EMIT(timeslice_index, tid, "replaceLRUWith", "slot %zu timeslice %zu (%" PRIu64 ")", oldestSlot.index, timestamp.value, *debugTimestamp);
    return std::make_tuple(ActionTaken::ReplaceUnused, oldestSlot);
  }
  auto oldTimestamp = std::get_if<uint64_t>(&mVariables[oldestSlot.index].get(0));
  if (oldTimestamp == nullptr) {
    mVariables[oldestSlot.index] = newContext;
    auto debugTimestamp = std::get_if<uint64_t>(&mVariables[oldestSlot.index].get(0));
    O2_SIGNPOST_EVENT_EMIT(timeslice_index, tid, "replaceLRUWith", "slot %zu timeslice %zu (%" PRIu64 ")", oldestSlot.index, timestamp.value, *debugTimestamp);
    return std::make_tuple(ActionTaken::ReplaceUnused, oldestSlot);
  }

  auto newTimestamp = std::get_if<uint64_t>(&newContext.get(0));
  if (newTimestamp == nullptr) {
    return std::make_tuple(ActionTaken::DropInvalid, TimesliceSlot{TimesliceSlot::INVALID});
  }

  if (*newTimestamp > *oldTimestamp) {
    switch (mBackpressurePolicy) {
      case BackpressureOp::DropAncient: {
        mVariables[oldestSlot.index] = newContext;
        auto debugTimestamp = std::get_if<uint64_t>(&mVariables[oldestSlot.index].get(0));
        O2_SIGNPOST_EVENT_EMIT(timeslice_index, tid, "replaceLRUWith", "slot %zu timeslice %zu (%" PRIu64 ")", oldestSlot.index, timestamp.value, *debugTimestamp);
        return std::make_tuple(ActionTaken::ReplaceObsolete, oldestSlot);
      }
      case BackpressureOp::DropRecent:
        return std::make_tuple(ActionTaken::DropObsolete, TimesliceSlot{TimesliceSlot::INVALID});
      case BackpressureOp::Wait:
        return std::make_tuple(ActionTaken::Wait, TimesliceSlot{TimesliceSlot::INVALID});
    }
  } else {
    switch (mBackpressurePolicy) {
      case BackpressureOp::DropRecent: {
        mVariables[oldestSlot.index] = newContext;
        auto debugTimestamp = std::get_if<uint64_t>(&mVariables[oldestSlot.index].get(0));
        O2_SIGNPOST_EVENT_EMIT(timeslice_index, tid, "replaceLRUWith", "slot %zu timeslice %zu (%" PRIu64 ")", oldestSlot.index, timestamp.value, *debugTimestamp);
        return std::make_tuple(ActionTaken::ReplaceObsolete, oldestSlot);
      }
      case BackpressureOp::DropAncient:
        return std::make_tuple(ActionTaken::DropObsolete, TimesliceSlot{TimesliceSlot::INVALID});
      case BackpressureOp::Wait:
        return std::make_tuple(ActionTaken::Wait, TimesliceSlot{TimesliceSlot::INVALID});
    }
  }
  O2_BUILTIN_UNREACHABLE();
}

bool TimesliceIndex::didReceiveData() const
{
  bool expectsData = false;
  for (int ci = 0; ci < mChannels.size(); ci++) {
    auto& channel = mChannels[ci];
    // Ignore non data channels
    if (channel.channelType != ChannelAccountingType::DPL) {
      continue;
    }
    expectsData = true;
    // A data channel provided the oldest possible timeframe information
    // we therefore can safely assume that we got some
    // data from it.
    if (channel.oldestForChannel.value != 0) {
      return true;
    }
  }
  return expectsData == false;
}

TimesliceIndex::OldestInputInfo TimesliceIndex::setOldestPossibleInput(TimesliceId timestamp, ChannelIndex channel)
{
  O2_SIGNPOST_ID_GENERATE(tid, timeslice_index);
  // Each channel oldest possible input must be monotoically increasing.
  if (timestamp.value < mChannels[channel.value].oldestForChannel.value) {
    O2_SIGNPOST_EVENT_EMIT_ERROR(timeslice_index, tid, "setOldestPossibleInput",
                                 "Received bogus oldest possible timeslice %zu for channel %d. Expected >= %zu.",
                                 timestamp.value, channel.value, mChannels[channel.value].oldestForChannel.value);
  }
  mChannels[channel.value].oldestForChannel = timestamp;
  OldestInputInfo result{timestamp, channel};
  bool changed = false;
  for (int ci = 0; ci < mChannels.size(); ci++) {
    // Check if this is a real channel. Skip otherwise.
    auto& channelRef = mChannels[ci];
    if (channelRef.channelType != ChannelAccountingType::DPL) {
      continue;
    }
    auto& a = channelRef.oldestForChannel;
    if (a.value < result.timeslice.value) {
      changed = true;
      result = {a, ChannelIndex{ci}};
    }
  }
  if (changed && mOldestPossibleInput.timeslice.value != result.timeslice.value) {
    O2_SIGNPOST_EVENT_EMIT(timeslice_index, tid, "setOldestPossibleInput", "Success (channel %d): Oldest possible input is %zu due to channel %d",
                           channel.value, result.timeslice.value, result.channel.value);
  } else if (mOldestPossibleInput.timeslice.value != result.timeslice.value) {
    O2_SIGNPOST_EVENT_EMIT(timeslice_index, tid, "setOldestPossibleInput", "channel %d: Oldest possible input updated from timestamp: %zu --> %zu",
                           channel.value, mOldestPossibleInput.timeslice.value, result.timeslice.value);
  } else {
    O2_SIGNPOST_EVENT_EMIT(timeslice_index, tid, "setOldestPossibleInput", "No change in oldest possible input");
  }
  if (mOldestPossibleInput.timeslice.value > result.timeslice.value) {
    LOG(error) << "DPL internal error - oldestPossibleInput of channel " << channel.value << ": " << getChannelInfo(channel).channel->GetName().c_str() << " decreased from " << mOldestPossibleOutput.timeslice.value << " to " << result.timeslice.value;
  }
  mOldestPossibleInput = result;
  return mOldestPossibleInput;
}

bool TimesliceIndex::validateSlot(TimesliceSlot slot, TimesliceId currentOldest)
{
  if (mDirty[slot.index] == true) {
    return true;
  }

  auto timestamp = std::get_if<uint64_t>(&mVariables[slot.index].get(0));
  if (timestamp != nullptr && *timestamp < mOldestPossibleInput.timeslice.value) {
    markAsInvalid(slot);
    return false;
  }
  return true;
}

TimesliceIndex::OldestOutputInfo TimesliceIndex::updateOldestPossibleOutput()
{
  auto oldestInput = getOldestPossibleInput();
  OldestOutputInfo result{oldestInput.timeslice, oldestInput.channel};

  bool changed = false;
  for (size_t i = 0; i < mVariables.size(); i++) {
    // We do not check invalid slots.
    if (isValid(TimesliceSlot{i}) == false) {
      continue;
    }
    auto timestamp = std::get_if<uint64_t>(&mVariables[i].get(0));
    if (timestamp != nullptr && *timestamp < result.timeslice.value) {
      changed = true;
      result.timeslice = TimesliceId{*timestamp};
      result.slot = {i};
      result.channel = {(int)-1};
    }
  }
  O2_SIGNPOST_ID_GENERATE(tid, timeslice_index);
  if (mOldestPossibleOutput.timeslice.value != result.timeslice.value) {
    if (changed) {
      O2_SIGNPOST_EVENT_EMIT(timeslice_index, tid, "updateOldestPossibleOutput", "Oldest possible output %zu (before %zu) due to %s %zu",
                             result.timeslice.value, mOldestPossibleOutput.timeslice.value,
                             result.channel.value == -1 ? "slot" : "channel",
                             result.channel.value == -1 ? result.slot.index : result.channel.value);
    } else {
      O2_SIGNPOST_EVENT_EMIT(timeslice_index, tid, "updateOldestPossibleOutput", "Oldest possible output updated from oldest Input : %zu --> %zu",
                             mOldestPossibleOutput.timeslice.value, result.timeslice.value);
    }
  }
  if (mOldestPossibleOutput.timeslice.value > result.timeslice.value) {
    LOG(error) << "DPL internal error - oldestPossibleOutput decreased from " << mOldestPossibleOutput.timeslice.value << " to " << result.timeslice.value;
  }
  mOldestPossibleOutput = result;

  return result;
}

InputChannelInfo const& TimesliceIndex::getChannelInfo(ChannelIndex channel) const
{
  return mChannels[channel.value];
}

auto TimesliceIndex::reset() -> void
{
  mOldestPossibleInput = {
    .timeslice = {0},
    /// The actual channel id of the oldest input.
    .channel = {ChannelIndex::INVALID}};
  mOldestPossibleOutput = {
    .timeslice = {0},
    .channel = {ChannelIndex::INVALID},
    .slot = {(size_t)-1}};
  for (auto& channel : mChannels) {
    channel.oldestForChannel = {0};
  }
}

} // namespace o2::framework
