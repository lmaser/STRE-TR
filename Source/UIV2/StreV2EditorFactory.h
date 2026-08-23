#pragma once

class STRETRAudioProcessor;
namespace juce { class AudioProcessorEditor; }

namespace TR::StreUIV2
{
juce::AudioProcessorEditor* createEditor(STRETRAudioProcessor& processor);
}
