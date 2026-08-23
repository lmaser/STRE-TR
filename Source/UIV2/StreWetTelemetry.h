#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace TR::StreUIV2
{
class StreWetTelemetry
{
public:
    static constexpr std::size_t ringCapacity = 32768;
    static constexpr std::size_t snapshotCapacity = 2048;

    struct Snapshot
    {
        std::array<float, snapshotCapacity> samples {};
        std::size_t sampleCount = 0;
        std::uint64_t sequence = 0;
        float sampleRate = 44100.0f;
        int engine = 0;
        bool triggerActive = false;
    };

    StreWetTelemetry() noexcept
    {
        static_assert((ringCapacity & (ringCapacity - 1)) == 0,
                      "Telemetry capacity must remain a power of two");
        static_assert(std::atomic<float>::is_always_lock_free,
                      "STRE wet telemetry requires lock-free float atomics");
        for (auto& sample : ring) sample.store(0.0f, std::memory_order_relaxed);
    }

    void beginCapture() noexcept
    {
        const auto previous = consumerCount.fetch_add(1, std::memory_order_acq_rel);
        if (previous == 0)
            writeSequence.store(0, std::memory_order_release);
    }

    void endCapture() noexcept
    {
        auto current = consumerCount.load(std::memory_order_acquire);
        while (current > 0
               && !consumerCount.compare_exchange_weak(current, current - 1,
                                                       std::memory_order_acq_rel,
                                                       std::memory_order_acquire))
        {
        }
    }

    bool isCaptureActive() const noexcept
    {
        return consumerCount.load(std::memory_order_relaxed) > 0;
    }

    void push(float left, float right, int currentEngine, float currentSampleRate,
              bool isTriggerActive) noexcept
    {
        if (!isCaptureActive()) return;

        const auto sequence = writeSequence.load(std::memory_order_relaxed);
        const auto mono = isTriggerActive ? 0.5f * (left + right) : 0.0f;
        ring[static_cast<std::size_t>(sequence) & (ringCapacity - 1)]
            .store(mono, std::memory_order_relaxed);
        engine.store(currentEngine, std::memory_order_relaxed);
        sampleRate.store(currentSampleRate, std::memory_order_relaxed);
        triggerActive.store(isTriggerActive, std::memory_order_relaxed);
        writeSequence.store(sequence + 1, std::memory_order_release);
    }

    Snapshot readLatest() const noexcept
    {
        Snapshot result;
        const auto end = writeSequence.load(std::memory_order_acquire);
        result.sampleCount = static_cast<std::size_t>(
            std::min<std::uint64_t>(end, snapshotCapacity));
        const auto start = end - result.sampleCount;
        for (std::size_t index = 0; index < result.sampleCount; ++index)
        {
            result.samples[index] =
                ring[static_cast<std::size_t>(start + index) & (ringCapacity - 1)]
                    .load(std::memory_order_relaxed);
        }
        result.sequence = end;
        result.sampleRate = sampleRate.load(std::memory_order_relaxed);
        result.engine = engine.load(std::memory_order_relaxed);
        result.triggerActive = triggerActive.load(std::memory_order_relaxed);
        return result;
    }

private:
    std::array<std::atomic<float>, ringCapacity> ring;
    std::atomic<std::uint64_t> writeSequence { 0 };
    std::atomic<int> consumerCount { 0 };
    std::atomic<float> sampleRate { 44100.0f };
    std::atomic<int> engine { 0 };
    std::atomic<bool> triggerActive { false };
};
}
