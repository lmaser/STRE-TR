#pragma once

#include "../../../TR-Shared/Modulation/Integration/TRParameterModulationBridge.h"

#include <vector>

namespace TR::StreModulation
{
enum Destination : int
{
    amount = 0,
    pitch,
    grain,
    jitter,
    mix,
    sidechainAmountOffset,
    jitterDepth,
    jitterWindowL,
    jitterWindowR,
    jitterAnchorL,
    jitterAnchorR,
    jitterPitchL,
    jitterPitchR,
    jitterRapidL,
    jitterRapidR,
    destinationCount
};

const std::vector<Modulation::Integration::ParameterDestination>& destinations();
Modulation::State makeJitterParityRecipe(Modulation::State, int macroOneBased = 1);
}
