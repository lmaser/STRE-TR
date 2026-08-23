#pragma once

#include "../PluginProcessor.h"
#include "../../../TR-Shared/SimpleUIV2/TRSimpleUIV2.h"
#include "../../../TR-Shared/SimpleUIV2/Runtime/SimpleJuceBackend.h"
#include "../../../TR-Shared/Modulation/UI/TRSimpleModulationWorkspace.h"

namespace TR::StreUIV2
{
class StreBackendBindings final : public SimpleUIV2::SimpleJuceBackend,
                                  public Modulation::UI::ModulationUiBackend
{
public:
    explicit StreBackendBindings(STRETRAudioProcessor& processorToUse) noexcept;
    ~StreBackendBindings() override;

    juce::AudioProcessorValueTreeState& parameters() const noexcept override;
    SimpleUIV2::ParameterSnapshot parameterSnapshot() const override;
    void updateParameterSnapshot(SimpleUIV2::ParameterSnapshot& destination) const override;
    void prepareForUiRefresh() override;
    const SimpleUIV2::SignatureAudioSnapshot* signatureAudioSnapshot() const noexcept override;
    std::optional<SimpleUIV2::ControlValuePolicy> controlValuePolicy(
        std::string_view controlId, std::string_view parameterId) const override;
    std::optional<juce::String> formatControlValue(std::string_view controlId,
                                                   double value) const override;
    std::optional<juce::String> formatControlValue(std::string_view controlId,
                                                   double value,
                                                   bool userIsInteracting) const override;
    std::optional<double> parseControlValue(std::string_view controlId,
                                            const juce::String& text) const override;

    float inputMeterPeak() const noexcept override;
    float outputMeterPeak() const noexcept override;
    SimpleUIV2::MusicalState readMusicalState() const override;
    SimpleUIV2::MusicalState defaultMusicalState() const override;
    bool validateMusicalState(const SimpleUIV2::MusicalState& state) const noexcept override;
    void writeMusicalState(const SimpleUIV2::MusicalState& state) override;
    SimpleUIV2::UiInstanceState readUiInstanceState() const override;
    void writeUiInstanceState(const SimpleUIV2::UiInstanceState& state) override;
    void setMacroName(int index, const juce::String& name) override;
    Modulation::State modulationState() const override;
    std::uint64_t modulationStateGeneration() const noexcept override;
    std::array<float, Modulation::macroCount> modulationMacroValues() const noexcept override;
    void setModulationMacroValue(int macro, float value) override;
    bool setModulationState(const Modulation::State&) override;
    Modulation::UI::SourceCapabilities modulationSourceCapabilities() const noexcept override;
    std::vector<Modulation::UI::MotionRecipeOption> modulationRecipeOptions() const override;
    bool installModulationRecipe(const juce::String&, int) override;
    Modulation::Runtime::TelemetrySnapshot modulationTelemetry() const noexcept override;
    Modulation::UI::SidechainWorkspaceCallbacks sidechainWorkspaceCallbacks() override;

private:
    STRETRAudioProcessor& processor;
    int preparedEngine = -1;
    int preparedWindow = -1;
    int preparedMaxWindow = -1;
    STRETRAudioProcessor::WetTelemetrySnapshot wetTelemetrySnapshot;
    SimpleUIV2::SignatureAudioSnapshot signatureAudio;
};
}
