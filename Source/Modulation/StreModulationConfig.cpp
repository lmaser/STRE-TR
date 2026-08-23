#include "StreModulationConfig.h"

#include "../PluginProcessor.h"
#include "../../../TR-Shared/Modulation/Recipes/TROrganicMotionRecipe.h"
#include "../../../TR-Shared/Modulation/Recipes/TRMotionRecipeUtilities.h"

namespace TR::StreModulation
{
const std::vector<Modulation::Integration::ParameterDestination>& destinations()
{
    static const std::vector<Modulation::Integration::ParameterDestination> result {
        { "core:amount", "CORE", "AMOUNT", STRETRAudioProcessor::kParamAmount,
          STRETRAudioProcessor::kAmountMin, STRETRAudioProcessor::kAmountMax,
          false, 0.01f },
        { "core:pitch", "CORE", "PITCH", STRETRAudioProcessor::kParamPitch,
          STRETRAudioProcessor::kPitchMin, STRETRAudioProcessor::kPitchMax,
          false, 0.01f },
        { "grain:size", "GRAIN", "GRAIN SIZE", STRETRAudioProcessor::kParamGrain,
          STRETRAudioProcessor::kGrainMin, STRETRAudioProcessor::kGrainMax,
          true, 0.02f },
        { "motion:jitter", "MOTION", "JITTER", STRETRAudioProcessor::kParamJitter,
          STRETRAudioProcessor::kJitterMin, STRETRAudioProcessor::kJitterMax,
          false, 0.02f },
        { "core:mix", "CORE", "MIX", STRETRAudioProcessor::kParamMix,
          STRETRAudioProcessor::kMixMin, STRETRAudioProcessor::kMixMax,
          false, 0.01f },
        { "sidechain:amount-offset", "SIDECHAIN", "AMOUNT OFFSET", "",
          -1.0f, 1.0f, false, 0.0f,
          Modulation::Runtime::ModulationDomain::blockControl,
          Modulation::Runtime::SignalRepresentation::control, true, 0.0f },
        { "motion:jitter-depth", "MOTION", "JITTER DEPTH", "", 0.0f, 1.0f, false, 0.0f,
          Modulation::Runtime::ModulationDomain::sampleControl,
          Modulation::Runtime::SignalRepresentation::control, true, 0.0f },
        { "motion:jitter-window-l", "MOTION", "WINDOW L", "", -1.0f, 1.0f, false, 0.0f,
          Modulation::Runtime::ModulationDomain::sampleControl,
          Modulation::Runtime::SignalRepresentation::control, true, 0.0f, 0 },
        { "motion:jitter-window-r", "MOTION", "WINDOW R", "", -1.0f, 1.0f, false, 0.0f,
          Modulation::Runtime::ModulationDomain::sampleControl,
          Modulation::Runtime::SignalRepresentation::control, true, 0.0f, 1 },
        { "motion:jitter-anchor-l", "MOTION", "ANCHOR L", "", -1.0f, 1.0f, false, 0.0f,
          Modulation::Runtime::ModulationDomain::sampleControl,
          Modulation::Runtime::SignalRepresentation::control, true, 0.0f, 0 },
        { "motion:jitter-anchor-r", "MOTION", "ANCHOR R", "", -1.0f, 1.0f, false, 0.0f,
          Modulation::Runtime::ModulationDomain::sampleControl,
          Modulation::Runtime::SignalRepresentation::control, true, 0.0f, 1 },
        { "motion:jitter-pitch-l", "MOTION", "PITCH L", "", -1.0f, 1.0f, false, 0.0f,
          Modulation::Runtime::ModulationDomain::sampleControl,
          Modulation::Runtime::SignalRepresentation::control, true, 0.0f, 0 },
        { "motion:jitter-pitch-r", "MOTION", "PITCH R", "", -1.0f, 1.0f, false, 0.0f,
          Modulation::Runtime::ModulationDomain::sampleControl,
          Modulation::Runtime::SignalRepresentation::control, true, 0.0f, 1 },
        { "motion:jitter-rapid-l", "MOTION", "RAPID L", "", -1.0f, 1.0f, false, 0.0f,
          Modulation::Runtime::ModulationDomain::sampleControl,
          Modulation::Runtime::SignalRepresentation::control, true, 0.0f, 0 },
        { "motion:jitter-rapid-r", "MOTION", "RAPID R", "", -1.0f, 1.0f, false, 0.0f,
          Modulation::Runtime::ModulationDomain::sampleControl,
          Modulation::Runtime::SignalRepresentation::control, true, 0.0f, 1 }
    };
    return result;
}

Modulation::State makeJitterParityRecipe(Modulation::State state, int macroOneBased)
{
    using namespace Modulation::Recipes;
    macroOneBased = juce::jlimit(1, Modulation::macroCount, macroOneBased);
    removeRoutesTo(state, { "motion:jitter-depth", "motion:jitter-window-l",
                            "motion:jitter-window-r", "motion:jitter-anchor-l",
                            "motion:jitter-anchor-r", "motion:jitter-pitch-l",
                            "motion:jitter-pitch-r", "motion:jitter-rapid-l",
                            "motion:jitter-rapid-r" });
    state.macros[static_cast<std::size_t>(macroOneBased - 1)].name = "JITTER DEPTH";
    constexpr std::uint64_t baseSeed = 0x5354524a49543031ull;
    OrganicRandomSourceConfig window { baseSeed + 0x17ull, 0.061f, 0.097f };
    window.fastRateMultiplier = 0.79f;
    window.maximumFastRateHz = 32.0f;
    configureOrganicRandomSource(state, 2, macroOneBased, window);
    auto anchor = window;
    anchor.seed = baseSeed + 0x31ull;
    anchor.driftRateAHz = 0.083f;
    anchor.driftRateBHz = 0.149f;
    anchor.fastRateMultiplier = 1.13f;
    configureOrganicRandomSource(state, 3, macroOneBased, anchor);
    auto pitch = window;
    pitch.seed = baseSeed + 0x4dull;
    pitch.driftRateAHz = 0.113f;
    pitch.driftRateBHz = 0.181f;
    pitch.fastRateMultiplier = 1.37f;
    configureOrganicRandomSource(state, 4, macroOneBased, pitch);
    auto rapid = window;
    rapid.seed = baseSeed + 0x6bull;
    rapid.driftRateAHz = 0.293f;
    rapid.driftRateBHz = 0.557f;
    rapid.fastRateMultiplier = 12.80f;
    rapid.fastRateExtraHz = 44.0f;
    rapid.maximumFastRateHz = 220.0f;
    rapid.maximumBlend = 0.80f;
    rapid.blendLaw = Modulation::OrganicBlendLaw::layeredControl;
    configureOrganicRandomSource(state, 5, macroOneBased, rapid);
    appendMacroDepthRoute(state, macroOneBased, "motion:jitter-depth");
    appendOrganicRoute(state, 2, "motion:jitter-window-l");
    appendOrganicRoute(state, 2, "motion:jitter-window-r");
    appendOrganicRoute(state, 3, "motion:jitter-anchor-l");
    appendOrganicRoute(state, 3, "motion:jitter-anchor-r");
    appendOrganicRoute(state, 4, "motion:jitter-pitch-l");
    appendOrganicRoute(state, 4, "motion:jitter-pitch-r");
    appendOrganicRoute(state, 5, "motion:jitter-rapid-l");
    appendOrganicRoute(state, 5, "motion:jitter-rapid-r");
    return state;
}
}
