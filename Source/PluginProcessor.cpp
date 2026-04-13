#include "PluginProcessor.h"
#include "PluginEditor.h"

namespace
{
	constexpr float kGainSmoothCoeff = 0.9955f;
	constexpr float kGainSmoothStep  = 1.0f - kGainSmoothCoeff;

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
	modParam     = apvts.getRawParameterValue (kParamMod);
	grainParam   = apvts.getRawParameterValue (kParamGrain);
	engineParam  = apvts.getRawParameterValue (kParamEngine);
	windowParam  = apvts.getRawParameterValue (kParamWindow);
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

	const int w = loadIntParamOrDefault (uiWidthParam, 360);
	const int h = loadIntParamOrDefault (uiHeightParam, 480);
	uiEditorWidth.store (w, std::memory_order_relaxed);
	uiEditorHeight.store (h, std::memory_order_relaxed);

	perfTrace.enableDesktopAutoDump();
	stretchDebugTrace_.enableDesktopAutoDump();
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
	smoothedWindow_    = loadAtomicOrDefault (windowParam, kWindowDefault);
	smoothedSpeed_     = juce::jmax (0.0f, 1.0f - loadAtomicOrDefault (amountParam, kAmountDefault) / 100.0f);
	smoothedPitchRate_ = std::exp2 ((loadAtomicOrDefault (modParam, kModDefault) - 0.5f) * 4.0f);

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
		const int desired = juce::jmin (kInputBufMaxLen, (int) (sampleRate * 5.5));
		// Round up to next power of 2 (kInputBufMaxLen is already 2^18)
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

	// Initialize WSOLA state
	resetWsolaAtPos (0.0);
	wsolaUnityBypassActive_ = false;
	stretchBootstrapSegments_ = 0;
	stretchTransitionRemaining_ = 0;
	stretchTransitionTotal_ = 0;
	stretchTransitionToUnity_ = false;
	triggerWasOn_ = false;

    // Initialize Granular state
	for (int g = 0; g < kMaxGrains; ++g)
		grains_[g] = {};
	grainNextSlot_ = 0;
	grainSpawnCountdown_ = 0;
	grainReadPos_ = 0.0f;
	grainPrevOutL_ = 0.0f;
	grainPrevOutR_ = 0.0f;

    // Initialize STFT state
	std::memset (&stft_, 0, sizeof (stft_));
	currentFftOrder_ = -1;
	fft_.reset();
	std::memset (fftWork_, 0, sizeof (fftWork_));
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
	for (int c = 0; c < 2; ++c)
	{
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
		const bool chaosFilterActive = chaosFilterEnabled_ && chaosAmtF_ > 0.01f;
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

	const bool chaosFilterActive = chaosFilterEnabled_ && chaosAmtF_ > 0.01f;
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

		std::memset (&stft_, 0, sizeof (stft_));
		stft_.activeFftSize = fftSize;
		const int behind = (inputBufWritePos_ - fftSize + inputBufLen_) & inputBufMask_;
		stft_.analysisReadPos = (double) behind;
	}
}

void STRETRAudioProcessor::performStftCycle (int fftSize, int analysisHop, int synthesisHop,
                                              float pitchRate, bool reverseOn, float pitchRateR,
                                              bool wideMode)
{
	if (fft_ == nullptr || inputBufLen_ <= 0 || fftSize <= 0) return;

	const int numBins    = fftSize / 2 + 1;
	const int outBufLen  = kStftOutBufLen;
	const float twoPi    = juce::MathConstants<float>::twoPi;
	const float pi       = juce::MathConstants<float>::pi;
	const float expBase  = twoPi / (float) fftSize;
	const float olaScale = 2.0f / 3.0f;

	for (int ch = 0; ch < 2; ++ch)
	{
		// DUAL: R channel uses pitchRateR if provided
		const float pr = (ch == 1 && pitchRateR > 0.0f) ? pitchRateR : pitchRate;

        // Analysis
		if (analysisHop > 0 || ! stft_.hasFrame)
		{
			for (int j = 0; j < fftSize; ++j)
			{
				const int idx = ((int) stft_.analysisReadPos + j) & inputBufMask_;
				fftWork_[j] = inputBuf_[ch][(size_t) idx] * fftWindow_[j];
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

				float phaseDiff = ph - stft_.prevPhase[ch][k];
				stft_.prevPhase[ch][k] = ph;

				if (analysisHop > 0)
				{
					phaseDiff -= expBase * (float) k * (float) analysisHop;
					while (phaseDiff >  pi) phaseDiff -= twoPi;
					while (phaseDiff < -pi) phaseDiff += twoPi;
					stft_.lastFreq[ch][k] = expBase * (float) k
					                       + phaseDiff / (float) analysisHop;
				}
				else
				{
					stft_.lastFreq[ch][k] = expBase * (float) k;
				}

				stft_.lastMag[ch][k] = mag;
			}
			stft_.hasFrame = true;
		}

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
			for (int k = 0; k < numBins; ++k)
			{
				float mag, freq;

				if (std::abs (pitchRate - 1.0f) > 0.001f)
				{
					const float srcF = (float) k / pitchRate;
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

			// Phase locking (only when not in passthrough)
			{
				bool isPeak[kMaxFftBins];
				isPeak[0] = (numBins > 1) ? (synthMag[0] >= synthMag[1]) : true;
				for (int k = 1; k < numBins - 1; ++k)
					isPeak[k] = (synthMag[k] >= synthMag[k - 1] && synthMag[k] >= synthMag[k + 1]);
				if (numBins > 1)
					isPeak[numBins - 1] = (synthMag[numBins - 1] >= synthMag[numBins - 2]);

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
						stft_.synthPhase[ch][k] = stft_.synthPhase[ch][pk]
						    + (stft_.prevPhase[ch][k] - stft_.prevPhase[ch][pk]);
					}
				}
			}
		}

		// Step 3: write complex output (only numBins used by performRealOnlyInverseTransform)
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

		for (int j = 0; j < fftSize; ++j)
		{
			const int outIdx = (stft_.outputReadPos + j) & (outBufLen - 1);
			stft_.outputAccum[ch][outIdx] += fftWork_[j] * fftWindow_[j] * olaScale;
		}
	}

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
}

// FFT Engine 3: Spectral Hold / Freeze
void STRETRAudioProcessor::performStftCycleSpectralHold (int fftSize, int synthesisHop,
                                                          float holdCoeff, float pitchRate, float pitchRateR,
                                                          bool wideMode)
{
	if (fft_ == nullptr || inputBufLen_ <= 0 || fftSize <= 0) return;

	const int   numBins    = fftSize / 2 + 1;
	const int   outBufLen  = kStftOutBufLen;
	const float twoPi      = juce::MathConstants<float>::twoPi;
	const float pi         = juce::MathConstants<float>::pi;
	const float expBase    = twoPi / (float) fftSize;
	const float olaScale   = 2.0f / 3.0f;
	const float blend      = 1.0f - holdCoeff;  // 1 = transparent, 0 = full freeze

	// Always read the most recent complete frame from the input buffer
	const int readStart = (inputBufWritePos_ - fftSize + inputBufLen_) & inputBufMask_;

	for (int ch = 0; ch < 2; ++ch)
	{
		// DUAL: R channel uses pitchRateR if provided
		const float pr = (ch == 1 && pitchRateR > 0.0f) ? pitchRateR : pitchRate;

        // Analysis
		for (int j = 0; j < fftSize; ++j)
		{
			const int idx = (readStart + j) & inputBufMask_;
			fftWork_[j] = inputBuf_[ch][(size_t) idx] * fftWindow_[j];
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

			phaseDiff -= expBase * (float) k * (float) synthesisHop;
			while (phaseDiff >  pi) phaseDiff -= twoPi;
			while (phaseDiff < -pi) phaseDiff += twoPi;
			const float freq = expBase * (float) k + phaseDiff / (float) synthesisHop;

			// Spectral hold: blend new analysis into retained state
			stft_.heldMag[ch][k]  = holdCoeff * stft_.heldMag[ch][k]  + blend * mag;
			stft_.heldFreq[ch][k] = holdCoeff * stft_.heldFreq[ch][k] + blend * freq;

            // Keep lastMag/lastFreq current for clean FFT2 -> FFT1 transition
			stft_.lastMag[ch][k]  = mag;
			stft_.lastFreq[ch][k] = freq;
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
			for (int k = 0; k < numBins; ++k)
			{
				float mag, freq;

				if (std::abs (pitchRate - 1.0f) > 0.001f)
				{
					const float srcF = (float) k / pitchRate;
					const int   s0   = (int) srcF;
					const float fr   = srcF - (float) s0;

					mag  = 0.0f;
					freq = expBase * (float) k;

					if (s0 >= 0 && s0 < numBins)
					{
						mag  += stft_.heldMag[ch][s0] * (1.0f - fr);
						freq  = stft_.heldFreq[ch][s0] * pr;
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
				stft_.synthPhase[ch][k] += freq * (float) synthesisHop;

				// Blend synthPhase toward analysis phase to prevent
				// permanent phase offset after high-holdCoeff periods
				{
					float phDelta = std::remainder (stft_.prevPhase[ch][k] - stft_.synthPhase[ch][k], twoPi);
					stft_.synthPhase[ch][k] += blend * phDelta;
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

		for (int j = 0; j < fftSize; ++j)
		{
			const int outIdx = (stft_.outputReadPos + j) & (outBufLen - 1);
			stft_.outputAccum[ch][outIdx] += fftWork_[j] * fftWindow_[j] * olaScale;
		}
	}

	// Keep analysisReadPos current so switching back to engine 2 starts from the right place
	stft_.analysisReadPos = (double) readStart;
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
	const int debugBlockIndex = stretchDebugBlockCounter_++;

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

	const float targetInputGain  = fastDecibelsToGain (inputGainDb);
	const float targetOutputGain = fastDecibelsToGain (outputGainDb);

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
	const float modVal     = loadAtomicOrDefault (modParam, kModDefault);         // 0..1
	const int   windowVal  = loadIntParamOrDefault (windowParam, (int) kWindowDefault);
	const int   styleVal   = loadIntParamOrDefault (styleParam, 1);
	const float grainMs    = loadAtomicOrDefault (grainParam, kGrainDefault);     // 1..500 ms
	const bool  reverseOn  = loadBoolParamOrDefault (reverseParam, false);
	const bool  triggerOn  = loadBoolParamOrDefault (triggerParam, false);

    // Amount -> speed: 0%=1.0 (no stretch), 100%=0.0 (freeze)
    // Unified mapping across all four engines - true freeze or full hold at 100%.
	const float targetSpeed = juce::jmax (0.0f, 1.0f - amountVal / 100.0f);

    // Mod -> pitch rate: center (0.5)=1.0x, 0=0.0625x, 1=16x
	const float targetPitchRate = std::exp2 ((modVal - 0.5f) * 4.0f);

    // Window -> continuous size (16..8192), smoothed
	const float targetWindow = (float) juce::jlimit (kWindowMin, kWindowMax, windowVal);
	const int windowSamples = (int) smoothedWindow_;

	// Grain size in samples
	const int grainSamples = juce::jmax (4, (int) (grainMs * 0.001f * (float) currentSampleRate));

	float dryLevelState = smoothedDryLevel;
	float wetLevelState = smoothedWetLevel;
	const float limThreshLinStart = smoothedLimThreshold;
	float limThreshLinState = smoothedLimThreshold;

	// Granular read rate: how fast the grain spawn position advances
	// Granular read rate computed per-sample from smoothedSpeed_ below

    // Trigger edge detection: reset engines on trigger press
	if (triggerOn && ! triggerWasOn_)
	{
        // Trigger just pressed - capture current write position as starting read point
		const double capturePos = (double) ((inputBufWritePos_ - 1 + inputBufLen_) & inputBufMask_);
		resetWsolaAtPos (capturePos);
		wsolaUnityBypassActive_ = false;

		for (int g = 0; g < kMaxGrains; ++g)
			grains_[g].active = false;
		grainReadPos_ = (float) capturePos;
		grainSpawnCountdown_ = 0;
		grainPrevOutL_ = 0.0f;
		grainPrevOutR_ = 0.0f;
	}
	triggerWasOn_ = triggerOn;

    // FFT engine setup
	if ((engineVal == 2 || engineVal == 3) && inputBufLen_ > 0)
	{
        // FFT requires power-of-2 sizes - snap continuous window value
		const int fftSize = juce::jlimit (64, kMaxFftSize, nextPowerOf2 (windowSamples));
		ensureFft (fftSize);
	}

	// Engine crossfade: trigger fade-in on engine change
	if (prevEngineVal_ >= 0 && engineVal != prevEngineVal_)
	{
		engineFadePos_ = kEngineFadeLen;
		if (engineVal == 0 && inputBufLen_ > 0)
		{
			const double capturePos = (double) ((inputBufWritePos_ - 1 + inputBufLen_) & inputBufMask_);
			resetWsolaAtPos (capturePos);
			wsolaUnityBypassActive_ = false;
		}
	}
	prevEngineVal_ = engineVal;

    // PDC and Align
	{
		const bool alignOn = loadBoolParamOrDefault (alignParam, false);
		const bool pdcOn   = loadBoolParamOrDefault (pdcParam, false);
		const int  fftLat  = ((engineVal == 2 || engineVal == 3) && stft_.activeFftSize > 0)
		                     ? stft_.activeFftSize : 0;
		setLatencySamples (pdcOn ? fftLat : 0);
		dryDelayLen_ = (alignOn && fftLat > 0) ? fftLat : 0;
	}

	if (chaosFilterEnabled_)
	{
		chaosAmtF_       = loadAtomicOrDefault (chaosAmtFilterParam, kChaosAmtDefault);
		const float spd  = loadAtomicOrDefault (chaosSpdFilterParam, kChaosSpdDefault);
		chaosShPeriodF_  = (float) currentSampleRate / juce::jmax (0.01f, spd);
		chaosFilterMaxOct_ = chaosAmtF_ * 0.02f;
	}
	if (chaosDelayEnabled_)
	{
		chaosAmtD_       = loadAtomicOrDefault (chaosAmtParam, kChaosAmtDefault);
		chaosAmtNormD_   = chaosAmtD_ * 0.01f;
		const float spd  = loadAtomicOrDefault (chaosSpdParam, kChaosSpdDefault);
		chaosShPeriodD_  = (float) currentSampleRate / juce::jmax (0.01f, spd);
		chaosDelayMaxSamples_ = chaosAmtNormD_ * 0.005f * static_cast<float> (currentSampleRate);
		chaosGainMaxDb_       = chaosAmtNormD_;
	}
	if (chaosFilterEnabled_ || chaosDelayEnabled_)
		chaosParamSmoothCoeff_ = cachedChaosParamSmoothCoeff_;

	chaosStereo_ = (styleVal >= 1);

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
		smoothedWindow_    += (targetWindow     - smoothedWindow_)    * kGainSmoothStep;
		smoothedSpeed_     += (targetSpeed      - smoothedSpeed_)     * kGainSmoothStep;
		smoothedPitchRate_ += (targetPitchRate  - smoothedPitchRate_) * kGainSmoothStep;

		const float speed     = smoothedSpeed_;
		const float pitchRate = smoothedPitchRate_;

		float inL = channelL[i] * smoothedInputGain;
		float inR = (channelR != nullptr) ? channelR[i] * smoothedInputGain : inL;

		// Save dry signal (with Align delay for FFT engine PDC)
		float dryOrigL, dryOrigR;
		if (dryDelayLen_ > 0)
		{
			dryDelayBuf_[0][dryDelayWritePos_] = channelL[i];
			dryDelayBuf_[1][dryDelayWritePos_] = (channelR != nullptr) ? channelR[i] : channelL[i];
			const int rdp = (dryDelayWritePos_ - dryDelayLen_ + kMaxFftSize) & (kMaxFftSize - 1);
			dryOrigL = dryDelayBuf_[0][rdp];
			dryOrigR = dryDelayBuf_[1][rdp];
			dryDelayWritePos_ = (dryDelayWritePos_ + 1) & (kMaxFftSize - 1);
		}
		else
		{
			dryOrigL = channelL[i];
			dryOrigR = (channelR != nullptr) ? channelR[i] : dryOrigL;
		}

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
		}

		float wetL = 0.0f;
		float wetR = 0.0f;

        // Engine dispatch
		const bool isDual = (styleVal == 3 && numChannels >= 2);
		const bool isWide = (styleVal == 2 && numChannels >= 2);
		const float pitchRateR = isDual ? (pitchRate * 0.5f) : -1.0f;
		if (! triggerOn || engineVal != 0)
		{
			wsolaUnityBypassActive_ = false;
			stretchBootstrapSegments_ = 0;
			stretchTransitionRemaining_ = 0;
			stretchTransitionTotal_ = 0;
			stretchTransitionToUnity_ = false;
		}

		if (! triggerOn)
		{
            // Trigger OFF -> passthrough (all engines)
			wetL = inL;
			wetR = inR;
		}
		else if ((engineVal == 2 || engineVal == 3) && inputBufLen_ > 0 && stft_.activeFftSize > 0)
		{
            // Engines 2 and 3: FFT-based (phase vocoder / spectral hold)
			const int outBufLen = kStftOutBufLen;

			wetL = stft_.outputAccum[0][stft_.outputReadPos];
			wetR = stft_.outputAccum[1][stft_.outputReadPos];
			stft_.outputAccum[0][stft_.outputReadPos] = 0.0f;
			stft_.outputAccum[1][stft_.outputReadPos] = 0.0f;
			stft_.outputReadPos = (stft_.outputReadPos + 1) & (outBufLen - 1);

			if (++stft_.synthCounter >= stft_.activeFftSize / 4)
			{
				stft_.synthCounter = 0;
				const int fftSynthHop = stft_.activeFftSize / 4;

				if (engineVal == 3)
				{
					// Spectral Hold: always analyze at normal rate, blend magnitudes
					// Power curve (t^0.25) so low amount values already produce audible hold
					const float t = 1.0f - speed;  // 0..1 = amount normalised
					const float holdCoeff = std::sqrt (std::sqrt (t));
					performStftCycleSpectralHold (stft_.activeFftSize, fftSynthHop,
					                              holdCoeff, pitchRate, pitchRateR, isWide);
				}
				else
				{
					// Phase Vocoder: reduce analysis hop for time stretch
					const int fftAnalysisHop = (int) ((float) fftSynthHop * speed);
					performStftCycle (stft_.activeFftSize, fftAnalysisHop,
					                  fftSynthHop, pitchRate, reverseOn, pitchRateR, isWide);
				}
			}
		}
		else if (engineVal == 0 && inputBufLen_ > 0)
		{
            // Engine 0: WSOLA-style time-domain stretch with true overlap-add.
			const bool stretchUnity = ! reverseOn
				&& ! isDual
				&& ! isWide
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

					StretchDebugEntry dbg {};
					dbg.blockIndex = debugBlockIndex;
					dbg.sampleIndex = i;
					dbg.eventType = 1;
					dbg.amount = amountVal;
					dbg.mod = modVal;
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
					wetL = stretchOutL * wetMix + inL * dryMix;
					wetR = stretchOutR * wetMix + inR * dryMix;
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
					wetL = inL;
					wetR = inR;
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
				const double targetAnalysisHop = (double) synthesisHop * (double) speed * (double) pitchRate;
				const bool nearUnity = std::abs (speed - 1.0f) <= 0.05f
					&& std::abs (pitchRate - 1.0f) <= 0.05f;
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
						const double analysisHopR = (double) synthesisHop * (double) speed * (double) pitchRate * 0.5;
						if (wsola_.hasPrevTail)
							wsola_.segInputStartR += analysisHopR * direction;

						if (! reverseOn)
						{
							const double maxStartR = capturePos - computeLookBehind (pitchRate * 0.5f, false);
							if (wsola_.segInputStartR > maxStartR)
								wsola_.segInputStartR = maxStartR;
						}

						const int prevBestOffsetR = wsola_.lastBestOffsetR;
						const auto matchR = wsola_.hasPrevTail
							? wsolaBestOverlapOffset (1, wsola_.segInputStartR, overlapLen,
							                          prevBestOffsetR, nearUnity,
							                          pitchRate * 0.5f * (float) direction, synthPos)
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
					const float readRateR = (isDual ? (pitchRate * 0.5f) : pitchRate) * (float) direction;
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

					StretchDebugEntry dbg {};
					dbg.blockIndex = debugBlockIndex;
					dbg.sampleIndex = i;
					dbg.eventType = 0;
					dbg.amount = amountVal;
					dbg.mod = modVal;
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
					wetL = inL * dryMix + wetL * wetMix;
					wetR = inR * dryMix + wetR * wetMix;
					--stretchTransitionRemaining_;
				}
			}
		}
		else if (engineVal == 1 && inputBufLen_ > 0)
		{
            // Engine 1: GRANULAR
            // Spawn new grains - readPos advances per spawn, not per sample
			if (--grainSpawnCountdown_ <= 0)
			{
				// Overlap density from WINDOW (clamped for COLA safety)
				const int density = juce::jlimit (2, kMaxGrains / 2, windowSamples / 64);
				const int spawnInterval = juce::jmax (1, grainSamples / density);
				grainSpawnCountdown_ = spawnInterval;

				// Advance read position by analysis hop (per-spawn, not per-sample)
                // analysisHop = spawnInterval * speed -> stretch ratio = 1/speed
				const float direction = reverseOn ? -1.0f : 1.0f;
				grainReadPos_ += (float) spawnInterval * speed * direction;
				if (grainReadPos_ >= (float) inputBufLen_)
					grainReadPos_ -= (float) inputBufLen_;
				else if (grainReadPos_ < 0.0f)
					grainReadPos_ += (float) inputBufLen_;

				if (isDual)
				{
                    // DUAL: spawn L-only grain at pitchRate + R-only grain at pitchRate x 0.5
					for (int dch = 0; dch < 2; ++dch)
					{
						for (int attempt = 0; attempt < kMaxGrains; ++attempt)
						{
							const int slot = (grainNextSlot_ + attempt) % kMaxGrains;
							if (! grains_[slot].active)
							{
								auto& g = grains_[slot];
								g.active  = true;
								g.length  = grainSamples;
								g.elapsed = 0;
								g.rate    = (dch == 0) ? (double) pitchRate : (double) (pitchRate * 0.5f);
								g.reverse = reverseOn;
								g.readPos = (double) grainReadPos_;
								g.playPos = g.reverse ? (double) (grainSamples - 1) : 0.0;
								g.dualCh  = dch;  // 0=L-only, 1=R-only
								grainNextSlot_ = (slot + 1) % kMaxGrains;
								break;
							}
						}
					}
				}
				else if (isWide)
				{
					// WIDE: spawn L-only + R-only grains, R offset by half grain for decorrelation
					for (int dch = 0; dch < 2; ++dch)
					{
						for (int attempt = 0; attempt < kMaxGrains; ++attempt)
						{
							const int slot = (grainNextSlot_ + attempt) % kMaxGrains;
							if (! grains_[slot].active)
							{
								auto& g = grains_[slot];
								g.active  = true;
								g.length  = grainSamples;
								g.elapsed = 0;
								g.rate    = (double) pitchRate;
								g.reverse = reverseOn;
								double rp = (double) grainReadPos_;
								if (dch == 1)
								{
									rp += (double) (grainSamples / 2);
									if (rp >= (double) inputBufLen_) rp -= (double) inputBufLen_;
								}
								g.readPos = rp;
								g.playPos = g.reverse ? (double) (grainSamples - 1) : 0.0;
								g.dualCh  = dch;  // 0=L-only, 1=R-only
								grainNextSlot_ = (slot + 1) % kMaxGrains;
								break;
							}
						}
					}
				}
				else
				{
					// Normal: spawn one grain for both channels
					for (int attempt = 0; attempt < kMaxGrains; ++attempt)
					{
						const int slot = (grainNextSlot_ + attempt) % kMaxGrains;
						if (! grains_[slot].active)
						{
							auto& g = grains_[slot];
							g.active  = true;
							g.length  = grainSamples;
							g.elapsed = 0;
							g.rate    = (double) pitchRate;
							g.reverse = reverseOn;
							g.readPos = (double) grainReadPos_;
							g.playPos = g.reverse ? (double) (grainSamples - 1) : 0.0;
							g.dualCh  = -1;  // both channels
							grainNextSlot_ = (slot + 1) % kMaxGrains;
							break;
						}
					}
				}
			}

			// Mix active grains
			float sumL = 0.0f, sumR = 0.0f;
			float sumEnvL = 0.0f, sumEnvR = 0.0f;
			for (int g = 0; g < kMaxGrains; ++g)
			{
				auto& gr = grains_[g];
				if (! gr.active) continue;

				// Hann window envelope
				const float phase = (float) gr.elapsed / (float) gr.length;
				const float env = hannWindow (phase);

				// Read position in input buffer
				const double bufPos = gr.readPos + gr.playPos;

				// Accumulate per-channel based on dualCh
				if (gr.dualCh <= 0)  // both (-1) or L-only (0)
				{
					sumL += readInputBuf (0, bufPos) * env;
					sumEnvL += env;
				}
				if (gr.dualCh == -1 || gr.dualCh == 1)  // both (-1) or R-only (1)
				{
					sumR += readInputBuf (1, bufPos) * env;
					sumEnvR += env;
				}

				// Advance grain playback position
				if (gr.reverse)
					gr.playPos -= gr.rate;
				else
					gr.playPos += gr.rate;

				gr.elapsed++;
				if (gr.elapsed >= gr.length)
					gr.active = false;
			}

            // Normalize: speed=1 (no stretch) -> grains correlated -> 1/sumEnv;
            // speed=0 (max stretch) -> grains uncorrelated -> 1/sqrt(sumEnv).
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
		}
		else
		{
			wetL = inL;
			wetR = inR;
		}

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

		// Chaos engines
		if (chaosFilterEnabled_) advanceChaosF();
		if (chaosDelayEnabled_)  advanceChaosD();

		// Wet-signal filter + tilt
		if (!tiltPre_)   tiltWetSample   (wetL, wetR);
		if (!filterPre_) filterWetSample (wetL, wetR);

		// Chaos delay (per-channel Hermite+Drift micro-delay + gain modulation)
		if (chaosDelayEnabled_ && chaosAmtD_ > 0.01f)
			applyChaosDelay (wetL, wetR);

		// Mode Out: M/S encode wet output
		if (numChannels >= 2 && modeOutVal != 0)
		{
			const float l = wetL, r = wetR;
			if (modeOutVal == 1)      { const float mid  = (l + r) * kSqrt2Over2; wetL = wetR = mid; }
			else /* modeOutVal==2 */   { const float side = (l - r) * kSqrt2Over2; wetL = wetR = side; }
		}

		// Engine crossfade: smooth fade-in after engine switch
		if (engineFadePos_ > 0)
		{
			const float fadeGain = 1.0f - (float) engineFadePos_ / (float) kEngineFadeLen;
			wetL *= fadeGain;
			wetR *= fadeGain;
			--engineFadePos_;
		}

		// DC blocker (1-pole HP ~5 Hz)
		{
			const float outL = wetL - dcBlockPrevIn_[0] + dcBlockR_ * dcBlockPrevOut_[0];
			const float outR = wetR - dcBlockPrevIn_[1] + dcBlockR_ * dcBlockPrevOut_[1];
			dcBlockPrevIn_[0] = wetL;  dcBlockPrevIn_[1] = wetR;
			dcBlockPrevOut_[0] = outL; dcBlockPrevOut_[1] = outR;
			wetL = outL;
			wetR = outR;
		}

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
	std::unique_ptr<juce::XmlElement> xml (state.createXml());
	copyXmlToBinary (*xml, destData);
}

void STRETRAudioProcessor::setStateInformation (const void* data, int sizeInBytes)
{
	std::unique_ptr<juce::XmlElement> xmlState (getXmlFromBinary (data, sizeInBytes));
	if (xmlState.get() != nullptr)
	{
		if (xmlState->hasTagName (apvts.state.getType()))
			apvts.replaceState (juce::ValueTree::fromXml (*xmlState));
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
		kParamMod, "Mod",
		juce::NormalisableRange<float> (kModMin, kModMax, 0.0f, 1.0f), kModDefault));
	params.push_back (std::make_unique<juce::AudioParameterFloat> (
		kParamGrain, "Grain",
		juce::NormalisableRange<float> (kGrainMin, kGrainMax, 0.01f, 0.25f), kGrainDefault));
	params.push_back (std::make_unique<juce::AudioParameterFloat> (
		kParamEngine, "Engine",
		juce::NormalisableRange<float> ((float) kEngineMin, (float) kEngineMax, 1.0f, 1.0f), kEngineDefault));
	params.push_back (std::make_unique<juce::AudioParameterFloat> (
		kParamWindow, "Window",
		juce::NormalisableRange<float> ((float) kWindowMin, (float) kWindowMax, 1.0f, 0.5f), kWindowDefault));
	params.push_back (std::make_unique<juce::AudioParameterFloat> (
		kParamStyle, "Style",
		juce::NormalisableRange<float> ((float) kStyleMin, (float) kStyleMax, 1.0f, 1.0f), kStyleDefault));

	params.push_back (std::make_unique<juce::AudioParameterFloat> (
		kParamInput, "Input",
		juce::NormalisableRange<float> (kInputMin, kInputMax, 0.0f, 2.5f), kInputDefault));
	params.push_back (std::make_unique<juce::AudioParameterFloat> (
		kParamOutput, "Output",
		juce::NormalisableRange<float> (kOutputMin, kOutputMax, 0.0f, 3.23f), kOutputDefault));
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

	params.push_back (std::make_unique<juce::AudioParameterBool> (kParamAlign, "Align", true));
	params.push_back (std::make_unique<juce::AudioParameterBool> (kParamPdc, "PDC", true));
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
