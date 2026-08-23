#include "StreBackendBindings.h"
#include "StreUiDefinition.h"
#include "../Modulation/StreModulationConfig.h"
#include "../../../TR-Shared/Modulation/Integration/TRModulationPresetCodec.h"

#include <cmath>

namespace TR::StreUIV2
{
namespace
{
constexpr const char* triggerDelayKey = "triggerDelayMs";
constexpr const char* stretchWindowKey = "stretchWindow";
constexpr const char* grainWindowKey = "grainWindow";
constexpr const char* fft1WindowKey = "fft1Window";
constexpr const char* fft2WindowKey = "fft2Window";
constexpr const char* selectedTaskKey = "uiV2SelectedTask";
constexpr const char* surfaceKey = "uiV2Surface";

int rawInt(const STRETRAudioProcessor& processor, const char* id, int fallback) noexcept
{
    if (const auto* value = processor.apvts.getRawParameterValue(id))
        return juce::roundToInt(value->load(std::memory_order_relaxed));
    return fallback;
}

bool isCanonicalFftWindow(int value) noexcept
{
    for (const auto candidate : STRETRAudioProcessor::kFftWindows)
        if (candidate == value) return true;
    return false;
}

}

StreBackendBindings::StreBackendBindings(STRETRAudioProcessor& processorToUse) noexcept
    : processor(processorToUse)
{
    processor.beginWetTelemetryCapture();
}

StreBackendBindings::~StreBackendBindings()
{
    processor.endWetTelemetryCapture();
}

juce::AudioProcessorValueTreeState& StreBackendBindings::parameters() const noexcept { return processor.apvts; }

SimpleUIV2::ParameterSnapshot StreBackendBindings::parameterSnapshot() const
{
    SimpleUIV2::ParameterSnapshot values;
    updateParameterSnapshot(values);
    return values;
}

void StreBackendBindings::updateParameterSnapshot(SimpleUIV2::ParameterSnapshot& values) const
{
    if (values.empty()) values.reserve(definition().parameters.size());
    for (const auto& parameter : definition().parameters)
    {
        if (parameter.domain != SimpleUIV2::StateDomain::musicalParameter) continue;
        if (const auto* raw = processor.apvts.getRawParameterValue(juce::String(parameter.parameterId)))
            values[parameter.parameterId] = static_cast<double>(raw->load(std::memory_order_relaxed));
    }
}

void StreBackendBindings::prepareForUiRefresh()
{
    wetTelemetrySnapshot = processor.getWetTelemetrySnapshot();
    signatureAudio.sampleCount = juce::jmin(wetTelemetrySnapshot.sampleCount,
                                            signatureAudio.samples.size());
    std::copy_n(wetTelemetrySnapshot.samples.begin(), signatureAudio.sampleCount,
                signatureAudio.samples.begin());
    signatureAudio.sequence = wetTelemetrySnapshot.sequence;
    signatureAudio.sampleRate = wetTelemetrySnapshot.sampleRate;
    signatureAudio.engine = wetTelemetrySnapshot.engine;
    signatureAudio.triggerActive = wetTelemetrySnapshot.triggerActive;
    const int engine = juce::jlimit(0, 3, rawInt(processor, STRETRAudioProcessor::kParamEngine, 0));
    const int maxWindow = processor.getCurrentMaxFftWindow();
    if (preparedEngine != engine || preparedMaxWindow != maxWindow)
    {
        processor.clampFftWindowFamiliesToMax(maxWindow);
        processor.syncWindowParameterToEngine(engine);
        preparedEngine = engine;
        preparedMaxWindow = maxWindow;
        preparedWindow = processor.getStoredWindowForEngine(engine);
        return;
    }

    const int rawWindow = rawInt(processor, STRETRAudioProcessor::kParamWindow,
                                 static_cast<int>(STRETRAudioProcessor::kWindowDefault));
    if (rawWindow == preparedWindow) return;
    if (const auto policy = controlValuePolicy("macro-window", STRETRAudioProcessor::kParamWindow))
    {
        const int constrained = juce::roundToInt(SimpleUIV2::constrainControlValue(*policy, rawWindow));
        processor.setStoredWindowForEngine(engine, constrained);
        preparedWindow = rawWindow;
    }
}

const SimpleUIV2::SignatureAudioSnapshot*
StreBackendBindings::signatureAudioSnapshot() const noexcept
{
    return &signatureAudio;
}

std::optional<SimpleUIV2::ControlValuePolicy> StreBackendBindings::controlValuePolicy(
    std::string_view controlId, std::string_view parameterId) const
{
    juce::ignoreUnused(parameterId);
    if (controlId == "max-window-control")
        return SimpleUIV2::ControlValuePolicy { 64.0, 8192.0, 0.0, 2048.0,
                                                { 64, 128, 256, 512, 1024, 2048, 4096, 8192 } };
    if (controlId != "macro-window") return std::nullopt;

    const int engine = juce::jlimit(0, 3, rawInt(processor, STRETRAudioProcessor::kParamEngine, 0));
    if (engine == 0)
        return SimpleUIV2::ControlValuePolicy { 16.0, 8192.0, 1.0, 1024.0, {} };
    if (engine == 1)
        return SimpleUIV2::ControlValuePolicy { 16.0, 2048.0, 1.0, 1024.0, {} };

    const int maxWindow = processor.getCurrentMaxFftWindow();
    std::vector<double> values;
    for (const auto candidate : STRETRAudioProcessor::kFftWindows)
        if (candidate <= maxWindow) values.push_back(static_cast<double>(candidate));
    return SimpleUIV2::ControlValuePolicy { 64.0, static_cast<double>(juce::jmax(65, maxWindow)), 0.0,
                                            static_cast<double>(juce::jmin(1024, maxWindow)), std::move(values) };
}

std::optional<juce::String> StreBackendBindings::formatControlValue(std::string_view controlId,
                                                                    double value) const
{
    if (controlId == "macro-pitch")
        return juce::String((value - 0.5) * 48.0, 1) + " st";
    if (controlId == "macro-window" || controlId == "max-window-control")
        return juce::String(juce::roundToInt(value));
    return std::nullopt;
}

std::optional<juce::String> StreBackendBindings::formatControlValue(std::string_view controlId,
                                                                    double value,
                                                                    bool userIsInteracting) const
{
    if (!userIsInteracting && controlId == "macro-window")
    {
        const int engine = juce::jlimit(0, 3, rawInt(processor, STRETRAudioProcessor::kParamEngine, 0));
        return juce::String(processor.getStoredWindowForEngine(engine));
    }
    if (!userIsInteracting && controlId == "max-window-control")
        return juce::String(processor.getCurrentMaxFftWindow());
    return formatControlValue(controlId, value);
}

std::optional<double> StreBackendBindings::parseControlValue(std::string_view controlId,
                                                             const juce::String& text) const
{
    const auto number = text.retainCharacters("0123456789-+.,").replaceCharacter(',', '.');
    if (controlId == "macro-pitch")
        return juce::jlimit(0.0, 1.0, number.getDoubleValue() / 48.0 + 0.5);
    if (controlId == "macro-window" || controlId == "max-window-control")
        return number.getDoubleValue();
    return std::nullopt;
}

float StreBackendBindings::inputMeterPeak() const noexcept { return processor.getInputMeterPeak(); }
float StreBackendBindings::outputMeterPeak() const noexcept { return processor.getOutputMeterPeak(); }

SimpleUIV2::MusicalState StreBackendBindings::readMusicalState() const
{
    SimpleUIV2::MusicalState state;
    state.values.emplace(triggerDelayKey, processor.getTriggerDelayMs());
    state.values.emplace(stretchWindowKey, processor.getStoredWindowForEngine(0));
    state.values.emplace(grainWindowKey, processor.getStoredWindowForEngine(1));
    state.values.emplace(fft1WindowKey, processor.getStoredWindowForEngine(2));
    state.values.emplace(fft2WindowKey, processor.getStoredWindowForEngine(3));
    Modulation::Integration::writePresetState(state, processor.modulationState());
    return state;
}

SimpleUIV2::MusicalState StreBackendBindings::defaultMusicalState() const
{
    SimpleUIV2::MusicalState state;
    state.values = { { triggerDelayKey, 0.0 }, { stretchWindowKey, 1024.0 }, { grainWindowKey, 1024.0 },
                     { fft1WindowKey, 1024.0 }, { fft2WindowKey, 1024.0 } };
    Modulation::Integration::writePresetState(state, Modulation::makeDefaultState());
    return state;
}

bool StreBackendBindings::validateMusicalState(const SimpleUIV2::MusicalState& state) const noexcept
{
    const auto marker = state.values.find(Modulation::Integration::presetStateId);
    const bool legacyMarker = marker != state.values.end() && marker->second == 0.0;
    if (state.values.size() != static_cast<std::size_t>(legacyMarker ? 6 : 5)
        || state.textValues.size() > 1
        || (!state.textValues.empty()
            && state.textValues.find(Modulation::Integration::presetStateId)
                 == state.textValues.end()))
        return false;
    const auto trigger = state.values.find(triggerDelayKey);
    const auto stretch = state.values.find(stretchWindowKey);
    const auto grain = state.values.find(grainWindowKey);
    const auto fft1 = state.values.find(fft1WindowKey);
    const auto fft2 = state.values.find(fft2WindowKey);
    if (trigger == state.values.end() || stretch == state.values.end() || grain == state.values.end()
        || fft1 == state.values.end() || fft2 == state.values.end()) return false;
    const auto integral = [](double value) { return std::isfinite(value) && std::floor(value) == value; };
    Modulation::State modulation;
    return integral(trigger->second) && trigger->second >= 0.0 && trigger->second <= 100.0
        && integral(stretch->second) && stretch->second >= 16.0 && stretch->second <= 8192.0
        && integral(grain->second) && grain->second >= 16.0 && grain->second <= 2048.0
        && integral(fft1->second) && isCanonicalFftWindow(static_cast<int>(fft1->second))
        && integral(fft2->second) && isCanonicalFftWindow(static_cast<int>(fft2->second))
        && Modulation::Integration::readPresetState(state, modulation);
}

void StreBackendBindings::writeMusicalState(const SimpleUIV2::MusicalState& state)
{
    if (!validateMusicalState(state)) return;
    processor.setTriggerDelayMs(static_cast<int>(state.values.at(triggerDelayKey)));
    processor.setStoredWindowForEngine(0, static_cast<int>(state.values.at(stretchWindowKey)));
    processor.setStoredWindowForEngine(1, static_cast<int>(state.values.at(grainWindowKey)));
    processor.setStoredWindowForEngine(2, static_cast<int>(state.values.at(fft1WindowKey)));
    processor.setStoredWindowForEngine(3, static_cast<int>(state.values.at(fft2WindowKey)));
    processor.clampFftWindowFamiliesToMax(processor.getCurrentMaxFftWindow());
    const int engine = juce::jlimit(0, 3, rawInt(processor, STRETRAudioProcessor::kParamEngine, 0));
    processor.syncWindowParameterToEngine(engine);
    Modulation::State modulation;
    if (Modulation::Integration::readPresetState(state, modulation))
        processor.setModulationState(modulation);
    preparedEngine = engine;
    preparedMaxWindow = processor.getCurrentMaxFftWindow();
    preparedWindow = processor.getStoredWindowForEngine(engine);
}

SimpleUIV2::UiInstanceState StreBackendBindings::readUiInstanceState() const
{
    SimpleUIV2::UiInstanceState state;
    state.selectedTask = static_cast<SimpleUIV2::TaskId>(juce::jlimit(
        0, 3, static_cast<int>(processor.apvts.state.getProperty(selectedTaskKey, 0))));
    state.surface = static_cast<SimpleUIV2::UiSurface>(juce::jlimit(
        0, 2, static_cast<int>(processor.apvts.state.getProperty(surfaceKey, 0))));
    return state;
}

void StreBackendBindings::writeUiInstanceState(const SimpleUIV2::UiInstanceState& state)
{
    processor.apvts.state.setProperty(selectedTaskKey, static_cast<int>(state.selectedTask), nullptr);
    processor.apvts.state.setProperty(surfaceKey, static_cast<int>(state.surface), nullptr);
}

void StreBackendBindings::setMacroName(int index, const juce::String& name)
{
    if (index < 0 || index >= Modulation::macroCount) return;
    auto mod = processor.modulationState();
    mod.macros[static_cast<std::size_t>(index)].name = name;
    processor.setModulationState(mod);
}

Modulation::State StreBackendBindings::modulationState() const { return processor.modulationState(); }
std::uint64_t StreBackendBindings::modulationStateGeneration() const noexcept { return processor.modulationStateGeneration(); }
std::array<float, Modulation::macroCount> StreBackendBindings::modulationMacroValues() const noexcept { return processor.modulationMacroValues(); }
void StreBackendBindings::setModulationMacroValue(int macro, float value) { processor.setModulationMacroValue(macro, value); }
bool StreBackendBindings::setModulationState(const Modulation::State& state) { return processor.setModulationState(state); }
Modulation::UI::SourceCapabilities StreBackendBindings::modulationSourceCapabilities() const noexcept { return { true }; }
std::vector<Modulation::UI::MotionRecipeOption> StreBackendBindings::modulationRecipeOptions() const
{
    return { { "native-jitter", "NATIVE JITTER" } };
}
bool StreBackendBindings::installModulationRecipe(const juce::String& id, int macro)
{
    if (id != "native-jitter") return false;
    auto* parameter = processor.apvts.getParameter(STRETRAudioProcessor::kParamJitter);
    if (parameter == nullptr) return false;
    const auto nativeAmount = parameter->getValue();
    const auto candidate = StreModulation::makeJitterParityRecipe(
        processor.modulationState(), macro);
    if (!processor.setModulationState(candidate)) return false;
    processor.setModulationMacroValue(macro - 1, nativeAmount);
    parameter->beginChangeGesture();
    parameter->setValueNotifyingHost(parameter->convertTo0to1(0.0f));
    parameter->endChangeGesture();
    return true;
}
Modulation::Runtime::TelemetrySnapshot StreBackendBindings::modulationTelemetry() const noexcept { return processor.modulationTelemetry(); }
Modulation::UI::SidechainWorkspaceCallbacks StreBackendBindings::sidechainWorkspaceCallbacks()
{
    return Modulation::UI::singleSidechainCallbacks(parameters(), STRETRAudioProcessor::kParamSidechain,
        "sidechain-options", "Enable external sidechain modulation; open OPTIONS for STRE-TR detector settings");
}
}
