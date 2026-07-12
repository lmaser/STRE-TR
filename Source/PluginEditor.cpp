// PluginEditor.cpp  ï¿½  STRE-TR
#include "PluginEditor.h"
#include "InfoContent.h"
#include <functional>

using namespace TR;

#if JUCE_WINDOWS
 #include <windows.h>
#endif

namespace UiStateKeys = TR::SimpleUiStateKeys;

// -- Timer & display constants --
static constexpr int   kCrtTimerHz   = 10;
static constexpr int   kIdleTimerHz  = 4;
static constexpr float kSilenceDb    = -80.0f;
static constexpr float kPitchZeroEpsilon = 0.005f;

// Pitch stays internally normalised 0..1. The UI exposes semitones because that
// is the real role of this parameter in STRE-TR.
static constexpr double kPitchCenter       = 0.5;
static constexpr double kPitchOctaves      = 4.0;
static constexpr double kPitchSemitoneSpan = kPitchOctaves * 12.0;
static constexpr double kPitchMinSemitones = -24.0;
static constexpr double kPitchMaxSemitones =  24.0;

static bool isGainFaderFloor (float dB) noexcept
{
    return dB <= STRETRAudioProcessor::kGainFloorDb + 0.001f;
}

static juce::String formatGainFaderDb (float dB)
{
    if (isGainFaderFloor (dB))
        return "-INF dB";
    if (std::abs (dB) < 0.05f)
        return "0.0 dB";
    return juce::String (dB, 1) + " dB";
}

static juce::String formatGainFaderDbCompact (float dB)
{
    if (isGainFaderFloor (dB))
        return "-INFdB";
    if (std::abs (dB) < 0.05f)
        return "0.0dB";
    return juce::String (dB, 1) + "dB";
}

static juce::String formatChaosTooltip (float amountPercent, float speedHz)
{
    return "AMT " + juce::String (juce::roundToInt (juce::jlimit (0.0f, 100.0f, amountPercent))) + "%"
         + " | SPD " + juce::String (juce::jlimit (STRETRAudioProcessor::kChaosSpdMin,
                                                   STRETRAudioProcessor::kChaosSpdMax,
                                                   speedHz), 1)
         + " Hz";
}

static juce::String formatPdcTooltip (bool enabled, int maxWindow)
{
    return juce::String (enabled ? "PDC ON" : "PDC OFF")
         + " | MAX WIN " + juce::String (STRETRAudioProcessor::getCanonicalFftWindow (maxWindow));
}

static juce::String formatTriggerDelayTooltip (int delayMs)
{
    return "DLY " + juce::String (juce::jlimit (0, 100, delayMs)) + " ms";
}

static juce::String formatSidechainTooltip (float gainDb, float smooth, float pol,
                                            bool hpOn, float hpFreq, int hpSlope,
                                            bool lpOn, float lpFreq, int lpSlope)
{
    auto slopeText = [] (int slope)
    {
        return juce::String (slope == 0 ? 6 : slope == 1 ? 12 : 24) + "dB";
    };
    auto freqText = [] (float hz)
    {
        return hz >= 1000.0f ? juce::String (hz / 1000.0f, 2) + "kHz" : juce::String (hz, 0) + "Hz";
    };

    juce::String text = "GAIN " + formatGainFaderDb (gainDb)
        + " / SMTH " + juce::String (smooth * 100.0f, 1) + "%"
        + " / POL " + juce::String (pol, 2);
    text += " / HP " + (hpOn ? freqText (hpFreq) + " " + slopeText (hpSlope) : juce::String ("OFF"));
    text += " / LP " + (lpOn ? freqText (lpFreq) + " " + slopeText (lpSlope) : juce::String ("OFF"));
    return text;
}

static double pitchSliderToSemitones (double v)
{
    v = juce::jlimit (0.0, 1.0, v);
    return (v - kPitchCenter) * kPitchSemitoneSpan;
}

static double semitonesToPitchSlider (double semitones)
{
    semitones = juce::jlimit (kPitchMinSemitones, kPitchMaxSemitones, semitones);
    return juce::jlimit (0.0, 1.0, kPitchCenter + (semitones / kPitchSemitoneSpan));
}

static juce::String formatPitchSemitones (double semitones, int decimals)
{
    if (std::abs (semitones) < kPitchZeroEpsilon)
        semitones = 0.0;

    return (semitones >= 0.0 ? "+" : "") + juce::String (semitones, decimals);
}

// -- Parameter listener IDs --
static constexpr std::array<const char*, 8> kUiMirrorParamIds {
    STRETRAudioProcessor::kParamUiPalette,
    STRETRAudioProcessor::kParamUiCrt,
    STRETRAudioProcessor::kParamUiIoFx,
    STRETRAudioProcessor::kParamUiColor0,
    STRETRAudioProcessor::kParamUiColor1,
    STRETRAudioProcessor::kParamUiColor2,
    STRETRAudioProcessor::kParamUiColor3,
    STRETRAudioProcessor::kParamEngine
};

//========================== Legacy local UI (disabled; STRE uses TR-Shared) ==========================

//========================== Legend width constants ==========================
namespace
{
    constexpr const char* kAmountLegendFull   = "100% AMOUNT";
    constexpr const char* kAmountLegendShort  = "100% AMT";
    constexpr const char* kAmountLegendInt    = "100%";

    constexpr const char* kPitchLegendFull   = "+24.00 st PITCH";
    constexpr const char* kPitchLegendShort  = "+24.00st";
    constexpr const char* kPitchLegendInt    = "+24.00st";

    constexpr const char* kJitterLegendFull  = "100% JITTER";
    constexpr const char* kJitterLegendShort = "100% JIT";
    constexpr const char* kJitterLegendInt   = "100%";

    constexpr const char* kGrainLegendFull  = "500.0 ms GRAIN";
    constexpr const char* kGrainLegendShort = "500.0ms GRN";
    constexpr const char* kGrainLegendInt   = "500ms";

    constexpr const char* kEngineLegendFull  = "STRETCH ENGINE";
    constexpr const char* kEngineLegendShort = "STRETCH";
    constexpr const char* kEngineLegendInt   = "STRETCH";

    constexpr const char* kWindowLegendFull  = "8192 WINDOW";
    constexpr const char* kWindowLegendShort = "8192 WIN";
    constexpr const char* kWindowLegendInt   = "8192";

    constexpr const char* kStyleLegendFull  = "STEREO STYLE";
    constexpr const char* kStyleLegendShort = "STEREO";
    constexpr const char* kStyleLegendInt   = "1";

    constexpr const char* kInputLegendFull   = "-INF dB INPUT";
    constexpr const char* kInputLegendShort  = "-INF dB IN";
    constexpr const char* kInputLegendInt    = "-INFdB";

    constexpr const char* kOutputLegendFull  = "-INF dB OUTPUT";
    constexpr const char* kOutputLegendShort = "-INF dB OUT";
    constexpr const char* kOutputLegendInt   = "-INFdB";

    constexpr const char* kMixLegendFull   = "100% MIX";
    constexpr const char* kMixLegendShort  = "100% MIX";
    constexpr const char* kMixLegendInt    = "100%";

    constexpr const char* kLimLegendFull   = "-36.0 dB LIM";
    constexpr const char* kLimLegendShort  = "-36.0 dB LIM";
    constexpr const char* kLimLegendInt    = "-36.0dB";

    constexpr int kResizerCornerPx = 22;
    constexpr int kTitleAreaExtraHeightPx = 4;
    constexpr int kTitleRightGapToInfoPx = 8;
    constexpr int kVersionGapPx = 8;

    // -- UI helper types for popup prompts (now in shared) --
}

//========================== Editor Constructor ==========================

STRETRAudioProcessorEditor::STRETRAudioProcessorEditor (STRETRAudioProcessor& p)
    : AudioProcessorEditor (&p), audioProcessor (p)
{
    const std::array<BarSlider*, 13> barSliders {
        &amountSlider, &pitchSlider, &grainSlider, &engineSlider, &windowSlider, &jitterSlider, &styleSlider,
        &inputSlider, &outputSlider, &tiltSlider, &panSlider, &mixSlider, &limThresholdSlider
    };

    useCustomPalette = audioProcessor.getUiUseCustomPalette();
    crtEnabled = audioProcessor.getUiCrtEnabled();
    ioFxEnabled = audioProcessor.getUiIoFxEnabled();
    ioSectionExpanded_ = audioProcessor.getUiIoExpanded();

    for (int i = 0; i < kPaletteColourCount; ++i)
        customPalette[(size_t) i] = audioProcessor.getUiCustomPaletteColour (i);

    TR::SimpleEditorLifecycle::initCommon (*this, audioProcessor, lnf, tooltipWindow,
        promptOverlay, resizeConstrainer, resizerCorner, kMinW, kMinH, kMaxW, kMaxH);
    applyActivePalette();

    const int restoredW = getWidth();
    const int restoredH = getHeight();
    suppressSizePersistence = true;
    suppressSizePersistence = false;
    lastPersistedEditorW = restoredW;
    lastPersistedEditorH = restoredH;

    for (auto* slider : barSliders)
    {
        slider->setOwner (this);
        setupBar (*slider);
        addAndMakeVisible (*slider);
        slider->addListener (this);
    }

    amountSlider.setNumDecimalPlacesToDisplay (2);
    pitchSlider.setNumDecimalPlacesToDisplay (2);
    grainSlider.setNumDecimalPlacesToDisplay (1);
    engineSlider.setNumDecimalPlacesToDisplay (0);
    windowSlider.setNumDecimalPlacesToDisplay (0);
    jitterSlider.setNumDecimalPlacesToDisplay (2);
    styleSlider.setNumDecimalPlacesToDisplay (0);
    inputSlider.setNumDecimalPlacesToDisplay (1);
    outputSlider.setNumDecimalPlacesToDisplay (1);
    inputSlider.setSkewFactor (STRETRAudioProcessor::kGainSkew);
    outputSlider.setSkewFactor (STRETRAudioProcessor::kGainSkew);
    tiltSlider.setNumDecimalPlacesToDisplay (1);
    panSlider.setNumDecimalPlacesToDisplay (1);
    mixSlider.setNumDecimalPlacesToDisplay (1);
    limThresholdSlider.setNumDecimalPlacesToDisplay (1);

    // IO sliders start hidden (collapsible section)
    TR::setSimpleComponentVisible (inputSlider, false);
    TR::setSimpleComponentVisible (outputSlider, false);
    TR::setSimpleComponentVisible (tiltSlider, false);
    TR::setSimpleComponentVisible (panSlider, false);
    TR::setSimpleComponentVisible (mixSlider, false);
    TR::setSimpleComponentVisible (limThresholdSlider, false);

    filterBar_.setOwner (this);
    filterBar_.setScheme (activeScheme);
    addAndMakeVisible (filterBar_);
    TR::setSimpleComponentVisible (filterBar_, false);
    filterBar_.updateFromProcessor();

    // Chaos filter button + tooltip overlay
    chaosFilterButton.setButtonText ("");
    addAndMakeVisible (chaosFilterButton);
    TR::setSimpleComponentVisible (chaosFilterButton, false);
    {
        const float savedAmtF = audioProcessor.apvts.getRawParameterValue (STRETRAudioProcessor::kParamChaosAmtFilter)->load();
        const float savedSpdF = audioProcessor.apvts.getRawParameterValue (STRETRAudioProcessor::kParamChaosSpdFilter)->load();
        chaosFilterDisplay.setText ("", juce::dontSendNotification);
        chaosFilterDisplay.setInterceptsMouseClicks (true, false);
        chaosFilterDisplay.addMouseListener (this, false);
        chaosFilterDisplay.setTooltip (formatChaosTooltip (savedAmtF, savedSpdF));
        TR::configureSimpleTransparentDisplayLabel (chaosFilterDisplay, activeScheme);
        addAndMakeVisible (chaosFilterDisplay);
        TR::setSimpleComponentVisible (chaosFilterDisplay, false);
    }

    // Chaos delay button + tooltip overlay
    chaosDelayButton.setButtonText ("");
    addAndMakeVisible (chaosDelayButton);
    TR::setSimpleComponentVisible (chaosDelayButton, false);
    {
        const float savedAmtD = audioProcessor.apvts.getRawParameterValue (STRETRAudioProcessor::kParamChaosAmt)->load();
        const float savedSpdD = audioProcessor.apvts.getRawParameterValue (STRETRAudioProcessor::kParamChaosSpd)->load();
        chaosDelayDisplay.setText ("", juce::dontSendNotification);
        chaosDelayDisplay.setInterceptsMouseClicks (true, false);
        chaosDelayDisplay.addMouseListener (this, false);
        chaosDelayDisplay.setTooltip (formatChaosTooltip (savedAmtD, savedSpdD));
        TR::configureSimpleTransparentDisplayLabel (chaosDelayDisplay, activeScheme);
        addAndMakeVisible (chaosDelayDisplay);
        TR::setSimpleComponentVisible (chaosDelayDisplay, false);
    }
    sidechainButton.setButtonText ("");
    addAndMakeVisible (sidechainButton);
    TR::setSimpleComponentVisible (sidechainButton, false);
    {
        auto& s = audioProcessor.apvts;
        sidechainDisplay.setText ("", juce::dontSendNotification);
        sidechainDisplay.setInterceptsMouseClicks (true, false);
        sidechainDisplay.addMouseListener (this, false);
        sidechainDisplay.setTooltip (formatSidechainTooltip (
            s.getRawParameterValue (STRETRAudioProcessor::kParamSidechainGain)->load(),
            s.getRawParameterValue (STRETRAudioProcessor::kParamSidechainSmooth)->load(),
            s.getRawParameterValue (STRETRAudioProcessor::kParamSidechainPol)->load(),
            s.getRawParameterValue (STRETRAudioProcessor::kParamSidechainHpOn)->load() > 0.5f,
            s.getRawParameterValue (STRETRAudioProcessor::kParamSidechainHp)->load(),
            (int) std::lround (s.getRawParameterValue (STRETRAudioProcessor::kParamSidechainHpSlope)->load()),
            s.getRawParameterValue (STRETRAudioProcessor::kParamSidechainLpOn)->load() > 0.5f,
            s.getRawParameterValue (STRETRAudioProcessor::kParamSidechainLp)->load(),
            (int) std::lround (s.getRawParameterValue (STRETRAudioProcessor::kParamSidechainLpSlope)->load())));
        TR::configureSimpleTransparentDisplayLabel (sidechainDisplay, activeScheme);
        addAndMakeVisible (sidechainDisplay);
        TR::setSimpleComponentVisible (sidechainDisplay, false);
    }

    reverseButton.setButtonText ("");
    triggerButton.setButtonText ("");
    alignButton.setButtonText ("");
    pdcButton.setButtonText ("");

    addAndMakeVisible (reverseButton);
    addAndMakeVisible (triggerButton);
    addAndMakeVisible (alignButton);
    addAndMakeVisible (pdcButton);

    triggerDisplay.setText ("", juce::dontSendNotification);
    triggerDisplay.setInterceptsMouseClicks (true, false);
    triggerDisplay.addMouseListener (this, false);
    triggerDisplay.setTooltip (formatTriggerDelayTooltip (audioProcessor.getTriggerDelayMs()));
    TR::configureSimpleTransparentDisplayLabel (triggerDisplay, activeScheme);
    addAndMakeVisible (triggerDisplay);

    pdcDisplay.setText ("", juce::dontSendNotification);
    pdcDisplay.setInterceptsMouseClicks (true, false);
    pdcDisplay.addMouseListener (this, false);
    pdcDisplay.setTooltip (formatPdcTooltip (pdcButton.getToggleState(), getCurrentMaxFftWindow()));
    TR::configureSimpleTransparentDisplayLabel (pdcDisplay, activeScheme);
    addAndMakeVisible (pdcDisplay);

    auto bindSlider = [&] (std::unique_ptr<SliderAttachment>& attachment,
                           const char* paramId, BarSlider& slider, double defaultValue)
    {
        attachment = std::make_unique<SliderAttachment> (audioProcessor.apvts, paramId, slider);
        slider.setDoubleClickReturnValue (true, defaultValue);
    };

    bindSlider (amountAttachment,  STRETRAudioProcessor::kParamAmount,  amountSlider,  kDefaultAmount);
    bindSlider (pitchAttachment,     STRETRAudioProcessor::kParamPitch,     pitchSlider,     0.5);
    bindSlider (grainAttachment,   STRETRAudioProcessor::kParamGrain,   grainSlider,   (double) STRETRAudioProcessor::kGrainDefault);
    bindSlider (engineAttachment,  STRETRAudioProcessor::kParamEngine,  engineSlider,  (double) STRETRAudioProcessor::kEngineDefault);
    bindSlider (windowAttachment,  STRETRAudioProcessor::kParamWindow,  windowSlider,  (double) STRETRAudioProcessor::kWindowDefault);
    bindSlider (jitterAttachment,  STRETRAudioProcessor::kParamJitter,  jitterSlider,  (double) STRETRAudioProcessor::kJitterDefault);
    bindSlider (styleAttachment,   STRETRAudioProcessor::kParamStyle,   styleSlider,   (double) STRETRAudioProcessor::kStyleDefault);
    bindSlider (inputAttachment,   STRETRAudioProcessor::kParamInput,   inputSlider,   kDefaultInput);
    bindSlider (outputAttachment,  STRETRAudioProcessor::kParamOutput,  outputSlider,  kDefaultOutput);
    bindSlider (tiltAttachment,    STRETRAudioProcessor::kParamTilt,    tiltSlider,    kDefaultTilt);
    bindSlider (panAttachment,     STRETRAudioProcessor::kParamPan,     panSlider,     0.5);
    bindSlider (mixAttachment,     STRETRAudioProcessor::kParamMix,     mixSlider,     kDefaultMix);
    bindSlider (limThresholdAttachment, STRETRAudioProcessor::kParamLimThreshold, limThresholdSlider, kDefaultLimThreshold);

    // Mode In / Mode Out / Sum Bus combos
    {
        auto setupModeCombo = [this] (juce::ComboBox& combo)
        {
            addAndMakeVisible (combo);
            combo.addItem ("L+R",  1);
            combo.addItem ("MID",  2);
            combo.addItem ("SIDE", 3);
            TR::centreSimpleCombo (combo);
            combo.setLookAndFeel (&lnf);
            TR::setSimpleComponentVisible (combo, false);
        };
        setupModeCombo (modeInCombo);
        setupModeCombo (modeOutCombo);

        addAndMakeVisible (sumBusCombo);
        sumBusCombo.addItem ("ST", 1);
        sumBusCombo.addItem (juce::String::fromUTF8 (u8"\u2192M"), 2);
        sumBusCombo.addItem (juce::String::fromUTF8 (u8"\u2192S"), 3);
        TR::centreSimpleCombo (sumBusCombo);
        sumBusCombo.setLookAndFeel (&lnf);
        TR::setSimpleComponentVisible (sumBusCombo, false);

        modeInAttachment  = std::make_unique<ComboBoxAttachment> (audioProcessor.apvts, STRETRAudioProcessor::kParamModeIn,  modeInCombo);
        modeOutAttachment = std::make_unique<ComboBoxAttachment> (audioProcessor.apvts, STRETRAudioProcessor::kParamModeOut, modeOutCombo);
        sumBusAttachment  = std::make_unique<ComboBoxAttachment> (audioProcessor.apvts, STRETRAudioProcessor::kParamSumBus,  sumBusCombo);
    }

    // Limiter mode combo
    {
        addAndMakeVisible (limModeCombo);
        limModeCombo.addItem ("NONE",   1);
        limModeCombo.addItem ("WET",    2);
        limModeCombo.addItem ("GLOBAL", 3);
        TR::centreSimpleCombo (limModeCombo);
        limModeCombo.setLookAndFeel (&lnf);
        TR::setSimpleComponentVisible (limModeCombo, false);
        limModeAttachment = std::make_unique<ComboBoxAttachment> (audioProcessor.apvts, STRETRAudioProcessor::kParamLimMode, limModeCombo);
    }

    // Invert Polarity / Invert Stereo combos
    {
        auto setupInvCombo = [this] (juce::ComboBox& combo)
        {
            addAndMakeVisible (combo);
            combo.addItem ("NONE",   1);
            combo.addItem ("WET",    2);
            combo.addItem ("GLOBAL", 3);
            TR::centreSimpleCombo (combo);
            combo.setLookAndFeel (&lnf);
            TR::setSimpleComponentVisible (combo, false);
        };
        setupInvCombo (invPolCombo);
        setupInvCombo (invStrCombo);
        invPolAttachment = std::make_unique<ComboBoxAttachment> (audioProcessor.apvts, STRETRAudioProcessor::kParamInvPol, invPolCombo);
        invStrAttachment = std::make_unique<ComboBoxAttachment> (audioProcessor.apvts, STRETRAudioProcessor::kParamInvStr, invStrCombo);
    }

    // Mix Mode combo (INSERT / SEND)
    {
        addAndMakeVisible (mixModeCombo);
        mixModeCombo.addItem ("INSERT", 1);
        mixModeCombo.addItem ("SEND",   2);
        TR::centreSimpleCombo (mixModeCombo);
        mixModeCombo.setLookAndFeel (&lnf);
        TR::setSimpleComponentVisible (mixModeCombo, false);
        mixModeAttachment = std::make_unique<ComboBoxAttachment> (audioProcessor.apvts, STRETRAudioProcessor::kParamMixMode, mixModeCombo);
    }

    // Filter Position combo (POST / PRE)
    {
        addAndMakeVisible (filterPosCombo);
        filterPosCombo.addItem (juce::String::fromUTF8 (u8"F\u25bc T\u25bc"), 1);
        filterPosCombo.addItem (juce::String::fromUTF8 (u8"F\u25b2 T\u25b2"), 2);
        filterPosCombo.addItem (juce::String::fromUTF8 (u8"F\u25b2 T\u25bc"), 3);
        filterPosCombo.addItem (juce::String::fromUTF8 (u8"F\u25bc T\u25b2"), 4);
        TR::centreSimpleCombo (filterPosCombo);
        filterPosCombo.setLookAndFeel (&lnf);
        TR::setSimpleComponentVisible (filterPosCombo, false);
        filterPosAttachment = std::make_unique<ComboBoxAttachment> (audioProcessor.apvts, STRETRAudioProcessor::kParamFilterPos, filterPosCombo);
    }

    // Dual Mix Bar (SEND mode)
    addAndMakeVisible (dualMixBar_);
    dualMixBar_.setOwner (this);
    TR::setSimpleComponentVisible (dualMixBar_, false);

    // Disable numeric popup for discrete sliders
    engineSlider.setAllowNumericPopup (false);
    windowSlider.setAllowNumericPopup (false);
    styleSlider.setAllowNumericPopup (false);

    auto bindButton = [&] (std::unique_ptr<ButtonAttachment>& attachment,
                           const char* paramId, juce::Button& button)
    {
        attachment = std::make_unique<ButtonAttachment> (audioProcessor.apvts, paramId, button);
    };

    bindButton (reverseAttachment,      STRETRAudioProcessor::kParamReverse,  reverseButton);
    bindButton (triggerAttachment,      STRETRAudioProcessor::kParamTrigger,  triggerButton);
    bindButton (alignAttachment,        STRETRAudioProcessor::kParamAlign,    alignButton);
    bindButton (pdcAttachment,          STRETRAudioProcessor::kParamPdc,      pdcButton);
    bindButton (chaosFilterAttachment,  STRETRAudioProcessor::kParamChaos,    chaosFilterButton);
    bindButton (chaosDelayAttachment,   STRETRAudioProcessor::kParamChaosD,   chaosDelayButton);
    bindButton (sidechainAttachment,    STRETRAudioProcessor::kParamSidechain, sidechainButton);

    for (auto* paramId : kUiMirrorParamIds)
        audioProcessor.apvts.addParameterListener (paramId, this);
    audioProcessor.apvts.addParameterListener (STRETRAudioProcessor::kParamPdc, this);
    audioProcessor.apvts.addParameterListener (STRETRAudioProcessor::kParamWindow, this);
    audioProcessor.apvts.addParameterListener (STRETRAudioProcessor::kParamMaxWindow, this);

    TR::SimpleEditorLifecycle::scheduleUiRestore (*this);

    applyCrtState (crtEnabled);
    syncFftWindowToMax (true);
    updatePdcTooltip();
    refreshLegendTextCache();
    resized();
    updateEngineControls();
}

STRETRAudioProcessorEditor::~STRETRAudioProcessorEditor()
{
    setComponentEffect (nullptr);
    stopTimer();

    for (auto* paramId : kUiMirrorParamIds)
        audioProcessor.apvts.removeParameterListener (paramId, this);
    audioProcessor.apvts.removeParameterListener (STRETRAudioProcessor::kParamPdc, this);
    audioProcessor.apvts.removeParameterListener (STRETRAudioProcessor::kParamWindow, this);
    audioProcessor.apvts.removeParameterListener (STRETRAudioProcessor::kParamMaxWindow, this);

    audioProcessor.setUiUseCustomPalette (useCustomPalette);
    audioProcessor.setUiCrtEnabled (crtEnabled);

    dismissEditorOwnedModalPrompts (lnf);
    setPromptOverlayActive (false);

    const std::array<BarSlider*, 13> barSliders {
        &amountSlider, &pitchSlider, &grainSlider, &engineSlider, &windowSlider, &jitterSlider, &styleSlider,
        &inputSlider, &outputSlider, &tiltSlider, &panSlider, &mixSlider, &limThresholdSlider
    };
    for (auto* slider : barSliders)
        slider->removeListener (this);

    if (tooltipWindow) tooltipWindow->setLookAndFeel (nullptr);

    modeInCombo.setLookAndFeel (nullptr);
    modeOutCombo.setLookAndFeel (nullptr);
    sumBusCombo.setLookAndFeel (nullptr);
    limModeCombo.setLookAndFeel (nullptr);
    invPolCombo.setLookAndFeel (nullptr);
    invStrCombo.setLookAndFeel (nullptr);
    mixModeCombo.setLookAndFeel (nullptr);
    filterPosCombo.setLookAndFeel (nullptr);

    setLookAndFeel (nullptr);
}

//========================== State management ==========================

void STRETRAudioProcessorEditor::applyActivePalette()
{
    const auto& palette = useCustomPalette ? customPalette : defaultPalette;

    activeScheme = TR::applySimplePalette (palette, lnf,
        { &chaosFilterDisplay, &chaosDelayDisplay, &sidechainDisplay, &triggerDisplay, &pdcDisplay },
        { &amountSlider, &pitchSlider, &grainSlider, &engineSlider, &windowSlider, &jitterSlider, &styleSlider,
          &inputSlider, &outputSlider, &tiltSlider, &panSlider, &mixSlider, &limThresholdSlider },
        { &modeInCombo, &modeOutCombo, &sumBusCombo, &limModeCombo, &invPolCombo, &invStrCombo, &mixModeCombo, &filterPosCombo });

    filterBar_.setScheme (activeScheme);
    dualMixBar_.setScheme (activeScheme);
    updateIoFxMeterSliders();
}

void STRETRAudioProcessorEditor::applyIoFxState (bool enabled)
{
    ioFxEnabled = enabled;
    updateIoFxMeterSliders();
}

void STRETRAudioProcessorEditor::updateIoFxMeterSliders()
{
    TR::SimpleUIController::updateIoMeters (defaultPalette, customPalette, useCustomPalette,
        inputSlider, outputSlider, ioFxEnabled,
        lastInputSignalMs, lastOutputSignalMs,
        audioProcessor.getInputMeterPeak(), audioProcessor.getOutputMeterPeak());
}

void STRETRAudioProcessorEditor::applyCrtState (bool enabled)
{
    // Legacy Graphic FX/CRT is intentionally disabled. I/O FX is handled by
    // applyIoFxState/updateIoFxMeterSliders and must not resurrect CRT from old settings.
    juce::ignoreUnused (enabled);
    crtEnabled = false;
    crtEffect.setEnabled (false);
    setComponentEffect (nullptr);
    stopTimer();
    startTimerHz (kIdleTimerHz);
}

void STRETRAudioProcessorEditor::applyLabelTextColour (juce::Label& label, juce::Colour colour)
{
    TR::applySimpleLabelTextColour (label, colour);
}

void STRETRAudioProcessorEditor::sliderValueChanged (juce::Slider* slider)
{
    if (slider == &windowSlider && ! clampingWindowSlider_)
    {
        const int engineVal = getCurrentEngineValue();
        const int effectiveWindow = getEffectiveWindowValue (windowSlider.getValue());
        if ((int) std::lround (windowSlider.getValue()) != effectiveWindow)
        {
            juce::ScopedValueSetter<bool> clampGuard (clampingWindowSlider_, true);
            audioProcessor.setStoredWindowForEngine (engineVal, effectiveWindow);
            audioProcessor.syncWindowParameterToEngine (engineVal);
            windowSlider.setValue ((double) effectiveWindow, juce::dontSendNotification);
        }
        else
        {
            audioProcessor.setStoredWindowForEngine (engineVal, effectiveWindow);
        }
    }

    refreshLegendTextCache();
    if (slider == nullptr) { repaint(); return; }

    auto isBar = [&] (const juce::Slider* s)
    {
        return s == &amountSlider || s == &pitchSlider || s == &grainSlider || s == &engineSlider || s == &windowSlider
            || s == &jitterSlider || s == &styleSlider || s == &inputSlider || s == &outputSlider || s == &tiltSlider
            || s == &panSlider || s == &mixSlider || s == &limThresholdSlider;
    };

    if (isBar (slider)) { repaint (getRowRepaintBounds (*slider)); return; }
    repaint();
}

void STRETRAudioProcessorEditor::setPromptOverlayActive (bool shouldBeActive)
{
    TR::SimpleUIController::setOverlayActive (*this, promptOverlay, promptOverlayActive, shouldBeActive, lnf);
    repaint();
    if (! shouldBeActive) updateEngineControls();  // re-apply engine dimming
    TR::SimpleUIController::anchorPromptsOnMove (*this, promptOverlayActive, promptOverlay, lnf);
}

void STRETRAudioProcessorEditor::moved()
{
    TR::SimpleUIController::anchorPromptsOnMove (*this, promptOverlayActive, promptOverlay, lnf);
}

void STRETRAudioProcessorEditor::parentHierarchyChanged()
{
#if JUCE_WINDOWS
    if (auto* peer = getPeer())
    {
        if (auto nativeHandle = peer->getNativeHandle())
        {
            static HBRUSH blackBrush = CreateSolidBrush (RGB (0, 0, 0));
            SetClassLongPtr (static_cast<HWND> (nativeHandle), GCLP_HBRBACKGROUND,
                             reinterpret_cast<LONG_PTR> (blackBrush));
        }
    }
#endif
}

//========================== Callbacks ==========================

void STRETRAudioProcessorEditor::parameterChanged (const juce::String& parameterID, float newValue)
{
    juce::ignoreUnused (newValue);
    if (parameterID == STRETRAudioProcessor::kParamUiPalette
        || parameterID == STRETRAudioProcessor::kParamUiCrt
        || parameterID == STRETRAudioProcessor::kParamUiIoFx
        || parameterID == STRETRAudioProcessor::kParamUiColor0
        || parameterID == STRETRAudioProcessor::kParamUiColor1
        || parameterID == STRETRAudioProcessor::kParamUiColor2
        || parameterID == STRETRAudioProcessor::kParamUiColor3)
    {
        juce::Component::SafePointer<STRETRAudioProcessorEditor> safeThis (this);
        juce::MessageManager::callAsync ([safeThis]()
        {
            if (safeThis) safeThis->applyPersistedUiStateFromProcessor (false, true);
        });
    }

    if (parameterID == STRETRAudioProcessor::kParamEngine)
    {
        juce::Component::SafePointer<STRETRAudioProcessorEditor> safeThis (this);
        juce::MessageManager::callAsync ([safeThis]()
        {
            if (safeThis)
            {
                safeThis->syncFftWindowToMax (true);
                safeThis->updateEngineControls();
                safeThis->updatePdcTooltip();
            }
        });
    }

    if (parameterID == STRETRAudioProcessor::kParamWindow
        || parameterID == STRETRAudioProcessor::kParamMaxWindow
        || parameterID == STRETRAudioProcessor::kParamPdc)
    {
        juce::Component::SafePointer<STRETRAudioProcessorEditor> safeThis (this);
        juce::MessageManager::callAsync ([safeThis]()
        {
            if (safeThis == nullptr) return;
            safeThis->syncFftWindowToMax (true);
            safeThis->updatePdcTooltip();
            safeThis->refreshLegendTextCache();
            safeThis->repaint();
        });
    }
}

void STRETRAudioProcessorEditor::timerCallback()
{
    if (crtEnabled)
    {
        crtTime += 1.0f / (float) kCrtTimerHz;
        crtEffect.setTime (crtTime);
        repaint();
    }

    // Size persistence
    {
        const int W = getWidth(), H = getHeight();
        if (! suppressSizePersistence && (W != lastPersistedEditorW || H != lastPersistedEditorH))
        {
            const uint32_t last = lastUserInteractionMs.load (std::memory_order_relaxed);
            const uint32_t now = juce::Time::getMillisecondCounter();
            if ((now - last) <= kUserInteractionPersistWindowMs)
            {
                audioProcessor.setUiEditorSize (W, H);
                lastPersistedEditorW = W;
                lastPersistedEditorH = H;
            }
        }
    }

    filterBar_.updateFromProcessor();
    updateIoFxMeterSliders();

    // Keep dual mix bar markers up to date + visibility swap
    if (ioSectionExpanded_)
    {
        const float prevDry = dualMixBar_.getDryLevel();
        const float prevWet = dualMixBar_.getWetLevel();
        dualMixBar_.updateFromProcessor();
        const bool isSendMode = mixModeCombo.getSelectedId() == 2;

        // Refresh legend when levels change in SEND mode
        if (isSendMode && (dualMixBar_.getDryLevel() != prevDry || dualMixBar_.getWetLevel() != prevWet))
        {
            if (refreshLegendTextCache())
                updateCachedLayout();
            repaint();
        }

        if (mixSlider.isVisible() == isSendMode)
        {
            TR::setSimpleComponentVisible (mixSlider, ! isSendMode);
            TR::setSimpleComponentVisible (dualMixBar_, isSendMode);
            if (refreshLegendTextCache())
                updateCachedLayout();
            repaint();
        }
    }
    else
    {
        if (dualMixBar_.isVisible())
            dualMixBar_.updateFromProcessor();

        const bool isSend = (mixModeCombo.getSelectedItemIndex() == 1);
        if (isSend && mixSlider.isVisible())
        {
            TR::setSimpleComponentVisible (mixSlider, false);
            TR::setSimpleComponentVisible (dualMixBar_, true);
        }
        else if (! isSend && dualMixBar_.isVisible())
        {
            TR::setSimpleComponentVisible (dualMixBar_, false);
            TR::setSimpleComponentVisible (mixSlider, true);
        }
    }
}

void STRETRAudioProcessorEditor::updateEngineControls()
{
    if (promptOverlayActive) return;  // don't override prompt overlay state

	auto* engineP = audioProcessor.apvts.getRawParameterValue (STRETRAudioProcessor::kParamEngine);
	const int engineVal = engineP ? (int) std::lround (engineP->load (std::memory_order_relaxed)) : 0;
    syncFftWindowToMax (false);

	const bool grainActive = (engineVal == 1);   // GRAIN only
	grainSlider.setAlpha (grainActive ? 1.0f : 0.35f);
	grainSlider.setEnabled (grainActive);

	const int familyWindow = audioProcessor.getStoredWindowForEngine (engineVal);
	if (! clampingWindowSlider_ && (int) std::lround (windowSlider.getValue()) != familyWindow)
	{
		juce::ScopedValueSetter<bool> clampGuard (clampingWindowSlider_, true);
		audioProcessor.syncWindowParameterToEngine (engineVal);
		windowSlider.setValue ((double) familyWindow, juce::dontSendNotification);
		return;
	}

	if ((engineVal == 2 || engineVal == 3) && ! clampingWindowSlider_)
	{
        const int effectiveWindow = getEffectiveWindowValue (windowSlider.getValue());
        if ((int) std::lround (windowSlider.getValue()) != effectiveWindow)
        {
            juce::ScopedValueSetter<bool> clampGuard (clampingWindowSlider_, true);
            audioProcessor.setStoredWindowForEngine (engineVal, effectiveWindow);
            audioProcessor.syncWindowParameterToEngine (engineVal);
            windowSlider.setValue ((double) effectiveWindow, juce::dontSendNotification);
            return;
        }
    }

	const bool reverseActive = true;
	reverseButton.setAlpha (reverseActive ? 1.0f : 0.35f);
	reverseButton.setEnabled (reverseActive);

    refreshLegendTextCache();
    repaint();
}

void STRETRAudioProcessorEditor::applyPersistedUiStateFromProcessor (bool applySize, bool applyPaletteAndFx)
{
    if (applySize)
    {
        const int targetW = juce::jlimit (kMinW, kMaxW, audioProcessor.getUiEditorWidth());
        const int targetH = juce::jlimit (kMinH, kMaxH, audioProcessor.getUiEditorHeight());
        if (targetW != getWidth() || targetH != getHeight())
        {
            suppressSizePersistence = true;
            setSize (targetW, targetH);
            suppressSizePersistence = false;
            lastPersistedEditorW = targetW;
            lastPersistedEditorH = targetH;
        }
    }

    if (applyPaletteAndFx)
    {
        const bool targetUseCustomPalette = audioProcessor.getUiUseCustomPalette();
        const bool targetCrtEnabled = audioProcessor.getUiCrtEnabled();
        const bool targetIoFxEnabled = audioProcessor.getUiIoFxEnabled();
        const bool targetIoExpanded = audioProcessor.getUiIoExpanded();

        std::array<juce::Colour, kPaletteColourCount> targetCustomPalette;
        for (int i = 0; i < kPaletteColourCount; ++i)
            targetCustomPalette[(size_t) i] = audioProcessor.getUiCustomPaletteColour (i);

        bool paletteChanged = false;
        for (int i = 0; i < kPaletteColourCount; ++i)
            if (targetCustomPalette[(size_t) i] != customPalette[(size_t) i])
            { customPalette[(size_t) i] = targetCustomPalette[(size_t) i]; paletteChanged = true; }

        const bool paletteSwitchChanged = targetUseCustomPalette != useCustomPalette;
        const bool fxChanged = targetCrtEnabled != crtEnabled;
        const bool ioFxChanged = targetIoFxEnabled != ioFxEnabled;
        const bool ioChanged = targetIoExpanded != ioSectionExpanded_;

        if (ioChanged) { ioSectionExpanded_ = targetIoExpanded; resized(); }
        if (paletteSwitchChanged) useCustomPalette = targetUseCustomPalette;
        if (fxChanged) applyCrtState (targetCrtEnabled);
        if (ioFxChanged) applyIoFxState (targetIoFxEnabled);
        if (paletteChanged || paletteSwitchChanged) applyActivePalette();
        if (paletteChanged || paletteSwitchChanged || fxChanged || ioFxChanged || ioChanged) repaint();
    }
}

//========================== Text getters & cache ==========================

void STRETRAudioProcessorEditor::setupBar (juce::Slider& s)
{
    s.setSliderStyle (juce::Slider::LinearBar);
    s.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
    s.setPopupDisplayEnabled (false, false, this);
    s.setTooltip (juce::String());
    s.setPopupMenuEnabled (false);
    TR::applySimpleTransparentSliderColours (s, activeScheme);
}

juce::String STRETRAudioProcessorEditor::getAmountText() const
{
    const float v = (float) amountSlider.getValue();
    return juce::String (juce::roundToInt (v)) + "% AMOUNT";
}
juce::String STRETRAudioProcessorEditor::getAmountTextShort() const
{
    const float v = (float) amountSlider.getValue();
    return juce::String (juce::roundToInt (v)) + "% AMT";
}

juce::String STRETRAudioProcessorEditor::getPitchText() const
{
    return formatPitchSemitones (pitchSliderToSemitones (pitchSlider.getValue()), 2) + " st PITCH";
}

juce::String STRETRAudioProcessorEditor::getPitchTextShort() const
{
    return formatPitchSemitones (pitchSliderToSemitones (pitchSlider.getValue()), 2) + "st";
}

juce::String STRETRAudioProcessorEditor::getJitterText() const
{
    const float v = (float) jitterSlider.getValue();
    return juce::String (juce::roundToInt (v)) + "% JITTER";
}
juce::String STRETRAudioProcessorEditor::getJitterTextShort() const
{
    const float v = (float) jitterSlider.getValue();
    return juce::String (juce::roundToInt (v)) + "% JIT";
}

juce::String STRETRAudioProcessorEditor::getGrainText() const
{
    if (getCurrentEngineValue() != 1)
        return "GRAIN";

    const float ms = (float) grainSlider.getValue();
    return juce::String (ms, 1) + " ms GRAIN";
}
juce::String STRETRAudioProcessorEditor::getGrainTextShort() const
{
    if (getCurrentEngineValue() != 1)
        return "GRAIN";

    const float ms = (float) grainSlider.getValue();
    return juce::String (ms, 1) + "ms GRN";
}

juce::String STRETRAudioProcessorEditor::getEngineText() const
{
    const int mode = (int) std::lround (engineSlider.getValue());
    switch (mode)
    {
        case 0: return "STRETCH ENGINE";
        case 1: return "GRAIN ENGINE";
        case 2: return "FFT1 ENGINE";
        case 3: return "FFT2 ENGINE";
        default: return "STRETCH ENGINE";
    }
}
juce::String STRETRAudioProcessorEditor::getEngineTextShort() const
{
    const int mode = (int) std::lround (engineSlider.getValue());
    switch (mode)
    {
        case 0: return "STRETCH";
        case 1: return "GRAIN";
        case 2: return "FFT1";
        case 3: return "FFT2";
        default: return "STRETCH";
    }
}

int STRETRAudioProcessorEditor::getCurrentEngineValue() const
{
    if (auto* engineP = audioProcessor.apvts.getRawParameterValue (STRETRAudioProcessor::kParamEngine))
        return (int) std::lround (engineP->load (std::memory_order_relaxed));

    return (int) std::lround (engineSlider.getValue());
}

bool STRETRAudioProcessorEditor::isCurrentEngineFft() const
{
    const int engineVal = getCurrentEngineValue();
    return engineVal == 2 || engineVal == 3;
}

int STRETRAudioProcessorEditor::getCurrentMaxFftWindow() const
{
    return STRETRAudioProcessor::getCanonicalFftWindow ((int) std::lround (
        audioProcessor.apvts.getRawParameterValue (STRETRAudioProcessor::kParamMaxWindow)->load()));
}

void STRETRAudioProcessorEditor::syncFftWindowToMax (bool notifyHost)
{
    const int maxWindow = getCurrentMaxFftWindow();
    audioProcessor.clampFftWindowFamiliesToMax (maxWindow);

    if (! isCurrentEngineFft())
    {
        const double windowMax = (getCurrentEngineValue() == 1)
                               ? (double) STRETRAudioProcessor::kGrainWindowMax
                               : (double) STRETRAudioProcessor::kWindowMax;
        windowSlider.setRange ((double) STRETRAudioProcessor::kWindowMin,
                               windowMax,
                               1.0);
        windowSlider.setSkewFactor (0.5);
        const int clampedWindow = audioProcessor.getStoredWindowForEngine (getCurrentEngineValue());
        juce::ScopedValueSetter<bool> clampGuard (clampingWindowSlider_, true);
        if (auto* p = audioProcessor.apvts.getParameter (STRETRAudioProcessor::kParamWindow))
        {
            const bool differs = std::abs (audioProcessor.apvts.getRawParameterValue (STRETRAudioProcessor::kParamWindow)->load()
                                           - (float) clampedWindow) > 0.5f;
            if (notifyHost && differs)
                p->setValueNotifyingHost (p->convertTo0to1 ((float) clampedWindow));
            else
                audioProcessor.syncWindowParameterToEngine (getCurrentEngineValue());
        }
        if ((int) std::lround (windowSlider.getValue()) != clampedWindow)
        {
            windowSlider.setValue ((double) clampedWindow, juce::dontSendNotification);
        }
        return;
    }

    const int engineVal = getCurrentEngineValue();
    const int clampedWindow = juce::jmin (audioProcessor.getStoredWindowForEngine (engineVal), maxWindow);
    audioProcessor.setStoredWindowForEngine (engineVal, clampedWindow);

    const double sliderMin = maxWindow > STRETRAudioProcessor::kFftWindowMin
                           ? (double) STRETRAudioProcessor::kFftWindowMin
                           : 0.0;
    windowSlider.setRange (sliderMin, (double) maxWindow, 1.0);
    windowSlider.setSkewFactor (1.0);

    juce::ScopedValueSetter<bool> clampGuard (clampingWindowSlider_, true);
    if (auto* p = audioProcessor.apvts.getParameter (STRETRAudioProcessor::kParamWindow))
    {
        const bool differs = std::abs (audioProcessor.apvts.getRawParameterValue (STRETRAudioProcessor::kParamWindow)->load()
                                       - (float) clampedWindow) > 0.5f;
        if (notifyHost && differs)
            p->setValueNotifyingHost (p->convertTo0to1 ((float) clampedWindow));
        else
            audioProcessor.syncWindowParameterToEngine (engineVal);
    }
    windowSlider.setValue ((double) clampedWindow, juce::dontSendNotification);
}

void STRETRAudioProcessorEditor::updatePdcTooltip()
{
    const auto tooltip = formatPdcTooltip (pdcButton.getToggleState(), getCurrentMaxFftWindow());
    pdcDisplay.setTooltip (tooltip);
}

int STRETRAudioProcessorEditor::getEffectiveWindowValue (double rawWindowValue) const
{
    const int canonical = STRETRAudioProcessor::getCanonicalWindowForEngine (getCurrentEngineValue(),
                                                                             (int) std::lround (rawWindowValue));
    return isCurrentEngineFft() ? juce::jmin (canonical, getCurrentMaxFftWindow()) : canonical;
}

juce::String STRETRAudioProcessorEditor::getWindowText() const
{
    const int sz = getEffectiveWindowValue (windowSlider.getValue());
    return juce::String (sz) + " WINDOW";
}
juce::String STRETRAudioProcessorEditor::getWindowTextShort() const
{
    const int sz = getEffectiveWindowValue (windowSlider.getValue());
    return juce::String (sz) + " WIN";
}

juce::String STRETRAudioProcessorEditor::getStyleText() const
{
    const int mode = (int) std::lround (styleSlider.getValue());
    switch (mode)
    {
        case 0: return "MONO STYLE";
        case 1: return "STEREO STYLE";
        case 2: return "WIDE STYLE";
        case 3: return "DUAL STYLE";
        default: return "STEREO STYLE";
    }
}
juce::String STRETRAudioProcessorEditor::getStyleTextShort() const
{
    const int mode = (int) std::lround (styleSlider.getValue());
    switch (mode)
    {
        case 0: return "MONO";
        case 1: return "STEREO";
        case 2: return "WIDE";
        case 3: return "DUAL";
        default: return "STEREO";
    }
}

juce::String STRETRAudioProcessorEditor::getInputText() const
{
    const float db = (float) inputSlider.getValue();
    return formatGainFaderDb (db) + " INPUT";
}
juce::String STRETRAudioProcessorEditor::getInputTextShort() const
{
    const float db = (float) inputSlider.getValue();
    return formatGainFaderDb (db) + " IN";
}

juce::String STRETRAudioProcessorEditor::getOutputText() const
{
    const float db = (float) outputSlider.getValue();
    return formatGainFaderDb (db) + " OUTPUT";
}
juce::String STRETRAudioProcessorEditor::getOutputTextShort() const
{
    const float db = (float) outputSlider.getValue();
    return formatGainFaderDb (db) + " OUT";
}

juce::String STRETRAudioProcessorEditor::getMixText() const
{
    if (mixModeCombo.getSelectedId() == 2)
    {
        const bool isDry = (dualMixBar_.getLastTouched() != DualMixBarComponent::WET);
        const float level = isDry ? dualMixBar_.getDryLevel() : dualMixBar_.getWetLevel();
        const float dB = (level <= 0.0001f) ? -100.0f : 20.0f * std::log10 (level);
        const juce::String suffix = isDry ? " DRY" : " WET";
        if (dB <= -100.0f) return "-INF dB" + suffix;
        if (std::abs (dB) < 0.05f) return "0.0 dB" + suffix;
        return juce::String (dB, 1) + " dB" + suffix;
    }
    const int pct = (int) std::lround (mixSlider.getValue() * 100.0);
    return juce::String (pct) + "% MIX";
}
juce::String STRETRAudioProcessorEditor::getMixTextShort() const
{
    if (mixModeCombo.getSelectedId() == 2)
    {
        const bool isDry = (dualMixBar_.getLastTouched() != DualMixBarComponent::WET);
        const float level = isDry ? dualMixBar_.getDryLevel() : dualMixBar_.getWetLevel();
        const float dB = (level <= 0.0001f) ? -100.0f : 20.0f * std::log10 (level);
        const juce::String suffix = isDry ? " DRY" : " WET";
        if (dB <= -100.0f) return "-INF" + suffix;
        if (std::abs (dB) < 0.05f) return "0.0dB" + suffix;
        return juce::String (dB, 1) + "dB" + suffix;
    }
    const int pct = (int) std::lround (mixSlider.getValue() * 100.0);
    return juce::String (pct) + "% MIX";
}

juce::String STRETRAudioProcessorEditor::getTiltText() const
{
    const float db = (float) tiltSlider.getValue();
    if (std::abs (db) < 0.05f) return "0.0 dB TILT";
    return juce::String (db, 1) + " dB TILT";
}
juce::String STRETRAudioProcessorEditor::getTiltTextShort() const
{
    const float db = (float) tiltSlider.getValue();
    if (std::abs (db) < 0.05f) return "0.0 dB TLT";
    return juce::String (db, 1) + " dB TLT";
}

juce::String STRETRAudioProcessorEditor::getPanText() const
{
    const float v = (float) panSlider.getValue();
    const int pct = juce::roundToInt ((v - 0.5f) * 200.0f);
    if (pct == 0) return "C PAN";
    if (pct < 0) return "L" + juce::String (-pct) + " PAN";
    return "R" + juce::String (pct) + " PAN";
}
juce::String STRETRAudioProcessorEditor::getPanTextShort() const
{
    const float v = (float) panSlider.getValue();
    const int pct = juce::roundToInt ((v - 0.5f) * 200.0f);
    if (pct == 0) return "C";
    if (pct < 0) return "L" + juce::String (-pct);
    return "R" + juce::String (pct);
}

juce::String STRETRAudioProcessorEditor::getLimThresholdText() const
{
    const float db = (float) limThresholdSlider.getValue();
    return juce::String (db, 1) + " dB LIM";
}

juce::String STRETRAudioProcessorEditor::getLimThresholdTextShort() const
{
    const float db = (float) limThresholdSlider.getValue();
    return juce::String (db, 1) + " dB LIM";
}

bool STRETRAudioProcessorEditor::refreshLegendTextCache()
{
    const auto oldAmountFull  = cachedAmountTextFull;
    const auto oldPitchFull     = cachedPitchTextFull;
    const auto oldJitterFull  = cachedJitterTextFull;
    const auto oldGrainFull   = cachedGrainTextFull;
    const auto oldEngineFull  = cachedEngineTextFull;
    const auto oldWindowFull  = cachedWindowTextFull;
    const auto oldStyleFull   = cachedStyleTextFull;
    const auto oldInputFull   = cachedInputTextFull;
    const auto oldOutputFull  = cachedOutputTextFull;
    const auto oldMixFull     = cachedMixTextFull;
    const auto oldTiltFull    = cachedTiltTextFull;
    const auto oldPanFull     = cachedPanTextFull;
    const auto oldLimFull     = cachedLimThresholdTextFull;

    cachedAmountTextFull  = getAmountText();    cachedAmountTextShort  = getAmountTextShort();
    cachedPitchTextFull     = getPitchText();        cachedPitchTextShort     = getPitchTextShort();
    cachedJitterTextFull  = getJitterText();     cachedJitterTextShort  = getJitterTextShort();
    cachedGrainTextFull   = getGrainText();      cachedGrainTextShort   = getGrainTextShort();
    cachedEngineTextFull  = getEngineText();      cachedEngineTextShort  = getEngineTextShort();
    cachedWindowTextFull  = getWindowText();      cachedWindowTextShort  = getWindowTextShort();
    cachedStyleTextFull   = getStyleText();       cachedStyleTextShort   = getStyleTextShort();
    cachedInputTextFull   = getInputText();       cachedInputTextShort   = getInputTextShort();
    cachedOutputTextFull  = getOutputText();      cachedOutputTextShort  = getOutputTextShort();
    cachedMixTextFull     = getMixText();          cachedMixTextShort     = getMixTextShort();
    cachedTiltTextFull    = getTiltText();         cachedTiltTextShort    = getTiltTextShort();
    cachedPanTextFull     = getPanText();          cachedPanTextShort     = getPanTextShort();

    // Int-only representations
    cachedAmountIntOnly  = juce::String ((int) std::lround (amountSlider.getValue())) + "%";
    cachedPitchIntOnly = formatPitchSemitones (pitchSliderToSemitones (pitchSlider.getValue()), 2) + "st";
    cachedJitterIntOnly  = juce::String ((int) std::lround (jitterSlider.getValue())) + "%";
    cachedGrainIntOnly   = (getCurrentEngineValue() == 1)
                           ? juce::String ((int) std::lround (grainSlider.getValue())) + "ms"
                           : juce::String ("GRAIN");
    cachedEngineIntOnly  = getEngineTextShort();
    cachedWindowIntOnly  = juce::String (getEffectiveWindowValue (windowSlider.getValue()));
    cachedStyleIntOnly   = getStyleTextShort();
    cachedInputIntOnly   = formatGainFaderDbCompact ((float) inputSlider.getValue());
    cachedOutputIntOnly  = formatGainFaderDbCompact ((float) outputSlider.getValue());

    if (mixModeCombo.getSelectedId() == 2)
    {
        const bool isDry = (dualMixBar_.getLastTouched() != DualMixBarComponent::WET);
        const float level = isDry ? dualMixBar_.getDryLevel() : dualMixBar_.getWetLevel();
        const float dB = (level <= 0.0001f) ? -100.0f : 20.0f * std::log10 (level);
        const juce::String suffix = isDry ? " DRY" : " WET";
        if (dB <= -100.0f) cachedMixIntOnly = "-INF" + suffix;
        else if (std::abs (dB) < 0.05f) cachedMixIntOnly = "0.0dB" + suffix;
        else cachedMixIntOnly = juce::String (dB, 1) + "dB" + suffix;
    }
    else
    {
        cachedMixIntOnly = juce::String ((int) std::lround (mixSlider.getValue() * 100.0)) + "%";
    }
    {
        const float tiltVal = (float) tiltSlider.getValue();
        cachedTiltIntOnly = (std::abs (tiltVal) < 0.05f) ? "0.0dB" : (juce::String (tiltVal, 1) + "dB");
    }

    cachedFilterTextFull  = "FILTER";
    cachedFilterTextShort = "FLTR";

    cachedPanTextFull  = getPanText();
    cachedPanTextShort = getPanTextShort();

    cachedLimThresholdTextFull  = getLimThresholdText();
    cachedLimThresholdTextShort = getLimThresholdTextShort();
    {
        const float limVal = (float) limThresholdSlider.getValue();
        cachedLimThresholdIntOnly = juce::String (limVal, 1) + "dB";
    }

    {
        const float panVal = (float) panSlider.getValue();
        const int panPct = juce::roundToInt ((panVal - 0.5f) * 200.0f);
        if (panPct == 0)       cachedPanIntOnly = "C";
        else if (panPct < 0)   cachedPanIntOnly = "L" + juce::String (-panPct);
        else                   cachedPanIntOnly = "R" + juce::String (panPct);
    }

    return oldAmountFull != cachedAmountTextFull || oldPitchFull != cachedPitchTextFull
        || oldJitterFull != cachedJitterTextFull || oldGrainFull != cachedGrainTextFull
        || oldEngineFull != cachedEngineTextFull
        || oldWindowFull != cachedWindowTextFull  || oldStyleFull != cachedStyleTextFull
        || oldInputFull != cachedInputTextFull    || oldOutputFull != cachedOutputTextFull
        || oldMixFull != cachedMixTextFull        || oldTiltFull != cachedTiltTextFull
        || oldPanFull != cachedPanTextFull
        || oldLimFull != cachedLimThresholdTextFull;
}

juce::Rectangle<int> STRETRAudioProcessorEditor::getRowRepaintBounds (const juce::Slider& s) const
{
    return s.getBounds().getUnion (getValueAreaFor (s.getBounds())).expanded (8, 8).getIntersection (getLocalBounds());
}

//========================== Layout ==========================

STRETRAudioProcessorEditor::HorizontalLayoutMetrics
STRETRAudioProcessorEditor::buildHorizontalLayout (int editorW, int valueColW)
{
    return TR::buildSimpleHorizontalLayout (editorW, valueColW);
}

STRETRAudioProcessorEditor::VerticalLayoutMetrics
STRETRAudioProcessorEditor::buildVerticalLayout (int editorH, int biasY, bool ioExpanded)
{
    TR::SimpleVerticalLayoutConfig config;
    config.mainRows = 7;
    config.collapsedButtonRows = 2;
    config.collapsedSliderBottomRow = 0;
    config.expandedHasSidechainRow = true;

    return TR::buildSimpleVerticalLayout (editorH, biasY, ioExpanded, config);
}

void STRETRAudioProcessorEditor::updateCachedLayout()
{
    cachedHLayout_ = buildHorizontalLayout (getWidth(), getTargetValueColumnWidth());
    cachedVLayout_ = buildVerticalLayout (getHeight(), kLayoutVerticalBiasPx, ioSectionExpanded_);

    const juce::Slider* sliders[12] = {
        &amountSlider, &pitchSlider, &grainSlider, &engineSlider, &windowSlider, &jitterSlider, &styleSlider,
        &inputSlider, &outputSlider, &tiltSlider, &panSlider, &mixSlider
    };

    for (int i = 0; i < 12; ++i)
    {
        if (sliders[i]->isVisible())
        {
            cachedValueAreas_[(size_t) i] = getValueAreaFor (sliders[i]->getBounds());
        }
        else
        {
            // MIX row (index 11): use dualMixBar_ bounds when SEND mode is active
            if (i == 11 && dualMixBar_.isVisible())
            {
                cachedValueAreas_[11] = getValueAreaFor (dualMixBar_.getBounds());
                continue;
            }
            cachedValueAreas_[(size_t) i] = {};
        }
    }

    if (filterBar_.isVisible())
    {
        const auto& bb = filterBar_.getBounds();
        cachedFilterValueArea_ = TR::makeSimpleValueArea (bb, cachedHLayout_, getWidth());
    }
    else cachedFilterValueArea_ = {};

    if (tiltSlider.isVisible())
    {
        const auto& bb = tiltSlider.getBounds();
        cachedTiltValueArea_ = TR::makeSimpleValueArea (bb, cachedHLayout_, getWidth());
    }
    else cachedTiltValueArea_ = {};

    if (panSlider.isVisible())
    {
        const auto& bb = panSlider.getBounds();
        cachedPanValueArea_ = TR::makeSimpleValueArea (bb, cachedHLayout_, getWidth());
    }
    else cachedPanValueArea_ = {};

    if (limThresholdSlider.isVisible())
    {
        const auto& bb = limThresholdSlider.getBounds();
        cachedLimThresholdValueArea_ = TR::makeSimpleValueArea (bb, cachedHLayout_, getWidth());
    }
    else cachedLimThresholdValueArea_ = {};

    if (chaosFilterButton.isVisible())
        cachedChaosArea_ = chaosFilterButton.getBounds().getUnion (chaosDelayButton.getBounds());
    else cachedChaosArea_ = {};

    cachedToggleBarArea_ = TR::makeSimpleToggleBarArea (cachedHLayout_, cachedVLayout_);
}

int STRETRAudioProcessorEditor::getTargetValueColumnWidth() const
{
    std::uint64_t key = 1469598103934665603ull;
    key ^= (std::uint64_t) getWidth();
    key *= 1099511628211ull;
    if (key == cachedValueColumnWidthKey) return cachedValueColumnWidth;

    const auto& font = kBoldFont40();
    auto maxSW = [&] (const char* a, const char* b, const char* c)
    { return juce::jmax (stringWidth (font, a), juce::jmax (stringWidth (font, b), stringWidth (font, c))); };

    int maxW = maxSW (kAmountLegendFull, kAmountLegendShort, kAmountLegendInt);
    maxW = juce::jmax (maxW, maxSW (kPitchLegendFull,    kPitchLegendShort,    kPitchLegendInt));
    maxW = juce::jmax (maxW, maxSW (kJitterLegendFull, kJitterLegendShort, kJitterLegendInt));
    maxW = juce::jmax (maxW, maxSW (kGrainLegendFull,  kGrainLegendShort,  kGrainLegendInt));
    maxW = juce::jmax (maxW, maxSW (kEngineLegendFull, kEngineLegendShort, kEngineLegendInt));
    maxW = juce::jmax (maxW, maxSW (kWindowLegendFull, kWindowLegendShort, kWindowLegendInt));
    maxW = juce::jmax (maxW, maxSW (kStyleLegendFull,  kStyleLegendShort,  kStyleLegendInt));
    maxW = juce::jmax (maxW, maxSW (kInputLegendFull,  kInputLegendShort,  kInputLegendInt));
    maxW = juce::jmax (maxW, maxSW (kOutputLegendFull, kOutputLegendShort, kOutputLegendInt));
    maxW = juce::jmax (maxW, maxSW (kMixLegendFull,    kMixLegendShort,    kMixLegendInt));
    maxW = juce::jmax (maxW, maxSW (kLimLegendFull,    kLimLegendShort,    kLimLegendInt));

    const int desired = maxW + 16;
    const int minW = 90;
    const int maxAllowed = juce::jmax (minW, (int) std::round (getWidth() * 0.40));
    cachedValueColumnWidth = juce::jlimit (minW, maxAllowed, desired);
    cachedValueColumnWidthKey = key;
    return cachedValueColumnWidth;
}

//========================== Hit areas ==========================

juce::Rectangle<int> STRETRAudioProcessorEditor::getValueAreaFor (const juce::Rectangle<int>& barBounds) const
{
    return TR::makeSimpleValueArea (barBounds, cachedHLayout_, getWidth());
}

juce::Slider* STRETRAudioProcessorEditor::getSliderForValueAreaPoint (juce::Point<int> p)
{
    return TR::findSimpleSliderForValueAreaPoint (p, cachedValueAreas_,
    {
        { 0, &amountSlider }, { 1, &pitchSlider }, { 2, &grainSlider },
        { 3, &engineSlider }, { 4, &windowSlider }, { 5, &jitterSlider },
        { 6, &styleSlider },  { 7, &inputSlider },  { 8, &outputSlider },
        { 9, &tiltSlider },   { 10, &panSlider },   { 11, &mixSlider },
        { &limThresholdSlider, cachedLimThresholdValueArea_ }
    });
}

juce::Rectangle<int> STRETRAudioProcessorEditor::getReverseLabelArea() const
{
    return TR::makeSimpleToggleLabelArea (reverseButton, triggerButton.getX() - TR::kSimpleToggleLegendCollisionPadPx, "REVERSE", "RVS");
}

juce::Rectangle<int> STRETRAudioProcessorEditor::getTriggerLabelArea() const
{
    return TR::makeSimpleToggleLabelArea (triggerButton, getWidth() - TR::kSimpleToggleLegendCollisionPadPx, "ARM", "ARM");
}

juce::Rectangle<int> STRETRAudioProcessorEditor::getAlignLabelArea() const
{
    return TR::makeSimpleToggleLabelArea (alignButton, pdcButton.getX() - TR::kSimpleToggleLegendCollisionPadPx, "ALIGN", "ALN");
}

juce::Rectangle<int> STRETRAudioProcessorEditor::getPdcLabelArea() const
{
    return TR::makeSimpleToggleLabelArea (pdcButton, getWidth() - TR::kSimpleToggleLegendCollisionPadPx, "PDC", "PDC");
}

juce::Rectangle<int> STRETRAudioProcessorEditor::getChaosLabelArea() const
{
    if (chaosFilterButton.getWidth() <= 0 || chaosFilterButton.getHeight() <= 0) return {};
    return TR::makeSimpleToggleLabelArea (chaosFilterButton,
                                          chaosDelayButton.getX() - TR::kSimpleToggleLegendCollisionPadPx,
                                          "CHSF", "CHSF");
}

juce::Rectangle<int> STRETRAudioProcessorEditor::getChaosDelayLabelArea() const
{
    if (chaosDelayButton.getWidth() <= 0 || chaosDelayButton.getHeight() <= 0) return {};
    return TR::makeSimpleToggleLabelArea (chaosDelayButton,
                                          getWidth() - TR::kSimpleToggleLegendCollisionPadPx,
                                          "CHSD", "CHSD");
}
juce::Rectangle<int> STRETRAudioProcessorEditor::getSidechainLabelArea() const
{
    if (sidechainButton.getWidth() <= 0 || sidechainButton.getHeight() <= 0) return {};
    return TR::makeSimpleToggleLabelArea (sidechainButton, getWidth() - TR::kSimpleToggleLegendCollisionPadPx, "SIDECHAIN", "SC");
}
juce::Rectangle<int> STRETRAudioProcessorEditor::getInfoIconArea() const
{
    int contentRight = 0;
    for (size_t i = 0; i < cachedValueAreas_.size(); ++i)
        if (! cachedValueAreas_[i].isEmpty()) { contentRight = cachedValueAreas_[i].getRight(); break; }
    if (contentRight <= 0) contentRight = getWidth() - 8;

    const int titleH = cachedVLayout_.titleH;
    const int titleY = cachedVLayout_.titleTopPad;
    const int titleAreaH = cachedVLayout_.titleAreaH;
    const int size = juce::jlimit (20, 36, titleH);
    return { contentRight - size, titleY + juce::jmax (0, (titleAreaH - size) / 2), size, size };
}

void STRETRAudioProcessorEditor::updateInfoIconCache()
{
    const auto iconArea = getInfoIconArea();
    const auto iconF = iconArea.toFloat();
    const auto center = iconF.getCentre();
    const float toothTipR = (float) iconArea.getWidth() * 0.47f;
    const float toothRootR = toothTipR * 0.78f;
    const float holeR = toothTipR * 0.40f;
    constexpr int teeth = 8;
    cachedInfoGearPath.clear();
    for (int i = 0; i < teeth * 2; ++i)
    {
        const float a = -juce::MathConstants<float>::halfPi
                      + (juce::MathConstants<float>::pi * (float) i / (float) teeth);
        const float r = (i % 2 == 0) ? toothTipR : toothRootR;
        const float x = center.x + std::cos (a) * r;
        const float y = center.y + std::sin (a) * r;
        if (i == 0) cachedInfoGearPath.startNewSubPath (x, y);
        else cachedInfoGearPath.lineTo (x, y);
    }
    cachedInfoGearPath.closeSubPath();
    cachedInfoGearHole = { center.x - holeR, center.y - holeR, holeR * 2.0f, holeR * 2.0f };
}

//========================== Mouse handlers ==========================

void STRETRAudioProcessorEditor::mouseDown (const juce::MouseEvent& e)
{
    lastUserInteractionMs.store (juce::Time::getMillisecondCounter(), std::memory_order_relaxed);
    const auto p = e.getEventRelativeTo (this).getPosition();

    const bool handled = TR::SimpleMouseRouter::routeMouseDown (*this, e, p,
        cachedToggleBarArea_, ioSectionExpanded_,
        amountSlider, {},
        [] { return false; },
        [] (bool) {},
        [] (bool) { return juce::String(); },
        [this] { audioProcessor.setUiIoExpanded (ioSectionExpanded_); },
        filterBar_, cachedFilterValueArea_,
        [this] { openFilterPrompt(); },
        [this] (juce::Point<int> pt) { return getSliderForValueAreaPoint (pt); },
        [this] (juce::Slider& slider) { openNumericEntryPopupForSlider (slider); },
        getInfoIconArea(), crtEnabled,
        [this] { openInfoPopup(); },
        {
            { &reverseButton,     getReverseLabelArea(),     nullptr,              nullptr,                         true },
            { &triggerButton,     getTriggerLabelArea(),     &triggerDisplay,      [this] { openTriggerDelayPrompt(); }, true },
            { &alignButton,       getAlignLabelArea(),       nullptr,              nullptr,                         true },
            { &pdcButton,         getPdcLabelArea(),         &pdcDisplay,          [this] { openPdcMaxWindowPrompt(); updatePdcTooltip(); }, true },
            { &chaosFilterButton, getChaosLabelArea(),       &chaosFilterDisplay,  [this] { openChaosFilterPrompt(); }, true },
            { &chaosDelayButton,  getChaosDelayLabelArea(),  &chaosDelayDisplay,   [this] { openChaosDelayPrompt(); }, true },
            { &sidechainButton,   getSidechainLabelArea(),   &sidechainDisplay,    [this] { openSidechainPrompt(); }, true }
        });

    if (handled)
    {
        audioProcessor.setUiIoExpanded (ioSectionExpanded_);
        return;
    }
}

void STRETRAudioProcessorEditor::mouseDrag (const juce::MouseEvent& e)
{
    juce::ignoreUnused (e);
    lastUserInteractionMs.store (juce::Time::getMillisecondCounter(), std::memory_order_relaxed);
}

void STRETRAudioProcessorEditor::mouseMove (const juce::MouseEvent& e)
{
    juce::ignoreUnused (e);
    TR::clearSimpleHoverTooltip (*this, tooltipWindow.get());
}

void STRETRAudioProcessorEditor::mouseExit (const juce::MouseEvent& e)
{
    juce::ignoreUnused (e);
    TR::clearSimpleHoverTooltip (*this, tooltipWindow.get());
}

void STRETRAudioProcessorEditor::mouseDoubleClick (const juce::MouseEvent& e)
{
    TR::SimpleMouseRouter::routeMouseDoubleClick (*this, e.getPosition(),
        [this] (juce::Point<int> pt) { return getSliderForValueAreaPoint (pt); },
        {
            { &amountSlider,       kDefaultAmount },
            { &pitchSlider,        0.5 },
            { &jitterSlider,       (double) STRETRAudioProcessor::kJitterDefault },
            { &grainSlider,        (double) STRETRAudioProcessor::kGrainDefault },
            { &inputSlider,        kDefaultInput },
            { &outputSlider,       kDefaultOutput },
            { &mixSlider,          kDefaultMix },
            { &limThresholdSlider, kDefaultLimThreshold }
        });
}

//========================== Paint ==========================

TR::SimpleMainPanelSpec STRETRAudioProcessorEditor::buildMainPanelSpec()
{
    TR::SimpleMainPanelSpec spec;
    spec.title = "STRE-TR";
    spec.version = juce::String ("v") + InfoContent::version;
    spec.ioExpanded = ioSectionExpanded_;
    spec.toggleBarArea = cachedToggleBarArea_;

    {
        const juce::String* full[12] = {
            &cachedAmountTextFull, &cachedPitchTextFull, &cachedGrainTextFull, &cachedEngineTextFull,
            &cachedWindowTextFull, &cachedJitterTextFull, &cachedStyleTextFull, &cachedInputTextFull,
            &cachedOutputTextFull, &cachedTiltTextFull, &cachedPanTextFull, &cachedMixTextFull
        };
        const juce::String* shrt[12] = {
            &cachedAmountTextShort, &cachedPitchTextShort, &cachedGrainTextShort, &cachedEngineTextShort,
            &cachedWindowTextShort, &cachedJitterTextShort, &cachedStyleTextShort, &cachedInputTextShort,
            &cachedOutputTextShort, &cachedTiltTextShort, &cachedPanTextShort, &cachedMixTextShort
        };
        const juce::String* intOnly[12] = {
            &cachedAmountIntOnly, &cachedPitchIntOnly, &cachedGrainIntOnly, &cachedEngineIntOnly,
            &cachedWindowIntOnly, &cachedJitterIntOnly, &cachedStyleIntOnly, &cachedInputIntOnly,
            &cachedOutputIntOnly, &cachedTiltIntOnly, &cachedPanIntOnly, &cachedMixIntOnly
        };

        for (int i = 0; i < 12; ++i)
            TR::addSimpleMainPanelRow (spec, false, full[i], shrt[i], intOnly[i],
                                       cachedValueAreas_[(size_t) i]);
    }

    {
        auto addIfVisible = [&] (const juce::Slider& s, const juce::Rectangle<int>& area,
                                 const juce::String* full, const juce::String* shrt,
                                 const juce::String* intOnly = nullptr)
        {
            TR::addSimpleMainPanelRow (spec, true, full, shrt, intOnly, area, s.isVisible());
        };

        addIfVisible (tiltSlider, cachedTiltValueArea_, &cachedTiltTextFull, &cachedTiltTextShort, &cachedTiltIntOnly);
        TR::addSimpleMainPanelRow (spec, true, &cachedFilterTextFull, &cachedFilterTextShort, nullptr,
                                   cachedFilterValueArea_, filterBar_.isVisible());
        addIfVisible (panSlider, cachedPanValueArea_, &cachedPanTextFull, &cachedPanTextShort, &cachedPanIntOnly);
        addIfVisible (limThresholdSlider, cachedLimThresholdValueArea_, &cachedLimThresholdTextFull, &cachedLimThresholdTextShort, &cachedLimThresholdIntOnly);
    }

    spec.combosVisible = modeInCombo.isVisible();
    spec.comboLabels = {
        { &modeInCombo, "MODE IN", "IN" },
        { &modeOutCombo, "MODE OUT", "OUT" },
        { &sumBusCombo, "SUM BUS", "SUM" },
        { &limModeCombo, "LIMIT", "LIM" },
        { &mixModeCombo, "MIX", "MIX" },
        { &filterPosCombo, "F / T", "F/T" },
        { &invPolCombo, "INV POL", "POL" },
        { &invStrCombo, "INV STR", "STR" }
    };

    const int W = getWidth();
    TR::addSimpleMainPanelToggle (spec, false, chaosFilterButton, getChaosLabelArea(), "CHSF", "CHSF",
                                  TR::makeSimpleMainPanelRightBoundBefore (chaosDelayButton, W));
    TR::addSimpleMainPanelToggle (spec, false, chaosDelayButton, getChaosDelayLabelArea(), "CHSD", "CHSD",
                                  TR::makeSimpleMainPanelRightBound (W));
    TR::addSimpleMainPanelToggle (spec, false, sidechainButton, getSidechainLabelArea(), "SIDECHAIN", "SC",
                                  TR::makeSimpleMainPanelRightBound (W));

    if (! ioSectionExpanded_)
    {
        TR::addSimpleMainPanelToggle (spec, true, alignButton, getAlignLabelArea(), "ALIGN", "ALN",
                                      TR::makeSimpleMainPanelRightBoundBefore (pdcButton, W));
        TR::addSimpleMainPanelToggle (spec, true, pdcButton, getPdcLabelArea(), "PDC", "PDC",
                                      TR::makeSimpleMainPanelRightBound (W));
        TR::addSimpleMainPanelToggle (spec, true, reverseButton, getReverseLabelArea(), "REVERSE", "RVS",
                                      TR::makeSimpleMainPanelRightBoundBefore (triggerButton, W),
                                      true, reverseButton.isEnabled());
        TR::addSimpleMainPanelToggle (spec, true, triggerButton, getTriggerLabelArea(), "ARM", "ARM",
                                      TR::makeSimpleMainPanelRightBound (W));
    }

    if (cachedInfoGearPath.isEmpty())
        updateInfoIconCache();
    TR::setSimpleMainPanelInfoGear (spec, cachedInfoGearPath, cachedInfoGearHole);

    return spec;
}


void STRETRAudioProcessorEditor::paint (juce::Graphics& g)
{
    TR::SimpleMainPanelRenderer::paint (g, buildMainPanelSpec(), activeScheme, kBoldFont40(), getWidth());
}

void STRETRAudioProcessorEditor::paintOverChildren (juce::Graphics& g)
{
    juce::ignoreUnused (g);
}

//========================== Resized ==========================

void STRETRAudioProcessorEditor::resized()
{
    refreshLegendTextCache();

    if (! suppressSizePersistence)
    {
        if (juce::ModifierKeys::getCurrentModifiers().isAnyMouseButtonDown()
            || juce::Desktop::getInstance().getMainMouseSource().isDragging())
            lastUserInteractionMs.store (juce::Time::getMillisecondCounter(), std::memory_order_relaxed);
    }

    const int W = getWidth();
    const int H = getHeight();

    if (! suppressSizePersistence)
    {
        const uint32_t last = lastUserInteractionMs.load (std::memory_order_relaxed);
        const uint32_t now = juce::Time::getMillisecondCounter();
        const bool userRecent = (now - last) <= (uint32_t) kUserInteractionPersistWindowMs;
        if ((W != lastPersistedEditorW || H != lastPersistedEditorH) && userRecent)
        {
            audioProcessor.setUiEditorSize (W, H);
            lastPersistedEditorW = W;
            lastPersistedEditorH = H;
        }
    }

    const auto horizontalLayout = buildHorizontalLayout (W, getTargetValueColumnWidth());
    const auto verticalLayout = buildVerticalLayout (H, kLayoutVerticalBiasPx, ioSectionExpanded_);

    if (ioSectionExpanded_)
    {
        TR::placeSimpleRowComponent (inputSlider, horizontalLayout, verticalLayout, 0);
        TR::placeSimpleRowComponent (outputSlider, horizontalLayout, verticalLayout, 1);
        TR::placeSimpleRowComponent (tiltSlider, horizontalLayout, verticalLayout, 2);
        TR::placeSimpleRowComponent (filterBar_, horizontalLayout, verticalLayout, 3);
        TR::placeSimpleRowComponent (panSlider, horizontalLayout, verticalLayout, 4);
        TR::placeSimpleRowComponent (mixSlider, horizontalLayout, verticalLayout, 5);
        TR::placeSimpleRowComponent (dualMixBar_, horizontalLayout, verticalLayout, 5);
        TR::placeSimpleRowComponent (limThresholdSlider, horizontalLayout, verticalLayout, 6);

        // Match the compact-menu combo block used by DISP/FREQ/ECHO/GRA.
        {
            const int labelOffset = 19;
            const int comboH = juce::jlimit (TR::kSimpleIoComboMinButtonH, 44,
                                             (verticalLayout.sidechainRowY - limThresholdSlider.getBottom()
                                              - verticalLayout.gapY * 2 - TR::kSimpleIoComboRowGapPx) / 2);
            const int comboBlockH = labelOffset + comboH + TR::kSimpleIoComboRowGapPx + labelOffset + comboH;
            const int blockTopLimit = limThresholdSlider.getBottom() + verticalLayout.gapY;
            const int blockBottomLimit = verticalLayout.sidechainRowY - verticalLayout.gapY;
            const int availableBlockH = juce::jmax (comboBlockH, blockBottomLimit - blockTopLimit);
            const int visualTop = blockTopLimit + juce::jmax (0, (availableBlockH - comboBlockH) / 2);
            TR::placeSimpleIoComboGrid (horizontalLayout, verticalLayout,
                                         visualTop + labelOffset,
                                         visualTop + comboBlockH,
                                         modeInCombo, modeOutCombo, sumBusCombo, limModeCombo,
                                         mixModeCombo, filterPosCombo, invPolCombo, invStrCombo);
        }

        const int chaosY = verticalLayout.chaosRowY;
        TR::placeSimpleWideTogglePair (chaosFilterButton, chaosDelayButton, horizontalLayout, verticalLayout, chaosY);
        TR::placeSimpleDisplayLabel (chaosFilterDisplay, getChaosLabelArea());
        TR::placeSimpleDisplayLabel (chaosDelayDisplay, getChaosDelayLabelArea());

        TR::placeSimpleToggleAt (sidechainButton, horizontalLayout, verticalLayout, false, verticalLayout.sidechainRowY);
        TR::placeSimpleDisplayLabel (sidechainDisplay, getSidechainLabelArea());

        TR::setSimpleComponentVisible (inputSlider, true);
        TR::setSimpleComponentVisible (outputSlider, true);
        TR::setSimpleComponentVisible (tiltSlider, true);
        TR::setSimpleComponentVisible (filterBar_, true);
        TR::setSimpleComponentVisible (panSlider, true);
        TR::setSimpleComponentVisible (mixSlider, true);
        TR::setSimpleComponentVisible (limThresholdSlider, true);
        TR::setSimpleComponentVisible (modeInCombo, true);
        TR::setSimpleComponentVisible (modeOutCombo, true);
        TR::setSimpleComponentVisible (sumBusCombo, true);
        TR::setSimpleComponentVisible (limModeCombo, true);
        TR::setSimpleComponentVisible (invPolCombo, true);
        TR::setSimpleComponentVisible (invStrCombo, true);
        TR::setSimpleComponentVisible (mixModeCombo, true);
        TR::setSimpleComponentVisible (filterPosCombo, true);
        TR::setSimpleComponentVisible (chaosFilterButton, true);
        TR::setSimpleComponentVisible (chaosFilterDisplay, true);
        TR::setSimpleComponentVisible (chaosDelayButton, true);
        TR::setSimpleComponentVisible (chaosDelayDisplay, true);
        TR::setSimpleComponentVisible (sidechainButton, true);
        TR::setSimpleComponentVisible (sidechainDisplay, true);

        {
            const bool isSendMode = mixModeCombo.getSelectedId() == 2;
            TR::setSimpleComponentVisible (mixSlider, ! isSendMode);
            TR::setSimpleComponentVisible (dualMixBar_, isSendMode);
        }

        TR::setSimpleComponentVisible (reverseButton, false);
        TR::setSimpleComponentVisible (triggerButton, false);
        TR::setSimpleComponentVisible (triggerDisplay, false);
        TR::setSimpleComponentVisible (alignButton, false);
        TR::setSimpleComponentVisible (pdcButton, false);
        TR::setSimpleComponentVisible (pdcDisplay, false);

        TR::setSimpleComponentVisible (amountSlider, false);
        TR::setSimpleComponentVisible (pitchSlider, false);
        TR::setSimpleComponentVisible (grainSlider, false);
        TR::setSimpleComponentVisible (engineSlider, false);
        TR::setSimpleComponentVisible (windowSlider, false);
        TR::setSimpleComponentVisible (jitterSlider, false);
        TR::setSimpleComponentVisible (styleSlider, false);
    }
    else
    {
        TR::placeSimpleRowComponent (amountSlider, horizontalLayout, verticalLayout, 0);
        TR::placeSimpleRowComponent (pitchSlider, horizontalLayout, verticalLayout, 1);
        TR::placeSimpleRowComponent (grainSlider, horizontalLayout, verticalLayout, 2);
        TR::placeSimpleRowComponent (engineSlider, horizontalLayout, verticalLayout, 3);
        TR::placeSimpleRowComponent (windowSlider, horizontalLayout, verticalLayout, 4);
        TR::placeSimpleRowComponent (jitterSlider, horizontalLayout, verticalLayout, 5);
        TR::placeSimpleRowComponent (styleSlider, horizontalLayout, verticalLayout, 6);

        TR::setSimpleComponentVisible (amountSlider, true);
        TR::setSimpleComponentVisible (pitchSlider, true);
        TR::setSimpleComponentVisible (grainSlider, true);
        TR::setSimpleComponentVisible (engineSlider, true);
        TR::setSimpleComponentVisible (windowSlider, true);
        TR::setSimpleComponentVisible (jitterSlider, true);
        TR::setSimpleComponentVisible (styleSlider, true);

        TR::setSimpleComponentVisible (inputSlider, false);
        TR::setSimpleComponentVisible (outputSlider, false);
        TR::setSimpleComponentVisible (tiltSlider, false);
        TR::setSimpleComponentVisible (mixSlider, false);
        TR::setSimpleComponentVisible (dualMixBar_, false);
        TR::setSimpleComponentVisible (panSlider, false);
        TR::setSimpleComponentVisible (filterBar_, false);
        TR::setSimpleComponentVisible (limThresholdSlider, false);
        TR::setSimpleComponentVisible (chaosFilterButton, false);
        TR::setSimpleComponentVisible (chaosFilterDisplay, false);
        TR::setSimpleComponentVisible (chaosDelayButton, false);
        TR::setSimpleComponentVisible (chaosDelayDisplay, false);
        TR::setSimpleComponentVisible (sidechainButton, false);
        TR::setSimpleComponentVisible (sidechainDisplay, false);
        TR::setSimpleComponentVisible (modeInCombo, false);
        TR::setSimpleComponentVisible (modeOutCombo, false);
        TR::setSimpleComponentVisible (sumBusCombo, false);
        TR::setSimpleComponentVisible (limModeCombo, false);
        TR::setSimpleComponentVisible (invPolCombo, false);
        TR::setSimpleComponentVisible (invStrCombo, false);
        TR::setSimpleComponentVisible (mixModeCombo, false);
        TR::setSimpleComponentVisible (filterPosCombo, false);

        TR::setSimpleComponentVisible (reverseButton, true);
        TR::setSimpleComponentVisible (triggerButton, true);
        TR::setSimpleComponentVisible (triggerDisplay, true);
        TR::setSimpleComponentVisible (alignButton, true);
        TR::setSimpleComponentVisible (pdcButton, true);
        TR::setSimpleComponentVisible (pdcDisplay, true);
    }

    // Button rows
    const int btnRow1Y = verticalLayout.btnRow1Y;
    const int btnRow2Y = verticalLayout.btnRow2Y;

    TR::placeSimpleToggleAt (alignButton, horizontalLayout, verticalLayout, false, btnRow1Y);
    TR::placeSimpleToggleAt (pdcButton, horizontalLayout, verticalLayout, true, btnRow1Y);
    TR::placeSimpleToggleAt (reverseButton, horizontalLayout, verticalLayout, false, btnRow2Y);
    TR::placeSimpleToggleAt (triggerButton, horizontalLayout, verticalLayout, true, btnRow2Y);
    TR::placeSimpleDisplayLabel (triggerDisplay, getTriggerLabelArea());
    TR::placeSimpleDisplayLabel (pdcDisplay, getPdcLabelArea());

    if (resizerCorner)
        resizerCorner->setBounds (W - kResizerCornerPx, H - kResizerCornerPx, kResizerCornerPx, kResizerCornerPx);

    promptOverlay.setBounds (getLocalBounds());
    if (promptOverlayActive) promptOverlay.toFront (false);

    updateCachedLayout();
    updateInfoIconCache();
    crtEffect.setResolution (static_cast<float> (W), static_cast<float> (H));
}

//========================== Prompts ==========================

void STRETRAudioProcessorEditor::openNumericEntryPopupForSlider (juce::Slider& s)
{
    if (&s == &engineSlider || &s == &windowSlider || &s == &styleSlider)
        return;

    lnf.setScheme (activeScheme);

    TR::NumericEntryPromptSpec spec;
    if (&s == &amountSlider)       { spec.label = "AMOUNT"; spec.unit = "%";  spec.suffix = " % AMT";    spec.suffixShort = " % AMT"; }
    else if (&s == &pitchSlider)   { spec.label = "PITCH";  spec.unit = "st"; spec.suffix = " st PITCH"; spec.suffixShort = " st PCH"; }
    else if (&s == &jitterSlider)  { spec.label = "JITTER"; spec.unit = "%";  spec.suffix = " % JITTER"; spec.suffixShort = " % JIT"; }
    else if (&s == &grainSlider)   { spec.label = "GRAIN";  spec.unit = "ms"; spec.suffix = " ms";       spec.suffixShort = " ms"; }
    else if (&s == &inputSlider)   { spec.label = "INPUT";  spec.unit = "dB"; spec.suffix = " dB INPUT"; spec.suffixShort = " dB IN"; }
    else if (&s == &outputSlider)  { spec.label = "OUTPUT"; spec.unit = "dB"; spec.suffix = " dB OUTPUT"; spec.suffixShort = " dB OUT"; }
    else if (&s == &mixSlider)     { spec.label = "MIX";    spec.unit = "%";  spec.suffix = " % MIX";    spec.suffixShort = " % MIX"; }
    else if (&s == &panSlider)     { spec.label = "PAN";    spec.unit = "%";  spec.suffix = " % PAN";    spec.suffixShort = " % PAN"; }
    else if (&s == &tiltSlider)    { spec.label = "TILT";   spec.unit = "dB"; spec.suffix = " dB TILT";  spec.suffixShort = " dB TILT"; }
    else if (&s == &limThresholdSlider) { spec.label = "LIMIT"; spec.unit = "dB"; spec.suffix = " dB LIM"; spec.suffixShort = " dB LIM"; }

    if (&s == &pitchSlider)
        spec.currentDisplay = formatPitchSemitones (pitchSliderToSemitones (s.getValue()), 2);
    else if (&s == &panSlider || &s == &mixSlider)
        spec.currentDisplay = juce::String (juce::jlimit (0.0, 100.0, s.getValue() * 100.0), &s == &panSlider ? 0 : 2);
    else if (&s == &grainSlider)
        spec.currentDisplay = juce::String (s.getValue(), 3);
    else
        spec.currentDisplay = s.getTextFromValue (s.getValue());

    if (&s == &amountSlider)       { spec.minValue = STRETRAudioProcessor::kAmountMin; spec.maxValue = STRETRAudioProcessor::kAmountMax; spec.maxDecimals = 2; spec.maxLength = 6; spec.worstCaseText = "100.00"; }
    else if (&s == &pitchSlider)   { spec.minValue = -24.0; spec.maxValue = 24.0; spec.maxDecimals = 2; spec.maxLength = 6; spec.worstCaseText = "+24.00"; }
    else if (&s == &jitterSlider)  { spec.minValue = STRETRAudioProcessor::kJitterMin; spec.maxValue = STRETRAudioProcessor::kJitterMax; spec.maxDecimals = 2; spec.maxLength = 6; spec.worstCaseText = "100.00"; }
    else if (&s == &grainSlider)   { spec.minValue = STRETRAudioProcessor::kGrainMin; spec.maxValue = STRETRAudioProcessor::kGrainMax; spec.maxDecimals = 3; spec.maxLength = 7; spec.worstCaseText = "500.000"; }
    else if (&s == &inputSlider || &s == &outputSlider) { spec.minValue = STRETRAudioProcessor::kGainFloorDb; spec.maxValue = STRETRAudioProcessor::kGainMaxDb; spec.maxDecimals = 1; spec.maxLength = 6; spec.worstCaseText = "-144.0"; }
    else if (&s == &mixSlider)     { spec.minValue = 0.0; spec.maxValue = 100.0; spec.maxDecimals = 2; spec.maxLength = 6; spec.worstCaseText = "100.00"; }
    else if (&s == &panSlider)     { spec.minValue = 0.0; spec.maxValue = 100.0; spec.maxDecimals = 0; spec.maxLength = 3; spec.worstCaseText = "100"; }
    else if (&s == &tiltSlider)    { spec.minValue = STRETRAudioProcessor::kTiltMin; spec.maxValue = STRETRAudioProcessor::kTiltMax; spec.maxDecimals = 1; spec.maxLength = 4; spec.worstCaseText = "-6.0"; }
    else if (&s == &limThresholdSlider) { spec.minValue = STRETRAudioProcessor::kLimThresholdMin; spec.maxValue = STRETRAudioProcessor::kLimThresholdMax; spec.maxDecimals = 1; spec.maxLength = 5; spec.worstCaseText = "-36.0"; }

    juce::Component::SafePointer<STRETRAudioProcessorEditor> safeThis (this);
    juce::Slider* sliderPtr = &s;
    spec.onAccept = [safeThis, sliderPtr] (const juce::String& txt)
    {
        if (safeThis == nullptr || sliderPtr == nullptr)
            return;

        auto normalised = txt.replaceCharacter (',', '.').trimStart();
        while (normalised.startsWithChar ('+')) normalised = normalised.substring (1).trimStart();
        double val = normalised.initialSectionContainingOnly ("0123456789.,-").getDoubleValue();

        if (sliderPtr == &safeThis->pitchSlider)
            val = semitonesToPitchSlider (std::round (val * 100.0) / 100.0);
        else if (sliderPtr == &safeThis->panSlider || sliderPtr == &safeThis->mixSlider)
            val = juce::jlimit (0.0, 1.0, val / 100.0);

        const auto range = sliderPtr->getRange();
        sliderPtr->setValue (juce::jlimit (range.getStart(), range.getEnd(), val),
                             juce::sendNotificationSync);
    };

    TR::openNumericEntryPopupShared (this, lnf, activeScheme, spec);
}


void STRETRAudioProcessorEditor::openPdcMaxWindowPrompt()
{
    lnf.setScheme (activeScheme);

    const int currentMaxWindow = getCurrentMaxFftWindow();
    const bool activeFft = isCurrentEngineFft();
    const int currentPluginWindow = getEffectiveWindowValue (windowSlider.getValue());

    auto windowToBar = [] (int window) noexcept
    {
        const int lane = STRETRAudioProcessor::getFftWindowLane (window);
        return (float) lane / (float) (STRETRAudioProcessor::kNumFftWindows - 1);
    };

    auto barToWindow = [] (float value01) noexcept
    {
        const int lane = juce::jlimit (0, STRETRAudioProcessor::kNumFftWindows - 1,
            (int) std::lround (juce::jlimit (0.0f, 1.0f, value01)
                * (float) (STRETRAudioProcessor::kNumFftWindows - 1)));
        return STRETRAudioProcessor::kFftWindows[lane];
    };

    auto applyMaxWindow = [this] (int window)
    {
        const int canonical = STRETRAudioProcessor::getCanonicalFftWindow (window);
        if (auto* p = audioProcessor.apvts.getParameter (STRETRAudioProcessor::kParamMaxWindow))
            p->setValueNotifyingHost (p->convertTo0to1 ((float) canonical));
        syncFftWindowToMax (true);
        updatePdcTooltip();
    };

    TR::openIntegerBarPromptShared<STRETRAudioProcessorEditor> (
        this, lnf, activeScheme, "maxwin", "MAX WIN", {},
        currentMaxWindow, STRETRAudioProcessor::kFftMaxWindowDefault,
        0, STRETRAudioProcessor::kWindowMax, 4,
        windowToBar,
        barToWindow,
        [] (int value) { return STRETRAudioProcessor::getCanonicalFftWindow (value); },
        applyMaxWindow,
        [this, activeFft, currentMaxWindow, currentPluginWindow]
        {
            if (auto* p = audioProcessor.apvts.getParameter (STRETRAudioProcessor::kParamMaxWindow))
                p->setValueNotifyingHost (p->convertTo0to1 ((float) currentMaxWindow));
            if (activeFft)
            {
                audioProcessor.setStoredWindowForEngine (getCurrentEngineValue(), currentPluginWindow);
                syncFftWindowToMax (true);
            }
            updatePdcTooltip();
        },
        [applyMaxWindow] (int value) { applyMaxWindow (value); });
}

void STRETRAudioProcessorEditor::openTriggerDelayPrompt()
{
    lnf.setScheme (activeScheme);
    const int delayMs = audioProcessor.getTriggerDelayMs();

    auto applyLiveDelay = [this] (int newDelayMs)
    {
        const int clamped = juce::jlimit (0, 100, newDelayMs);
        audioProcessor.setTriggerDelayMs (clamped);
        triggerDisplay.setTooltip (formatTriggerDelayTooltip (clamped));
    };

    TR::openSimpleDelayMsPromptAction<STRETRAudioProcessorEditor> (
        this, lnf, activeScheme, delayMs, 100,
        applyLiveDelay,
        [this, delayMs]
        {
            audioProcessor.setTriggerDelayMs (delayMs);
            triggerDisplay.setTooltip (formatTriggerDelayTooltip (delayMs));
        },
        [applyLiveDelay] (int value) { applyLiveDelay (value); });
}

void STRETRAudioProcessorEditor::openMixSendPrompt()
{
    TR::openMixSendPromptShared<STRETRAudioProcessorEditor> (this,
                                          lnf,
                                          activeScheme,
                                          audioProcessor.apvts,
                                          STRETRAudioProcessor::kParamDryLevel,
                                          STRETRAudioProcessor::kParamWetLevel,
                                          STRETRAudioProcessor::kDryLevelDefault,
                                          STRETRAudioProcessor::kWetLevelDefault,
                                          [this]() { dualMixBar_.updateFromProcessor(); });
}

void STRETRAudioProcessorEditor::openFilterPrompt()
{
    lnf.setScheme (activeScheme);
    auto& vts = audioProcessor.apvts;

    FilterPromptSpec spec;
    spec.hpParam = STRETRAudioProcessor::kParamFilterHpFreq;
    spec.lpParam = STRETRAudioProcessor::kParamFilterLpFreq;
    spec.hpOnParam = STRETRAudioProcessor::kParamFilterHpOn;
    spec.lpOnParam = STRETRAudioProcessor::kParamFilterLpOn;
    spec.hpSlopeParam = STRETRAudioProcessor::kParamFilterHpSlope;
    spec.lpSlopeParam = STRETRAudioProcessor::kParamFilterLpSlope;
    spec.freqMin = STRETRAudioProcessor::kFilterFreqMin;
    spec.freqMax = STRETRAudioProcessor::kFilterFreqMax;
    spec.hpDefault = STRETRAudioProcessor::kFilterHpFreqDefault;
    spec.lpDefault = STRETRAudioProcessor::kFilterLpFreqDefault;
    spec.slopeMin = STRETRAudioProcessor::kFilterSlopeMin;
    spec.slopeMax = STRETRAudioProcessor::kFilterSlopeMax;
    spec.refreshFilterDisplay = [this] { filterBar_.updateFromProcessor(); };

    openFilterPromptShared (this, lnf, activeScheme, vts, spec);
}

void STRETRAudioProcessorEditor::openChaosConfigPrompt (const char* amtParamId, const char* spdParamId,
                                                        const juce::String& title)
{
    auto& vts = audioProcessor.apvts;
    const bool isFilterChaos = title == "CHSF";
    const TR::SimpleChaosPromptBinding binding {
        amtParamId,
        spdParamId,
        vts.getRawParameterValue (amtParamId)->load(),
        vts.getRawParameterValue (spdParamId)->load()
    };

    TR::openSimpleChaosPromptAction<STRETRAudioProcessorEditor> (this,
                                            lnf,
                                            activeScheme,
                                            vts,
                                            binding,
                                            [this, isFilterChaos, amtParamId, spdParamId]
                                            {
                                                const auto amt = audioProcessor.apvts.getRawParameterValue (amtParamId)->load();
                                                const auto spd = audioProcessor.apvts.getRawParameterValue (spdParamId)->load();
                                                const auto tip = formatChaosTooltip (amt, spd);
                                                if (isFilterChaos)
                                                    chaosFilterDisplay.setTooltip (tip);
                                                else
                                                    chaosDelayDisplay.setTooltip (tip);
                                                repaint();
                                            });
}

void STRETRAudioProcessorEditor::openChaosFilterPrompt()
{
    TR::openSimpleChaosSelectorPromptAction (
        [this] (const char* amountParamId, const char* speedParamId, const juce::String& title)
        {
            openChaosConfigPrompt (amountParamId, speedParamId, title);
        },
        STRETRAudioProcessor::kParamChaosAmtFilter,
        STRETRAudioProcessor::kParamChaosSpdFilter,
        true);
}

void STRETRAudioProcessorEditor::openChaosDelayPrompt()
{
    TR::openSimpleChaosSelectorPromptAction (
        [this] (const char* amountParamId, const char* speedParamId, const juce::String& title)
        {
            openChaosConfigPrompt (amountParamId, speedParamId, title);
        },
        STRETRAudioProcessor::kParamChaosAmt,
        STRETRAudioProcessor::kParamChaosSpd,
        false);
}

void STRETRAudioProcessorEditor::openSidechainPrompt()
{
    lnf.setScheme (activeScheme);
    auto& vts = audioProcessor.apvts;

    SidechainPromptSpec spec;
    spec.gainParam = STRETRAudioProcessor::kParamSidechainGain;
    spec.smoothParam = STRETRAudioProcessor::kParamSidechainSmooth;
    spec.polParam = STRETRAudioProcessor::kParamSidechainPol;
    spec.hpParam = STRETRAudioProcessor::kParamSidechainHp;
    spec.lpParam = STRETRAudioProcessor::kParamSidechainLp;
    spec.hpOnParam = STRETRAudioProcessor::kParamSidechainHpOn;
    spec.lpOnParam = STRETRAudioProcessor::kParamSidechainLpOn;
    spec.hpSlopeParam = STRETRAudioProcessor::kParamSidechainHpSlope;
    spec.lpSlopeParam = STRETRAudioProcessor::kParamSidechainLpSlope;
    spec.gainMin = STRETRAudioProcessor::kSidechainGainMin;
    spec.gainMax = STRETRAudioProcessor::kSidechainGainMax;
    spec.gainDefault = STRETRAudioProcessor::kSidechainGainDefault;
    spec.gainSkew = STRETRAudioProcessor::kGainSkew;
    spec.smoothMin = STRETRAudioProcessor::kSidechainSmoothMin;
    spec.smoothMax = STRETRAudioProcessor::kSidechainSmoothMax;
    spec.smoothDefault = STRETRAudioProcessor::kSidechainSmoothDefault;
    spec.polMin = STRETRAudioProcessor::kSidechainPolMin;
    spec.polMax = STRETRAudioProcessor::kSidechainPolMax;
    spec.polDefault = STRETRAudioProcessor::kSidechainPolDefault;
    spec.freqMin = STRETRAudioProcessor::kSidechainFilterFreqMin;
    spec.freqMax = STRETRAudioProcessor::kSidechainFilterFreqMax;
    spec.hpDefault = STRETRAudioProcessor::kSidechainHpDefault;
    spec.lpDefault = STRETRAudioProcessor::kSidechainLpDefault;
    spec.slopeMin = STRETRAudioProcessor::kFilterSlopeMin;
    spec.slopeMax = STRETRAudioProcessor::kFilterSlopeMax;
    spec.refreshTooltip = [this]
    {
        auto& state = audioProcessor.apvts;
        sidechainDisplay.setTooltip (formatSidechainTooltip (
            state.getRawParameterValue (STRETRAudioProcessor::kParamSidechainGain)->load(),
            state.getRawParameterValue (STRETRAudioProcessor::kParamSidechainSmooth)->load(),
            state.getRawParameterValue (STRETRAudioProcessor::kParamSidechainPol)->load(),
            state.getRawParameterValue (STRETRAudioProcessor::kParamSidechainHpOn)->load() > 0.5f,
            state.getRawParameterValue (STRETRAudioProcessor::kParamSidechainHp)->load(),
            (int) std::lround (state.getRawParameterValue (STRETRAudioProcessor::kParamSidechainHpSlope)->load()),
            state.getRawParameterValue (STRETRAudioProcessor::kParamSidechainLpOn)->load() > 0.5f,
            state.getRawParameterValue (STRETRAudioProcessor::kParamSidechainLp)->load(),
            (int) std::lround (state.getRawParameterValue (STRETRAudioProcessor::kParamSidechainLpSlope)->load()))); 
    };

    openSidechainPromptShared (this, lnf, activeScheme, vts, spec);
}

void STRETRAudioProcessorEditor::openInfoPopup()
{
    TR::openInfoPopupFromXmlShared<STRETRAudioProcessorEditor> (this,
                                           lnf,
                                           activeScheme,
                                           InfoContent::xml,
                                           [this]() { openGraphicsPopup(); });
}

// -- Graphics popup helper: sync state ----------------------------

void STRETRAudioProcessorEditor::openGraphicsPopup()
{
    lnf.setScheme (activeScheme);
    useCustomPalette = audioProcessor.getUiUseCustomPalette();
    crtEnabled = false;
    ioFxEnabled = audioProcessor.getUiIoFxEnabled();
    crtEffect.setEnabled (false);
    applyActivePalette();

    TR::openGraphicsPopupShared<STRETRAudioProcessorEditor> (this,
                                        lnf,
                                        activeScheme,
                                        defaultPalette,
                                        customPalette,
                                        useCustomPalette,
                                        ioFxEnabled,
                                        [this] (bool enabled)
                                        {
                                            useCustomPalette = enabled;
                                            audioProcessor.setUiUseCustomPalette (enabled);
                                        },
                                        [this] (int index, juce::Colour colour)
                                        {
                                            customPalette[(size_t) index] = colour;
                                            audioProcessor.setUiCustomPaletteColour (index, colour);
                                        },
                                        [this] (bool enabled)
                                        {
                                            applyIoFxState (enabled);
                                            audioProcessor.setUiIoFxEnabled (enabled);
                                        },
                                        [this]()
                                        {
                                            applyActivePalette();
                                            updateIoFxMeterSliders();
                                            repaint();
                                        });
}





