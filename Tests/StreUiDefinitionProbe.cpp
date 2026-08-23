#include "../Source/UIV2/StreUiDefinition.h"
#include "../Source/UIV2/StreWetTelemetry.h"

#include <chrono>
#include <iostream>
#include <set>
#include <stdexcept>

namespace V2 = TR::SimpleUIV2;

namespace
{
void require(bool value, const std::string& message)
{
    if (!value) throw std::runtime_error(message);
}

const V2::SimplePageSpec& page(const V2::SimplePluginDefinition& definition, V2::TaskId task)
{
    for (const auto& candidate : definition.pages)
        if (candidate.taskId == task) return candidate;
    throw std::runtime_error("Missing task page");
}

const V2::SimpleGroupSpec& group(const V2::SimplePageSpec& source, const std::string& id)
{
    for (const auto& candidate : source.groups)
        if (candidate.groupId == id) return candidate;
    throw std::runtime_error("Missing group: " + id);
}

void requireIds(const std::vector<V2::SimpleControlSpec>& controls,
                std::initializer_list<const char*> expected, const std::string& message)
{
    require(controls.size() == expected.size(), message);
    std::size_t index = 0;
    for (const auto* id : expected) require(controls[index++].controlId == id, message);
}

const V2::SimpleControlSpec& findControl(const std::vector<V2::SimpleControlSpec>& controls,
                                        const std::string& id)
{
    for (const auto& candidate : controls)
        if (candidate.controlId == id) return candidate;
    throw std::runtime_error("Missing control: " + id);
}

const V2::SimplePromptSpec& prompt(const V2::SimplePluginDefinition& definition,
                                   const std::string& id)
{
    for (const auto& candidate : definition.prompts)
        if (candidate.promptId == id) return candidate;
    throw std::runtime_error("Missing prompt: " + id);
}
}

int main()
{
    try
    {
        const auto& definition = TR::StreUIV2::definition();
        const auto issues = V2::validateDefinition(definition);
        if (V2::hasValidationErrors(issues))
        {
            for (const auto& issue : issues)
                std::cerr << issue.code << " at " << issue.path << " - " << issue.message << '\n';
            throw std::runtime_error("STRE definition validation failed");
        }

        std::set<std::string> apvts, musicalState, preset, presetState, retired;
        for (const auto& item : definition.parameters)
        {
            if (item.domain == V2::StateDomain::musicalParameter) apvts.insert(item.parameterId);
            if (item.domain == V2::StateDomain::musicalState) musicalState.insert(item.parameterId);
        }
        preset.insert(definition.preset.parameterWhitelist.begin(), definition.preset.parameterWhitelist.end());
        presetState.insert(definition.preset.musicalStateWhitelist.begin(), definition.preset.musicalStateWhitelist.end());
        retired.insert(TR::StreUIV2::retiredUiParameterIds().begin(), TR::StreUIV2::retiredUiParameterIds().end());

        require(apvts.size() == 59, "Expected 59 musical APVTS parameters");
        require(musicalState == std::set<std::string> { "fft1Window", "fft2Window", "grainWindow",
                                                        "modulation_v1", "stretchWindow", "triggerDelayMs" },
                "Expected modulation, trigger delay and four window-family values");
        require(preset == apvts, "Preset APVTS whitelist differs from musical definition");
        require(presetState == musicalState, "Preset musical-state whitelist differs from definition");
        require(retired.size() == 9, "Expected nine retired UI parameter IDs");
        for (const auto& id : retired)
            require(apvts.count(id) == 0 && preset.count(id) == 0, "Retired UI state leaked into presets");

        requireIds(definition.macros,
                   { "macro-amount", "macro-pitch", "macro-window", "macro-mix" },
                   "Macro order must remain AMOUNT, PITCH, WINDOW, MIX");
        require(definition.signatureModel == V2::SignatureModel::temporalMaterial,
                "STRE must use the temporal-material signature model");
        require(definition.signature.empty(),
                "STRE real-telemetry signature must not declare parameter geometry");
        const auto& windowMacro = definition.macros[2];
        require(windowMacro.enabledWhen.size() == 1
                    && windowMacro.enabledWhen.front().parameterId == "engine"
                    && windowMacro.enabledWhen.front().comparison == V2::Comparison::notEqual
                    && windowMacro.enabledWhen.front().value == 1.0
                    && windowMacro.unavailableReason.find("Grain duration") != std::string::npos,
                "WINDOW must stay visible but disabled for the Grain engine");

        TR::StreUIV2::StreWetTelemetry telemetry;
        require(!telemetry.isCaptureActive(), "Wet telemetry must be dormant without an editor");
        telemetry.push(1.0f, 1.0f, 1, 48000.0f, true);
        require(telemetry.readLatest().sampleCount == 0,
                "Dormant wet telemetry must not collect audio");
        telemetry.beginCapture();
        telemetry.push(0.25f, 0.75f, 1, 48000.0f, true);
        telemetry.push(1.0f, -1.0f, 1, 48000.0f, false);
        const auto initialWet = telemetry.readLatest();
        require(initialWet.sampleCount == 2 && initialWet.samples[0] == 0.5f
                    && initialWet.samples[1] == 0.0f,
                "Wet telemetry must publish mono engine output and silence when trigger is inactive");
        require(initialWet.engine == 1 && initialWet.sampleRate == 48000.0f
                    && !initialWet.triggerActive,
                "Wet telemetry metadata differs from the producing engine");
        for (std::size_t index = 0;
             index < TR::StreUIV2::StreWetTelemetry::ringCapacity + 17; ++index)
            telemetry.push(static_cast<float>(index), static_cast<float>(index),
                           2, 96000.0f, true);
        const auto wrappedWet = telemetry.readLatest();
        require(wrappedWet.sampleCount == TR::StreUIV2::StreWetTelemetry::snapshotCapacity,
                "Wet telemetry snapshot must remain bounded");
        const auto expectedFirst = static_cast<float>(
            TR::StreUIV2::StreWetTelemetry::ringCapacity + 17
            - TR::StreUIV2::StreWetTelemetry::snapshotCapacity);
        require(wrappedWet.samples.front() == expectedFirst
                    && wrappedWet.samples[wrappedWet.sampleCount - 1]
                        == static_cast<float>(TR::StreUIV2::StreWetTelemetry::ringCapacity + 16),
                "Wet telemetry wrap must preserve chronological order");
        telemetry.endCapture();
        require(!telemetry.isCaptureActive(), "Wet telemetry capture lifetime leaked");

        constexpr int benchmarkSamples = 4800000;
        const auto inactiveStart = std::chrono::steady_clock::now();
        for (int index = 0; index < benchmarkSamples; ++index)
            telemetry.push(0.25f, -0.25f, 0, 48000.0f, true);
        const auto inactiveEnd = std::chrono::steady_clock::now();
        telemetry.beginCapture();
        const auto activeStart = std::chrono::steady_clock::now();
        for (int index = 0; index < benchmarkSamples; ++index)
            telemetry.push(0.25f, -0.25f, 0, 48000.0f, true);
        const auto activeEnd = std::chrono::steady_clock::now();
        telemetry.endCapture();
        const auto inactiveNs = std::chrono::duration<double, std::nano>(
            inactiveEnd - inactiveStart).count() / benchmarkSamples;
        const auto activeNs = std::chrono::duration<double, std::nano>(
            activeEnd - activeStart).count() / benchmarkSamples;

        require(definition.pages.size() == 2
                    && definition.pages[0].label == "MAIN"
                    && definition.pages[1].label == "I/O",
                "STRE must expose exactly MAIN and I/O");
        const auto& main = page(definition, V2::TaskId::core);
        const auto& mainControls = group(main, "main-controls").controls;
        requireIds(mainControls, { "engine-control", "grain-control", "style-control",
                                    "reverse-control", "chaos-filter-control",
                                    "chaos-delay-control" },
                   "MAIN order changed");
        requireIds(definition.auxiliaryControls, { "sidechain-control" },
                   "SIDECHAIN must be owned by the shared MACROS workspace");
        requireIds(main.signatureActions, { "trigger-control" },
                   "MAIN signature activation order changed");
        require(findControl(mainControls, "grain-control").visibleWhen.size() == 1,
                "GRAIN must be conditional on the GRAIN engine");
        require(main.fixedActions.empty(), "STRE MAIN must not retain a parameter footer");

        const auto& io = page(definition, V2::TaskId::io);
        requireIds(io.fixedActions, { "filter-options-action", "routing-options-action",
                                      "latency-options-action" },
                   "I/O utilities must remain FILTER, ROUTING, ALIGNMENT");
        const auto& alignment = prompt(definition, "latency-options");
        requireIds(alignment.controls,
                   { "compensated-alignment-control", "align-control", "pdc-control",
                     "max-window-control" },
                   "STRE ALIGNMENT must lead with the shared mode before advanced controls");
        require(alignment.controls.front().compositePair.has_value()
                    && alignment.controls.front().choiceLabels
                           == std::vector<std::string> { "LIVE", "COMPENSATED" },
                "STRE lost the shared compensated-alignment contract");
        requireIds(group(io, "io-levels").controls, { "input-control", "output-control" },
                   "INPUT and OUTPUT must remain one consecutive LEVELS pair");
        requireIds(group(io, "io-image").controls, { "pan-control" }, "I/O image group changed");
        requireIds(group(io, "io-mix").controls, { "mix-mode-control", "dry-level-control" },
                   "I/O mix group changed");
        requireIds(group(io, "io-limiter").controls,
                   { "lim-mode-control", "lim-quality-control", "lim-threshold-control" },
                   "I/O limiter group changed");

        std::cout << "STRE UI V2 definition passed: 59 APVTS + 6 musical state, "
                     "9 retired UI IDs excluded; telemetry "
                  << inactiveNs << " ns/sample inactive, "
                  << activeNs << " ns/sample active.\n";
        return 0;
    }
    catch (const std::exception& exception)
    {
        std::cerr << "STRE UI V2 definition failed: " << exception.what() << '\n';
        return 1;
    }
}
