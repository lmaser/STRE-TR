#include "../Source/PluginProcessor.h"
#include "../Source/UIV2/StreBackendBindings.h"
#include "../Source/UIV2/StreUiDefinition.h"
#include "../../TR-Shared/Modulation/Tests/TRNativeSidechainBaseline.h"
#include "../../TR-Shared/Modulation/Tests/TRModulationJourneyAssertions.h"
#include "../../TR-Shared/Modulation/Tests/TRDualSineSmoothRandomAssertions.h"
#include "../../TR-Shared/Modulation/Tests/TRJitterMotionEvidence.h"
#include "../../TR-Shared/Modulation/Tests/TRMotionRecipeUiAssertions.h"
#include "../../TR-Shared/Testing/TRPluginCpuBenchmark.h"
#include "../../TR-Shared/SimpleUIV2/Preset/TRPresetManager.h"

#include <array>
#include <cmath>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>

struct StreNativeSidechainTestAccess
{
    static void selectNative(STRETRAudioProcessor& processor)
    {
        processor.useNativeSidechainForTests_ = true;
    }
    static bool enableShared(STRETRAudioProcessor& processor)
    {
        return TR::Modulation::Tests::setNativeBaselineParameter(
            processor.apvts, STRETRAudioProcessor::kParamSidechain, 1.0f);
    }
    static void extract(const STRETRAudioProcessor& processor, float* values)
    {
        values[0] = processor.sidechainRmsEnv_;
        values[1] = processor.sidechainGateSmoothed_;
        values[2] = processor.sidechainDepthSmoothed_;
        values[3] = processor.sidechainGateSmoothed_ * processor.sidechainDepthSmoothed_ * 100.0f;
    }
    static void extractShared(const STRETRAudioProcessor& processor, int sample, float* values)
    {
        const auto control = processor.modulation.analysisControlSignal(1);
        values[0] = control.valid() ? control.samples[sample] : 0.0f;
        values[1] = values[0] * 100.0f;
    }

    static std::array<float, 9> jitterSnapshot(const STRETRAudioProcessor& processor) noexcept
    {
        return { processor.jitterSmoothed_,
                 processor.jitterWindowOut_[0], processor.jitterWindowOut_[1],
                 processor.jitterAnchorOut_[0], processor.jitterAnchorOut_[1],
                 processor.jitterPitchOut_[0], processor.jitterPitchOut_[1],
                 processor.jitterRapidOut_[0], processor.jitterRapidOut_[1] };
    }
};

namespace
{
void require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

juce::Component* findById(juce::Component& parent, const juce::String& id)
{
    if (parent.getComponentID() == id) return &parent;
    for (auto* child : parent.getChildren())
        if (auto* found = findById(*child, id)) return found;
    return nullptr;
}

void process(STRETRAudioProcessor& processor, bool noteOn, float sidechain = 0.0f)
{
    constexpr int blockSize = 512;
    juce::AudioBuffer<float> audio(processor.getTotalNumInputChannels(), blockSize);
    for (int sample = 0; sample < blockSize; ++sample)
    {
        const auto value = 0.1f * std::sin(0.01f * static_cast<float>(sample));
        audio.setSample(0, sample, value);
        audio.setSample(1, sample, value);
        if (audio.getNumChannels() >= 4)
        {
            const auto sc = sidechain * std::sin(0.13f * static_cast<float>(sample));
            audio.setSample(2, sample, sc);
            audio.setSample(3, sample, sc);
        }
    }
    juce::MidiBuffer midi;
    if (noteOn)
        midi.addEvent(juce::MidiMessage::noteOn(1, 127, static_cast<juce::uint8>(127)), 16);
    processor.processBlock(audio, midi);
    for (int channel = 0; channel < audio.getNumChannels(); ++channel)
        for (int sample = 0; sample < blockSize; ++sample)
            require(std::isfinite(audio.getSample(channel, sample)), "STRE produced non-finite audio");
}

std::vector<float> renderJitterControls(int path, double sampleRate, int blockSize,
                                        int engine, bool automate)
{
    auto processor = std::make_unique<STRETRAudioProcessor>();
    using TR::Modulation::Tests::setNativeBaselineParameter;
    require(setNativeBaselineParameter(processor->apvts, STRETRAudioProcessor::kParamEngine,
                                        static_cast<float>(engine))
                && setNativeBaselineParameter(processor->apvts, STRETRAudioProcessor::kParamJitter, 0.0f)
                && setNativeBaselineParameter(processor->apvts, STRETRAudioProcessor::kParamMix, 1.0f),
            "STRE parity parameters rejected");
    if (path == 2)
    {
        const auto recipe = TR::StreModulation::makeJitterParityRecipe(
            TR::Modulation::makeDefaultState());
        require(setNativeBaselineParameter(processor->apvts, "mod_macro_1", 0.0f)
                    && processor->setModulationState(recipe),
                "STRE parity recipe rejected");
    }
    processor->prepareToPlay(sampleRate, blockSize);
    const auto totalSamples = static_cast<int>(sampleRate * (automate ? 8.0 : 4.0));
    std::vector<float> result;
    result.reserve(static_cast<std::size_t>((totalSamples / blockSize + 1) * 9));
    for (int offset = 0; offset < totalSamples; offset += blockSize)
    {
        const auto count = juce::jmin(blockSize, totalSamples - offset);
        const float amount = automate
            ? std::array<float, 4> { 0.2f, 0.6f, 1.0f, 0.35f }
                [static_cast<std::size_t>((offset / juce::jmax(1, static_cast<int>(sampleRate * 0.5))) & 3)]
            : 0.6f;
        require(setNativeBaselineParameter(processor->apvts,
                    path == 1 ? STRETRAudioProcessor::kParamJitter : "mod_macro_1",
                    path == 1 ? amount * 100.0f : amount),
                "STRE parity automation rejected");
        juce::AudioBuffer<float> block(2, count);
        for (int sample = 0; sample < count; ++sample)
        {
            const auto phase = static_cast<float>((offset + sample) * 0.017);
            block.setSample(0, sample, 0.1f * std::sin(phase));
            block.setSample(1, sample, 0.08f * std::sin(phase * 1.013f));
        }
        juce::MidiBuffer midi;
        processor->processBlock(block, midi);
        const auto snapshot = StreNativeSidechainTestAccess::jitterSnapshot(*processor);
        result.insert(result.end(), snapshot.begin(), snapshot.end());
    }
    return result;
}

bool writeJitterParityMatrix(const juce::File& output, bool automate)
{
    std::ofstream csv(output.getFullPathName().toStdString(), std::ios::trunc);
    csv << "sample_rate_hz,block_size,engine,rms_ratio,correlation,rms_error,max_window_rms_ratio_error,passed\n";
    bool passed = true;
    for (const auto sampleRate : { 44100.0, 48000.0, 96000.0, 192000.0 })
        for (const auto blockSize : { 64, 257, 2048 })
            for (int engine = 0; engine < 4; ++engine)
            {
                const auto native = renderJitterControls(1, sampleRate, blockSize, engine, automate);
                const auto matrix = renderJitterControls(2, sampleRate, blockSize, engine, automate);
                const auto skip = static_cast<std::size_t>(juce::jmax(9,
                    static_cast<int>(sampleRate / blockSize) * 9));
                const auto ratio = TR::Modulation::Tests::rmsRatio(native, matrix, skip);
                const auto correlation = TR::Modulation::Tests::correlation(native, matrix, skip);
                const auto error = TR::Modulation::Tests::rmsDifference(native, matrix, skip);
                const auto window = static_cast<std::size_t>(juce::jmax(18,
                    static_cast<int>(sampleRate * 0.25 / blockSize) * 9));
                const auto windowError = TR::Modulation::Tests::maximumWindowedRmsRatioError(
                    native, matrix, skip, window);
                const auto rowPassed = ratio >= 0.97 && ratio <= 1.03
                    && (automate ? windowError <= 0.15
                                 : correlation >= 0.995 && error <= 0.01);
                passed = passed && rowPassed;
                csv << sampleRate << ',' << blockSize << ',' << engine << ',' << ratio << ','
                    << correlation << ',' << error << ',' << windowError << ',' << rowPassed << '\n';
            }
    return csv.good() && passed;
}

bool writeJitterPresetEvidence(const juce::File& output)
{
    require(output.createDirectory(), "STRE Jitter evidence directory unavailable");
    auto processor = std::make_unique<STRETRAudioProcessor>();
    const auto state = TR::StreModulation::makeJitterParityRecipe(
        TR::Modulation::makeDefaultState());
    using TR::Modulation::Tests::setNativeBaselineParameter;
    require(setNativeBaselineParameter(processor->apvts, STRETRAudioProcessor::kParamJitter, 0.0f)
                && setNativeBaselineParameter(processor->apvts, "mod_macro_1", 1.0f)
                && processor->setModulationState(state),
            "STRE Jitter preset state rejected");
    const auto staging = output.getChildFile("preset-staging");
    TR::StreUIV2::StreBackendBindings backend(*processor);
    TR::SimpleUIV2::TRPresetManager manager(TR::StreUIV2::definition(), backend, staging);
    constexpr const char* name = "STRE Jitter MATRIX 100";
    require(manager.saveAs(name, true).wasOk(), "STRE Jitter preset save failed");
    const auto saved = manager.libraryFolder().getChildFile(juce::String(name) + ".trpreset");
    const auto evidence = output.getChildFile(saved.getFileName());
    require(saved.existsAsFile() && saved.copyFileTo(evidence), "STRE Jitter preset copy failed");
    auto restored = std::make_unique<STRETRAudioProcessor>();
    TR::StreUIV2::StreBackendBindings restoredBackend(*restored);
    TR::SimpleUIV2::TRPresetManager restoredManager(
        TR::StreUIV2::definition(), restoredBackend, staging);
    require(restoredManager.load(name).wasOk() && restored->modulationState() == state
                && std::abs(restored->apvts.getRawParameterValue(
                    STRETRAudioProcessor::kParamJitter)->load()) <= 1.0e-7f
                && std::abs(restored->apvts.getRawParameterValue("mod_macro_1")->load()
                            - 1.0f) <= 1.0e-7f,
            "STRE Jitter preset round-trip failed");
    std::ofstream proof(output.getChildFile("preset-verification.csv")
                            .getFullPathName().toStdString(), std::ios::trunc);
    proof << "preset,native_jitter,macro_1,route_count,round_trip\n"
          << name << ",0,1," << state.routes.size() << ",1\n";
    return proof.good();
}
}

int main(int argc, char** argv)
{
    try
    {
        juce::ScopedJuceInitialiser_GUI juceInitialiser;
        std::cerr << "STRE dual-sine assertion start\n";
        TR::Modulation::Tests::assertDualSineSmoothRandomExtraction();
        std::cerr << "STRE dual-sine assertion passed\n";
        if (argc == 3 && juce::String(argv[1]) == "--qualify-jitter-host-matrix")
            return writeJitterParityMatrix(juce::File(argv[2]), false) ? 0 : 3;
        if (argc == 3 && juce::String(argv[1]) == "--qualify-jitter-automation")
            return writeJitterParityMatrix(juce::File(argv[2]), true) ? 0 : 4;
        if (argc == 3 && juce::String(argv[1]) == "--export-jitter-motion-evidence")
            return writeJitterPresetEvidence(juce::File(argv[2])) ? 0 : 2;
        if (argc == 3 && juce::String(argv[1]) == "--export-native-sidechain-baseline")
        {
            const auto ok = TR::Modulation::Tests::exportNativeSidechainBaseline<STRETRAudioProcessor>(
                juce::File(argv[2]), "STRE-TR", "rms,gate,depth,amount_offset", 4,
                [](auto& processor) -> auto& { return processor.apvts; },
                [](auto& processor, auto& state)
                {
                    StreNativeSidechainTestAccess::selectNative(processor);
                    using namespace TR::Modulation::Tests;
                    return setNativeBaselineParameter(state, STRETRAudioProcessor::kParamSidechain, 1.0f)
                        && setNativeBaselineParameter(state, STRETRAudioProcessor::kParamSidechainGain, 0.0f)
                        && setNativeBaselineParameter(state, STRETRAudioProcessor::kParamSidechainSmooth, 0.5f)
                        && setNativeBaselineParameter(state, STRETRAudioProcessor::kParamSidechainPol, 1.0f);
                },
                [](const auto& processor, int, int, float* values)
                { StreNativeSidechainTestAccess::extract(processor, values); });
            return ok ? 0 : 2;
        }
        if (argc == 3 && juce::String(argv[1]) == "--export-shared-sidechain-baseline")
        {
            const auto ok = TR::Modulation::Tests::exportNativeSidechainBaseline<STRETRAudioProcessor>(
                juce::File(argv[2]), "STRE-TR", "control,amount_offset", 2,
                [](auto& processor) -> auto& { return processor.apvts; },
                [](auto&, auto& state)
                {
                    using namespace TR::Modulation::Tests;
                    return setNativeBaselineParameter(state, STRETRAudioProcessor::kParamSidechain, 1.0f)
                        && setNativeBaselineParameter(state, STRETRAudioProcessor::kParamSidechainGain, 0.0f)
                        && setNativeBaselineParameter(state, STRETRAudioProcessor::kParamSidechainSmooth, 0.5f)
                        && setNativeBaselineParameter(state, STRETRAudioProcessor::kParamSidechainPol, 1.0f);
                },
                [](const auto& processor, int sample, int, float* values)
                { StreNativeSidechainTestAccess::extractShared(processor, sample, values); });
            return ok ? 0 : 2;
        }
        {
            auto auditProcessor = std::make_unique<STRETRAudioProcessor>();
            TR::StreUIV2::StreBackendBindings auditBackend(*auditProcessor);
            require(TR::Modulation::Tests::auditMotionRecipeBackend(
                        auditBackend, auditProcessor->apvts,
                        STRETRAudioProcessor::kParamJitter, "native-jitter", 3, 9, 1).passed(),
                    "STRE Jitter recipe UI/backend contract failed");
        }
        auto processor = std::make_unique<STRETRAudioProcessor>();
        require(processor->acceptsMidi(), "STRE does not advertise MIDI input");
        auto layout = processor->getBusesLayout();
        layout.inputBuses.set(1, juce::AudioChannelSet::stereo());
        require(processor->setBusesLayout(layout), "STRE shared sidechain layout rejected");
        processor->prepareToPlay(48000.0, 512);

        auto state = TR::Modulation::makeDefaultState();
        TR::Modulation::setLinkedSmooth(state.analysisSources[1].detector, 0.0f);
        TR::Modulation::setLinkedSmooth(state.analysisSources[2].detector, 0.0f);
        TR::Modulation::setLinkedSmooth(state.analysisSources[3].detector, 0.0f);
        state.midiSources[static_cast<std::size_t>(TR::Modulation::MidiSourceType::note)]
            .smoothingSeconds = 0.0f;
        require(TR::Modulation::appendRoute(state, TR::Modulation::Route {
            0, 0, true, TR::Modulation::SourceId::midi(TR::Modulation::MidiSourceType::note),
            TR::Modulation::Polarity::unipolar, 1.0f, "macro:1",
            TR::Modulation::SourceId::none(), TR::Modulation::Polarity::unipolar,
            TR::Modulation::makeLinearCurve(), TR::Modulation::makeLinearCurve() }),
            "STRE MIDI -> Macro route rejected");
        require(TR::Modulation::appendRoute(state, TR::Modulation::Route {
            0, 0, true, TR::Modulation::SourceId::macro(1),
            TR::Modulation::Polarity::unipolar, 1.0f, "core:amount",
            TR::Modulation::SourceId::none(), TR::Modulation::Polarity::unipolar,
            TR::Modulation::makeLinearCurve(), TR::Modulation::makeLinearCurve() }),
            "STRE Macro -> Amount route rejected");
        for (int source = 1; source <= 3; ++source)
            require(TR::Modulation::appendRoute(state, TR::Modulation::Route {
                0, 0, true,
                source == 1 ? TR::Modulation::SourceId::sidechainEnvelope()
                            : TR::Modulation::SourceId::sidechainAnalysis(source),
                TR::Modulation::Polarity::unipolar, 1.0f, "core:pitch",
                TR::Modulation::SourceId::none(), TR::Modulation::Polarity::unipolar,
                TR::Modulation::makeLinearCurve(), TR::Modulation::makeLinearCurve() }),
                "STRE shared Sidechain source route rejected");
        require(processor->setModulationState(state), "STRE modulation state rejected");

        TR::StreUIV2::StreBackendBindings presetBackend(*processor);
        const auto presetMusicalState = presetBackend.readMusicalState();
        require(presetBackend.validateMusicalState(presetMusicalState)
                    && presetMusicalState.textValues.count(
                           TR::Modulation::Integration::presetStateId) == 1,
                "STRE internal preset state omitted modulation XML");
        require(presetBackend.parameterSnapshot().count("mod_macro_1") == 1,
                "STRE internal preset state omitted Macro parameters");

        std::unique_ptr<juce::AudioProcessorEditor> editor(processor->createEditor());
        editor->addToDesktop(juce::ComponentPeer::windowIsTemporary);
        editor->setVisible(true);
        juce::Timer::callPendingTimersSynchronously();
        auto* macrosButton = dynamic_cast<juce::Button*>(findById(*editor, "macros-panel-button"));
        auto* matrixButton = dynamic_cast<juce::Button*>(findById(*editor, "matrix-workspace-button"));
        auto* workspace = findById(*editor, "auxiliary-workspace");
        require(macrosButton != nullptr && matrixButton != nullptr
                    && workspace != nullptr && !workspace->isVisible(),
                "STRE MACROS/MATRIX controls are missing");
        const auto productSize = juce::Point<int> { editor->getWidth(), editor->getHeight() };
        TR::Modulation::Tests::clickButton(*macrosButton);
        auto* compactPanel = findById(*editor, "macro-panel");
        require(compactPanel != nullptr && compactPanel->isShowing()
                    && !workspace->isVisible()
                    && editor->getWidth() == productSize.x + 200
                    && editor->getHeight() == productSize.y,
                "First MACROS click did not open the compact Macro panel");
        TR::Modulation::Tests::clickButton(*matrixButton);
        require(workspace->isVisible() && matrixButton->getToggleState(),
                "STRE MATRIX workspace did not open");
        require(editor->getWidth() == 1040 && editor->getHeight() == 680,
                "STRE MATRIX workspace did not request its canonical size");
        const auto journey = TR::Modulation::Tests::auditMacroJourney(workspace);
        require(journey.workspaceFound && journey.visible && journey.hasAllMacroCards
                    && journey.hasFocusTargets && journey.containerHasNoFocusRing
                    && journey.nameEditingContract,
                "STRE MATRIX journey has complete cards and control-local focus");
        TR::Modulation::Tests::clickButton(*matrixButton);
        require(compactPanel->isShowing()
                    && editor->getWidth() == productSize.x + 200
                    && editor->getHeight() == productSize.y,
                "STRE MATRIX did not restore the originating MACROS panel");

        process(*processor, true);
        for (int block = 0; block < 32; ++block) process(*processor, false);
        float base = 0.0f, effective = 0.0f;
        require(processor->modulationDestinationValues("core:amount", base, effective),
                "STRE destination telemetry unavailable");
        require(processor->modulationTelemetry().destinationCount > 0,
                "STRE workspace telemetry snapshot is empty");
        require(effective > base + 20.0f, "STRE MIDI Macro route did not reach DSP destination");
        require(StreNativeSidechainTestAccess::enableShared(*processor),
                "STRE legacy Sidechain adapter could not be enabled");
        for (int block = 0; block < 32; ++block) process(*processor, false, 0.5f);
        const auto sidechainTelemetry = processor->modulationTelemetry();
        require(sidechainTelemetry.sources[1].signalState
                    == TR::Modulation::Runtime::SourceSignalState::active
                    && sidechainTelemetry.sources[1].value > 0.45f,
                "STRE legacy controls did not drive the shared RMS profile");
        for (int source = 2; source <= 3; ++source)
        {
            const auto telemetry = processor->modulationTelemetry();
            require(telemetry.sources[source].available
                        && telemetry.sources[source].signalState
                            == TR::Modulation::Runtime::SourceSignalState::active,
                    "STRE shared Sidechain source did not become active");
        }
        require(TR::Modulation::Tests::setNativeBaselineParameter(
                    processor->apvts, STRETRAudioProcessor::kParamSidechain, 0.0f),
                "STRE legacy Sidechain could not be disabled");
        auto matrixSidechain = TR::Modulation::makeDefaultState();
        matrixSidechain.analysisSources[1].feature = TR::Modulation::AnalysisFeature::motionRmsEnvelope;
        matrixSidechain.analysisSources[1].detector.smooth = STRETRAudioProcessor::kSidechainSmoothDefault;
        require(TR::Modulation::appendRoute(matrixSidechain, { 0, 0, true,
            TR::Modulation::SourceId::sidechainEnvelope(), TR::Modulation::Polarity::unipolar,
            1.0f, "sidechain:amount-offset", TR::Modulation::SourceId::none(),
            TR::Modulation::Polarity::unipolar, TR::Modulation::makeLinearCurve(),
            TR::Modulation::makeLinearCurve() }) && processor->setModulationState(matrixSidechain),
            "STRE explicit MATRIX Sidechain route was rejected");
        for (int block = 0; block < 8; ++block) process(*processor, false, 0.5f);
        require(processor->modulationDestinationValues(
                    "sidechain:amount-offset", base, effective)
                    && base == 0.0f && effective > 0.05f,
                "STRE MATRIX-only Sidechain did not reach the +/-100 Amount law");
        require(processor->setModulationState(state),
                "STRE could not restore its main smoke state after MATRIX Sidechain proof");
        auto jitterRecipe = TR::StreModulation::makeJitterParityRecipe(
            TR::Modulation::makeDefaultState());
        require(jitterRecipe.routes.size() == 9,
                "STRE Jitter parity recipe topology is incomplete");
        require(TR::Modulation::Tests::setNativeBaselineParameter(
                    processor->apvts, STRETRAudioProcessor::kParamJitter, 0.0f)
                    && TR::Modulation::Tests::setNativeBaselineParameter(
                        processor->apvts, "mod_macro_1", 1.0f)
                    && processor->setModulationState(jitterRecipe),
                "STRE Jitter parity recipe was rejected");
        for (int block = 0; block < 32; ++block) process(*processor, false);
        require(processor->modulationDestinationValues(
                    "motion:jitter-depth", base, effective)
                    && base == 0.0f && effective > 0.95f,
                "STRE Jitter parity depth did not reach the internal adapter");
        require(processor->modulationDestinationValues(
                    "motion:jitter-pitch-l", base, effective)
                    && std::isfinite(effective) && std::abs(effective) > 1.0e-5f,
                "STRE Organic Random source did not reach the pitch adapter");
        const auto encodedJitterRecipe = TR::Modulation::encodeState(jitterRecipe);
        require(encodedJitterRecipe.has_value(),
                "STRE schema-11 Jitter recipe could not be encoded");
        const auto decodedJitterRecipe = TR::Modulation::decodeState(*encodedJitterRecipe);
        require(decodedJitterRecipe.ok && decodedJitterRecipe.state == jitterRecipe,
                "STRE schema-11 Jitter recipe did not round-trip exactly");
        require(processor->setModulationState(state),
                "STRE could not restore its main smoke state after Jitter proof");
        require(TR::Testing::writePluginCpuComparison (std::cout, "STRE", *processor),
                "STRE CPU comparison could not restore modulation state");

        juce::MemoryBlock preset;
        processor->getStateInformation(preset);
        editor.reset();
        auto restored = std::make_unique<STRETRAudioProcessor>();
        restored->setStateInformation(preset.getData(), static_cast<int>(preset.getSize()));
        require(restored->modulationState().routes.size() == 5,
                "STRE modulation routes did not survive preset round-trip");
        std::cout << "STRE modulation smoke probe passed\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "STRE modulation smoke probe failed: " << error.what() << '\n';
        return 1;
    }
}
