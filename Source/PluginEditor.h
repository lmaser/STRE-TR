#pragma once

#include <cstdint>
#include <atomic>
#include <JuceHeader.h>
#include "PluginProcessor.h"
#include "CrtEffect.h"
#include "../../TR-Shared/SimpleUI/TRSharedUI.h"

class STRETRAudioProcessorEditor  : public juce::AudioProcessorEditor,
                                     public juce::SettableTooltipClient,
                                     private juce::Slider::Listener,
                                     private juce::AudioProcessorValueTreeState::Listener,
                                     private juce::Timer
{
public:
    explicit STRETRAudioProcessorEditor (STRETRAudioProcessor&);
    ~STRETRAudioProcessorEditor() override;

    void paint (juce::Graphics&) override;
    void paintOverChildren (juce::Graphics&) override;
    void resized() override;
    void moved() override;
    void parentHierarchyChanged() override;

private:
    void mouseDown (const juce::MouseEvent& e) override;
    void mouseDoubleClick (const juce::MouseEvent& e) override;
    void mouseDrag (const juce::MouseEvent& e) override;
    void mouseMove (const juce::MouseEvent& e) override;
    void mouseExit (const juce::MouseEvent& e) override;

    void openNumericEntryPopupForSlider (juce::Slider& s);
    void openFilterPrompt();
    void openChaosConfigPrompt (const char* amtParamId, const char* spdParamId, const juce::String& title);
    void openChaosFilterPrompt();
    void openChaosDelayPrompt();
    void openPdcMaxWindowPrompt();
    void openTriggerDelayPrompt();
    void openMixSendPrompt();
    void openSidechainPrompt();
    void openInfoPopup();
    void openGraphicsPopup();
    void setPromptOverlayActive (bool shouldBeActive);
    void updateEngineControls();

    STRETRAudioProcessor& audioProcessor;

    class BarSlider : public TR::SimpleBarSliderBase
    {
    public:
        using TR::SimpleBarSliderBase::SimpleBarSliderBase;

        void setOwner (STRETRAudioProcessorEditor* o) { owner = o; }

        void setAllowNumericPopup (bool allow) { allowNumericPopup = allow; }

        void mouseDown (const juce::MouseEvent& e) override
        {
            if (e.mods.isPopupMenu())
            {
                if (allowNumericPopup && owner != nullptr)
                    owner->openNumericEntryPopupForSlider (*this);
                return;
            }

            if (owner != nullptr && this == &owner->windowSlider && owner->isCurrentEngineFft())
            {
                setFftWindowFromMouse (e);
                return;
            }

            TR::SimpleBarSliderBase::mouseDown (e);
        }

        void mouseDrag (const juce::MouseEvent& e) override
        {
            if (owner != nullptr && this == &owner->windowSlider && owner->isCurrentEngineFft())
            {
                setFftWindowFromMouse (e);
                return;
            }

            TR::SimpleBarSliderBase::mouseDrag (e);
        }

        juce::String getTextFromValue (double v) override
        {
            if (owner != nullptr && this == &owner->mixSlider)
            {
                double percent = v * 100.0;
                return juce::String (percent, 2);
            }

            // Amount (0-100%)
            if (owner != nullptr && this == &owner->amountSlider)
            {
                return juce::String (v, 2);
            }

            // Jitter (0-100%)
            if (owner != nullptr && this == &owner->jitterSlider)
            {
                return juce::String (v, 2);
            }

            // Grain (ms)
            if (owner != nullptr && this == &owner->grainSlider)
            {
                return juce::String (v, 1) + "ms";
            }

            // Input/output gain
            if (owner != nullptr && (this == &owner->inputSlider || this == &owner->outputSlider))
            {
                if (v <= STRETRAudioProcessor::kGainFloorDb + 0.001)
                    return "-INF";
                const double rounded1 = std::round (v * 10.0) / 10.0;
                return juce::String (rounded1, 1);
            }

            // PITCH (0-1 -> -24st..+24st)
            if (owner != nullptr && this == &owner->pitchSlider)
            {
                double st = (juce::jlimit (0.0, 1.0, v) - 0.5) * 48.0;
                if (std::abs (st) < 0.005)
                    st = 0.0;
                return (st >= 0.0 ? "+" : "") + juce::String (st, 2);
            }

            // Pan (0-1 ? L/C/R)
            if (owner != nullptr && this == &owner->panSlider)
            {
                double percent = v * 100.0;
                if (std::abs (percent - 50.0) < 1.0) return "C";
                if (percent < 50.0) return "L" + juce::String (50.0 - percent, 0);
                return "R" + juce::String (percent - 50.0, 0);
            }

            // Engine
            if (owner != nullptr && this == &owner->engineSlider)
            {
                const int mode = (int) std::lround (v);
                switch (mode)
                {
                    case 0: return "STRETCH";
                    case 1: return "GRAIN";
                    case 2: return "FFT";
                    default: return "STRETCH";
                }
            }

            // Window (FFT engines snap to canonical powers of two)
            if (owner != nullptr && this == &owner->windowSlider)
            {
                return juce::String (owner->getEffectiveWindowValue (v));
            }

            // Style (0=MONO 1=STEREO 2=WIDE 3=DUAL)
            if (owner != nullptr && this == &owner->styleSlider)
            {
                const int mode = (int) std::lround (v);
                switch (mode)
                {
                    case 0: return "MONO";
                    case 1: return "STEREO";
                    case 2: return "WIDE";
                    case 3: return "DUAL";
                    default: return "STEREO";
                }
            }

            juce::String t = juce::Slider::getTextFromValue (v);
            int dot = t.indexOfChar ('.');
            if (dot >= 0)
                t = t.substring (0, dot + 1 + 4);
            return t;
        }

        double snapValue (double attemptedValue, DragMode dragMode) override
        {
            if (owner != nullptr && this == &owner->windowSlider && owner->isCurrentEngineFft())
                return (double) owner->getEffectiveWindowValue (attemptedValue);

            return juce::Slider::snapValue (attemptedValue, dragMode);
        }

        double valueToProportionOfLength (double value) override
        {
            if (owner != nullptr && this == &owner->windowSlider && owner->isCurrentEngineFft())
            {
                const int maxLane = STRETRAudioProcessor::getFftWindowLane (owner->getCurrentMaxFftWindow());
                if (maxLane <= 0)
                    return 1.0;

                const int lane = juce::jlimit (0, maxLane,
                    STRETRAudioProcessor::getFftWindowLane ((int) std::lround (value)));
                return (double) lane / (double) maxLane;
            }

            return juce::Slider::valueToProportionOfLength (value);
        }

        double proportionOfLengthToValue (double proportion) override
        {
            if (owner != nullptr && this == &owner->windowSlider && owner->isCurrentEngineFft())
            {
                const int maxLane = STRETRAudioProcessor::getFftWindowLane (owner->getCurrentMaxFftWindow());
                if (maxLane <= 0)
                    return (double) STRETRAudioProcessor::kFftWindowMin;

                const int lane = juce::jlimit (0, maxLane,
                    (int) std::lround (juce::jlimit (0.0, 1.0, proportion) * (double) maxLane));
                return (double) STRETRAudioProcessor::kFftWindows[lane];
            }

            return juce::Slider::proportionOfLengthToValue (proportion);
        }

    private:
        void setFftWindowFromMouse (const juce::MouseEvent& e)
        {
            const float innerW = (float) getWidth() - 14.0f;
            const float proportion = innerW > 0.0f ? ((float) e.x - 7.0f) / innerW : 0.0f;
            const int maxLane = STRETRAudioProcessor::getFftWindowLane (owner->getCurrentMaxFftWindow());
            const int lane = juce::jlimit (0, maxLane,
                (int) std::lround (juce::jlimit (0.0f, 1.0f, proportion) * (float) maxLane));
            setValue ((double) STRETRAudioProcessor::kFftWindows[lane], juce::sendNotificationSync);
        }

        STRETRAudioProcessorEditor* owner = nullptr;
        bool allowNumericPopup = true;
    };

    using MainGuiToggleButton = TR::MainGuiPromptToggleButton;

    BarSlider amountSlider;
    BarSlider pitchSlider;
    BarSlider grainSlider;
    BarSlider engineSlider;
    BarSlider windowSlider;
    BarSlider jitterSlider;
    BarSlider styleSlider;
    BarSlider inputSlider;
    BarSlider outputSlider;
    BarSlider tiltSlider;
    BarSlider panSlider;
    BarSlider mixSlider;
    BarSlider limThresholdSlider;

    juce::ComboBox modeInCombo;
    juce::ComboBox modeOutCombo;
    juce::ComboBox sumBusCombo;
    juce::ComboBox limModeCombo;
    juce::ComboBox invPolCombo;
    juce::ComboBox invStrCombo;
    juce::ComboBox mixModeCombo;
    juce::ComboBox filterPosCombo;

    MainGuiToggleButton reverseButton;
    MainGuiToggleButton triggerButton;
    MainGuiToggleButton alignButton;
    MainGuiToggleButton pdcButton;
    juce::Label triggerDisplay;
    juce::Label pdcDisplay;
    MainGuiToggleButton chaosFilterButton;
    MainGuiToggleButton chaosDelayButton;
    MainGuiToggleButton sidechainButton;

    juce::Label chaosFilterDisplay;
    juce::Label chaosDelayDisplay;
    juce::Label sidechainDisplay;

    using SliderAttachment = juce::AudioProcessorValueTreeState::SliderAttachment;
    using ButtonAttachment = juce::AudioProcessorValueTreeState::ButtonAttachment;

    std::unique_ptr<SliderAttachment> amountAttachment;
    std::unique_ptr<SliderAttachment> pitchAttachment;
    std::unique_ptr<SliderAttachment> grainAttachment;
    std::unique_ptr<SliderAttachment> engineAttachment;
    std::unique_ptr<SliderAttachment> windowAttachment;
    std::unique_ptr<SliderAttachment> jitterAttachment;
    std::unique_ptr<SliderAttachment> styleAttachment;
    std::unique_ptr<SliderAttachment> inputAttachment;
    std::unique_ptr<SliderAttachment> outputAttachment;
    std::unique_ptr<SliderAttachment> tiltAttachment;
    std::unique_ptr<SliderAttachment> panAttachment;
    std::unique_ptr<SliderAttachment> mixAttachment;
    std::unique_ptr<SliderAttachment> limThresholdAttachment;

    using ComboBoxAttachment = juce::AudioProcessorValueTreeState::ComboBoxAttachment;
    std::unique_ptr<ComboBoxAttachment> modeInAttachment;
    std::unique_ptr<ComboBoxAttachment> modeOutAttachment;
    std::unique_ptr<ComboBoxAttachment> sumBusAttachment;
    std::unique_ptr<ComboBoxAttachment> limModeAttachment;
    std::unique_ptr<ComboBoxAttachment> invPolAttachment;
    std::unique_ptr<ComboBoxAttachment> invStrAttachment;
    std::unique_ptr<ComboBoxAttachment> mixModeAttachment;
    std::unique_ptr<ComboBoxAttachment> filterPosAttachment;

    std::unique_ptr<ButtonAttachment> reverseAttachment;
    std::unique_ptr<ButtonAttachment> triggerAttachment;
    std::unique_ptr<ButtonAttachment> alignAttachment;
    std::unique_ptr<ButtonAttachment> pdcAttachment;
    std::unique_ptr<ButtonAttachment> chaosFilterAttachment;
    std::unique_ptr<ButtonAttachment> chaosDelayAttachment;
    std::unique_ptr<ButtonAttachment> sidechainAttachment;

    juce::ComponentBoundsConstrainer resizeConstrainer;
    std::unique_ptr<juce::ResizableCornerComponent> resizerCorner;

    using STREScheme = TR::TRScheme;

    STREScheme activeScheme;

    using HorizontalLayoutMetrics = TR::SimpleHorizontalLayoutMetrics;
    using VerticalLayoutMetrics = TR::SimpleVerticalLayoutMetrics;

    static HorizontalLayoutMetrics buildHorizontalLayout (int editorW, int valueColW);
    static VerticalLayoutMetrics buildVerticalLayout (int editorH, int biasY, bool ioExpanded);
    void updateCachedLayout();

    using MinimalLNF = TR::SimpleLookAndFeel;
    using FilterBarComponent = TR::SimpleFilterBarComponent<STRETRAudioProcessorEditor, STRETRAudioProcessor, STREScheme>;
    FilterBarComponent filterBar_;

    using DualMixBarComponent = TR::SimpleDualMixBarComponent<STRETRAudioProcessorEditor, STRETRAudioProcessor, STREScheme>;
    DualMixBarComponent dualMixBar_;

    using PromptOverlay = TR::PromptOverlay;

    MinimalLNF lnf;
    std::unique_ptr<juce::TooltipWindow> tooltipWindow;
    PromptOverlay promptOverlay;

    void setupBar (juce::Slider& s);

    juce::String getAmountText() const;
    juce::String getAmountTextShort() const;

    juce::String getPitchText() const;
    juce::String getPitchTextShort() const;

    juce::String getJitterText() const;
    juce::String getJitterTextShort() const;

    juce::String getGrainText() const;
    juce::String getGrainTextShort() const;

    juce::String getEngineText() const;
    juce::String getEngineTextShort() const;

    int getCurrentEngineValue() const;
    bool isCurrentEngineFft() const;
    int getCurrentMaxFftWindow() const;
    void syncFftWindowToMax (bool notifyHost);
    void updatePdcTooltip();
    int getEffectiveWindowValue (double rawWindowValue) const;
    juce::String getWindowText() const;
    juce::String getWindowTextShort() const;

    juce::String getStyleText() const;
    juce::String getStyleTextShort() const;

    juce::String getInputText() const;
    juce::String getInputTextShort() const;

    juce::String getOutputText() const;
    juce::String getOutputTextShort() const;

    juce::String getMixText() const;
    juce::String getMixTextShort() const;

    juce::String getTiltText() const;
    juce::String getTiltTextShort() const;

    juce::String getPanText() const;
    juce::String getPanTextShort() const;

    juce::String getLimThresholdText() const;
    juce::String getLimThresholdTextShort() const;

    int getTargetValueColumnWidth() const;

    void sliderValueChanged (juce::Slider* slider) override;
    void parameterChanged (const juce::String& parameterID, float newValue) override;
    void timerCallback() override;

    void applyPersistedUiStateFromProcessor (bool applySize, bool applyPaletteAndFx);

public:
    void triggerUiRestore() { applyPersistedUiStateFromProcessor (true, true); }

private:
    void applyLabelTextColour (juce::Label& label, juce::Colour colour);

    friend class TR::SimpleFilterBarComponent<STRETRAudioProcessorEditor, STRETRAudioProcessor, STREScheme>;
    friend class TR::SimpleDualMixBarComponent<STRETRAudioProcessorEditor, STRETRAudioProcessor, STREScheme>;
    friend struct TR::PromptHostBridge;

    template <typename T>
    friend void TR::embedAlertWindowInOverlay (T*, juce::AlertWindow*, bool);

    juce::Rectangle<int> getValueAreaFor (const juce::Rectangle<int>& barBounds) const;
    juce::Slider* getSliderForValueAreaPoint (juce::Point<int> p);
    juce::Rectangle<int> getReverseLabelArea() const;
    juce::Rectangle<int> getTriggerLabelArea() const;
    juce::Rectangle<int> getAlignLabelArea() const;
    juce::Rectangle<int> getPdcLabelArea() const;
    juce::Rectangle<int> getChaosLabelArea() const;
    juce::Rectangle<int> getChaosDelayLabelArea() const;
    juce::Rectangle<int> getSidechainLabelArea() const;
    juce::Rectangle<int> getInfoIconArea() const;
    void updateInfoIconCache();
    bool refreshLegendTextCache();
    TR::SimpleMainPanelSpec buildMainPanelSpec();
    juce::Rectangle<int> getRowRepaintBounds (const juce::Slider& s) const;
    void applyActivePalette();
    void applyCrtState (bool enabled);
    void applyIoFxState (bool enabled);
    void updateIoFxMeterSliders();

    juce::Path cachedInfoGearPath;
    juce::Rectangle<float> cachedInfoGearHole;
    bool clampingWindowSlider_ = false;

    juce::String cachedAmountTextFull;
    juce::String cachedAmountTextShort;
    juce::String cachedPitchTextFull;
    juce::String cachedPitchTextShort;
    juce::String cachedJitterTextFull;
    juce::String cachedJitterTextShort;
    juce::String cachedGrainTextFull;
    juce::String cachedGrainTextShort;
    juce::String cachedEngineTextFull;
    juce::String cachedEngineTextShort;
    juce::String cachedWindowTextFull;
    juce::String cachedWindowTextShort;
    juce::String cachedStyleTextFull;
    juce::String cachedStyleTextShort;
    juce::String cachedInputTextFull;
    juce::String cachedInputTextShort;
    juce::String cachedOutputTextFull;
    juce::String cachedOutputTextShort;
    juce::String cachedMixTextFull;
    juce::String cachedMixTextShort;
    juce::String cachedTiltTextFull;
    juce::String cachedTiltTextShort;
    juce::String cachedLimThresholdTextFull;
    juce::String cachedLimThresholdTextShort;
    juce::String cachedLimThresholdIntOnly;

    juce::String cachedAmountIntOnly;
    juce::String cachedPitchIntOnly;
    juce::String cachedJitterIntOnly;
    juce::String cachedGrainIntOnly;
    juce::String cachedEngineIntOnly;
    juce::String cachedWindowIntOnly;
    juce::String cachedStyleIntOnly;
    juce::String cachedInputIntOnly;
    juce::String cachedOutputIntOnly;
    juce::String cachedMixIntOnly;
    juce::String cachedTiltIntOnly;

    juce::String cachedFilterTextFull;
    juce::String cachedFilterTextShort;
    juce::String cachedPanTextFull;
    juce::String cachedPanTextShort;
    juce::String cachedPanIntOnly;

    mutable std::uint64_t cachedValueColumnWidthKey = 0;
    mutable int cachedValueColumnWidth = 90;

    HorizontalLayoutMetrics cachedHLayout_;
    VerticalLayoutMetrics cachedVLayout_;
    std::array<juce::Rectangle<int>, 12> cachedValueAreas_;
    juce::Rectangle<int> cachedFilterValueArea_;
    juce::Rectangle<int> cachedPanValueArea_;
    juce::Rectangle<int> cachedLimThresholdValueArea_;
    juce::Rectangle<int> cachedTiltValueArea_;
    juce::Rectangle<int> cachedToggleBarArea_;
    juce::Rectangle<int> cachedChaosArea_;
    bool ioSectionExpanded_ = false;

    static constexpr double kDefaultAmount = (double) STRETRAudioProcessor::kAmountDefault;
    static constexpr double kDefaultMix    = (double) STRETRAudioProcessor::kMixDefault;
    static constexpr double kDefaultInput  = (double) STRETRAudioProcessor::kInputDefault;
    static constexpr double kDefaultOutput = (double) STRETRAudioProcessor::kOutputDefault;
    static constexpr double kDefaultTilt   = (double) STRETRAudioProcessor::kTiltDefault;
    static constexpr double kDefaultLimThreshold = 0.0;

    static constexpr int kMinW = 360;
    static constexpr int kMinH = 752;
    static constexpr int kMaxW = kMinW + (kMinW / 2);
    static constexpr int kMaxH = kMinH;

    static constexpr int kLayoutVerticalBiasPx = 10;

    bool promptOverlayActive = false;
    bool suppressSizePersistence = false;
    int lastPersistedEditorW = -1;
    int lastPersistedEditorH = -1;
    std::atomic<uint32_t> lastUserInteractionMs { 0 };
    static constexpr uint32_t kUserInteractionPersistWindowMs = 5000;
    bool crtEnabled = false;
    bool ioFxEnabled = true;
    bool useCustomPalette = false;
    double lastInputSignalMs = -10000.0;
    double lastOutputSignalMs = -10000.0;

    CrtEffect crtEffect;
    float     crtTime = 0.0f;

    static constexpr int kPaletteColourCount = 4;
    std::array<juce::Colour, kPaletteColourCount> defaultPalette = TR::defaultSimplePalette();
    std::array<juce::Colour, kPaletteColourCount> customPalette = TR::defaultSimpleCustomPalette();

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (STRETRAudioProcessorEditor)
};



