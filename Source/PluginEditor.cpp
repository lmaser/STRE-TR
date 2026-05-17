// PluginEditor.cpp  –  STRE-TR
#include "PluginEditor.h"
#include "InfoContent.h"
#include <functional>

using namespace TR;

#if JUCE_WINDOWS
 #include <windows.h>
#endif

namespace UiStateKeys
{
    constexpr const char* editorWidth       = "uiEditorWidth";
    constexpr const char* editorHeight      = "uiEditorHeight";
    constexpr const char* useCustomPalette  = "uiUseCustomPalette";
    constexpr const char* crtEnabled        = "uiFxTailEnabled";
    constexpr std::array<const char*, 2> customPalette {
        "uiCustomPalette0",
        "uiCustomPalette1"
    };
}

// ── Timer & display constants ──
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

// ── Parameter listener IDs ──
static constexpr std::array<const char*, 5> kUiMirrorParamIds {
    STRETRAudioProcessor::kParamUiPalette,
    STRETRAudioProcessor::kParamUiCrt,
    STRETRAudioProcessor::kParamUiColor0,
    STRETRAudioProcessor::kParamUiColor1,
    STRETRAudioProcessor::kParamEngine
};

//========================== LookAndFeel ==========================

void STRETRAudioProcessorEditor::MinimalLNF::drawLinearSlider (juce::Graphics& g,
    int x, int y, int width, int height,
    float sliderPos, float, float,
    const juce::Slider::SliderStyle, juce::Slider&)
{
    const juce::Rectangle<float> r ((float) x, (float) y, (float) width, (float) height);
    g.setColour (scheme.outline);
    g.drawRect (r, 4.0f);
    const float pad = 7.0f;
    auto inner = r.reduced (pad);
    g.setColour (scheme.bg);
    g.fillRect (inner);
    const float fillW = juce::jlimit (0.0f, inner.getWidth(), sliderPos - inner.getX());
    g.setColour (scheme.fg);
    g.fillRect (inner.withWidth (fillW));
}

void STRETRAudioProcessorEditor::MinimalLNF::drawTickBox (juce::Graphics& g, juce::Component& button,
    float, float, float, float,
    bool ticked, bool, bool, bool)
{
    const auto local = button.getLocalBounds().toFloat().reduced (1.0f);
    const float side = juce::jlimit (14.0f,
                                     juce::jmax (14.0f, local.getHeight() - 2.0f),
                                     std::round (local.getHeight() * 0.65f));
    auto r = juce::Rectangle<float> (local.getX() + 2.0f,
                                     local.getCentreY() - (side * 0.5f),
                                     side, side).getIntersection (local);
    if (ticked)
    {
        g.setColour (scheme.outline);
        g.fillRect (r);
    }
    else
    {
        g.setColour (scheme.outline);
        g.drawRect (r, 4.0f);
        const float innerInset = juce::jlimit (1.0f, side * 0.45f, side * UiMetrics::tickBoxInnerInsetRatio);
        g.setColour (scheme.bg);
        g.fillRect (r.reduced (innerInset));
    }
}

void STRETRAudioProcessorEditor::MinimalLNF::drawToggleButton (
    juce::Graphics& g, juce::ToggleButton& button,
    bool shouldDrawButtonAsHighlighted, bool shouldDrawButtonAsDown)
{
    drawTickBox (g, button, 0, 0, 0, 0,
                 button.getToggleState(), button.isEnabled(),
                 shouldDrawButtonAsHighlighted, shouldDrawButtonAsDown);

    const auto local = button.getLocalBounds().toFloat().reduced (1.0f);
    const float side = juce::jlimit (14.0f,
                                     juce::jmax (14.0f, local.getHeight() - 2.0f),
                                     std::round (local.getHeight() * 0.65f));
    const float textX = local.getX() + 2.0f + side + 2.0f;
    auto textArea = button.getLocalBounds().toFloat();
    textArea.removeFromLeft (textX);

    g.setColour (button.findColour (juce::ToggleButton::textColourId));
    float fontSize = juce::jlimit (12.0f, 40.0f, (float) button.getHeight() - 6.0f);
    const auto text = button.getButtonText();
    const float availW = textArea.getWidth();
    if (availW > 0)
    {
        juce::Font testFont (juce::FontOptions (fontSize).withStyle ("Bold"));
        juce::GlyphArrangement ga;
        ga.addLineOfText (testFont, text, 0.0f, 0.0f);
        const float neededW = ga.getBoundingBox (0, -1, false).getWidth();
        if (neededW > availW)
            fontSize = juce::jmax (8.0f, fontSize * (availW / neededW));
    }
    g.setFont (juce::Font (juce::FontOptions (fontSize).withStyle ("Bold")));
    g.drawText (text, textArea, juce::Justification::centredLeft, false);
}

void STRETRAudioProcessorEditor::MinimalLNF::drawButtonBackground (juce::Graphics& g,
    juce::Button& button, const juce::Colour& backgroundColour,
    bool shouldDrawButtonAsHighlighted, bool shouldDrawButtonAsDown)
{
    auto r = button.getLocalBounds();
    auto fill = backgroundColour;
    if (shouldDrawButtonAsDown) fill = fill.brighter (0.12f);
    else if (shouldDrawButtonAsHighlighted) fill = fill.brighter (0.06f);
    g.setColour (fill);
    g.fillRect (r);
    g.setColour (scheme.outline);
    g.drawRect (r.reduced (1), 3);
}

void STRETRAudioProcessorEditor::MinimalLNF::drawAlertBox (juce::Graphics& g,
    juce::AlertWindow& alert, const juce::Rectangle<int>& textArea, juce::TextLayout& textLayout)
{
    auto bounds = alert.getLocalBounds();
    g.setColour (scheme.bg);
    g.fillRect (bounds);
    g.setColour (scheme.outline);
    g.drawRect (bounds.reduced (1), 3);
    g.setColour (scheme.text);
    textLayout.draw (g, textArea.toFloat());
}

void STRETRAudioProcessorEditor::MinimalLNF::drawBubble (juce::Graphics& g,
    juce::BubbleComponent&, const juce::Point<float>&, const juce::Rectangle<float>& body)
{
    drawOverlayPanel (g, body.getSmallestIntegerContainer(),
                      findColour (juce::TooltipWindow::backgroundColourId),
                      findColour (juce::TooltipWindow::outlineColourId));
}

void STRETRAudioProcessorEditor::MinimalLNF::drawScrollbar (juce::Graphics& g,
    juce::ScrollBar&, int x, int y, int width, int height,
    bool isScrollbarVertical, int thumbStartPosition, int thumbSize,
    bool isMouseOver, bool isMouseDown)
{
    juce::ignoreUnused (x, y, width, height);
    const auto thumbColour = scheme.text.withAlpha (isMouseDown ? 0.7f : isMouseOver ? 0.5f : 0.3f);
    constexpr float barThickness = 7.0f;
    constexpr float cornerRadius = 3.5f;
    if (isScrollbarVertical)
    {
        const float bx = (float) (x + width) - barThickness - 1.0f;
        g.setColour (thumbColour);
        g.fillRoundedRectangle (bx, (float) thumbStartPosition, barThickness, (float) thumbSize, cornerRadius);
    }
    else
    {
        const float by = (float) (y + height) - barThickness - 1.0f;
        g.setColour (thumbColour);
        g.fillRoundedRectangle ((float) thumbStartPosition, by, (float) thumbSize, barThickness, cornerRadius);
    }
}

void STRETRAudioProcessorEditor::MinimalLNF::drawComboBox (
    juce::Graphics& g, int width, int height,
    bool, int, int, int, int, juce::ComboBox&)
{
    const juce::Rectangle<int> r (0, 0, width, height);
    g.setColour (scheme.bg);  g.fillRect (r);
    g.setColour (scheme.outline); g.drawRect (r, 3);
}

void STRETRAudioProcessorEditor::MinimalLNF::drawPopupMenuBackground (
    juce::Graphics& g, int width, int height)
{
    g.fillAll (scheme.bg);
    g.setColour (scheme.outline);
    g.drawRect (0, 0, width, height, 2);
}

juce::Font STRETRAudioProcessorEditor::MinimalLNF::getComboBoxFont (juce::ComboBox& box)
{
    const float h = juce::jlimit (12.0f, 24.0f, box.getHeight() * 0.59f);
    return juce::Font (juce::FontOptions (h).withStyle ("Bold"));
}

juce::Font STRETRAudioProcessorEditor::MinimalLNF::getTextButtonFont (juce::TextButton&, int buttonHeight)
{
    const float h = juce::jlimit (12.0f, 26.0f, buttonHeight * 0.48f);
    return juce::Font (juce::FontOptions (h).withStyle ("Bold"));
}

juce::Font STRETRAudioProcessorEditor::MinimalLNF::getAlertWindowMessageFont()
{
    auto f = juce::LookAndFeel_V4::getAlertWindowMessageFont();
    f.setBold (true);
    return f;
}

juce::Font STRETRAudioProcessorEditor::MinimalLNF::getLabelFont (juce::Label& label)
{
    auto f = label.getFont();
    if (f.getHeight() <= 0.0f)
    {
        const float h = juce::jlimit (12.0f, 40.0f, (float) juce::jmax (12, label.getHeight() - 6));
        f = juce::Font (juce::FontOptions (h).withStyle ("Bold"));
    }
    else
    {
        f.setBold (true);
    }
    return f;
}

juce::Font STRETRAudioProcessorEditor::MinimalLNF::getSliderPopupFont (juce::Slider&)
{
    return makeOverlayDisplayFont();
}

juce::Rectangle<int> STRETRAudioProcessorEditor::MinimalLNF::getTooltipBounds (const juce::String& tipText,
    juce::Point<int> screenPos, juce::Rectangle<int> parentArea)
{
    const auto f = makeOverlayDisplayFont();
    const int h = juce::jmax (UiMetrics::tooltipMinHeight,
                              (int) std::ceil (f.getHeight() * UiMetrics::tooltipHeightScale));
    const int anchorOffsetX = juce::jmax (8, (int) std::round ((double) h * UiMetrics::tooltipAnchorXRatio));
    const int anchorOffsetY = juce::jmax (10, (int) std::round ((double) h * UiMetrics::tooltipAnchorYRatio));
    const int parentMargin = juce::jmax (2, (int) std::round ((double) h * UiMetrics::tooltipParentMarginRatio));
    const int widthPad = juce::jmax (16, (int) std::round (f.getHeight() * UiMetrics::tooltipWidthPadFontRatio));
    const int w = juce::jmax (UiMetrics::tooltipMinWidth, stringWidth (f, tipText) + widthPad);
    auto r = juce::Rectangle<int> (screenPos.x + anchorOffsetX, screenPos.y + anchorOffsetY, w, h);
    return r.constrainedWithin (parentArea.reduced (parentMargin));
}

void STRETRAudioProcessorEditor::MinimalLNF::drawTooltip (juce::Graphics& g,
    const juce::String& text, int width, int height)
{
    const auto f = makeOverlayDisplayFont();
    const int h = juce::jmax (UiMetrics::tooltipMinHeight,
                              (int) std::ceil (f.getHeight() * UiMetrics::tooltipHeightScale));
    const int textInsetX = juce::jmax (4, (int) std::round ((double) h * UiMetrics::tooltipTextInsetXRatio));
    const int textInsetY = juce::jmax (1, (int) std::round ((double) h * UiMetrics::tooltipTextInsetYRatio));
    drawOverlayPanel (g, { 0, 0, width, height },
                      findColour (juce::TooltipWindow::backgroundColourId),
                      findColour (juce::TooltipWindow::outlineColourId));
    g.setColour (findColour (juce::TooltipWindow::textColourId));
    g.setFont (f);
    g.drawFittedText (text, textInsetX, textInsetY,
                      juce::jmax (1, width - (textInsetX * 2)),
                      juce::jmax (1, height - (textInsetY * 2)),
                      juce::Justification::centred, 1);
}

//========================== FilterBarComponent ==========================

juce::Rectangle<float> STRETRAudioProcessorEditor::FilterBarComponent::getInnerArea() const
{ return getLocalBounds().toFloat().reduced (kPad); }

float STRETRAudioProcessorEditor::FilterBarComponent::freqToNormX (float freq) const
{
    const float clamped = juce::jlimit (kMinFreq, kMaxFreq, freq);
    return std::log2 (clamped / kMinFreq) / std::log2 (kMaxFreq / kMinFreq);
}

float STRETRAudioProcessorEditor::FilterBarComponent::normXToFreq (float normX) const
{
    return kMinFreq * std::pow (2.0f, juce::jlimit (0.0f, 1.0f, normX) * std::log2 (kMaxFreq / kMinFreq));
}

float STRETRAudioProcessorEditor::FilterBarComponent::getMarkerScreenX (float freq) const
{
    const auto inner = getInnerArea();
    return inner.getX() + freqToNormX (freq) * inner.getWidth();
}

STRETRAudioProcessorEditor::FilterBarComponent::DragTarget
STRETRAudioProcessorEditor::FilterBarComponent::hitTestMarker (juce::Point<float> p) const
{
    const float hpX = getMarkerScreenX (hpFreq_);
    const float lpX = getMarkerScreenX (lpFreq_);
    const float distHp = std::abs (p.x - hpX);
    const float distLp = std::abs (p.x - lpX);
    if (distHp <= kMarkerHitPx && distHp <= distLp) return HP;
    if (distLp <= kMarkerHitPx) return LP;
    if (distHp <= kMarkerHitPx) return HP;
    return None;
}

void STRETRAudioProcessorEditor::FilterBarComponent::setFreqFromMouseX (float mouseX, DragTarget target)
{
    if (owner == nullptr || target == None) return;
    const auto inner = getInnerArea();
    const float normX = (inner.getWidth() > 0.0f) ? (mouseX - inner.getX()) / inner.getWidth() : 0.0f;
    float freq = normXToFreq (normX);
    auto& proc = owner->audioProcessor;
    if (target == HP)
    {
        const float other = proc.apvts.getRawParameterValue (STRETRAudioProcessor::kParamFilterLpFreq)->load();
        freq = juce::jmin (freq, other);
    }
    else
    {
        const float other = proc.apvts.getRawParameterValue (STRETRAudioProcessor::kParamFilterHpFreq)->load();
        freq = juce::jmax (freq, other);
    }
    const char* paramId = (target == HP) ? STRETRAudioProcessor::kParamFilterHpFreq
                                         : STRETRAudioProcessor::kParamFilterLpFreq;
    if (auto* param = proc.apvts.getParameter (paramId))
        param->setValueNotifyingHost (param->convertTo0to1 (freq));
}

void STRETRAudioProcessorEditor::FilterBarComponent::updateTooltipForTarget (DragTarget target)
{
    if (target == HP) setTooltip ("HP " + juce::String (juce::roundToInt (hpFreq_)) + " Hz");
    else if (target == LP) setTooltip ("LP " + juce::String (juce::roundToInt (lpFreq_)) + " Hz");
    else setTooltip ({});
}

void STRETRAudioProcessorEditor::FilterBarComponent::updateFromProcessor()
{
    if (owner == nullptr) return;
    auto& proc = owner->audioProcessor;
    const float newHp = proc.apvts.getRawParameterValue (STRETRAudioProcessor::kParamFilterHpFreq)->load();
    const float newLp = proc.apvts.getRawParameterValue (STRETRAudioProcessor::kParamFilterLpFreq)->load();
    const bool  newHpOn = proc.apvts.getRawParameterValue (STRETRAudioProcessor::kParamFilterHpOn)->load() > 0.5f;
    const bool  newLpOn = proc.apvts.getRawParameterValue (STRETRAudioProcessor::kParamFilterLpOn)->load() > 0.5f;
    if (newHp == hpFreq_ && newLp == lpFreq_ && newHpOn == hpOn_ && newLpOn == lpOn_) return;
    hpFreq_ = newHp; lpFreq_ = newLp; hpOn_ = newHpOn; lpOn_ = newLpOn;
    repaint();
}

void STRETRAudioProcessorEditor::FilterBarComponent::paint (juce::Graphics& g)
{
    const auto r = getLocalBounds().toFloat();
    g.setColour (scheme.outline); g.drawRect (r, 4.0f);
    const auto inner = getInnerArea();
    g.setColour (scheme.bg); g.fillRect (inner);
    if (hpOn_ || lpOn_)
    {
        const float hpX = hpOn_ ? getMarkerScreenX (hpFreq_) : inner.getX();
        const float lpX = lpOn_ ? getMarkerScreenX (lpFreq_) : inner.getRight();
        if (lpX > hpX)
        {
            g.setColour (scheme.fg.withAlpha (0.12f));
            g.fillRect (juce::Rectangle<float> (hpX, inner.getY(), lpX - hpX, inner.getHeight()).getIntersection (inner));
        }
    }
    auto drawMarker = [&] (float freq, bool on)
    {
        const float mx = getMarkerScreenX (freq);
        if (mx >= inner.getX() && mx <= inner.getRight())
        {
            g.setColour (scheme.fg.withAlpha (on ? 1.0f : 0.25f));
            const float hw = 2.5f, overshoot = 3.0f;
            g.fillRoundedRectangle (mx - hw, inner.getY() - overshoot, hw * 2.0f,
                                    inner.getHeight() + overshoot * 2.0f, 2.0f);
        }
    };
    drawMarker (hpFreq_, hpOn_);
    drawMarker (lpFreq_, lpOn_);
}

void STRETRAudioProcessorEditor::FilterBarComponent::mouseDown (const juce::MouseEvent& e)
{
    if (e.mods.isPopupMenu()) { if (owner) owner->openFilterPrompt(); return; }
    currentDrag_ = hitTestMarker (e.position);
    if (currentDrag_ != None) { setFreqFromMouseX (e.position.x, currentDrag_); updateFromProcessor(); updateTooltipForTarget (currentDrag_); }
}

void STRETRAudioProcessorEditor::FilterBarComponent::mouseDrag (const juce::MouseEvent& e)
{
    if (currentDrag_ != None) { setFreqFromMouseX (e.position.x, currentDrag_); updateFromProcessor(); updateTooltipForTarget (currentDrag_); }
}

void STRETRAudioProcessorEditor::FilterBarComponent::mouseUp (const juce::MouseEvent&) { currentDrag_ = None; }

void STRETRAudioProcessorEditor::FilterBarComponent::mouseMove (const juce::MouseEvent& e)
{
    updateTooltipForTarget (hitTestMarker (e.position));
}

void STRETRAudioProcessorEditor::FilterBarComponent::mouseDoubleClick (const juce::MouseEvent& e)
{
    if (owner == nullptr) return;
    auto& proc = owner->audioProcessor;
    const auto target = hitTestMarker (e.position);
    if (target == HP)
    {
        if (auto* param = proc.apvts.getParameter (STRETRAudioProcessor::kParamFilterHpOn))
        { const bool cur = param->getValue() > 0.5f; param->setValueNotifyingHost (cur ? 0.0f : 1.0f); }
    }
    else if (target == LP)
    {
        if (auto* param = proc.apvts.getParameter (STRETRAudioProcessor::kParamFilterLpOn))
        { const bool cur = param->getValue() > 0.5f; param->setValueNotifyingHost (cur ? 0.0f : 1.0f); }
    }
    else
    {
        owner->openFilterPrompt();
    }
}

//========================== DualMixBarComponent ==========================

juce::Rectangle<float> STRETRAudioProcessorEditor::DualMixBarComponent::getInnerArea() const
{
    return getLocalBounds().toFloat().reduced (kPad);
}

STRETRAudioProcessorEditor::DualMixBarComponent::DragTarget
STRETRAudioProcessorEditor::DualMixBarComponent::hitTestMarker (juce::Point<float> p) const
{
    const auto inner = getInnerArea();
    const float halfW = inner.getWidth() * 0.5f;
    const float midX  = inner.getX() + halfW;
    return (p.x < midX) ? DRY : WET;
}

void STRETRAudioProcessorEditor::DualMixBarComponent::setLevelFromMouseX (float mouseX, DragTarget target)
{
    if (owner == nullptr || target == None) return;
    const auto inner = getInnerArea();
    const float halfW = inner.getWidth() * 0.5f;
    float level;
    if (target == DRY)
        level = (halfW > 0.0f) ? juce::jlimit (0.0f, 1.0f, (mouseX - inner.getX()) / halfW) : 0.0f;
    else
        level = (halfW > 0.0f) ? juce::jlimit (0.0f, 1.0f, (mouseX - (inner.getX() + halfW)) / halfW) : 0.0f;
    const char* paramId = (target == DRY) ? STRETRAudioProcessor::kParamDryLevel
                                          : STRETRAudioProcessor::kParamWetLevel;
    if (auto* param = owner->audioProcessor.apvts.getParameter (paramId))
        param->setValueNotifyingHost (level);
}

void STRETRAudioProcessorEditor::DualMixBarComponent::updateTooltipForTarget (DragTarget target)
{
    if (target == None)
    {
        setTooltip ({});
        return;
    }

    const float level = (target == DRY) ? dryLevel_ : wetLevel_;
    const float dB = (level <= 0.0001f) ? -100.0f : 20.0f * std::log10 (level);
    const juce::String label = (target == DRY) ? "DRY" : "WET";
    setTooltip (dB <= -100.0f ? (label + ": -INF dB") : (label + ": " + juce::String (dB, 1) + " dB"));
}

void STRETRAudioProcessorEditor::DualMixBarComponent::updateFromProcessor()
{
    if (owner == nullptr) return;
    auto& proc = owner->audioProcessor;
    const float newDry = proc.apvts.getRawParameterValue (STRETRAudioProcessor::kParamDryLevel)->load();
    const float newWet = proc.apvts.getRawParameterValue (STRETRAudioProcessor::kParamWetLevel)->load();
    if (newDry == dryLevel_ && newWet == wetLevel_) return;
    dryLevel_ = newDry;
    wetLevel_ = newWet;
    repaint();
}

void STRETRAudioProcessorEditor::DualMixBarComponent::paint (juce::Graphics& g)
{
    const auto r = getLocalBounds().toFloat();
    g.setColour (scheme.outline);
    g.drawRect (r, 4.0f);
    const auto inner = getInnerArea();
    g.setColour (scheme.bg);
    g.fillRect (inner);
    const float halfW = inner.getWidth() * 0.5f;
    const float divX  = inner.getX() + halfW;
    g.setColour (scheme.fg.withAlpha (0.25f));
    g.drawVerticalLine ((int) divX, inner.getY(), inner.getBottom());
    {
        const float fillW = dryLevel_ * halfW;
        g.setColour (scheme.fg.withAlpha (0.18f));
        g.fillRect (juce::Rectangle<float> (inner.getX(), inner.getY(), fillW, inner.getHeight()).getIntersection (inner));
    }
    {
        const float fillW = wetLevel_ * halfW;
        g.setColour (scheme.fg.withAlpha (0.35f));
        g.fillRect (juce::Rectangle<float> (divX, inner.getY(), fillW, inner.getHeight()).getIntersection (inner));
    }
    {
        const float mx = inner.getX() + dryLevel_ * halfW;
        if (mx >= inner.getX() && mx <= divX)
        {
            const float hw = 2.5f; const float overshoot = 3.0f;
            g.setColour (scheme.fg.withAlpha (0.7f));
            g.fillRoundedRectangle (mx - hw, inner.getY() - overshoot, hw * 2.0f, inner.getHeight() + overshoot * 2.0f, 2.0f);
        }
    }
    {
        const float mx = divX + wetLevel_ * halfW;
        if (mx >= divX && mx <= inner.getRight())
        {
            const float hw = 2.5f; const float overshoot = 3.0f;
            g.setColour (scheme.fg);
            g.fillRoundedRectangle (mx - hw, inner.getY() - overshoot, hw * 2.0f, inner.getHeight() + overshoot * 2.0f, 2.0f);
        }
    }
}

void STRETRAudioProcessorEditor::DualMixBarComponent::mouseDown (const juce::MouseEvent& e)
{
    if (e.mods.isPopupMenu()) { if (owner) owner->openMixSendPrompt(); return; }
    currentDrag_ = hitTestMarker (e.position);
    if (currentDrag_ != None)
    {
        lastTouched_ = currentDrag_;
        setLevelFromMouseX (e.position.x, currentDrag_);
        updateFromProcessor();
        updateTooltipForTarget (currentDrag_);
        if (owner) { if (owner->refreshLegendTextCache()) owner->updateCachedLayout(); owner->repaint(); }
    }
}

void STRETRAudioProcessorEditor::DualMixBarComponent::mouseDrag (const juce::MouseEvent& e)
{
    if (currentDrag_ != None)
    {
        setLevelFromMouseX (e.position.x, currentDrag_);
        updateFromProcessor();
        updateTooltipForTarget (currentDrag_);
        if (owner) { if (owner->refreshLegendTextCache()) owner->updateCachedLayout(); owner->repaint(); }
    }
}

void STRETRAudioProcessorEditor::DualMixBarComponent::mouseUp (const juce::MouseEvent&)
{
    currentDrag_ = None;
}

void STRETRAudioProcessorEditor::DualMixBarComponent::mouseMove (const juce::MouseEvent& e)
{
    updateTooltipForTarget (hitTestMarker (e.position));
}

//========================== Legend width constants ==========================
namespace
{
    constexpr const char* kAmountLegendFull   = "100% AMOUNT";
    constexpr const char* kAmountLegendShort  = "100% AMT";
    constexpr const char* kAmountLegendInt    = "100%";

    constexpr const char* kPitchLegendFull   = "+24.00 st PITCH";
    constexpr const char* kPitchLegendShort  = "+24.00st";
    constexpr const char* kPitchLegendInt    = "+24.00st";

    constexpr const char* kJitterLegendFull  = "100% JIT";
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
    constexpr const char* kMixLegendShort  = "100% MX";
    constexpr const char* kMixLegendInt    = "100%";

    constexpr const char* kLimLegendFull   = "-36.0 dB LIM";
    constexpr const char* kLimLegendShort  = "-36.0 dB LIM";
    constexpr const char* kLimLegendInt    = "-36.0dB";

    constexpr int kValueAreaHeightPx = 44;
    constexpr int kValueAreaRightMarginPx = 24;
    constexpr int kToggleLabelGapPx = 4;
    constexpr int kResizerCornerPx = 22;
    constexpr int kToggleBoxPx = 72;
    constexpr int kToggleLegendCollisionPadPx = 6;
    constexpr int kTitleAreaExtraHeightPx = 4;
    constexpr int kTitleRightGapToInfoPx = 8;
    constexpr int kVersionGapPx = 8;

    // ── UI helper types for popup prompts ──
    struct PopupSwatchButton final : public juce::TextButton
    {
        std::function<void()> onLeftClick;
        std::function<void()> onRightClick;
        void clicked() override { if (onLeftClick) onLeftClick(); else juce::TextButton::clicked(); }
        void mouseUp (const juce::MouseEvent& e) override
        { if (e.mods.isPopupMenu()) { if (onRightClick) onRightClick(); return; } juce::TextButton::mouseUp (e); }
    };

    struct PopupClickableLabel final : public juce::Label
    {
        using juce::Label::Label;
        std::function<void()> onClick;
        void mouseUp (const juce::MouseEvent& e) override
        { juce::Label::mouseUp (e); if (! e.mods.isPopupMenu() && onClick) onClick(); }
    };

    struct TextLayoutLabel final : public juce::Label
    {
        using juce::Label::Label;
        void paint (juce::Graphics& g) override
        {
            g.fillAll (findColour (backgroundColourId));
            if (isBeingEdited()) return;
            const auto f = getFont();
            const auto area = getBorderSize().subtractedFrom (getLocalBounds()).toFloat();
            juce::AttributedString as;
            as.append (getText(), f, findColour (textColourId).withMultipliedAlpha (isEnabled() ? 1.0f : 0.5f));
            as.setJustification (getJustificationType());
            juce::TextLayout layout;
            layout.createLayout (as, area.getWidth());
            layout.draw (g, area);
        }
    };

    int getToggleVisualBoxSidePx (const juce::Component& button)
    {
        const int h = button.getHeight();
        return juce::jlimit (14, juce::jmax (14, h - 2), (int) std::lround ((double) h * 0.65));
    }

    int getToggleVisualBoxLeftPx (const juce::Component& button)
    {
        return button.getX() + 2;
    }

    juce::Rectangle<int> makeToggleLabelArea (const juce::Component& button,
                                              int collisionRight,
                                              const juce::String& fullLabel,
                                              const juce::String& shortLabel)
    {
        const int visualRight = getToggleVisualBoxLeftPx (button) + getToggleVisualBoxSidePx (button);
        const int x = visualRight + kToggleLabelGapPx;
        const auto& labelFont = kBoldFont40();
        const int fullW  = stringWidth (labelFont, fullLabel) + 2;
        const int shortW = stringWidth (labelFont, shortLabel) + 2;
        const int maxW   = juce::jmax (0, collisionRight - x);
        const int w = (fullW <= maxW) ? fullW : juce::jmin (shortW, maxW);
        return { x, button.getY(), w, button.getHeight() };
    }

    juce::String chooseToggleLabel (const juce::Component& button,
                                   int collisionRight,
                                   const juce::String& fullLabel,
                                   const juce::String& shortLabel)
    {
        const int visualRight = getToggleVisualBoxLeftPx (button) + getToggleVisualBoxSidePx (button);
        const int x = visualRight + kToggleLabelGapPx;
        const int fullW = stringWidth (kBoldFont40(), fullLabel) + 2;
        return (fullW <= juce::jmax (0, collisionRight - x)) ? fullLabel : shortLabel;
    }
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
    ioSectionExpanded_ = audioProcessor.getUiIoExpanded();

    for (int i = 0; i < 2; ++i)
        customPalette[(size_t) i] = audioProcessor.getUiCustomPaletteColour (i);

    setOpaque (true);
    setBufferedToImage (true);

    applyActivePalette();
    setLookAndFeel (&lnf);
    tooltipWindow = std::make_unique<juce::TooltipWindow> (this, 250);
    tooltipWindow->setLookAndFeel (&lnf);
    tooltipWindow->setAlwaysOnTop (true);
    tooltipWindow->setInterceptsMouseClicks (false, false);

    setResizable (true, true);
    setResizeLimits (kMinW, kMinH, kMaxW, kMaxH);
    resizeConstrainer.setMinimumSize (kMinW, kMinH);
    resizeConstrainer.setMaximumSize (kMaxW, kMaxH);
    resizerCorner = std::make_unique<juce::ResizableCornerComponent> (this, &resizeConstrainer);
    addAndMakeVisible (*resizerCorner);
    resizerCorner->addMouseListener (this, true);

    addAndMakeVisible (promptOverlay);
    promptOverlay.setInterceptsMouseClicks (true, true);
    promptOverlay.setVisible (false);

    const int restoredW = juce::jlimit (kMinW, kMaxW, audioProcessor.getUiEditorWidth());
    const int restoredH = juce::jlimit (kMinH, kMaxH, audioProcessor.getUiEditorHeight());
    suppressSizePersistence = true;
    setSize (restoredW, restoredH);
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
    inputSlider.setVisible (false);
    outputSlider.setVisible (false);
    tiltSlider.setVisible (false);
    panSlider.setVisible (false);
    mixSlider.setVisible (false);
    limThresholdSlider.setVisible (false);

    filterBar_.setOwner (this);
    filterBar_.setScheme (activeScheme);
    addAndMakeVisible (filterBar_);
    filterBar_.setVisible (false);
    filterBar_.updateFromProcessor();

    // Chaos filter button + tooltip overlay
    chaosFilterButton.setButtonText ("");
    addAndMakeVisible (chaosFilterButton);
    chaosFilterButton.setVisible (false);
    {
        const float savedAmtF = audioProcessor.apvts.getRawParameterValue (STRETRAudioProcessor::kParamChaosAmtFilter)->load();
        const float savedSpdF = audioProcessor.apvts.getRawParameterValue (STRETRAudioProcessor::kParamChaosSpdFilter)->load();
        chaosFilterDisplay.setText ("", juce::dontSendNotification);
        chaosFilterDisplay.setInterceptsMouseClicks (true, false);
        chaosFilterDisplay.addMouseListener (this, false);
        chaosFilterDisplay.setTooltip (formatChaosTooltip (savedAmtF, savedSpdF));
        chaosFilterDisplay.setColour (juce::Label::backgroundColourId, juce::Colours::transparentBlack);
        chaosFilterDisplay.setColour (juce::Label::outlineColourId, juce::Colours::transparentBlack);
        chaosFilterDisplay.setOpaque (false);
        addAndMakeVisible (chaosFilterDisplay);
        chaosFilterDisplay.setVisible (false);
    }

    // Chaos delay button + tooltip overlay
    chaosDelayButton.setButtonText ("");
    addAndMakeVisible (chaosDelayButton);
    chaosDelayButton.setVisible (false);
    {
        const float savedAmtD = audioProcessor.apvts.getRawParameterValue (STRETRAudioProcessor::kParamChaosAmt)->load();
        const float savedSpdD = audioProcessor.apvts.getRawParameterValue (STRETRAudioProcessor::kParamChaosSpd)->load();
        chaosDelayDisplay.setText ("", juce::dontSendNotification);
        chaosDelayDisplay.setInterceptsMouseClicks (true, false);
        chaosDelayDisplay.addMouseListener (this, false);
        chaosDelayDisplay.setTooltip (formatChaosTooltip (savedAmtD, savedSpdD));
        chaosDelayDisplay.setColour (juce::Label::backgroundColourId, juce::Colours::transparentBlack);
        chaosDelayDisplay.setColour (juce::Label::outlineColourId, juce::Colours::transparentBlack);
        chaosDelayDisplay.setOpaque (false);
        addAndMakeVisible (chaosDelayDisplay);
        chaosDelayDisplay.setVisible (false);
    }

    reverseButton.setButtonText ("");
    triggerButton.setButtonText ("");
    alignButton.setButtonText ("");
    pdcButton.setButtonText ("");

    addAndMakeVisible (reverseButton);
    addAndMakeVisible (triggerButton);
    addAndMakeVisible (alignButton);
    addAndMakeVisible (pdcButton);

    pdcDisplay.setText ("", juce::dontSendNotification);
    pdcDisplay.setInterceptsMouseClicks (true, false);
    pdcDisplay.addMouseListener (this, false);
    pdcDisplay.setTooltip (formatPdcTooltip (pdcButton.getToggleState(), getCurrentMaxFftWindow()));
    pdcDisplay.setColour (juce::Label::backgroundColourId, juce::Colours::transparentBlack);
    pdcDisplay.setColour (juce::Label::outlineColourId, juce::Colours::transparentBlack);
    pdcDisplay.setOpaque (false);
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
            combo.setJustificationType (juce::Justification::centred);
            combo.setLookAndFeel (&lnf);
            combo.setVisible (false);
        };
        setupModeCombo (modeInCombo);
        setupModeCombo (modeOutCombo);

        addAndMakeVisible (sumBusCombo);
        sumBusCombo.addItem ("ST", 1);
        sumBusCombo.addItem (juce::String::fromUTF8 (u8"\u2192M"), 2);
        sumBusCombo.addItem (juce::String::fromUTF8 (u8"\u2192S"), 3);
        sumBusCombo.setJustificationType (juce::Justification::centred);
        sumBusCombo.setLookAndFeel (&lnf);
        sumBusCombo.setVisible (false);

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
        limModeCombo.setJustificationType (juce::Justification::centred);
        limModeCombo.setLookAndFeel (&lnf);
        limModeCombo.setVisible (false);
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
            combo.setJustificationType (juce::Justification::centred);
            combo.setLookAndFeel (&lnf);
            combo.setVisible (false);
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
        mixModeCombo.setJustificationType (juce::Justification::centred);
        mixModeCombo.setLookAndFeel (&lnf);
        mixModeCombo.setVisible (false);
        mixModeAttachment = std::make_unique<ComboBoxAttachment> (audioProcessor.apvts, STRETRAudioProcessor::kParamMixMode, mixModeCombo);
    }

    // Filter Position combo (POST / PRE)
    {
        addAndMakeVisible (filterPosCombo);
        filterPosCombo.addItem (juce::String::fromUTF8 (u8"F\u25bc T\u25bc"), 1);
        filterPosCombo.addItem (juce::String::fromUTF8 (u8"F\u25b2 T\u25b2"), 2);
        filterPosCombo.addItem (juce::String::fromUTF8 (u8"F\u25b2 T\u25bc"), 3);
        filterPosCombo.addItem (juce::String::fromUTF8 (u8"F\u25bc T\u25b2"), 4);
        filterPosCombo.setJustificationType (juce::Justification::centred);
        filterPosCombo.setLookAndFeel (&lnf);
        filterPosCombo.setVisible (false);
        filterPosAttachment = std::make_unique<ComboBoxAttachment> (audioProcessor.apvts, STRETRAudioProcessor::kParamFilterPos, filterPosCombo);
    }

    // Dual Mix Bar (SEND mode)
    addAndMakeVisible (dualMixBar_);
    dualMixBar_.setOwner (this);
    dualMixBar_.setVisible (false);

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

    for (auto* paramId : kUiMirrorParamIds)
        audioProcessor.apvts.addParameterListener (paramId, this);
    audioProcessor.apvts.addParameterListener (STRETRAudioProcessor::kParamPdc, this);
    audioProcessor.apvts.addParameterListener (STRETRAudioProcessor::kParamWindow, this);
    audioProcessor.apvts.addParameterListener (STRETRAudioProcessor::kParamMaxWindow, this);

    juce::Component::SafePointer<STRETRAudioProcessorEditor> safeThis (this);
    juce::MessageManager::callAsync ([safeThis]() { if (safeThis) safeThis->applyPersistedUiStateFromProcessor (true, true); });
    juce::Timer::callAfterDelay (250, [safeThis]() { if (safeThis) safeThis->applyPersistedUiStateFromProcessor (true, true); });
    juce::Timer::callAfterDelay (750, [safeThis]() { if (safeThis) safeThis->applyPersistedUiStateFromProcessor (true, true); });

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
    STREScheme s;
    s.bg = palette[1]; s.fg = palette[0]; s.outline = palette[0]; s.text = palette[0];
    activeScheme = s;
    lnf.setScheme (activeScheme);
    filterBar_.setScheme (activeScheme);
    dualMixBar_.setScheme (activeScheme);

    for (auto* combo : { &modeInCombo, &modeOutCombo, &sumBusCombo, &limModeCombo, &invPolCombo, &invStrCombo, &mixModeCombo, &filterPosCombo })
    {
        combo->setColour (juce::ComboBox::textColourId,       s.text);
        combo->setColour (juce::ComboBox::backgroundColourId, s.bg);
        combo->setColour (juce::ComboBox::outlineColourId,    s.outline);
    }
}

void STRETRAudioProcessorEditor::applyCrtState (bool enabled)
{
    crtEnabled = enabled;
    crtEffect.setEnabled (crtEnabled);
    setComponentEffect (crtEnabled ? &crtEffect : nullptr);
    stopTimer();
    startTimerHz (crtEnabled ? kCrtTimerHz : kIdleTimerHz);
}

void STRETRAudioProcessorEditor::applyLabelTextColour (juce::Label& label, juce::Colour colour)
{
    label.setColour (juce::Label::textColourId, colour);
}

void STRETRAudioProcessorEditor::sliderValueChanged (juce::Slider* slider)
{
    if (slider == &windowSlider && ! clampingWindowSlider_)
    {
        const int engineVal = getCurrentEngineValue();
        const int effectiveWindow = getEffectiveWindowValue (windowSlider.getValue());
        if (isCurrentEngineFft() && (int) std::lround (windowSlider.getValue()) != effectiveWindow)
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
        return s == &amountSlider || s == &pitchSlider || s == &jitterSlider || s == &grainSlider
            || s == &inputSlider  || s == &outputSlider || s == &mixSlider;
    };

    if (isBar (slider)) { repaint (getRowRepaintBounds (*slider)); return; }
    repaint();
}

void STRETRAudioProcessorEditor::setPromptOverlayActive (bool shouldBeActive)
{
    if (promptOverlayActive == shouldBeActive) return;
    promptOverlayActive = shouldBeActive;
    promptOverlay.setBounds (getLocalBounds());
    promptOverlay.setVisible (shouldBeActive);
    if (shouldBeActive) promptOverlay.toFront (false);

    const bool enable = ! shouldBeActive;
    const std::array<juce::Component*, 12> controls {
        &amountSlider, &pitchSlider, &grainSlider, &engineSlider, &windowSlider, &jitterSlider, &styleSlider,
        &reverseButton, &triggerButton, &alignButton, &pdcButton, &pdcDisplay
    };
    for (auto* c : controls) c->setEnabled (enable);
    if (resizerCorner) resizerCorner->setEnabled (enable);
    repaint();
    if (! shouldBeActive) updateEngineControls();  // re-apply engine dimming
    if (promptOverlayActive) promptOverlay.toFront (false);
    anchorEditorOwnedPromptWindows (*this, lnf);
}

void STRETRAudioProcessorEditor::moved()
{
    if (promptOverlayActive) promptOverlay.toFront (false);
    anchorEditorOwnedPromptWindows (*this, lnf);
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
        || parameterID == STRETRAudioProcessor::kParamUiColor0
        || parameterID == STRETRAudioProcessor::kParamUiColor1)
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
            mixSlider.setVisible (! isSendMode);
            dualMixBar_.setVisible (isSendMode);
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
            mixSlider.setVisible (false);
            dualMixBar_.setVisible (true);
        }
        else if (! isSend && dualMixBar_.isVisible())
        {
            dualMixBar_.setVisible (false);
            mixSlider.setVisible (true);
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
        const bool targetIoExpanded = audioProcessor.getUiIoExpanded();

        std::array<juce::Colour, 2> targetCustomPalette;
        for (int i = 0; i < 2; ++i)
            targetCustomPalette[(size_t) i] = audioProcessor.getUiCustomPaletteColour (i);

        bool paletteChanged = false;
        for (int i = 0; i < 2; ++i)
            if (targetCustomPalette[(size_t) i] != customPalette[(size_t) i])
            { customPalette[(size_t) i] = targetCustomPalette[(size_t) i]; paletteChanged = true; }

        const bool paletteSwitchChanged = targetUseCustomPalette != useCustomPalette;
        const bool fxChanged = targetCrtEnabled != crtEnabled;
        const bool ioChanged = targetIoExpanded != ioSectionExpanded_;

        if (ioChanged) { ioSectionExpanded_ = targetIoExpanded; resized(); }
        if (paletteSwitchChanged) useCustomPalette = targetUseCustomPalette;
        if (fxChanged) applyCrtState (targetCrtEnabled);
        if (paletteChanged || paletteSwitchChanged) applyActivePalette();
        if (paletteChanged || paletteSwitchChanged || fxChanged || ioChanged) repaint();
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
    s.setColour (juce::Slider::trackColourId, juce::Colours::transparentBlack);
    s.setColour (juce::Slider::backgroundColourId, juce::Colours::transparentBlack);
    s.setColour (juce::Slider::thumbColourId, juce::Colours::transparentBlack);
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
    return juce::String (juce::roundToInt (v)) + "% JIT";
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
        windowSlider.setRange ((double) STRETRAudioProcessor::kWindowMin,
                               (double) STRETRAudioProcessor::kWindowMax,
                               1.0);
        windowSlider.setSkewFactor (0.5);
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
    pdcButton.setTooltip (tooltip);
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
    return juce::String (pct) + "% MX";
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
    HorizontalLayoutMetrics m;
    m.barW = (int) std::round (editorW * 0.455);
    m.valuePad = (int) std::round (editorW * 0.02);
    m.valueW = valueColW;
    m.contentW = m.barW + m.valuePad + m.valueW;
    m.leftX = juce::jmax (6, (editorW - m.contentW) / 2);
    return m;
}

STRETRAudioProcessorEditor::VerticalLayoutMetrics
STRETRAudioProcessorEditor::buildVerticalLayout (int editorH, int biasY, bool ioExpanded)
{
    VerticalLayoutMetrics m;
    m.rhythm = juce::jlimit (6, 16, (int) std::round (editorH * 0.018));
    const int nominalBarH = juce::jlimit (14, 120, m.rhythm * 6);
    const int nominalGapY = juce::jmax (4, m.rhythm * 4);

    m.titleH = juce::jlimit (24, 56, m.rhythm * 4);
    m.titleAreaH = m.titleH + 4;
    const int computedTitleTopPad = 6 + biasY;
    m.titleTopPad = (computedTitleTopPad > 8) ? computedTitleTopPad : 8;
    const int titleGap = m.titleTopPad;
    m.topMargin = m.titleTopPad + m.titleAreaH + titleGap;
    m.betweenSlidersAndButtons = juce::jmax (8, m.rhythm * 2);
    m.bottomMargin = m.titleTopPad;

    m.box = juce::jlimit (40, kToggleBoxPx, (int) std::round (editorH * 0.085));
    m.btnRowGap = juce::jlimit (4, 14, (int) std::round (editorH * 0.008));

    // Only 2 button rows for STRE-TR (ALIGN+PDC, RVS+TRG)
    m.btnRow2Y = editorH - m.bottomMargin - m.box;
    m.btnRow1Y = m.btnRow2Y - m.btnRowGap - m.box;

    // When IO is expanded, buttons are hidden and chaos sits at the bottom
    m.chaosRowY = ioExpanded ? (editorH - m.bottomMargin - m.box) : 0;

    const int sliderBottomRef = ioExpanded ? m.chaosRowY : m.btnRow1Y;
    m.availableForSliders = juce::jmax (40, sliderBottomRef - m.betweenSlidersAndButtons - m.topMargin);

    // Keep the expanded compact-menu density identical to DISP/ECHO/GRA.
    const int numSliders = ioExpanded ? 10 : 7;
    const int numGaps    = ioExpanded ? 10 : 7;

    m.toggleBarH = 20;
    const int spaceForScale = juce::jmax (40, m.availableForSliders - m.toggleBarH);
    const int nominalStack = numSliders * nominalBarH + numGaps * nominalGapY;
    const double stackScale = nominalStack > 0 ? juce::jmin (1.0, (double) spaceForScale / (double) nominalStack) : 1.0;

    m.barH = juce::jmax (14, (int) std::round (nominalBarH * stackScale));
    m.gapY = juce::jmax (4,  (int) std::round (nominalGapY * stackScale));

    auto stackHeight = [&]() { return numSliders * m.barH + numGaps * m.gapY; };
    while (stackHeight() > spaceForScale && m.gapY > 4) --m.gapY;
    while (stackHeight() > spaceForScale && m.barH > 14) --m.barH;

    m.topY = m.topMargin;
    m.toggleBarY = m.topY;
    return m;
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
        const int valueX = bb.getRight() + cachedHLayout_.valuePad;
        const int maxW = juce::jmax (0, getWidth() - valueX - kValueAreaRightMarginPx);
        const int vw = juce::jmin (cachedHLayout_.valueW, maxW);
        const int y = bb.getCentreY() - (kValueAreaHeightPx / 2);
        cachedFilterValueArea_ = { valueX, y, juce::jmax (0, vw), kValueAreaHeightPx };
    }
    else cachedFilterValueArea_ = {};

    if (tiltSlider.isVisible())
    {
        const auto& bb = tiltSlider.getBounds();
        const int valueX = bb.getRight() + cachedHLayout_.valuePad;
        const int maxW = juce::jmax (0, getWidth() - valueX - kValueAreaRightMarginPx);
        const int vw = juce::jmin (cachedHLayout_.valueW, maxW);
        const int y = bb.getCentreY() - (kValueAreaHeightPx / 2);
        cachedTiltValueArea_ = { valueX, y, juce::jmax (0, vw), kValueAreaHeightPx };
    }
    else cachedTiltValueArea_ = {};

    if (panSlider.isVisible())
    {
        const auto& bb = panSlider.getBounds();
        const int valueX = bb.getRight() + cachedHLayout_.valuePad;
        const int maxW = juce::jmax (0, getWidth() - valueX - kValueAreaRightMarginPx);
        const int vw = juce::jmin (cachedHLayout_.valueW, maxW);
        const int y = bb.getCentreY() - (kValueAreaHeightPx / 2);
        cachedPanValueArea_ = { valueX, y, juce::jmax (0, vw), kValueAreaHeightPx };
    }
    else cachedPanValueArea_ = {};

    if (limThresholdSlider.isVisible())
    {
        const auto& bb = limThresholdSlider.getBounds();
        const int valueX = bb.getRight() + cachedHLayout_.valuePad;
        const int maxW = juce::jmax (0, getWidth() - valueX - kValueAreaRightMarginPx);
        const int vw = juce::jmin (cachedHLayout_.valueW, maxW);
        const int y = bb.getCentreY() - (kValueAreaHeightPx / 2);
        cachedLimThresholdValueArea_ = { valueX, y, juce::jmax (0, vw), kValueAreaHeightPx };
    }
    else cachedLimThresholdValueArea_ = {};

    if (chaosFilterButton.isVisible())
        cachedChaosArea_ = chaosFilterButton.getBounds().getUnion (chaosDelayButton.getBounds());
    else cachedChaosArea_ = {};

    cachedToggleBarArea_ = { cachedHLayout_.leftX, cachedVLayout_.toggleBarY,
                             cachedHLayout_.contentW, cachedVLayout_.toggleBarH };
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
    const int valueX = barBounds.getRight() + cachedHLayout_.valuePad;
    const int maxW = juce::jmax (0, getWidth() - valueX - kValueAreaRightMarginPx);
    const int valueW = juce::jmin (cachedHLayout_.valueW, maxW);
    const int y = barBounds.getCentreY() - (kValueAreaHeightPx / 2);
    return { valueX, y, juce::jmax (0, valueW), kValueAreaHeightPx };
}

juce::Slider* STRETRAudioProcessorEditor::getSliderForValueAreaPoint (juce::Point<int> p)
{
    juce::Slider* sliders[12] = {
        &amountSlider, &pitchSlider, &grainSlider, &engineSlider, &windowSlider, &jitterSlider, &styleSlider,
        &inputSlider, &outputSlider, &tiltSlider, &panSlider, &mixSlider
    };
    for (int i = 0; i < 12; ++i)
        if (cachedValueAreas_[(size_t) i].contains (p))
            return sliders[i];
    return nullptr;
}

juce::Rectangle<int> STRETRAudioProcessorEditor::getReverseLabelArea() const
{
    return makeToggleLabelArea (reverseButton, triggerButton.getX() - kToggleLegendCollisionPadPx, "REVERSE", "RVS");
}

juce::Rectangle<int> STRETRAudioProcessorEditor::getTriggerLabelArea() const
{
    return makeToggleLabelArea (triggerButton, getWidth() - kToggleLegendCollisionPadPx, "TRIGGER", "TRG");
}

juce::Rectangle<int> STRETRAudioProcessorEditor::getAlignLabelArea() const
{
    return makeToggleLabelArea (alignButton, pdcButton.getX() - kToggleLegendCollisionPadPx, "ALIGN", "ALN");
}

juce::Rectangle<int> STRETRAudioProcessorEditor::getPdcLabelArea() const
{
    return makeToggleLabelArea (pdcButton, getWidth() - kToggleLegendCollisionPadPx, "PDC", "PDC");
}

juce::Rectangle<int> STRETRAudioProcessorEditor::getChaosLabelArea() const
{
    if (! chaosFilterButton.isVisible()) return {};
    return makeToggleLabelArea (chaosFilterButton,
                                chaosDelayButton.getX() - kToggleLegendCollisionPadPx,
                                "CHSF", "CHSF");
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

    if (cachedToggleBarArea_.contains (p))
    {
        ioSectionExpanded_ = ! ioSectionExpanded_;
        audioProcessor.setUiIoExpanded (ioSectionExpanded_);
        resized(); repaint(); return;
    }

    if (e.mods.isPopupMenu())
    {
        if (auto* slider = getSliderForValueAreaPoint (p))
        { openNumericEntryPopupForSlider (*slider); return; }
    }

    {
        auto infoArea = getInfoIconArea();
        if (crtEnabled) infoArea = infoArea.expanded (4, 0);
        if (infoArea.contains (p)) { openInfoPopup(); return; }
    }

    if (reverseButton.isVisible() && getReverseLabelArea().contains (p) && reverseButton.isEnabled())
    { reverseButton.setToggleState (! reverseButton.getToggleState(), juce::sendNotificationSync); return; }

    if (triggerButton.isVisible() && getTriggerLabelArea().contains (p))
    { triggerButton.setToggleState (! triggerButton.getToggleState(), juce::sendNotificationSync); return; }

    if (alignButton.isVisible() && getAlignLabelArea().contains (p))
    { alignButton.setToggleState (! alignButton.getToggleState(), juce::sendNotificationSync); return; }

    if (pdcButton.isVisible() && (getPdcLabelArea().contains (p) || pdcDisplay.getBounds().contains (p)))
    {
        if (e.mods.isPopupMenu())
            openPdcMaxWindowPrompt();
        else
            pdcButton.setToggleState (! pdcButton.getToggleState(), juce::sendNotificationSync);
        updatePdcTooltip();
        return;
    }

    if (chaosFilterButton.isVisible() && (getChaosLabelArea().contains (p) || chaosFilterDisplay.getBounds().contains (p)))
    {
        if (e.mods.isPopupMenu()) openChaosFilterPrompt();
        else chaosFilterButton.setToggleState (! chaosFilterButton.getToggleState(), juce::sendNotificationSync);
        return;
    }

    if (chaosDelayButton.isVisible())
    {
        const auto bb = chaosDelayButton.getBounds();
        const int toggleVisualSide = juce::jlimit (14, juce::jmax (14, bb.getHeight() - 2),
                                                   (int) std::lround (bb.getHeight() * 0.65));
        const int toggleHitW = toggleVisualSide + 6;
        const int lx = bb.getX() + toggleHitW + 4;
        const juce::Rectangle<int> dLabelArea { lx, bb.getY(), bb.getRight() - lx, bb.getHeight() };
        if (dLabelArea.contains (p) || chaosDelayDisplay.getBounds().contains (p))
        {
            if (e.mods.isPopupMenu()) openChaosDelayPrompt();
            else chaosDelayButton.setToggleState (! chaosDelayButton.getToggleState(), juce::sendNotificationSync);
            return;
        }
    }
}

void STRETRAudioProcessorEditor::mouseDrag (const juce::MouseEvent& e)
{
    juce::ignoreUnused (e);
    lastUserInteractionMs.store (juce::Time::getMillisecondCounter(), std::memory_order_relaxed);
}

void STRETRAudioProcessorEditor::mouseDoubleClick (const juce::MouseEvent& e)
{
    const auto p = e.getPosition();
    if (auto* slider = getSliderForValueAreaPoint (p))
    {
        if (slider == &amountSlider)      slider->setValue (kDefaultAmount, juce::sendNotificationSync);
        else if (slider == &pitchSlider)    slider->setValue (0.5, juce::sendNotificationSync);
        else if (slider == &jitterSlider) slider->setValue ((double) STRETRAudioProcessor::kJitterDefault, juce::sendNotificationSync);
        else if (slider == &grainSlider)  slider->setValue ((double) STRETRAudioProcessor::kGrainDefault, juce::sendNotificationSync);
        else if (slider == &inputSlider)  slider->setValue (kDefaultInput, juce::sendNotificationSync);
        else if (slider == &outputSlider) slider->setValue (kDefaultOutput, juce::sendNotificationSync);
        else if (slider == &mixSlider)    slider->setValue (kDefaultMix, juce::sendNotificationSync);
    }
}

//========================== Paint ==========================

void STRETRAudioProcessorEditor::paint (juce::Graphics& g)
{
    const auto& horizontalLayout = cachedHLayout_;
    const auto& verticalLayout = cachedVLayout_;
    const int W = getWidth();
    const auto scheme = activeScheme;

    g.fillAll (scheme.bg);
    g.setColour (scheme.text);

    constexpr float baseFontPx = 40.0f;
    constexpr float minFontPx = 18.0f;
    constexpr float fullShrinkFloor = baseFontPx * 0.75f;
    g.setFont (kBoldFont40());

    auto tryDrawLegend = [&] (const juce::Rectangle<int>& area,
                              const juce::String& text, float shrinkFloor) -> bool
    {
        auto t = text.trim();
        if (t.isEmpty() || area.getWidth() <= 2 || area.getHeight() <= 2) return false;
        const int split = t.lastIndexOfChar (' ');
        if (split <= 0 || split >= t.length() - 1)
        {
            g.setFont (kBoldFont40());
            return drawIfFitsWithOptionalShrink (g, area, t, baseFontPx, shrinkFloor);
        }
        const auto value  = t.substring (0, split).trimEnd();
        const auto suffix = t.substring (split + 1).trimStart();
        g.setFont (kBoldFont40());
        if (drawValueWithRightAlignedSuffix (g, area, value, suffix, false, baseFontPx, shrinkFloor))
        { g.setColour (scheme.text); return true; }
        return false;
    };

    auto drawLegendForMode = [&] (const juce::Rectangle<int>& area,
                                  const juce::String& fullLegend,
                                  const juce::String& shortLegend,
                                  const juce::String& intOnlyLegend)
    {
        if (tryDrawLegend (area, fullLegend, fullShrinkFloor)) return;
        if (tryDrawLegend (area, shortLegend, minFontPx)) return;
        g.setFont (kBoldFont40());
        drawValueNoEllipsis (g, area, intOnlyLegend, juce::String(), intOnlyLegend, baseFontPx, minFontPx);
        g.setColour (scheme.text);
    };

    // ── Title ──
    {
        const int titleH = verticalLayout.titleH;
        const int contentW = horizontalLayout.contentW;
        const int leftX = horizontalLayout.leftX;
        const int titleX = juce::jlimit (0, juce::jmax (0, W - 1), leftX);
        const int titleW = juce::jmax (0, juce::jmin (contentW, W - titleX));
        const int titleY = verticalLayout.titleTopPad;

        auto titleFont = g.getCurrentFont();
        titleFont.setHeight ((float) titleH);
        g.setFont (titleFont);

        const auto titleArea = juce::Rectangle<int> (titleX, titleY, titleW, titleH + kTitleAreaExtraHeightPx);
        const juce::String titleText ("STRE-TR");

        g.drawText (titleText, titleArea.getX(), titleArea.getY(), titleArea.getWidth(), titleArea.getHeight(),
                    juce::Justification::left, false);

        const auto infoIconArea = getInfoIconArea();
        const int titleRightLimit = infoIconArea.getX() - kTitleRightGapToInfoPx;
        const int titleMaxW = juce::jmax (0, titleRightLimit - titleArea.getX());
        const int barW = horizontalLayout.barW;
        const int titleBaseW = stringWidth (titleFont, titleText);
        const int originalTitleLimitW = juce::jmax (0, juce::jmin (titleW, barW));
        const bool originalWouldClipTitle = titleBaseW > originalTitleLimitW;

        if (titleMaxW > 0 && (originalWouldClipTitle || titleBaseW > titleMaxW))
        {
            auto fittedTitleFont = titleFont;
            fittedTitleFont.setHorizontalScale (1.0f);
            const float titleMinScale = juce::jlimit (0.4f, 1.0f, 12.0f / (float) titleH);
            for (float s = 1.0f; s >= titleMinScale; s -= 0.025f)
            {
                fittedTitleFont.setHorizontalScale (s);
                if (stringWidth (fittedTitleFont, titleText) <= titleMaxW) break;
            }
            g.setColour (scheme.text);
            g.setFont (fittedTitleFont);
            g.drawText (titleText, titleArea.getX(), titleArea.getY(), titleMaxW, titleArea.getHeight(),
                        juce::Justification::left, false);
        }

        g.setColour (scheme.text);
        auto versionFont = juce::Font (juce::FontOptions (juce::jmax (10.0f, (float) titleH * UiMetrics::versionFontRatio)).withStyle ("Bold"));
        g.setFont (versionFont);
        const int versionH = juce::jlimit (10, infoIconArea.getHeight(), (int) std::round ((double) infoIconArea.getHeight() * UiMetrics::versionHeightRatio));
        const int versionY = infoIconArea.getBottom() - versionH;
        const int desiredVersionW = juce::jlimit (28, 64, (int) std::round ((double) infoIconArea.getWidth() * UiMetrics::versionDesiredWidthRatio));
        const int versionRight = infoIconArea.getX() - kVersionGapPx;
        const int versionLeftLimit = titleArea.getX();
        const int versionX = juce::jmax (versionLeftLimit, versionRight - desiredVersionW);
        const int versionW = juce::jmax (0, versionRight - versionX);
        if (versionW > 0)
            g.drawText (juce::String ("v") + InfoContent::version,
                        versionX, versionY, versionW, versionH,
                        juce::Justification::bottomRight, false);
        g.setFont (kBoldFont40());
    }

    // ── Toggle bar (triangle + rounded horizontal bar) ──
    {
        if (! cachedToggleBarArea_.isEmpty())
        {
            const float barRadius = (float) cachedToggleBarArea_.getHeight() * 0.3f;
            g.setColour (scheme.fg.withAlpha (0.25f));
            g.fillRoundedRectangle (cachedToggleBarArea_.toFloat(), barRadius);
            const float triH = (float) cachedToggleBarArea_.getHeight() * 0.8f;
            const float triW = triH * 1.125f;
            const float cx = (float) cachedToggleBarArea_.getCentreX();
            const float cy = (float) cachedToggleBarArea_.getCentreY();
            juce::Path tri;
            if (ioSectionExpanded_)
                tri.addTriangle (cx - triW * 0.5f, cy + triH * 0.35f,
                                 cx + triW * 0.5f, cy + triH * 0.35f,
                                 cx, cy - triH * 0.35f);
            else
                tri.addTriangle (cx - triW * 0.5f, cy - triH * 0.35f,
                                 cx + triW * 0.5f, cy - triH * 0.35f,
                                 cx, cy + triH * 0.35f);
            g.setColour (scheme.text);
            g.fillPath (tri);
        }
    }

    g.setColour (scheme.text);

    // ── Slider legends ──
    {
        const juce::String* fullTexts[12]  = {
            &cachedAmountTextFull, &cachedPitchTextFull, &cachedGrainTextFull, &cachedEngineTextFull,
            &cachedWindowTextFull, &cachedJitterTextFull, &cachedStyleTextFull,
            &cachedInputTextFull, &cachedOutputTextFull, &cachedTiltTextFull,
            &cachedPanTextFull, &cachedMixTextFull
        };
        const juce::String* shortTexts[12] = {
            &cachedAmountTextShort, &cachedPitchTextShort, &cachedGrainTextShort, &cachedEngineTextShort,
            &cachedWindowTextShort, &cachedJitterTextShort, &cachedStyleTextShort,
            &cachedInputTextShort, &cachedOutputTextShort, &cachedTiltTextShort,
            &cachedPanTextShort, &cachedMixTextShort
        };
        const juce::String* intTexts[12] = {
            &cachedAmountIntOnly, &cachedPitchIntOnly, &cachedGrainIntOnly, &cachedEngineIntOnly,
            &cachedWindowIntOnly, &cachedJitterIntOnly, &cachedStyleIntOnly,
            &cachedInputIntOnly, &cachedOutputIntOnly, &cachedTiltIntOnly,
            &cachedPanIntOnly, &cachedMixIntOnly
        };

        const juce::Slider* sliders[12] = {
            &amountSlider, &pitchSlider, &grainSlider, &engineSlider, &windowSlider, &jitterSlider, &styleSlider,
            &inputSlider, &outputSlider, &tiltSlider, &panSlider, &mixSlider
        };

        for (int i = 0; i < 12; ++i)
        {
            g.setColour (scheme.text.withAlpha (sliders[i]->getAlpha()));
            drawLegendForMode (cachedValueAreas_[(size_t) i], *fullTexts[i], *shortTexts[i], *intTexts[i]);
        }
        g.setColour (scheme.text);

        if (tiltSlider.isVisible() && cachedTiltValueArea_.getWidth() > 0)
            drawLegendForMode (cachedTiltValueArea_, cachedTiltTextFull, cachedTiltTextShort, cachedTiltIntOnly);
        if (filterBar_.isVisible() && cachedFilterValueArea_.getWidth() > 0)
            drawLegendForMode (cachedFilterValueArea_, cachedFilterTextFull, cachedFilterTextShort, cachedFilterTextShort);
        if (panSlider.isVisible() && cachedPanValueArea_.getWidth() > 0)
            drawLegendForMode (cachedPanValueArea_, cachedPanTextFull, cachedPanTextShort, cachedPanTextShort);

        if (limThresholdSlider.isVisible() && cachedLimThresholdValueArea_.getWidth() > 0)
            drawLegendForMode (cachedLimThresholdValueArea_, cachedLimThresholdTextFull, cachedLimThresholdTextShort, cachedLimThresholdIntOnly);

        // Compact-menu combo labels.
        if (modeInCombo.isVisible())
        {
            const auto font = juce::Font (juce::FontOptions (15.0f).withStyle ("Bold"));
            g.setColour (scheme.text);
            g.setFont (font);
            auto drawComboLabel = [&] (const juce::ComboBox& combo, const juce::String& full, const juce::String& shortTxt)
            {
                const auto area = combo.getBounds().withHeight (18).translated (0, -19);
                const float comboW = (float) combo.getWidth();
                juce::GlyphArrangement ga;
                ga.addLineOfText (font, full, 0.0f, 0.0f);
                const bool useShort = ga.getBoundingBox (0, -1, false).getWidth() > comboW;
                g.drawText (useShort ? shortTxt : full, area, juce::Justification::centred);
            };
            drawComboLabel (modeInCombo,  "MODE IN",  "IN");
            drawComboLabel (modeOutCombo, "MODE OUT", "OUT");
            drawComboLabel (sumBusCombo,  "SUM BUS",  "SUM");
            drawComboLabel (limModeCombo, "LIMIT",    "LIM");
            drawComboLabel (mixModeCombo, "MIX", "MIX");
            drawComboLabel (filterPosCombo, "F / T", "F/T");
            drawComboLabel (invPolCombo, "INV POL", "POL");
            drawComboLabel (invStrCombo, "INV STR", "STR");
        }

        // CHSF/CHSD labels
        g.setFont (kBoldFont40());
        if (chaosFilterButton.isVisible())
        {
            const auto chaosArea = getChaosLabelArea();
            if (chaosArea.getWidth() > 0)
                g.drawText ("CHSF", chaosArea, juce::Justification::left, true);
        }
        if (chaosDelayButton.isVisible())
        {
            const auto dArea = makeToggleLabelArea (chaosDelayButton,
                                                     getWidth() - kToggleLegendCollisionPadPx,
                                                     "CHSD", "CHSD");
            if (dArea.getWidth() > 0)
                g.drawText ("CHSD", dArea, juce::Justification::left, true);
        }
    }

    // ── Toggle button labels ──
    {
        const auto& labelFont = kBoldFont40();
        g.setFont (labelFont);

        if (reverseButton.isVisible())
        {
        // Row 1: RVS + TRG
        const int rvsCR = triggerButton.getX() - kToggleLegendCollisionPadPx;
        const int trgCR = getWidth() - kToggleLegendCollisionPadPx;
        // Row 2: ALIGN + PDC
        const int alnCR = pdcButton.getX() - kToggleLegendCollisionPadPx;
        const int pdcCR = getWidth() - kToggleLegendCollisionPadPx;

        const juce::String rvsLabel = chooseToggleLabel (reverseButton, rvsCR, "REVERSE", "RVS");
        const juce::String trgLabel = chooseToggleLabel (triggerButton, trgCR, "TRIGGER", "TRG");
        const juce::String alnLabel = chooseToggleLabel (alignButton, alnCR, "ALIGN", "ALN");
        const juce::String pdcLabel = chooseToggleLabel (pdcButton, pdcCR, "PDC", "PDC");

        auto drawToggleLegend = [&] (const juce::Rectangle<int>& labelArea,
                                     const juce::String& labelText, int noCollisionRight,
                                     bool enabled = true)
        {
            const int safeW = juce::jmax (0, noCollisionRight - labelArea.getX());
            auto snapEven = [] (int v) { return v & ~1; };
            const auto drawArea = juce::Rectangle<int> (snapEven (labelArea.getX()), snapEven (labelArea.getY()),
                                                        snapEven (safeW), labelArea.getHeight());
            if (! enabled)
                g.setColour (scheme.text.withAlpha (0.35f));
            g.drawText (labelText, drawArea.getX(), drawArea.getY(), drawArea.getWidth(), drawArea.getHeight(),
                        juce::Justification::left, true);
            if (! enabled)
                g.setColour (scheme.text);
        };

        drawToggleLegend (getReverseLabelArea(), rvsLabel, rvsCR, reverseButton.isEnabled());
        drawToggleLegend (getTriggerLabelArea(), trgLabel, trgCR);
        drawToggleLegend (getAlignLabelArea(), alnLabel, alnCR);
        drawToggleLegend (getPdcLabelArea(), pdcLabel, pdcCR);
        }
    }

    // ── Info gear icon ──
    g.setColour (scheme.text);
    {
        if (cachedInfoGearPath.isEmpty()) updateInfoIconCache();
        g.setColour (scheme.text);
        g.fillPath (cachedInfoGearPath);
        g.strokePath (cachedInfoGearPath, juce::PathStrokeType (1.0f));
        g.setColour (scheme.bg);
        g.fillEllipse (cachedInfoGearHole);
    }
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

    const int mainTop = verticalLayout.toggleBarY + verticalLayout.toggleBarH + verticalLayout.gapY;
    const int step = verticalLayout.barH + verticalLayout.gapY;

    if (ioSectionExpanded_)
    {
        inputSlider.setBounds  (horizontalLayout.leftX, mainTop + 0 * step, horizontalLayout.barW, verticalLayout.barH);
        outputSlider.setBounds (horizontalLayout.leftX, mainTop + 1 * step, horizontalLayout.barW, verticalLayout.barH);
        tiltSlider.setBounds   (horizontalLayout.leftX, mainTop + 2 * step, horizontalLayout.barW, verticalLayout.barH);
        filterBar_.setBounds   (horizontalLayout.leftX, mainTop + 3 * step, horizontalLayout.barW, verticalLayout.barH);
        panSlider.setBounds    (horizontalLayout.leftX, mainTop + 4 * step, horizontalLayout.barW, verticalLayout.barH);
        mixSlider.setBounds    (horizontalLayout.leftX, mainTop + 5 * step, horizontalLayout.barW, verticalLayout.barH);
        dualMixBar_.setBounds  (horizontalLayout.leftX, mainTop + 5 * step, horizontalLayout.barW, verticalLayout.barH);
        limThresholdSlider.setBounds (horizontalLayout.leftX, mainTop + 6 * step, horizontalLayout.barW, verticalLayout.barH);

        // Match the compact-menu combo block used by DISP/FREQ/ECHO/GRA.
        {
            const int comboGap = 4;
            const int totalW = horizontalLayout.barW + horizontalLayout.valuePad + horizontalLayout.valueW;
            const int comboW = (totalW - comboGap * 3) / 4;
            const int comboH = juce::jlimit (38, 48, verticalLayout.barH + 14);
            const int labelOffset = 19;
            const int comboBlockH = labelOffset + comboH + comboGap + labelOffset + comboH;
            const int blockTopLimit = limThresholdSlider.getBottom() + verticalLayout.gapY;
            const int blockBottomLimit = verticalLayout.chaosRowY - verticalLayout.gapY;
            const int availableBlockH = juce::jmax (comboBlockH, blockBottomLimit - blockTopLimit);
            const int visualTop = blockTopLimit + juce::jmax (0, (availableBlockH - comboBlockH) / 2);
            const int modeY = visualTop + labelOffset;
            modeInCombo.setBounds  (horizontalLayout.leftX,                           modeY, comboW, comboH);
            modeOutCombo.setBounds (horizontalLayout.leftX + (comboW + comboGap),      modeY, comboW, comboH);
            sumBusCombo.setBounds  (horizontalLayout.leftX + (comboW + comboGap) * 2,  modeY, comboW, comboH);
            limModeCombo.setBounds (horizontalLayout.leftX + (comboW + comboGap) * 3,  modeY, comboW, comboH);
        }

        // Invert Polarity / Invert Stereo / Mix Mode / Filter Pos — 4 combos on row 8
        {
            const int invY = modeInCombo.getY() + modeInCombo.getHeight() + 4 + 19;
            const int comboGap = 4;
            const int totalW = horizontalLayout.barW + horizontalLayout.valuePad + horizontalLayout.valueW;
            const int comboW = (totalW - comboGap * 3) / 4;
            const int comboH = juce::jlimit (38, 48, verticalLayout.barH + 14);
            mixModeCombo.setBounds  (horizontalLayout.leftX,                          invY, comboW, comboH);
            filterPosCombo.setBounds(horizontalLayout.leftX + (comboW + comboGap),     invY, comboW, comboH);
            invPolCombo.setBounds   (horizontalLayout.leftX + (comboW + comboGap) * 2, invY, comboW, comboH);
            invStrCombo.setBounds   (horizontalLayout.leftX + (comboW + comboGap) * 3, invY, comboW, comboH);
        }

        const int chaosY = verticalLayout.chaosRowY;
        const int chaosH = verticalLayout.box;
        const int chaosRightX = horizontalLayout.leftX + horizontalLayout.barW + horizontalLayout.valuePad;
        const int chaosLeftW  = chaosRightX - horizontalLayout.leftX;
        const int chaosRightW = horizontalLayout.leftX + horizontalLayout.contentW - chaosRightX;
        chaosFilterButton.setBounds  (horizontalLayout.leftX, chaosY, chaosLeftW,  chaosH);
        chaosFilterDisplay.setBounds (horizontalLayout.leftX, chaosY, chaosLeftW,  chaosH);
        chaosDelayButton.setBounds   (chaosRightX,            chaosY, chaosRightW, chaosH);
        chaosDelayDisplay.setBounds  (chaosRightX,            chaosY, chaosRightW, chaosH);

        inputSlider.setVisible (true);   outputSlider.setVisible (true);
        tiltSlider.setVisible (true);    filterBar_.setVisible (true);
        panSlider.setVisible (true);     mixSlider.setVisible (true);
        limThresholdSlider.setVisible (true);
        modeInCombo.setVisible (true);   modeOutCombo.setVisible (true);
        sumBusCombo.setVisible (true);   limModeCombo.setVisible (true);
        invPolCombo.setVisible (true);   invStrCombo.setVisible (true);
        mixModeCombo.setVisible (true);  filterPosCombo.setVisible (true);
        chaosFilterButton.setVisible (true);  chaosFilterDisplay.setVisible (true);
        chaosDelayButton.setVisible (true);   chaosDelayDisplay.setVisible (true);

        {
            const bool isSendMode = mixModeCombo.getSelectedId() == 2;
            mixSlider.setVisible (! isSendMode);
            dualMixBar_.setVisible (isSendMode);
        }

        reverseButton.setVisible (false);
        triggerButton.setVisible (false);
        alignButton.setVisible (false);
        pdcButton.setVisible (false);
        pdcDisplay.setVisible (false);

        amountSlider.setBounds (0, 0, 0, 0); pitchSlider.setBounds (0, 0, 0, 0);
        grainSlider.setBounds (0, 0, 0, 0); engineSlider.setBounds (0, 0, 0, 0);
        windowSlider.setBounds (0, 0, 0, 0); jitterSlider.setBounds (0, 0, 0, 0);
        styleSlider.setBounds (0, 0, 0, 0);
        amountSlider.setVisible (false); pitchSlider.setVisible (false);
        grainSlider.setVisible (false); engineSlider.setVisible (false);
        windowSlider.setVisible (false); jitterSlider.setVisible (false);
        styleSlider.setVisible (false);
    }
    else
    {
        amountSlider.setBounds (horizontalLayout.leftX, mainTop + 0 * step, horizontalLayout.barW, verticalLayout.barH);
        pitchSlider.setBounds    (horizontalLayout.leftX, mainTop + 1 * step, horizontalLayout.barW, verticalLayout.barH);
        grainSlider.setBounds  (horizontalLayout.leftX, mainTop + 2 * step, horizontalLayout.barW, verticalLayout.barH);
        engineSlider.setBounds (horizontalLayout.leftX, mainTop + 3 * step, horizontalLayout.barW, verticalLayout.barH);
        windowSlider.setBounds (horizontalLayout.leftX, mainTop + 4 * step, horizontalLayout.barW, verticalLayout.barH);
        jitterSlider.setBounds (horizontalLayout.leftX, mainTop + 5 * step, horizontalLayout.barW, verticalLayout.barH);
        styleSlider.setBounds  (horizontalLayout.leftX, mainTop + 6 * step, horizontalLayout.barW, verticalLayout.barH);

        amountSlider.setVisible (true); pitchSlider.setVisible (true);
        grainSlider.setVisible (true); engineSlider.setVisible (true);
        windowSlider.setVisible (true); jitterSlider.setVisible (true);
        styleSlider.setVisible (true);

        inputSlider.setBounds (0, 0, 0, 0);  outputSlider.setBounds (0, 0, 0, 0);
        tiltSlider.setBounds (0, 0, 0, 0);   mixSlider.setBounds (0, 0, 0, 0);
        panSlider.setBounds (0, 0, 0, 0);    filterBar_.setBounds (0, 0, 0, 0);
        limThresholdSlider.setBounds (0, 0, 0, 0);

        inputSlider.setVisible (false);  outputSlider.setVisible (false);
        tiltSlider.setVisible (false);   mixSlider.setVisible (false);
        panSlider.setVisible (false);    filterBar_.setVisible (false);
        limThresholdSlider.setVisible (false);
        dualMixBar_.setBounds (0, 0, 0, 0);
        dualMixBar_.setVisible (false);
        chaosFilterButton.setVisible (false);  chaosFilterDisplay.setVisible (false);
        chaosDelayButton.setVisible (false);   chaosDelayDisplay.setVisible (false);
        modeInCombo.setVisible (false);  modeOutCombo.setVisible (false);
        sumBusCombo.setVisible (false);  limModeCombo.setVisible (false);
        invPolCombo.setVisible (false);  invStrCombo.setVisible (false);
        mixModeCombo.setVisible (false); filterPosCombo.setVisible (false);

        reverseButton.setVisible (true);
        triggerButton.setVisible (true);
        alignButton.setVisible (true);
        pdcButton.setVisible (true);
        pdcDisplay.setVisible (true);
    }

    // Button rows
    const int buttonAreaX = horizontalLayout.leftX;
    const int toggleVisualSide = juce::jlimit (14,
                                               juce::jmax (14, verticalLayout.box - 2),
                                               (int) std::lround ((double) verticalLayout.box * 0.65));
    const int toggleHitW = toggleVisualSide + 6;
    const int leftBlockX = buttonAreaX;
    const int rightBlockX = horizontalLayout.leftX + horizontalLayout.barW + horizontalLayout.valuePad;
    const int btnRow1Y = verticalLayout.btnRow1Y;
    const int btnRow2Y = verticalLayout.btnRow2Y;

    reverseButton.setBounds  (leftBlockX,  btnRow1Y, toggleHitW, verticalLayout.box);
    triggerButton.setBounds  (rightBlockX, btnRow1Y, toggleHitW, verticalLayout.box);
    alignButton.setBounds    (leftBlockX,  btnRow2Y, toggleHitW, verticalLayout.box);
    pdcButton.setBounds      (rightBlockX, btnRow2Y, toggleHitW, verticalLayout.box);
    pdcDisplay.setBounds (pdcButton.getBounds().getUnion (getPdcLabelArea()));

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
    const auto scheme = activeScheme;

    juce::String suffix;
    juce::String suffixShort;
    if (&s == &amountSlider)       { suffix = " % AMT";      suffixShort = " % AMT"; }
    else if (&s == &pitchSlider)   { suffix = " ST PITCH";   suffixShort = " ST"; }
    else if (&s == &jitterSlider)  { suffix = " % JIT";      suffixShort = " % JIT"; }
    else if (&s == &grainSlider)   { suffix = " MS GRN";     suffixShort = " MS GRN"; }
    else if (&s == &windowSlider)  { suffix = " WINDOW";     suffixShort = " WIN"; }
    else if (&s == &inputSlider)   { suffix = " DB INPUT";   suffixShort = " DB IN"; }
    else if (&s == &outputSlider)  { suffix = " DB OUTPUT";  suffixShort = " DB OUT"; }
    else if (&s == &mixSlider)     { suffix = " % MIX";      suffixShort = " % MIX"; }
    else if (&s == &panSlider)     { suffix = " % PAN";      suffixShort = " %"; }
    else if (&s == &tiltSlider)    { suffix = " DB TILT";    suffixShort = " DB TILT"; }
    else if (&s == &limThresholdSlider) { suffix = " DB LIM"; suffixShort = " DB LIM"; }

    const juce::String suffixText      = suffix.trimStart();
    const juce::String suffixTextShort = suffixShort.trimStart();

    auto* aw = new juce::AlertWindow ("", "", juce::AlertWindow::NoIcon);
    aw->setLookAndFeel (&lnf);

    juce::String currentDisplay;
    if (&s == &pitchSlider)
        currentDisplay = formatPitchSemitones (pitchSliderToSemitones (s.getValue()), 2);
    else if (&s == &grainSlider)
        currentDisplay = juce::String (s.getValue(), 3);
    else if (&s == &panSlider)
        currentDisplay = juce::String (juce::jlimit (0.0, 100.0, s.getValue() * 100.0), 0);
    else if (&s == &windowSlider)
        currentDisplay = juce::String (getEffectiveWindowValue (s.getValue()));
    else
        currentDisplay = s.getTextFromValue (s.getValue());

    aw->addTextEditor ("val", currentDisplay, juce::String());

    juce::Label* suffixLabel = nullptr;
    juce::Rectangle<int> editorBaseBounds;
    std::function<void()> layoutValueAndSuffix;

    if (auto* te = aw->getTextEditor ("val"))
    {
        const auto& f = kBoldFont40();
        te->setFont (f);
        te->applyFontToAllText (f);
        auto r = te->getBounds();
        r.setHeight ((int) (f.getHeight() * kPromptEditorHeightScale) + kPromptEditorHeightPadPx);
        r.setY (juce::jmax (kPromptEditorMinTopPx, r.getY() - kPromptEditorRaiseYPx));
        editorBaseBounds = r;

        suffixLabel = new juce::Label ("suffix", suffixText);
        suffixLabel->setComponentID (kPromptSuffixLabelId);
        suffixLabel->setJustificationType (juce::Justification::centredLeft);
        applyLabelTextColour (*suffixLabel, scheme.text);
        suffixLabel->setBorderSize (juce::BorderSize<int> (0));
        suffixLabel->setFont (f);
        aw->addAndMakeVisible (suffixLabel);

        juce::String worstCaseText;
        if (&s == &amountSlider)       worstCaseText = "100.00";
        else if (&s == &pitchSlider)     worstCaseText = "+24.00";
        else if (&s == &jitterSlider)  worstCaseText = "100.00";
        else if (&s == &grainSlider)   worstCaseText = "500.000";
        else if (&s == &windowSlider)  worstCaseText = "8192";
        else if (&s == &inputSlider)   worstCaseText = "-144.0";
        else if (&s == &outputSlider)  worstCaseText = "-144.0";
        else if (&s == &mixSlider)     worstCaseText = "100.00";
        else if (&s == &panSlider)     worstCaseText = "100";
        else if (&s == &tiltSlider)    worstCaseText = "-100.0";
        else if (&s == &limThresholdSlider) worstCaseText = "-36.0";
        else                           worstCaseText = "999.99";

        const int maxInputTextW = juce::jmax (1, stringWidth (f, worstCaseText));

        layoutValueAndSuffix = [aw, te, suffixLabel, editorBaseBounds, suffixText, suffixTextShort, maxInputTextW]()
        {
            const int contentPad = kPromptInlineContentPadPx;
            const int contentLeft = contentPad;
            const int contentRight = (aw != nullptr ? aw->getWidth() - contentPad : editorBaseBounds.getRight());
            const int availableW = contentRight - contentLeft;
            const int contentCenter = (contentLeft + contentRight) / 2;

            const int fullLabelW = stringWidth (suffixLabel->getFont(), suffixText) + 2;
            const bool stickPercentFull = suffixText.containsChar ('%');
            const int spaceWFull = stickPercentFull ? 0 : juce::jmax (2, stringWidth (suffixLabel->getFont(), " "));
            const int worstCaseFullW = maxInputTextW + spaceWFull + fullLabelW;

            const bool useShort = (worstCaseFullW > availableW) && suffixTextShort != suffixText;
            const juce::String& activeSuffix = useShort ? suffixTextShort : suffixText;
            suffixLabel->setText (activeSuffix, juce::dontSendNotification);

            const auto txt = te->getText();
            const int textW = juce::jmax (1, stringWidth (te->getFont(), txt));
            int labelW = stringWidth (suffixLabel->getFont(), activeSuffix) + 2;
            auto er = te->getBounds();

            const bool stickPercent = activeSuffix.containsChar ('%');
            const int spaceW = stickPercent ? 0 : juce::jmax (2, stringWidth (te->getFont(), " "));
            const int minGapPx = juce::jmax (1, spaceW);

            constexpr int kEditorTextPadPx = 12;
            constexpr int kMinEditorWidthPx = 24;
            const int editorW = juce::jlimit (kMinEditorWidthPx,
                                              editorBaseBounds.getWidth(),
                                              textW + (kEditorTextPadPx * 2));
            er.setWidth (editorW);

            const int combinedW = textW + minGapPx + labelW;
            int blockLeft = contentCenter - (combinedW / 2);
            const int minBlockLeft = contentLeft;
            const int maxBlockLeft = juce::jmax (minBlockLeft, contentRight - combinedW);
            blockLeft = juce::jlimit (minBlockLeft, maxBlockLeft, blockLeft);

            int teX = blockLeft - ((editorW - textW) / 2);
            const int minTeX = contentLeft;
            const int maxTeX = juce::jmax (minTeX, contentRight - editorW);
            teX = juce::jlimit (minTeX, maxTeX, teX);
            er.setX (teX);
            te->setBounds (er);

            const int textLeftActual = er.getX() + (er.getWidth() - textW) / 2;
            int labelX = textLeftActual + textW + minGapPx;
            const int minLabelX = contentLeft;
            const int maxLabelX = juce::jmax (minLabelX, contentRight - labelW);
            labelX = juce::jlimit (minLabelX, maxLabelX, labelX);

            const int labelY = er.getY();
            const int labelH = juce::jmax (1, er.getHeight());
            suffixLabel->setBounds (labelX, labelY, labelW, labelH);
        };

        te->setBounds (editorBaseBounds);
        int labelW0 = stringWidth (suffixLabel->getFont(), suffixText) + 2;
        suffixLabel->setBounds (r.getRight() + 2, r.getY() + 1, labelW0, juce::jmax (1, r.getHeight() - 2));

        if (layoutValueAndSuffix)
            layoutValueAndSuffix();

        te->setInputFilter (new juce::TextEditor::LengthAndCharacterRestriction (10, "0123456789.-+"), true);

        te->onTextChange = [te, layoutValueAndSuffix]() mutable
        {
            if (layoutValueAndSuffix)
                layoutValueAndSuffix();
        };
    }

    aw->addButton ("OK", 1, juce::KeyPress (juce::KeyPress::returnKey));
    aw->addButton ("CANCEL", 0, juce::KeyPress (juce::KeyPress::escapeKey));
    applyPromptShellSize (*aw);
    layoutAlertWindowButtons (*aw);

    const juce::Font& kPromptFont = kBoldFont40();
    preparePromptTextEditor (*aw, "val", scheme.bg, scheme.text, scheme.fg, kPromptFont, false);

    if (suffixLabel != nullptr && ! editorBaseBounds.isEmpty())
    {
        if (auto* te = aw->getTextEditor ("val"))
            suffixLabel->setFont (te->getFont());
        if (layoutValueAndSuffix)
            layoutValueAndSuffix();
    }

    styleAlertButtons (*aw, lnf);

    juce::Component::SafePointer<STRETRAudioProcessorEditor> safeThis (this);
    setPromptOverlayActive (true);

    fitAlertWindowToEditor (*aw, safeThis.getComponent(), [layoutValueAndSuffix, scheme, kPromptFont] (juce::AlertWindow& a)
    {
        if (layoutValueAndSuffix)
            layoutValueAndSuffix();
        layoutAlertWindowButtons (a);
        preparePromptTextEditor (a, "val", scheme.bg, scheme.text, scheme.fg, kPromptFont, false);
    });
    embedAlertWindowInOverlay (safeThis.getComponent(), aw);

    {
        preparePromptTextEditor (*aw, "val", scheme.bg, scheme.text, scheme.fg, kPromptFont, false);
        if (auto* suffixLbl = dynamic_cast<juce::Label*> (aw->findChildWithID (kPromptSuffixLabelId)))
        {
            if (auto* te = aw->getTextEditor ("val"))
                suffixLbl->setFont (te->getFont());
        }
        if (layoutValueAndSuffix)
            layoutValueAndSuffix();

        juce::Component::SafePointer<juce::AlertWindow> safeAw (aw);
        juce::MessageManager::callAsync ([safeAw]()
        {
            if (safeAw == nullptr) return;
            bringPromptWindowToFront (*safeAw);
            safeAw->repaint();
        });
    }

    juce::Slider* targetSlider = &s;
    aw->enterModalState (true,
        juce::ModalCallbackFunction::create ([safeThis, aw, targetSlider] (int result)
        {
            std::unique_ptr<juce::AlertWindow> killer (aw);
            if (safeThis) safeThis->setPromptOverlayActive (false);
            if (safeThis == nullptr || result != 1) return;

            const auto txt = aw->getTextEditorContents ("val").trim();
            if (txt.isEmpty()) return;

            double val = txt.getDoubleValue();
            if (targetSlider == &safeThis->pitchSlider)
            {
                val = std::round (val * 100.0) / 100.0;
                val = semitonesToPitchSlider (val);
            }
            else if (targetSlider == &safeThis->panSlider)
                val = juce::jlimit (0.0, 1.0, val / 100.0);
            else if (targetSlider == &safeThis->mixSlider)
                val = juce::jlimit (0.0, 1.0, val / 100.0);
            else if (targetSlider == &safeThis->windowSlider)
                val = (double) safeThis->getEffectiveWindowValue (val);

            targetSlider->setValue (val, juce::sendNotificationSync);
        }), false);
}

void STRETRAudioProcessorEditor::openPdcMaxWindowPrompt()
{
    lnf.setScheme (activeScheme);
    const auto scheme = activeScheme;

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

    auto setMaxWindowParam = [this] (int window)
    {
        if (auto* p = audioProcessor.apvts.getParameter (STRETRAudioProcessor::kParamMaxWindow))
            p->setValueNotifyingHost (p->convertTo0to1 ((float) STRETRAudioProcessor::getCanonicalFftWindow (window)));
        syncFftWindowToMax (true);
        updatePdcTooltip();
        refreshLegendTextCache();
        repaint();
    };

    auto* aw = new juce::AlertWindow ("", "", juce::AlertWindow::NoIcon);
    aw->setLookAndFeel (&lnf);
    aw->addTextEditor ("maxwin", juce::String (currentMaxWindow), juce::String());

    struct WindowInputFilter : juce::TextEditor::InputFilter
    {
        juce::String filterNewText (juce::TextEditor& editor, const juce::String& newText) override
        {
            juce::String result;
            for (auto c : newText)
            {
                if (juce::CharacterFunctions::isDigit (c))
                    result += c;
                if (result.length() >= 4)
                    break;
            }

            juce::String proposed = editor.getText();
            const int insertPos = editor.getCaretPosition();
            proposed = proposed.substring (0, insertPos) + result
                     + proposed.substring (insertPos + editor.getHighlightedText().length());

            if (proposed.length() > 4 || proposed.getIntValue() > STRETRAudioProcessor::kWindowMax)
                return {};

            return result;
        }
    };

    struct PromptBar : public juce::Component
    {
        STREScheme colours;
        float value01 = 1.0f;
        std::function<void (float)> onValueChanged;

        PromptBar (const STREScheme& s, float initial01)
            : colours (s), value01 (juce::jlimit (0.0f, 1.0f, initial01)) {}

        void paint (juce::Graphics& g) override
        {
            const auto r = getLocalBounds().toFloat();
            g.setColour (colours.outline);
            g.drawRect (r, 4.0f);
            auto inner = r.reduced (7.0f);
            g.setColour (colours.bg);
            g.fillRect (inner);
            g.setColour (colours.fg);
            g.fillRect (inner.withWidth (inner.getWidth() * value01));
        }

        void mouseDown (const juce::MouseEvent& e) override { updateFromMouse (e); }
        void mouseDrag (const juce::MouseEvent& e) override { updateFromMouse (e); }
        void mouseDoubleClick (const juce::MouseEvent&) override { setValue (1.0f); }

        void setValue (float newValue01)
        {
            value01 = juce::jlimit (0.0f, 1.0f, newValue01);
            repaint();
            if (onValueChanged)
                onValueChanged (value01);
        }

    private:
        void updateFromMouse (const juce::MouseEvent& e)
        {
            const float innerW = (float) getWidth() - 14.0f;
            const float next = innerW > 0.0f ? ((float) e.x - 7.0f) / innerW : 0.0f;
            const int lane = juce::jlimit (0, STRETRAudioProcessor::kNumFftWindows - 1,
                (int) std::lround (juce::jlimit (0.0f, 1.0f, next)
                    * (float) (STRETRAudioProcessor::kNumFftWindows - 1)));
            setValue ((float) lane / (float) (STRETRAudioProcessor::kNumFftWindows - 1));
        }
    };

    const auto& f = kBoldFont40();
    auto* nameLabel = new juce::Label ("maxwin_label", "MAX WIN");
    nameLabel->setJustificationType (juce::Justification::centredLeft);
    nameLabel->setBorderSize (juce::BorderSize<int> (0));
    nameLabel->setFont (f);
    applyLabelTextColour (*nameLabel, scheme.text);
    aw->addAndMakeVisible (nameLabel);

    auto* bar = new PromptBar (scheme, windowToBar (currentMaxWindow));
    aw->addAndMakeVisible (bar);

    if (auto* te = aw->getTextEditor ("maxwin"))
    {
        te->setFont (f);
        te->applyFontToAllText (f);
        te->setInputFilter (new WindowInputFilter(), true);
    }

    auto syncing = std::make_shared<bool> (false);
    auto layoutRows = [aw, nameLabel, bar]()
    {
        auto* te = aw->getTextEditor ("maxwin");
        if (te == nullptr || nameLabel == nullptr || bar == nullptr)
            return;

        const int buttonsTop = getAlertButtonsTop (*aw);
        auto er = te->getBounds();
        er.setHeight ((int) (te->getFont().getHeight() * kPromptEditorHeightScale) + kPromptEditorHeightPadPx);
        const int rowH = er.getHeight();
        const int barH = juce::jmax (12, rowH / 2);
        const int barGap = juce::jmax (4, rowH / 4);
        const int totalH = rowH + barGap + barH;
        const int y = juce::jmax (kPromptEditorMinTopPx, (buttonsTop - totalH) / 2);
        const int contentPad = kPromptInnerMargin;
        const int contentW = aw->getWidth() - contentPad * 2;
        const int labelW = stringWidth (nameLabel->getFont(), nameLabel->getText()) + 8;
        const int textW = juce::jmax (stringWidth (te->getFont(), "8192"), stringWidth (te->getFont(), te->getText()));
        const int editorW = juce::jmax (40, textW + 10);
        const int gap = juce::jmax (8, stringWidth (te->getFont(), " "));
        const int visualW = labelW + gap + editorW;
        const int blockLeft = contentPad + juce::jmax (0, (contentW - visualW) / 2);

        nameLabel->setBounds (blockLeft, y, labelW, rowH);
        te->setBounds (blockLeft + labelW + gap, y, editorW, rowH);
        bar->setBounds (contentPad, y + rowH + barGap, contentW, barH);
    };

    bar->onValueChanged = [aw, barToWindow, setMaxWindowParam, syncing, layoutRows] (float value01)
    {
        if (*syncing)
            return;

        *syncing = true;
        const int window = barToWindow (value01);
        if (auto* te = aw->getTextEditor ("maxwin"))
        {
            te->setText (juce::String (window), juce::sendNotification);
            te->selectAll();
        }
        *syncing = false;
        setMaxWindowParam (window);
        layoutRows();
    };

    if (auto* te = aw->getTextEditor ("maxwin"))
        te->onTextChange = [aw, bar, windowToBar, setMaxWindowParam, syncing, layoutRows]()
        {
            if (*syncing)
                return;

            *syncing = true;
            int window = STRETRAudioProcessor::kFftMaxWindowDefault;
            if (auto* editor = aw->getTextEditor ("maxwin"))
                window = STRETRAudioProcessor::getCanonicalFftWindow (editor->getText().getIntValue());
            bar->setValue (windowToBar (window));
            *syncing = false;
            setMaxWindowParam (window);
            layoutRows();
        };

    aw->addButton ("OK", 1, juce::KeyPress (juce::KeyPress::returnKey));
    aw->addButton ("CANCEL", 0, juce::KeyPress (juce::KeyPress::escapeKey));
    applyPromptShellSize (*aw);
    layoutAlertWindowButtons (*aw);
    preparePromptTextEditor (*aw, "maxwin", scheme.bg, scheme.text, scheme.fg, f, false);
    layoutRows();
    styleAlertButtons (*aw, lnf);

    setPromptOverlayActive (true);
    juce::Component::SafePointer<STRETRAudioProcessorEditor> safeThis (this);
    fitAlertWindowToEditor (*aw, safeThis.getComponent(), [layoutRows] (juce::AlertWindow& a)
    {
        juce::ignoreUnused (a);
        layoutAlertWindowButtons (a);
        layoutRows();
    });
    embedAlertWindowInOverlay (safeThis.getComponent(), aw);

    aw->enterModalState (true,
        juce::ModalCallbackFunction::create (
            [safeThis, aw, savedMaxWindow = currentMaxWindow,
             savedPluginWindow = currentPluginWindow, restoreActiveFft = activeFft] (int result) mutable
        {
            std::unique_ptr<juce::AlertWindow> killer (aw);
            if (safeThis != nullptr)
                safeThis->setPromptOverlayActive (false);
            if (safeThis == nullptr)
                return;

            if (result != 1)
            {
                if (auto* p = safeThis->audioProcessor.apvts.getParameter (STRETRAudioProcessor::kParamMaxWindow))
                    p->setValueNotifyingHost (p->convertTo0to1 ((float) savedMaxWindow));
                if (restoreActiveFft)
                    if (auto* p = safeThis->audioProcessor.apvts.getParameter (STRETRAudioProcessor::kParamWindow))
                        p->setValueNotifyingHost (p->convertTo0to1 ((float) juce::jmin (savedPluginWindow, savedMaxWindow)));
            }
            safeThis->syncFftWindowToMax (true);
            safeThis->updatePdcTooltip();
        }),
        false);
}

void STRETRAudioProcessorEditor::openMixSendPrompt()
{
    using namespace TR;
    lnf.setScheme (activeScheme);
    const auto scheme = activeScheme;

    auto& proc = audioProcessor;
    const float curDry = proc.apvts.getRawParameterValue (STRETRAudioProcessor::kParamDryLevel)->load();
    const float curWet = proc.apvts.getRawParameterValue (STRETRAudioProcessor::kParamWetLevel)->load();

    auto* aw = new juce::AlertWindow ("", "", juce::AlertWindow::NoIcon);
    aw->setLookAndFeel (&lnf);

    auto linearToDb = [] (float g) -> float { return (g <= 0.0001f) ? -100.0f : 20.0f * std::log10 (g); };
    auto dbToLinear = [] (float dB) -> float { return (dB <= -100.0f) ? 0.0f : std::pow (10.0f, dB / 20.0f); };
    auto dbString = [&linearToDb] (float g) -> juce::String
    {
        const float dB = linearToDb (g);
        if (dB <= -100.0f) return "-INF";
        if (std::abs (dB) < 0.05f) return "0";
        return juce::String (dB, 1);
    };

    aw->addTextEditor ("dryLevel", dbString (curDry), juce::String());
    aw->addTextEditor ("wetLevel", dbString (curWet), juce::String());

    struct PromptBar : public juce::Component
    {
        STREScheme colours;
        float  value01   = 1.0f;
        float  default01 = 1.0f;
        std::function<void (float)> onValueChanged;

        PromptBar (const STREScheme& s, float initial, float def)
            : colours (s), value01 (initial), default01 (def) {}

        void paint (juce::Graphics& g) override
        {
            const auto r = getLocalBounds().toFloat();
            g.setColour (colours.outline);
            g.drawRect (r, 4.0f);
            const float pad = 7.0f;
            auto inner = r.reduced (pad);
            g.setColour (colours.bg);
            g.fillRect (inner);
            const float fillW = juce::jlimit (0.0f, inner.getWidth(), inner.getWidth() * value01);
            g.setColour (colours.fg);
            g.fillRect (inner.withWidth (fillW));
        }

        void mouseDown (const juce::MouseEvent& e) override  { updateFromMouse (e); }
        void mouseDrag (const juce::MouseEvent& e) override  { updateFromMouse (e); }
        void mouseDoubleClick (const juce::MouseEvent&) override { setValue (default01); }

        void setValue (float v01)
        {
            value01 = juce::jlimit (0.0f, 1.0f, v01);
            repaint();
            if (onValueChanged) onValueChanged (value01);
        }

    private:
        void updateFromMouse (const juce::MouseEvent& e)
        {
            const float pad = 7.0f;
            const float innerW = (float) getWidth() - pad * 2.0f;
            setValue (innerW > 0.0f ? ((float) e.x - pad) / innerW : 0.0f);
        }
    };

    auto* dryBar = new PromptBar (scheme, curDry, STRETRAudioProcessor::kDryLevelDefault);
    auto* wetBar = new PromptBar (scheme, curWet, STRETRAudioProcessor::kWetLevelDefault);
    aw->addAndMakeVisible (dryBar);
    aw->addAndMakeVisible (wetBar);

    auto syncing  = std::make_shared<bool> (false);
    auto layoutFn = std::make_shared<std::function<void()>> ([] {});

    juce::Component::SafePointer<STRETRAudioProcessorEditor> safeThis (this);

    auto pushParams = [safeThis, aw, dbToLinear] ()
    {
        if (safeThis == nullptr) return;
        auto& p = safeThis->audioProcessor;
        auto setP = [&p] (const char* id, float plain)
        {
            if (auto* param = p.apvts.getParameter (id))
                param->setValueNotifyingHost (param->convertTo0to1 (plain));
        };
        auto* dryTe = aw->getTextEditor ("dryLevel");
        auto* wetTe = aw->getTextEditor ("wetLevel");
        const float dryLin = dryTe ? juce::jlimit (0.0f, 1.0f, dbToLinear (dryTe->getText().getFloatValue())) : 1.0f;
        const float wetLin = wetTe ? juce::jlimit (0.0f, 1.0f, dbToLinear (wetTe->getText().getFloatValue())) : 1.0f;
        setP (STRETRAudioProcessor::kParamDryLevel, dryLin);
        setP (STRETRAudioProcessor::kParamWetLevel, wetLin);
        safeThis->dualMixBar_.updateFromProcessor();
    };

    auto barToText = [aw, syncing, pushParams, dbString] (const char* editorId, float v01)
    {
        if (*syncing) return;
        *syncing = true;
        if (auto* te = aw->getTextEditor (editorId))
        {
            te->setText (dbString (v01), juce::sendNotification);
            te->selectAll();
        }
        *syncing = false;
        pushParams();
    };

    dryBar->onValueChanged = [barToText] (float v) { barToText ("dryLevel", v); };
    wetBar->onValueChanged = [barToText] (float v) { barToText ("wetLevel", v); };

    auto textToBar = [syncing, pushParams, dbToLinear] (juce::TextEditor* te, PromptBar* bar)
    {
        if (*syncing || te == nullptr || bar == nullptr) return;
        *syncing = true;
        const float dB  = te->getText().getFloatValue();
        const float lin = juce::jlimit (0.0f, 1.0f, dbToLinear (dB));
        bar->value01 = lin;
        bar->repaint();
        *syncing = false;
        pushParams();
    };

    const auto& promptFont = kBoldFont40();

    auto addPromptLabel = [&] (const juce::String& text)
    {
        auto* label = new juce::Label ("", text);
        label->setJustificationType (juce::Justification::centredLeft);
        applyLabelTextColour (*label, scheme.text);
        label->setBorderSize (juce::BorderSize<int> (0));
        label->setFont (promptFont);
        aw->addAndMakeVisible (label);
        return label;
    };

    auto* dryNameLabel = addPromptLabel ("DRY");
    auto* wetNameLabel = addPromptLabel ("WET");
    auto* dryDbLabel = addPromptLabel ("dB");
    auto* wetDbLabel = addPromptLabel ("dB");

    preparePromptTextEditor (*aw, "dryLevel", scheme.bg, scheme.text, scheme.fg, promptFont, false);
    preparePromptTextEditor (*aw, "wetLevel", scheme.bg, scheme.text, scheme.fg, promptFont, false);

    auto layoutRows = [aw, dryNameLabel, wetNameLabel, dryDbLabel, wetDbLabel,
                        dryBar, wetBar, promptFont] ()
    {
        auto* dryTe = aw->getTextEditor ("dryLevel");
        auto* wetTe = aw->getTextEditor ("wetLevel");
        if (dryTe == nullptr || wetTe == nullptr) return;

        const int buttonsTop = getAlertButtonsTop (*aw);
        const int rowH       = dryTe->getHeight();
        const int barH       = juce::jmax (10, rowH / 2);
        const int barGap     = juce::jmax (2, rowH / 6);
        const int gap        = juce::jmax (4, rowH / 3);
        const int rowTotal   = rowH + barGap + barH;
        const int totalH     = rowTotal * 2 + gap;
        const int startY     = juce::jmax (kPromptEditorMinTopPx, (buttonsTop - totalH) / 2);

        const int barX = kPromptInnerMargin;
        const int barR = aw->getWidth() - kPromptInnerMargin;

        const int nameW = stringWidth (promptFont, "WET") + 6;
        const int hzGap = 2;
        const int dbW   = stringWidth (promptFont, "dB") + 2;

        auto placeRow = [&] (juce::Label* nameLabel, juce::TextEditor* te,
                             juce::Label* dbLabel, PromptBar* bar, int y)
        {
            nameLabel->setFont (promptFont);
            dbLabel->setFont (promptFont);
            nameLabel->setBounds (barX, y, nameW, rowH);

            const int midL = barX + nameW;
            const int midR = barR;
            const int midW = midR - midL;

            const auto txt = te->getText();
            const int textW = juce::jmax (1, stringWidth (promptFont, txt));
            constexpr int kEditorPad = 6;
            const int editorW = textW + kEditorPad * 2;
            const int groupW  = editorW + hzGap + dbW;
            const int groupX = midL + juce::jmax (0, (midW - groupW) / 2);

            te->setBounds (groupX, y, editorW, rowH);
            dbLabel->setBounds (groupX + editorW + hzGap, y, dbW, rowH);

            const int barW = juce::jmax (60, barR - barX);
            bar->setBounds (barX, y + rowH + barGap, barW, barH);
        };

        placeRow (dryNameLabel, dryTe, dryDbLabel, dryBar, startY);
        placeRow (wetNameLabel, wetTe, wetDbLabel, wetBar, startY + rowTotal + gap);
    };

    auto* dryTe = aw->getTextEditor ("dryLevel");
    auto* wetTe = aw->getTextEditor ("wetLevel");
    if (dryTe != nullptr)
        dryTe->onTextChange = [textToBar, dryTe, dryBar, layoutFn] () { textToBar (dryTe, dryBar); if (*layoutFn) (*layoutFn)(); };
    if (wetTe != nullptr)
        wetTe->onTextChange = [textToBar, wetTe, wetBar, layoutFn] () { textToBar (wetTe, wetBar); if (*layoutFn) (*layoutFn)(); };

    aw->addButton ("OK",     1, juce::KeyPress (juce::KeyPress::returnKey));
    aw->addButton ("CANCEL", 0, juce::KeyPress (juce::KeyPress::escapeKey));

    applyPromptShellSize (*aw);
    layoutAlertWindowButtons (*aw);
    layoutRows();

    preparePromptTextEditor (*aw, "dryLevel", scheme.bg, scheme.text, scheme.fg, promptFont, false);
    preparePromptTextEditor (*aw, "wetLevel", scheme.bg, scheme.text, scheme.fg, promptFont, false);
    layoutRows();

    styleAlertButtons (*aw, lnf);

    const float origDry = curDry;
    const float origWet = curWet;

    fitAlertWindowToEditor (*aw, safeThis.getComponent(), [layoutRows] (juce::AlertWindow& a)
    {
        layoutAlertWindowButtons (a);
        layoutRows();
    });

    embedAlertWindowInOverlay (safeThis.getComponent(), aw);
    *layoutFn = layoutRows;

    {
        preparePromptTextEditor (*aw, "dryLevel", scheme.bg, scheme.text, scheme.fg, promptFont, false);
        preparePromptTextEditor (*aw, "wetLevel", scheme.bg, scheme.text, scheme.fg, promptFont, false);
        layoutRows();

        juce::Component::SafePointer<juce::AlertWindow> safeAw (aw);
        juce::MessageManager::callAsync ([safeAw]()
        {
            if (safeAw == nullptr) return;
            bringPromptWindowToFront (*safeAw);
            safeAw->repaint();
        });
    }

    setPromptOverlayActive (true);

    aw->enterModalState (true,
        juce::ModalCallbackFunction::create (
            [safeThis, aw, origDry, origWet] (int result)
        {
            std::unique_ptr<juce::AlertWindow> killer (aw);

            if (safeThis != nullptr)
                safeThis->setPromptOverlayActive (false);

            if (safeThis == nullptr)
                return;

            if (result != 1)
            {
                auto& p = safeThis->audioProcessor;
                auto setP = [&p] (const char* id, float plain)
                {
                    if (auto* param = p.apvts.getParameter (id))
                        param->setValueNotifyingHost (param->convertTo0to1 (plain));
                };
                setP (STRETRAudioProcessor::kParamDryLevel, origDry);
                setP (STRETRAudioProcessor::kParamWetLevel, origWet);
                safeThis->dualMixBar_.updateFromProcessor();
            }
        }),
        false);
}

void STRETRAudioProcessorEditor::openFilterPrompt()
{
    lnf.setScheme (activeScheme);
    const auto scheme = activeScheme;

    auto& proc = audioProcessor;
    const float hpFreq = proc.apvts.getRawParameterValue (STRETRAudioProcessor::kParamFilterHpFreq)->load();
    const float lpFreq = proc.apvts.getRawParameterValue (STRETRAudioProcessor::kParamFilterLpFreq)->load();
    const int hpSlope  = juce::roundToInt (proc.apvts.getRawParameterValue (STRETRAudioProcessor::kParamFilterHpSlope)->load());
    const int lpSlope  = juce::roundToInt (proc.apvts.getRawParameterValue (STRETRAudioProcessor::kParamFilterLpSlope)->load());
    const bool hpOn    = proc.apvts.getRawParameterValue (STRETRAudioProcessor::kParamFilterHpOn)->load() > 0.5f;
    const bool lpOn    = proc.apvts.getRawParameterValue (STRETRAudioProcessor::kParamFilterLpOn)->load() > 0.5f;

    auto* aw = new juce::AlertWindow ("", "", juce::AlertWindow::NoIcon);
    aw->setLookAndFeel (&lnf);

    aw->addTextEditor ("hpFreq", juce::String (hpFreq, 2), juce::String());
    aw->addTextEditor ("lpFreq", juce::String (lpFreq, 2), juce::String());

    aw->addButton ("OK", 1, juce::KeyPress (juce::KeyPress::returnKey));
    aw->addButton ("CANCEL", 0, juce::KeyPress (juce::KeyPress::escapeKey));
    applyPromptShellSize (*aw);
    layoutAlertWindowButtons (*aw);
    styleAlertButtons (*aw, lnf);

    for (const char* id : { "hpFreq", "lpFreq" })
    {
        if (auto* te = aw->getTextEditor (id))
        {
            te->setInputFilter (new juce::TextEditor::LengthAndCharacterRestriction (9, "0123456789."), true);
            preparePromptTextEditor (*aw, id, scheme.bg, scheme.text, scheme.fg, kBoldFont40(), false);
        }
    }

    juce::Component::SafePointer<STRETRAudioProcessorEditor> safeThis (this);
    setPromptOverlayActive (true);

    const float origHpFreq = hpFreq, origLpFreq = lpFreq;
    const int origHpSlope = hpSlope, origLpSlope = lpSlope;
    const bool origHpOn = hpOn, origLpOn = lpOn;

    fitAlertWindowToEditor (*aw, safeThis.getComponent(), [] (juce::AlertWindow& a) { layoutAlertWindowButtons (a); });
    embedAlertWindowInOverlay (safeThis.getComponent(), aw);

    aw->enterModalState (true,
        juce::ModalCallbackFunction::create ([safeThis, aw, origHpFreq, origLpFreq, origHpSlope, origLpSlope, origHpOn, origLpOn] (int result)
        {
            std::unique_ptr<juce::AlertWindow> killer (aw);
            if (safeThis == nullptr) return;
            if (result != 1)
            {
                auto& p = safeThis->audioProcessor;
                auto setP = [&p] (const char* id, float plain)
                { if (auto* param = p.apvts.getParameter (id)) param->setValueNotifyingHost (param->convertTo0to1 (plain)); };
                setP (STRETRAudioProcessor::kParamFilterHpFreq, origHpFreq);
                setP (STRETRAudioProcessor::kParamFilterLpFreq, origLpFreq);
                safeThis->filterBar_.updateFromProcessor();
            }
            else
            {
                auto& p = safeThis->audioProcessor;
                const auto hpTxt = aw->getTextEditorContents ("hpFreq").trim();
                const auto lpTxt = aw->getTextEditorContents ("lpFreq").trim();
                if (hpTxt.isNotEmpty())
                {
                    float f = juce::jlimit (20.0f, 20000.0f, (float) hpTxt.getDoubleValue());
                    if (auto* param = p.apvts.getParameter (STRETRAudioProcessor::kParamFilterHpFreq))
                        param->setValueNotifyingHost (param->convertTo0to1 (f));
                }
                if (lpTxt.isNotEmpty())
                {
                    float f = juce::jlimit (20.0f, 20000.0f, (float) lpTxt.getDoubleValue());
                    if (auto* param = p.apvts.getParameter (STRETRAudioProcessor::kParamFilterLpFreq))
                        param->setValueNotifyingHost (param->convertTo0to1 (f));
                }
                safeThis->filterBar_.updateFromProcessor();
            }
            safeThis->setPromptOverlayActive (false);
        }), false);
}

void STRETRAudioProcessorEditor::openChaosConfigPrompt (const char* amtParamId, const char* spdParamId,
                                                         const juce::String& /*title*/)
{
    using namespace TR;
    lnf.setScheme (activeScheme);
    const auto scheme = activeScheme;

    const float currentAmt = audioProcessor.apvts.getRawParameterValue (amtParamId)->load();
    const float currentSpd = audioProcessor.apvts.getRawParameterValue (spdParamId)->load();

    auto* aw = new juce::AlertWindow ("", "", juce::AlertWindow::NoIcon);
    aw->setLookAndFeel (&lnf);
    aw->addTextEditor ("amt", juce::String (juce::roundToInt (currentAmt)), juce::String());
    aw->addTextEditor ("spd", juce::String (currentSpd, 2), juce::String());

    // ── PromptBar: interactive fill bar ──────────────────────────
    struct PromptBar : public juce::Component
    {
        STREScheme colours; float value = 0.5f; float defaultVal = 0.5f;
        std::function<void (float)> onValueChanged;
        PromptBar (const STREScheme& s, float initial01, float default01) : colours (s), value (initial01), defaultVal (default01) {}
        void paint (juce::Graphics& g) override
        { const auto r = getLocalBounds().toFloat(); g.setColour (colours.outline); g.drawRect (r, 4.0f);
          const float pad = 7.0f; auto inner = r.reduced (pad); g.setColour (colours.bg); g.fillRect (inner);
          const float fillW = juce::jlimit (0.0f, inner.getWidth(), inner.getWidth() * value);
          g.setColour (colours.fg); g.fillRect (inner.withWidth (fillW)); }
        void mouseDown (const juce::MouseEvent& e) override { updateFromMouse (e); }
        void mouseDrag (const juce::MouseEvent& e) override { updateFromMouse (e); }
        void mouseDoubleClick (const juce::MouseEvent&) override { setValue (defaultVal); }
        void setValue (float v01) { value = juce::jlimit (0.0f, 1.0f, v01); repaint(); if (onValueChanged) onValueChanged (value); }
    private:
        void updateFromMouse (const juce::MouseEvent& e)
        { const float pad = 7.0f; const float innerW = (float) getWidth() - pad * 2.0f; setValue (innerW > 0.0f ? ((float) e.x - pad) / innerW : 0.0f); }
    };

    struct ResetLabel : public juce::Label
    { PromptBar* pairedBar = nullptr;
      void mouseDoubleClick (const juce::MouseEvent&) override { if (pairedBar != nullptr) pairedBar->setValue (pairedBar->defaultVal); } };

    const auto& f = kBoldFont40();
    ResetLabel* amtSuffix = nullptr; ResetLabel* spdSuffix = nullptr;
    juce::Label* amtUnitLabel = nullptr; juce::Label* spdUnitLabel = nullptr;

    auto setupField = [&] (const char* editorId, const juce::String& suffixText,
                           const juce::String& unitText, bool useDecimalFilter,
                           ResetLabel*& suffixOut, juce::Label*& unitOut)
    {
        if (auto* te = aw->getTextEditor (editorId))
        {
            te->setFont (f); te->applyFontToAllText (f);
            if (useDecimalFilter) te->setInputRestrictions (6, "0123456789.");
            else                  te->setInputFilter (new PctInputFilter(), true);
            auto r = te->getBounds();
            r.setHeight ((int) (f.getHeight() * kPromptEditorHeightScale) + kPromptEditorHeightPadPx);
            te->setBounds (r);

            suffixOut = new ResetLabel();
            suffixOut->setText (suffixText, juce::dontSendNotification);
            suffixOut->setJustificationType (juce::Justification::centredLeft);
            applyLabelTextColour (*suffixOut, scheme.text);
            suffixOut->setBorderSize (juce::BorderSize<int> (0));
            suffixOut->setFont (f);
            aw->addAndMakeVisible (suffixOut);

            unitOut = new juce::Label ("", unitText);
            unitOut->setJustificationType (juce::Justification::centredLeft);
            applyLabelTextColour (*unitOut, scheme.text);
            unitOut->setBorderSize (juce::BorderSize<int> (0));
            unitOut->setFont (f);
            aw->addAndMakeVisible (unitOut);
        }
    };

    setupField ("amt", "AMT", "%",  false, amtSuffix, amtUnitLabel);
    setupField ("spd", "SPD", "Hz", true,  spdSuffix, spdUnitLabel);

    // ── Logarithmic speed mapping ────────────────────────────────
    const float spdLogMin   = std::log (STRETRAudioProcessor::kChaosSpdMin);
    const float spdLogMax   = std::log (STRETRAudioProcessor::kChaosSpdMax);
    const float spdLogRange = spdLogMax - spdLogMin;

    auto hzToBar = [spdLogMin, spdLogRange] (float hz) -> float
    { if (hz <= STRETRAudioProcessor::kChaosSpdMin) return 0.0f;
      if (hz >= STRETRAudioProcessor::kChaosSpdMax) return 1.0f;
      return (std::log (hz) - spdLogMin) / spdLogRange; };
    auto barToHz = [spdLogMin, spdLogRange] (float v01) -> float
    { return std::exp (spdLogMin + v01 * spdLogRange); };

    auto* amtBar = new PromptBar (scheme, currentAmt * 0.01f, STRETRAudioProcessor::kChaosAmtDefault * 0.01f);
    auto* spdBar = new PromptBar (scheme, hzToBar (currentSpd), hzToBar (STRETRAudioProcessor::kChaosSpdDefault));
    aw->addAndMakeVisible (amtBar);
    aw->addAndMakeVisible (spdBar);

    if (amtSuffix != nullptr) amtSuffix->pairedBar = amtBar;
    if (spdSuffix != nullptr) spdSuffix->pairedBar = spdBar;

    // ── Bar ↔ text sync ──────────────────────────────────────────
    auto syncing = std::make_shared<bool> (false);
    auto* amtApvts = audioProcessor.apvts.getParameter (amtParamId);
    auto* spdApvts = audioProcessor.apvts.getParameter (spdParamId);

    auto barToTextAmt = [aw, syncing, amtApvts] (float v01)
    { if (*syncing) return; *syncing = true;
      if (auto* te = aw->getTextEditor ("amt")) { te->setText (juce::String (juce::roundToInt (v01 * 100.0f)), juce::sendNotification); te->selectAll(); }
      if (amtApvts != nullptr) amtApvts->setValueNotifyingHost (amtApvts->convertTo0to1 (v01 * 100.0f));
      *syncing = false; };

    auto barToTextSpd = [aw, syncing, spdApvts, barToHz] (float v01)
    { if (*syncing) return; *syncing = true;
      const float hz = juce::jlimit (STRETRAudioProcessor::kChaosSpdMin, STRETRAudioProcessor::kChaosSpdMax, barToHz (v01));
      if (auto* te = aw->getTextEditor ("spd")) { te->setText (juce::String (hz, 2), juce::sendNotification); te->selectAll(); }
      if (spdApvts != nullptr) spdApvts->setValueNotifyingHost (spdApvts->convertTo0to1 (hz));
      *syncing = false; };

    amtBar->onValueChanged = barToTextAmt;
    spdBar->onValueChanged = barToTextSpd;

    // ── Layout rows ──────────────────────────────────────────────
    auto layoutRows = [aw, amtSuffix, spdSuffix, amtUnitLabel, spdUnitLabel, amtBar, spdBar] ()
    {
        auto* amtTe = aw->getTextEditor ("amt");
        auto* spdTe = aw->getTextEditor ("spd");
        if (amtTe == nullptr || spdTe == nullptr) return;
        const int buttonsTop = getAlertButtonsTop (*aw);
        const int rowH = amtTe->getHeight();
        const int barH = juce::jmax (10, rowH / 2);
        const int barGap = juce::jmax (2, rowH / 6);
        const int rowTotal = rowH + barGap + barH;
        const int gap = juce::jmax (4, rowH / 3);
        const int totalH = rowTotal * 2 + gap;
        const int startY = juce::jmax (kPromptEditorMinTopPx, (buttonsTop - totalH) / 2);
        const int contentPad = kPromptInlineContentPadPx;
        const int contentW = aw->getWidth() - contentPad * 2;
        const auto& font = amtTe->getFont();
        const int spaceW = juce::jmax (2, stringWidth (font, " "));

        auto placeRow = [&] (juce::TextEditor* te, juce::Label* suffix, juce::Label* unitLabel, PromptBar* bar, int y)
        {
            if (te == nullptr || suffix == nullptr || bar == nullptr) return;
            const int labelW = stringWidth (suffix->getFont(), suffix->getText()) + 2;
            const auto txt = te->getText();
            const int textW = juce::jmax (1, stringWidth (font, txt));
            const int unitW = (unitLabel != nullptr) ? stringWidth (font, unitLabel->getText()) + 2 : 0;
            constexpr int kEditorTextPadPx = 12; constexpr int kMinEditorWidthPx = 24;
            const int maxEditorWidthPx = (unitLabel != nullptr && unitLabel->getText() == "Hz")
                ? juce::jmax (80, stringWidth (font, "100.00") + kEditorTextPadPx * 2)
                : 80;
            const int editorW = juce::jlimit (kMinEditorWidthPx, maxEditorWidthPx, textW + kEditorTextPadPx * 2);
            const int visualW = labelW + spaceW + textW + unitW;
            const int centerX = contentPad + contentW / 2;
            int blockLeft = juce::jlimit (contentPad, juce::jmax (contentPad, contentPad + contentW - visualW), centerX - visualW / 2);
            suffix->setBounds (blockLeft, y, labelW, rowH);
            int teX = blockLeft + labelW + spaceW - (editorW - textW) / 2;
            teX = juce::jlimit (contentPad, juce::jmax (contentPad, contentPad + contentW - editorW), teX);
            te->setBounds (teX, y, editorW, rowH);
            if (unitLabel != nullptr) { const int textRightX = blockLeft + labelW + spaceW + textW; unitLabel->setBounds (textRightX, y, unitW, rowH); }
            const int barX = kPromptInnerMargin;
            const int barW = juce::jmax (60, aw->getWidth() - kPromptInnerMargin * 2);
            bar->setBounds (barX, y + rowH + barGap, barW, barH);
        };
        placeRow (amtTe, amtSuffix, amtUnitLabel, amtBar, startY);
        placeRow (spdTe, spdSuffix, spdUnitLabel, spdBar, startY + rowTotal + gap);
    };

    // ── Text → bar sync ──────────────────────────────────────────
    auto textToBar = [syncing, hzToBar] (juce::TextEditor* te, PromptBar* bar, juce::RangedAudioParameter* param, bool isSpeed)
    {
        if (*syncing || te == nullptr || bar == nullptr) return;
        *syncing = true;
        const float raw = juce::jlimit (0.0f, 100.0f, te->getText().getFloatValue());
        if (isSpeed)
        { const float hz = juce::jlimit (STRETRAudioProcessor::kChaosSpdMin, STRETRAudioProcessor::kChaosSpdMax, raw);
          bar->value = hzToBar (hz);
          if (param != nullptr) param->setValueNotifyingHost (param->convertTo0to1 (hz)); }
        else
        { bar->value = raw * 0.01f;
          if (param != nullptr) param->setValueNotifyingHost (param->convertTo0to1 (raw)); }
        bar->repaint(); *syncing = false;
    };

    if (auto* amtTe = aw->getTextEditor ("amt"))
        amtTe->onTextChange = [layoutRows, amtTe, amtBar, textToBar, amtApvts] () mutable { textToBar (amtTe, amtBar, amtApvts, false); layoutRows(); };
    if (auto* spdTe = aw->getTextEditor ("spd"))
        spdTe->onTextChange = [layoutRows, spdTe, spdBar, textToBar, spdApvts] () mutable { textToBar (spdTe, spdBar, spdApvts, true); layoutRows(); };

    aw->addButton ("OK", 1, juce::KeyPress (juce::KeyPress::returnKey));
    aw->addButton ("CANCEL", 0, juce::KeyPress (juce::KeyPress::escapeKey));
    aw->setEscapeKeyCancels (true);
    applyPromptShellSize (*aw);
    layoutAlertWindowButtons (*aw);
    layoutRows();

    const auto& kChaosFont = kBoldFont40();
    preparePromptTextEditor (*aw, "amt", scheme.bg, scheme.text, scheme.fg, kChaosFont, false);
    preparePromptTextEditor (*aw, "spd", scheme.bg, scheme.text, scheme.fg, kChaosFont, false);
    layoutRows();
    styleAlertButtons (*aw, lnf);

    juce::Component::SafePointer<STRETRAudioProcessorEditor> safeThis (this);

    if (safeThis != nullptr)
    {
        fitAlertWindowToEditor (*aw, safeThis.getComponent(), [layoutRows] (juce::AlertWindow& a)
        { juce::ignoreUnused (a); layoutAlertWindowButtons (a); layoutRows(); });
        embedAlertWindowInOverlay (safeThis.getComponent(), aw);
    }
    else
    {
        aw->centreAroundComponent (this, aw->getWidth(), aw->getHeight());
        bringPromptWindowToFront (*aw);
    }

    {
        preparePromptTextEditor (*aw, "amt", scheme.bg, scheme.text, scheme.fg, kChaosFont, false);
        preparePromptTextEditor (*aw, "spd", scheme.bg, scheme.text, scheme.fg, kChaosFont, false);
        layoutRows();
        if (amtSuffix != nullptr) { if (auto* te = aw->getTextEditor ("amt")) { amtSuffix->setFont (te->getFont()); if (amtUnitLabel != nullptr) amtUnitLabel->setFont (te->getFont()); } }
        if (spdSuffix != nullptr) { if (auto* te = aw->getTextEditor ("spd")) { spdSuffix->setFont (te->getFont()); if (spdUnitLabel != nullptr) spdUnitLabel->setFont (te->getFont()); } }
        layoutRows();
        juce::Component::SafePointer<juce::AlertWindow> safeAw (aw);
        juce::MessageManager::callAsync ([safeAw]() { if (safeAw == nullptr) return; bringPromptWindowToFront (*safeAw); safeAw->repaint(); });
    }

    aw->enterModalState (true,
        juce::ModalCallbackFunction::create (
            [safeThis, aw, amtBar, spdBar, savedAmt = currentAmt, savedSpd = currentSpd,
             spdLogMin, spdLogRange, amtParamId, spdParamId] (int result) mutable
        {
            std::unique_ptr<juce::AlertWindow> killer (aw);
            if (safeThis != nullptr) safeThis->setPromptOverlayActive (false);
            if (safeThis == nullptr) return;
            if (result != 1)
            {
                if (auto* p = safeThis->audioProcessor.apvts.getParameter (amtParamId))
                    p->setValueNotifyingHost (p->convertTo0to1 (savedAmt));
                if (auto* p = safeThis->audioProcessor.apvts.getParameter (spdParamId))
                    p->setValueNotifyingHost (p->convertTo0to1 (savedSpd));
                return;
            }
            const float newAmt = juce::jlimit (0.0f, 100.0f, amtBar->value * 100.0f);
            const float newSpd = juce::jlimit (STRETRAudioProcessor::kChaosSpdMin, STRETRAudioProcessor::kChaosSpdMax,
                                                std::exp (spdLogMin + juce::jlimit (0.0f, 1.0f, spdBar->value) * spdLogRange));
            const auto tip = formatChaosTooltip (newAmt, newSpd);
            if (juce::String (amtParamId) == STRETRAudioProcessor::kParamChaosAmtFilter)
                safeThis->chaosFilterDisplay.setTooltip (tip);
            else
                safeThis->chaosDelayDisplay.setTooltip (tip);
        }), false);
}

void STRETRAudioProcessorEditor::openChaosFilterPrompt()
{
    openChaosConfigPrompt (STRETRAudioProcessor::kParamChaosAmtFilter,
                           STRETRAudioProcessor::kParamChaosSpdFilter,
                           "CHS F");
}

void STRETRAudioProcessorEditor::openChaosDelayPrompt()
{
    openChaosConfigPrompt (STRETRAudioProcessor::kParamChaosAmt,
                           STRETRAudioProcessor::kParamChaosSpd,
                           "CHS D");
}

// ── Info popup layout helper ─────────────────────────────────────
static void layoutInfoPopupContent (juce::AlertWindow& aw)
{
    using namespace TR;
    layoutAlertWindowButtons (aw);

    const int contentTop = kPromptBodyTopPad;
    const int contentBottom = getAlertButtonsTop (aw) - kPromptBodyBottomPad;
    const int contentH = juce::jmax (0, contentBottom - contentTop);
    const int bodyW = aw.getWidth() - (2 * kPromptInnerMargin);

    auto* viewport = dynamic_cast<juce::Viewport*> (aw.findChildWithID ("bodyViewport"));
    if (viewport == nullptr) return;

    viewport->setBounds (kPromptInnerMargin, contentTop, bodyW, contentH);

    auto* content = viewport->getViewedComponent();
    if (content == nullptr) return;

    constexpr int kItemGap = 10;
    int y = 0;
    const int innerW = bodyW - 10;

    for (int i = 0; i < content->getNumChildComponents(); ++i)
    {
        auto* child = content->getChildComponent (i);
        if (child == nullptr || ! child->isVisible()) continue;

        int itemH = 30;
        if (auto* label = dynamic_cast<juce::Label*> (child))
        {
            auto font = label->getFont();
            const auto text = label->getText();
            const auto border = label->getBorderSize();

            if (! text.containsChar ('\n'))
                itemH = (int) std::ceil (font.getHeight()) + border.getTopAndBottom();
            else
            {
                juce::AttributedString as;
                as.append (text, font, label->findColour (juce::Label::textColourId));
                as.setJustification (label->getJustificationType());
                juce::TextLayout layout;
                const int textAreaW = innerW - border.getLeftAndRight();
                layout.createLayout (as, (float) juce::jmax (1, textAreaW));
                itemH = juce::jmax (20, (int) std::ceil (layout.getHeight() + font.getDescent())
                                        + border.getTopAndBottom() + 4);
            }
        }
        else if (dynamic_cast<juce::HyperlinkButton*> (child) != nullptr)
            itemH = 28;

        child->setBounds (0, y, innerW, itemH);

        if (auto* label = dynamic_cast<juce::Label*> (child))
        {
            const auto& props = label->getProperties();
            if (props.contains ("poemPadFraction"))
            {
                const float padFrac = (float) props["poemPadFraction"];
                const int padPx = juce::jmax (4, (int) std::round (innerW * padFrac));
                label->setBorderSize (juce::BorderSize<int> (0, padPx, 0, padPx));

                auto font = label->getFont();
                const int textAreaW = innerW - 2 * padPx;
                for (float scale = 1.0f; scale >= 0.65f; scale -= 0.025f)
                {
                    font.setHorizontalScale (scale);
                    juce::GlyphArrangement glyphs;
                    glyphs.addLineOfText (font, label->getText(), 0.0f, 0.0f);
                    if (static_cast<int> (std::ceil (glyphs.getBoundingBox (0, -1, false).getWidth())) <= textAreaW)
                        break;
                }
                label->setFont (font);
            }
        }

        y += itemH + kItemGap;
    }

    if (y > kItemGap) y -= kItemGap;
    content->setSize (innerW, juce::jmax (contentH, y));
}

void STRETRAudioProcessorEditor::openInfoPopup()
{
    lnf.setScheme (activeScheme);

    setPromptOverlayActive (true);

    auto* aw = new juce::AlertWindow ("", "", juce::AlertWindow::NoIcon);
    juce::Component::SafePointer<juce::AlertWindow> safeAw (aw);
    juce::Component::SafePointer<STRETRAudioProcessorEditor> safeThis (this);
    aw->setLookAndFeel (&lnf);
    aw->addButton ("OK", 1, juce::KeyPress (juce::KeyPress::returnKey));
    aw->addButton ("GRAPHICS", 2);

    applyPromptShellSize (*aw);

    auto* bodyContent = new juce::Component();
    bodyContent->setComponentID ("bodyContent");

    auto infoFont = lnf.getAlertWindowMessageFont();
    infoFont.setHeight (infoFont.getHeight() * 1.45f);

    auto headingFont = infoFont;
    headingFont.setBold (true);
    headingFont.setHeight (infoFont.getHeight() * 1.25f);

    auto linkFont = infoFont;
    linkFont.setHeight (infoFont.getHeight() * 1.08f);

    auto poemFont = infoFont;
    poemFont.setItalic (true);

    auto xmlDoc = juce::XmlDocument::parse (InfoContent::xml);
    auto* contentNode = xmlDoc != nullptr ? xmlDoc->getChildByName ("content") : nullptr;

    if (contentNode != nullptr)
    {
        int elemIdx = 0;
        for (auto* node : contentNode->getChildIterator())
        {
            const auto tag  = node->getTagName();
            const auto text = node->getAllSubText().trim();
            const auto id   = tag + juce::String (elemIdx++);

            if (tag == "heading")
            {
                auto* l = new juce::Label (id, text);
                l->setComponentID (id);
                l->setJustificationType (juce::Justification::centred);
                applyLabelTextColour (*l, activeScheme.text);
                l->setFont (headingFont);
                bodyContent->addAndMakeVisible (l);
            }
            else if (tag == "text" || tag == "separator")
            {
                auto* l = new juce::Label (id, text);
                l->setComponentID (id);
                l->setJustificationType (juce::Justification::centred);
                applyLabelTextColour (*l, activeScheme.text);
                l->setFont (infoFont);
                l->setBorderSize (juce::BorderSize<int> (0));
                bodyContent->addAndMakeVisible (l);
            }
            else if (tag == "link")
            {
                const auto url = node->getStringAttribute ("url");
                auto* lnk = new juce::HyperlinkButton (text, juce::URL (url));
                lnk->setComponentID (id);
                lnk->setJustificationType (juce::Justification::centred);
                lnk->setColour (juce::HyperlinkButton::textColourId, activeScheme.text);
                lnk->setFont (linkFont, false, juce::Justification::centred);
                lnk->setTooltip ("");
                bodyContent->addAndMakeVisible (lnk);
            }
            else if (tag == "poem")
            {
                auto* l = new juce::Label (id, text);
                l->setComponentID (id);
                l->setJustificationType (juce::Justification::centred);
                applyLabelTextColour (*l, activeScheme.text);
                l->setFont (poemFont);
                l->setBorderSize (juce::BorderSize<int> (0, 0, 0, 0));
                l->getProperties().set ("poemPadFraction", 0.12f);
                bodyContent->addAndMakeVisible (l);
            }
            else if (tag == "spacer")
            {
                auto* l = new juce::Label (id, "");
                l->setComponentID (id);
                l->setFont (infoFont);
                l->setBorderSize (juce::BorderSize<int> (0));
                bodyContent->addAndMakeVisible (l);
            }
        }
    }

    auto* viewport = new juce::Viewport();
    viewport->setComponentID ("bodyViewport");
    viewport->setViewedComponent (bodyContent, true);
    viewport->setScrollBarsShown (true, false);
    viewport->setScrollBarThickness (8);
    viewport->setLookAndFeel (&lnf);
    aw->addAndMakeVisible (viewport);

    layoutInfoPopupContent (*aw);

    if (safeThis != nullptr)
    {
        fitAlertWindowToEditor (*aw, safeThis.getComponent(), [] (juce::AlertWindow& a)
        { layoutInfoPopupContent (a); });
        embedAlertWindowInOverlay (safeThis.getComponent(), aw);
    }
    else
    {
        aw->centreAroundComponent (this, aw->getWidth(), aw->getHeight());
        bringPromptWindowToFront (*aw); aw->repaint();
    }

    juce::MessageManager::callAsync ([safeAw, safeThis]()
    {
        if (safeAw == nullptr || safeThis == nullptr) return;
        bringPromptWindowToFront (*safeAw);
        safeAw->repaint();
    });

    aw->enterModalState (true,
        juce::ModalCallbackFunction::create ([safeThis, aw] (int result) mutable
        {
            std::unique_ptr<juce::AlertWindow> killer (aw);
            if (safeThis == nullptr) return;
            if (result == 2)
            {
                safeThis->openGraphicsPopup();
                return;
            }
            safeThis->setPromptOverlayActive (false);
        }));
}

// ── Graphics popup helper: sync state ────────────────────────────
static void syncGraphicsPopupState (juce::AlertWindow& aw,
                                    const std::array<juce::Colour, 2>& defaultPalette,
                                    const std::array<juce::Colour, 2>& customPalette,
                                    bool useCustomPalette)
{
    using namespace TR;
    if (auto* t = dynamic_cast<juce::ToggleButton*> (aw.findChildWithID ("paletteDefaultToggle")))
        t->setToggleState (! useCustomPalette, juce::dontSendNotification);
    if (auto* t = dynamic_cast<juce::ToggleButton*> (aw.findChildWithID ("paletteCustomToggle")))
        t->setToggleState (useCustomPalette, juce::dontSendNotification);

    for (int i = 0; i < 2; ++i)
    {
        if (auto* dflt = dynamic_cast<juce::TextButton*> (aw.findChildWithID ("defaultSwatch" + juce::String (i))))
            setPaletteSwatchColour (*dflt, defaultPalette[(size_t) i]);
        if (auto* custom = dynamic_cast<juce::TextButton*> (aw.findChildWithID ("customSwatch" + juce::String (i))))
        {
            setPaletteSwatchColour (*custom, customPalette[(size_t) i]);
            custom->setTooltip (colourToHexRgb (customPalette[(size_t) i]));
        }
    }

    auto applyLabelTextColourTo = [] (juce::Label* lbl, juce::Colour col)
    { if (lbl != nullptr) lbl->setColour (juce::Label::textColourId, col); };

    const juce::Colour activeText = useCustomPalette ? customPalette[0] : defaultPalette[0];
    applyLabelTextColourTo (dynamic_cast<juce::Label*> (aw.findChildWithID ("paletteDefaultLabel")), activeText);
    applyLabelTextColourTo (dynamic_cast<juce::Label*> (aw.findChildWithID ("paletteCustomLabel")), activeText);
    applyLabelTextColourTo (dynamic_cast<juce::Label*> (aw.findChildWithID ("paletteTitle")), activeText);
    applyLabelTextColourTo (dynamic_cast<juce::Label*> (aw.findChildWithID ("fxLabel")), activeText);
}

// ── Graphics popup helper: layout content ────────────────────────
static void layoutGraphicsPopupContent (juce::AlertWindow& aw)
{
    using namespace TR;
    layoutAlertWindowButtons (aw);

    auto snapEven = [] (int v) { return v & ~1; };

    const int contentLeft = kPromptInnerMargin;
    const int contentRight = aw.getWidth() - kPromptInnerMargin;
    const int contentW = juce::jmax (0, contentRight - contentLeft);

    auto* dfltToggle   = dynamic_cast<juce::ToggleButton*> (aw.findChildWithID ("paletteDefaultToggle"));
    auto* dfltLabel    = dynamic_cast<juce::Label*> (aw.findChildWithID ("paletteDefaultLabel"));
    auto* customToggle = dynamic_cast<juce::ToggleButton*> (aw.findChildWithID ("paletteCustomToggle"));
    auto* customLabel  = dynamic_cast<juce::Label*> (aw.findChildWithID ("paletteCustomLabel"));
    auto* paletteTitle = dynamic_cast<juce::Label*> (aw.findChildWithID ("paletteTitle"));
    auto* fxToggle     = dynamic_cast<juce::ToggleButton*> (aw.findChildWithID ("fxToggle"));
    auto* fxLabel      = dynamic_cast<juce::Label*> (aw.findChildWithID ("fxLabel"));
    auto* okBtn        = aw.getNumButtons() > 0 ? aw.getButton (0) : nullptr;

    constexpr int toggleBox  = GraphicsPromptLayout::toggleBox;
    constexpr int toggleGap  = 4;
    constexpr int toggleVisualInsetLeft = 2;
    constexpr int swatchSize = GraphicsPromptLayout::swatchSize;
    constexpr int swatchGap  = GraphicsPromptLayout::swatchGap;
    constexpr int columnGap  = GraphicsPromptLayout::columnGap;
    constexpr int titleH     = GraphicsPromptLayout::titleHeight;

    const int toggleVisualSide = juce::jlimit (14,
                                               juce::jmax (14, toggleBox - 2),
                                               (int) std::lround ((double) toggleBox * 0.65));

    const int swatchW = swatchSize;
    const int swatchH = (2 * swatchSize) + swatchGap;
    const int swatchGroupSize = (2 * swatchW) + swatchGap;
    const int swatchesH = swatchH;
    const int modeH = toggleBox;

    const int baseGap1 = GraphicsPromptLayout::titleToModeGap;
    const int baseGap2 = GraphicsPromptLayout::modeToSwatchesGap;

    const int titleY = snapEven (kPromptFooterBottomPad);
    const int footerY = getAlertButtonsTop (aw);

    const int bodyH = modeH + baseGap2 + swatchesH;
    const int bodyZoneTop = titleY + titleH + baseGap1;
    const int bodyZoneBottom = footerY - baseGap1;
    const int bodyZoneH = juce::jmax (0, bodyZoneBottom - bodyZoneTop);
    const int bodyY = snapEven (bodyZoneTop + juce::jmax (0, (bodyZoneH - bodyH) / 2));

    const int modeY = bodyY;
    const int blocksY = snapEven (modeY + modeH + baseGap2);

    const int dfltLabelW   = (dfltLabel   != nullptr) ? juce::jmax (38, stringWidth (dfltLabel->getFont(), "DFLT") + 2) : 40;
    const int customLabelW = (customLabel != nullptr) ? juce::jmax (38, stringWidth (customLabel->getFont(), "CSTM") + 2) : 40;
    const int fxLabelW     = (fxLabel != nullptr)
                           ? juce::jmax (90, stringWidth (fxLabel->getFont(), fxLabel->getText().toUpperCase()) + 2)
                           : 96;

    const int toggleLabelStartOffset = toggleVisualInsetLeft + toggleVisualSide + toggleGap;
    const int dfltRowW   = toggleLabelStartOffset + dfltLabelW;
    const int customRowW = toggleLabelStartOffset + customLabelW;
    const int fxRowW     = toggleLabelStartOffset + fxLabelW;
    const int okBtnW     = (okBtn != nullptr) ? okBtn->getWidth() : 96;

    const int leftColumnW  = juce::jmax (swatchGroupSize, juce::jmax (dfltRowW, fxRowW));
    const int rightColumnW = juce::jmax (swatchGroupSize, juce::jmax (customRowW, okBtnW));
    const int columnsRowW  = leftColumnW + columnGap + rightColumnW;
    const int columnsX     = snapEven (contentLeft + juce::jmax (0, (contentW - columnsRowW) / 2));
    const int col0X = columnsX;
    const int col1X = columnsX + leftColumnW + columnGap;

    if (paletteTitle != nullptr)
    {
        const int paletteW = juce::jmax (100, juce::jmin (leftColumnW, contentRight - col0X));
        paletteTitle->setBounds (col0X, titleY, paletteW, titleH);
    }

    if (dfltToggle   != nullptr) dfltToggle->setBounds (col0X, modeY, toggleBox, toggleBox);
    if (dfltLabel    != nullptr) dfltLabel->setBounds (col0X + toggleLabelStartOffset, modeY, dfltLabelW, toggleBox);
    if (customToggle != nullptr) customToggle->setBounds (col1X, modeY, toggleBox, toggleBox);
    if (customLabel  != nullptr) customLabel->setBounds (col1X + toggleLabelStartOffset, modeY, customLabelW, toggleBox);

    auto placeSwatchGroup = [&] (const juce::String& prefix, int startX)
    {
        for (int i = 0; i < 2; ++i)
            if (auto* b = dynamic_cast<juce::TextButton*> (aw.findChildWithID (prefix + juce::String (i))))
                b->setBounds (startX + i * (swatchW + swatchGap), blocksY, swatchW, swatchH);
    };
    placeSwatchGroup ("defaultSwatch", col0X);
    placeSwatchGroup ("customSwatch", col1X);

    if (okBtn != nullptr)
    {
        auto okR = okBtn->getBounds();
        okR.setX (col1X);
        okR.setY (footerY);
        okBtn->setBounds (okR);

        const int fxY = snapEven (footerY + juce::jmax (0, (okR.getHeight() - toggleBox) / 2));
        if (fxToggle != nullptr) fxToggle->setBounds (col0X, fxY, toggleBox, toggleBox);
        if (fxLabel  != nullptr) fxLabel->setBounds (col0X + toggleLabelStartOffset, fxY, fxLabelW, toggleBox);
    }
}

void STRETRAudioProcessorEditor::openGraphicsPopup()
{
    lnf.setScheme (activeScheme);
    useCustomPalette = audioProcessor.getUiUseCustomPalette();
    crtEnabled = audioProcessor.getUiCrtEnabled();
    crtEffect.setEnabled (crtEnabled);
    applyActivePalette();

    setPromptOverlayActive (true);

    auto* aw = new juce::AlertWindow ("", "", juce::AlertWindow::NoIcon);
    juce::Component::SafePointer<STRETRAudioProcessorEditor> safeThis (this);
    juce::Component::SafePointer<juce::AlertWindow> safeAw (aw);
    aw->setLookAndFeel (&lnf);
    aw->addButton ("OK", 1, juce::KeyPress (juce::KeyPress::returnKey));

    auto labelFont = lnf.getAlertWindowMessageFont();
    labelFont.setHeight (labelFont.getHeight() * 1.20f);

    auto addPopupLabel = [this, aw] (const juce::String& id, const juce::String& text, juce::Font font,
                                     juce::Justification justification = juce::Justification::centredLeft)
    {
        auto* label = new PopupClickableLabel (id, text);
        label->setComponentID (id); label->setJustificationType (justification);
        applyLabelTextColour (*label, activeScheme.text);
        label->setBorderSize (juce::BorderSize<int> (0));
        label->setFont (font); label->setMouseCursor (juce::MouseCursor::PointingHandCursor);
        aw->addAndMakeVisible (label);
        return label;
    };

    auto* defaultToggle = new juce::ToggleButton ("");
    defaultToggle->setComponentID ("paletteDefaultToggle");
    aw->addAndMakeVisible (defaultToggle);
    auto* defaultLabel = addPopupLabel ("paletteDefaultLabel", "DFLT", labelFont);

    auto* customToggle = new juce::ToggleButton ("");
    customToggle->setComponentID ("paletteCustomToggle");
    aw->addAndMakeVisible (customToggle);
    auto* customLabel = addPopupLabel ("paletteCustomLabel", "CSTM", labelFont);

    auto paletteTitleFont = labelFont;
    paletteTitleFont.setHeight (paletteTitleFont.getHeight() * 1.30f);
    addPopupLabel ("paletteTitle", "PALETTE", paletteTitleFont, juce::Justification::centredLeft);

    for (int i = 0; i < 2; ++i)
    {
        auto* dflt = new juce::TextButton();
        dflt->setComponentID ("defaultSwatch" + juce::String (i));
        dflt->setTooltip ("Default palette colour " + juce::String (i + 1));
        aw->addAndMakeVisible (dflt);

        auto* custom = new PopupSwatchButton();
        custom->setComponentID ("customSwatch" + juce::String (i));
        custom->setTooltip (colourToHexRgb (customPalette[(size_t) i]));
        aw->addAndMakeVisible (custom);
    }

    auto* fxToggle = new juce::ToggleButton ("");
    fxToggle->setComponentID ("fxToggle");
    fxToggle->setToggleState (crtEnabled, juce::dontSendNotification);
    fxToggle->onClick = [safeThis, fxToggle]()
    {
        if (safeThis == nullptr || fxToggle == nullptr) return;
        safeThis->applyCrtState (fxToggle->getToggleState());
        safeThis->audioProcessor.setUiCrtEnabled (safeThis->crtEnabled);
        safeThis->repaint();
    };
    aw->addAndMakeVisible (fxToggle);

    auto* fxLabel = addPopupLabel ("fxLabel", "GRAPHIC FX", labelFont);

    auto syncAndRepaintPopup = [safeThis, safeAw]()
    {
        if (safeThis == nullptr || safeAw == nullptr) return;
        syncGraphicsPopupState (*safeAw, safeThis->defaultPalette, safeThis->customPalette, safeThis->useCustomPalette);
        layoutGraphicsPopupContent (*safeAw);
        safeAw->repaint();
    };

    auto applyPaletteAndRepaint = [safeThis]()
    { if (safeThis == nullptr) return; safeThis->applyActivePalette(); safeThis->repaint(); };

    defaultToggle->onClick = [safeThis, defaultToggle, customToggle, applyPaletteAndRepaint, syncAndRepaintPopup]() mutable
    {
        if (safeThis == nullptr || defaultToggle == nullptr || customToggle == nullptr) return;
        safeThis->useCustomPalette = false;
        safeThis->audioProcessor.setUiUseCustomPalette (safeThis->useCustomPalette);
        defaultToggle->setToggleState (true, juce::dontSendNotification);
        customToggle->setToggleState (false, juce::dontSendNotification);
        applyPaletteAndRepaint(); syncAndRepaintPopup();
    };

    customToggle->onClick = [safeThis, defaultToggle, customToggle, applyPaletteAndRepaint, syncAndRepaintPopup]() mutable
    {
        if (safeThis == nullptr || defaultToggle == nullptr || customToggle == nullptr) return;
        safeThis->useCustomPalette = true;
        safeThis->audioProcessor.setUiUseCustomPalette (safeThis->useCustomPalette);
        defaultToggle->setToggleState (false, juce::dontSendNotification);
        customToggle->setToggleState (true, juce::dontSendNotification);
        applyPaletteAndRepaint(); syncAndRepaintPopup();
    };

    if (defaultLabel != nullptr && defaultToggle != nullptr)
        defaultLabel->onClick = [defaultToggle]() { defaultToggle->triggerClick(); };
    if (customLabel != nullptr && customToggle != nullptr)
        customLabel->onClick = [customToggle]() { customToggle->triggerClick(); };
    if (fxLabel != nullptr && fxToggle != nullptr)
        fxLabel->onClick = [fxToggle]() { fxToggle->triggerClick(); };

    for (int i = 0; i < 2; ++i)
    {
        if (auto* customSwatch = dynamic_cast<PopupSwatchButton*> (aw->findChildWithID ("customSwatch" + juce::String (i))))
        {
            customSwatch->onLeftClick = [safeThis, safeAw, i]()
            {
                if (safeThis == nullptr) return;
                auto& rng = juce::Random::getSystemRandom();
                const auto randomColour = juce::Colour::fromRGB ((juce::uint8) rng.nextInt (256),
                                                                 (juce::uint8) rng.nextInt (256),
                                                                 (juce::uint8) rng.nextInt (256));
                safeThis->customPalette[(size_t) i] = randomColour;
                safeThis->audioProcessor.setUiCustomPaletteColour (i, randomColour);
                if (safeThis->useCustomPalette) { safeThis->applyActivePalette(); safeThis->repaint(); }
                if (safeAw != nullptr)
                { syncGraphicsPopupState (*safeAw, safeThis->defaultPalette, safeThis->customPalette, safeThis->useCustomPalette);
                  layoutGraphicsPopupContent (*safeAw); safeAw->repaint(); }
            };

            customSwatch->onRightClick = [safeThis, safeAw, i]()
            {
                if (safeThis == nullptr) return;
                const auto scheme = safeThis->activeScheme;
                auto* colorAw = new juce::AlertWindow ("", "", juce::AlertWindow::NoIcon);
                colorAw->setLookAndFeel (&safeThis->lnf);
                colorAw->addTextEditor ("hex", colourToHexRgb (safeThis->customPalette[(size_t) i]), juce::String());
                if (auto* te = colorAw->getTextEditor ("hex")) te->setInputFilter (new HexInputFilter(), true);
                colorAw->addButton ("OK", 1, juce::KeyPress (juce::KeyPress::returnKey));
                colorAw->addButton ("CANCEL", 0, juce::KeyPress (juce::KeyPress::escapeKey));
                styleAlertButtons (*colorAw, safeThis->lnf);
                applyPromptShellSize (*colorAw);
                layoutAlertWindowButtons (*colorAw);
                const juce::Font& kHexPromptFont = kBoldFont40();
                preparePromptTextEditor (*colorAw, "hex", scheme.bg, scheme.text, scheme.fg, kHexPromptFont, true, 6);

                if (safeThis != nullptr)
                {
                    fitAlertWindowToEditor (*colorAw, safeThis.getComponent(), [&] (juce::AlertWindow& a)
                    { layoutAlertWindowButtons (a); preparePromptTextEditor (a, "hex", scheme.bg, scheme.text, scheme.fg, kHexPromptFont, true, 6); });
                    embedAlertWindowInOverlay (safeThis.getComponent(), colorAw, true);
                }
                else
                {
                    colorAw->centreAroundComponent (safeThis.getComponent(), colorAw->getWidth(), colorAw->getHeight());
                    bringPromptWindowToFront (*colorAw); colorAw->repaint();
                }

                preparePromptTextEditor (*colorAw, "hex", scheme.bg, scheme.text, scheme.fg, kHexPromptFont, true, 6);
                juce::Component::SafePointer<juce::AlertWindow> safeColorAw (colorAw);
                juce::MessageManager::callAsync ([safeColorAw]() { if (safeColorAw != nullptr) { bringPromptWindowToFront (*safeColorAw); safeColorAw->repaint(); } });

                colorAw->enterModalState (true,
                    juce::ModalCallbackFunction::create ([safeThis, safeAw, colorAw, i] (int result) mutable
                    {
                        std::unique_ptr<juce::AlertWindow> killer (colorAw);
                        if (safeThis == nullptr) return;
                        if (result != 1) return;
                        juce::Colour parsed;
                        if (! tryParseHexColour (killer->getTextEditorContents ("hex"), parsed)) return;
                        safeThis->customPalette[(size_t) i] = parsed;
                        safeThis->audioProcessor.setUiCustomPaletteColour (i, parsed);
                        if (safeThis->useCustomPalette) { safeThis->applyActivePalette(); safeThis->repaint(); }
                        if (safeAw != nullptr)
                        { syncGraphicsPopupState (*safeAw, safeThis->defaultPalette, safeThis->customPalette, safeThis->useCustomPalette);
                          layoutGraphicsPopupContent (*safeAw); safeAw->repaint(); }
                    }));
            };
        }
    }

    applyPromptShellSize (*aw);
    syncGraphicsPopupState (*aw, defaultPalette, customPalette, useCustomPalette);
    layoutGraphicsPopupContent (*aw);

    if (safeThis != nullptr)
    {
        fitAlertWindowToEditor (*aw, safeThis.getComponent(), [&] (juce::AlertWindow& a)
        { syncGraphicsPopupState (a, defaultPalette, customPalette, useCustomPalette); layoutGraphicsPopupContent (a); });
    }
    if (safeThis != nullptr)
    {
        embedAlertWindowInOverlay (safeThis.getComponent(), aw);
        juce::MessageManager::callAsync ([safeAw, safeThis]()
        { if (safeAw == nullptr || safeThis == nullptr) return; safeAw->toFront (false); safeAw->repaint(); });
    }
    else
    {
        aw->centreAroundComponent (this, aw->getWidth(), aw->getHeight());
        bringPromptWindowToFront (*aw); aw->repaint();
    }

    aw->enterModalState (true,
        juce::ModalCallbackFunction::create ([safeThis, aw] (int) mutable
        { std::unique_ptr<juce::AlertWindow> killer (aw); if (safeThis != nullptr) safeThis->setPromptOverlayActive (false); }));
}
