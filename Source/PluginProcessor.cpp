#include "PluginProcessor.h"
#include "PluginEditor.h"

namespace
{
	constexpr float powInt (float base, int exponent) noexcept
	{
		float result = 1.0f;
		for (int i = 0; i < exponent; ++i)
			result *= base;
		return result;
	}

	constexpr float kGainSmoothCoeff = 0.9955f;
	constexpr float kGainSmoothStep  = 1.0f - kGainSmoothCoeff;
	constexpr float kStretchJitterPitchSmoothStep = 0.95f;
	constexpr float kGrainSizeSmoothSeconds = 0.045f;

	// Developer diagnostics are centralized here so temporary dumps can be
	// switched off without touching DSP paths.
	struct DeveloperDiagnosticsConfig
	{
		static constexpr bool kEnableAutoDump = false;
		static constexpr bool kEnableFftAutoDump = false;
		static constexpr bool kEnableHeavyFftDebugTrace = false;
		static constexpr bool kEnableFft1AmountFreezeDump = STRETR_ENABLE_FFT1_CLICK_DUMP != 0;
#if JUCE_DEBUG
		static constexpr bool kCompileStretchDebugTrace = true;
		static constexpr bool kCompileGrainDebugTrace = true;
		static constexpr bool kCompileFftDebugTrace = true;
#else
		static constexpr bool kCompileStretchDebugTrace = false;
		static constexpr bool kCompileGrainDebugTrace = false;
		static constexpr bool kCompileFftDebugTrace = false;
#endif
		static constexpr bool kEnableFftTraceRecording = kCompileFftDebugTrace && kEnableHeavyFftDebugTrace;
	};

	inline float loadAtomicOrDefault (std::atomic<float>* p, float def) noexcept
	{
		return p != nullptr ? p->load (std::memory_order_relaxed) : def;
	}

	inline int loadIntParamOrDefault (std::atomic<float>* p, int def) noexcept
	{
		return (int) std::lround (loadAtomicOrDefault (p, (float) def));
	}

	inline bool loadBoolParamOrDefault (std::atomic<float>* p, bool def) noexcept
	{
		return loadAtomicOrDefault (p, def ? 1.0f : 0.0f) > 0.5f;
	}

#if STRETR_ENABLE_FFT1_CLICK_DUMP
	inline bool updateDumpMaxDelta (float prevL, float prevR,
	                                float currL, float currR,
	                                int sampleIndex,
	                                int& maxSample,
	                                float& maxAbsDeltaL, float& maxAbsDeltaR,
	                                float& prevAtMaxL, float& prevAtMaxR,
	                                float& currAtMaxL, float& currAtMaxR) noexcept
	{
		const float absDeltaL = std::abs (currL - prevL);
		const float absDeltaR = std::abs (currR - prevR);
		const float currentMax = juce::jmax (maxAbsDeltaL, maxAbsDeltaR);
		const float newMax = juce::jmax (absDeltaL, absDeltaR);
		if (newMax <= currentMax)
			return false;

		maxSample = sampleIndex;
		maxAbsDeltaL = absDeltaL;
		maxAbsDeltaR = absDeltaR;
		prevAtMaxL = prevL;
		prevAtMaxR = prevR;
		currAtMaxL = currL;
		currAtMaxR = currR;
		return true;
	}

	inline bool updateDumpMaxDelta (float prevL, float prevR,
	                                float currL, float currR,
	                                int sampleIndex,
	                                StretrDumpStageDelta& delta) noexcept
	{
		return updateDumpMaxDelta (prevL, prevR,
		                           currL, currR,
		                           sampleIndex,
		                           delta.sample,
		                           delta.absDeltaL,
		                           delta.absDeltaR,
		                           delta.prevL,
		                           delta.prevR,
	                           delta.currL,
	                           delta.currR);
	}
#endif

	inline void setParameterPlainValue (juce::AudioProcessorValueTreeState& apvts,
	                                    const char* paramId, float plainValue)
	{
		if (auto* param = apvts.getParameter (paramId))
		{
			const float norm = param->convertTo0to1 (plainValue);
			param->setValueNotifyingHost (norm);
		}
	}

	inline float fastDecibelsToGain (float dB) noexcept
	{
		return (dB <= -100.0f) ? 0.0f : std::exp2 (dB * 0.16609640474f);
	}

	inline float gainFaderDecibelsToGain (float dB) noexcept
	{
		return (dB <= STRETRAudioProcessor::kGainFloorDb) ? 0.0f : std::exp2 (dB * 0.16609640474f);
	}

	inline float amountToSpeedForEngine (int engineVal, float amountPercent) noexcept
	{
		const float speed = juce::jmax (0.0f, 1.0f - amountPercent * 0.01f);
		if (engineVal == 2 || engineVal == 3)
		{
			// FFT engines use <= 0.0001 as a hard hold/freeze route. Keep the
			// endpoint perceptually at full amount without entering that branch.
			return juce::jmax (speed, 0.0002f);
		}
		return speed;
	}

	inline juce::NormalisableRange<float> makeGainFaderRange() noexcept
	{
		return juce::NormalisableRange<float> (STRETRAudioProcessor::kGainFloorDb,
		                                       STRETRAudioProcessor::kGainMaxDb,
		                                       0.0f,
		                                       STRETRAudioProcessor::kGainSkew);
	}

	inline float fastAtan2Approx (float y, float x) noexcept
	{
		constexpr float kPiOver4 = juce::MathConstants<float>::pi * 0.25f;
		constexpr float k3PiOver4 = juce::MathConstants<float>::pi * 0.75f;
		const float absY = std::abs (y) + 1.0e-12f;

		float angle;
		if (x < 0.0f)
		{
			const float r = (x + absY) / (absY - x);
			angle = k3PiOver4 + (0.1963f * r * r - 0.9817f) * r;
		}
		else
		{
			const float r = (x - absY) / (x + absY);
			angle = kPiOver4 + (0.1963f * r * r - 0.9817f) * r;
		}

		return (y < 0.0f) ? -angle : angle;
	}

	inline float wrapPhaseToPiFast (float phase) noexcept
	{
		const float twoPi = juce::MathConstants<float>::twoPi;
		const float pi = juce::MathConstants<float>::pi;

		if (phase > pi)
		{
			phase -= twoPi;
			if (phase > pi)
				phase -= twoPi;
		}
		else if (phase < -pi)
		{
			phase += twoPi;
			if (phase < -pi)
				phase += twoPi;
		}

		return phase;
	}

    // Wet-signal biquad filter helpers
	using BQC = STRETRAudioProcessor::WetFilterBiquadCoeffs;

	constexpr float kBW4_Q1 = 0.54119610f;
	constexpr float kBW4_Q2 = 1.30656296f;
	constexpr float kBW2_Q  = 0.70710678f;

	inline BQC calcOnePoleLP (float fc, float sr)
	{
		const float w = std::tan (juce::MathConstants<float>::pi * juce::jlimit (1.0f, sr * 0.499f, fc) / sr);
		BQC c; c.b0 = w / (1.0f + w); c.b1 = c.b0; c.b2 = 0.0f;
		c.a1 = (w - 1.0f) / (1.0f + w); c.a2 = 0.0f; return c;
	}
	inline BQC calcOnePoleHP (float fc, float sr)
	{
		const float w = std::tan (juce::MathConstants<float>::pi * juce::jlimit (1.0f, sr * 0.499f, fc) / sr);
		BQC c; c.b0 = 1.0f / (1.0f + w); c.b1 = -c.b0; c.b2 = 0.0f;
		c.a1 = (w - 1.0f) / (1.0f + w); c.a2 = 0.0f; return c;
	}
	inline BQC calcBiquadLP (float fc, float sr, float Q)
	{
		const float w0 = 2.0f * juce::MathConstants<float>::pi * juce::jlimit (1.0f, sr * 0.499f, fc) / sr;
		const float cosw = std::cos (w0), sinw = std::sin (w0);
		const float alpha = sinw / (2.0f * Q), a0inv = 1.0f / (1.0f + alpha);
		BQC c; c.b0 = ((1.0f - cosw) * 0.5f) * a0inv; c.b1 = (1.0f - cosw) * a0inv;
		c.b2 = c.b0; c.a1 = (-2.0f * cosw) * a0inv; c.a2 = (1.0f - alpha) * a0inv; return c;
	}
	inline BQC calcBiquadHP (float fc, float sr, float Q)
	{
		const float w0 = 2.0f * juce::MathConstants<float>::pi * juce::jlimit (1.0f, sr * 0.499f, fc) / sr;
		const float cosw = std::cos (w0), sinw = std::sin (w0);
		const float alpha = sinw / (2.0f * Q), a0inv = 1.0f / (1.0f + alpha);
		BQC c; c.b0 = ((1.0f + cosw) * 0.5f) * a0inv; c.b1 = (-(1.0f + cosw)) * a0inv;
		c.b2 = c.b0; c.a1 = (-2.0f * cosw) * a0inv; c.a2 = (1.0f - alpha) * a0inv; return c;
	}

	inline float processBiquad (float in, const BQC& c,
	                            STRETRAudioProcessor::WetFilterBiquadState& s) noexcept
	{
		const float out = c.b0 * in + s.z1;
		s.z1 = c.b1 * in - c.a1 * out + s.z2;
		s.z2 = c.b2 * in - c.a2 * out;
		return out;
	}
}

STRETRAudioProcessor::WindowFamily STRETRAudioProcessor::getWindowFamilyForEngineInternal (int engineVal) const noexcept
{
	if (engineVal == 1)
		return WindowFamily::grain;
	if (engineVal == 2)
		return WindowFamily::fft1;
	if (engineVal == 3)
		return WindowFamily::fft2;

	return WindowFamily::stretch;
}

int STRETRAudioProcessor::getCanonicalWindowForFamily (WindowFamily family, int windowValue) const noexcept
{
	const int clamped = juce::jlimit (kWindowMin, kWindowMax, windowValue);
	if (family == WindowFamily::fft1 || family == WindowFamily::fft2)
		return juce::jmin (getCanonicalFftWindow (clamped), getCurrentMaxFftWindow());
	return clamped;
}

int STRETRAudioProcessor::getStoredWindowForFamily (WindowFamily family) const noexcept
{
	const int index = (int) family;
	return getCanonicalWindowForFamily (family,
	                                    windowFamilyValues_[index].load (std::memory_order_relaxed));
}

void STRETRAudioProcessor::setStoredWindowForFamily (WindowFamily family, int windowValue) noexcept
{
	const int index = (int) family;
	windowFamilyValues_[index].store (getCanonicalWindowForFamily (family, windowValue),
	                                  std::memory_order_relaxed);
}

void STRETRAudioProcessor::initialiseWindowFamilies (int fallbackWindow) noexcept
{
	const int safeWindow = juce::jlimit (kWindowMin, kWindowMax, fallbackWindow);
	setStoredWindowForFamily (WindowFamily::stretch, safeWindow);
	setStoredWindowForFamily (WindowFamily::grain, safeWindow);
	setStoredWindowForFamily (WindowFamily::fft1, safeWindow);
	setStoredWindowForFamily (WindowFamily::fft2, safeWindow);

	for (int i = 0; i < 4; ++i)
		smoothedWindowByFamily_[(size_t) i] = (float) getStoredWindowForFamily ((WindowFamily) i);

	const auto family = getWindowFamilyForEngineInternal (loadIntParamOrDefault (engineParam, 0));
	activeWindowFamily_.store ((int) family, std::memory_order_relaxed);
	lastObservedWindowParam_.store (safeWindow, std::memory_order_relaxed);
	smoothedWindow_ = (float) getStoredWindowForFamily (family);
	windowFamiliesInitialised_.store (true, std::memory_order_relaxed);
}

void STRETRAudioProcessor::restoreWindowFamilyStateFromTree() noexcept
{
	const int fallbackWindow = loadIntParamOrDefault (windowParam, (int) kWindowDefault);
	const auto legacyFftWindow = apvts.state.getProperty (UiStateKeys::fftWindow);
	const int legacyFftFallback = juce::jlimit (kWindowMin,
	                                           kWindowMax,
	                                           legacyFftWindow.isVoid() ? fallbackWindow : (int) legacyFftWindow);

	auto readWindowProperty = [this] (const char* key, int propertyFallback)
	{
		const auto fromState = apvts.state.getProperty (key);
		return juce::jlimit (kWindowMin,
		                     kWindowMax,
		                     fromState.isVoid() ? propertyFallback : (int) fromState);
	};

	setStoredWindowForFamily (WindowFamily::stretch, readWindowProperty (UiStateKeys::stretchWindow, fallbackWindow));
	setStoredWindowForFamily (WindowFamily::grain,   readWindowProperty (UiStateKeys::grainWindow, fallbackWindow));
	setStoredWindowForFamily (WindowFamily::fft1,    readWindowProperty (UiStateKeys::fft1Window, legacyFftFallback));
	setStoredWindowForFamily (WindowFamily::fft2,    readWindowProperty (UiStateKeys::fft2Window, legacyFftFallback));

	for (int i = 0; i < 4; ++i)
		smoothedWindowByFamily_[(size_t) i] = (float) getStoredWindowForFamily ((WindowFamily) i);

	const auto family = getWindowFamilyForEngineInternal (loadIntParamOrDefault (engineParam, 0));
	activeWindowFamily_.store ((int) family, std::memory_order_relaxed);
	lastObservedWindowParam_.store (loadIntParamOrDefault (windowParam, (int) kWindowDefault),
	                                std::memory_order_relaxed);
	smoothedWindow_ = smoothedWindowByFamily_[(size_t) family];
	windowFamiliesInitialised_.store (true, std::memory_order_relaxed);
}

void STRETRAudioProcessor::writeWindowFamilyStateToTree (juce::ValueTree& state) const
{
	state.setProperty (UiStateKeys::stretchWindow,
	                   getStoredWindowForFamily (WindowFamily::stretch),
	                   nullptr);
	state.setProperty (UiStateKeys::grainWindow,
	                   getStoredWindowForFamily (WindowFamily::grain),
	                   nullptr);
	state.setProperty (UiStateKeys::fft1Window,
	                   getStoredWindowForFamily (WindowFamily::fft1),
	                   nullptr);
	state.setProperty (UiStateKeys::fft2Window,
	                   getStoredWindowForFamily (WindowFamily::fft2),
	                   nullptr);
	state.setProperty (UiStateKeys::fftWindow,
	                   getStoredWindowForFamily (WindowFamily::fft1),
	                   nullptr);
}

int STRETRAudioProcessor::getCurrentMaxFftWindow() const noexcept
{
	return getCanonicalFftWindow (loadIntParamOrDefault (maxWindowParam, kFftMaxWindowDefault));
}

void STRETRAudioProcessor::clampFftWindowFamiliesToMax (int maxWindow) noexcept
{
	const int safeMax = getCanonicalFftWindow (maxWindow);
	const int fft1 = getCanonicalFftWindow (windowFamilyValues_[(int) WindowFamily::fft1].load (std::memory_order_relaxed));
	const int fft2 = getCanonicalFftWindow (windowFamilyValues_[(int) WindowFamily::fft2].load (std::memory_order_relaxed));

	if (fft1 > safeMax)
		setStoredWindowForFamily (WindowFamily::fft1, safeMax);
	if (fft2 > safeMax)
		setStoredWindowForFamily (WindowFamily::fft2, safeMax);

	const auto activeFamily = (WindowFamily) activeWindowFamily_.load (std::memory_order_relaxed);
	if (activeFamily == WindowFamily::fft1 || activeFamily == WindowFamily::fft2)
		lastObservedWindowParam_.store (getStoredWindowForFamily (activeFamily), std::memory_order_relaxed);
}

//==============================================================================
STRETRAudioProcessor::STRETRAudioProcessor()
#ifndef JucePlugin_PreferredChannelConfigurations
	: AudioProcessor (BusesProperties()
	                 #if ! JucePlugin_IsMidiEffect
	                  #if ! JucePlugin_IsSynth
	                   .withInput  ("Input", juce::AudioChannelSet::stereo(), true)
	                  #endif
	                   .withOutput ("Output", juce::AudioChannelSet::stereo(), true)
	                 #endif
	                   )
#endif
	, apvts (*this, nullptr, "Parameters", createParameterLayout())
{
	amountParam  = apvts.getRawParameterValue (kParamAmount);
	pitchParam     = apvts.getRawParameterValue (kParamPitch);
	jitterParam  = apvts.getRawParameterValue (kParamJitter);
	grainParam   = apvts.getRawParameterValue (kParamGrain);
	engineParam  = apvts.getRawParameterValue (kParamEngine);
	windowParam  = apvts.getRawParameterValue (kParamWindow);
	maxWindowParam = apvts.getRawParameterValue (kParamMaxWindow);
	styleParam   = apvts.getRawParameterValue (kParamStyle);
	inputParam   = apvts.getRawParameterValue (kParamInput);
	outputParam  = apvts.getRawParameterValue (kParamOutput);
	mixParam     = apvts.getRawParameterValue (kParamMix);
	modeInParam  = apvts.getRawParameterValue (kParamModeIn);
	modeOutParam = apvts.getRawParameterValue (kParamModeOut);
	sumBusParam  = apvts.getRawParameterValue (kParamSumBus);
	limThresholdParam = apvts.getRawParameterValue (kParamLimThreshold);
	limModeParam      = apvts.getRawParameterValue (kParamLimMode);
	invPolParam       = apvts.getRawParameterValue (kParamInvPol);
	invStrParam       = apvts.getRawParameterValue (kParamInvStr);
	mixModeParam   = apvts.getRawParameterValue (kParamMixMode);
	dryLevelParam  = apvts.getRawParameterValue (kParamDryLevel);
	wetLevelParam  = apvts.getRawParameterValue (kParamWetLevel);
	filterPosParam = apvts.getRawParameterValue (kParamFilterPos);
	alignParam   = apvts.getRawParameterValue (kParamAlign);
	pdcParam     = apvts.getRawParameterValue (kParamPdc);
	triggerParam = apvts.getRawParameterValue (kParamTrigger);
	reverseParam = apvts.getRawParameterValue (kParamReverse);

	filterHpFreqParam  = apvts.getRawParameterValue (kParamFilterHpFreq);
	filterLpFreqParam  = apvts.getRawParameterValue (kParamFilterLpFreq);
	filterHpSlopeParam = apvts.getRawParameterValue (kParamFilterHpSlope);
	filterLpSlopeParam = apvts.getRawParameterValue (kParamFilterLpSlope);
	filterHpOnParam    = apvts.getRawParameterValue (kParamFilterHpOn);
	filterLpOnParam    = apvts.getRawParameterValue (kParamFilterLpOn);
	tiltParam          = apvts.getRawParameterValue (kParamTilt);
	panParam           = apvts.getRawParameterValue (kParamPan);
	chaosParam         = apvts.getRawParameterValue (kParamChaos);
	chaosDelayParam    = apvts.getRawParameterValue (kParamChaosD);
	chaosAmtParam      = apvts.getRawParameterValue (kParamChaosAmt);
	chaosSpdParam      = apvts.getRawParameterValue (kParamChaosSpd);
	chaosAmtFilterParam = apvts.getRawParameterValue (kParamChaosAmtFilter);
	chaosSpdFilterParam = apvts.getRawParameterValue (kParamChaosSpdFilter);

	uiWidthParam   = apvts.getRawParameterValue (kParamUiWidth);
	uiHeightParam  = apvts.getRawParameterValue (kParamUiHeight);
	uiPaletteParam = apvts.getRawParameterValue (kParamUiPalette);
	uiCrtParam     = apvts.getRawParameterValue (kParamUiCrt);
	uiColorParams[0] = apvts.getRawParameterValue (kParamUiColor0);
	uiColorParams[1] = apvts.getRawParameterValue (kParamUiColor1);

	initialiseWindowFamilies (loadIntParamOrDefault (windowParam, (int) kWindowDefault));

	const int w = loadIntParamOrDefault (uiWidthParam, 360);
	const int h = loadIntParamOrDefault (uiHeightParam, 480);
	uiEditorWidth.store (w, std::memory_order_relaxed);
	uiEditorHeight.store (h, std::memory_order_relaxed);

	if constexpr (DeveloperDiagnosticsConfig::kEnableAutoDump)
	{
		perfTrace.enableDesktopAutoDump();
	}
#if STRETR_ENABLE_FFT1_CLICK_DUMP
	if constexpr (DeveloperDiagnosticsConfig::kEnableFft1AmountFreezeDump)
	{
		fft1AmountFreezeDumpTrace_.enableDesktopAutoDump ("stretr_fft_amount_click_delta_dump.csv");
	}
#endif
#if JUCE_DEBUG
	if constexpr (DeveloperDiagnosticsConfig::kEnableAutoDump)
	{
		stretchDebugTrace_.enableDesktopAutoDump();
		grainDebugTrace_.enableDesktopAutoDump();
	}
	if constexpr (DeveloperDiagnosticsConfig::kEnableFftAutoDump)
	{
		fftDebugTrace_.enableDesktopAutoDump();
	}
#endif
}

STRETRAudioProcessor::~STRETRAudioProcessor() {}

//==============================================================================
const juce::String STRETRAudioProcessor::getName() const { return JucePlugin_Name; }
bool STRETRAudioProcessor::acceptsMidi() const
{
#if JucePlugin_WantsMidiInput
	return true;
#else
	return false;
#endif
}
bool STRETRAudioProcessor::producesMidi() const
{
#if JucePlugin_ProducesMidiOutput
	return true;
#else
	return false;
#endif
}
bool STRETRAudioProcessor::isMidiEffect() const
{
#if JucePlugin_IsMidiEffect
	return true;
#else
	return false;
#endif
}
double STRETRAudioProcessor::getTailLengthSeconds() const { return 0.0; }

int STRETRAudioProcessor::getNumPrograms()  { return 1; }
int STRETRAudioProcessor::getCurrentProgram() { return 0; }
void STRETRAudioProcessor::setCurrentProgram (int) {}
const juce::String STRETRAudioProcessor::getProgramName (int) { return {}; }
void STRETRAudioProcessor::changeProgramName (int, const juce::String&) {}

//==============================================================================
void STRETRAudioProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
	juce::ignoreUnused (samplesPerBlock);
	currentSampleRate = sampleRate;

	smoothedInputGain  = 1.0f;
	smoothedOutputGain = 1.0f;
	smoothedMix        = 0.5f;
	smoothedDryLevel   = loadAtomicOrDefault (dryLevelParam, kDryLevelDefault);
	smoothedWetLevel   = loadAtomicOrDefault (wetLevelParam, kWetLevelDefault);
	smoothedLimThreshold = fastDecibelsToGain (loadAtomicOrDefault (limThresholdParam, kLimThresholdDefault));
	if (! windowFamiliesInitialised_.load (std::memory_order_relaxed))
		initialiseWindowFamilies (loadIntParamOrDefault (windowParam, (int) kWindowDefault));
	for (int i = 0; i < 4; ++i)
		smoothedWindowByFamily_[(size_t) i] = (float) getStoredWindowForFamily ((WindowFamily) i);
	const auto prepareWindowFamily = getWindowFamilyForEngineInternal (loadIntParamOrDefault (engineParam, 0));
	activeWindowFamily_.store ((int) prepareWindowFamily, std::memory_order_relaxed);
	lastObservedWindowParam_.store (loadIntParamOrDefault (windowParam, (int) kWindowDefault),
	                                std::memory_order_relaxed);
	smoothedWindow_ = smoothedWindowByFamily_[(size_t) prepareWindowFamily];
	const int initialWindowVal = (int) std::lround (smoothedWindow_);
	const float initialAmountVal = loadAtomicOrDefault (amountParam, kAmountDefault);
	const int initialEngineVal = loadIntParamOrDefault (engineParam, 0);
	const bool initialTriggerOn = loadBoolParamOrDefault (triggerParam, false);
	fft2GeometryLog2Window_ = std::log2 (juce::jlimit ((float) kWindowMin, (float) kWindowMax, smoothedWindow_));
	smoothedSpeed_     = amountToSpeedForEngine (initialEngineVal, initialAmountVal);
	smoothedPitchRate_ = std::exp2 ((loadAtomicOrDefault (pitchParam, kPitchDefault) - 0.5f) * 4.0f);
	smoothedGrainLogMs_ = std::log (juce::jlimit (kGrainMin, kGrainMax,
		loadAtomicOrDefault (grainParam, kGrainDefault)));
	jitterSmoothed_ = juce::jlimit (0.0f, 1.0f,
	                                loadAtomicOrDefault (jitterParam, kJitterDefault) * 0.01f);
	{
		const float t = 1.0f - smoothedSpeed_;
		fft2HoldCoeffSmoothed_ = std::sqrt (std::sqrt (juce::jlimit (0.0f, 1.0f, t)));
		fft2AudioHoldCoeffSmoothed_ = fft2HoldCoeffSmoothed_;
	}
	windowSmoothStep_  = 1.0f - std::exp (-1.0f / (static_cast<float> (currentSampleRate) * 0.030f));
	jitterSmoothStep_  = 1.0f - std::exp (-1.0f / (static_cast<float> (currentSampleRate) * 0.050f));
	resetJitterEngines();
	resetFftWindowDuckPrepareState (initialWindowVal, initialAmountVal, initialEngineVal, initialTriggerOn);

    // Initialize Hann LUT
	for (int i = 0; i <= kHannLutSize; ++i)
		hannLut_[i] = 0.5f * (1.0f - std::cos (juce::MathConstants<float>::twoPi
		              * (float) i / (float) kHannLutSize));

    // Initialize inverse sqrt LUT for granular normalization
	invSqrtLut_[0] = 1.0f;
	for (int i = 1; i <= kMaxGrains; ++i)
		invSqrtLut_[i] = 1.0f / std::sqrt ((float) i);

	// Initialize input buffer (power-of-2 for bitmask wrapping)
	{
		const int desired = juce::jmin (kInputBufMaxLen, (int) (sampleRate * 30.0));
		// Round up to next power of 2 (kInputBufMaxLen is already a power of 2)
		int po2 = 1;
		while (po2 < desired) po2 <<= 1;
		if (po2 > kInputBufMaxLen) po2 = kInputBufMaxLen;
		inputBufLen_ = po2;
		inputBufMask_ = po2 - 1;
	}
	for (int ch = 0; ch < 2; ++ch)
	{
		inputBuf_[ch].resize ((size_t) inputBufLen_, 0.0f);
		std::fill (inputBuf_[ch].begin(), inputBuf_[ch].end(), 0.0f);
	}
	inputBufWritePos_ = 0;
	inputBufWriteAbsPos_ = 0.0;

	// Initialize WSOLA state
	resetWsolaAtPos (0.0);
	wsolaUnityBypassActive_ = false;
	stretchBootstrapSegments_ = 0;
	stretchTransitionRemaining_ = 0;
	stretchTransitionTotal_ = 0;
	stretchTransitionToUnity_ = false;
	triggerWasOn_ = false;
	transportWasPlaying_ = false;
	transportHasSamplePos_ = false;
	transportLastSamplePos_ = 0;

    // Initialize Granular state
	for (int g = 0; g < kMaxGrains; ++g)
		grains_[g] = {};
	grainNextSlot_ = 0;
	grainSpawnCountdown_ = 0;
	grainReadPos_ = 0.0;
	grainPrevOutL_ = 0.0f;
	grainPrevOutR_ = 0.0f;
	grainUnityBypassActive_ = false;
	grainTransitionRemaining_ = 0;
	grainTransitionTotal_ = 0;
	grainTransitionToUnity_ = false;
	grainFreezeHoldActive_ = false;

    // Initialize STFT state
	std::memset (&stft_, 0, sizeof (stft_));
	currentFftOrder_ = -1;
	fft_.reset();
	std::memset (fftWork_, 0, sizeof (fftWork_));
	std::memset (fftOutputPadBuf_, 0, sizeof (fftOutputPadBuf_));
	std::memset (fftWetHistory_, 0, sizeof (fftWetHistory_));
	fftWetHistoryWritePos_ = 0;
	fftPrevWetPreWindowL_ = 0.0f;
	fftPrevWetPostWindowL_ = 0.0f;
	fftPrevWetPostOutputL_ = 0.0f;
	fftLastStableWetL_ = 0.0f;
	fftLastStableWetR_ = 0.0f;
	fftLastStableWetValid_ = false;
	fft1WindowTransitionRemaining_ = 0;
	fft1WindowTransitionTotal_ = 0;
	fftOutputPadWritePos_ = 0;
	fftUnityBypassActive_ = false;
	fft1AmountUnityBypassActive_ = false;
	fftTransitionRemaining_ = 0;
	fftTransitionTotal_ = 0;
	fftTransitionHoldSamples_ = 0;
	fftTransitionToUnity_ = false;
	fftFreezeTransitionRemaining_ = 0;
	fftFreezeTransitionTotal_ = 0;
	fftFreezeTransitionReadPos_ = 0;
	fft2WindowTransitionRemaining_ = 0;
	fft2WindowTransitionTotal_ = 0;
	fftExplicitFreezeActive_ = false;
	fftExplicitFreezeCapturePending_ = false;
	fft2AmountZeroHoldBypassActive_ = false;
	clearFft1FreezeSnapshot();
	std::memset (wetOutputHistory_, 0, sizeof (wetOutputHistory_));
	wetOutputHistoryWritePos_ = 0;
	resetEngineFadeState();
	resetFftOutputFadeState();
	std::memset (dryDelayBuf_, 0, sizeof (dryDelayBuf_));
	dryDelayWritePos_ = 0;
	dryDelayLen_ = 0;

	wetFilterState_[0].reset();
	wetFilterState_[1].reset();
	smoothedFilterHpFreq_ = loadAtomicOrDefault (filterHpFreqParam, kFilterHpFreqDefault);
	smoothedFilterLpFreq_ = loadAtomicOrDefault (filterLpFreqParam, kFilterLpFreqDefault);
	lastCalcHpFreq_ = -1.0f; lastCalcLpFreq_ = -1.0f;
	lastCalcHpSlope_ = -1;   lastCalcLpSlope_ = -1;
	filterCoeffCountdown_ = 0;
	updateFilterCoeffs (true, true);

	tiltDb_ = 0.0f;
	tiltB0_ = 1.0f; tiltB1_ = 0.0f; tiltA1_ = 0.0f;
	tiltTargetB0_ = 1.0f; tiltTargetB1_ = 0.0f; tiltTargetA1_ = 0.0f;
	tiltState_[0] = tiltState_[1] = 0.0f;
	lastTiltDb_ = 0.0f;
	tiltSmoothSc_ = 1.0f - std::exp (-1.0f / (static_cast<float> (currentSampleRate) * 0.03f));

	chaosFilterEnabled_ = false;
	chaosDelayEnabled_  = false;
	chaosStereo_ = false;
	chaosAmtD_ = 0.0f; chaosAmtNormD_ = 0.0f; chaosAmtF_ = 0.0f;
	chaosParamSmoothCoeff_ = std::exp (-1.0f / (static_cast<float> (currentSampleRate) * 0.010f));
	cachedChaosParamSmoothCoeff_ = chaosParamSmoothCoeff_;
	chaosShPeriodD_ = 8820.0f; smoothedChaosShPeriodD_ = 8820.0f;
	chaosShPeriodF_ = 8820.0f; smoothedChaosShPeriodF_ = 8820.0f;
	chaosDelayMaxSamples_ = 0.0f; smoothedChaosDelayMaxSamples_ = 0.0f;
	chaosGainMaxDb_ = 0.0f; smoothedChaosGainMaxDb_ = 0.0f;
	chaosFilterMaxOct_ = 0.0f; smoothedChaosFilterMaxOct_ = 0.0f;
	chaosDelaySmoothStep_ = 1.0f - std::exp (-1.0f / (static_cast<float> (currentSampleRate) * 0.002f));
	chaosDriveAmtSmoothed_ = 0.0f;
	chaosDriveSpdSmoothed_ = kChaosSpdDefault;
	chaosDriveParamSmoothReady_ = false;
	chaosFilterAmtSmoothed_ = 0.0f;
	chaosFilterSpdSmoothed_ = kChaosSpdDefault;
	chaosFilterParamSmoothReady_ = false;
	for (int c = 0; c < 2; ++c)
	{
		chaosDelaySmoothedSamples_[c] = 0.0f;
		chaosDelaySmoothReady_[c] = false;
		chaosDPrev_[c] = chaosDCurr_[c] = chaosDNext_[c] = 0.0f;
		chaosDPhase_[c] = 0.0f; chaosDDriftPhase_[c] = 0.0f; chaosDDriftFreqHz_[c] = 0.0f; chaosDOut_[c] = 0.0f;
		chaosGPrev_[c] = chaosGCurr_[c] = chaosGNext_[c] = 0.0f;
		chaosGPhase_[c] = 0.0f; chaosGDriftPhase_[c] = 0.0f; chaosGDriftFreqHz_[c] = 0.0f; chaosGOut_[c] = 0.0f;
	}
	chaosFPrev_ = chaosFCurr_ = chaosFNext_ = 0.0f;
	chaosFPhase_ = 0.0f; chaosFDriftPhase_ = 0.0f; chaosFDriftFreqHz_ = 0.0f;
	chaosFOut_[0] = chaosFOut_[1] = 0.0f;
	std::memset (chaosDelayBuf_, 0, sizeof (chaosDelayBuf_));
	chaosDelayWritePos_ = 0;

	lastPan_ = 0.5f;
	lastPanLeft_  = 0.70710678f;
	lastPanRight_ = 0.70710678f;

	// Limiter state reset
	limEnv1_[0] = limEnv1_[1] = kLimFloor;
	limEnv2_[0] = limEnv2_[1] = kLimFloor;
	{
		const float sr = static_cast<float> (currentSampleRate);
		limAtt1_ = std::exp (-1.0f / (sr * 0.002f));   // 2 ms attack
		limRel1_ = std::exp (-1.0f / (sr * 0.010f));   // 10 ms release
		limRel2_ = std::exp (-1.0f / (sr * 0.100f));   // 100 ms release
	}

	// Engine crossfade
	prevEngineVal_ = -1;
	engineFadePos_ = 0;
	lastReportedLatency_ = -1;

	// DC blocker
	dcBlockR_ = 1.0f - (juce::MathConstants<float>::twoPi * 5.0f / (float) sampleRate);
	dcBlockPrevIn_[0] = dcBlockPrevIn_[1] = 0.0f;
	dcBlockPrevOut_[0] = dcBlockPrevOut_[1] = 0.0f;
}

void STRETRAudioProcessor::releaseResources()
{
	for (int ch = 0; ch < 2; ++ch)
		inputBuf_[ch].clear();
	inputBufLen_ = 0;
	transportWasPlaying_ = false;
	transportHasSamplePos_ = false;
	transportLastSamplePos_ = 0;
}

void STRETRAudioProcessor::resetJitterEngines() noexcept
{
	auto randomBipolar = [] (juce::Random& rng) noexcept
	{
		return rng.nextFloat() * 2.0f - 1.0f;
	};

	auto initEngine = [&] (JitterEngine& engine, juce::int64 seed, float rateA, float rateB) noexcept
	{
		engine.rng = juce::Random (seed);
		engine.driftPhaseA = engine.rng.nextFloat();
		engine.driftPhaseB = engine.rng.nextFloat();
		engine.driftRateHzA = rateA * (0.85f + engine.rng.nextFloat() * 0.30f);
		engine.driftRateHzB = rateB * (0.85f + engine.rng.nextFloat() * 0.30f);
		engine.shCurr = randomBipolar (engine.rng);
		engine.shNext = randomBipolar (engine.rng);
		engine.shPhase = engine.rng.nextFloat();
	};

	for (int ch = 0; ch < 2; ++ch)
	{
		const juce::int64 baseSeed = (ch == 0) ? 0x5354524a49543031ll : 0x5354524a49543032ll;
		initEngine (jitterWindow_[ch], baseSeed + 0x17ll, 0.061f, 0.097f);
		initEngine (jitterAnchor_[ch], baseSeed + 0x31ll, 0.083f, 0.149f);
		initEngine (jitterPitch_[ch],  baseSeed + 0x4dll, 0.113f, 0.181f);
		initEngine (jitterRapid_[ch],  baseSeed + 0x6bll, 0.293f, 0.557f);

		jitterWindowOut_[ch] = 0.0f;
		jitterAnchorOut_[ch] = 0.0f;
		jitterPitchOut_[ch] = 0.0f;
		jitterRapidOut_[ch] = 0.0f;
		stretchJitterPitchScaleSmoothed_[ch] = 1.0f;
	}
}

float STRETRAudioProcessor::advanceJitterEngine (JitterEngine& engine, float fastRateHz, float fastBlend,
                                                 float maxFastRateHz, float maxBlend) noexcept
{
	const float sr = juce::jmax (1.0f, (float) currentSampleRate);
	auto wrapPhase = [] (float phase) noexcept
	{
		return phase >= 1.0f ? phase - std::floor (phase) : phase;
	};
	auto smootherStep = [] (float t) noexcept
	{
		t = juce::jlimit (0.0f, 1.0f, t);
		return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f);
	};

	engine.driftPhaseA = wrapPhase (engine.driftPhaseA + engine.driftRateHzA / sr);
	engine.driftPhaseB = wrapPhase (engine.driftPhaseB + engine.driftRateHzB / sr);
	const float slow = std::sin (engine.driftPhaseA * kTwoPi) * 0.68f
	                 + std::sin (engine.driftPhaseB * kTwoPi) * 0.32f;

	const float safeFastRateHz = juce::jlimit (0.1f, juce::jmax (0.1f, maxFastRateHz), fastRateHz);
	engine.shPhase += safeFastRateHz / sr;
	if (engine.shPhase >= 1.0f)
	{
		engine.shPhase -= std::floor (engine.shPhase);
		engine.shCurr = engine.shNext;
		engine.shNext = engine.rng.nextFloat() * 2.0f - 1.0f;
	}

	const float sh = engine.shCurr + (engine.shNext - engine.shCurr) * smootherStep (engine.shPhase);
	const float blend = juce::jlimit (0.0f, juce::jmax (0.0f, maxBlend), fastBlend);
	return juce::jlimit (-1.0f, 1.0f, slow * (1.0f - blend) + sh * blend);
}

void STRETRAudioProcessor::advanceJitterEngines (float amount) noexcept
{
	const float amt = juce::jlimit (0.0f, 1.0f, amount);
	const float high = juce::jlimit (0.0f, 1.0f, (amt - 0.55f) / 0.45f);
	const float fastBlend = high * high * (3.0f - 2.0f * high) * 0.35f;
	const float fastBaseHz = 2.0f + 16.0f * amt * amt;
	const float finalRange = juce::jlimit (0.0f, 1.0f, (amt - 0.80f) / 0.20f);
	const float finalShape = finalRange * finalRange * (3.0f - 2.0f * finalRange);
	const float rapidRange = juce::jlimit (0.0f, 1.0f, (amt - 0.25f) / 0.75f);
	const float rapidShape = rapidRange * rapidRange * (3.0f - 2.0f * rapidRange);
	const float rapidBlend = juce::jmax (finalShape * 0.80f, rapidShape * 0.70f);

	for (int ch = 0; ch < 2; ++ch)
	{
		jitterWindowOut_[ch] = advanceJitterEngine (jitterWindow_[ch], fastBaseHz * 0.79f, fastBlend);
		jitterAnchorOut_[ch] = advanceJitterEngine (jitterAnchor_[ch], fastBaseHz * 1.13f, fastBlend);
		jitterPitchOut_[ch]  = advanceJitterEngine (jitterPitch_[ch],  fastBaseHz * 1.37f, fastBlend);
		jitterRapidOut_[ch]  = advanceJitterEngine (jitterRapid_[ch],
			fastBaseHz * 12.80f + 44.0f * rapidShape, rapidBlend, 220.0f, 0.80f);
	}
}

STRETRAudioProcessor::JitterRuntimeValues STRETRAudioProcessor::makeJitterRuntimeValues (int lane,
                                                                                         float referenceSamples,
                                                                                         float pitchAmountScale,
                                                                                         float motionAmountScale,
                                                                                         bool allowAnchor) const noexcept
{
	JitterRuntimeValues values;
	const float pitchAmt = juce::jlimit (0.0f, 1.0f, jitterSmoothed_ * pitchAmountScale);
	const float motionAmt = juce::jlimit (0.0f, 1.0f, jitterSmoothed_ * motionAmountScale);
	if (pitchAmt <= 1.0e-5f && motionAmt <= 1.0e-5f)
		return values;

	const int ch = juce::jlimit (0, 1, lane);
	const float rapid = jitterRapidOut_[ch];

	if (pitchAmt > 1.0e-5f)
	{
		const float pitchDepth = pitchAmt * pitchAmt;
		const float pitchFinalRange = juce::jlimit (0.0f, 1.0f, (pitchAmt - 0.80f) / 0.20f);
		const float pitchFinalShape = pitchFinalRange * pitchFinalRange * (3.0f - 2.0f * pitchFinalRange);
		const float pitchDepthCents = 1.5f * pitchAmt + 7.5f * pitchDepth;
		const float pitchCents = juce::jlimit (-12.0f, 12.0f,
			jitterPitchOut_[ch] * pitchDepthCents + rapid * pitchFinalShape * 2.5f);
		values.pitchScale = std::exp2 (pitchCents / 1200.0f);
	}

	if (allowAnchor && motionAmt > 1.0e-5f)
	{
		const float motionDepth = motionAmt * motionAmt;
		const float motionFinalRange = juce::jlimit (0.0f, 1.0f, (motionAmt - 0.80f) / 0.20f);
		const float motionFinalShape = motionFinalRange * motionFinalRange * (3.0f - 2.0f * motionFinalRange);
		const float lengthDepth = (0.004f * motionAmt + 0.018f * motionDepth) * (1.0f + 0.55f * motionFinalShape);
		const float lengthModulator = juce::jlimit (-1.0f, 1.0f,
			jitterWindowOut_[ch] + rapid * motionFinalShape * 0.45f);
		values.lengthScale = juce::jlimit (0.88f, 1.12f, 1.0f + lengthModulator * lengthDepth);

		const float ref = juce::jmax (1.0f, referenceSamples);
		const float anchorDepth = (0.003f * motionAmt + 0.012f * motionDepth) * (1.0f + 0.60f * motionFinalShape);
		const float anchorLimit = juce::jmin (ref * anchorDepth,
		                                      (float) currentSampleRate * (0.010f + 0.010f * motionFinalShape));
		const float anchorModulator = juce::jlimit (-1.0f, 1.0f,
			jitterAnchorOut_[ch] + jitterWindowOut_[ch] * 0.35f + rapid * motionFinalShape * 0.75f);
		values.anchorOffsetSamples = (double) anchorModulator * (double) anchorLimit;
	}

	return values;
}

STRETRAudioProcessor::JitterRuntimeValues STRETRAudioProcessor::makeStretchJitterRuntimeValues (int lane) const noexcept
{
	JitterRuntimeValues values;
	const float amt = juce::jlimit (0.0f, 1.0f, jitterSmoothed_);
	if (amt <= 1.0e-5f)
		return values;

	const int ch = juce::jlimit (0, 1, lane);
	const float curveAmt = std::sqrt (amt);
	const float depth = curveAmt * curveAmt;
	const float finalRange = juce::jlimit (0.0f, 1.0f, (amt - 0.80f) / 0.20f);
	const float finalShape = finalRange * finalRange * (3.0f - 2.0f * finalRange);
	const float fastRange = juce::jlimit (0.0f, 1.0f, (curveAmt - 0.16f) / 0.84f);
	const float fastShape = fastRange * fastRange * (3.0f - 2.0f * fastRange);
	const float rapidWeight = 0.32f + 0.63f * fastShape;
	const float modulator = juce::jlimit (-1.0f, 1.0f,
		jitterPitchOut_[ch] * (1.0f - rapidWeight) + jitterRapidOut_[ch] * rapidWeight);

	const float pitchDepthCents = (8.0f * curveAmt + 105.0f * depth) * (1.0f + 0.35f * finalShape);
	const float pitchCents = juce::jlimit (-180.0f, 180.0f, modulator * pitchDepthCents);
	values.pitchScale = std::exp2 (pitchCents / 1200.0f);
	return values;
}

STRETRAudioProcessor::JitterRuntimeValues STRETRAudioProcessor::makeFftJitterRuntimeValues (int lane) const noexcept
{
	JitterRuntimeValues values;
	const float amt = juce::jlimit (0.0f, 1.0f, jitterSmoothed_);
	if (amt <= 1.0e-5f)
		return values;

	const int ch = juce::jlimit (0, 1, lane);
	const float depth = amt * amt;
	const float finalRange = juce::jlimit (0.0f, 1.0f, (amt - 0.80f) / 0.20f);
	const float finalShape = finalRange * finalRange * (3.0f - 2.0f * finalRange);
	const float rapid = jitterRapidOut_[ch];

	const float fastRange = juce::jlimit (0.0f, 1.0f, (amt - 0.35f) / 0.65f);
	const float fastShape = fastRange * fastRange * (3.0f - 2.0f * fastRange);
	const float rapidWeight = 0.20f + 0.75f * fastShape;
	const float modulator = juce::jlimit (-1.0f, 1.0f,
		jitterPitchOut_[ch] * (1.0f - rapidWeight) + rapid * rapidWeight);
	const float modDepthNorm = (0.006f * amt + 0.044f * depth) * (1.0f + 0.30f * finalShape);
	const float modOffsetNorm = juce::jlimit (-0.14f, 0.14f,
		modulator * modDepthNorm);
	values.pitchScale = std::exp2 (modOffsetNorm * 4.0f);

	return values;
}

#ifndef JucePlugin_PreferredChannelConfigurations
bool STRETRAudioProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
#if JucePlugin_IsMidiEffect
	juce::ignoreUnused (layouts);
	return true;
#else
	if (layouts.getMainOutputChannelSet() != juce::AudioChannelSet::mono()
	 && layouts.getMainOutputChannelSet() != juce::AudioChannelSet::stereo())
		return false;
#if ! JucePlugin_IsSynth
	if (layouts.getMainOutputChannelSet() != layouts.getMainInputChannelSet())
		return false;
#endif
	return true;
#endif
}
#endif

//==============================================================================
// Wet-signal filter coefficient update

void STRETRAudioProcessor::updateFilterCoeffs (bool forceHp, bool forceLp)
{
	const float sr = (float) currentSampleRate;
	const int hpSlope = juce::roundToInt (loadAtomicOrDefault (filterHpSlopeParam, (float) kFilterSlopeDefault));
	const int lpSlope = juce::roundToInt (loadAtomicOrDefault (filterLpSlopeParam, (float) kFilterSlopeDefault));

	if (forceHp || hpSlope != lastCalcHpSlope_ || std::abs (smoothedFilterHpFreq_ - lastCalcHpFreq_) > 0.01f)
	{
		lastCalcHpFreq_ = smoothedFilterHpFreq_;
		lastCalcHpSlope_ = hpSlope;
		if (hpSlope == 0)      { hpCoeffs_[0] = calcOnePoleHP (smoothedFilterHpFreq_, sr); hpCoeffs_[1] = {}; }
		else if (hpSlope == 1) { hpCoeffs_[0] = calcBiquadHP  (smoothedFilterHpFreq_, sr, kBW2_Q);  hpCoeffs_[1] = {}; }
		else                   { hpCoeffs_[0] = calcBiquadHP  (smoothedFilterHpFreq_, sr, kBW4_Q1); hpCoeffs_[1] = calcBiquadHP (smoothedFilterHpFreq_, sr, kBW4_Q2); }
	}

	if (forceLp || lpSlope != lastCalcLpSlope_ || std::abs (smoothedFilterLpFreq_ - lastCalcLpFreq_) > 0.01f)
	{
		lastCalcLpFreq_ = smoothedFilterLpFreq_;
		lastCalcLpSlope_ = lpSlope;
		if (lpSlope == 0)      { lpCoeffs_[0] = calcOnePoleLP (smoothedFilterLpFreq_, sr); lpCoeffs_[1] = {}; }
		else if (lpSlope == 1) { lpCoeffs_[0] = calcBiquadLP  (smoothedFilterLpFreq_, sr, kBW2_Q);  lpCoeffs_[1] = {}; }
		else                   { lpCoeffs_[0] = calcBiquadLP  (smoothedFilterLpFreq_, sr, kBW4_Q1); lpCoeffs_[1] = calcBiquadLP (smoothedFilterLpFreq_, sr, kBW4_Q2); }
	}
}

void STRETRAudioProcessor::filterWetSample (float& wetL, float& wetR)
{
	float hpTarget = wetFilterTargetHpFreq_;
	float lpTarget = wetFilterTargetLpFreq_;

	// EMA frequency smoothing (base, no chaos)
	smoothedFilterHpFreq_ += (hpTarget - smoothedFilterHpFreq_) * kGainSmoothStep;
	smoothedFilterLpFreq_ += (lpTarget - smoothedFilterLpFreq_) * kGainSmoothStep;

	// Batched coefficient update (with per-channel chaos overlay)
	if (--filterCoeffCountdown_ <= 0)
	{
		filterCoeffCountdown_ = kFilterCoeffUpdateInterval;
		const bool chaosFilterActive = chaosFilterEnabled_
			&& (chaosAmtF_ > 0.01f || (chaosFilterParamSmoothReady_ && chaosFilterAmtSmoothed_ > 0.01f));
		if (chaosFilterActive)
		{
			const float sHp = smoothedFilterHpFreq_;
			const float sLp = smoothedFilterLpFreq_;

			// L channel coefficients
			const float octL = chaosFOut_[0] * smoothedChaosFilterMaxOct_;
			const float freqMultL = std::exp2 (octL);
			const float hpBaseL = wetFilterHpOn_ ? sHp : kFilterFreqMin;
			const float lpBaseL = wetFilterLpOn_ ? sLp : kFilterFreqMax;
			smoothedFilterHpFreq_ = juce::jlimit (kFilterFreqMin, kFilterFreqMax, hpBaseL * freqMultL);
			smoothedFilterLpFreq_ = juce::jlimit (kFilterFreqMin, kFilterFreqMax, lpBaseL * freqMultL);
			updateFilterCoeffs (true, true);

			if (chaosStereo_)
			{
				auto hpL0 = hpCoeffs_[0]; auto hpL1 = hpCoeffs_[1];
				auto lpL0 = lpCoeffs_[0]; auto lpL1 = lpCoeffs_[1];

				const float octR = chaosFOut_[1] * smoothedChaosFilterMaxOct_;
				const float freqMultR = std::exp2 (octR);
				smoothedFilterHpFreq_ = juce::jlimit (kFilterFreqMin, kFilterFreqMax, hpBaseL * freqMultR);
				smoothedFilterLpFreq_ = juce::jlimit (kFilterFreqMin, kFilterFreqMax, lpBaseL * freqMultR);
				updateFilterCoeffs (true, true);

				hpCoeffsR_[0] = hpCoeffs_[0]; hpCoeffsR_[1] = hpCoeffs_[1];
				lpCoeffsR_[0] = lpCoeffs_[0]; lpCoeffsR_[1] = lpCoeffs_[1];
				hpCoeffs_[0] = hpL0; hpCoeffs_[1] = hpL1;
				lpCoeffs_[0] = lpL0; lpCoeffs_[1] = lpL1;
			}
			else
			{
				hpCoeffsR_[0] = hpCoeffs_[0]; hpCoeffsR_[1] = hpCoeffs_[1];
				lpCoeffsR_[0] = lpCoeffs_[0]; lpCoeffsR_[1] = lpCoeffs_[1];
			}

			smoothedFilterHpFreq_ = sHp;
			smoothedFilterLpFreq_ = sLp;
		}
		else
		{
			updateFilterCoeffs (false, false);
			hpCoeffsR_[0] = hpCoeffs_[0]; hpCoeffsR_[1] = hpCoeffs_[1];
			lpCoeffsR_[0] = lpCoeffs_[0]; lpCoeffsR_[1] = lpCoeffs_[1];
		}
	}

	const bool chaosFilterActive = chaosFilterEnabled_
		&& (chaosAmtF_ > 0.01f || (chaosFilterParamSmoothReady_ && chaosFilterAmtSmoothed_ > 0.01f));
	if (wetFilterHpOn_ || chaosFilterActive)
	{
		for (int s = 0; s < wetFilterNumSectionsHp_; ++s)
		{
			wetL = processBiquad (wetL, hpCoeffs_[s], wetFilterState_[0].hp[s]);
			wetR = processBiquad (wetR, hpCoeffsR_[s], wetFilterState_[1].hp[s]);
		}
	}

	if (wetFilterLpOn_ || chaosFilterActive)
	{
		for (int s = 0; s < wetFilterNumSectionsLp_; ++s)
		{
			wetL = processBiquad (wetL, lpCoeffs_[s], wetFilterState_[0].lp[s]);
			wetR = processBiquad (wetR, lpCoeffsR_[s], wetFilterState_[1].lp[s]);
		}
	}

    // TILT filter - handled by tiltWetSample()
}

void STRETRAudioProcessor::tiltWetSample (float& wetL, float& wetR)
{
	if (std::abs (tiltDb_) > 0.05f)
	{
		if (std::abs (tiltDb_ - lastTiltDb_) > 0.02f)
		{
			lastTiltDb_ = tiltDb_;
			const double pivot = 1000.0;
			const double octToNy = std::log2 ((currentSampleRate * 0.5) / pivot);
			const double gainNyDb = static_cast<double> (tiltDb_) * octToNy;
			const double gNy = std::pow (10.0, gainNyDb / 20.0);
			const double wc = 2.0 * currentSampleRate
			                * std::tan (juce::MathConstants<double>::pi * pivot / currentSampleRate);
			const double K = wc / (2.0 * currentSampleRate);
			const double g = std::sqrt (gNy);
			const double norm = 1.0 / (1.0 + K * g);
			tiltTargetB0_ = static_cast<float> ((g + K) * norm);
			tiltTargetB1_ = static_cast<float> ((K - g) * norm);
			tiltTargetA1_ = static_cast<float> ((K * g - 1.0) * norm);
		}

		const float sc = tiltSmoothSc_;
		tiltB0_ += (tiltTargetB0_ - tiltB0_) * sc;
		tiltB1_ += (tiltTargetB1_ - tiltB1_) * sc;
		tiltA1_ += (tiltTargetA1_ - tiltA1_) * sc;

		{ const float x = wetL; const float y = tiltB0_ * x + tiltState_[0]; tiltState_[0] = tiltB1_ * x - tiltA1_ * y; wetL = y; }
		{ const float x = wetR; const float y = tiltB0_ * x + tiltState_[1]; tiltState_[1] = tiltB1_ * x - tiltA1_ * y; wetR = y; }
	}
	else if (std::abs (lastTiltDb_) > 0.05f)
	{
		lastTiltDb_ = 0.0f;
		tiltB0_ = 1.0f; tiltB1_ = 0.0f; tiltA1_ = 0.0f;
		tiltTargetB0_ = 1.0f; tiltTargetB1_ = 0.0f; tiltTargetA1_ = 0.0f;
		tiltState_[0] = tiltState_[1] = 0.0f;
	}
}

//==============================================================================
// WSOLA helpers

namespace
{
static inline float wsolaFadeInWeight (int idx, int overlapLen) noexcept
{
	if (overlapLen <= 0)
		return 1.0f;

	const float phase = ((float) idx + 0.5f) / (float) overlapLen;
	const float s = std::sin (phase * juce::MathConstants<float>::halfPi);
	return s * s;
}

static inline float wsolaFadeOutWeight (int idx, int overlapLen) noexcept
{
	if (overlapLen <= 0)
		return 1.0f;

	const float phase = ((float) idx + 0.5f) / (float) overlapLen;
	const float c = std::cos (phase * juce::MathConstants<float>::halfPi);
	return c * c;
}

static inline float wsolaSegmentWeight (int sampleIndex, int segLen, int overlapLen,
                                        bool hasPrevSegment) noexcept
{
	if (segLen <= 0 || overlapLen <= 0)
		return 1.0f;

	const int tailStart = juce::jmax (0, segLen - overlapLen);
	float weight = 1.0f;

	if (hasPrevSegment && sampleIndex < overlapLen)
		weight *= wsolaFadeInWeight (sampleIndex, overlapLen);

	if (sampleIndex >= tailStart)
		weight *= wsolaFadeOutWeight (sampleIndex - tailStart, overlapLen);

	return weight;
}

static inline int wsolaRecommendedOverlapLen (int segLen, double sampleRate) noexcept
{
	const int timeCap = juce::jlimit (32, 256, (int) std::round (sampleRate * 0.005)); // ~5 ms
	const int ratioTarget = juce::jmax (16, segLen / 4);
	return juce::jlimit (16, juce::jmax (16, segLen - 1), juce::jmin (ratioTarget, timeCap));
}
}

void STRETRAudioProcessor::resetWsolaAtPos (double capturePos) noexcept
{
	wsola_ = {};
	wsola_.segInputStart = capturePos;
	wsola_.segInputStartR = capturePos;
	wsola_.samplesUntilNextSeg = 0;
	wsola_.outputReadPos = 0;
	wsola_.nextSynthPos = 0;
	wsola_.lastBestOffset = 0;
	wsola_.lastBestOffsetR = 0;
	wsola_.lastAnalysisHop = 0.0;
	wsola_.hasPrevTail = false;
}

double STRETRAudioProcessor::currentCaptureAbsPos() const noexcept
{
	return inputBufWriteAbsPos_ - 1.0;
}

double STRETRAudioProcessor::computeGrainLookBehind (int grainSamples, float pitchRate,
                                                     bool reverseOn, bool wideMode) const noexcept
{
	const double baseSpan = reverseOn
		? (double) juce::jmax (0, grainSamples - 1)
		: (double) juce::jmax (0.0f, pitchRate) * (double) juce::jmax (0, grainSamples - 1);
	const double wideOffset = wideMode ? (double) grainSamples * 0.5 : 0.0;
	return juce::jmax (4.0, baseSpan + wideOffset + 2.0);
}

double STRETRAudioProcessor::clampGrainSpawnPos (double desiredPos, double capturePos,
                                                 double lookBehind) const noexcept
{
	if (inputBufLen_ <= 8)
		return desiredPos;

	const double maxPos = capturePos - lookBehind;
	const double minPos = capturePos - (double) (inputBufLen_ - 4);
	return juce::jlimit (minPos, maxPos, desiredPos);
}

void STRETRAudioProcessor::resetGrainAtCapturePos (double capturePos, int grainSamples, float pitchRate,
                                                   bool reverseOn, bool wideMode) noexcept
{
	for (int g = 0; g < kMaxGrains; ++g)
		grains_[g].active = false;

	grainNextSlot_ = 0;
	grainSpawnCountdown_ = 0;
	grainPrevOutL_ = 0.0f;
	grainPrevOutR_ = 0.0f;
	grainFreezeHoldActive_ = false;

	const double lookBehind = computeGrainLookBehind (grainSamples, pitchRate, reverseOn, wideMode);
	grainReadPos_ = clampGrainSpawnPos (capturePos - lookBehind, capturePos, lookBehind);
}

int STRETRAudioProcessor::countActiveGrains() const noexcept
{
	int active = 0;
	for (int g = 0; g < kMaxGrains; ++g)
		if (grains_[g].active)
			++active;
	return active;
}

STRETRAudioProcessor::WsolaMatchResult
STRETRAudioProcessor::wsolaBestOverlapOffset (int channel, double nominalPos, int overlapLen,
                                              int prevBestOffset, bool nearUnity,
                                              float readRate, int synthPos) const
{
	WsolaMatchResult result {};
	if (overlapLen <= 0 || inputBufLen_ <= 0 || ! wsola_.hasPrevTail)
		return result;

	const int len = inputBufLen_;
	const int outMask = kWsolaOutBufLen - 1;
	const int nomWrapped = ((int) std::floor (nominalPos) % len + len) % len;
	const int seekCap = juce::jlimit (24, 128, (int) std::round (currentSampleRate * 0.0025)); // ~2.5 ms
	const int baseSeek = juce::jmin (juce::jmax (8, overlapLen / 3), len / 4);
	const int seekRadius = nearUnity
		? juce::jmin (baseSeek, juce::jmax (8, seekCap / 2))
		: juce::jmin (baseSeek, seekCap);
	const int step = (overlapLen >= 2048) ? 8
		: (overlapLen >= 1024) ? 4
		: (overlapLen >= 256)  ? 2
		: 1;
	const int continuityAnchor = juce::jlimit (-seekRadius, seekRadius, prevBestOffset);
	const int minOff = -seekRadius;
	const int maxOff = seekRadius;

	float refEnergy = 1.0e-12f;
	for (int j = 0; j < overlapLen; j += step)
	{
		const float refL = (channel == 0)
			? wsola_.outputAccumL[(synthPos + j) & outMask]
			: wsola_.outputAccumR[(synthPos + j) & outMask];
		refEnergy += refL * refL;
	}

	float bestScore = -1.0e30f;
	int bestOffset = 0;
	float bestNormCorr = 0.0f;
	float bestCenterPenalty = 0.0f;
	float bestDriftPenalty = 0.0f;
	float bestStartDeltaL = 0.0f;
	float bestStartDeltaR = 0.0f;
	float bestRmseL = 0.0f;
	float bestRmseR = 0.0f;

	for (int off = minOff; off <= maxOff; ++off)
	{
		float dot = 0.0f;
		float candEnergy = 1.0e-12f;
		float diffEnergyL = 0.0f;
		double candPos = (double) nomWrapped + (double) off;
		float startDeltaL = 0.0f;
		float startDeltaR = 0.0f;

		for (int j = 0; j < overlapLen; j += step)
		{
			const float candL = readInputBuf (channel, candPos);
			const float refL = (channel == 0)
				? wsola_.outputAccumL[(synthPos + j) & outMask]
				: wsola_.outputAccumR[(synthPos + j) & outMask];
			dot += candL * refL;
			candEnergy += candL * candL;
			const float diffL = candL - refL;
			diffEnergyL += diffL * diffL;
			if (j == 0)
				startDeltaL = diffL;
			candPos += (double) readRate * (double) step;
		}

		const float normCorr = dot / std::sqrt (candEnergy * refEnergy);
		const float centerPenalty = (seekRadius > 0)
			? (nearUnity ? 0.030f : 0.012f) * (float) std::abs (off) / (float) seekRadius
			: 0.0f;
		const float driftPenalty = (seekRadius > 0)
			? (nearUnity ? 0.018f : 0.006f) * (float) std::abs (off - continuityAnchor) / (float) seekRadius
			: 0.0f;
		const float score = normCorr - centerPenalty - driftPenalty;
		if (score > bestScore)
		{
			bestScore = score;
			bestOffset = off;
			bestNormCorr = normCorr;
			bestCenterPenalty = centerPenalty;
			bestDriftPenalty = driftPenalty;
			bestStartDeltaL = startDeltaL;
			bestStartDeltaR = startDeltaR;
			bestRmseL = std::sqrt (diffEnergyL / (float) juce::jmax (1, (overlapLen + step - 1) / step));
			bestRmseR = bestRmseL;
		}
	}

	result.bestOffset = bestOffset;
	result.bestScore = bestScore;
	result.bestNormCorr = bestNormCorr;
	result.centerPenalty = bestCenterPenalty;
	result.driftPenalty = bestDriftPenalty;
	result.startDeltaL = bestStartDeltaL;
	result.startDeltaR = bestStartDeltaR;
	result.overlapRmseL = bestRmseL;
	result.overlapRmseR = bestRmseR;
	return result;
}

//==============================================================================
// FFT / Phase Vocoder helpers

void STRETRAudioProcessor::ensureFft (int fftSize)
{
	const int order = (int) std::round (std::log2 ((double) fftSize));
	if (order != currentFftOrder_)
	{
		currentFftOrder_ = order;
		fft_ = std::make_unique<juce::dsp::FFT> (order);

		for (int j = 0; j < fftSize; ++j)
			fftWindow_[j] = 0.5f * (1.0f - std::cos (juce::MathConstants<float>::twoPi
			                * (float) j / (float) fftSize));
	}

	stft_.activeFftSize = fftSize;
}

void STRETRAudioProcessor::resetStftAtPos (double capturePos, int fftSize) noexcept
{
	StftState fresh {};
	fresh.activeFftSize = fftSize;

	if (inputBufLen_ > 0 && fftSize > 0)
	{
		double startPos = capturePos - (double) fftSize + 1.0;
		while (startPos >= (double) inputBufLen_)
			startPos -= (double) inputBufLen_;
		while (startPos < 0.0)
			startPos += (double) inputBufLen_;
		fresh.analysisReadPos = startPos;
	}

	stft_ = fresh;
	fftLastStableWetL_ = 0.0f;
	fftLastStableWetR_ = 0.0f;
	fftLastStableWetValid_ = false;
}

void STRETRAudioProcessor::resizeStftAtPos (double capturePos, int fftSize) noexcept
{
	if (fftSize <= 0)
		return;

	stftResizeScratch_ = stft_;
	const StftState& previous = stftResizeScratch_;
	if (previous.activeFftSize <= 0 || ! previous.hasFrame)
	{
		resetStftAtPos (capturePos, fftSize);
		return;
	}

	stft_ = {};
	StftState& fresh = stft_;
	fresh.activeFftSize = fftSize;

	if (inputBufLen_ > 0)
	{
		double startPos = capturePos - (double) fftSize + 1.0;
		while (startPos >= (double) inputBufLen_)
			startPos -= (double) inputBufLen_;
		while (startPos < 0.0)
			startPos += (double) inputBufLen_;
		fresh.analysisReadPos = startPos;
	}

	const int oldFftSize = juce::jmax (1, previous.activeFftSize);
	const int oldBins = oldFftSize / 2 + 1;
	const int newBins = fftSize / 2 + 1;
	const int oldHop = juce::jmax (1, recommendedFftSynthHop (oldFftSize));
	const int newHop = juce::jmax (1, recommendedFftSynthHop (fftSize));

	auto sampleLinear = [] (const float* src, int size, float index) noexcept
	{
		if (size <= 0)
			return 0.0f;
		if (index <= 0.0f)
			return src[0];
		const float maxIndex = (float) (size - 1);
		if (index >= maxIndex)
			return src[size - 1];
		const int i0 = (int) index;
		const int i1 = juce::jmin (size - 1, i0 + 1);
		const float frac = index - (float) i0;
		return src[i0] + (src[i1] - src[i0]) * frac;
	};

	auto samplePhase = [&sampleLinear] (const float* src, int size, float index) noexcept
	{
		if (size <= 0)
			return 0.0f;
		if (index <= 0.0f)
			return src[0];
		const float maxIndex = (float) (size - 1);
		if (index >= maxIndex)
			return src[size - 1];
		const int i0 = (int) index;
		const int i1 = juce::jmin (size - 1, i0 + 1);
		const float frac = index - (float) i0;
		const float a0 = src[i0];
		const float a1 = src[i1];
		const float x = std::cos (a0) * (1.0f - frac) + std::cos (a1) * frac;
		const float y = std::sin (a0) * (1.0f - frac) + std::sin (a1) * frac;
		return std::atan2 (y, x);
	};

	for (int ch = 0; ch < 2; ++ch)
	{
		for (int k = 0; k < newBins; ++k)
		{
			const float srcIndex = ((float) k * (float) oldFftSize) / (float) fftSize;
			fresh.prevMag[ch][k]   = sampleLinear (previous.prevMag[ch], oldBins, srcIndex);
			fresh.lastMag[ch][k]   = sampleLinear (previous.lastMag[ch], oldBins, srcIndex);
			fresh.lastFreq[ch][k]  = sampleLinear (previous.lastFreq[ch], oldBins, srcIndex);
			fresh.heldMag[ch][k]   = sampleLinear (previous.heldMag[ch], oldBins, srcIndex);
			fresh.heldFreq[ch][k]  = sampleLinear (previous.heldFreq[ch], oldBins, srcIndex);
			fresh.prevPhase[ch][k] = samplePhase (previous.prevPhase[ch], oldBins, srcIndex);
			fresh.synthPhase[ch][k] = samplePhase (previous.synthPhase[ch], oldBins, srcIndex);
		}

		std::copy (std::begin (previous.outputAccum[ch]),
		           std::end (previous.outputAccum[ch]),
		           std::begin (fresh.outputAccum[ch]));
	}

	std::copy (std::begin (previous.outputNormAccum),
	           std::end (previous.outputNormAccum),
	           std::begin (fresh.outputNormAccum));

	fresh.outputReadPos = previous.outputReadPos;
	fresh.synthCounter = juce::jlimit (0, newHop - 1,
	                                   (int) std::lround ((double) previous.synthCounter
	                                                      * (double) newHop
	                                                      / (double) oldHop));
	fresh.filteredAnalysisHop = previous.filteredAnalysisHop;
	fresh.analysisHopQuantError = previous.analysisHopQuantError;
	fresh.lastAnalysisHop = previous.lastAnalysisHop;
	fresh.freezeEntryWarmupCycles = previous.freezeEntryWarmupCycles;
	fresh.analysisHopSlewNorm = previous.analysisHopSlewNorm;
	fresh.analysisHopStepNorm = previous.analysisHopStepNorm;
	fresh.hasFrame = previous.hasFrame;
	fresh.cyclesSinceReset = previous.cyclesSinceReset;

	// Window/geometry changes should keep the spectral state, but not the
	// old overlap-add residue. Reusing outputAccum/outputNormAccum across
	// incompatible windows/hops leaks clicks through the active path.
	clearStftOutputResidueForResize();
}

void STRETRAudioProcessor::clearStftOutputResidueForResize() noexcept
{
	for (int ch = 0; ch < 2; ++ch)
		std::fill (std::begin (stft_.outputAccum[ch]), std::end (stft_.outputAccum[ch]), 0.0f);
	std::fill (std::begin (stft_.outputNormAccum), std::end (stft_.outputNormAccum), 0.0f);

	stft_.identityErrSqAccum[0] = 0.0;
	stft_.identityErrSqAccum[1] = 0.0;
	stft_.identityRefSqAccum[0] = 0.0;
	stft_.identityRefSqAccum[1] = 0.0;
	stft_.identityMaxAbsErr[0] = 0.0f;
	stft_.identityMaxAbsErr[1] = 0.0f;
	stft_.identitySampleCount = 0;
	stft_.cyclesSinceReset = 0;
	fftLastStableWetL_ = 0.0f;
	fftLastStableWetR_ = 0.0f;
	fftLastStableWetValid_ = false;

	const int newHop = juce::jmax (1, recommendedFftSynthHop (stft_.activeFftSize));
	stft_.synthCounter = juce::jmax (0, newHop - 1);
}

void STRETRAudioProcessor::resizeFft2StateAtPos (double capturePos, int fftSize, bool freezeTarget) noexcept
{
	resizeStftAtPos (capturePos, fftSize);

	if (fftSize <= 0)
		return;

	const int numBins = juce::jlimit (0, kMaxFftBins, fftSize / 2 + 1);
	for (int ch = 0; ch < 2; ++ch)
	{
		float heldEnergy = 0.0f;
		float lastEnergy = 0.0f;
		for (int k = 0; k < numBins; ++k)
		{
			heldEnergy += std::abs (stft_.heldMag[ch][k]);
			lastEnergy += std::abs (stft_.lastMag[ch][k]);
		}

		// FFT2 can enter a full-hold window resize with an empty retained cloud.
		// Seed from the latest analysed frame so amount=100 does not freeze silence.
		if (heldEnergy <= 1.0e-4f && lastEnergy > 1.0e-4f)
		{
			for (int k = 0; k < numBins; ++k)
			{
				stft_.heldMag[ch][k] = stft_.lastMag[ch][k];
				stft_.heldFreq[ch][k] = stft_.lastFreq[ch][k];
				stft_.synthPhase[ch][k] = stft_.prevPhase[ch][k];
			}
		}

		if (freezeTarget)
		{
			for (int k = 0; k < numBins; ++k)
				stft_.synthPhase[ch][k] = stft_.prevPhase[ch][k];
		}
	}

}

int STRETRAudioProcessor::samplesForMs (double ms) const noexcept
{
	const double sr = currentSampleRate > 1.0 ? currentSampleRate : 44100.0;
	return juce::jmax (1, (int) std::lround (sr * 0.001 * ms));
}

int STRETRAudioProcessor::recommendedFftWindowCrossfadeSamples() const noexcept
{
	return juce::jlimit (64, kFftWetHistoryLen - 1, samplesForMs (40.0));
}

int STRETRAudioProcessor::recommendedFft2WindowCrossfadeSamples (int fromFftSize, int toFftSize) const noexcept
{
	const int baseSamples = samplesForMs (80.0);
	const int safeFromFft = juce::jmax (1, fromFftSize);
	const int safeToFft = juce::jmax (1, toFftSize);
	const int maxFft = juce::jmax (safeFromFft, safeToFft);
	const int maxHop = juce::jmax (recommendedFftSynthHop (safeFromFft),
	                               recommendedFftSynthHop (safeToFft));
	const int geometrySamples = juce::jmax (maxFft, maxHop * 4);
	return juce::jlimit (128, kFftWetHistoryLen - 1,
	                     juce::jmax (baseSamples, geometrySamples));
}

int STRETRAudioProcessor::recommendedFftTriggerDuckHoldSamples (int fftSize) const noexcept
{
	const int safeFft = juce::jmax (1, fftSize);
	return safeFft + recommendedFftSynthHop (safeFft);
}

int STRETRAudioProcessor::recommendedFftFreezeTransitionSamples (int fftSize) const noexcept
{
	const int safeFft = juce::jmax (1, fftSize);
	const int safeHop = recommendedFftSynthHop (safeFft);
	return juce::jlimit (96, kFftWetHistoryLen - 1,
	                     juce::jmax (samplesForMs (10.0), safeHop));
}

int STRETRAudioProcessor::recommendedEngineCrossfadeSamples() const noexcept
{
	return juce::jlimit (64, kWetOutputHistoryLen - 1, samplesForMs (60.0));
}

void STRETRAudioProcessor::resetFftWindowDuckPrepareState (int capturedWindowVal, float amountVal,
                                                           int engineVal, bool triggerOn) noexcept
{
	fftParamDuckGain_ = 1.0f;
	fftParamDuckHoldRemaining_ = 0;
	fftDuckBridgeRemaining_ = 0;
	fftDuckBridgeTotal_ = 0;
	fftDuckBridgeStartL_ = 0.0f;
	fftDuckBridgeStartR_ = 0.0f;
	fftLastPostDuckOutL_ = 0.0f;
	fftLastPostDuckOutR_ = 0.0f;
	fftLastStableWetL_ = 0.0f;
	fftLastStableWetR_ = 0.0f;
	fftLastStableWetValid_ = false;
#if STRETR_ENABLE_FFT1_CLICK_DUMP
	fftDumpPrevFftWetL_ = 0.0f;
	fftDumpPrevFftWetR_ = 0.0f;
	fftDumpPrevPreStyleWetL_ = 0.0f;
	fftDumpPrevPreStyleWetR_ = 0.0f;
	fftDumpPrevPostStyleWetL_ = 0.0f;
	fftDumpPrevPostStyleWetR_ = 0.0f;
	fftDumpPrevPostFilterWetL_ = 0.0f;
	fftDumpPrevPostFilterWetR_ = 0.0f;
	fftDumpPrevPostChaosWetL_ = 0.0f;
	fftDumpPrevPostChaosWetR_ = 0.0f;
	fftDumpPrevPreDcWetL_ = 0.0f;
	fftDumpPrevPreDcWetR_ = 0.0f;
	fftDumpPrevPostDcWetL_ = 0.0f;
	fftDumpPrevPostDcWetR_ = 0.0f;
	fftDumpPrevEngineWetL_ = 0.0f;
	fftDumpPrevEngineWetR_ = 0.0f;
	fftDumpPrevFinalWetL_ = 0.0f;
	fftDumpPrevFinalWetR_ = 0.0f;
	fftDumpPrevOutL_ = 0.0f;
	fftDumpPrevOutR_ = 0.0f;
	fftDumpPrevPostDuckOutL_ = 0.0f;
	fftDumpPrevPostDuckOutR_ = 0.0f;
#endif
	fftWindowApplyDelayRemaining_ = 0;
	fftWindowCaptureRemaining_ = 0;
	fftCapturedWindowVal_ = capturedWindowVal;
	fftPendingWindowVal_ = capturedWindowVal;
	fftWindowTraceRemaining_ = 0;
	fftAmountTraceRemaining_ = 0;
	fft1ReentryTraceRemaining_ = 0;
	prevFftDuckWindowVal_ = capturedWindowVal;
	prevFftDuckAmountVal_ = amountVal;
	prevFftDuckEngineVal_ = engineVal;
	prevFftDuckTriggerOn_ = triggerOn;
}

void STRETRAudioProcessor::clearFftWindowDuckRuntimeState() noexcept
{
	fftParamDuckHoldRemaining_ = 0;
	fftParamDuckGain_ = 1.0f;
	fftDuckBridgeRemaining_ = 0;
	fftDuckBridgeTotal_ = 0;
	fftDuckBridgeStartL_ = 0.0f;
	fftDuckBridgeStartR_ = 0.0f;
	fftWindowApplyDelayRemaining_ = 0;
	fftWindowCaptureRemaining_ = 0;
	fftWindowTraceRemaining_ = 0;
	fftAmountTraceRemaining_ = 0;
	fft1ReentryTraceRemaining_ = 0;
}

void STRETRAudioProcessor::clearEngineFadeState() noexcept
{
	engineFadePos_ = 0;
	engineFadeTotal_ = 0;
	engineFadeHoldSamples_ = 0;
}

void STRETRAudioProcessor::resetEngineFadeState() noexcept
{
	engineFadeStartL_ = 0.0f;
	engineFadeStartR_ = 0.0f;
	clearEngineFadeState();
}

void STRETRAudioProcessor::clearFftOutputFadeState() noexcept
{
	fftOutputFadePos_ = 0;
	fftOutputFadeTotal_ = 0;
	fftOutputFadeHoldSamples_ = 0;
}

void STRETRAudioProcessor::resetFftOutputFadeState() noexcept
{
	fftOutputFadeReadPos_ = 0;
	clearFftOutputFadeState();
}

void STRETRAudioProcessor::clearFft1FreezeSnapshot() noexcept
{
	fft1FreezeSnapshot_ = {};
}

void STRETRAudioProcessor::captureFft1FreezeSnapshot (int styleVal, bool reverseOn) noexcept
{
	const bool genericFreezeActive = stft_.hasFrame && (stft_.lastAnalysisHop == 0);
	const bool explicitFreezeReady = fftExplicitFreezeActive_ && ! fftExplicitFreezeCapturePending_;
	if (stft_.activeFftSize <= 0 || ! stft_.hasFrame || (! genericFreezeActive && ! explicitFreezeReady))
	{
		clearFft1FreezeSnapshot();
		return;
	}

	fft1FreezeSnapshot_.fftSize = stft_.activeFftSize;
	fft1FreezeSnapshot_.style = styleVal;
	fft1FreezeSnapshot_.reverseOn = reverseOn;
	fft1FreezeSnapshot_.explicitFreeze = explicitFreezeReady;
	fft1FreezeSnapshot_.hasFrame = stft_.hasFrame;
	fft1FreezeSnapshot_.valid = true;

	for (int ch = 0; ch < 2; ++ch)
	{
		std::copy (std::begin (stft_.prevPhase[ch]), std::end (stft_.prevPhase[ch]),
		           std::begin (fft1FreezeSnapshot_.prevPhase[ch]));
		std::copy (std::begin (stft_.synthPhase[ch]), std::end (stft_.synthPhase[ch]),
		           std::begin (fft1FreezeSnapshot_.synthPhase[ch]));
		std::copy (std::begin (stft_.prevMag[ch]), std::end (stft_.prevMag[ch]),
		           std::begin (fft1FreezeSnapshot_.prevMag[ch]));
		std::copy (std::begin (stft_.lastMag[ch]), std::end (stft_.lastMag[ch]),
		           std::begin (fft1FreezeSnapshot_.lastMag[ch]));
		std::copy (std::begin (stft_.lastFreq[ch]), std::end (stft_.lastFreq[ch]),
		           std::begin (fft1FreezeSnapshot_.lastFreq[ch]));
		std::copy (std::begin (stft_.heldMag[ch]), std::end (stft_.heldMag[ch]),
		           std::begin (fft1FreezeSnapshot_.heldMag[ch]));
		std::copy (std::begin (stft_.heldFreq[ch]), std::end (stft_.heldFreq[ch]),
		           std::begin (fft1FreezeSnapshot_.heldFreq[ch]));
	}
}

bool STRETRAudioProcessor::canRestoreFft1FreezeSnapshot (int fftSize, int styleVal, bool reverseOn,
                                                         bool triggerOn, bool targetFreeze,
                                                         float targetSpeed) const noexcept
{
	juce::ignoreUnused (targetSpeed);
	if (! fft1FreezeSnapshot_.valid || ! triggerOn || ! fft1FreezeSnapshot_.hasFrame)
		return false;

	if (! targetFreeze)
		return false;

	if (fft1FreezeSnapshot_.fftSize != fftSize
	    || fft1FreezeSnapshot_.style != styleVal
	    || fft1FreezeSnapshot_.reverseOn != reverseOn)
	{
		return false;
	}

	if (fft1FreezeSnapshot_.explicitFreeze && ! targetFreeze)
		return false;

	return true;
}

void STRETRAudioProcessor::restoreFft1FreezeSnapshot (bool targetFreeze) noexcept
{
	if (! fft1FreezeSnapshot_.valid)
		return;

	for (int ch = 0; ch < 2; ++ch)
	{
		std::copy (std::begin (fft1FreezeSnapshot_.prevPhase[ch]), std::end (fft1FreezeSnapshot_.prevPhase[ch]),
		           std::begin (stft_.prevPhase[ch]));
		std::copy (std::begin (fft1FreezeSnapshot_.synthPhase[ch]), std::end (fft1FreezeSnapshot_.synthPhase[ch]),
		           std::begin (stft_.synthPhase[ch]));
		std::copy (std::begin (fft1FreezeSnapshot_.prevMag[ch]), std::end (fft1FreezeSnapshot_.prevMag[ch]),
		           std::begin (stft_.prevMag[ch]));
		std::copy (std::begin (fft1FreezeSnapshot_.lastMag[ch]), std::end (fft1FreezeSnapshot_.lastMag[ch]),
		           std::begin (stft_.lastMag[ch]));
		std::copy (std::begin (fft1FreezeSnapshot_.lastFreq[ch]), std::end (fft1FreezeSnapshot_.lastFreq[ch]),
		           std::begin (stft_.lastFreq[ch]));
		std::copy (std::begin (fft1FreezeSnapshot_.heldMag[ch]), std::end (fft1FreezeSnapshot_.heldMag[ch]),
		           std::begin (stft_.heldMag[ch]));
		std::copy (std::begin (fft1FreezeSnapshot_.heldFreq[ch]), std::end (fft1FreezeSnapshot_.heldFreq[ch]),
		           std::begin (stft_.heldFreq[ch]));
	}

	stft_.hasFrame = fft1FreezeSnapshot_.hasFrame;
	stft_.lastAnalysisHop = 0;
	stft_.filteredAnalysisHop = -1.0;
	stft_.analysisHopQuantError = 0.0;
	stft_.freezeEntryWarmupCycles = 0;
	stft_.analysisHopSlewNorm = 0.0f;
	stft_.analysisHopStepNorm = 0.0f;
	stft_.cyclesSinceReset = 0;
	fftStartupWarmupRemainingCycles_ = 0;
	fftFreezeTransitionRemaining_ = 0;
	fftFreezeTransitionTotal_ = 0;
	fftExplicitFreezeActive_ = targetFreeze && fft1FreezeSnapshot_.explicitFreeze;
	fftExplicitFreezeCapturePending_ = false;
}

int STRETRAudioProcessor::getWindowTransitionRemainingForEngine (int engineVal) const noexcept
{
	if (engineVal == 2)
		return fft1WindowTransitionRemaining_;
	if (engineVal == 3)
		return fft2WindowTransitionRemaining_;
	return 0;
}

int STRETRAudioProcessor::getWindowTransitionTotalForEngine (int engineVal) const noexcept
{
	if (engineVal == 2)
		return fft1WindowTransitionTotal_;
	if (engineVal == 3)
		return fft2WindowTransitionTotal_;
	return 0;
}

bool STRETRAudioProcessor::isWindowTransitionActiveForEngine (int engineVal) const noexcept
{
	return getWindowTransitionRemainingForEngine (engineVal) > 0
		&& getWindowTransitionTotalForEngine (engineVal) > 0;
}

float STRETRAudioProcessor::getWindowTransitionProgressForEngine (int engineVal) const noexcept
{
	const int remaining = getWindowTransitionRemainingForEngine (engineVal);
	const int total = getWindowTransitionTotalForEngine (engineVal);
	if (remaining <= 0 || total <= 0)
		return 1.0f;

	return juce::jlimit (0.0f, 1.0f, 1.0f - (float) remaining / (float) total);
}

void STRETRAudioProcessor::startWindowTransitionForEngine (int engineVal, int totalSamples) noexcept
{
	if (engineVal == 2)
	{
		fft1WindowTransitionTotal_ = totalSamples;
		fft1WindowTransitionRemaining_ = fft1WindowTransitionTotal_;
	}
	else if (engineVal == 3)
	{
		fft2WindowTransitionTotal_ = totalSamples;
		fft2WindowTransitionRemaining_ = fft2WindowTransitionTotal_;
	}
}

void STRETRAudioProcessor::clearWindowTransitionForEngine (int engineVal) noexcept
{
	if (engineVal == 2)
	{
		fft1WindowTransitionRemaining_ = 0;
		fft1WindowTransitionTotal_ = 0;
	}
	else if (engineVal == 3)
	{
		fft2WindowTransitionRemaining_ = 0;
		fft2WindowTransitionTotal_ = 0;
	}
}

void STRETRAudioProcessor::decrementWindowTransitionForEngine (int engineVal) noexcept
{
	if (engineVal == 2)
	{
		if (fft1WindowTransitionRemaining_ > 0)
			--fft1WindowTransitionRemaining_;
	}
	else if (engineVal == 3)
	{
		if (fft2WindowTransitionRemaining_ > 0)
			--fft2WindowTransitionRemaining_;
	}
}

int STRETRAudioProcessor::recommendedFftSynthHop (int fftSize) const noexcept
{
	if (fftSize <= 0)
		return 0;

	if (fftSize <= 64)
		return 8;

	if (fftSize <= 128)
		return 16;

	return juce::jmax (16, fftSize / 4);
}

void STRETRAudioProcessor::performStftCycle (int fftSize, int analysisHop, int synthesisHop,
                                              float pitchRate, bool reverseOn, float pitchRateR,
                                              bool wideMode)
{
	if (fft_ == nullptr || inputBufLen_ <= 0 || fftSize <= 0) return;
   #if JUCE_DEBUG
	const bool fftDebugEnabled = DeveloperDiagnosticsConfig::kEnableFftTraceRecording;
   #else
	constexpr bool fftDebugEnabled = false;
   #endif
   #if JUCE_DEBUG
	const auto cycleStartTicks = fftDebugEnabled ? juce::Time::getHighResolutionTicks() : juce::int64 { 0 };
   #endif

	const int numBins    = fftSize / 2 + 1;
	const int outBufLen  = kStftOutBufLen;
	const float twoPi    = juce::MathConstants<float>::twoPi;
	const float pi       = juce::MathConstants<float>::pi;
	const float expBase  = twoPi / (float) fftSize;
#if JUCE_DEBUG || STRETR_ENABLE_FFT1_CLICK_DUMP
	const double analysisReadBefore = stft_.analysisReadPos;
#endif
#if STRETR_ENABLE_FFT1_CLICK_DUMP
	if constexpr (DeveloperDiagnosticsConfig::kEnableFft1AmountFreezeDump)
	{
		++fftDebugContext_.fftCycleSerial;
		fftDebugContext_.fftRuntimeRoute = 1;
		fftDebugContext_.signedAnalysisHop = reverseOn ? -analysisHop : analysisHop;
		fftDebugContext_.freezeAnalysisInput = 0;
		fftDebugContext_.spectralHoldCoeff = 0.0f;
		fftDebugContext_.analysisReadBefore = analysisReadBefore;
		fftDebugContext_.analysisReadAfter = analysisReadBefore;
	}
#endif
   #if JUCE_DEBUG
	const float normAtRead = stft_.outputNormAccum[stft_.outputReadPos];
	const float invNormAtRead = (normAtRead > 1.0e-6f) ? (1.0f / normAtRead) : 0.0f;
	const float previewOutL = stft_.outputAccum[0][stft_.outputReadPos] * invNormAtRead;
	const float previewOutR = stft_.outputAccum[1][stft_.outputReadPos] * invNormAtRead;
	const double analysisLagSamples = (fftDebugEnabled && inputBufLen_ > 0)
		? std::fmod ((double) inputBufWritePos_ - analysisReadBefore + (double) inputBufLen_,
		             (double) inputBufLen_)
		: 0.0;
   #endif
	int debugPeakCount[2] = {};
	int debugLockedBins[2] = {};
	float debugFrameRms[2] = {};
	float debugOutputRms[2] = {};
	float debugOutputStartDelta[2] = {};
	float debugSpectralFlux[2] = {};
	float debugPhaseResetMix[2] = {};
	float debugLockStrengthMean[2] = {};
	const bool freezeEntryCrossfadeActive = (fftSize >= 4096)
		&& (analysisHop <= 0)
		&& (stft_.freezeEntryWarmupCycles > 0 || stft_.cyclesSinceReset < 8);
	const float freezeEntryCrossfadeNorm = freezeEntryCrossfadeActive
		? juce::jlimit (0.0f, 1.0f,
		                juce::jmax ((float) stft_.freezeEntryWarmupCycles / ((fftSize >= 8192) ? 8.0f : 6.0f),
		                            (8.0f - (float) stft_.cyclesSinceReset) / 8.0f))
		: 0.0f;
	const int freezeEntryFadeSamples = freezeEntryCrossfadeActive
		? juce::jlimit (64, fftSize / 8, juce::jmax (64, synthesisHop / 4))
		: 0;
	juce::int64 forwardFftTicks = 0;
	juce::int64 binAnalysisTicks = 0;
	juce::int64 pitchMapTicks = 0;
	juce::int64 phaseLockTicks = 0;
	juce::int64 ifftOlaTicks = 0;
	const bool analyseCurrentFrame = (analysisHop > 0) || ! stft_.hasFrame;

	for (int ch = 0; ch < 2; ++ch)
	{
		// DUAL: R channel uses pitchRateR if provided
		const float pr = (ch == 1 && pitchRateR > 0.0f) ? pitchRateR : pitchRate;
		float frameEnergy = 0.0f;

		// Analysis
		if (analyseCurrentFrame)
		{
			const auto forwardStartTicks = fftDebugEnabled ? juce::Time::getHighResolutionTicks() : juce::int64 { 0 };
			for (int j = 0; j < fftSize; ++j)
			{
				const int idx = ((int) stft_.analysisReadPos + j) & inputBufMask_;
				fftWork_[j] = inputBuf_[ch][(size_t) idx] * fftWindow_[j];
				frameEnergy += fftWork_[j] * fftWork_[j];
			}
			for (int j = fftSize; j < fftSize * 2; ++j)
				fftWork_[j] = 0.0f;

			fft_->performRealOnlyForwardTransform (fftWork_, true);
			if (fftDebugEnabled)
				forwardFftTicks += juce::Time::getHighResolutionTicks() - forwardStartTicks;

			const auto binAnalysisStartTicks = fftDebugEnabled ? juce::Time::getHighResolutionTicks() : juce::int64 { 0 };
			const bool useFastLargeFftAnalysis = (fftSize > 1024);
			const float phaseAnalysisHop = reverseOn ? - (float) analysisHop : (float) analysisHop;
			const float invPhaseAnalysisHop = (analysisHop > 0) ? (1.0f / phaseAnalysisHop) : 0.0f;
			const float expectedPhaseStep = expBase * phaseAnalysisHop;
			float expectedPhase = 0.0f;
			float baseFreq = 0.0f;
			float positiveFlux = 0.0f;
			float fluxNorm = 0.0f;
			for (int k = 0; k < numBins; ++k)
			{
				const float re  = fftWork_[k * 2];
				const float im  = fftWork_[k * 2 + 1];
				const float mag = std::sqrt (re * re + im * im);
				const float prevMag = stft_.prevMag[ch][k];
				const float ph  = useFastLargeFftAnalysis ? fastAtan2Approx (im, re)
				                                          : std::atan2 (im, re);

				float phaseDiff = ph - stft_.prevPhase[ch][k];
				stft_.prevPhase[ch][k] = ph;

				if (analysisHop > 0)
				{
					if (useFastLargeFftAnalysis)
					{
						phaseDiff -= expectedPhase;
						phaseDiff = wrapPhaseToPiFast (phaseDiff);
						stft_.lastFreq[ch][k] = baseFreq + phaseDiff * invPhaseAnalysisHop;
					}
					else
					{
						phaseDiff -= expBase * (float) k * phaseAnalysisHop;
						while (phaseDiff >  pi) phaseDiff -= twoPi;
						while (phaseDiff < -pi) phaseDiff += twoPi;
						stft_.lastFreq[ch][k] = expBase * (float) k
						                       + phaseDiff / phaseAnalysisHop;
					}
				}
				else
				{
					stft_.lastFreq[ch][k] = useFastLargeFftAnalysis ? baseFreq
					                                                : expBase * (float) k;
				}

				positiveFlux += juce::jmax (0.0f, mag - prevMag);
				fluxNorm += juce::jmax (mag, prevMag);
				stft_.prevMag[ch][k] = mag;
				stft_.lastMag[ch][k] = mag;
				if (useFastLargeFftAnalysis)
				{
					expectedPhase += expectedPhaseStep;
					baseFreq += expBase;
				}
			}
			debugSpectralFlux[ch] = (fluxNorm > 1.0e-6f) ? (positiveFlux / fluxNorm) : 0.0f;
			if (fftDebugEnabled)
				binAnalysisTicks += juce::Time::getHighResolutionTicks() - binAnalysisStartTicks;
		}
		debugFrameRms[ch] = std::sqrt (frameEnergy / (float) juce::jmax (1, fftSize));

        // Synthesis

		// Step 1: compute per-bin magnitude and phase for synthesis
		const bool passthrough = (analysisHop == synthesisHop)
		                      && (std::abs (pr - 1.0f) <= 0.001f);

		float synthMag[kMaxFftBins];

		if (passthrough)
		{
			// Perfect reconstruction: use analysis mag & phase directly
			for (int k = 0; k < numBins; ++k)
			{
				synthMag[k] = stft_.lastMag[ch][k];
				stft_.synthPhase[ch][k] = stft_.prevPhase[ch][k];
			}
		}
		else
		{
			const auto pitchMapStartTicks = fftDebugEnabled ? juce::Time::getHighResolutionTicks() : juce::int64 { 0 };
			for (int k = 0; k < numBins; ++k)
			{
				float mag, freq;

				if (std::abs (pr - 1.0f) > 0.001f)
				{
					const float srcF = (float) k / pr;
					const int   s0   = (int) srcF;
					const float fr   = srcF - (float) s0;

					mag  = 0.0f;
					freq = expBase * (float) k;

					if (s0 >= 0 && s0 < numBins)
					{
						mag  += stft_.lastMag[ch][s0] * (1.0f - fr);
						freq  = stft_.lastFreq[ch][s0] * pr;
					}
					if (s0 + 1 < numBins)
						mag += stft_.lastMag[ch][s0 + 1] * fr;
				}
				else
				{
					mag  = stft_.lastMag[ch][k];
					freq = stft_.lastFreq[ch][k];
				}

				synthMag[k] = mag;
				stft_.synthPhase[ch][k] += freq * (float) synthesisHop;
			}
			if (fftDebugEnabled)
				pitchMapTicks += juce::Time::getHighResolutionTicks() - pitchMapStartTicks;

			const bool lowFft64 = (fftSize <= 64);
			const float lowFftPitchNorm = juce::jlimit (0.0f, 1.0f,
			                                            std::abs (pr - 1.0f) / 3.0f);

			// Phase locking (only when not in passthrough).
			// Keep the legacy identity locking for <=1024, but for larger FFTs
			// use prominent peaks and regions of influence so bins stay coherent
			// without being chained to every weak local maximum.
			{
				const auto phaseLockStartTicks = fftDebugEnabled ? juce::Time::getHighResolutionTicks() : juce::int64 { 0 };
				bool isPeak[kMaxFftBins] = {};
				isPeak[0] = (numBins > 1) ? (synthMag[0] >= synthMag[1]) : true;
				for (int k = 1; k < numBins - 1; ++k)
					isPeak[k] = (synthMag[k] >= synthMag[k - 1] && synthMag[k] >= synthMag[k + 1]);
				if (numBins > 1)
					isPeak[numBins - 1] = (synthMag[numBins - 1] >= synthMag[numBins - 2]);

				if (fftSize <= 1024)
				{
					int peakCount = 0;
					float lockStrengthSum = 0.0f;
					int lockStrengthCount = 0;
					for (int k = 0; k < numBins; ++k)
						if (isPeak[k])
							++peakCount;
					debugPeakCount[ch] = peakCount;

					int nearestPk[kMaxFftBins];
					int lp = 0;
					for (int k = 0; k < numBins; ++k) { if (isPeak[k]) lp = k; nearestPk[k] = lp; }
					lp = numBins - 1;
					for (int k = numBins - 1; k >= 0; --k)
					{
						if (isPeak[k]) lp = k;
						if (std::abs (k - lp) < std::abs (k - nearestPk[k]))
							nearestPk[k] = lp;
					}

					for (int k = 0; k < numBins; ++k)
					{
						if (! isPeak[k])
						{
							const int pk = nearestPk[k];
							const float lockedPhase = stft_.synthPhase[ch][pk]
							    + (stft_.prevPhase[ch][k] - stft_.prevPhase[ch][pk]);
							if (lowFft64)
							{
								const float lowFftLockBlend = 0.18f + 0.14f * (1.0f - lowFftPitchNorm);
								const float phaseToLocked = wrapPhaseToPiFast (lockedPhase - stft_.synthPhase[ch][k]);
								stft_.synthPhase[ch][k] += lowFftLockBlend * phaseToLocked;
								lockStrengthSum += lowFftLockBlend;
							}
							else
							{
								stft_.synthPhase[ch][k] = lockedPhase;
								lockStrengthSum += 1.0f;
							}
							++debugLockedBins[ch];
							++lockStrengthCount;
						}
					}
					debugLockStrengthMean[ch] = (lockStrengthCount > 0)
						? (lockStrengthSum / (float) lockStrengthCount)
						: 0.0f;
				}
				else
				{
					int prominentPeaks[kMaxFftBins];
					int prominentPeakCount = 0;
					const bool transientAwareReset = (analysisHop > 0);
					float pitchResetScale = 1.0f;
					if (std::abs (pr - 1.0f) > 0.001f)
						pitchResetScale = juce::jlimit (0.35f, 0.8f, 1.0f / std::sqrt (std::abs (pr)));

					const float transientResetMix = transientAwareReset
						? juce::jlimit (0.0f, 1.0f, (debugSpectralFlux[ch] - 0.07f) / 0.10f) * pitchResetScale
						: 0.0f;
					float stabilityResetMix = 0.0f;
					float startupResetMix = 0.0f;
					if (fftSize == 2048)
					{
						const float stretchNorm = juce::jlimit (0.0f, 1.0f,
						                                        std::abs ((float) analysisHop - (float) synthesisHop)
						                                            / (float) juce::jmax (1, synthesisHop));
						const float pitchNorm = juce::jlimit (0.0f, 1.0f,
						                                      std::abs (pr - 1.0f) / 3.0f);
						const float activityNorm = juce::jmax (stretchNorm, pitchNorm);
						if (activityNorm > 0.001f)
						{
							const float cycleNorm = juce::jlimit (0.0f, 1.0f,
							                                      (float) stft_.cyclesSinceReset / 160.0f);
							stabilityResetMix = (0.03f + 0.08f * cycleNorm)
							                  * juce::jmax (0.35f, activityNorm);

							const float startupNorm = juce::jlimit (0.0f, 1.0f,
							                                        (96.0f - (float) stft_.cyclesSinceReset) / 96.0f);
							startupResetMix = (0.06f + 0.18f * pitchNorm)
							                * startupNorm
							                * juce::jmax (0.35f, activityNorm);
						}
					}
					float hopSlewResetMix = 0.0f;
					float hopStepResetMix = 0.0f;
					if (fftSize == 2048)
					{
						const float lowFluxNorm = juce::jlimit (0.0f, 1.0f,
						                                        (0.10f - debugSpectralFlux[ch]) / 0.07f);
						hopSlewResetMix = 0.16f
						                * juce::jlimit (0.0f, 1.0f, stft_.analysisHopSlewNorm)
						                * (0.65f + 0.35f * lowFluxNorm);
						hopStepResetMix = 0.28f
						                * juce::jlimit (0.0f, 1.0f, stft_.analysisHopStepNorm)
						                * (0.60f + 0.40f * lowFluxNorm);
					}
					float pitchStabilityResetMix = 0.0f;
					if (fftSize == 2048)
					{
						const float pitchNorm = juce::jlimit (0.0f, 1.0f,
						                                      (std::abs (pr - 1.0f) - 0.15f) / 2.85f);
						const float lowFluxNorm = juce::jlimit (0.0f, 1.0f,
						                                        (0.12f - debugSpectralFlux[ch]) / 0.08f);
						pitchStabilityResetMix = 0.06f * pitchNorm * lowFluxNorm;
					}
					float freezeTransitionResetMix = 0.0f;
					if (fftSize != 2048 && synthesisHop > 0)
					{
						const float freezeHopThreshold = (float) juce::jmax (1, synthesisHop / 8);
						const float lowHopNorm = (analysisHop <= 0)
							? 1.0f
							: juce::jlimit (0.0f, 1.0f,
							                (freezeHopThreshold - (float) analysisHop) / freezeHopThreshold);
						if (lowHopNorm > 0.0f)
						{
							const float lowFluxNorm = juce::jlimit (0.0f, 1.0f,
							                                        (0.09f - debugSpectralFlux[ch]) / 0.06f);
							const float transitionNorm = juce::jmax (juce::jlimit (0.0f, 1.0f, stft_.analysisHopStepNorm),
							                                         juce::jlimit (0.0f, 1.0f, stft_.analysisHopSlewNorm));
							const float baseTransitionMix = (fftSize >= 8192) ? 0.40f
							                              : (fftSize >= 4096) ? 0.33f
							                                                   : 0.26f;
							const float transitionFloor = (fftSize >= 4096 && analysisHop <= 0) ? 0.45f : 0.0f;
							const float effectiveTransitionNorm = juce::jmax (transitionNorm, transitionFloor);
							freezeTransitionResetMix = baseTransitionMix * lowHopNorm * effectiveTransitionNorm
							                         * (0.70f + 0.30f * lowFluxNorm);
						}
					}
					float freezeWarmupResetMix = 0.0f;
					if (fftSize != 2048 && synthesisHop > 0 && stft_.freezeEntryWarmupCycles > 0)
					{
						const float freezeHopThreshold = (float) juce::jmax (1, synthesisHop / 8);
						const float lowHopNorm = (analysisHop <= 0)
							? 1.0f
							: juce::jlimit (0.0f, 1.0f,
							                (freezeHopThreshold - (float) analysisHop) / freezeHopThreshold);
						const float lowFluxNorm = juce::jlimit (0.0f, 1.0f,
						                                        (0.09f - debugSpectralFlux[ch]) / 0.06f);
						const float warmupDivisor = (fftSize >= 8192) ? 8.0f
						                         : (fftSize >= 4096) ? 6.0f
						                                              : 4.0f;
						const float warmupNorm = juce::jlimit (0.0f, 1.0f,
						                                       (float) stft_.freezeEntryWarmupCycles / warmupDivisor);
						const float baseFloor = (fftSize >= 8192) ? 0.34f
						                         : (fftSize >= 4096) ? 0.26f
						                                              : 0.11f;
						const float hopFloor = (fftSize >= 4096) ? 0.80f : 0.45f;
						freezeWarmupResetMix = baseFloor * warmupNorm
						                     * juce::jmax (hopFloor, lowHopNorm)
						                     * (0.75f + 0.25f * lowFluxNorm);
					}
					float freezeStartupResetMix = 0.0f;
					if (fftSize >= 4096 && synthesisHop > 0 && analysisHop <= 0)
					{
						const float startupNorm = juce::jlimit (0.0f, 1.0f,
						                                        (8.0f - (float) stft_.cyclesSinceReset) / 8.0f);
						if (startupNorm > 0.0f)
						{
							const float lowFluxNorm = juce::jlimit (0.0f, 1.0f,
							                                        (0.10f - debugSpectralFlux[ch]) / 0.07f);
							const float baseFloor = (fftSize >= 8192) ? 0.30f : 0.22f;
							freezeStartupResetMix = baseFloor * startupNorm
							                      * (0.75f + 0.25f * lowFluxNorm);
						}
					}
					float unityPitchStartupResetMix = 0.0f;
					if (fftSize >= 2048
					    && fftStartupWarmupRemainingCycles_ > 0
					    && std::abs (pr - 1.0f) > 0.0015f)
					{
						const float startupNorm = juce::jlimit (0.0f, 1.0f,
						                                        (float) fftStartupWarmupRemainingCycles_ / 4.0f);
						const float pitchNorm = juce::jlimit (0.0f, 1.0f,
						                                      std::abs (pr - 1.0f) / 3.0f);
						const float lowFluxNorm = juce::jlimit (0.0f, 1.0f,
						                                        (0.14f - debugSpectralFlux[ch]) / 0.10f);
						const float baseFloor = (fftSize >= 8192) ? 0.30f
						                         : (fftSize >= 4096) ? 0.24f
						                                              : 0.18f;
						unityPitchStartupResetMix = baseFloor * startupNorm
						                          * (0.50f + 0.50f * pitchNorm)
						                          * (0.60f + 0.40f * lowFluxNorm);
					}
					const float resetMix = juce::jlimit (0.0f, 1.0f,
					                                     juce::jmax (juce::jmax (juce::jmax (juce::jmax (juce::jmax (juce::jmax (transientResetMix, stabilityResetMix),
					                                                                                               startupResetMix),
					                                                                                  hopSlewResetMix),
					                                                                     hopStepResetMix),
					                                                 pitchStabilityResetMix),
					                                                 juce::jmax (juce::jmax (juce::jmax (freezeTransitionResetMix,
					                                                                         freezeWarmupResetMix),
					                                                                         freezeStartupResetMix),
					                                                             unityPitchStartupResetMix)));
					debugPhaseResetMix[ch] = resetMix;

					float maxSynthMag = 0.0f;
					for (int k = 0; k < numBins; ++k)
						maxSynthMag = juce::jmax (maxSynthMag, synthMag[k]);

					const float peakFloor = maxSynthMag * 0.005f;
					const float prominenceFloor = maxSynthMag * 0.0015f;
					const int minPeakSpacing = 3;

					for (int k = 0; k < numBins; ++k)
					{
						if (! isPeak[k])
							continue;

						const float mag = synthMag[k];
						const float left = (k > 0) ? synthMag[k - 1] : mag;
						const float right = (k + 1 < numBins) ? synthMag[k + 1] : mag;
						const float localProminence = mag - 0.5f * (left + right);

						if (mag < peakFloor || localProminence < prominenceFloor)
							continue;

						if (prominentPeakCount > 0
						    && (k - prominentPeaks[prominentPeakCount - 1]) <= minPeakSpacing)
						{
							const int prevPk = prominentPeaks[prominentPeakCount - 1];
							if (mag > synthMag[prevPk])
								prominentPeaks[prominentPeakCount - 1] = k;
							continue;
						}

						prominentPeaks[prominentPeakCount++] = k;
					}

					if (prominentPeakCount == 0 && numBins > 0)
					{
						int strongestPeak = 0;
						for (int k = 1; k < numBins; ++k)
							if (synthMag[k] > synthMag[strongestPeak])
								strongestPeak = k;
						prominentPeaks[prominentPeakCount++] = strongestPeak;
					}

					debugPeakCount[ch] = prominentPeakCount;

					for (int peakIndex = 0; peakIndex < prominentPeakCount; ++peakIndex)
					{
						const int pk = prominentPeaks[peakIndex];
						const int leftEdge = (peakIndex == 0)
							? 0
							: ((prominentPeaks[peakIndex - 1] + pk) / 2 + 1);
						const int rightEdge = (peakIndex == prominentPeakCount - 1)
							? (numBins - 1)
							: ((pk + prominentPeaks[peakIndex + 1]) / 2);
						const float peakPrevPhase = stft_.prevPhase[ch][pk];
						float peakSynthPhase = stft_.synthPhase[ch][pk];

						if (resetMix > 0.0f)
						{
							const float phaseToAnalysis = wrapPhaseToPiFast (peakPrevPhase - peakSynthPhase);
							peakSynthPhase += resetMix * phaseToAnalysis;
							stft_.synthPhase[ch][pk] = peakSynthPhase;
						}

						const int leftRadius = juce::jmax (1, pk - leftEdge);
						const int rightRadius = juce::jmax (1, rightEdge - pk);
						const int regionRadius = juce::jmax (leftRadius, rightRadius);
						const float baseEdgeLock = (fftSize >= 8192) ? 0.18f
						                         : (fftSize >= 4096) ? 0.32f
						                                              : 0.55f;
						const float widthRelax = juce::jlimit (0.0f, 0.12f,
						                                       0.008f * (float) juce::jmax (0, regionRadius - 8));
						const float pitchRelax = juce::jlimit (0.0f, 0.16f,
						                                       0.06f * std::abs (pr - 1.0f));
						const float resetTighten = 0.08f * resetMix;
						const float edgeLock = juce::jlimit (0.12f, 0.85f,
						                                     baseEdgeLock - widthRelax - pitchRelax + resetTighten);
						float lockStrengthSum = 0.0f;
						int lockStrengthCount = 0;

						for (int k = leftEdge; k <= rightEdge; ++k)
						{
							if (k == pk)
								continue;

							const int sideRadius = (k < pk) ? leftRadius : rightRadius;
							const float t = juce::jlimit (0.0f, 1.0f,
							                              (float) std::abs (k - pk) / (float) juce::jmax (1, sideRadius));
							const float smoothT = t * t * (3.0f - 2.0f * t);
							const float lockBlend = 1.0f + (edgeLock - 1.0f) * smoothT;
							const float lockedPhase = peakSynthPhase
							    + (stft_.prevPhase[ch][k] - peakPrevPhase);
							const float phaseToLocked = wrapPhaseToPiFast (lockedPhase - stft_.synthPhase[ch][k]);
							stft_.synthPhase[ch][k] += lockBlend * phaseToLocked;
							++debugLockedBins[ch];
							lockStrengthSum += lockBlend;
							++lockStrengthCount;
						}

						if (lockStrengthCount > 0)
							debugLockStrengthMean[ch] += lockStrengthSum / (float) lockStrengthCount;
					}

					if (prominentPeakCount > 0)
						debugLockStrengthMean[ch] /= (float) prominentPeakCount;
				}

				if (lowFft64)
				{
					const float analysisPhaseRelax = 0.12f + 0.18f * lowFftPitchNorm;
					for (int k = 0; k < numBins; ++k)
					{
						const float phaseToAnalysis = wrapPhaseToPiFast (stft_.prevPhase[ch][k]
						                                                 - stft_.synthPhase[ch][k]);
						stft_.synthPhase[ch][k] += analysisPhaseRelax * phaseToAnalysis;
					}
				}
				if (fftDebugEnabled)
					phaseLockTicks += juce::Time::getHighResolutionTicks() - phaseLockStartTicks;
			}
		}

		// Step 3: write complex output (only numBins used by performRealOnlyInverseTransform)
		const auto ifftOlaStartTicks = fftDebugEnabled ? juce::Time::getHighResolutionTicks() : juce::int64 { 0 };
		for (int k = 0; k < numBins; ++k)
		{
            // WIDE: add linear phase ramp to R -> temporal shift of fftSize/2 samples
			float ph = stft_.synthPhase[ch][k];
			if (wideMode && ch == 1)
                ph += pi * (float) k;  // k * pi = half-window linear delay
			fftWork_[k * 2]     = synthMag[k] * std::cos (ph);
			fftWork_[k * 2 + 1] = synthMag[k] * std::sin (ph);
		}

		fft_->performRealOnlyInverseTransform (fftWork_);

		float outputEnergy = 0.0f;
		const int startOutIdx = stft_.outputReadPos & (outBufLen - 1);
		const float previousStart = stft_.outputAccum[ch][startOutIdx];
		for (int j = 0; j < fftSize; ++j)
		{
			const int outIdx = (stft_.outputReadPos + j) & (outBufLen - 1);
			const float windowSq = fftWindow_[j] * fftWindow_[j];
			float outSample = fftWork_[j] * fftWindow_[j];
			if (freezeEntryCrossfadeActive && j < freezeEntryFadeSamples)
			{
				const float fadePos = (freezeEntryFadeSamples > 1)
					? ((float) j / (float) (freezeEntryFadeSamples - 1))
					: 1.0f;
				const float fadeCurve = fadePos * fadePos * (3.0f - 2.0f * fadePos);
				const float startGain = (fftSize >= 8192) ? 0.10f : 0.18f;
				const float fadeGain = (startGain + (1.0f - startGain) * fadeCurve);
				const float gain = juce::jlimit (0.0f, 1.0f,
				                                 1.0f - freezeEntryCrossfadeNorm
				                                       + freezeEntryCrossfadeNorm * fadeGain);
				outSample *= gain;
			}
			if (j == 0)
				debugOutputStartDelta[ch] = outSample - previousStart;
			outputEnergy += outSample * outSample;
			stft_.outputAccum[ch][outIdx] += outSample;
			if (ch == 0)
				stft_.outputNormAccum[outIdx] += windowSq;
		}
		if (fftDebugEnabled)
			ifftOlaTicks += juce::Time::getHighResolutionTicks() - ifftOlaStartTicks;
		debugOutputRms[ch] = std::sqrt (outputEnergy / (float) juce::jmax (1, fftSize));
	}

	if (analyseCurrentFrame)
		stft_.hasFrame = true;

	// Advance analysis read position
	if (analysisHop > 0)
	{
		const double dir = reverseOn ? -1.0 : 1.0;
		stft_.analysisReadPos += (double) analysisHop * dir;
		if (stft_.analysisReadPos >= (double) inputBufLen_)
			stft_.analysisReadPos -= (double) inputBufLen_;
		else if (stft_.analysisReadPos < 0.0)
			stft_.analysisReadPos += (double) inputBufLen_;
	}
#if STRETR_ENABLE_FFT1_CLICK_DUMP
	if constexpr (DeveloperDiagnosticsConfig::kEnableFft1AmountFreezeDump)
		fftDebugContext_.analysisReadAfter = stft_.analysisReadPos;
#endif

   #if JUCE_DEBUG
	if (fftDebugEnabled)
	{
		FftDebugEntry dbg {};
		dbg.blockIndex = fftDebugContext_.blockIndex;
		dbg.sampleIndex = fftDebugContext_.sampleIndex;
		dbg.eventType = 0;
		dbg.engine = fftDebugContext_.engine;
		dbg.amount = fftDebugContext_.amount;
		dbg.pitch = fftDebugContext_.pitch;
		dbg.speed = fftDebugContext_.speed;
		dbg.pitchRate = pitchRate;
		dbg.windowSamples = fftDebugContext_.windowSamples;
		dbg.fftSize = fftSize;
		dbg.targetAnalysisHop = fftDebugContext_.targetAnalysisHop;
		dbg.filteredAnalysisHop = fftDebugContext_.filteredAnalysisHop;
		dbg.analysisHopQuantError = fftDebugContext_.analysisHopQuantError;
		dbg.lastAnalysisHop = fftDebugContext_.lastAnalysisHop;
		dbg.freezeEntryWarmupCycles = fftDebugContext_.freezeEntryWarmupCycles;
		dbg.fftStartupWarmupRemainingCycles = fftDebugContext_.fftStartupWarmupRemainingCycles;
		dbg.fftExplicitFreezeActive = fftDebugContext_.fftExplicitFreezeActive;
		dbg.fftExplicitFreezeCapturePending = fftDebugContext_.fftExplicitFreezeCapturePending;
		dbg.fftTargetFreeze = fftDebugContext_.fftTargetFreeze;
		dbg.analysisHop = analysisHop;
		dbg.synthesisHop = synthesisHop;
		dbg.style = fftDebugContext_.style;
		dbg.reverseOn = reverseOn ? 1 : 0;
		dbg.triggerOn = fftDebugContext_.triggerOn;
		dbg.wideMode = wideMode ? 1 : 0;
		dbg.passthrough = ((analysisHop == synthesisHop) && (std::abs (pitchRate - 1.0f) <= 0.001f)) ? 1 : 0;
		dbg.peakCountL = debugPeakCount[0];
		dbg.peakCountR = debugPeakCount[1];
		dbg.lockedBinsL = debugLockedBins[0];
		dbg.lockedBinsR = debugLockedBins[1];
		dbg.analysisReadBefore = analysisReadBefore;
		dbg.analysisReadAfter = stft_.analysisReadPos;
		dbg.frameRmsL = debugFrameRms[0];
		dbg.frameRmsR = debugFrameRms[1];
		dbg.outputRmsL = debugOutputRms[0];
		dbg.outputRmsR = debugOutputRms[1];
		dbg.outputStartDeltaL = debugOutputStartDelta[0];
		dbg.outputStartDeltaR = debugOutputStartDelta[1];
		dbg.outputNormAtRead = normAtRead;
		dbg.previewOutL = previewOutL;
		dbg.previewOutR = previewOutR;
		if (stft_.identitySampleCount > 0)
		{
			dbg.identityRefRmsL = std::sqrt ((float) (stft_.identityRefSqAccum[0] / (double) stft_.identitySampleCount));
			dbg.identityRefRmsR = std::sqrt ((float) (stft_.identityRefSqAccum[1] / (double) stft_.identitySampleCount));
			dbg.identityErrRmsL = std::sqrt ((float) (stft_.identityErrSqAccum[0] / (double) stft_.identitySampleCount));
			dbg.identityErrRmsR = std::sqrt ((float) (stft_.identityErrSqAccum[1] / (double) stft_.identitySampleCount));
			dbg.identityMaxAbsErrL = stft_.identityMaxAbsErr[0];
			dbg.identityMaxAbsErrR = stft_.identityMaxAbsErr[1];
		}
		dbg.spectralFluxL = debugSpectralFlux[0];
		dbg.spectralFluxR = debugSpectralFlux[1];
		dbg.phaseResetMixL = debugPhaseResetMix[0];
		dbg.phaseResetMixR = debugPhaseResetMix[1];
		dbg.lockStrengthMeanL = debugLockStrengthMean[0];
		dbg.lockStrengthMeanR = debugLockStrengthMean[1];
		const double cycleDurationUs = juce::Time::highResolutionTicksToSeconds (
			juce::Time::getHighResolutionTicks() - cycleStartTicks) * 1000000.0;
		const double cycleBudgetUs = (currentSampleRate > 0.0 && synthesisHop > 0)
			? ((double) synthesisHop / currentSampleRate) * 1000000.0
			: 0.0;
		dbg.cycleDurationUs = (float) cycleDurationUs;
		dbg.cycleRealtimeCpuPct = (cycleBudgetUs > 0.0)
			? (float) ((cycleDurationUs / cycleBudgetUs) * 100.0)
			: 0.0f;
		dbg.forwardFftUs = (float) (juce::Time::highResolutionTicksToSeconds (forwardFftTicks) * 1000000.0);
		dbg.binAnalysisUs = (float) (juce::Time::highResolutionTicksToSeconds (binAnalysisTicks) * 1000000.0);
		dbg.pitchMapUs = (float) (juce::Time::highResolutionTicksToSeconds (pitchMapTicks) * 1000000.0);
		dbg.phaseLockUs = (float) (juce::Time::highResolutionTicksToSeconds (phaseLockTicks) * 1000000.0);
		dbg.ifftOlaUs = (float) (juce::Time::highResolutionTicksToSeconds (ifftOlaTicks) * 1000000.0);
		dbg.analysisLagSamples = analysisLagSamples;
		dbg.cyclesSinceReset = stft_.cyclesSinceReset;
		dbg.alignOn = fftDebugContext_.alignOn;
		dbg.pdcOn = fftDebugContext_.pdcOn;
		dbg.reportedLatency = fftDebugContext_.reportedLatency;
		dbg.dryDelayLen = fftDebugContext_.dryDelayLen;
		dbg.fftOutputPadLen = fftDebugContext_.fftOutputPadLen;
		dbg.smoothedWindow = fftDebugContext_.smoothedWindow;
		dbg.targetWindow = fftDebugContext_.targetWindow;
		dbg.windowTransitionActive = fftDebugContext_.windowTransitionActive;
		dbg.windowTransitionProgress = fftDebugContext_.windowTransitionProgress;
		dbg.fftOutputFadeActive = fftDebugContext_.fftOutputFadeActive;
		dbg.fftOutputFadeProgress = fftDebugContext_.fftOutputFadeProgress;
		dbg.fftWetPreWindowFadeL = fftDebugContext_.fftWetPreWindowFadeL;
		dbg.fftWetPostWindowFadeL = fftDebugContext_.fftWetPostWindowFadeL;
		dbg.fftWetPreOutputFadeL = fftDebugContext_.fftWetPreOutputFadeL;
		dbg.fftWetPostOutputFadeL = fftDebugContext_.fftWetPostOutputFadeL;
		dbg.fftWetPreWindowDeltaL = fftDebugContext_.fftWetPreWindowDeltaL;
		dbg.fftWetPostWindowDeltaL = fftDebugContext_.fftWetPostWindowDeltaL;
		dbg.fftWetPostOutputDeltaL = fftDebugContext_.fftWetPostOutputDeltaL;
		dbg.rawWindowChanged = fftDebugContext_.rawWindowChanged;
		dbg.rawAmountChanged = fftDebugContext_.rawAmountChanged;
		dbg.fftWindowMotionActive = fftDebugContext_.fftWindowMotionActive;
		dbg.fftWindowApplyDelayRemaining = fftDebugContext_.fftWindowApplyDelayRemaining;
		dbg.fftWindowCaptureRemaining = fftDebugContext_.fftWindowCaptureRemaining;
		dbg.fftDuckGain = fftDebugContext_.fftDuckGain;
		dbg.engineFadeOldOutL = fftDebugContext_.engineFadeOldOutL;
		dbg.engineFadeOldMix = fftDebugContext_.engineFadeOldMix;
		dbg.engineFadeNewMix = fftDebugContext_.engineFadeNewMix;
		dbg.fftOutputFadeOldOutL = fftDebugContext_.fftOutputFadeOldOutL;
		dbg.fftOutputFadeOldMix = fftDebugContext_.fftOutputFadeOldMix;
		dbg.fftOutputFadeNewMix = fftDebugContext_.fftOutputFadeNewMix;
		fftDebugTrace_.record (dbg);
	}
   #endif
	stft_.identityErrSqAccum[0] = 0.0;
	stft_.identityErrSqAccum[1] = 0.0;
	stft_.identityRefSqAccum[0] = 0.0;
	stft_.identityRefSqAccum[1] = 0.0;
	stft_.identityMaxAbsErr[0] = 0.0f;
	stft_.identityMaxAbsErr[1] = 0.0f;
	stft_.identitySampleCount = 0;
	++stft_.cyclesSinceReset;
}

// FFT Engine 3: Spectral Hold / Freeze
void STRETRAudioProcessor::performStftCycleSpectralHold (int fftSize, int synthesisHop,
                                                          float holdCoeff, float pitchRate, bool reverseOn,
                                                          bool freezeAnalysisInput,
                                                          float pitchRateR, bool wideMode)
{
	if (fft_ == nullptr || inputBufLen_ <= 0 || fftSize <= 0) return;
   #if JUCE_DEBUG
	const bool fftDebugEnabled = DeveloperDiagnosticsConfig::kEnableFftTraceRecording;
   #endif
   #if JUCE_DEBUG
	const auto cycleStartTicks = fftDebugEnabled ? juce::Time::getHighResolutionTicks() : juce::int64 { 0 };
   #endif

	const int   numBins    = fftSize / 2 + 1;
	const int   outBufLen  = kStftOutBufLen;
	const float twoPi      = juce::MathConstants<float>::twoPi;
	const float pi         = juce::MathConstants<float>::pi;
	const float expBase    = twoPi / (float) fftSize;
	const float blend      = 1.0f - holdCoeff;  // 1 = transparent, 0 = full freeze
	const float phaseAnalysisHop = reverseOn ? - (float) synthesisHop : (float) synthesisHop;
	const double analysisReadBefore = stft_.analysisReadPos;
#if STRETR_ENABLE_FFT1_CLICK_DUMP
	if constexpr (DeveloperDiagnosticsConfig::kEnableFft1AmountFreezeDump)
	{
		++fftDebugContext_.fftCycleSerial;
		fftDebugContext_.fftRuntimeRoute = 2;
		fftDebugContext_.signedAnalysisHop = freezeAnalysisInput ? 0 : (reverseOn ? -synthesisHop : synthesisHop);
		fftDebugContext_.freezeAnalysisInput = freezeAnalysisInput ? 1 : 0;
		fftDebugContext_.spectralHoldCoeff = holdCoeff;
		fftDebugContext_.analysisReadBefore = analysisReadBefore;
		fftDebugContext_.analysisReadAfter = analysisReadBefore;
	}
#endif
	int readStart = 0;
	if (inputBufLen_ > 0)
	{
		double wrappedReadPos = analysisReadBefore;
		while (wrappedReadPos >= (double) inputBufLen_)
			wrappedReadPos -= (double) inputBufLen_;
		while (wrappedReadPos < 0.0)
			wrappedReadPos += (double) inputBufLen_;
		readStart = ((int) std::floor (wrappedReadPos)) & inputBufMask_;
	}
   #if JUCE_DEBUG
	const float normAtRead = stft_.outputNormAccum[stft_.outputReadPos];
	const float invNormAtRead = (normAtRead > 1.0e-6f) ? (1.0f / normAtRead) : 0.0f;
	const float previewOutL = stft_.outputAccum[0][stft_.outputReadPos] * invNormAtRead;
	const float previewOutR = stft_.outputAccum[1][stft_.outputReadPos] * invNormAtRead;
	const double analysisLagSamples = (fftDebugEnabled && inputBufLen_ > 0)
		? std::fmod ((double) inputBufWritePos_ - analysisReadBefore + (double) inputBufLen_,
		             (double) inputBufLen_)
		: 0.0;
   #endif
	float debugFrameRms[2] = {};
	float debugOutputRms[2] = {};
	float debugOutputStartDelta[2] = {};

	for (int ch = 0; ch < 2; ++ch)
	{
		// DUAL: R channel uses pitchRateR if provided
		const float pr = (ch == 1 && pitchRateR > 0.0f) ? pitchRateR : pitchRate;
		const bool lowFft64 = (fftSize <= 64);
		const float lowFftPitchNorm = lowFft64
			? juce::jlimit (0.0f, 1.0f, std::abs (pr - 1.0f) / 3.0f)
			: 0.0f;
		const bool lowFftPartialHold = lowFft64 && (holdCoeff > 0.001f) && (holdCoeff < 0.999f);
		const float lowFftHoldMagBlend = lowFftPartialHold
			? blend * juce::jlimit (0.18f, 0.48f,
			                        0.34f - 0.10f * lowFftPitchNorm + 0.08f * blend)
			: blend;
		const float lowFftHoldFreqBlend = lowFftPartialHold
			? blend * juce::jlimit (0.10f, 0.30f,
			                        0.20f - 0.06f * lowFftPitchNorm + 0.06f * blend)
			: blend;
		float frameEnergy = 0.0f;
		bool heldStateSeededFromCurrentFrame = false;
		bool heldStateNeedsSeed = false;

		if (holdCoeff >= 0.999f)
		{
			float heldEnergy = 0.0f;
			float lastEnergy = 0.0f;
			for (int k = 0; k < numBins; ++k)
			{
				heldEnergy += std::abs (stft_.heldMag[ch][k]);
				lastEnergy += std::abs (stft_.lastMag[ch][k]);
			}

			if (heldEnergy <= 1.0e-4f)
			{
				if (lastEnergy > 1.0e-4f)
				{
					for (int k = 0; k < numBins; ++k)
					{
						stft_.heldMag[ch][k] = stft_.lastMag[ch][k];
						stft_.heldFreq[ch][k] = stft_.lastFreq[ch][k];
						stft_.synthPhase[ch][k] = stft_.prevPhase[ch][k];
					}
				}
				else
				{
					heldStateNeedsSeed = true;
				}
			}
		}

		if (! freezeAnalysisInput)
		{
	        // Analysis
			for (int j = 0; j < fftSize; ++j)
			{
				const int idx = (readStart + j) & inputBufMask_;
				fftWork_[j] = inputBuf_[ch][(size_t) idx] * fftWindow_[j];
				frameEnergy += fftWork_[j] * fftWork_[j];
			}
			for (int j = fftSize; j < fftSize * 2; ++j)
				fftWork_[j] = 0.0f;

			fft_->performRealOnlyForwardTransform (fftWork_, true);

			for (int k = 0; k < numBins; ++k)
			{
				const float re  = fftWork_[k * 2];
				const float im  = fftWork_[k * 2 + 1];
				const float mag = std::sqrt (re * re + im * im);
				const float ph  = std::atan2 (im, re);

				// Instantaneous frequency via phase difference
				float phaseDiff = ph - stft_.prevPhase[ch][k];
				stft_.prevPhase[ch][k] = ph;

				phaseDiff -= expBase * (float) k * phaseAnalysisHop;
				while (phaseDiff >  pi) phaseDiff -= twoPi;
				while (phaseDiff < -pi) phaseDiff += twoPi;
				const float freq = expBase * (float) k + phaseDiff / phaseAnalysisHop;

				// Spectral hold: blend new analysis into retained state
				if (heldStateNeedsSeed)
				{
					stft_.heldMag[ch][k] = mag;
					stft_.heldFreq[ch][k] = freq;
					heldStateSeededFromCurrentFrame = true;
				}
				else
				{
					if (lowFftPartialHold)
					{
						stft_.heldMag[ch][k]  = (1.0f - lowFftHoldMagBlend) * stft_.heldMag[ch][k]
						                      + lowFftHoldMagBlend * mag;
						stft_.heldFreq[ch][k] = (1.0f - lowFftHoldFreqBlend) * stft_.heldFreq[ch][k]
						                      + lowFftHoldFreqBlend * freq;
					}
					else
					{
						stft_.heldMag[ch][k]  = holdCoeff * stft_.heldMag[ch][k]  + blend * mag;
						stft_.heldFreq[ch][k] = holdCoeff * stft_.heldFreq[ch][k] + blend * freq;
					}
				}

	            // Keep lastMag/lastFreq current for clean FFT2 -> FFT1 transition
				stft_.lastMag[ch][k]  = mag;
				stft_.lastFreq[ch][k] = freq;
			}
			debugFrameRms[ch] = std::sqrt (frameEnergy / (float) juce::jmax (1, fftSize));
		}

		if (heldStateSeededFromCurrentFrame)
		{
			for (int k = 0; k < numBins; ++k)
				stft_.synthPhase[ch][k] = stft_.prevPhase[ch][k];
		}

        // Synthesis: use held magnitudes/frequencies
		const bool passthrough = (holdCoeff < 0.001f)
		                      && (std::abs (pr - 1.0f) <= 0.001f);

		float synthMag[kMaxFftBins];

		if (passthrough)
		{
			// Perfect reconstruction: use analysis phase directly
			for (int k = 0; k < numBins; ++k)
			{
				synthMag[k] = stft_.heldMag[ch][k];
				stft_.synthPhase[ch][k] = stft_.prevPhase[ch][k];
			}
		}
		else
		{
			const bool hardFrozenFrame = freezeAnalysisInput
				&& (holdCoeff >= 0.999f)
				&& ((fftSize <= 64) || (std::abs (pr - 1.0f) <= 0.001f));

			for (int k = 0; k < numBins; ++k)
			{
				float mag, freq;

				if (std::abs (pr - 1.0f) > 0.001f)
				{
					const float srcF = (float) k / pr;
					const int   s0   = (int) srcF;
					const float fr   = srcF - (float) s0;

					mag  = 0.0f;
					freq = expBase * (float) k;

					if (s0 >= 0 && s0 < numBins)
					{
						mag  += stft_.heldMag[ch][s0] * (1.0f - fr);
						const float freq0 = stft_.heldFreq[ch][s0];
						const float freq1 = (s0 + 1 < numBins) ? stft_.heldFreq[ch][s0 + 1] : freq0;
						freq = (freq0 + (freq1 - freq0) * fr) * pr;
					}
					if (s0 + 1 < numBins)
						mag += stft_.heldMag[ch][s0 + 1] * fr;
				}
				else
				{
					mag  = stft_.heldMag[ch][k];
					freq = stft_.heldFreq[ch][k];
				}

				synthMag[k] = mag;
				if (hardFrozenFrame)
				{
					stft_.synthPhase[ch][k] = stft_.prevPhase[ch][k];
					continue;
				}

				stft_.synthPhase[ch][k] += freq * (float) synthesisHop;

				// Blend synthPhase toward analysis phase to prevent
				// permanent phase offset after high-holdCoeff periods
				{
					float phDelta = std::remainder (stft_.prevPhase[ch][k] - stft_.synthPhase[ch][k], twoPi);
					const float phaseFollow = freezeAnalysisInput
						? 0.0f
						: lowFft64
						? juce::jlimit (lowFftPartialHold ? 0.24f : 0.18f,
						                lowFftPartialHold ? 0.52f : 0.42f,
						                (lowFftPartialHold ? 0.36f : 0.30f) + 0.10f * blend
						                    - (lowFftPartialHold ? 0.05f : 0.08f) * lowFftPitchNorm)
						: blend;
					stft_.synthPhase[ch][k] += phaseFollow * phDelta;
				}
			}
		}

		// Write complex output (only numBins used by performRealOnlyInverseTransform)
		for (int k = 0; k < numBins; ++k)
		{
            // WIDE: add linear phase ramp to R -> temporal shift of fftSize/2 samples
			float ph = stft_.synthPhase[ch][k];
			if (wideMode && ch == 1)
                ph += pi * (float) k;  // k * pi = half-window linear delay
			fftWork_[k * 2]     = synthMag[k] * std::cos (ph);
			fftWork_[k * 2 + 1] = synthMag[k] * std::sin (ph);
		}

		fft_->performRealOnlyInverseTransform (fftWork_);

		float outputEnergy = 0.0f;
		const int startOutIdx = stft_.outputReadPos & (outBufLen - 1);
		const float previousStart = stft_.outputAccum[ch][startOutIdx];
		for (int j = 0; j < fftSize; ++j)
		{
			const int outIdx = (stft_.outputReadPos + j) & (outBufLen - 1);
			const float windowSq = fftWindow_[j] * fftWindow_[j];
			const float outSample = fftWork_[j] * fftWindow_[j];
			if (j == 0)
				debugOutputStartDelta[ch] = outSample - previousStart;
			outputEnergy += outSample * outSample;
			stft_.outputAccum[ch][outIdx] += outSample;
			if (ch == 0)
				stft_.outputNormAccum[outIdx] += windowSq;
		}
		debugOutputRms[ch] = std::sqrt (outputEnergy / (float) juce::jmax (1, fftSize));
	}

	// Advance the analysis cursor in the selected direction so FFT2 can honor reverse mode.
	if (! freezeAnalysisInput && inputBufLen_ > 0)
	{
		stft_.hasFrame = true;
		const double dir = reverseOn ? -1.0 : 1.0;
		stft_.analysisReadPos = analysisReadBefore + (double) synthesisHop * dir;
		while (stft_.analysisReadPos >= (double) inputBufLen_)
			stft_.analysisReadPos -= (double) inputBufLen_;
		while (stft_.analysisReadPos < 0.0)
			stft_.analysisReadPos += (double) inputBufLen_;
	}
#if STRETR_ENABLE_FFT1_CLICK_DUMP
	if constexpr (DeveloperDiagnosticsConfig::kEnableFft1AmountFreezeDump)
		fftDebugContext_.analysisReadAfter = stft_.analysisReadPos;
#endif

   #if JUCE_DEBUG
	if (fftDebugEnabled)
	{
		FftDebugEntry dbg {};
		dbg.blockIndex = fftDebugContext_.blockIndex;
		dbg.sampleIndex = fftDebugContext_.sampleIndex;
		dbg.eventType = 0;
		dbg.engine = fftDebugContext_.engine;
		dbg.amount = fftDebugContext_.amount;
		dbg.pitch = fftDebugContext_.pitch;
		dbg.speed = fftDebugContext_.speed;
		dbg.pitchRate = pitchRate;
		dbg.windowSamples = fftDebugContext_.windowSamples;
		dbg.fftSize = fftSize;
		dbg.targetAnalysisHop = fftDebugContext_.targetAnalysisHop;
		dbg.filteredAnalysisHop = fftDebugContext_.filteredAnalysisHop;
		dbg.analysisHopQuantError = fftDebugContext_.analysisHopQuantError;
		dbg.lastAnalysisHop = fftDebugContext_.lastAnalysisHop;
		dbg.freezeEntryWarmupCycles = fftDebugContext_.freezeEntryWarmupCycles;
		dbg.fftStartupWarmupRemainingCycles = fftDebugContext_.fftStartupWarmupRemainingCycles;
		dbg.fftExplicitFreezeActive = fftDebugContext_.fftExplicitFreezeActive;
		dbg.fftExplicitFreezeCapturePending = fftDebugContext_.fftExplicitFreezeCapturePending;
		dbg.fftTargetFreeze = fftDebugContext_.fftTargetFreeze;
		dbg.analysisHop = (fftDebugContext_.analysisHopDebug >= 0)
			? fftDebugContext_.analysisHopDebug
			: synthesisHop;
		dbg.synthesisHop = synthesisHop;
		dbg.style = fftDebugContext_.style;
		dbg.reverseOn = reverseOn ? 1 : 0;
		dbg.triggerOn = fftDebugContext_.triggerOn;
		dbg.wideMode = wideMode ? 1 : 0;
		dbg.passthrough = ((holdCoeff < 0.001f) && (std::abs (pitchRate - 1.0f) <= 0.001f)) ? 1 : 0;
		dbg.analysisReadBefore = analysisReadBefore;
		dbg.analysisReadAfter = stft_.analysisReadPos;
		dbg.frameRmsL = debugFrameRms[0];
		dbg.frameRmsR = debugFrameRms[1];
		dbg.outputRmsL = debugOutputRms[0];
		dbg.outputRmsR = debugOutputRms[1];
		dbg.outputStartDeltaL = debugOutputStartDelta[0];
		dbg.outputStartDeltaR = debugOutputStartDelta[1];
		dbg.outputNormAtRead = normAtRead;
		dbg.previewOutL = previewOutL;
		dbg.previewOutR = previewOutR;
		if (stft_.identitySampleCount > 0)
		{
			dbg.identityRefRmsL = std::sqrt ((float) (stft_.identityRefSqAccum[0] / (double) stft_.identitySampleCount));
			dbg.identityRefRmsR = std::sqrt ((float) (stft_.identityRefSqAccum[1] / (double) stft_.identitySampleCount));
			dbg.identityErrRmsL = std::sqrt ((float) (stft_.identityErrSqAccum[0] / (double) stft_.identitySampleCount));
			dbg.identityErrRmsR = std::sqrt ((float) (stft_.identityErrSqAccum[1] / (double) stft_.identitySampleCount));
			dbg.identityMaxAbsErrL = stft_.identityMaxAbsErr[0];
			dbg.identityMaxAbsErrR = stft_.identityMaxAbsErr[1];
		}
		const double cycleDurationUs = juce::Time::highResolutionTicksToSeconds (
			juce::Time::getHighResolutionTicks() - cycleStartTicks) * 1000000.0;
		const double cycleBudgetUs = (currentSampleRate > 0.0 && synthesisHop > 0)
			? ((double) synthesisHop / currentSampleRate) * 1000000.0
			: 0.0;
		dbg.cycleDurationUs = (float) cycleDurationUs;
		dbg.cycleRealtimeCpuPct = (cycleBudgetUs > 0.0)
			? (float) ((cycleDurationUs / cycleBudgetUs) * 100.0)
			: 0.0f;
		dbg.analysisLagSamples = analysisLagSamples;
		dbg.cyclesSinceReset = stft_.cyclesSinceReset;
		dbg.alignOn = fftDebugContext_.alignOn;
		dbg.pdcOn = fftDebugContext_.pdcOn;
		dbg.reportedLatency = fftDebugContext_.reportedLatency;
		dbg.dryDelayLen = fftDebugContext_.dryDelayLen;
		dbg.fftOutputPadLen = fftDebugContext_.fftOutputPadLen;
		dbg.smoothedWindow = fftDebugContext_.smoothedWindow;
		dbg.targetWindow = fftDebugContext_.targetWindow;
		dbg.windowTransitionActive = fftDebugContext_.windowTransitionActive;
		dbg.windowTransitionProgress = fftDebugContext_.windowTransitionProgress;
		dbg.fftOutputFadeActive = fftDebugContext_.fftOutputFadeActive;
		dbg.fftOutputFadeProgress = fftDebugContext_.fftOutputFadeProgress;
		dbg.fftWetPreWindowFadeL = fftDebugContext_.fftWetPreWindowFadeL;
		dbg.fftWetPostWindowFadeL = fftDebugContext_.fftWetPostWindowFadeL;
		dbg.fftWetPreOutputFadeL = fftDebugContext_.fftWetPreOutputFadeL;
		dbg.fftWetPostOutputFadeL = fftDebugContext_.fftWetPostOutputFadeL;
		dbg.fftWetPreWindowDeltaL = fftDebugContext_.fftWetPreWindowDeltaL;
		dbg.fftWetPostWindowDeltaL = fftDebugContext_.fftWetPostWindowDeltaL;
		dbg.fftWetPostOutputDeltaL = fftDebugContext_.fftWetPostOutputDeltaL;
		dbg.rawWindowChanged = fftDebugContext_.rawWindowChanged;
		dbg.rawAmountChanged = fftDebugContext_.rawAmountChanged;
		dbg.fftWindowMotionActive = fftDebugContext_.fftWindowMotionActive;
		dbg.fftWindowApplyDelayRemaining = fftDebugContext_.fftWindowApplyDelayRemaining;
		dbg.fftWindowCaptureRemaining = fftDebugContext_.fftWindowCaptureRemaining;
		dbg.fftDuckGain = fftDebugContext_.fftDuckGain;
		dbg.engineFadeOldOutL = fftDebugContext_.engineFadeOldOutL;
		dbg.engineFadeOldMix = fftDebugContext_.engineFadeOldMix;
		dbg.engineFadeNewMix = fftDebugContext_.engineFadeNewMix;
		dbg.fftOutputFadeOldOutL = fftDebugContext_.fftOutputFadeOldOutL;
		dbg.fftOutputFadeOldMix = fftDebugContext_.fftOutputFadeOldMix;
		dbg.fftOutputFadeNewMix = fftDebugContext_.fftOutputFadeNewMix;
		fftDebugTrace_.record (dbg);
	}
   #endif
	stft_.identityErrSqAccum[0] = 0.0;
	stft_.identityErrSqAccum[1] = 0.0;
	stft_.identityRefSqAccum[0] = 0.0;
	stft_.identityRefSqAccum[1] = 0.0;
	stft_.identityMaxAbsErr[0] = 0.0f;
	stft_.identityMaxAbsErr[1] = 0.0f;
	stft_.identitySampleCount = 0;
	++stft_.cyclesSinceReset;
}

//==============================================================================
void STRETRAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages)
{
	juce::ScopedNoDenormals noDenormals;
	juce::ignoreUnused (midiMessages);

	const int numChannels = buffer.getNumChannels();
	const int numSamples  = buffer.getNumSamples();
	if (numChannels < 1 || numSamples < 1) return;

	float* channelL = buffer.getWritePointer (0);
	float* channelR = numChannels >= 2 ? buffer.getWritePointer (1) : nullptr;
#if JUCE_DEBUG
	const int debugBlockIndex = stretchDebugBlockCounter_++;
#else
	constexpr int debugBlockIndex = 0;
#endif

    // Read parameters
	const float inputGainDb  = loadAtomicOrDefault (inputParam, kInputDefault);
	const float outputGainDb = loadAtomicOrDefault (outputParam, kOutputDefault);
	const float mixValue     = loadAtomicOrDefault (mixParam, kMixDefault);
	const int   mixMode  = loadIntParamOrDefault (mixModeParam, kMixModeDefault);
	const float dryLevelTarget = (mixMode == 1) ? loadAtomicOrDefault (dryLevelParam, kDryLevelDefault) : smoothedDryLevel;
	const float wetLevelTarget = (mixMode == 1) ? loadAtomicOrDefault (wetLevelParam, kWetLevelDefault) : smoothedWetLevel;

	// Filter / Tilt position
	{
		const int fltPos = loadIntParamOrDefault (filterPosParam, kFilterPosDefault);
        // 0=F-post T-post  1=F-pre T-pre  2=F-pre T-post  3=F-post T-pre
		filterPre_ = (fltPos == 1 || fltPos == 2);
		tiltPre_   = (fltPos == 1 || fltPos == 3);
	}

	const int modeInVal  = loadIntParamOrDefault (modeInParam,  kModeInOutDefault);
	const int modeOutVal = loadIntParamOrDefault (modeOutParam, kModeInOutDefault);
	const int sumBusVal  = loadIntParamOrDefault (sumBusParam,  kSumBusDefault);
	const int invPol     = loadIntParamOrDefault (invPolParam,  kInvPolDefault);
	const int invStr     = loadIntParamOrDefault (invStrParam,  kInvStrDefault);

	const float targetInputGain  = gainFaderDecibelsToGain (inputGainDb);
	const float targetOutputGain = gainFaderDecibelsToGain (outputGainDb);

    // Limiter
	const int limMode = loadIntParamOrDefault (limModeParam, kLimModeDefault);
	const float limThreshLinTarget = (limMode != 0)
		? fastDecibelsToGain (loadAtomicOrDefault (limThresholdParam, kLimThresholdDefault))
		: 1.0f;

	const float panValue = loadAtomicOrDefault (panParam, kPanDefault);
	tiltDb_ = loadAtomicOrDefault (tiltParam, kTiltDefault);

	// Wet-signal filter params
	wetFilterHpOn_ = loadBoolParamOrDefault (filterHpOnParam, false);
	wetFilterLpOn_ = loadBoolParamOrDefault (filterLpOnParam, false);
	wetFilterTargetHpFreq_ = loadAtomicOrDefault (filterHpFreqParam, kFilterHpFreqDefault);
	wetFilterTargetLpFreq_ = loadAtomicOrDefault (filterLpFreqParam, kFilterLpFreqDefault);

	const int hpSlope = loadIntParamOrDefault (filterHpSlopeParam, kFilterSlopeDefault);
	const int lpSlope = loadIntParamOrDefault (filterLpSlopeParam, kFilterSlopeDefault);
	wetFilterNumSectionsHp_ = (hpSlope == 2) ? 2 : 1;
	wetFilterNumSectionsLp_ = (lpSlope == 2) ? 2 : 1;

	// Chaos params
	chaosFilterEnabled_ = loadBoolParamOrDefault (chaosParam, false);
	chaosDelayEnabled_  = loadBoolParamOrDefault (chaosDelayParam, false);

    // Engine params
	const int   engineVal  = loadIntParamOrDefault (engineParam, 0);
	const float amountVal  = loadAtomicOrDefault (amountParam, kAmountDefault);   // 0..100
	const float pitchVal     = loadAtomicOrDefault (pitchParam, kPitchDefault);         // 0..1
	const float jitterTarget = juce::jlimit (0.0f, 1.0f,
	                                         loadAtomicOrDefault (jitterParam, kJitterDefault) * 0.01f);
	const int   rawWindowParamVal = juce::jlimit (kWindowMin, kWindowMax,
	                                              loadIntParamOrDefault (windowParam, (int) kWindowDefault));
	const int   maxFftWindowVal = getCurrentMaxFftWindow();
	const int   styleVal   = loadIntParamOrDefault (styleParam, 1);
	const float grainMsTarget = juce::jlimit (kGrainMin, kGrainMax,
		loadAtomicOrDefault (grainParam, kGrainDefault)); // 1..500 ms
	const float grainTargetLogMs = std::log (grainMsTarget);
	const float grainSizeBlockStep = 1.0f - std::exp (- (float) numSamples
		/ (juce::jmax (1.0f, (float) currentSampleRate) * kGrainSizeSmoothSeconds));
	smoothedGrainLogMs_ += (grainTargetLogMs - smoothedGrainLogMs_) * grainSizeBlockStep;
	const float grainMs = std::exp (smoothedGrainLogMs_);
	const bool  reverseOn  = loadBoolParamOrDefault (reverseParam, false);
	const bool  triggerOn  = loadBoolParamOrDefault (triggerParam, false);
	const bool  alignOn    = loadBoolParamOrDefault (alignParam, false);
	const bool  pdcOn      = loadBoolParamOrDefault (pdcParam, false);
	const int previousEngineVal = prevEngineVal_;
	const bool engineChangedThisBlock = previousEngineVal >= 0 && engineVal != previousEngineVal;
	if (! windowFamiliesInitialised_.load (std::memory_order_relaxed))
		initialiseWindowFamilies (rawWindowParamVal);
	clampFftWindowFamiliesToMax (maxFftWindowVal);
	const WindowFamily windowFamily = getWindowFamilyForEngineInternal (engineVal);
	const int previousWindowFamily = activeWindowFamily_.load (std::memory_order_relaxed);
	const int previousObservedWindowParam = lastObservedWindowParam_.load (std::memory_order_relaxed);
	int windowVal = getStoredWindowForFamily (windowFamily);
	if ((int) windowFamily != previousWindowFamily)
	{
		activeWindowFamily_.store ((int) windowFamily, std::memory_order_relaxed);
		if (rawWindowParamVal != previousObservedWindowParam)
		{
			windowVal = getCanonicalWindowForFamily (windowFamily, rawWindowParamVal);
			setStoredWindowForFamily (windowFamily, windowVal);
		}
		lastObservedWindowParam_.store (rawWindowParamVal, std::memory_order_relaxed);
	}
	else if (rawWindowParamVal != previousObservedWindowParam)
	{
		windowVal = getCanonicalWindowForFamily (windowFamily, rawWindowParamVal);
		setStoredWindowForFamily (windowFamily, windowVal);
		lastObservedWindowParam_.store (rawWindowParamVal, std::memory_order_relaxed);
	}
	int effectiveWindowVal = windowVal;
	bool fftCapturedWindowChanged = false;
	if (engineVal == 2 || engineVal == 3)
	{
		if (triggerOn)
		{
			const int captureWindowSamples = (engineVal == 3) ? samplesForMs (6.0) : samplesForMs (4.0);
			fftPendingWindowVal_ = windowVal;
			if (fftWindowCaptureRemaining_ > 0)
			{
				fftWindowCaptureRemaining_ = juce::jmax (0, fftWindowCaptureRemaining_ - numSamples);
			}
			else if (fftCapturedWindowVal_ != fftPendingWindowVal_)
			{
				fftWindowCaptureRemaining_ = captureWindowSamples;
			}
			if (fftWindowCaptureRemaining_ == 0 && fftCapturedWindowVal_ != fftPendingWindowVal_)
			{
				fftCapturedWindowVal_ = fftPendingWindowVal_;
				fftCapturedWindowChanged = true;
			}
			effectiveWindowVal = fftCapturedWindowVal_;
		}
		else
		{
			fftWindowCaptureRemaining_ = 0;
			fftCapturedWindowVal_ = windowVal;
			fftPendingWindowVal_ = windowVal;
			effectiveWindowVal = windowVal;
		}
	}
	else
	{
		fftWindowCaptureRemaining_ = 0;
		fftCapturedWindowVal_ = windowVal;
		fftPendingWindowVal_ = windowVal;
	}
	const bool  fftEngineSelected = (engineVal == 2 || engineVal == 3);
	const bool  fftDuckEngineActive = triggerOn && fftEngineSelected;
#if STRETR_ENABLE_FFT1_CLICK_DUMP
	const bool  fftTriggerPressedThisBlock = triggerOn && ! triggerWasOn_ && fftEngineSelected;
	const bool  fftAmountMovedThisBlockForDump = fftEngineSelected
		&& (prevFftDuckEngineVal_ == engineVal)
		&& (std::abs (amountVal - prevFftDuckAmountVal_) > 0.001f);
#endif
	const bool  fftTriggerReleasedThisBlock = (! triggerOn) && triggerWasOn_ && fftEngineSelected;
	const bool  fftRawWindowChanged = fftDuckEngineActive
		&& prevFftDuckTriggerOn_
		&& (prevFftDuckEngineVal_ == engineVal)
		&& (windowVal != prevFftDuckWindowVal_);
	const bool  fftRawAmountChanged = fftDuckEngineActive
		&& prevFftDuckTriggerOn_
		&& (prevFftDuckEngineVal_ == engineVal)
		&& (std::abs (amountVal - prevFftDuckAmountVal_) > 0.001f);
	if (fftRawWindowChanged)
	{
		fftWindowTraceRemaining_ = juce::jmax (fftWindowTraceRemaining_,
			engineVal == 3 ? samplesForMs (20.0) : samplesForMs (14.0));
		if (engineVal == 2 || engineVal == 3)
		{
			const int duckHoldSamples = samplesForMs (engineVal == 3 ? 240.0 : 180.0);
			fftParamDuckHoldRemaining_ = juce::jmax (fftParamDuckHoldRemaining_, duckHoldSamples);
		}
		const int applyDelaySamples = (engineVal == 3) ? samplesForMs (24.0) : samplesForMs (14.0);
		fftWindowApplyDelayRemaining_ = juce::jmax (fftWindowApplyDelayRemaining_, applyDelaySamples);
	}
	if (fftRawAmountChanged)
	{
		fftAmountTraceRemaining_ = juce::jmax (fftAmountTraceRemaining_,
			engineVal == 3 ? samplesForMs (18.0) : samplesForMs (14.0));
	}
	else if (! fftEngineSelected)
	{
		clearFftWindowDuckRuntimeState();
	}
	else if (! triggerOn)
	{
		fftWindowApplyDelayRemaining_ = 0;
		fftWindowCaptureRemaining_ = 0;
		fftWindowTraceRemaining_ = 0;
		fftAmountTraceRemaining_ = 0;
		fft1ReentryTraceRemaining_ = 0;
		fftParamDuckHoldRemaining_ = 0;
	}
	prevFftDuckWindowVal_ = windowVal;
	prevFftDuckAmountVal_ = amountVal;
	prevFftDuckEngineVal_ = engineVal;
	prevFftDuckTriggerOn_ = triggerOn;

    // Amount -> speed: 0%=1.0 (no stretch), 100%=minimum speed.
    // FFT engines keep a tiny floor so the endpoint does not enter hard hold/freeze.
	const float targetSpeed = amountToSpeedForEngine (engineVal, amountVal);

    // Pitch -> playback rate: center (0.5)=1.0x, 0=-24st, 1=+24st.
	const float targetPitchRate = std::exp2 ((pitchVal - 0.5f) * 4.0f);

	// Window -> stored per engine; FFT engines receive canonical powers of two.
	smoothedWindow_ = smoothedWindowByFamily_[(size_t) windowFamily];
	const float targetWindow = (float) juce::jlimit (kWindowMin, kWindowMax, effectiveWindowVal);
	const float fft2TargetLog2Window = std::log2 (targetWindow);
	if (engineVal == 3 && triggerOn)
	{
		const float fft2BlockWindowStep = 1.0f - std::exp (- (float) numSamples
			/ (static_cast<float> (currentSampleRate) * 0.045f));
		fft2GeometryLog2Window_ += (fft2TargetLog2Window - fft2GeometryLog2Window_) * fft2BlockWindowStep;
	}
	else
	{
		fft2GeometryLog2Window_ = fft2TargetLog2Window;
	}
	const int windowSamples = juce::jlimit (kWindowMin, kWindowMax, (int) std::lround (smoothedWindow_));
	const int fft2GeometryWindowSamples = juce::jlimit (kWindowMin, kWindowMax,
	                                                    (int) std::lround (std::exp2 (fft2GeometryLog2Window_)));
	const bool fftEngineActive = (engineVal == 2 || engineVal == 3);
	bool fftWindowMotionActiveBlock = false;
	int requestedFftSize = 0;
	int desiredFftSizeForDump = 0;
	int previousFftSizeForDump = stft_.activeFftSize;
	bool fftSizeChanged = false;
	if (fftEngineActive && inputBufLen_ > 0)
	{
		const int fftWindowSamples = (engineVal == 3) ? fft2GeometryWindowSamples : windowSamples;
		const int desiredFftSize = juce::jlimit (64, kMaxFftSize, nextPowerOf2 (fftWindowSamples));
		const int previousFftSize = stft_.activeFftSize;
		desiredFftSizeForDump = desiredFftSize;
		previousFftSizeForDump = previousFftSize;
		const bool delayWindowApply = triggerOn && (previousFftSize > 0) && (fftWindowApplyDelayRemaining_ > 0);
		if (triggerOn)
			ensureFft (desiredFftSize);
		requestedFftSize = delayWindowApply ? previousFftSize : desiredFftSize;
		fftSizeChanged = triggerOn && ! delayWindowApply && (requestedFftSize != previousFftSize);
		if (fftWindowApplyDelayRemaining_ > 0)
			fftWindowApplyDelayRemaining_ = juce::jmax (0, fftWindowApplyDelayRemaining_ - numSamples);
		const int windowTransitionRemaining = getWindowTransitionRemainingForEngine (engineVal);
		fftWindowMotionActiveBlock = fftDuckEngineActive
			&& (fftRawWindowChanged
			    || fftWindowCaptureRemaining_ > 0
			    || fftWindowApplyDelayRemaining_ > 0
			    || windowTransitionRemaining > 0);
	}

	// Grain size in samples
	const int grainSamples = juce::jmax (4, (int) (grainMs * 0.001f * (float) currentSampleRate));

	float dryLevelState = smoothedDryLevel;
	float wetLevelState = smoothedWetLevel;
	const float limThreshLinStart = smoothedLimThreshold;
	float limThreshLinState = smoothedLimThreshold;

	// Granular read rate computed per-sample from smoothedSpeed_ below
   #if JUCE_DEBUG
	const bool fftDebugEnabled = DeveloperDiagnosticsConfig::kEnableFftTraceRecording;

	auto populateFftMotionTraceFields = [&] (FftDebugEntry& dbg)
	{
		dbg.rawWindowChanged = fftRawWindowChanged ? 1 : 0;
		dbg.rawAmountChanged = fftRawAmountChanged ? 1 : 0;
		dbg.fftWindowMotionActive = fftWindowMotionActiveBlock ? 1 : 0;
		dbg.fftWindowApplyDelayRemaining = fftWindowApplyDelayRemaining_;
		dbg.fftWindowCaptureRemaining = fftWindowCaptureRemaining_;
		dbg.fftDuckGain = fftParamDuckGain_;
	};

	auto recordFftReset = [&] (int eventType, double capturePos)
	{
		if (! fftDebugEnabled || requestedFftSize <= 0)
			return;

		FftDebugEntry dbg {};
		dbg.blockIndex = debugBlockIndex;
		dbg.sampleIndex = 0;
		dbg.eventType = eventType;
		dbg.engine = engineVal;
		dbg.amount = amountVal;
		dbg.pitch = pitchVal;
		dbg.speed = targetSpeed;
		dbg.pitchRate = targetPitchRate;
		dbg.windowSamples = (engineVal == 3) ? fft2GeometryWindowSamples : windowSamples;
		dbg.fftSize = requestedFftSize;
		dbg.style = styleVal;
		dbg.reverseOn = reverseOn ? 1 : 0;
		dbg.triggerOn = triggerOn ? 1 : 0;
		dbg.wideMode = (styleVal == 2 && numChannels >= 2) ? 1 : 0;
		dbg.analysisReadBefore = capturePos;
		dbg.analysisReadAfter = stft_.analysisReadPos;
		dbg.analysisLagSamples = (inputBufLen_ > 0)
			? std::fmod ((double) inputBufWritePos_ - capturePos + (double) inputBufLen_,
			             (double) inputBufLen_)
			: 0.0;
		dbg.cyclesSinceReset = stft_.cyclesSinceReset;
		dbg.alignOn = alignOn ? 1 : 0;
		dbg.pdcOn = pdcOn ? 1 : 0;
		const int requestedFftSynthHop = recommendedFftSynthHop (requestedFftSize);
		const int requestedFftWetLatency = requestedFftSize + requestedFftSynthHop;
		dbg.reportedLatency = (pdcOn && requestedFftSize > 0) ? requestedFftWetLatency : 0;
		dbg.dryDelayLen = (alignOn && requestedFftSize > 0)
			? juce::jmin (requestedFftWetLatency, kDryDelayBufLen - 1)
			: 0;
		dbg.fftOutputPadLen = 0;
		populateFftMotionTraceFields (dbg);
		fftDebugTrace_.record (dbg);
	};

	auto recordFftControlEvent = [&] (int eventType)
	{
		if (! fftDebugEnabled || requestedFftSize <= 0)
			return;

		FftDebugEntry dbg {};
		dbg.blockIndex = debugBlockIndex;
		dbg.sampleIndex = 0;
		dbg.eventType = eventType;
		dbg.engine = engineVal;
		dbg.amount = amountVal;
		dbg.pitch = pitchVal;
		dbg.speed = targetSpeed;
		dbg.pitchRate = targetPitchRate;
		dbg.windowSamples = (engineVal == 3) ? fft2GeometryWindowSamples : windowSamples;
		dbg.fftSize = requestedFftSize;
		dbg.style = styleVal;
		dbg.reverseOn = reverseOn ? 1 : 0;
		dbg.triggerOn = triggerOn ? 1 : 0;
		dbg.wideMode = (styleVal == 2 && numChannels >= 2) ? 1 : 0;
		dbg.analysisReadBefore = stft_.analysisReadPos;
		dbg.analysisReadAfter = stft_.analysisReadPos;
		dbg.cyclesSinceReset = stft_.cyclesSinceReset;
		dbg.alignOn = alignOn ? 1 : 0;
		dbg.pdcOn = pdcOn ? 1 : 0;
		dbg.reportedLatency = 0;
		dbg.dryDelayLen = dryDelayLen_;
		dbg.fftOutputPadLen = 0;
		dbg.smoothedWindow = (engineVal == 3) ? (float) fft2GeometryWindowSamples : smoothedWindow_;
		dbg.targetWindow = targetWindow;
		populateFftMotionTraceFields (dbg);
		fftDebugTrace_.record (dbg);
	};
   #else
	constexpr bool fftDebugEnabled = false;
	auto recordFftReset = [&] (int, double) {};
	auto recordFftControlEvent = [&] (int) {};
   #endif

	auto populateFftDebugContextControlState = [&] (int reportedLatency, int fftOutputPadLen)
	{
		if (! fftDebugEnabled)
			return;

		fftDebugContext_.alignOn = alignOn ? 1 : 0;
		fftDebugContext_.pdcOn = pdcOn ? 1 : 0;
		fftDebugContext_.reportedLatency = reportedLatency;
		fftDebugContext_.dryDelayLen = dryDelayLen_;
		fftDebugContext_.fftOutputPadLen = fftOutputPadLen;
		fftDebugContext_.smoothedWindow = (engineVal == 3) ? (float) fft2GeometryWindowSamples : smoothedWindow_;
		fftDebugContext_.targetWindow = targetWindow;
		fftDebugContext_.rawWindowChanged = fftRawWindowChanged ? 1 : 0;
		fftDebugContext_.rawAmountChanged = fftRawAmountChanged ? 1 : 0;
		fftDebugContext_.fftWindowMotionActive = fftWindowMotionActiveBlock ? 1 : 0;
		fftDebugContext_.fftWindowApplyDelayRemaining = fftWindowApplyDelayRemaining_;
		fftDebugContext_.fftWindowCaptureRemaining = fftWindowCaptureRemaining_;
		fftDebugContext_.fftDuckGain = fftParamDuckGain_;
		fftDebugContext_.windowTransitionActive = isWindowTransitionActiveForEngine (engineVal) ? 1 : 0;
		fftDebugContext_.windowTransitionProgress = getWindowTransitionProgressForEngine (engineVal);
		fftDebugContext_.fftOutputFadeActive = ((engineVal == 2 || engineVal == 3) && fftOutputFadePos_ > 0 && fftOutputFadeTotal_ > 0) ? 1 : 0;
		fftDebugContext_.fftOutputFadeProgress = (fftOutputFadePos_ > 0 && fftOutputFadeTotal_ > 0)
			? juce::jlimit (0.0f, 1.0f, 1.0f - (float) fftOutputFadePos_ / (float) fftOutputFadeTotal_)
			: 1.0f;
	};

	if (fftRawWindowChanged)
		recordFftControlEvent (5);
	if (fftRawAmountChanged)
		recordFftControlEvent (6);

	bool stretchTransportRestartedThisBlock = false;
	bool fftTransportRestartedThisBlock = false;
	bool grainTransportRestartedThisBlock = false;
	if (auto* audioPlayHead = getPlayHead())
	{
		if (auto position = audioPlayHead->getPosition())
		{
			const bool isPlayingNow = position->getIsPlaying();
			const auto timeInSamples = position->getTimeInSamples();
			const bool hasSamplePos = timeInSamples.hasValue();
			const juce::int64 samplePos = hasSamplePos ? *timeInSamples : 0;
			const bool transportStartedThisBlock = isPlayingNow && ! transportWasPlaying_;
			const bool transportJumpedBackwardThisBlock = isPlayingNow
				&& transportHasSamplePos_
				&& hasSamplePos
				&& (samplePos + (juce::int64) numSamples < transportLastSamplePos_);
			const bool triggerTransportRestartedThisBlock = triggerOn
				&& (transportStartedThisBlock || transportJumpedBackwardThisBlock);
			stretchTransportRestartedThisBlock = triggerTransportRestartedThisBlock
				&& (engineVal == 0);
			fftTransportRestartedThisBlock = triggerTransportRestartedThisBlock
				&& (engineVal == 2 || engineVal == 3);
			grainTransportRestartedThisBlock = triggerTransportRestartedThisBlock
				&& (engineVal == 1);
			transportWasPlaying_ = isPlayingNow;
			if (hasSamplePos)
			{
				transportLastSamplePos_ = samplePos;
				transportHasSamplePos_ = true;
			}
			else if (! isPlayingNow)
			{
				transportHasSamplePos_ = false;
				transportLastSamplePos_ = 0;
			}
		}
		else
		{
			transportWasPlaying_ = false;
			transportHasSamplePos_ = false;
			transportLastSamplePos_ = 0;
		}
	}
	else
	{
		transportWasPlaying_ = false;
		transportHasSamplePos_ = false;
		transportLastSamplePos_ = 0;
	}

    // Trigger edge detection: reset engines on trigger press
	bool fftReseededThisBlock = false;
	auto armFftInstantDuck = [this] (int holdSamples) noexcept
	{
		fftParamDuckHoldRemaining_ = juce::jmax (fftParamDuckHoldRemaining_, holdSamples);
		fftParamDuckGain_ = 0.0f;
		const int bridgeSamples = juce::jlimit (0, holdSamples, samplesForMs (1.5));
		if (bridgeSamples > 0)
		{
			fftDuckBridgeStartL_ = std::isfinite (fftLastPostDuckOutL_) ? fftLastPostDuckOutL_ : 0.0f;
			fftDuckBridgeStartR_ = std::isfinite (fftLastPostDuckOutR_) ? fftLastPostDuckOutR_ : fftDuckBridgeStartL_;
			fftDuckBridgeTotal_ = bridgeSamples;
			fftDuckBridgeRemaining_ = bridgeSamples;
		}
	};
	if (triggerOn && ! triggerWasOn_)
	{
        // Trigger just pressed - capture current write position as starting read point
		const double capturePos = (double) ((inputBufWritePos_ - 1 + inputBufLen_) & inputBufMask_);
		resetWsolaAtPos (capturePos);
		wsolaUnityBypassActive_ = false;
		if ((engineVal == 2 || engineVal == 3) && requestedFftSize > 0)
		{
			const int triggerDuckHoldSamples = recommendedFftTriggerDuckHoldSamples (requestedFftSize);
			armFftInstantDuck (triggerDuckHoldSamples);
			resetStftAtPos (capturePos, requestedFftSize);
			recordFftReset (1, capturePos);
			fftReseededThisBlock = true;
		}
		const double captureAbsPos = currentCaptureAbsPos();
		resetGrainAtCapturePos (captureAbsPos, grainSamples, targetPitchRate, reverseOn, styleVal == 2 && numChannels >= 2);

#if JUCE_DEBUG
		GrainDebugEntry dbg {};
		dbg.blockIndex = debugBlockIndex;
		dbg.sampleIndex = 0;
		dbg.eventType = 1;
		dbg.amount = amountVal;
		dbg.pitch = pitchVal;
		dbg.speed = targetSpeed;
		dbg.pitchRate = targetPitchRate;
		dbg.windowSamples = windowSamples;
		dbg.grainSamples = grainSamples;
		dbg.style = styleVal;
		dbg.reverseOn = reverseOn ? 1 : 0;
		dbg.triggerOn = 1;
		dbg.activeGrains = 0;
		dbg.capturePos = captureAbsPos;
		dbg.readPosBefore = captureAbsPos;
		dbg.spawnPos = grainReadPos_;
		dbg.readPosAfter = grainReadPos_;
		dbg.lookBehind = computeGrainLookBehind (grainSamples, targetPitchRate, reverseOn, styleVal == 2 && numChannels >= 2);
		dbg.futureMargin = captureAbsPos - (grainReadPos_ + dbg.lookBehind - 2.0);
		grainDebugTrace_.record (dbg);
#endif
	}
	else if (fftTransportRestartedThisBlock && requestedFftSize > 0)
	{
		const double capturePos = (double) ((inputBufWritePos_ - 1 + inputBufLen_) & inputBufMask_);
		const int triggerDuckHoldSamples = recommendedFftTriggerDuckHoldSamples (requestedFftSize);
		armFftInstantDuck (triggerDuckHoldSamples);
		resetStftAtPos (capturePos, requestedFftSize);
		recordFftReset (1, capturePos);
		fftReseededThisBlock = true;
	}
	else if (stretchTransportRestartedThisBlock && inputBufLen_ > 0)
	{
		const double capturePos = (double) ((inputBufWritePos_ - 1 + inputBufLen_) & inputBufMask_);
		resetWsolaAtPos (capturePos);
		wsolaUnityBypassActive_ = false;
		stretchBootstrapSegments_ = 0;
		stretchTransitionRemaining_ = 0;
		stretchTransitionTotal_ = 0;
		stretchTransitionToUnity_ = false;

#if JUCE_DEBUG
		StretchDebugEntry dbg {};
		dbg.blockIndex = debugBlockIndex;
		dbg.sampleIndex = 0;
		dbg.eventType = 2;
		dbg.amount = amountVal;
		dbg.pitch = pitchVal;
		dbg.speed = targetSpeed;
		dbg.pitchRate = targetPitchRate;
		dbg.windowSamples = windowSamples;
		dbg.style = styleVal;
		dbg.reverseOn = reverseOn ? 1 : 0;
		dbg.triggerOn = 1;
		dbg.hasPrevTail = 0;
		dbg.nearUnity = 0;
		dbg.segInputStart = wsola_.segInputStart;
		stretchDebugTrace_.record (dbg);
#endif
	}
	else if (grainTransportRestartedThisBlock && inputBufLen_ > 0)
	{
		const double captureAbsPos = currentCaptureAbsPos();
		resetGrainAtCapturePos (captureAbsPos, grainSamples, targetPitchRate, reverseOn, styleVal == 2 && numChannels >= 2);

#if JUCE_DEBUG
		GrainDebugEntry dbg {};
		dbg.blockIndex = debugBlockIndex;
		dbg.sampleIndex = 0;
		dbg.eventType = 3;
		dbg.amount = amountVal;
		dbg.pitch = pitchVal;
		dbg.speed = targetSpeed;
		dbg.pitchRate = targetPitchRate;
		dbg.windowSamples = windowSamples;
		dbg.grainSamples = grainSamples;
		dbg.style = styleVal;
		dbg.reverseOn = reverseOn ? 1 : 0;
		dbg.triggerOn = 1;
		dbg.activeGrains = 0;
		dbg.capturePos = captureAbsPos;
		dbg.readPosBefore = captureAbsPos;
		dbg.spawnPos = grainReadPos_;
		dbg.readPosAfter = grainReadPos_;
		dbg.lookBehind = computeGrainLookBehind (grainSamples, targetPitchRate, reverseOn, styleVal == 2 && numChannels >= 2);
		dbg.futureMargin = captureAbsPos - (grainReadPos_ + dbg.lookBehind - 2.0);
		grainDebugTrace_.record (dbg);
#endif
	}
	if (fftTriggerReleasedThisBlock && requestedFftSize > 0)
		armFftInstantDuck (samplesForMs (2.0));
	triggerWasOn_ = triggerOn;

	// Engine crossfade: trigger fade-in on engine change
	if (engineChangedThisBlock)
	{
		if (previousEngineVal == 2)
		{
			if (triggerOn && ! fftUnityBypassActive_ && ! fftTransitionToUnity_)
				captureFft1FreezeSnapshot (styleVal, reverseOn);
			else
				clearFft1FreezeSnapshot();
		}

		engineFadeHoldSamples_ = samplesForMs ((engineVal == 2 || engineVal == 3) ? 80.0 : 25.0);
		engineFadeTotal_ = recommendedEngineCrossfadeSamples() + engineFadeHoldSamples_;
		engineFadePos_ = engineFadeTotal_;
		const int lastOutIdx = (wetOutputHistoryWritePos_ - 1 + kWetOutputHistoryLen) & (kWetOutputHistoryLen - 1);
		engineFadeStartL_ = wetOutputHistory_[0][lastOutIdx];
		engineFadeStartR_ = wetOutputHistory_[1][lastOutIdx];
		clearFftOutputFadeState();
		fft1ReentryTraceRemaining_ = (engineVal == 2 && triggerOn) ? samplesForMs (80.0) : 0;
		if (engineVal == 0 && inputBufLen_ > 0)
		{
			const double capturePos = (double) ((inputBufWritePos_ - 1 + inputBufLen_) & inputBufMask_);
			resetWsolaAtPos (capturePos);
			wsolaUnityBypassActive_ = false;
		}
		else if (engineVal == 1 && inputBufLen_ > 0)
		{
			const double captureAbsPos = currentCaptureAbsPos();
			resetGrainAtCapturePos (captureAbsPos, grainSamples, targetPitchRate, reverseOn, styleVal == 2 && numChannels >= 2);

#if JUCE_DEBUG
			GrainDebugEntry dbg {};
			dbg.blockIndex = debugBlockIndex;
			dbg.sampleIndex = 0;
			dbg.eventType = 2;
			dbg.amount = amountVal;
			dbg.pitch = pitchVal;
			dbg.speed = targetSpeed;
			dbg.pitchRate = targetPitchRate;
			dbg.windowSamples = windowSamples;
			dbg.grainSamples = grainSamples;
			dbg.style = styleVal;
			dbg.reverseOn = reverseOn ? 1 : 0;
			dbg.triggerOn = triggerOn ? 1 : 0;
			dbg.activeGrains = 0;
			dbg.capturePos = captureAbsPos;
			dbg.readPosBefore = captureAbsPos;
			dbg.spawnPos = grainReadPos_;
			dbg.readPosAfter = grainReadPos_;
			dbg.lookBehind = computeGrainLookBehind (grainSamples, targetPitchRate, reverseOn, styleVal == 2 && numChannels >= 2);
			dbg.futureMargin = captureAbsPos - (grainReadPos_ + dbg.lookBehind - 2.0);
			grainDebugTrace_.record (dbg);
#endif
		}
		else if (triggerOn && (engineVal == 2 || engineVal == 3) && requestedFftSize > 0)
		{
			const double capturePos = (double) ((inputBufWritePos_ - 1 + inputBufLen_) & inputBufMask_);
			resetStftAtPos (capturePos, requestedFftSize);
			if (engineVal == 2)
			{
				const bool fftTargetFreeze = false;
				if (canRestoreFft1FreezeSnapshot (requestedFftSize, styleVal, reverseOn,
				                                 triggerOn, fftTargetFreeze, targetSpeed))
				{
					restoreFft1FreezeSnapshot (fftTargetFreeze);
				}
			}
			recordFftReset (2, capturePos);
			fftReseededThisBlock = true;
		}
	}
	prevEngineVal_ = engineVal;

	if (! triggerOn || ! fftEngineActive)
	{
		clearFftOutputFadeState();
	}

	if (triggerOn && ! fftReseededThisBlock && fftSizeChanged && (engineVal == 2 || engineVal == 3) && requestedFftSize > 0)
	{
		const double capturePos = (double) ((inputBufWritePos_ - 1 + inputBufLen_) & inputBufMask_);
		const int fftWindowFadeSamples = (engineVal == 3)
			? recommendedFft2WindowCrossfadeSamples (stft_.activeFftSize, requestedFftSize)
			: recommendedFftWindowCrossfadeSamples();
		const int fftWindowHoldSamples = juce::jlimit (0, fftWindowFadeSamples - 1,
			engineVal == 3
				? juce::jmax (samplesForMs (32.0), fftWindowFadeSamples / 3)
				: samplesForMs (20.0));
		armFftInstantDuck (fftWindowHoldSamples);
		fftOutputFadeHoldSamples_ = fftWindowHoldSamples;
		fftOutputFadeTotal_ = fftWindowFadeSamples + fftOutputFadeHoldSamples_;
		fftOutputFadePos_ = fftOutputFadeTotal_;
		fftOutputFadeReadPos_ = (wetOutputHistoryWritePos_ - fftOutputFadeTotal_ + kWetOutputHistoryLen)
			& (kWetOutputHistoryLen - 1);
		if (engineVal == 3)
		{
			startWindowTransitionForEngine (engineVal, fftWindowFadeSamples);
			const bool fft2FreezeSettled = (smoothedSpeed_ <= 0.0001f)
			                            && (fft2HoldCoeffSmoothed_ >= 0.999f);
			resizeFft2StateAtPos (capturePos, requestedFftSize, fft2FreezeSettled);
		}
		else
		{
			startWindowTransitionForEngine (engineVal, fftWindowFadeSamples);
			resizeStftAtPos (capturePos, requestedFftSize);
		}
		recordFftReset (3, capturePos);
	}

	int fftOutputPadLen = 0;
	int reportedLatency = 0;

	// PDC and Align
	{
		// PDC/ALIGN are FFT-only. GRAIN/STRETCH look-behind is part of the
		// effect behavior and must not move host timing or the dry path.
		const int fftLatencySize = (engineVal == 2 || engineVal == 3) ? maxFftWindowVal : 0;
		const int fftSynthHopForAlign = (fftLatencySize > 0) ? recommendedFftSynthHop (fftLatencySize) : 0;
		const int fftWetLatency = (fftLatencySize > 0) ? (fftLatencySize + fftSynthHopForAlign) : 0;
		const int activeFftLatency = fftWetLatency;
		reportedLatency = (pdcOn && activeFftLatency > 0) ? activeFftLatency : 0;
		if (reportedLatency != lastReportedLatency_)
		{
			setLatencySamples (reportedLatency);
			lastReportedLatency_ = reportedLatency;
		}
		dryDelayLen_ = (alignOn && activeFftLatency > 0)
			? juce::jmin (activeFftLatency, kDryDelayBufLen - 1)
			: 0;
		fftOutputPadLen = 0;
	}

	if (chaosFilterEnabled_)
	{
		chaosAmtF_ = juce::jlimit (kChaosAmtMin, kChaosAmtMax,
			loadAtomicOrDefault (chaosAmtFilterParam, kChaosAmtDefault));
		const float spd = juce::jlimit (kChaosSpdMin, kChaosSpdMax,
			loadAtomicOrDefault (chaosSpdFilterParam, kChaosSpdDefault));
		chaosShPeriodF_ = (float) currentSampleRate / spd;
		chaosFilterMaxOct_ = chaosAmtF_ * 0.02f;
	}
	else
	{
		chaosAmtF_ = 0.0f;
		chaosFilterMaxOct_ = 0.0f;
		chaosFilterAmtSmoothed_ = 0.0f;
		chaosFilterSpdSmoothed_ = kChaosSpdDefault;
		chaosFilterParamSmoothReady_ = false;
	}
	if (chaosDelayEnabled_)
	{
		chaosAmtD_ = juce::jlimit (kChaosAmtMin, kChaosAmtMax,
			loadAtomicOrDefault (chaosAmtParam, kChaosAmtDefault));
		chaosAmtNormD_ = chaosAmtD_ * 0.01f;
		const float spd = juce::jlimit (kChaosSpdMin, kChaosSpdMax,
			loadAtomicOrDefault (chaosSpdParam, kChaosSpdDefault));
		chaosShPeriodD_ = (float) currentSampleRate / spd;
		chaosDelayMaxSamples_ = chaosAmtNormD_ * 0.005f * static_cast<float> (currentSampleRate);
		chaosGainMaxDb_       = chaosAmtNormD_;
	}
	else
	{
		chaosAmtD_ = 0.0f;
		chaosAmtNormD_ = 0.0f;
		chaosDelayMaxSamples_ = 0.0f;
		chaosGainMaxDb_ = 0.0f;
		chaosDriveAmtSmoothed_ = 0.0f;
		chaosDriveSpdSmoothed_ = kChaosSpdDefault;
		chaosDriveParamSmoothReady_ = false;
		chaosDelaySmoothedSamples_[0] = chaosDelaySmoothedSamples_[1] = 0.0f;
		chaosDelaySmoothReady_[0] = chaosDelaySmoothReady_[1] = false;
	}
	if (chaosFilterEnabled_ || chaosDelayEnabled_)
		chaosParamSmoothCoeff_ = cachedChaosParamSmoothCoeff_;
	else
		chaosParamSmoothCoeff_ = cachedChaosParamSmoothCoeff_;

	chaosStereo_ = (styleVal >= 1);
#if STRETR_ENABLE_FFT1_CLICK_DUMP
	const bool fft1AmountFreezeDumpActiveBlock = DeveloperDiagnosticsConfig::kEnableFft1AmountFreezeDump
		&& (engineVal == 2 || engineVal == 3)
		&& (triggerOn
		    || fftTriggerReleasedThisBlock
		    || fftAmountMovedThisBlockForDump
		    || fftAmountTraceRemaining_ > 0
		    || fftParamDuckHoldRemaining_ > 0
		    || fftParamDuckGain_ < 0.9999f
		    || fftOutputFadePos_ > 0);
	const int fft1AmountFreezeDumpBlockIndex = ++fft1AmountFreezeDumpBlockCounter_;
	const int fft1AmountFreezeDumpTriggerEdge = fftTriggerPressedThisBlock ? 1 : (fftTriggerReleasedThisBlock ? -1 : 0);
	const int fft1AmountFreezeDumpDuckHoldStart = fftParamDuckHoldRemaining_;
	const float fft1AmountFreezeDumpDuckGainStart = fftParamDuckGain_;
	const int fft1AmountFreezeDumpCycleSerialStart = fftDebugContext_.fftCycleSerial;
	double fft1AmountFreezeEngineWetSqL = 0.0;
	double fft1AmountFreezeEngineWetSqR = 0.0;
	double fft1AmountFreezeFinalWetSqL = 0.0;
	double fft1AmountFreezeFinalWetSqR = 0.0;
	double fft1AmountFreezeOutSqL = 0.0;
	double fft1AmountFreezeOutSqR = 0.0;
	double fft1AmountFreezePostDuckOutSqL = 0.0;
	double fft1AmountFreezePostDuckOutSqR = 0.0;
	float fft1AmountFreezeEngineWetPeakL = 0.0f;
	float fft1AmountFreezeEngineWetPeakR = 0.0f;
	float fft1AmountFreezeFinalWetPeakL = 0.0f;
	float fft1AmountFreezeFinalWetPeakR = 0.0f;
	float fft1AmountFreezeOutPeakL = 0.0f;
	float fft1AmountFreezeOutPeakR = 0.0f;
	float fft1AmountFreezePostDuckOutPeakL = 0.0f;
	float fft1AmountFreezePostDuckOutPeakR = 0.0f;
	int fft1AmountFreezeMaxFftWetDeltaSample = -1;
	float fft1AmountFreezeMaxFftWetAbsDeltaL = 0.0f;
	float fft1AmountFreezeMaxFftWetAbsDeltaR = 0.0f;
	float fft1AmountFreezeMaxFftWetPrevL = 0.0f;
	float fft1AmountFreezeMaxFftWetPrevR = 0.0f;
	float fft1AmountFreezeMaxFftWetCurrL = 0.0f;
	float fft1AmountFreezeMaxFftWetCurrR = 0.0f;
	float fft1AmountFreezeMaxFftWetNorm = 0.0f;
	int fft1AmountFreezeMaxFftWetOutputReadPos = -1;
	int fft1AmountFreezeMaxFftWetSynthCounter = -1;
	int fft1AmountFreezeMaxEngineWetDeltaSample = -1;
	float fft1AmountFreezeMaxEngineWetAbsDeltaL = 0.0f;
	float fft1AmountFreezeMaxEngineWetAbsDeltaR = 0.0f;
	float fft1AmountFreezeMaxEngineWetPrevL = 0.0f;
	float fft1AmountFreezeMaxEngineWetPrevR = 0.0f;
	float fft1AmountFreezeMaxEngineWetCurrL = 0.0f;
	float fft1AmountFreezeMaxEngineWetCurrR = 0.0f;
	int fft1AmountFreezeMaxFinalWetDeltaSample = -1;
	float fft1AmountFreezeMaxFinalWetAbsDeltaL = 0.0f;
	float fft1AmountFreezeMaxFinalWetAbsDeltaR = 0.0f;
	float fft1AmountFreezeMaxFinalWetPrevL = 0.0f;
	float fft1AmountFreezeMaxFinalWetPrevR = 0.0f;
	float fft1AmountFreezeMaxFinalWetCurrL = 0.0f;
	float fft1AmountFreezeMaxFinalWetCurrR = 0.0f;
	int fft1AmountFreezeMaxOutDeltaSample = -1;
	float fft1AmountFreezeMaxOutAbsDeltaL = 0.0f;
	float fft1AmountFreezeMaxOutAbsDeltaR = 0.0f;
	float fft1AmountFreezeMaxOutPrevL = 0.0f;
	float fft1AmountFreezeMaxOutPrevR = 0.0f;
	float fft1AmountFreezeMaxOutCurrL = 0.0f;
	float fft1AmountFreezeMaxOutCurrR = 0.0f;
	int fft1AmountFreezeMaxPostDuckOutDeltaSample = -1;
	float fft1AmountFreezeMaxPostDuckOutAbsDeltaL = 0.0f;
	float fft1AmountFreezeMaxPostDuckOutAbsDeltaR = 0.0f;
	float fft1AmountFreezeMaxPostDuckOutPrevL = 0.0f;
	float fft1AmountFreezeMaxPostDuckOutPrevR = 0.0f;
	float fft1AmountFreezeMaxPostDuckOutCurrL = 0.0f;
	float fft1AmountFreezeMaxPostDuckOutCurrR = 0.0f;
	StretrDumpStageDelta fft1AmountFreezeMaxPreStyleWetDelta;
	StretrDumpStageDelta fft1AmountFreezeMaxPostStyleWetDelta;
	StretrDumpStageDelta fft1AmountFreezeMaxPostFilterWetDelta;
	StretrDumpStageDelta fft1AmountFreezeMaxPostChaosWetDelta;
	StretrDumpStageDelta fft1AmountFreezeMaxPreDcWetDelta;
	StretrDumpStageDelta fft1AmountFreezeMaxPostDcWetDelta;
	float fft1AmountFreezeMaxPostDcPrevDcInL = 0.0f;
	float fft1AmountFreezeMaxPostDcPrevDcInR = 0.0f;
	float fft1AmountFreezeMaxPostDcPrevDcOutL = 0.0f;
	float fft1AmountFreezeMaxPostDcPrevDcOutR = 0.0f;
	float fft1AmountFreezeMaxPostDcInputL = 0.0f;
	float fft1AmountFreezeMaxPostDcInputR = 0.0f;
#endif
	float fftDuckAttackStepBlock = 0.0f;
	float fftDuckReleaseStepBlock = 0.0f;
	if (engineVal == 2 || engineVal == 3)
	{
		const float attackMs = 4.0f;
		const float releaseSeconds = (engineVal == 3) ? 0.320f : 0.250f;
		fftDuckAttackStepBlock = 1.0f - std::exp (-1.0f / (static_cast<float> (currentSampleRate) * attackMs * 0.001f));
		fftDuckReleaseStepBlock = 1.0f - std::exp (-1.0f / (static_cast<float> (currentSampleRate) * releaseSeconds));
	}

    // Per-sample processing
	for (int i = 0; i < numSamples; ++i)
	{
		// Smooth gains
		smoothedInputGain  += (targetInputGain  - smoothedInputGain)  * kGainSmoothStep;
		smoothedOutputGain += (targetOutputGain - smoothedOutputGain) * kGainSmoothStep;
		smoothedMix        += (mixValue         - smoothedMix)        * kGainSmoothStep;
		dryLevelState      += (dryLevelTarget   - dryLevelState)      * kGainSmoothStep;
		wetLevelState      += (wetLevelTarget   - wetLevelState)      * kGainSmoothStep;
		limThreshLinState  += (limThreshLinTarget - limThreshLinState) * kGainSmoothStep;
		float& smoothedWindowActive = smoothedWindowByFamily_[(size_t) windowFamily];
		smoothedWindowActive += (targetWindow - smoothedWindowActive) * windowSmoothStep_;
		smoothedWindow_ = smoothedWindowActive;
		smoothedSpeed_     += (targetSpeed      - smoothedSpeed_)     * kGainSmoothStep;
		smoothedPitchRate_ += (targetPitchRate  - smoothedPitchRate_) * kGainSmoothStep;
		jitterSmoothed_    += (jitterTarget     - jitterSmoothed_)    * jitterSmoothStep_;
		if (jitterTarget <= 1.0e-5f && jitterSmoothed_ < 1.0e-5f)
			jitterSmoothed_ = 0.0f;
		if (jitterTarget > 1.0e-5f || jitterSmoothed_ > 1.0e-5f)
			advanceJitterEngines (jitterSmoothed_);

		const float controlSpeed = smoothedSpeed_;
		const float basePitchRate = smoothedPitchRate_;
		const bool stretchJitterActive = (engineVal == 0) && (jitterSmoothed_ > 1.0e-5f);
		const bool grainJitterActive = (engineVal == 1) && (jitterSmoothed_ > 1.0e-5f);
		const bool fftJitterActive = (engineVal == 2 || engineVal == 3) && (jitterSmoothed_ > 1.0e-5f);
		const float jitterAmountNorm = juce::jlimit (0.0f, 1.0f, 1.0f - controlSpeed);
		const float jitterMotionAmountScale = (! stretchJitterActive && ! fftJitterActive && jitterAmountNorm > 1.0e-5f)
			? std::sqrt (jitterAmountNorm)
			: 0.0f;
		const float jitterReferenceSamples = (engineVal == 1)
			? (float) grainSamples
			: (float) ((engineVal == 3) ? fft2GeometryWindowSamples : windowSamples);
		const bool jitterAllowAnchor = (engineVal == 1);
		const auto grainJitterL = makeJitterRuntimeValues (0, jitterReferenceSamples, 1.0f, jitterMotionAmountScale, jitterAllowAnchor);
		const auto grainJitterR = makeJitterRuntimeValues ((styleVal == 0) ? 0 : 1,
		                                                   jitterReferenceSamples, 1.0f, jitterMotionAmountScale, jitterAllowAnchor);
		auto stretchJitterL = stretchJitterActive ? makeStretchJitterRuntimeValues (0) : JitterRuntimeValues {};
		auto stretchJitterR = stretchJitterActive
			? makeStretchJitterRuntimeValues ((styleVal == 0) ? 0 : 1)
			: JitterRuntimeValues {};
		if (stretchJitterActive)
		{
			stretchJitterPitchScaleSmoothed_[0] += (stretchJitterL.pitchScale - stretchJitterPitchScaleSmoothed_[0])
				* kStretchJitterPitchSmoothStep;
			stretchJitterPitchScaleSmoothed_[1] += (stretchJitterR.pitchScale - stretchJitterPitchScaleSmoothed_[1])
				* kStretchJitterPitchSmoothStep;
			stretchJitterL.pitchScale = stretchJitterPitchScaleSmoothed_[0];
			stretchJitterR.pitchScale = stretchJitterPitchScaleSmoothed_[1];
		}
		else
		{
			stretchJitterPitchScaleSmoothed_[0] += (1.0f - stretchJitterPitchScaleSmoothed_[0])
				* kStretchJitterPitchSmoothStep;
			stretchJitterPitchScaleSmoothed_[1] += (1.0f - stretchJitterPitchScaleSmoothed_[1])
				* kStretchJitterPitchSmoothStep;
		}
		const auto fftJitterL = fftJitterActive ? makeFftJitterRuntimeValues (0) : JitterRuntimeValues {};
		const auto fftJitterR = fftJitterActive
			? makeFftJitterRuntimeValues ((styleVal == 0) ? 0 : 1)
			: JitterRuntimeValues {};
		const auto& jitterL = fftJitterActive ? fftJitterL : (stretchJitterActive ? stretchJitterL : grainJitterL);
		const auto& jitterR = fftJitterActive ? fftJitterR : (stretchJitterActive ? stretchJitterR : grainJitterR);
		const float speed = controlSpeed;
		const float pitchRate = basePitchRate * jitterL.pitchScale;
		if (engineVal == 3 && triggerOn)
		{
			const float fft2ZeroAmountTol = fft2AmountZeroHoldBypassActive_ ? 0.05f : 0.02f;
			const bool fft2AmountZeroTarget = (! fftJitterActive) && amountVal <= fft2ZeroAmountTol;
			const float fft2TargetHoldCoeff = std::sqrt (std::sqrt (juce::jlimit (0.0f, 1.0f, 1.0f - controlSpeed)));
			const float fft2AudioTargetHoldCoeff = std::sqrt (std::sqrt (juce::jlimit (0.0f, 1.0f, 1.0f - speed)));
			const float fft2HoldCoeffStep = 1.0f - std::exp (-1.0f / (static_cast<float> (currentSampleRate) * 0.022f));
			fft2HoldCoeffSmoothed_ += (fft2TargetHoldCoeff - fft2HoldCoeffSmoothed_) * fft2HoldCoeffStep;
			fft2AudioHoldCoeffSmoothed_ += (fft2AudioTargetHoldCoeff - fft2AudioHoldCoeffSmoothed_) * fft2HoldCoeffStep;
			const bool fft2AmountZeroSettled = fft2AmountZeroTarget
			                                && (controlSpeed >= 0.9995f)
			                                && (fft2HoldCoeffSmoothed_ <= 0.001f);
			if (fft2AmountZeroSettled)
			{
				if (! fft2AmountZeroHoldBypassActive_)
				{
					for (int ch = 0; ch < 2; ++ch)
					{
						std::fill (std::begin (stft_.heldMag[ch]), std::end (stft_.heldMag[ch]), 0.0f);
						std::fill (std::begin (stft_.heldFreq[ch]), std::end (stft_.heldFreq[ch]), 0.0f);
					}
				}
				fft2AmountZeroHoldBypassActive_ = true;
				fft2HoldCoeffSmoothed_ = 0.0f;
				fft2AudioHoldCoeffSmoothed_ = 0.0f;
			}
			else
			{
				fft2AmountZeroHoldBypassActive_ = false;
			}
		}
		else
		{
			fft2AmountZeroHoldBypassActive_ = false;
			const float fft2TargetHoldCoeff = std::sqrt (std::sqrt (juce::jlimit (0.0f, 1.0f, 1.0f - controlSpeed)));
			fft2HoldCoeffSmoothed_ = fft2TargetHoldCoeff;
			fft2AudioHoldCoeffSmoothed_ = fft2TargetHoldCoeff;
		}

		float inL = channelL[i] * smoothedInputGain;
		float inR = (channelR != nullptr) ? channelR[i] * smoothedInputGain : inL;

		// Save dry signal (with Align delay for FFT engine PDC)
		float dryOrigL, dryOrigR;
		const float dryInL = channelL[i];
		const float dryInR = (channelR != nullptr) ? channelR[i] : dryInL;

		dryDelayBuf_[0][dryDelayWritePos_] = dryInL;
		dryDelayBuf_[1][dryDelayWritePos_] = dryInR;

		if (dryDelayLen_ > 0)
		{
			const int rdp = (dryDelayWritePos_ - dryDelayLen_ + kDryDelayBufLen) & (kDryDelayBufLen - 1);
			dryOrigL = dryDelayBuf_[0][rdp];
			dryOrigR = dryDelayBuf_[1][rdp];
		}
		else
		{
			dryOrigL = dryInL;
			dryOrigR = dryInR;
		}

		dryDelayWritePos_ = (dryDelayWritePos_ + 1) & (kDryDelayBufLen - 1);

		// Mode In: M/S encode input
		if (numChannels >= 2 && modeInVal != 0)
		{
			const float l = inL, r = inR;
			if (modeInVal == 1)      { const float mid  = (l + r) * kSqrt2Over2; inL = inR = mid; }
			else /* modeInVal==2 */   { const float side = (l - r) * kSqrt2Over2; inL = inR = side; }
		}

		// PRE filter/tilt: apply before circular buffer
		if (filterPre_) filterWetSample (inL, inR);
		if (tiltPre_)   tiltWetSample   (inL, inR);

        // Write input to circular buffer
		if (inputBufLen_ > 0)
		{
			inputBuf_[0][inputBufWritePos_] = inL;
			inputBuf_[1][inputBufWritePos_] = inR;
			inputBufWritePos_ = (inputBufWritePos_ + 1) & inputBufMask_;
			inputBufWriteAbsPos_ += 1.0;
		}

		float unityRefL = inL;
		float unityRefR = inR;
		if (alignOn && dryDelayLen_ > 0 && inputBufLen_ > 0)
		{
			const int unityRefPos = (inputBufWritePos_ - 1 - dryDelayLen_ + inputBufLen_) & inputBufMask_;
			unityRefL = inputBuf_[0][unityRefPos];
			unityRefR = inputBuf_[1][unityRefPos];
		}

		float wetL = 0.0f;
		float wetR = 0.0f;

        // Engine dispatch
		const bool isDual = (styleVal == 3 && numChannels >= 2);
		const bool isWide = (styleVal == 2 && numChannels >= 2);
		const float pitchRateR = isDual ? (basePitchRate * 0.5f * jitterR.pitchScale) : -1.0f;
		if (! triggerOn || engineVal != 0)
		{
			wsolaUnityBypassActive_ = false;
			stretchBootstrapSegments_ = 0;
			stretchTransitionRemaining_ = 0;
			stretchTransitionTotal_ = 0;
			stretchTransitionToUnity_ = false;
		}
		if (! triggerOn || engineVal != 1)
		{
			grainUnityBypassActive_ = false;
			grainTransitionRemaining_ = 0;
			grainTransitionTotal_ = 0;
			grainTransitionToUnity_ = false;
			grainFreezeHoldActive_ = false;
		}
		if (! triggerOn || (engineVal != 2 && engineVal != 3))
		{
			fftUnityBypassActive_ = false;
			fft1AmountUnityBypassActive_ = false;
			fftTransitionRemaining_ = 0;
			fftTransitionTotal_ = 0;
			fftTransitionHoldSamples_ = 0;
			fftTransitionToUnity_ = false;
		}
		if (! triggerOn || engineVal != 2)
		{
			fft1AmountUnityBypassActive_ = false;
			fft1ReentryTraceRemaining_ = 0;
			fftFreezeTransitionRemaining_ = 0;
			fftFreezeTransitionTotal_ = 0;
			clearWindowTransitionForEngine (2);
			fftExplicitFreezeActive_ = false;
			fftExplicitFreezeCapturePending_ = false;
			fftStartupWarmupRemainingCycles_ = 0;
		}
		if (! triggerOn || engineVal != 3)
		{
			clearWindowTransitionForEngine (3);
		}

		if (! triggerOn)
		{
            // Trigger OFF -> unity path, aligned if requested
			wetL = unityRefL;
			wetR = unityRefR;
		}
		else if ((engineVal == 2 || engineVal == 3) && inputBufLen_ > 0 && stft_.activeFftSize > 0)
		{
            // Engines 2 and 3: FFT-based (phase vocoder / spectral hold)
			const int outBufLen = kStftOutBufLen;
			const int fftSynthHop = recommendedFftSynthHop (stft_.activeFftSize);
			const int fftWetLatency = stft_.activeFftSize + fftSynthHop;
			const int identityRefPos = (inputBufWritePos_ - fftWetLatency + inputBufLen_) & inputBufMask_;
			const float identityRefL = inputBuf_[0][identityRefPos];
			const float identityRefR = inputBuf_[1][identityRefPos];
			const bool fftStandardUnityCapable = (engineVal == 2 || engineVal == 3)
			                              && ! reverseOn
			                              && ! isDual
			                              && ! isWide
			                              && ! fftJitterActive;
			const bool fftExplicitFreezeCapable = false;
			const bool fftTargetFreeze = fftExplicitFreezeCapable && (amountVal >= (kAmountMax - 0.0005f));
			const bool fft1ReverseDirectFreezeTarget = (engineVal == 2)
			                                        && reverseOn
			                                        && (controlSpeed <= 0.0001f);
			if (fft1ReverseDirectFreezeTarget)
			{
				fftExplicitFreezeActive_ = false;
				fftExplicitFreezeCapturePending_ = false;
				fftFreezeTransitionRemaining_ = 0;
				fftFreezeTransitionTotal_ = 0;
				stft_.freezeEntryWarmupCycles = 0;
			}
			const bool fftUnityStateActive = fftUnityBypassActive_ || fftTransitionToUnity_;
			const float fft1UnityAmountTol = fftUnityStateActive ? 0.05f : 0.02f;
			const float fft2UnityAmountTol = fftUnityStateActive ? 5.50f : 5.00f;
			const bool fftLargeFftNearUnity = (engineVal == 2) && (stft_.activeFftSize >= 4096);
			const float unityEnterSpeedTol = 0.0005f;
			const float unityExitSpeedTol  = 0.0035f;
			const float unityEnterPitchTol = 0.0015f;
			const float unityExitPitchTol  = 0.0060f;
			const float unityEnterAmountTol = (stft_.activeFftSize >= 8192) ? 0.80f
			                              : (stft_.activeFftSize >= 4096) ? 0.40f
			                                                           : 0.0f;
			const float unityExitAmountTol = (stft_.activeFftSize >= 8192) ? 1.40f
			                             : (stft_.activeFftSize >= 4096) ? 0.85f
			                                                          : 0.0f;
			const float fftLargeUnityAmountTol = fftUnityStateActive ? unityExitAmountTol : unityEnterAmountTol;
			const bool fftUnitySpeedOk = fftLargeFftNearUnity
				? (amountVal <= fftLargeUnityAmountTol
				    && controlSpeed >= (1.0f - fftLargeUnityAmountTol / 100.0f))
				: (std::abs (controlSpeed - 1.0f)
				       <= (fftUnityStateActive ? unityExitSpeedTol : unityEnterSpeedTol));
			const bool fftUnityPitchOk = std::abs (basePitchRate - 1.0f)
			                          <= (fftUnityStateActive ? unityExitPitchTol : unityEnterPitchTol);
			const bool fft1AmountUnity = (engineVal == 2)
			                          && ! fftJitterActive
			                          && (amountVal <= fft1UnityAmountTol)
			                          && fftUnitySpeedOk
			                          && fftUnityPitchOk;
			const bool fft2HardAmountUnity = (engineVal == 3)
			                              && ! fftJitterActive
			                              && (amountVal <= fft1UnityAmountTol)
			                              && fftUnitySpeedOk
			                              && fftUnityPitchOk
			                              && fft2AmountZeroHoldBypassActive_;
			const bool fft2AmountUnity = (engineVal == 3)
			                          && ! fftJitterActive
			                          && (amountVal <= fft2UnityAmountTol)
			                          && (controlSpeed >= (1.0f - fft2UnityAmountTol / 100.0f))
			                          && fftUnityPitchOk;
			const bool fftUnityPathActive = fftUnityStateActive
			                             || (fftTransitionRemaining_ > 0 && fftTransitionTotal_ > 0);
			const bool fftUnityCapable = fftStandardUnityCapable || fft1AmountUnity || fft2AmountUnity || fftUnityPathActive;
			const bool fftUnity = fft1AmountUnity
			                   || fft2AmountUnity
			                   || (fftStandardUnityCapable && fftUnitySpeedOk && fftUnityPitchOk);
			const int fftTransitionSamples = juce::jlimit (32,
			                                               (int) std::round (currentSampleRate * 0.08),
			                                               juce::jmax ((int) std::round (currentSampleRate * 0.02),
			                                                           juce::jmax (1, fftSynthHop / 2)));

			auto readCurrentFftWet = [&] (float& outL, float& outR)
			{
				const float norm = stft_.outputNormAccum[stft_.outputReadPos];
				constexpr float kFftOutputNormFloor = 1.0e-4f;
				constexpr float kFftStableNormFloor = 5.0e-3f;
				if (norm <= kFftOutputNormFloor)
				{
					outL = 0.0f;
					outR = 0.0f;
					return;
				}

				const float invNorm = 1.0f / norm;
				const float rawL = stft_.outputAccum[0][stft_.outputReadPos] * invNorm;
				const float rawR = stft_.outputAccum[1][stft_.outputReadPos] * invNorm;
				if (norm >= kFftStableNormFloor)
				{
					outL = rawL;
					outR = rawR;
					fftLastStableWetL_ = rawL;
					fftLastStableWetR_ = rawR;
					fftLastStableWetValid_ = true;
					return;
				}

				const float coverage = juce::jlimit (0.0f, 1.0f,
					(norm - kFftOutputNormFloor) / (kFftStableNormFloor - kFftOutputNormFloor));
				const float lowNormMix = coverage * coverage * (3.0f - 2.0f * coverage);
				const float fallbackL = fftLastStableWetValid_ ? fftLastStableWetL_ : 0.0f;
				const float fallbackR = fftLastStableWetValid_ ? fftLastStableWetR_ : 0.0f;
				outL = fallbackL + (rawL - fallbackL) * lowNormMix;
				outR = fallbackR + (rawR - fallbackR) * lowNormMix;
			};

			auto consumeCurrentFftWet = [&]()
			{
				stft_.outputAccum[0][stft_.outputReadPos] = 0.0f;
				stft_.outputAccum[1][stft_.outputReadPos] = 0.0f;
				stft_.outputNormAccum[stft_.outputReadPos] = 0.0f;
				stft_.outputReadPos = (stft_.outputReadPos + 1) & (outBufLen - 1);
			};

			auto accumulateIdentityMetrics = [&] (float actualWetL, float actualWetR)
			{
				if (! fftDebugEnabled)
					return;

				const float identityErrL = actualWetL - identityRefL;
				const float identityErrR = actualWetR - identityRefR;
				stft_.identityRefSqAccum[0] += (double) identityRefL * (double) identityRefL;
				stft_.identityRefSqAccum[1] += (double) identityRefR * (double) identityRefR;
				stft_.identityErrSqAccum[0] += (double) identityErrL * (double) identityErrL;
				stft_.identityErrSqAccum[1] += (double) identityErrR * (double) identityErrR;
				stft_.identityMaxAbsErr[0] = juce::jmax (stft_.identityMaxAbsErr[0], std::abs (identityErrL));
				stft_.identityMaxAbsErr[1] = juce::jmax (stft_.identityMaxAbsErr[1], std::abs (identityErrR));
				++stft_.identitySampleCount;
			};

			auto runFftCycle = [&] (float cycleSpeed, float cyclePitchRate, float cyclePitchRateR)
			{
				const bool fft1ReverseDirectFreezeCycle = (engineVal == 2)
				                                       && reverseOn
				                                       && (controlSpeed <= 0.0001f);
				const bool fftRuntimeFreezeTarget = (! fft1ReverseDirectFreezeCycle && fftTargetFreeze)
					|| ((((engineVal == 2) && ! fft1ReverseDirectFreezeCycle) || (engineVal == 3))
					    && (controlSpeed <= 0.0001f));
				if (fftDebugEnabled)
				{
					fftDebugContext_.blockIndex = debugBlockIndex;
					fftDebugContext_.sampleIndex = i;
					fftDebugContext_.engine = engineVal;
					fftDebugContext_.amount = amountVal;
					fftDebugContext_.pitch = pitchVal;
					fftDebugContext_.speed = cycleSpeed;
					fftDebugContext_.pitchRate = cyclePitchRate;
					fftDebugContext_.targetAnalysisHop = 0.0f;
					fftDebugContext_.filteredAnalysisHop = 0.0f;
					fftDebugContext_.analysisHopQuantError = 0.0f;
					fftDebugContext_.lastAnalysisHop = stft_.lastAnalysisHop;
					fftDebugContext_.freezeEntryWarmupCycles = stft_.freezeEntryWarmupCycles;
					fftDebugContext_.fftStartupWarmupRemainingCycles = fftStartupWarmupRemainingCycles_;
					fftDebugContext_.fftExplicitFreezeActive = fftExplicitFreezeActive_ ? 1 : 0;
					fftDebugContext_.fftExplicitFreezeCapturePending = fftExplicitFreezeCapturePending_ ? 1 : 0;
					fftDebugContext_.fftTargetFreeze = fftRuntimeFreezeTarget ? 1 : 0;
					fftDebugContext_.analysisHopDebug = -1;
					fftDebugContext_.windowSamples = (engineVal == 3) ? fft2GeometryWindowSamples : windowSamples;
					fftDebugContext_.style = styleVal;
					fftDebugContext_.reverseOn = reverseOn ? 1 : 0;
					fftDebugContext_.triggerOn = triggerOn ? 1 : 0;
				}
				populateFftDebugContextControlState (reportedLatency, fftOutputPadLen);
				if (fftDebugEnabled)
				{
					fftDebugContext_.lastAnalysisHop = stft_.lastAnalysisHop;
					fftDebugContext_.freezeEntryWarmupCycles = stft_.freezeEntryWarmupCycles;
					fftDebugContext_.fftStartupWarmupRemainingCycles = fftStartupWarmupRemainingCycles_;
					fftDebugContext_.fftExplicitFreezeActive = fftExplicitFreezeActive_ ? 1 : 0;
					fftDebugContext_.fftExplicitFreezeCapturePending = fftExplicitFreezeCapturePending_ ? 1 : 0;
					fftDebugContext_.fftTargetFreeze = fftRuntimeFreezeTarget ? 1 : 0;
				}

				if (engineVal == 3)
				{
					const bool fft2FullHold = (controlSpeed <= 0.0001f)
					                       && (fft2HoldCoeffSmoothed_ >= 0.999f);
					const float holdCoeff = fft2FullHold
						? 1.0f
						: juce::jlimit (0.0f, 1.0f, fft2AudioHoldCoeffSmoothed_);
					if (holdCoeff <= 0.001f)
					{
						fftDebugContext_.fft1FreezeHoldRoute = 0;
						performStftCycle (stft_.activeFftSize, fftSynthHop, fftSynthHop,
						                  cyclePitchRate, reverseOn, cyclePitchRateR, isWide);
					}
					else
					{
						if (fft2FullHold)
							fftDebugContext_.analysisHopDebug = 0;
						fftDebugContext_.fft1FreezeHoldRoute = 0;
						performStftCycleSpectralHold (stft_.activeFftSize, fftSynthHop,
						                              holdCoeff, cyclePitchRate, reverseOn,
						                              fft2FullHold && stft_.hasFrame,
						                              cyclePitchRateR, isWide);
					}
				}
				else if (engineVal == 2 && fftExplicitFreezeActive_ && ! fft1ReverseDirectFreezeCycle)
				{
					fftDebugContext_.analysisHopDebug = 0;
					const float holdCoeff = fftExplicitFreezeCapturePending_ ? 0.0f : 1.0f;
					fftDebugContext_.fft1FreezeHoldRoute = 1;
					performStftCycleSpectralHold (stft_.activeFftSize, fftSynthHop,
					                              holdCoeff, cyclePitchRate, reverseOn,
					                              ! fftExplicitFreezeCapturePending_,
					                              cyclePitchRateR, isWide);
					if (fftExplicitFreezeCapturePending_)
					{
						fftExplicitFreezeCapturePending_ = false;
						fftFreezeTransitionTotal_ = recommendedFftFreezeTransitionSamples (stft_.activeFftSize);
						fftFreezeTransitionRemaining_ = fftFreezeTransitionTotal_;
						fftFreezeTransitionReadPos_ = (fftWetHistoryWritePos_ - 1
							+ kFftWetHistoryLen) & (kFftWetHistoryLen - 1);
					}
				}
				else
				{
					double targetAnalysisHop = 0.0;
					double filteredAnalysisHop = 0.0;
					const int freezeHopThreshold = juce::jmax (1, fftSynthHop / 8);
					const int minAnalysisHop = 1;
					if (cycleSpeed > 0.0001f)
						targetAnalysisHop = juce::jlimit ((double) minAnalysisHop, (double) fftSynthHop,
						                                  (double) fftSynthHop * (double) cycleSpeed);
					else if (engineVal == 2)
						targetAnalysisHop = fft1ReverseDirectFreezeCycle ? (double) minAnalysisHop : 0.0;
					int fftAnalysisHop = (targetAnalysisHop > 0.0001)
						? juce::jlimit (minAnalysisHop, fftSynthHop, (int) std::lround (targetAnalysisHop))
						: 0;
					stft_.analysisHopSlewNorm = 0.0f;
					stft_.analysisHopStepNorm = 0.0f;
					bool enteringFreeze = false;
					bool freezeImmediatelyAfterReset = false;
					const bool useContinuousAnalysisHop = (engineVal == 2)
						&& (stft_.activeFftSize == 2048);
					const bool resetContinuousForFreeze = (engineVal == 2)
						&& ! fft1ReverseDirectFreezeCycle
						&& (targetAnalysisHop <= (double) freezeHopThreshold);
					if (useContinuousAnalysisHop)
					{
						filteredAnalysisHop = targetAnalysisHop;
						if (resetContinuousForFreeze)
						{
							stft_.filteredAnalysisHop = -1.0;
							stft_.analysisHopQuantError = 0.0;
						}
						else if (stft_.filteredAnalysisHop >= 0.0)
						{
							const double maxHopDelta = (double) ((stft_.activeFftSize >= 8192)
								? juce::jmax (24, fftSynthHop / 12)
								: (stft_.activeFftSize >= 4096)
									? juce::jmax (20, fftSynthHop / 14)
									: juce::jmax (16, fftSynthHop / 8));
							filteredAnalysisHop = stft_.filteredAnalysisHop;
							if (targetAnalysisHop > filteredAnalysisHop + maxHopDelta)
								filteredAnalysisHop += maxHopDelta;
							else if (targetAnalysisHop < filteredAnalysisHop - maxHopDelta)
								filteredAnalysisHop -= maxHopDelta;
							else
								filteredAnalysisHop = targetAnalysisHop;

							stft_.analysisHopSlewNorm = juce::jlimit (0.0f, 1.0f,
							                                          (float) std::abs (targetAnalysisHop - filteredAnalysisHop)
							                                              / (float) juce::jmax (1, fftSynthHop));
						}
						else if (stft_.activeFftSize >= 4096 && targetAnalysisHop > 0.0001)
						{
							const bool nearUnityHop = targetAnalysisHop >= ((double) fftSynthHop * 0.75);
							filteredAnalysisHop = nearUnityHop ? (double) fftSynthHop : targetAnalysisHop;
						}

						if (filteredAnalysisHop > 0.0001)
						{
							const double quantizedHop = filteredAnalysisHop + stft_.analysisHopQuantError;
							fftAnalysisHop = juce::jlimit (1, fftSynthHop, (int) std::lround (quantizedHop));
							stft_.analysisHopQuantError = juce::jlimit (-1.0, 1.0,
							                                            stft_.analysisHopQuantError
							                                                + filteredAnalysisHop
							                                                - (double) fftAnalysisHop);
						}
						else
						{
							fftAnalysisHop = 0;
							stft_.analysisHopQuantError = 0.0;
						}
						stft_.filteredAnalysisHop = filteredAnalysisHop;
					}
					else
					{
						filteredAnalysisHop = targetAnalysisHop;
					}
					fftDebugContext_.targetAnalysisHop = (float) targetAnalysisHop;
					fftDebugContext_.filteredAnalysisHop = (float) filteredAnalysisHop;
					fftDebugContext_.analysisHopQuantError = (float) stft_.analysisHopQuantError;

					if (engineVal == 2)
					{
						if (stft_.activeFftSize >= 4096
						    && ! fft1ReverseDirectFreezeCycle
						    && stft_.freezeEntryWarmupCycles > 0
						    && targetAnalysisHop > 0.0001
						    && targetAnalysisHop <= (double) freezeHopThreshold)
						{
							const float warmupDivisor = (stft_.activeFftSize >= 8192) ? 8.0f : 6.0f;
							const float warmupNorm = juce::jlimit (0.0f, 1.0f,
							                                       (float) stft_.freezeEntryWarmupCycles / warmupDivisor);
							const float freezeRamp = warmupNorm * warmupNorm;
							const int freezeFloorHop = juce::jlimit (0, freezeHopThreshold,
							                                         (int) std::lround ((float) freezeHopThreshold * freezeRamp));
							fftAnalysisHop = juce::jmax (fftAnalysisHop, freezeFloorHop);
						}
						enteringFreeze = ! fft1ReverseDirectFreezeCycle
							&& (fftAnalysisHop <= 0)
							&& (stft_.lastAnalysisHop > 0);
						freezeImmediatelyAfterReset = ! fft1ReverseDirectFreezeCycle
							&& (fftAnalysisHop <= 0)
							&& (stft_.lastAnalysisHop < 0);
						if (enteringFreeze || freezeImmediatelyAfterReset)
						{
							stft_.freezeEntryWarmupCycles = (stft_.activeFftSize >= 8192) ? 8
							                              : (stft_.activeFftSize >= 4096) ? 6
							                                                                   : 3;
							if (stft_.activeFftSize >= 4096)
							{
								fftFreezeTransitionTotal_ = recommendedFftFreezeTransitionSamples (stft_.activeFftSize);
								fftFreezeTransitionRemaining_ = fftFreezeTransitionTotal_;
								fftFreezeTransitionReadPos_ = (fftWetHistoryWritePos_ - 1
									+ kFftWetHistoryLen) & (kFftWetHistoryLen - 1);
							}
						}

						if (stft_.lastAnalysisHop >= 0)
						{
							stft_.analysisHopSlewNorm = juce::jlimit (0.0f, 1.0f,
							                                          juce::jmax (stft_.analysisHopSlewNorm,
							                                                      (float) std::abs (targetAnalysisHop - (double) fftAnalysisHop)
							                                                          / (float) juce::jmax (1, fftSynthHop)));
							stft_.analysisHopStepNorm = juce::jlimit (0.0f, 1.0f,
							                                          juce::jmax (stft_.analysisHopStepNorm,
							                                                      (float) std::abs (fftAnalysisHop - stft_.lastAnalysisHop)
							                                                          / (float) juce::jmax (1, fftSynthHop)));
						}

						if (! useContinuousAnalysisHop)
						{
							stft_.filteredAnalysisHop = -1.0;
							stft_.analysisHopQuantError = 0.0;
						}

						stft_.lastAnalysisHop = fftAnalysisHop;
						fftDebugContext_.lastAnalysisHop = stft_.lastAnalysisHop;
						fftDebugContext_.freezeEntryWarmupCycles = stft_.freezeEntryWarmupCycles;
						fftDebugContext_.fftStartupWarmupRemainingCycles = fftStartupWarmupRemainingCycles_;
						fftDebugContext_.fftExplicitFreezeActive = fftExplicitFreezeActive_ ? 1 : 0;
						fftDebugContext_.fftExplicitFreezeCapturePending = fftExplicitFreezeCapturePending_ ? 1 : 0;
						fftDebugContext_.fftTargetFreeze = fftTargetFreeze ? 1 : 0;
					}

					const bool fft1FreezeHoldRoute = (engineVal == 2)
					                              && ! fft1ReverseDirectFreezeCycle
					                              && (fftAnalysisHop <= 0);
					if (fft1FreezeHoldRoute)
					{
						fftDebugContext_.analysisHopDebug = 0;
						fftDebugContext_.fft1FreezeHoldRoute = 1;
						const bool captureFreezeFrame = ! stft_.hasFrame
							|| enteringFreeze
							|| freezeImmediatelyAfterReset;
						const float holdCoeff = captureFreezeFrame ? 0.0f : 1.0f;
						performStftCycleSpectralHold (stft_.activeFftSize, fftSynthHop,
						                              holdCoeff, cyclePitchRate, reverseOn,
						                              ! captureFreezeFrame,
						                              cyclePitchRateR, isWide);
					}
					else
					{
						fftDebugContext_.fft1FreezeHoldRoute = 0;
						performStftCycle (stft_.activeFftSize, fftAnalysisHop,
						                  fftSynthHop, cyclePitchRate, reverseOn, cyclePitchRateR, isWide);
					}
					if (engineVal == 2 && stft_.freezeEntryWarmupCycles > 0)
						--stft_.freezeEntryWarmupCycles;
				}
			};

			if (fftUnityCapable && ! fftUnity && fftTransitionToUnity_)
			{
				fftTransitionToUnity_ = false;
				fft1AmountUnityBypassActive_ = false;
				fftTransitionRemaining_ = 0;
				fftTransitionTotal_ = 0;
				fftTransitionHoldSamples_ = 0;
				if (engineVal == 2)
					fftStartupWarmupRemainingCycles_ = 0;
				fftUnityBypassActive_ = false;
				if (engineVal == 2)
				{
					fftFreezeTransitionRemaining_ = 0;
					fftFreezeTransitionTotal_ = 0;
				}
			}

			if (engineVal == 2 && fftExplicitFreezeActive_ && ! fftTargetFreeze)
			{
				fftExplicitFreezeActive_ = false;
				fftExplicitFreezeCapturePending_ = false;
				fftFreezeTransitionTotal_ = recommendedFftFreezeTransitionSamples (stft_.activeFftSize);
				fftFreezeTransitionRemaining_ = fftFreezeTransitionTotal_;
				fftFreezeTransitionReadPos_ = (fftWetHistoryWritePos_ - 1
					+ kFftWetHistoryLen) & (kFftWetHistoryLen - 1);
			}

			if (engineVal == 2 && fftTargetFreeze && ! fftExplicitFreezeActive_)
			{
				fftExplicitFreezeActive_ = true;
				fftExplicitFreezeCapturePending_ = true;
			}

			if (fft2HardAmountUnity && ! fftUnityBypassActive_ && ! fftTransitionToUnity_)
			{
				fftUnityBypassActive_ = true;
				fftTransitionToUnity_ = false;
				fftTransitionRemaining_ = 0;
				fftTransitionTotal_ = 0;
				fftTransitionHoldSamples_ = 0;
			}

			if (fftUnityCapable && ! fftUnity && fftUnityBypassActive_)
			{
				const bool leavingFft1AmountUnityBypass = (engineVal == 2) && fft1AmountUnityBypassActive_;
				const int fftSizeForReset = (requestedFftSize > 0) ? requestedFftSize : stft_.activeFftSize;
				const double capturePos = (double) ((inputBufWritePos_ - 1 + inputBufLen_) & inputBufMask_);
				resetStftAtPos (capturePos, fftSizeForReset);
				recordFftReset (4, capturePos);
				stft_.synthCounter = 0;
				const float bootstrapPitchRate = pitchRate;
				const float bootstrapPitchRateR = isDual ? pitchRateR : -1.0f;
				runFftCycle (speed, bootstrapPitchRate, bootstrapPitchRateR);
				fftUnityBypassActive_ = false;
				fftTransitionToUnity_ = false;
				if (engineVal == 2)
				{
					fftFreezeTransitionRemaining_ = 0;
					fftFreezeTransitionTotal_ = 0;
					fftExplicitFreezeActive_ = false;
					fftExplicitFreezeCapturePending_ = false;
					fftStartupWarmupRemainingCycles_ = leavingFft1AmountUnityBypass
						? juce::jmax (1, (stft_.activeFftSize + fftSynthHop - 1) / fftSynthHop)
						: 0;
					fft1AmountUnityBypassActive_ = false;
					if (fftTargetFreeze)
					{
						fftTransitionTotal_ = 0;
						fftTransitionRemaining_ = 0;
					}
					else if (fftStartupWarmupRemainingCycles_ > 0)
					{
						fftTransitionTotal_ = 0;
						fftTransitionRemaining_ = 0;
						fftTransitionHoldSamples_ = 0;
					}
					else
					{
						fftTransitionTotal_ = fftTransitionSamples;
						fftTransitionRemaining_ = fftTransitionTotal_;
						fftTransitionHoldSamples_ = 0;
					}
				}
				else
				{
					fftTransitionHoldSamples_ = samplesForMs (18.0);
					fftTransitionTotal_ = fftTransitionSamples + fftTransitionHoldSamples_;
					fftTransitionRemaining_ = fftTransitionTotal_;
				}
			}

			if (fftUnityCapable && fftUnity && ! fftUnityBypassActive_ && ! fftTransitionToUnity_)
			{
				if (engineVal == 2 && fftStartupWarmupRemainingCycles_ > 0)
				{
					fftStartupWarmupRemainingCycles_ = 0;
					fftUnityBypassActive_ = true;
					fft1AmountUnityBypassActive_ = fft1AmountUnity;
					fftTransitionRemaining_ = 0;
					fftTransitionTotal_ = 0;
					fftTransitionHoldSamples_ = 0;
					fftTransitionToUnity_ = false;
					fftFreezeTransitionRemaining_ = 0;
					fftFreezeTransitionTotal_ = 0;
					fftExplicitFreezeActive_ = false;
					fftExplicitFreezeCapturePending_ = false;
				}
				else
				{
					if (stft_.hasFrame)
					{
						fftTransitionToUnity_ = true;
						fft1AmountUnityBypassActive_ = fft1AmountUnity;
						fftTransitionTotal_ = fftTransitionSamples;
						fftTransitionRemaining_ = fftTransitionTotal_;
						fftTransitionHoldSamples_ = 0;
					}
					else
					{
						fftUnityBypassActive_ = true;
						fft1AmountUnityBypassActive_ = fft1AmountUnity;
						fftTransitionRemaining_ = 0;
						fftTransitionTotal_ = 0;
						fftTransitionHoldSamples_ = 0;
						fftTransitionToUnity_ = false;
						if (engineVal == 2)
						{
							fftFreezeTransitionRemaining_ = 0;
							fftFreezeTransitionTotal_ = 0;
							fftExplicitFreezeActive_ = false;
							fftExplicitFreezeCapturePending_ = false;
						}
					}
				}
			}

			if (fftUnityCapable && fftUnityBypassActive_ && ! fftTransitionToUnity_)
			{
				wetL = identityRefL;
				wetR = identityRefR;
				accumulateIdentityMetrics (wetL, wetR);
			}
			else
			{
				float fftWetL = 0.0f, fftWetR = 0.0f;
#if STRETR_ENABLE_FFT1_CLICK_DUMP
				const int fftWetOutputReadPosBefore = stft_.outputReadPos;
				const int fftWetSynthCounterBefore = stft_.synthCounter;
				const float fftWetNormAtRead = stft_.outputNormAccum[fftWetOutputReadPosBefore];
#endif
				readCurrentFftWet (fftWetL, fftWetR);
#if STRETR_ENABLE_FFT1_CLICK_DUMP
				if (fft1AmountFreezeDumpActiveBlock
				    && updateDumpMaxDelta (fftDumpPrevFftWetL_, fftDumpPrevFftWetR_,
				                           fftWetL, fftWetR, i,
				                           fft1AmountFreezeMaxFftWetDeltaSample,
				                           fft1AmountFreezeMaxFftWetAbsDeltaL,
				                           fft1AmountFreezeMaxFftWetAbsDeltaR,
				                           fft1AmountFreezeMaxFftWetPrevL,
				                           fft1AmountFreezeMaxFftWetPrevR,
				                           fft1AmountFreezeMaxFftWetCurrL,
				                           fft1AmountFreezeMaxFftWetCurrR))
				{
					fft1AmountFreezeMaxFftWetNorm = fftWetNormAtRead;
					fft1AmountFreezeMaxFftWetOutputReadPos = fftWetOutputReadPosBefore;
					fft1AmountFreezeMaxFftWetSynthCounter = fftWetSynthCounterBefore;
				}
				fftDumpPrevFftWetL_ = fftWetL;
				fftDumpPrevFftWetR_ = fftWetR;
#endif
				fftDebugContext_.fftWetPreWindowFadeL = fftWetL;
				fftDebugContext_.fftWetPreWindowDeltaL = fftWetL - fftPrevWetPreWindowL_;
				fftDebugContext_.fftWetPostWindowFadeL = fftWetL;

				if (engineVal == 2 && fftStartupWarmupRemainingCycles_ > 0)
				{
					wetL = identityRefL;
					wetR = identityRefR;
				}
				else if (fftUnityCapable && fftTransitionRemaining_ > 0 && fftTransitionTotal_ > 0)
				{
					const int transitionFadeSamples = juce::jmax (1, fftTransitionTotal_ - fftTransitionHoldSamples_);
					float progress = 0.0f;
					if (fftTransitionToUnity_)
					{
						progress = 1.0f - ((float) fftTransitionRemaining_ / (float) fftTransitionTotal_);
					}
					else if (fftTransitionRemaining_ <= transitionFadeSamples)
					{
						progress = 1.0f - ((float) fftTransitionRemaining_ / (float) transitionFadeSamples);
					}
					if (fftTransitionToUnity_)
					{
						const float unityMix = std::sin (progress * juce::MathConstants<float>::halfPi);
						const float fftMix = std::cos (progress * juce::MathConstants<float>::halfPi);
						wetL = fftWetL * fftMix + identityRefL * unityMix;
						wetR = fftWetR * fftMix + identityRefR * unityMix;
					}
					else
					{
						const float fftMix = std::sin (progress * juce::MathConstants<float>::halfPi);
						const float unityMix = std::cos (progress * juce::MathConstants<float>::halfPi);
						wetL = identityRefL * unityMix + fftWetL * fftMix;
						wetR = identityRefR * unityMix + fftWetR * fftMix;
					}
					--fftTransitionRemaining_;
				}
				else if (engineVal == 2 && fftFreezeTransitionRemaining_ > 0 && fftFreezeTransitionTotal_ > 0)
				{
					const float progress = 1.0f
						- ((float) fftFreezeTransitionRemaining_ / (float) fftFreezeTransitionTotal_);
					const float freezeMix = std::sin (progress * juce::MathConstants<float>::halfPi);
					const float liveMix = std::cos (progress * juce::MathConstants<float>::halfPi);
					const int histIdx = fftFreezeTransitionReadPos_ & (kFftWetHistoryLen - 1);
					const float liveWetL = fftWetHistory_[0][histIdx];
					const float liveWetR = fftWetHistory_[1][histIdx];
					wetL = liveWetL * liveMix + fftWetL * freezeMix;
					wetR = liveWetR * liveMix + fftWetR * freezeMix;
					fftFreezeTransitionReadPos_ = (fftFreezeTransitionReadPos_ - 1 + kFftWetHistoryLen) & (kFftWetHistoryLen - 1);
					--fftFreezeTransitionRemaining_;
				}
				else if ((engineVal == 2 || engineVal == 3) && isWindowTransitionActiveForEngine (engineVal))
				{
					wetL = fftWetL;
					wetR = fftWetR;
					decrementWindowTransitionForEngine (engineVal);
				}
				else
				{
					wetL = fftWetL;
					wetR = fftWetR;
				}

				fftDebugContext_.fftWetPostWindowFadeL = wetL;
				fftDebugContext_.fftWetPostWindowDeltaL = wetL - fftPrevWetPostWindowL_;
				fftPrevWetPreWindowL_ = fftWetL;
				fftPrevWetPostWindowL_ = wetL;

				accumulateIdentityMetrics (wetL, wetR);
				consumeCurrentFftWet();

				if (++stft_.synthCounter >= fftSynthHop)
				{
					stft_.synthCounter = 0;
					runFftCycle (speed, pitchRate, pitchRateR);
					if (engineVal == 2 && fftStartupWarmupRemainingCycles_ > 0)
					{
						--fftStartupWarmupRemainingCycles_;
						if (fftStartupWarmupRemainingCycles_ <= 0)
						{
							fftStartupWarmupRemainingCycles_ = 0;
							fftTransitionTotal_ = fftTransitionSamples;
							fftTransitionRemaining_ = fftTransitionTotal_;
						}
					}
				}

				if (fftUnityCapable && fftTransitionRemaining_ <= 0)
				{
					if (fftTransitionToUnity_)
					{
						fftUnityBypassActive_ = true;
						fftTransitionToUnity_ = false;
					}
					fftTransitionRemaining_ = 0;
					fftTransitionTotal_ = 0;
				}
				if (engineVal == 2 && fftFreezeTransitionRemaining_ <= 0)
				{
					fftFreezeTransitionRemaining_ = 0;
					fftFreezeTransitionTotal_ = 0;
				}
				if ((engineVal == 2 || engineVal == 3) && getWindowTransitionRemainingForEngine (engineVal) <= 0)
				{
					clearWindowTransitionForEngine (engineVal);
				}
			}

			if (engineVal == 2 || engineVal == 3)
			{
				fftWetHistory_[0][fftWetHistoryWritePos_] = wetL;
				fftWetHistory_[1][fftWetHistoryWritePos_] = wetR;
				fftWetHistoryWritePos_ = (fftWetHistoryWritePos_ + 1) & (kFftWetHistoryLen - 1);
			}
		}
		else if (engineVal == 0 && inputBufLen_ > 0)
		{
            // Engine 0: WSOLA-style time-domain stretch with true overlap-add.
			const bool stretchUnity = ! reverseOn
				&& ! isDual
				&& ! isWide
				&& ! stretchJitterActive
				&& std::abs (speed - 1.0f) <= 0.0005f
				&& std::abs (pitchRate - 1.0f) <= 0.0005f;
			if (stretchUnity)
			{
				const double capturePos = (double) ((inputBufWritePos_ - 1 + inputBufLen_) & inputBufMask_);
				const bool enteringUnityBypass = ! wsolaUnityBypassActive_ && ! stretchTransitionToUnity_;
				if (enteringUnityBypass)
				{
					if (wsola_.hasPrevTail)
					{
						stretchTransitionToUnity_ = true;
						stretchTransitionTotal_ = juce::jlimit (32,
						                                        (int) std::round (currentSampleRate * 0.08),
						                                        juce::jmax ((int) std::round (currentSampleRate * 0.02),
						                                                    juce::jmax (wsola_.overlapLen * 2,
						                                                                juce::jmax (1, wsola_.synthesisHop / 2))));
						stretchTransitionRemaining_ = stretchTransitionTotal_;
					}
					else
					{
						resetWsolaAtPos (capturePos);
						wsola_.segInputStart = capturePos;
						wsola_.segInputStartR = capturePos;
						wsola_.samplesUntilNextSeg = 0;
						wsolaUnityBypassActive_ = true;
						stretchBootstrapSegments_ = 0;
						stretchTransitionRemaining_ = 0;
						stretchTransitionTotal_ = 0;
						stretchTransitionToUnity_ = false;
					}

#if JUCE_DEBUG
					StretchDebugEntry dbg {};
					dbg.blockIndex = debugBlockIndex;
					dbg.sampleIndex = i;
					dbg.eventType = 1;
					dbg.amount = amountVal;
					dbg.pitch = pitchVal;
					dbg.speed = speed;
					dbg.pitchRate = pitchRate;
					dbg.windowSamples = windowSamples;
					dbg.style = styleVal;
					dbg.reverseOn = reverseOn ? 1 : 0;
					dbg.triggerOn = triggerOn ? 1 : 0;
					dbg.hasPrevTail = wsola_.hasPrevTail ? 1 : 0;
					dbg.nearUnity = 1;
					dbg.segInputStart = wsola_.segInputStart;
					stretchDebugTrace_.record (dbg);
#endif
				}

				if (stretchTransitionToUnity_ && stretchTransitionRemaining_ > 0)
				{
					const int outMask = kWsolaOutBufLen - 1;
					const float stretchOutL = wsola_.outputAccumL[wsola_.outputReadPos];
					const float stretchOutR = wsola_.outputAccumR[wsola_.outputReadPos];
					wsola_.outputAccumL[wsola_.outputReadPos] = 0.0f;
					wsola_.outputAccumR[wsola_.outputReadPos] = 0.0f;
					wsola_.outputReadPos = (wsola_.outputReadPos + 1) & outMask;
					if (wsola_.samplesUntilNextSeg > 0)
						--wsola_.samplesUntilNextSeg;

					const float progress = 1.0f
						- ((float) stretchTransitionRemaining_ / (float) stretchTransitionTotal_);
					const float dryMix = std::sin (progress * juce::MathConstants<float>::halfPi);
					const float wetMix = std::cos (progress * juce::MathConstants<float>::halfPi);
					wetL = stretchOutL * wetMix + unityRefL * dryMix;
					wetR = stretchOutR * wetMix + unityRefR * dryMix;
					--stretchTransitionRemaining_;

					if (stretchTransitionRemaining_ <= 0)
					{
						resetWsolaAtPos (capturePos);
						wsola_.segInputStart = capturePos;
						wsola_.segInputStartR = capturePos;
						wsola_.samplesUntilNextSeg = 0;
						wsolaUnityBypassActive_ = true;
						stretchBootstrapSegments_ = 0;
						stretchTransitionRemaining_ = 0;
						stretchTransitionTotal_ = 0;
						stretchTransitionToUnity_ = false;
					}
				}
				else
				{
					wsola_.segInputStart = capturePos;
					wsola_.segInputStartR = capturePos;
					wsola_.samplesUntilNextSeg = 0;
					wsolaUnityBypassActive_ = true;
					stretchBootstrapSegments_ = 0;
					stretchTransitionRemaining_ = 0;
					stretchTransitionTotal_ = 0;
					stretchTransitionToUnity_ = false;
					wetL = unityRefL;
					wetR = unityRefR;
				}
			}
			else
			{
				const bool leavingUnityBypass = wsolaUnityBypassActive_;
				wsolaUnityBypassActive_ = false;
				stretchTransitionToUnity_ = false;
				const int stretchWindowSamples = juce::jlimit (kWindowMin, kWindowMax,
					(int) std::round (smoothedWindow_));
				const int segLen = juce::jmax (64, stretchWindowSamples);
				const int overlapLen = wsolaRecommendedOverlapLen (segLen, currentSampleRate);
				const int synthesisHop = juce::jmax (1, segLen - overlapLen);
				const int outMask = kWsolaOutBufLen - 1;
				const double direction = reverseOn ? -1.0 : 1.0;
				const float stretchPitchRateR = isDual ? pitchRateR
					: (isWide ? basePitchRate * jitterR.pitchScale : pitchRate);
				const double targetAnalysisHop = (double) synthesisHop * (double) speed * (double) pitchRate;
				const bool nearUnity = std::abs (speed - 1.0f) <= 0.05f
					&& std::abs (basePitchRate - 1.0f) <= 0.05f;
				const int seekCap = juce::jlimit (24, 128, (int) std::round (currentSampleRate * 0.0025));
				const int baseSeek = juce::jmin (juce::jmax (8, overlapLen / 3), inputBufLen_ / 4);
				const int seekRadius = nearUnity
					? juce::jmin (baseSeek, juce::jmax (8, seekCap / 2))
					: juce::jmin (baseSeek, seekCap);
				const double capturePos = (double) ((inputBufWritePos_ - 1 + inputBufLen_) & inputBufMask_);
				const auto computeLookBehind = [&](float readRateAbs, bool wideMode) noexcept
				{
					const double wideExtra = wideMode ? (double) (segLen / 2) : 0.0;
					return (double) seekRadius
						+ wideExtra
						+ ((double) (segLen - 1) * (double) readRateAbs)
						+ 8.0;
				};
				if (leavingUnityBypass)
				{
					double seedPos = capturePos;
					if (! reverseOn)
						seedPos -= computeLookBehind (pitchRate, isWide);
					resetWsolaAtPos (seedPos);
					stretchBootstrapSegments_ = juce::jlimit (2, 4, 1 + segLen / 768);
					stretchTransitionTotal_ = juce::jlimit (64,
					                                        (int) std::round (currentSampleRate * 0.12),
					                                        juce::jmax ((int) std::round (currentSampleRate * 0.06),
					                                                    synthesisHop * 2));
					stretchTransitionRemaining_ = stretchTransitionTotal_;
				}

				const auto scheduleStretchSegment = [&]()
				{
					double analysisHop = targetAnalysisHop;
					if (wsola_.hasPrevTail && nearUnity)
					{
						if (wsola_.lastAnalysisHop <= 0.0)
							wsola_.lastAnalysisHop = targetAnalysisHop;

						const double maxDelta = juce::jmax (1.0, (double) synthesisHop * 0.12);
						const double limitedTarget = juce::jlimit (wsola_.lastAnalysisHop - maxDelta,
						                                           wsola_.lastAnalysisHop + maxDelta,
						                                           targetAnalysisHop);
						analysisHop = wsola_.lastAnalysisHop + (limitedTarget - wsola_.lastAnalysisHop) * 0.35;
					}

					if (wsola_.hasPrevTail)
						wsola_.segInputStart += analysisHop * direction;

					if (! reverseOn)
					{
						const double maxStartL = capturePos - computeLookBehind (pitchRate, isWide);
						if (wsola_.segInputStart > maxStartL)
							wsola_.segInputStart = maxStartL;
					}

					wsola_.lastAnalysisHop = analysisHop;

					wsola_.segLen = segLen;
					wsola_.overlapLen = overlapLen;
					wsola_.synthesisHop = synthesisHop;

					const int synthPos = wsola_.nextSynthPos & outMask;
					const int prevBestOffset = wsola_.lastBestOffset;
					const auto match = wsola_.hasPrevTail
						? wsolaBestOverlapOffset (0, wsola_.segInputStart, overlapLen,
						                          prevBestOffset, nearUnity,
						                          pitchRate * (float) direction, synthPos)
						: WsolaMatchResult {};
					const int bestOff = match.bestOffset;
					wsola_.lastBestOffset = bestOff;

					double readPosL = wsola_.segInputStart + (double) bestOff;
					double readPosR = readPosL;
					if (isDual)
					{
						const double analysisHopR = (double) synthesisHop * (double) speed * (double) stretchPitchRateR;
						if (wsola_.hasPrevTail)
							wsola_.segInputStartR += analysisHopR * direction;

						if (! reverseOn)
						{
							const double maxStartR = capturePos - computeLookBehind (stretchPitchRateR, false);
							if (wsola_.segInputStartR > maxStartR)
								wsola_.segInputStartR = maxStartR;
						}

						const int prevBestOffsetR = wsola_.lastBestOffsetR;
						const auto matchR = wsola_.hasPrevTail
							? wsolaBestOverlapOffset (1, wsola_.segInputStartR, overlapLen,
							                          prevBestOffsetR, nearUnity,
							                          stretchPitchRateR * (float) direction, synthPos)
							: WsolaMatchResult {};
						wsola_.lastBestOffsetR = matchR.bestOffset;
						readPosR = wsola_.segInputStartR + (double) matchR.bestOffset;
					}
					else if (isWide)
					{
						readPosR = readPosL + (double) (segLen / 2);
					}

					const bool hasPrevSegment = wsola_.hasPrevTail;
					const float readRateL = pitchRate * (float) direction;
					const float readRateR = stretchPitchRateR * (float) direction;
					for (int n = 0; n < segLen; ++n)
					{
						const float sL = readInputBuf (0, readPosL);
						const float sR = (isDual || isWide) ? readInputBuf (1, readPosR)
						                                    : readInputBuf (1, readPosL);
						const float window = wsolaSegmentWeight (n, segLen, overlapLen, hasPrevSegment);
						const int outIdx = (synthPos + n) & outMask;
						wsola_.outputAccumL[outIdx] += sL * window;
						wsola_.outputAccumR[outIdx] += sR * window;
						readPosL += (double) readRateL;
						if (isDual || isWide)
							readPosR += (double) readRateR;
					}

#if JUCE_DEBUG
					StretchDebugEntry dbg {};
					dbg.blockIndex = debugBlockIndex;
					dbg.sampleIndex = i;
					dbg.eventType = 0;
					dbg.amount = amountVal;
					dbg.pitch = pitchVal;
					dbg.speed = speed;
					dbg.pitchRate = pitchRate;
					dbg.windowSamples = stretchWindowSamples;
					dbg.segLen = segLen;
					dbg.overlapLen = overlapLen;
					dbg.analysisHop = analysisHop;
					dbg.segInputStart = wsola_.segInputStart;
					dbg.nominalPos = ((int) std::floor (wsola_.segInputStart) % inputBufLen_ + inputBufLen_) % inputBufLen_;
					dbg.prevBestOffset = prevBestOffset;
					dbg.bestOffset = bestOff;
					dbg.style = styleVal;
					dbg.reverseOn = reverseOn ? 1 : 0;
					dbg.triggerOn = triggerOn ? 1 : 0;
					dbg.hasPrevTail = hasPrevSegment ? 1 : 0;
					dbg.nearUnity = nearUnity ? 1 : 0;
					dbg.bestScore = match.bestScore;
					dbg.bestNormCorr = match.bestNormCorr;
					dbg.centerPenalty = match.centerPenalty;
					dbg.driftPenalty = match.driftPenalty;
					dbg.startDeltaL = match.startDeltaL;
					dbg.startDeltaR = match.startDeltaR;
					dbg.overlapRmseL = match.overlapRmseL;
					dbg.overlapRmseR = match.overlapRmseR;
					stretchDebugTrace_.record (dbg);
#endif

					wsola_.nextSynthPos = (wsola_.nextSynthPos + synthesisHop) & outMask;
					wsola_.samplesUntilNextSeg = synthesisHop;
					wsola_.hasPrevTail = true;
				};

				if (wsola_.samplesUntilNextSeg <= 0)
				{
					scheduleStretchSegment();
					if (stretchBootstrapSegments_ > 0)
						--stretchBootstrapSegments_;
				}

				while (stretchBootstrapSegments_ > 0)
				{
					scheduleStretchSegment();
					--stretchBootstrapSegments_;
				}

				wetL = wsola_.outputAccumL[wsola_.outputReadPos];
				wetR = wsola_.outputAccumR[wsola_.outputReadPos];
				wsola_.outputAccumL[wsola_.outputReadPos] = 0.0f;
				wsola_.outputAccumR[wsola_.outputReadPos] = 0.0f;
				wsola_.outputReadPos = (wsola_.outputReadPos + 1) & outMask;
				if (wsola_.samplesUntilNextSeg > 0)
					--wsola_.samplesUntilNextSeg;

				if (stretchTransitionRemaining_ > 0 && stretchTransitionTotal_ > 0)
				{
					const float progress = 1.0f
						- ((float) stretchTransitionRemaining_ / (float) stretchTransitionTotal_);
					const float wetMix = std::sin (progress * juce::MathConstants<float>::halfPi);
					const float dryMix = std::cos (progress * juce::MathConstants<float>::halfPi);
					wetL = unityRefL * dryMix + wetL * wetMix;
					wetR = unityRefR * dryMix + wetR * wetMix;
					--stretchTransitionRemaining_;
				}
			}
		}
		else if (engineVal == 1 && inputBufLen_ > 0)
		{
            // Engine 1: GRANULAR
			const double grainCapturePos = currentCaptureAbsPos();
			const double grainLookBehind = computeGrainLookBehind (grainSamples, pitchRate, reverseOn, isWide);
			const bool grainFreezeTarget = (speed <= 0.0001f);
			if (grainFreezeTarget)
				grainFreezeHoldActive_ = true;
			else if (grainFreezeHoldActive_ && speed > 0.0035f)
				grainFreezeHoldActive_ = false;
			const bool grainFreezeHold = grainFreezeHoldActive_;
			const bool grainUnity = ! reverseOn
				&& ! isDual
				&& ! isWide
				&& ! grainJitterActive
				&& std::abs (speed - 1.0f) <= 0.0005f
				&& std::abs (pitchRate - 1.0f) <= 0.0005f;

			const auto spawnGranularGrains = [&]()
			{
				if (grainFreezeHold)
				{
					// In true freeze, keep the captured anchor stable when PITCH changes so
					// pitch returning to x1 does not pull the frozen grain toward newer audio.
					const double minPos = grainCapturePos - (double) (inputBufLen_ - 4);
					grainReadPos_ = juce::jmax (minPos, grainReadPos_);
				}
				else
				{
					grainReadPos_ = clampGrainSpawnPos (grainReadPos_, grainCapturePos, grainLookBehind);
				}
				if (--grainSpawnCountdown_ > 0)
					return;

				const int grainWindowSamples = juce::jlimit (kWindowMin, kWindowMax,
					(int) std::round (smoothedWindow_));
				const int density = juce::jlimit (2, kMaxGrains / 2, grainWindowSamples / 64);
				const int spawnInterval = juce::jmax (1, grainSamples / density);
				grainSpawnCountdown_ = spawnInterval;
				const double spawnPos = grainReadPos_;
				const float direction = reverseOn ? -1.0f : 1.0f;
				const double nextReadPos = grainReadPos_ + (double) spawnInterval * (double) speed * (double) direction;
				if (grainFreezeHold)
				{
					const double minPos = grainCapturePos - (double) (inputBufLen_ - 4);
					grainReadPos_ = juce::jmax (minPos, nextReadPos);
				}
				else
				{
					grainReadPos_ = clampGrainSpawnPos (nextReadPos, grainCapturePos, grainLookBehind);
				}
				const auto jitteredSpawnPos = [&] (const JitterRuntimeValues& jitterValues,
				                                   int grainLen, float readRate, bool wideMode) noexcept
				{
					double pos = spawnPos + jitterValues.anchorOffsetSamples;
					if (grainFreezeHold)
					{
						const double minPos = grainCapturePos - (double) (inputBufLen_ - 4);
						return juce::jlimit (minPos, grainCapturePos - 4.0, pos);
					}
					const double effectiveLookBehind = computeGrainLookBehind (grainLen, readRate, reverseOn, wideMode);
					return clampGrainSpawnPos (pos, grainCapturePos, effectiveLookBehind);
				};
				const auto jitteredGrainLength = [&] (const JitterRuntimeValues& jitterValues) noexcept
				{
					return juce::jmax (4, (int) std::lround ((float) grainSamples * jitterValues.lengthScale));
				};

				if (isDual)
				{
					for (int dch = 0; dch < 2; ++dch)
					{
						const auto& grainJitter = (dch == 0) ? jitterL : jitterR;
						const int grainLen = jitteredGrainLength (grainJitter);
						for (int attempt = 0; attempt < kMaxGrains; ++attempt)
						{
							const int slot = (grainNextSlot_ + attempt) % kMaxGrains;
							if (! grains_[slot].active)
							{
								auto& g = grains_[slot];
								g.active  = true;
								g.length  = grainLen;
								g.elapsed = 0;
								g.rate    = (dch == 0) ? (double) pitchRate : (double) pitchRateR;
								g.reverse = reverseOn;
								g.readPos = jitteredSpawnPos (grainJitter, grainLen,
								                              (dch == 0) ? pitchRate : pitchRateR, false);
								g.playPos = g.reverse ? (double) (grainLen - 1) : 0.0;
								g.dualCh  = dch;
								grainNextSlot_ = (slot + 1) % kMaxGrains;
								break;
							}
						}
					}
				}
				else if (isWide)
				{
					for (int dch = 0; dch < 2; ++dch)
					{
						const auto& grainJitter = (dch == 0) ? jitterL : jitterR;
						const int grainLen = jitteredGrainLength (grainJitter);
						for (int attempt = 0; attempt < kMaxGrains; ++attempt)
						{
							const int slot = (grainNextSlot_ + attempt) % kMaxGrains;
							if (! grains_[slot].active)
							{
								auto& g = grains_[slot];
								g.active  = true;
								g.length  = grainLen;
								g.elapsed = 0;
								const float wideRate = (dch == 0) ? pitchRate : (basePitchRate * jitterR.pitchScale);
								g.rate    = (double) wideRate;
								g.reverse = reverseOn;
								double rp = jitteredSpawnPos (grainJitter, grainLen, wideRate, dch == 1);
								if (dch == 1)
									rp += (double) (grainLen / 2);
								g.readPos = rp;
								g.playPos = g.reverse ? (double) (grainLen - 1) : 0.0;
								g.dualCh  = dch;
								grainNextSlot_ = (slot + 1) % kMaxGrains;
								break;
							}
						}
					}
				}
				else
				{
					const int grainLen = jitteredGrainLength (jitterL);
					for (int attempt = 0; attempt < kMaxGrains; ++attempt)
					{
						const int slot = (grainNextSlot_ + attempt) % kMaxGrains;
						if (! grains_[slot].active)
						{
							auto& g = grains_[slot];
							g.active  = true;
							g.length  = grainLen;
							g.elapsed = 0;
							g.rate    = (double) pitchRate;
							g.reverse = reverseOn;
							g.readPos = jitteredSpawnPos (jitterL, grainLen, pitchRate, false);
							g.playPos = g.reverse ? (double) (grainLen - 1) : 0.0;
							g.dualCh  = -1;
							grainNextSlot_ = (slot + 1) % kMaxGrains;
							break;
						}
					}
				}

#if JUCE_DEBUG
				GrainDebugEntry dbg {};
				dbg.blockIndex = debugBlockIndex;
				dbg.sampleIndex = i;
				dbg.eventType = 0;
				dbg.amount = amountVal;
				dbg.pitch = pitchVal;
				dbg.speed = speed;
				dbg.pitchRate = pitchRate;
				dbg.windowSamples = grainWindowSamples;
				dbg.grainSamples = grainSamples;
				dbg.density = density;
				dbg.spawnInterval = spawnInterval;
				dbg.style = styleVal;
				dbg.reverseOn = reverseOn ? 1 : 0;
				dbg.triggerOn = triggerOn ? 1 : 0;
				dbg.activeGrains = countActiveGrains();
				dbg.capturePos = grainCapturePos;
				dbg.readPosBefore = spawnPos;
				dbg.spawnPos = spawnPos;
				dbg.readPosAfter = grainReadPos_;
				dbg.lookBehind = grainLookBehind;
				dbg.futureMargin = grainCapturePos - (spawnPos + grainLookBehind - 2.0);
				grainDebugTrace_.record (dbg);
#endif
			};

			const auto mixGranularGrains = [&]()
			{
				float sumL = 0.0f, sumR = 0.0f;
				float sumEnvL = 0.0f, sumEnvR = 0.0f;
				for (int g = 0; g < kMaxGrains; ++g)
				{
					auto& gr = grains_[g];
					if (! gr.active) continue;

					const float phase = (float) gr.elapsed / (float) gr.length;
					const float env = hannWindow (phase);
					const double bufPos = gr.readPos + gr.playPos;

					if (gr.dualCh <= 0)
					{
						sumL += readInputBuf (0, bufPos) * env;
						sumEnvL += env;
					}
					if (gr.dualCh == -1 || gr.dualCh == 1)
					{
						sumR += readInputBuf (1, bufPos) * env;
						sumEnvR += env;
					}

					if (gr.reverse)
						gr.playPos -= gr.rate;
					else
						gr.playPos += gr.rate;

					gr.elapsed++;
					if (gr.elapsed >= gr.length)
						gr.active = false;
				}

				if (sumEnvL > 1.0f)
				{
					const float inv  = 1.0f / sumEnvL;
					const float uncr = std::sqrt (inv);
					sumL *= uncr + speed * (inv - uncr);
				}
				if (sumEnvR > 1.0f)
				{
					const float inv  = 1.0f / sumEnvR;
					const float uncr = std::sqrt (inv);
					sumR *= uncr + speed * (inv - uncr);
				}

				wetL = sumL;
				wetR = sumR;
			};

			if (grainUnity)
			{
				const bool enteringUnityBypass = ! grainUnityBypassActive_ && ! grainTransitionToUnity_;
				if (enteringUnityBypass)
				{
					if (countActiveGrains() > 0)
					{
						grainTransitionToUnity_ = true;
						grainTransitionTotal_ = juce::jlimit (32,
						                                      (int) std::round (currentSampleRate * 0.08),
						                                      juce::jmax ((int) std::round (currentSampleRate * 0.02),
						                                                  juce::jmax (1, grainSamples / 4)));
						grainTransitionRemaining_ = grainTransitionTotal_;
					}
					else
					{
						resetGrainAtCapturePos (grainCapturePos, grainSamples, 1.0f, false, false);
						grainUnityBypassActive_ = true;
						grainTransitionRemaining_ = 0;
						grainTransitionTotal_ = 0;
						grainTransitionToUnity_ = false;
					}
				}

				if (grainTransitionToUnity_ && grainTransitionRemaining_ > 0)
				{
					mixGranularGrains();
					const float progress = 1.0f
						- ((float) grainTransitionRemaining_ / (float) grainTransitionTotal_);
					const float dryMix = std::sin (progress * juce::MathConstants<float>::halfPi);
					const float wetMix = std::cos (progress * juce::MathConstants<float>::halfPi);
					wetL = wetL * wetMix + unityRefL * dryMix;
					wetR = wetR * wetMix + unityRefR * dryMix;
					--grainTransitionRemaining_;

					if (grainTransitionRemaining_ <= 0 || countActiveGrains() <= 0)
					{
						resetGrainAtCapturePos (grainCapturePos, grainSamples, 1.0f, false, false);
						grainUnityBypassActive_ = true;
						grainTransitionRemaining_ = 0;
						grainTransitionTotal_ = 0;
						grainTransitionToUnity_ = false;
					}
				}
				else
				{
					resetGrainAtCapturePos (grainCapturePos, grainSamples, 1.0f, false, false);
					grainUnityBypassActive_ = true;
					grainTransitionRemaining_ = 0;
					grainTransitionTotal_ = 0;
					grainTransitionToUnity_ = false;
					wetL = unityRefL;
					wetR = unityRefR;
				}
			}
			else
			{
				const bool leavingUnityBypass = grainUnityBypassActive_;
				grainUnityBypassActive_ = false;
				grainTransitionToUnity_ = false;
				if (leavingUnityBypass)
				{
					resetGrainAtCapturePos (grainCapturePos, grainSamples, pitchRate, reverseOn, isWide);
					grainTransitionTotal_ = juce::jlimit (48,
					                                      (int) std::round (currentSampleRate * 0.10),
					                                      juce::jmax ((int) std::round (currentSampleRate * 0.03),
					                                                  juce::jmax (1, grainSamples / 3)));
					grainTransitionRemaining_ = grainTransitionTotal_;
				}

				spawnGranularGrains();
				mixGranularGrains();

				if (grainTransitionRemaining_ > 0 && grainTransitionTotal_ > 0)
				{
					const float progress = 1.0f
						- ((float) grainTransitionRemaining_ / (float) grainTransitionTotal_);
					const float wetMix = std::sin (progress * juce::MathConstants<float>::halfPi);
					const float dryMix = std::cos (progress * juce::MathConstants<float>::halfPi);
					wetL = unityRefL * dryMix + wetL * wetMix;
					wetR = unityRefR * dryMix + wetR * wetMix;
					--grainTransitionRemaining_;
				}
			}
		}
		else
		{
			wetL = unityRefL;
			wetR = unityRefR;
		}

#if STRETR_ENABLE_FFT1_CLICK_DUMP
		if (fft1AmountFreezeDumpActiveBlock)
			updateDumpMaxDelta (fftDumpPrevPreStyleWetL_, fftDumpPrevPreStyleWetR_,
			                    wetL, wetR, i,
			                    fft1AmountFreezeMaxPreStyleWetDelta);
		fftDumpPrevPreStyleWetL_ = wetL;
		fftDumpPrevPreStyleWetR_ = wetR;
#endif

		// Style processing (WIDE / DUAL)
		if (numChannels >= 2)
		{
			if (styleVal == 2) // WIDE: M/S boost (on top of per-engine decorrelation)
			{
				const float mid  = (wetL + wetR) * 0.5f;
				const float side = (wetL - wetR) * 0.5f;
				wetL = mid + side * 1.5f;
				wetR = mid - side * 1.5f;
			}
			else if (styleVal == 0) // MONO: collapse to mono
			{
				const float mono = (wetL + wetR) * 0.5f;
				wetL = mono;
				wetR = mono;
			}
			// styleVal == 1 (STEREO) and 3 (DUAL): no change here
			// DUAL and WIDE are handled per-engine (separate L/R processing)
		}
#if STRETR_ENABLE_FFT1_CLICK_DUMP
		if (fft1AmountFreezeDumpActiveBlock)
			updateDumpMaxDelta (fftDumpPrevPostStyleWetL_, fftDumpPrevPostStyleWetR_,
			                    wetL, wetR, i,
			                    fft1AmountFreezeMaxPostStyleWetDelta);
		fftDumpPrevPostStyleWetL_ = wetL;
		fftDumpPrevPostStyleWetR_ = wetR;
#endif

		// Chaos engines
		if (chaosFilterEnabled_) advanceChaosF();
		if (chaosDelayEnabled_)  advanceChaosD();

		// Wet-signal filter + tilt
		if (!tiltPre_)   tiltWetSample   (wetL, wetR);
		if (!filterPre_) filterWetSample (wetL, wetR);
#if STRETR_ENABLE_FFT1_CLICK_DUMP
		if (fft1AmountFreezeDumpActiveBlock)
			updateDumpMaxDelta (fftDumpPrevPostFilterWetL_, fftDumpPrevPostFilterWetR_,
			                    wetL, wetR, i,
			                    fft1AmountFreezeMaxPostFilterWetDelta);
		fftDumpPrevPostFilterWetL_ = wetL;
		fftDumpPrevPostFilterWetR_ = wetR;
#endif

		// Chaos delay (per-channel smooth S&H micro-delay + gain modulation)
		if (chaosDelayEnabled_
		    && (chaosAmtD_ > 0.01f || (chaosDriveParamSmoothReady_ && chaosDriveAmtSmoothed_ > 0.01f)))
			applyChaosDelay (wetL, wetR);
#if STRETR_ENABLE_FFT1_CLICK_DUMP
		if (fft1AmountFreezeDumpActiveBlock)
			updateDumpMaxDelta (fftDumpPrevPostChaosWetL_, fftDumpPrevPostChaosWetR_,
			                    wetL, wetR, i,
			                    fft1AmountFreezeMaxPostChaosWetDelta);
		fftDumpPrevPostChaosWetL_ = wetL;
		fftDumpPrevPostChaosWetR_ = wetR;
#endif

		// Mode Out: MID stays dual-mono, SIDE becomes true stereo (+S / -S)
		if (numChannels >= 2 && modeOutVal != 0)
		{
			const float mono = (wetL + wetR) * 0.5f;
			if (modeOutVal == 1)
			{
				wetL = mono;
				wetR = mono;
			}
			else /* modeOutVal == 2 */
			{
				wetL = mono;
				wetR = -mono;
			}
		}
#if STRETR_ENABLE_FFT1_CLICK_DUMP
		if (fft1AmountFreezeDumpActiveBlock)
			updateDumpMaxDelta (fftDumpPrevPreDcWetL_, fftDumpPrevPreDcWetR_,
			                    wetL, wetR, i,
			                    fft1AmountFreezeMaxPreDcWetDelta);
		fftDumpPrevPreDcWetL_ = wetL;
		fftDumpPrevPreDcWetR_ = wetR;
#endif

		// DC blocker (1-pole HP ~5 Hz)
		{
#if STRETR_ENABLE_FFT1_CLICK_DUMP
			const float prevDcInL = dcBlockPrevIn_[0];
			const float prevDcInR = dcBlockPrevIn_[1];
			const float prevDcOutL = dcBlockPrevOut_[0];
			const float prevDcOutR = dcBlockPrevOut_[1];
			const float dcInputL = wetL;
			const float dcInputR = wetR;
#endif
			const float outL = wetL - dcBlockPrevIn_[0] + dcBlockR_ * dcBlockPrevOut_[0];
			const float outR = wetR - dcBlockPrevIn_[1] + dcBlockR_ * dcBlockPrevOut_[1];
#if STRETR_ENABLE_FFT1_CLICK_DUMP
			if (fft1AmountFreezeDumpActiveBlock
			    && updateDumpMaxDelta (fftDumpPrevPostDcWetL_, fftDumpPrevPostDcWetR_,
			                           outL, outR, i,
			                           fft1AmountFreezeMaxPostDcWetDelta))
			{
				fft1AmountFreezeMaxPostDcPrevDcInL = prevDcInL;
				fft1AmountFreezeMaxPostDcPrevDcInR = prevDcInR;
				fft1AmountFreezeMaxPostDcPrevDcOutL = prevDcOutL;
				fft1AmountFreezeMaxPostDcPrevDcOutR = prevDcOutR;
				fft1AmountFreezeMaxPostDcInputL = dcInputL;
				fft1AmountFreezeMaxPostDcInputR = dcInputR;
			}
#endif
			dcBlockPrevIn_[0] = wetL;  dcBlockPrevIn_[1] = wetR;
			dcBlockPrevOut_[0] = outL; dcBlockPrevOut_[1] = outR;
			wetL = outL;
			wetR = outR;
		}
#if STRETR_ENABLE_FFT1_CLICK_DUMP
		fftDumpPrevPostDcWetL_ = wetL;
		fftDumpPrevPostDcWetR_ = wetR;
		if (fft1AmountFreezeDumpActiveBlock)
		{
			updateDumpMaxDelta (fftDumpPrevEngineWetL_, fftDumpPrevEngineWetR_,
			                    wetL, wetR, i,
			                    fft1AmountFreezeMaxEngineWetDeltaSample,
			                    fft1AmountFreezeMaxEngineWetAbsDeltaL,
			                    fft1AmountFreezeMaxEngineWetAbsDeltaR,
			                    fft1AmountFreezeMaxEngineWetPrevL,
			                    fft1AmountFreezeMaxEngineWetPrevR,
			                    fft1AmountFreezeMaxEngineWetCurrL,
			                    fft1AmountFreezeMaxEngineWetCurrR);
			fft1AmountFreezeEngineWetSqL += (double) wetL * (double) wetL;
			fft1AmountFreezeEngineWetSqR += (double) wetR * (double) wetR;
			fft1AmountFreezeEngineWetPeakL = juce::jmax (fft1AmountFreezeEngineWetPeakL, std::abs (wetL));
			fft1AmountFreezeEngineWetPeakR = juce::jmax (fft1AmountFreezeEngineWetPeakR, std::abs (wetR));
		}
		fftDumpPrevEngineWetL_ = wetL;
		fftDumpPrevEngineWetR_ = wetR;
#endif

		// Mix dry/wet with Sum Bus routing
		float dG, wG;
		if (mixMode == 0) { dG = 1.0f - smoothedMix; wG = smoothedMix; }
		else              { dG = dryLevelState; wG = wetLevelState; }
		const float dL = dryOrigL * dG;
		const float dR = dryOrigR * dG;
		float wL = wetL * smoothedOutputGain;
		float wR = wetR * smoothedOutputGain;
		if (limMode == 1)
			applyLimiterSample (wL, wR, limThreshLinState);

		// Invert Polarity / Stereo (WET mode: after Limiter WET)
		if (invPol == 1) { wL = -wL; wR = -wR; }
		if (invStr == 1 && numChannels >= 2) std::swap (wL, wR);

		wL *= wG;
		wR *= wG;
#if STRETR_ENABLE_FFT1_CLICK_DUMP
		if (fft1AmountFreezeDumpActiveBlock)
		{
			updateDumpMaxDelta (fftDumpPrevFinalWetL_, fftDumpPrevFinalWetR_,
			                    wL, wR, i,
			                    fft1AmountFreezeMaxFinalWetDeltaSample,
			                    fft1AmountFreezeMaxFinalWetAbsDeltaL,
			                    fft1AmountFreezeMaxFinalWetAbsDeltaR,
			                    fft1AmountFreezeMaxFinalWetPrevL,
			                    fft1AmountFreezeMaxFinalWetPrevR,
			                    fft1AmountFreezeMaxFinalWetCurrL,
			                    fft1AmountFreezeMaxFinalWetCurrR);
			fft1AmountFreezeFinalWetSqL += (double) wL * (double) wL;
			fft1AmountFreezeFinalWetSqR += (double) wR * (double) wR;
			fft1AmountFreezeFinalWetPeakL = juce::jmax (fft1AmountFreezeFinalWetPeakL, std::abs (wL));
			fft1AmountFreezeFinalWetPeakR = juce::jmax (fft1AmountFreezeFinalWetPeakR, std::abs (wR));
		}
		fftDumpPrevFinalWetL_ = wL;
		fftDumpPrevFinalWetR_ = wR;
#endif
		if (sumBusVal == 0) // ST
		{
			channelL[i] = dL + wL;
			if (channelR != nullptr) channelR[i] = dR + wR;
		}
        else if (sumBusVal == 1) // ->M
		{
			const float midBus = (wL + wR) * 0.5f;
			channelL[i] = dL + midBus;
			if (channelR != nullptr) channelR[i] = dR + midBus;
		}
        else // ->S
		{
			const float sideBus = (wL - wR) * 0.5f;
			channelL[i] = dL + sideBus;
			if (channelR != nullptr) channelR[i] = dR - sideBus;
		}
#if STRETR_ENABLE_FFT1_CLICK_DUMP
		if (fft1AmountFreezeDumpActiveBlock)
		{
			const float outDumpL = channelL[i];
			const float outDumpR = (channelR != nullptr) ? channelR[i] : outDumpL;
			updateDumpMaxDelta (fftDumpPrevOutL_, fftDumpPrevOutR_,
			                    outDumpL, outDumpR, i,
			                    fft1AmountFreezeMaxOutDeltaSample,
			                    fft1AmountFreezeMaxOutAbsDeltaL,
			                    fft1AmountFreezeMaxOutAbsDeltaR,
			                    fft1AmountFreezeMaxOutPrevL,
			                    fft1AmountFreezeMaxOutPrevR,
			                    fft1AmountFreezeMaxOutCurrL,
			                    fft1AmountFreezeMaxOutCurrR);
			fft1AmountFreezeOutSqL += (double) outDumpL * (double) outDumpL;
			fft1AmountFreezeOutSqR += (double) outDumpR * (double) outDumpR;
			fft1AmountFreezeOutPeakL = juce::jmax (fft1AmountFreezeOutPeakL, std::abs (outDumpL));
			fft1AmountFreezeOutPeakR = juce::jmax (fft1AmountFreezeOutPeakR, std::abs (outDumpR));
			fftDumpPrevOutL_ = outDumpL;
			fftDumpPrevOutR_ = outDumpR;
		}
		else
		{
			fftDumpPrevOutL_ = channelL[i];
			fftDumpPrevOutR_ = (channelR != nullptr) ? channelR[i] : channelL[i];
		}
#endif
	}

	smoothedDryLevel = dryLevelState;
	smoothedWetLevel = wetLevelState;
	smoothedLimThreshold = limThreshLinState;

	// Pan
	{
		const float targetPanLeft  = std::cos (panValue * juce::MathConstants<float>::halfPi);
		const float targetPanRight = std::sin (panValue * juce::MathConstants<float>::halfPi);
		if (numChannels >= 2 && (std::abs (panValue - 0.5f) > 0.001f || std::abs (lastPan_ - 0.5f) > 0.001f))
		{
			for (int i = 0; i < numSamples; ++i)
			{
				lastPanLeft_  += (targetPanLeft  - lastPanLeft_)  * kGainSmoothStep;
				lastPanRight_ += (targetPanRight - lastPanRight_) * kGainSmoothStep;
				channelL[i] *= lastPanLeft_  * 1.4142135f;
				channelR[i] *= lastPanRight_ * 1.4142135f;
			}
		}
		lastPan_ = panValue;
	}

    // Transparent Peak Limiter (GLOBAL: after pan, before safety)
	if (limMode == 2)
	{
		float* left  = buffer.getWritePointer (0);
		float* right = numChannels >= 2 ? buffer.getWritePointer (1) : nullptr;
		if (right != nullptr)
			applyLimiter (left, right, numSamples, limThreshLinStart, smoothedLimThreshold);
		else
		{
			const float thresholdStep = (numSamples > 1)
				? (smoothedLimThreshold - limThreshLinStart) / (float) (numSamples - 1)
				: 0.0f;
			float thresholdGain = limThreshLinStart;
			for (int i = 0; i < numSamples; ++i)
			{
				float dummy = 0.0f;
				applyLimiterSample (left[i], dummy, thresholdGain);
				thresholdGain += thresholdStep;
			}
		}
	}

    // Invert Polarity / Stereo (GLOBAL mode: after Limiter GLOBAL, before safety)
	if (invPol == 2)
		for (int ch = 0; ch < numChannels; ++ch)
			juce::FloatVectorOperations::multiply (buffer.getWritePointer (ch), -1.0f, numSamples);
	if (invStr == 2 && numChannels >= 2)
	{
		float* sL = buffer.getWritePointer (0);
		float* sR = buffer.getWritePointer (1);
		for (int n = 0; n < numSamples; ++n)
			std::swap (sL[n], sR[n]);
	}

	// Safety hard-limiter (+48 dBFS runway protection)
	for (int ch = 0; ch < numChannels; ++ch)
	{
		float* data = buffer.getWritePointer (ch);
		juce::FloatVectorOperations::clip (data, data, -251.19f, 251.19f, numSamples);
	}

	{
		float* left = buffer.getWritePointer (0);
		float* right = (numChannels >= 2) ? buffer.getWritePointer (1) : nullptr;

		for (int i = 0; i < numSamples; ++i)
		{
			fftOutputPadBuf_[0][fftOutputPadWritePos_] = left[i];
			const int readPos = (fftOutputPadWritePos_ - fftOutputPadLen + kMaxFftSize) & (kMaxFftSize - 1);
			left[i] = fftOutputPadBuf_[0][readPos];

			if (right != nullptr)
			{
				fftOutputPadBuf_[1][fftOutputPadWritePos_] = right[i];
				right[i] = fftOutputPadBuf_[1][readPos];
			}

			fftOutputPadWritePos_ = (fftOutputPadWritePos_ + 1) & (kMaxFftSize - 1);
		}
	}

	{
		float* left = buffer.getWritePointer (0);
		float* right = (numChannels >= 2) ? buffer.getWritePointer (1) : nullptr;

	   #if JUCE_DEBUG
		auto recordFftOutputTrace = [&] (int eventType, int sampleIndex, float preFadeL, float outL)
		{
			if (! fftDebugEnabled || ! triggerOn || (engineVal != 2 && engineVal != 3) || stft_.activeFftSize <= 0)
				return;

			FftDebugEntry dbg {};
			dbg.blockIndex = debugBlockIndex;
			dbg.sampleIndex = sampleIndex;
			dbg.eventType = eventType;
			dbg.engine = engineVal;
			dbg.amount = amountVal;
			dbg.pitch = pitchVal;
			dbg.speed = smoothedSpeed_;
			dbg.pitchRate = smoothedPitchRate_;
			dbg.windowSamples = (engineVal == 3) ? fft2GeometryWindowSamples : windowSamples;
			dbg.fftSize = stft_.activeFftSize;
			dbg.analysisHop = stft_.lastAnalysisHop;
			dbg.analysisHopQuantError = fftDebugContext_.analysisHopQuantError;
			dbg.lastAnalysisHop = fftDebugContext_.lastAnalysisHop;
			dbg.freezeEntryWarmupCycles = fftDebugContext_.freezeEntryWarmupCycles;
			dbg.fftStartupWarmupRemainingCycles = fftDebugContext_.fftStartupWarmupRemainingCycles;
			dbg.fftExplicitFreezeActive = fftDebugContext_.fftExplicitFreezeActive;
			dbg.fftExplicitFreezeCapturePending = fftDebugContext_.fftExplicitFreezeCapturePending;
			dbg.fftTargetFreeze = fftDebugContext_.fftTargetFreeze;
			dbg.synthesisHop = recommendedFftSynthHop (stft_.activeFftSize);
			dbg.style = styleVal;
			dbg.reverseOn = reverseOn ? 1 : 0;
			dbg.triggerOn = 1;
			dbg.wideMode = (styleVal == 2 && numChannels >= 2) ? 1 : 0;
			dbg.analysisReadBefore = stft_.analysisReadPos;
			dbg.analysisReadAfter = stft_.analysisReadPos;
			dbg.cyclesSinceReset = stft_.cyclesSinceReset;
			dbg.alignOn = alignOn ? 1 : 0;
			dbg.pdcOn = pdcOn ? 1 : 0;
			dbg.reportedLatency = reportedLatency;
			dbg.dryDelayLen = dryDelayLen_;
			dbg.fftOutputPadLen = fftOutputPadLen;
			dbg.smoothedWindow = (engineVal == 3) ? (float) fft2GeometryWindowSamples : smoothedWindow_;
			dbg.targetWindow = targetWindow;
			dbg.windowTransitionActive = isWindowTransitionActiveForEngine (engineVal) ? 1 : 0;
			dbg.fftOutputFadeActive = (fftOutputFadePos_ > 0 && fftOutputFadeTotal_ > 0) ? 1 : 0;
			dbg.fftWetPreOutputFadeL = preFadeL;
			dbg.fftWetPostOutputFadeL = outL;
			dbg.fftWetPostOutputDeltaL = outL - fftPrevWetPostOutputL_;
			dbg.rawWindowChanged = fftRawWindowChanged ? 1 : 0;
			dbg.rawAmountChanged = fftRawAmountChanged ? 1 : 0;
			dbg.fftWindowMotionActive = fftWindowMotionActiveBlock ? 1 : 0;
			dbg.fftWindowApplyDelayRemaining = fftWindowApplyDelayRemaining_;
			dbg.fftWindowCaptureRemaining = fftWindowCaptureRemaining_;
			dbg.fftDuckGain = fftParamDuckGain_;
			fftDebugTrace_.record (dbg);
		};
	   #else
		auto recordFftOutputTrace = [&] (int, int, float, float) {};
	   #endif

		for (int i = 0; i < numSamples; ++i)
		{
			float outL = left[i];
			float outR = (right != nullptr) ? right[i] : outL;
			const float preFadeL = outL;
			if (fftDebugEnabled)
			{
				fftDebugContext_.engineFadeOldOutL = 0.0f;
				fftDebugContext_.engineFadeOldMix = 0.0f;
				fftDebugContext_.engineFadeNewMix = 0.0f;
				fftDebugContext_.fftOutputFadeOldOutL = 0.0f;
				fftDebugContext_.fftOutputFadeOldMix = 0.0f;
				fftDebugContext_.fftOutputFadeNewMix = 0.0f;
			}

			if (engineFadePos_ > 0 && engineFadeTotal_ > 0)
			{
				const int fadeSamples = juce::jmax (1, engineFadeTotal_ - engineFadeHoldSamples_);
				float newMix = 0.0f;
				float oldMix = 0.0f;
				float oldOutL = engineFadeStartL_;
				float oldOutR = engineFadeStartR_;
				if (engineFadePos_ > fadeSamples && engineFadeHoldSamples_ > 0)
				{
					const float holdProgress = juce::jlimit (0.0f, 1.0f,
						1.0f - (float) (engineFadePos_ - fadeSamples) / (float) engineFadeHoldSamples_);
					const float fadeOut = holdProgress * holdProgress * (3.0f - 2.0f * holdProgress);
					oldMix = 1.0f - fadeOut;
				}
				else if (engineFadePos_ <= fadeSamples)
				{
					const float progress = juce::jlimit (0.0f, 1.0f,
						1.0f - (float) engineFadePos_ / (float) fadeSamples);
					newMix = progress * progress * (3.0f - 2.0f * progress);
				}
				if (fftDebugEnabled)
				{
					fftDebugContext_.engineFadeOldOutL = oldOutL;
					fftDebugContext_.engineFadeOldMix = oldMix;
					fftDebugContext_.engineFadeNewMix = newMix;
				}
				outL = oldOutL * oldMix + outL * newMix;
				outR = oldOutR * oldMix + outR * newMix;
				--engineFadePos_;
				if (engineFadePos_ <= 0)
				{
					engineFadePos_ = 0;
					engineFadeTotal_ = 0;
					engineFadeHoldSamples_ = 0;
				}
			}
			else if (triggerOn && (engineVal == 2 || engineVal == 3) && fftOutputFadePos_ > 0 && fftOutputFadeTotal_ > 0
			         && ! fftWindowMotionActiveBlock)
			{
				const int fadeSamples = juce::jmax (1, fftOutputFadeTotal_ - fftOutputFadeHoldSamples_);
				const float newMix = juce::jlimit (0.0f, 1.0f,
					1.0f - (float) fftOutputFadePos_ / (float) fadeSamples);
				const float oldMix = 1.0f - newMix;
				const int histIdx = fftOutputFadeReadPos_ & (kWetOutputHistoryLen - 1);
				const float oldOutL = wetOutputHistory_[0][histIdx];
				const float oldOutR = wetOutputHistory_[1][histIdx];
				if (fftDebugEnabled)
				{
					fftDebugContext_.fftOutputFadeOldOutL = oldOutL;
					fftDebugContext_.fftOutputFadeOldMix = oldMix;
					fftDebugContext_.fftOutputFadeNewMix = newMix;
				}
				outL = oldOutL * oldMix + outL * newMix;
				outR = oldOutR * oldMix + outR * newMix;
				fftOutputFadeReadPos_ = (fftOutputFadeReadPos_ + 1) & (kWetOutputHistoryLen - 1);
				--fftOutputFadePos_;
				if (fftOutputFadePos_ <= 0)
				{
					fftOutputFadePos_ = 0;
					fftOutputFadeTotal_ = 0;
					fftOutputFadeHoldSamples_ = 0;
				}
			}
			else if (fftWindowMotionActiveBlock && (engineVal == 2 || engineVal == 3))
			{
				fftOutputFadePos_ = 0;
				fftOutputFadeTotal_ = 0;
				fftOutputFadeHoldSamples_ = 0;
			}

			const bool fftDuckRuntimeActive = (engineVal == 2 || engineVal == 3)
				&& ((triggerOn && fftWindowMotionActiveBlock)
				    || fftParamDuckHoldRemaining_ > 0
				    || fftParamDuckGain_ < 0.9999f);
			if (fftDuckRuntimeActive)
			{
				const float fftDuckTarget = (fftParamDuckHoldRemaining_ > 0) ? 0.0f : 1.0f;
				const float fftDuckStep = (fftDuckTarget < fftParamDuckGain_)
					? fftDuckAttackStepBlock
					: fftDuckReleaseStepBlock;
				fftParamDuckGain_ += (fftDuckTarget - fftParamDuckGain_) * fftDuckStep;
				outL *= fftParamDuckGain_;
				outR *= fftParamDuckGain_;
				if (fftParamDuckHoldRemaining_ > 0)
					--fftParamDuckHoldRemaining_;
			}

			if ((engineVal == 2 || engineVal == 3) && fftDuckBridgeRemaining_ > 0 && fftDuckBridgeTotal_ > 0)
			{
				const int bridgeDenom = juce::jmax (1, fftDuckBridgeTotal_ - 1);
				const float bridgePhase = (float) juce::jmax (0, fftDuckBridgeRemaining_ - 1) / (float) bridgeDenom;
				const float bridgeGain = bridgePhase * bridgePhase * (3.0f - 2.0f * bridgePhase);
				outL += fftDuckBridgeStartL_ * bridgeGain;
				outR += fftDuckBridgeStartR_ * bridgeGain;
				--fftDuckBridgeRemaining_;
				if (fftDuckBridgeRemaining_ <= 0)
				{
					fftDuckBridgeRemaining_ = 0;
					fftDuckBridgeTotal_ = 0;
				}
			}

			if (fftDebugEnabled)
			{
				fftDebugContext_.fftWetPreOutputFadeL = preFadeL;
				fftDebugContext_.fftWetPostOutputFadeL = outL;
			}
			if (engineVal == 2 || engineVal == 3)
			{
				if (fftDebugEnabled)
					fftDebugContext_.fftWetPostOutputDeltaL = outL - fftPrevWetPostOutputL_;
				if (fftWindowTraceRemaining_ > 0)
				{
					recordFftOutputTrace (7, i, preFadeL, outL);
					--fftWindowTraceRemaining_;
				}
				if (fftAmountTraceRemaining_ > 0)
				{
					recordFftOutputTrace (8, i, preFadeL, outL);
					--fftAmountTraceRemaining_;
				}
				if (engineVal == 2 && fft1ReentryTraceRemaining_ > 0)
				{
					recordFftOutputTrace (9, i, preFadeL, outL);
					--fft1ReentryTraceRemaining_;
				}
				fftPrevWetPostOutputL_ = outL;
			}

#if STRETR_ENABLE_FFT1_CLICK_DUMP
			if (fft1AmountFreezeDumpActiveBlock)
			{
				updateDumpMaxDelta (fftDumpPrevPostDuckOutL_, fftDumpPrevPostDuckOutR_,
				                    outL, outR, i,
				                    fft1AmountFreezeMaxPostDuckOutDeltaSample,
				                    fft1AmountFreezeMaxPostDuckOutAbsDeltaL,
				                    fft1AmountFreezeMaxPostDuckOutAbsDeltaR,
				                    fft1AmountFreezeMaxPostDuckOutPrevL,
				                    fft1AmountFreezeMaxPostDuckOutPrevR,
				                    fft1AmountFreezeMaxPostDuckOutCurrL,
				                    fft1AmountFreezeMaxPostDuckOutCurrR);
				fft1AmountFreezePostDuckOutSqL += (double) outL * (double) outL;
				fft1AmountFreezePostDuckOutSqR += (double) outR * (double) outR;
				fft1AmountFreezePostDuckOutPeakL = juce::jmax (fft1AmountFreezePostDuckOutPeakL, std::abs (outL));
				fft1AmountFreezePostDuckOutPeakR = juce::jmax (fft1AmountFreezePostDuckOutPeakR, std::abs (outR));
			}
			fftDumpPrevPostDuckOutL_ = outL;
			fftDumpPrevPostDuckOutR_ = outR;
#endif

			left[i] = outL;
			if (right != nullptr)
				right[i] = outR;

			fftLastPostDuckOutL_ = outL;
			fftLastPostDuckOutR_ = outR;
			wetOutputHistory_[0][wetOutputHistoryWritePos_] = outL;
			wetOutputHistory_[1][wetOutputHistoryWritePos_] = outR;
			wetOutputHistoryWritePos_ = (wetOutputHistoryWritePos_ + 1) & (kWetOutputHistoryLen - 1);
		}
	}

#if STRETR_ENABLE_FFT1_CLICK_DUMP
	if (fft1AmountFreezeDumpActiveBlock)
	{
		const float invCount = 1.0f / (float) juce::jmax (1, numSamples);
		const bool fftTargetFreeze = ((engineVal == 2 && ! reverseOn) || engineVal == 3)
			&& (smoothedSpeed_ <= 0.0001f)
			&& ((engineVal != 3) || (fft2HoldCoeffSmoothed_ >= 0.999f));
		Fft1AmountFreezeDumpEntry dbg {};
		dbg.blockIndex = fft1AmountFreezeDumpBlockIndex;
		dbg.engine = engineVal;
		dbg.triggerOn = triggerOn ? 1 : 0;
		dbg.alignOn = alignOn ? 1 : 0;
		dbg.pdcOn = pdcOn ? 1 : 0;
		dbg.triggerEdge = fft1AmountFreezeDumpTriggerEdge;
		dbg.fftWindowMotionActive = fftWindowMotionActiveBlock ? 1 : 0;
		dbg.fftAmountMotionActive = (fftAmountMovedThisBlockForDump || fftAmountTraceRemaining_ > 0) ? 1 : 0;
		dbg.fftSizeChanged = fftSizeChanged ? 1 : 0;
		dbg.fftOutputFadePos = fftOutputFadePos_;
		dbg.fftOutputFadeTotal = fftOutputFadeTotal_;
		dbg.fftDuckHoldStart = fft1AmountFreezeDumpDuckHoldStart;
		dbg.fftDuckHoldEnd = fftParamDuckHoldRemaining_;
		dbg.fftDuckBridgeRemaining = fftDuckBridgeRemaining_;
		dbg.fftDuckBridgeTotal = fftDuckBridgeTotal_;
		dbg.fftDuckGainStart = fft1AmountFreezeDumpDuckGainStart;
		dbg.fftDuckGainEnd = fftParamDuckGain_;
		dbg.style = styleVal;
		dbg.reverseOn = reverseOn ? 1 : 0;
		dbg.wideMode = (styleVal == 2 && numChannels >= 2) ? 1 : 0;
		dbg.dualMode = (styleVal == 3 && numChannels >= 2) ? 1 : 0;
		dbg.amount = amountVal;
		dbg.pitch = pitchVal;
		dbg.speed = targetSpeed;
		dbg.smoothedSpeed = smoothedSpeed_;
		dbg.pitchRate = targetPitchRate;
		const float dumpJitterAmountNorm = juce::jlimit (0.0f, 1.0f, 1.0f - smoothedSpeed_);
		const float dumpJitterAmountScale = (dumpJitterAmountNorm > 1.0e-5f) ? std::sqrt (dumpJitterAmountNorm) : 0.0f;
		const float dumpJitterReferenceSamples = (float) ((engineVal == 3) ? fft2GeometryWindowSamples : windowSamples);
		const bool dumpStretchJitterActive = (engineVal == 0) && (jitterSmoothed_ > 1.0e-5f);
		const bool dumpFftJitterActive = (engineVal == 2 || engineVal == 3) && (jitterSmoothed_ > 1.0e-5f);
		const float dumpMotionAmountScale = (dumpStretchJitterActive || dumpFftJitterActive) ? 0.0f : dumpJitterAmountScale;
		const auto dumpJitterL = makeJitterRuntimeValues (0, dumpJitterReferenceSamples, 1.0f, dumpMotionAmountScale, false);
		const auto dumpJitterR = makeJitterRuntimeValues ((styleVal == 0) ? 0 : 1,
		                                                  dumpJitterReferenceSamples, 1.0f, dumpMotionAmountScale, false);
		auto dumpStretchJitterL = dumpStretchJitterActive ? makeStretchJitterRuntimeValues (0) : JitterRuntimeValues {};
		auto dumpStretchJitterR = dumpStretchJitterActive
			? makeStretchJitterRuntimeValues ((styleVal == 0) ? 0 : 1)
			: JitterRuntimeValues {};
		if (dumpStretchJitterActive)
		{
			dumpStretchJitterL.pitchScale = stretchJitterPitchScaleSmoothed_[0];
			dumpStretchJitterR.pitchScale = stretchJitterPitchScaleSmoothed_[1];
		}
		const auto dumpFftJitterL = dumpFftJitterActive ? makeFftJitterRuntimeValues (0) : JitterRuntimeValues {};
		const auto dumpFftJitterR = dumpFftJitterActive
			? makeFftJitterRuntimeValues ((styleVal == 0) ? 0 : 1)
			: JitterRuntimeValues {};
		const auto& dumpEffectiveJitterL = dumpFftJitterActive ? dumpFftJitterL : (dumpStretchJitterActive ? dumpStretchJitterL : dumpJitterL);
		const auto& dumpEffectiveJitterR = dumpFftJitterActive ? dumpFftJitterR : (dumpStretchJitterActive ? dumpStretchJitterR : dumpJitterR);
		dbg.jitterTarget = jitterTarget;
		dbg.jitterSmoothed = jitterSmoothed_;
		dbg.jitterAmountScale = dumpMotionAmountScale;
		dbg.effectivePitchRateL = smoothedPitchRate_ * dumpEffectiveJitterL.pitchScale;
		dbg.effectivePitchRateR = ((styleVal == 3 && numChannels >= 2)
			? smoothedPitchRate_ * 0.5f
			: smoothedPitchRate_) * dumpEffectiveJitterR.pitchScale;
		dbg.windowSamples = (engineVal == 3) ? fft2GeometryWindowSamples : windowSamples;
		dbg.fftSize = stft_.activeFftSize;
		dbg.rawWindowParam = rawWindowParamVal;
		dbg.storedWindow = windowVal;
		dbg.effectiveWindow = effectiveWindowVal;
		dbg.targetWindow = targetWindow;
		dbg.smoothedWindow = smoothedWindow_;
		dbg.capturedWindow = fftCapturedWindowVal_;
		dbg.pendingWindow = fftPendingWindowVal_;
		dbg.fftWindowCaptureRemaining = fftWindowCaptureRemaining_;
		dbg.fftWindowApplyDelayRemaining = fftWindowApplyDelayRemaining_;
		dbg.fft2GeometryWindow = fft2GeometryWindowSamples;
		dbg.fft2GeometryLog2Window = fft2GeometryLog2Window_;
		dbg.desiredFftSize = desiredFftSizeForDump;
		dbg.requestedFftSize = requestedFftSize;
		dbg.previousFftSize = previousFftSizeForDump;
		dbg.activeFftSize = stft_.activeFftSize;
		dbg.fft2AmountZeroHoldBypassActive = fft2AmountZeroHoldBypassActive_ ? 1 : 0;
		dbg.fft2TargetFullHold = (engineVal == 3 && targetSpeed <= 0.0001f) ? 1 : 0;
		dbg.fft2SmoothedFullHold = (engineVal == 3
			&& smoothedSpeed_ <= 0.0001f
			&& fft2HoldCoeffSmoothed_ >= 0.999f) ? 1 : 0;
		dbg.reportedLatency = reportedLatency;
		dbg.dryDelayLen = dryDelayLen_;
		dbg.fftTargetFreeze = fftTargetFreeze ? 1 : 0;
		dbg.fftExplicitFreezeActive = fftExplicitFreezeActive_ ? 1 : 0;
		dbg.fftExplicitFreezeCapturePending = fftExplicitFreezeCapturePending_ ? 1 : 0;
		dbg.lastAnalysisHop = stft_.lastAnalysisHop;
		dbg.freezeEntryWarmupCycles = stft_.freezeEntryWarmupCycles;
		dbg.fftTransitionRemaining = fftTransitionRemaining_;
		dbg.fftTransitionTotal = fftTransitionTotal_;
		dbg.fftFreezeTransitionRemaining = fftFreezeTransitionRemaining_;
		dbg.fftFreezeTransitionTotal = fftFreezeTransitionTotal_;
		dbg.windowTransitionRemaining = getWindowTransitionRemainingForEngine (engineVal);
		dbg.windowTransitionTotal = getWindowTransitionTotalForEngine (engineVal);
		dbg.fftUnityBypassActive = fftUnityBypassActive_ ? 1 : 0;
		dbg.fftTransitionToUnity = fftTransitionToUnity_ ? 1 : 0;
		dbg.fft1AmountUnityBypassActive = fft1AmountUnityBypassActive_ ? 1 : 0;
		dbg.fft2HoldCoeff = fft2HoldCoeffSmoothed_;
		dbg.fft2TargetHoldCoeff = std::sqrt (std::sqrt (juce::jlimit (0.0f, 1.0f, 1.0f - targetSpeed)));
		dbg.fftCycleCount = juce::jmax (0, fftDebugContext_.fftCycleSerial - fft1AmountFreezeDumpCycleSerialStart);
		if (dbg.fftCycleCount > 0)
		{
			dbg.fftRuntimeRoute = fftDebugContext_.fftRuntimeRoute;
			dbg.fft1FreezeHoldRoute = fftDebugContext_.fft1FreezeHoldRoute;
			dbg.signedAnalysisHop = fftDebugContext_.signedAnalysisHop;
			dbg.freezeAnalysisInput = fftDebugContext_.freezeAnalysisInput;
			dbg.spectralHoldCoeff = fftDebugContext_.spectralHoldCoeff;
			dbg.analysisReadBefore = fftDebugContext_.analysisReadBefore;
			dbg.analysisReadAfter = fftDebugContext_.analysisReadAfter;
			dbg.analysisReadDelta = dbg.analysisReadAfter - dbg.analysisReadBefore;
			if (inputBufLen_ > 0)
			{
				const double halfLen = (double) inputBufLen_ * 0.5;
				if (dbg.analysisReadDelta > halfLen)
					dbg.analysisReadDelta -= (double) inputBufLen_;
				else if (dbg.analysisReadDelta < -halfLen)
					dbg.analysisReadDelta += (double) inputBufLen_;
			}
		}
		dbg.engineWetRmsL = std::sqrt ((float) (fft1AmountFreezeEngineWetSqL * invCount));
		dbg.engineWetRmsR = std::sqrt ((float) (fft1AmountFreezeEngineWetSqR * invCount));
		dbg.engineWetPeakL = fft1AmountFreezeEngineWetPeakL;
		dbg.engineWetPeakR = fft1AmountFreezeEngineWetPeakR;
		dbg.finalWetRmsL = std::sqrt ((float) (fft1AmountFreezeFinalWetSqL * invCount));
		dbg.finalWetRmsR = std::sqrt ((float) (fft1AmountFreezeFinalWetSqR * invCount));
		dbg.finalWetPeakL = fft1AmountFreezeFinalWetPeakL;
		dbg.finalWetPeakR = fft1AmountFreezeFinalWetPeakR;
		dbg.outRmsL = std::sqrt ((float) (fft1AmountFreezeOutSqL * invCount));
		dbg.outRmsR = std::sqrt ((float) (fft1AmountFreezeOutSqR * invCount));
		dbg.outPeakL = fft1AmountFreezeOutPeakL;
		dbg.outPeakR = fft1AmountFreezeOutPeakR;
		dbg.postDuckOutRmsL = std::sqrt ((float) (fft1AmountFreezePostDuckOutSqL * invCount));
		dbg.postDuckOutRmsR = std::sqrt ((float) (fft1AmountFreezePostDuckOutSqR * invCount));
		dbg.postDuckOutPeakL = fft1AmountFreezePostDuckOutPeakL;
		dbg.postDuckOutPeakR = fft1AmountFreezePostDuckOutPeakR;
		dbg.modeIn = modeInVal;
		dbg.modeOut = modeOutVal;
		dbg.sumBus = sumBusVal;
		dbg.mixMode = mixMode;
		dbg.filterPre = filterPre_ ? 1 : 0;
		dbg.tiltPre = tiltPre_ ? 1 : 0;
		dbg.wetFilterHpOn = wetFilterHpOn_ ? 1 : 0;
		dbg.wetFilterLpOn = wetFilterLpOn_ ? 1 : 0;
		dbg.chaosFilterOn = chaosFilterEnabled_ ? 1 : 0;
		dbg.chaosDelayOn = chaosDelayEnabled_ ? 1 : 0;
		dbg.chaosAmtF = chaosAmtF_;
		dbg.chaosAmtD = chaosAmtD_;
		dbg.tiltDb = tiltDb_;
		dbg.filterHpFreq = smoothedFilterHpFreq_;
		dbg.filterLpFreq = smoothedFilterLpFreq_;
		dbg.maxFftWetDeltaSample = fft1AmountFreezeMaxFftWetDeltaSample;
		dbg.maxFftWetAbsDeltaL = fft1AmountFreezeMaxFftWetAbsDeltaL;
		dbg.maxFftWetAbsDeltaR = fft1AmountFreezeMaxFftWetAbsDeltaR;
		dbg.maxFftWetPrevL = fft1AmountFreezeMaxFftWetPrevL;
		dbg.maxFftWetPrevR = fft1AmountFreezeMaxFftWetPrevR;
		dbg.maxFftWetCurrL = fft1AmountFreezeMaxFftWetCurrL;
		dbg.maxFftWetCurrR = fft1AmountFreezeMaxFftWetCurrR;
		dbg.maxFftWetNorm = fft1AmountFreezeMaxFftWetNorm;
		dbg.maxFftWetOutputReadPos = fft1AmountFreezeMaxFftWetOutputReadPos;
		dbg.maxFftWetSynthCounter = fft1AmountFreezeMaxFftWetSynthCounter;
		dbg.maxEngineWetDeltaSample = fft1AmountFreezeMaxEngineWetDeltaSample;
		dbg.maxEngineWetAbsDeltaL = fft1AmountFreezeMaxEngineWetAbsDeltaL;
		dbg.maxEngineWetAbsDeltaR = fft1AmountFreezeMaxEngineWetAbsDeltaR;
		dbg.maxEngineWetPrevL = fft1AmountFreezeMaxEngineWetPrevL;
		dbg.maxEngineWetPrevR = fft1AmountFreezeMaxEngineWetPrevR;
		dbg.maxEngineWetCurrL = fft1AmountFreezeMaxEngineWetCurrL;
		dbg.maxEngineWetCurrR = fft1AmountFreezeMaxEngineWetCurrR;
		dbg.maxFinalWetDeltaSample = fft1AmountFreezeMaxFinalWetDeltaSample;
		dbg.maxFinalWetAbsDeltaL = fft1AmountFreezeMaxFinalWetAbsDeltaL;
		dbg.maxFinalWetAbsDeltaR = fft1AmountFreezeMaxFinalWetAbsDeltaR;
		dbg.maxFinalWetPrevL = fft1AmountFreezeMaxFinalWetPrevL;
		dbg.maxFinalWetPrevR = fft1AmountFreezeMaxFinalWetPrevR;
		dbg.maxFinalWetCurrL = fft1AmountFreezeMaxFinalWetCurrL;
		dbg.maxFinalWetCurrR = fft1AmountFreezeMaxFinalWetCurrR;
		dbg.maxOutDeltaSample = fft1AmountFreezeMaxOutDeltaSample;
		dbg.maxOutAbsDeltaL = fft1AmountFreezeMaxOutAbsDeltaL;
		dbg.maxOutAbsDeltaR = fft1AmountFreezeMaxOutAbsDeltaR;
		dbg.maxOutPrevL = fft1AmountFreezeMaxOutPrevL;
		dbg.maxOutPrevR = fft1AmountFreezeMaxOutPrevR;
		dbg.maxOutCurrL = fft1AmountFreezeMaxOutCurrL;
		dbg.maxOutCurrR = fft1AmountFreezeMaxOutCurrR;
		dbg.maxPostDuckOutDeltaSample = fft1AmountFreezeMaxPostDuckOutDeltaSample;
		dbg.maxPostDuckOutAbsDeltaL = fft1AmountFreezeMaxPostDuckOutAbsDeltaL;
		dbg.maxPostDuckOutAbsDeltaR = fft1AmountFreezeMaxPostDuckOutAbsDeltaR;
		dbg.maxPostDuckOutPrevL = fft1AmountFreezeMaxPostDuckOutPrevL;
		dbg.maxPostDuckOutPrevR = fft1AmountFreezeMaxPostDuckOutPrevR;
		dbg.maxPostDuckOutCurrL = fft1AmountFreezeMaxPostDuckOutCurrL;
		dbg.maxPostDuckOutCurrR = fft1AmountFreezeMaxPostDuckOutCurrR;
		dbg.maxPreStyleWet = fft1AmountFreezeMaxPreStyleWetDelta;
		dbg.maxPostStyleWet = fft1AmountFreezeMaxPostStyleWetDelta;
		dbg.maxPostFilterWet = fft1AmountFreezeMaxPostFilterWetDelta;
		dbg.maxPostChaosWet = fft1AmountFreezeMaxPostChaosWetDelta;
		dbg.maxPreDcWet = fft1AmountFreezeMaxPreDcWetDelta;
		dbg.maxPostDcWet = fft1AmountFreezeMaxPostDcWetDelta;
		dbg.maxPostDcPrevDcInL = fft1AmountFreezeMaxPostDcPrevDcInL;
		dbg.maxPostDcPrevDcInR = fft1AmountFreezeMaxPostDcPrevDcInR;
		dbg.maxPostDcPrevDcOutL = fft1AmountFreezeMaxPostDcPrevDcOutL;
		dbg.maxPostDcPrevDcOutR = fft1AmountFreezeMaxPostDcPrevDcOutR;
		dbg.maxPostDcInputL = fft1AmountFreezeMaxPostDcInputL;
		dbg.maxPostDcInputR = fft1AmountFreezeMaxPostDcInputR;
		fft1AmountFreezeDumpTrace_.record (dbg);
	}
#endif
}

//==============================================================================
bool STRETRAudioProcessor::hasEditor() const { return true; }

juce::AudioProcessorEditor* STRETRAudioProcessor::createEditor()
{
	return new STRETRAudioProcessorEditor (*this);
}

//==============================================================================
void STRETRAudioProcessor::getStateInformation (juce::MemoryBlock& destData)
{
	auto state = apvts.copyState();
	writeWindowFamilyStateToTree (state);
	std::unique_ptr<juce::XmlElement> xml (state.createXml());
	copyXmlToBinary (*xml, destData);
}

void STRETRAudioProcessor::setStateInformation (const void* data, int sizeInBytes)
{
	std::unique_ptr<juce::XmlElement> xmlState (getXmlFromBinary (data, sizeInBytes));
	if (xmlState.get() != nullptr)
	{
		if (xmlState->hasTagName (apvts.state.getType()))
		{
			apvts.replaceState (juce::ValueTree::fromXml (*xmlState));
			restoreWindowFamilyStateFromTree();
		}
	}
}

void STRETRAudioProcessor::getCurrentProgramStateInformation (juce::MemoryBlock& destData) { getStateInformation (destData); }
void STRETRAudioProcessor::setCurrentProgramStateInformation (const void* data, int sizeInBytes) { setStateInformation (data, sizeInBytes); }

//==============================================================================
juce::AudioProcessorValueTreeState::ParameterLayout STRETRAudioProcessor::createParameterLayout()
{
	std::vector<std::unique_ptr<juce::RangedAudioParameter>> params;

	params.push_back (std::make_unique<juce::AudioParameterFloat> (
		kParamAmount, "Amount",
		juce::NormalisableRange<float> (kAmountMin, kAmountMax, 0.01f, 1.0f), kAmountDefault));
	params.push_back (std::make_unique<juce::AudioParameterFloat> (
		kParamPitch, "Pitch",
		juce::NormalisableRange<float> (kPitchMin, kPitchMax, 0.0f, 1.0f), kPitchDefault));
	params.push_back (std::make_unique<juce::AudioParameterFloat> (
		kParamJitter, "Jitter",
		juce::NormalisableRange<float> (kJitterMin, kJitterMax, 0.01f, 1.0f), kJitterDefault));
	params.push_back (std::make_unique<juce::AudioParameterFloat> (
		kParamGrain, "Grain",
		juce::NormalisableRange<float> (kGrainMin, kGrainMax, 0.001f, 0.25f), kGrainDefault));
	params.push_back (std::make_unique<juce::AudioParameterFloat> (
		kParamEngine, "Engine",
		juce::NormalisableRange<float> ((float) kEngineMin, (float) kEngineMax, 1.0f, 1.0f), kEngineDefault));
	params.push_back (std::make_unique<juce::AudioParameterFloat> (
		kParamWindow, "Window",
		juce::NormalisableRange<float> ((float) kWindowMin, (float) kWindowMax, 1.0f, 0.5f), kWindowDefault));
	params.push_back (std::make_unique<juce::AudioParameterFloat> (
		kParamMaxWindow, "Max Window",
		juce::NormalisableRange<float> ((float) kFftWindowMin, (float) kWindowMax, 1.0f, 1.0f),
		(float) kFftMaxWindowDefault));
	params.push_back (std::make_unique<juce::AudioParameterFloat> (
		kParamStyle, "Style",
		juce::NormalisableRange<float> ((float) kStyleMin, (float) kStyleMax, 1.0f, 1.0f), kStyleDefault));

	params.push_back (std::make_unique<juce::AudioParameterFloat> (
		kParamInput, "Input",
		makeGainFaderRange(), kInputDefault));
	params.push_back (std::make_unique<juce::AudioParameterFloat> (
		kParamOutput, "Output",
		makeGainFaderRange(), kOutputDefault));
	params.push_back (std::make_unique<juce::AudioParameterFloat> (
		kParamMix, "Mix",
		juce::NormalisableRange<float> (kMixMin, kMixMax, 0.0f, 1.0f), kMixDefault));

	params.push_back (std::make_unique<juce::AudioParameterChoice> (
		kParamModeIn, "Mode In", juce::StringArray { "L+R", "MID", "SIDE" }, kModeInOutDefault));
	params.push_back (std::make_unique<juce::AudioParameterChoice> (
		kParamModeOut, "Mode Out", juce::StringArray { "L+R", "MID", "SIDE" }, kModeInOutDefault));
	params.push_back (std::make_unique<juce::AudioParameterChoice> (
		kParamSumBus, "Sum Bus", juce::StringArray { "ST", u8"\u2192M", u8"\u2192S" }, kSumBusDefault));

	// Invert Polarity / Invert Stereo
	params.push_back (std::make_unique<juce::AudioParameterChoice> (
		kParamInvPol, "Invert Polarity",
		juce::StringArray { "NONE", "WET", "GLOBAL" }, kInvPolDefault));
	params.push_back (std::make_unique<juce::AudioParameterChoice> (
		kParamInvStr, "Invert Stereo",
		juce::StringArray { "NONE", "WET", "GLOBAL" }, kInvStrDefault));

	// Mix Mode + Dry/Wet levels (SEND mode) + Filter position
	params.push_back (std::make_unique<juce::AudioParameterChoice> (
		kParamMixMode, "Mix Mode",
		juce::StringArray { "INSERT", "SEND" }, kMixModeDefault));
	params.push_back (std::make_unique<juce::AudioParameterFloat> (
		kParamDryLevel, "Dry Level",
		juce::NormalisableRange<float> (0.0f, 1.0f, 0.01f), kDryLevelDefault));
	params.push_back (std::make_unique<juce::AudioParameterFloat> (
		kParamWetLevel, "Wet Level",
		juce::NormalisableRange<float> (0.0f, 1.0f, 0.01f), kWetLevelDefault));
	// Filter / Tilt position (PRE / POST)
	params.push_back (std::make_unique<juce::AudioParameterChoice> (
		kParamFilterPos, "Filter Position",
		juce::StringArray { juce::String::fromUTF8 (u8"F\u25bc T\u25bc"),
		                    juce::String::fromUTF8 (u8"F\u25b2 T\u25b2"),
		                    juce::String::fromUTF8 (u8"F\u25b2 T\u25bc"),
		                    juce::String::fromUTF8 (u8"F\u25bc T\u25b2") },
		kFilterPosDefault));

	params.push_back (std::make_unique<juce::AudioParameterBool> (kParamAlign, "Align", false));
	params.push_back (std::make_unique<juce::AudioParameterBool> (kParamPdc, "PDC", false));
	params.push_back (std::make_unique<juce::AudioParameterBool> (kParamReverse, "Reverse", false));
	params.push_back (std::make_unique<juce::AudioParameterBool> (kParamTrigger, "Trigger", false));

	// HP/LP wet-signal filter
	params.push_back (std::make_unique<juce::AudioParameterFloat> (
		kParamFilterHpFreq, "Filter HP Freq",
		juce::NormalisableRange<float> (kFilterFreqMin, kFilterFreqMax, 0.01f, 0.35f), kFilterHpFreqDefault));
	params.push_back (std::make_unique<juce::AudioParameterFloat> (
		kParamFilterLpFreq, "Filter LP Freq",
		juce::NormalisableRange<float> (kFilterFreqMin, kFilterFreqMax, 0.01f, 0.35f), kFilterLpFreqDefault));
	params.push_back (std::make_unique<juce::AudioParameterFloat> (
		kParamFilterHpSlope, "Filter HP Slope",
		juce::NormalisableRange<float> ((float) kFilterSlopeMin, (float) kFilterSlopeMax, 1.0f), (float) kFilterSlopeDefault));
	params.push_back (std::make_unique<juce::AudioParameterFloat> (
		kParamFilterLpSlope, "Filter LP Slope",
		juce::NormalisableRange<float> ((float) kFilterSlopeMin, (float) kFilterSlopeMax, 1.0f), (float) kFilterSlopeDefault));
	params.push_back (std::make_unique<juce::AudioParameterBool> (kParamFilterHpOn, "Filter HP On", false));
	params.push_back (std::make_unique<juce::AudioParameterBool> (kParamFilterLpOn, "Filter LP On", false));

	// Tilt / Pan
	params.push_back (std::make_unique<juce::AudioParameterFloat> (
		kParamTilt, "Tilt",
		juce::NormalisableRange<float> (kTiltMin, kTiltMax, 0.01f), kTiltDefault));
	params.push_back (std::make_unique<juce::AudioParameterFloat> (
		kParamPan, "Pan",
		juce::NormalisableRange<float> (kPanMin, kPanMax, 0.01f), kPanDefault));

	// Chaos
	params.push_back (std::make_unique<juce::AudioParameterBool> (kParamChaos, "Chaos Filter", false));
	params.push_back (std::make_unique<juce::AudioParameterBool> (kParamChaosD, "Chaos Delay", false));
	params.push_back (std::make_unique<juce::AudioParameterFloat> (
		kParamChaosAmt, "Chaos Amount",
		juce::NormalisableRange<float> (kChaosAmtMin, kChaosAmtMax, 0.1f), kChaosAmtDefault));
	params.push_back (std::make_unique<juce::AudioParameterFloat> (
		kParamChaosSpd, "Chaos Speed",
		juce::NormalisableRange<float> (kChaosSpdMin, kChaosSpdMax, 0.01f, 0.3f), kChaosSpdDefault));
	params.push_back (std::make_unique<juce::AudioParameterFloat> (
		kParamChaosAmtFilter, "Chaos Filter Amount",
		juce::NormalisableRange<float> (kChaosAmtMin, kChaosAmtMax, 0.1f), kChaosAmtDefault));
	params.push_back (std::make_unique<juce::AudioParameterFloat> (
		kParamChaosSpdFilter, "Chaos Filter Speed",
		juce::NormalisableRange<float> (kChaosSpdMin, kChaosSpdMax, 0.01f, 0.3f), kChaosSpdDefault));

	// Limiter
	params.push_back (std::make_unique<juce::AudioParameterFloat> (
		kParamLimThreshold, "Lim Threshold",
		juce::NormalisableRange<float> (kLimThresholdMin, kLimThresholdMax, 0.1f), kLimThresholdDefault));
	params.push_back (std::make_unique<juce::AudioParameterChoice> (
		kParamLimMode, "Lim Mode", juce::StringArray { "NONE", "WET", "GLOBAL" }, kLimModeDefault));

	// UI state
	params.push_back (std::make_unique<juce::AudioParameterInt> (kParamUiWidth, "UI Width", 360, 1600, 360));
	params.push_back (std::make_unique<juce::AudioParameterInt> (kParamUiHeight, "UI Height", 240, 1200, 480));
	params.push_back (std::make_unique<juce::AudioParameterBool> (kParamUiPalette, "UI Palette", false));
	params.push_back (std::make_unique<juce::AudioParameterBool> (kParamUiCrt, "UI CRT", false));
	params.push_back (std::make_unique<juce::AudioParameterInt> (kParamUiColor0, "UI Color 0", 0, 0xFFFFFF, 0x00FF00));
	params.push_back (std::make_unique<juce::AudioParameterInt> (kParamUiColor1, "UI Color 1", 0, 0xFFFFFF, 0x000000));

	return { params.begin(), params.end() };
}

//==============================================================================
// UI state management

void STRETRAudioProcessor::setUiEditorSize (int width, int height)
{
	const int w = juce::jlimit (360, 1600, width);
	const int h = juce::jlimit (240, 1200, height);
	uiEditorWidth.store (w, std::memory_order_relaxed);
	uiEditorHeight.store (h, std::memory_order_relaxed);
	apvts.state.setProperty (UiStateKeys::editorWidth, w, nullptr);
	apvts.state.setProperty (UiStateKeys::editorHeight, h, nullptr);
	setParameterPlainValue (apvts, kParamUiWidth, (float) w);
	setParameterPlainValue (apvts, kParamUiHeight, (float) h);
	updateHostDisplay();
}

int STRETRAudioProcessor::getUiEditorWidth() const noexcept
{
	const auto fromState = apvts.state.getProperty (UiStateKeys::editorWidth);
	if (! fromState.isVoid()) return (int) fromState;
	if (uiWidthParam != nullptr) return (int) std::lround (uiWidthParam->load (std::memory_order_relaxed));
	return uiEditorWidth.load (std::memory_order_relaxed);
}

int STRETRAudioProcessor::getUiEditorHeight() const noexcept
{
	const auto fromState = apvts.state.getProperty (UiStateKeys::editorHeight);
	if (! fromState.isVoid()) return (int) fromState;
	if (uiHeightParam != nullptr) return (int) std::lround (uiHeightParam->load (std::memory_order_relaxed));
	return uiEditorHeight.load (std::memory_order_relaxed);
}

int STRETRAudioProcessor::getStoredWindowForEngine (int engineVal) const noexcept
{
	return getStoredWindowForFamily (getWindowFamilyForEngineInternal (engineVal));
}

void STRETRAudioProcessor::setStoredWindowForEngine (int engineVal, int windowValue) noexcept
{
	const auto family = getWindowFamilyForEngineInternal (engineVal);
	const int safeWindow = getCanonicalWindowForFamily (family, windowValue);
	setStoredWindowForFamily (family, safeWindow);
	lastObservedWindowParam_.store (safeWindow, std::memory_order_relaxed);
}

void STRETRAudioProcessor::syncWindowParameterToEngine (int engineVal)
{
	const int windowValue = getStoredWindowForEngine (engineVal);
	lastObservedWindowParam_.store (windowValue, std::memory_order_relaxed);
	if (loadIntParamOrDefault (windowParam, (int) kWindowDefault) != windowValue)
		setParameterPlainValue (apvts, kParamWindow, (float) windowValue);
}

void STRETRAudioProcessor::setUiUseCustomPalette (bool shouldUseCustomPalette)
{
	uiUseCustomPalette.store (shouldUseCustomPalette ? 1 : 0, std::memory_order_relaxed);
	apvts.state.setProperty (UiStateKeys::useCustomPalette, shouldUseCustomPalette, nullptr);
	setParameterPlainValue (apvts, kParamUiPalette, shouldUseCustomPalette ? 1.0f : 0.0f);
	updateHostDisplay();
}

bool STRETRAudioProcessor::getUiUseCustomPalette() const noexcept
{
	const auto fromState = apvts.state.getProperty (UiStateKeys::useCustomPalette);
	if (! fromState.isVoid()) return (bool) fromState;
	if (uiPaletteParam != nullptr) return uiPaletteParam->load (std::memory_order_relaxed) > 0.5f;
	return uiUseCustomPalette.load (std::memory_order_relaxed) != 0;
}

void STRETRAudioProcessor::setUiCrtEnabled (bool enabled)
{
	uiCrtEnabled.store (enabled ? 1 : 0, std::memory_order_relaxed);
	apvts.state.setProperty (UiStateKeys::crtEnabled, enabled, nullptr);
	setParameterPlainValue (apvts, kParamUiCrt, enabled ? 1.0f : 0.0f);
	updateHostDisplay();
}

bool STRETRAudioProcessor::getUiCrtEnabled() const noexcept
{
	const auto fromState = apvts.state.getProperty (UiStateKeys::crtEnabled);
	if (! fromState.isVoid()) return (bool) fromState;
	if (uiCrtParam != nullptr) return uiCrtParam->load (std::memory_order_relaxed) > 0.5f;
	return uiCrtEnabled.load (std::memory_order_relaxed) != 0;
}

void STRETRAudioProcessor::setUiIoExpanded (bool expanded)
{
	apvts.state.setProperty (UiStateKeys::ioExpanded, expanded, nullptr);
}

bool STRETRAudioProcessor::getUiIoExpanded() const noexcept
{
	const auto fromState = apvts.state.getProperty (UiStateKeys::ioExpanded);
	if (! fromState.isVoid()) return (bool) fromState;
	return false;
}

void STRETRAudioProcessor::setUiCustomPaletteColour (int index, juce::Colour colour)
{
	if (index >= 0 && index < 2)
	{
		uiCustomPalette[(size_t) index].store (colour.getARGB(), std::memory_order_relaxed);
		const juce::String key = UiStateKeys::customPalette[(size_t) index];
		apvts.state.setProperty (key, (int) colour.getARGB(), nullptr);
		if (uiColorParams[(size_t) index] != nullptr)
			setParameterPlainValue (apvts, (index == 0 ? kParamUiColor0 : kParamUiColor1),
			                        (float) (int) colour.getARGB());
		updateHostDisplay();
	}
}

juce::Colour STRETRAudioProcessor::getUiCustomPaletteColour (int index) const noexcept
{
	if (index < 0 || index >= 2) return juce::Colours::white;
	const juce::String key = UiStateKeys::customPalette[(size_t) index];
	const auto fromState = apvts.state.getProperty (key);
	if (! fromState.isVoid())
		return juce::Colour ((juce::uint32) (int) fromState);
	if (uiColorParams[(size_t) index] != nullptr)
	{
		const int rgb = juce::jlimit (0, 0xFFFFFF,
		                              (int) std::lround (uiColorParams[(size_t) index]->load (std::memory_order_relaxed)));
		return juce::Colour::fromRGB ((juce::uint8) ((rgb >> 16) & 0xFF),
		                              (juce::uint8) ((rgb >> 8) & 0xFF),
		                              (juce::uint8) (rgb & 0xFF));
	}
	return juce::Colour (uiCustomPalette[(size_t) index].load (std::memory_order_relaxed));
}

//==============================================================================
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
	return new STRETRAudioProcessor();
}
