// Tests the actual production TelemetrySource (app/src/main/cpp/
// telemetry_source.{h,cpp}), compiled directly — not a reimplementation —
// as a standalone host binary. No test framework dependency: this is
// small and focused enough that a minimal custom runner is the honestly
// appropriate amount of infrastructure, not GoogleTest for two files.
//
// Build (also runnable under ThreadSanitizer for real race detection, not
// just the invariant checks below):
//   clang++ -std=c++20 -pthread -Wall -Wextra \
//       native_tests/telemetry_source_test.cpp \
//       app/src/main/cpp/telemetry_source.cpp \
//       -I app/src/main/cpp -o /tmp/telemetry_source_test && /tmp/telemetry_source_test
//
//   clang++ -std=c++20 -pthread -fsanitize=thread -Wall -Wextra \
//       native_tests/telemetry_source_test.cpp \
//       app/src/main/cpp/telemetry_source.cpp \
//       -I app/src/main/cpp -o /tmp/telemetry_source_test_tsan && /tmp/telemetry_source_test_tsan

#include "telemetry_source.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>
#include <vector>

namespace {

int g_tests = 0;
int g_failures = 0;

void expectTrue(bool condition, const char* description) {
    ++g_tests;
    if (condition) {
        std::printf("PASS: %s\n", description);
    } else {
        ++g_failures;
        std::printf("FAIL: %s\n", description);
    }
}

void testSetExternalOverridesValue() {
    TelemetrySource source;
    source.setExternal(42.0f, 3000.0f);
    TelemetrySample sample = source.latest();
    expectTrue(sample.speedKph == 42.0f && sample.rpm == 3000.0f,
               "setExternal() value is immediately visible via latest()");
}

void testInternalGeneratorStopsAfterExternal() {
    TelemetrySource source;
    source.start();
    // Let the internal fake generator write at least once before we
    // override it — this is the tick interval telemetry_source.cpp uses
    // (~10Hz), not a magic number.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    source.setExternal(99.0f, 5000.0f);

    // Sample repeatedly across several internal-thread tick intervals
    // (~375ms total, ~3.75x the 100ms tick). If the internal generator
    // hadn't genuinely stopped writing, at least one of these samples
    // would show a different (sine-wave) value instead of the constant
    // externally-set one.
    bool allMatched = true;
    for (int i = 0; i < 15; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
        TelemetrySample sample = source.latest();
        if (sample.speedKph != 99.0f || sample.rpm != 5000.0f) {
            allMatched = false;
            std::printf("  unexpected sample at iteration %d: speed=%f rpm=%f\n", i, sample.speedKph,
                        sample.rpm);
        }
    }
    source.stop();

    expectTrue(allMatched,
               "internal generator stops overwriting once setExternal() is called "
               "(sampled across ~3.75x the tick interval)");
}

void testNoTornReadsUnderConcurrentWriters() {
    TelemetrySource source;
    std::atomic<bool> stopFlag{false};
    std::atomic<bool> violationFound{false};

    // 4 writer threads, each hammering setExternal() with values that
    // satisfy a deterministic invariant: rpm is always exactly 10x
    // speedKph. If the mutex were broken or removed, a reader could
    // observe a torn combination — speedKph from one write interleaved
    // with rpm from a different write — which would violate that
    // invariant. This is checking correctness, not just "didn't crash."
    std::vector<std::thread> writers;
    for (int t = 0; t < 4; ++t) {
        writers.emplace_back([&source, &stopFlag, t]() {
            float counter = static_cast<float>(t) * 1000.0f;
            while (!stopFlag.load()) {
                counter += 1.0f;
                source.setExternal(counter, counter * 10.0f);
            }
        });
    }

    std::thread reader([&source, &stopFlag, &violationFound]() {
        while (!stopFlag.load()) {
            TelemetrySample sample = source.latest();
            if (sample.rpm != sample.speedKph * 10.0f) {
                violationFound.store(true);
                std::printf("  torn read detected: speed=%f rpm=%f (expected rpm=%f)\n", sample.speedKph,
                            sample.rpm, sample.speedKph * 10.0f);
            }
        }
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    stopFlag.store(true);
    for (auto& w : writers) w.join();
    reader.join();

    expectTrue(!violationFound.load(),
               "no torn reads observed under 4 concurrent writers + 1 reader for 300ms");
}

} // namespace

int main() {
    testSetExternalOverridesValue();
    testInternalGeneratorStopsAfterExternal();
    testNoTornReadsUnderConcurrentWriters();

    std::printf("\n%d/%d tests passed\n", g_tests - g_failures, g_tests);
    return g_failures == 0 ? 0 : 1;
}
