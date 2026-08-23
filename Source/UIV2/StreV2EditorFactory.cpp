#include "StreV2EditorFactory.h"
#include "StreBackendBindings.h"
#include "StreUiDefinition.h"
#include "../Modulation/StreModulationConfig.h"
#include "../../../TR-Shared/Modulation/UI/TRSimpleModulationWorkspace.h"
#include "../../../TR-Shared/SimpleUIV2/Runtime/SimpleEditorHost.h"

namespace TR::StreUIV2
{
juce::AudioProcessorEditor* createEditor(STRETRAudioProcessor& processor)
{
    std::vector<Modulation::UI::DestinationOption> destinations;
    int telemetryIndex = 0;
    for (const auto& descriptor : StreModulation::destinations())
        destinations.push_back({ descriptor.id, descriptor.group, descriptor.label,
                                 true, {}, telemetryIndex++ });
    auto backend = std::make_unique<StreBackendBindings>(processor);
    auto& modulationBackend = *backend;
    auto modulation = std::make_unique<Modulation::UI::SimpleModulationWorkspace>(
        Modulation::UI::workspaceCallbacks(modulationBackend), std::move(destinations),
        modulationBackend.sidechainWorkspaceCallbacks());
    return new SimpleUIV2::SimpleEditorHost(
        processor, definition(), std::move(backend),
        std::move(modulation));
}
}
