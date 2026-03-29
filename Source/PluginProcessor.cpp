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

	// ── Wet-signal biquad filter helpers ──
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
	smoothedWindow_    = loadAtomicOrDefault (windowParam, kWindowDefault);
	smoothedSpeed_     = juce::jmax (0.0f, 1.0f - loadAtomicOrDefault (amountParam, kAmountDefault) / 100.0f);
	smoothedPitchRate_ = std::exp2 ((loadAtomicOrDefault (modParam, kModDefault) - 0.5f) * 4.0f);

	// ── Initialize Hann LUT ──
	for (int i = 0; i <= kHannLutSize; ++i)
		hannLut_[i] = 0.5f * (1.0f - std::cos (juce::MathConstants<float>::twoPi
		              * (float) i / (float) kHannLutSize));

	// ── Initialize inverse sqrt LUT for granular normalization ──
	invSqrtLut_[0] = 1.0f;
	for (int i = 1; i <= kMaxGrains; ++i)
		invSqrtLut_[i] = 1.0f / std::sqrt ((float) i);

	// ── Initialize input buffer (power-of-2 for bitmask wrapping) ──
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

	// ── Initialize WSOLA state ──
	wsola_ = {};
	wsola_.readPos = 0.0;
	wsola_.segInputStart = 0.0;
	wsola_.segRemaining = 0;
	triggerWasOn_ = false;

	// ── Initialize Granular state ──
	for (int g = 0; g < kMaxGrains; ++g)
		grains_[g] = {};
	grainNextSlot_ = 0;
	grainSpawnCountdown_ = 0;
	grainReadPos_ = 0.0f;
	grainPrevOutL_ = 0.0f;
	grainPrevOutR_ = 0.0f;

	// ── Initialize STFT state ──
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
	chaosAmtD_ = 0.0f; chaosAmtF_ = 0.0f;
	chaosParamSmoothCoeff_ = std::exp (-1.0f / (static_cast<float> (currentSampleRate) * 0.02f));
	chaosDSmoothCoeff_ = std::exp (-1.0f / (static_cast<float> (currentSampleRate) * 0.005f));
	chaosGSmoothCoeff_ = chaosDSmoothCoeff_;
	chaosFSmoothCoeff_ = std::exp (-1.0f / (static_cast<float> (currentSampleRate) * 0.01f));
	chaosShPeriodD_ = 8820.0f; smoothedChaosShPeriodD_ = 8820.0f;
	chaosShPeriodF_ = 8820.0f; smoothedChaosShPeriodF_ = 8820.0f;
	chaosDelayMaxSamples_ = 0.0f; smoothedChaosDelayMaxSamples_ = 0.0f;
	chaosGainMaxDb_ = 0.0f; smoothedChaosGainMaxDb_ = 0.0f;
	chaosFilterMaxOct_ = 0.0f; smoothedChaosFilterMaxOct_ = 0.0f;
	chaosDPhase_ = 0.0f; chaosDTarget_ = 0.0f; chaosDSmoothed_ = 0.0f;
	chaosGPhase_ = 0.0f; chaosGTarget_ = 0.0f; chaosGSmoothed_ = 0.0f;
	chaosFPhase_ = 0.0f; chaosFTarget_ = 0.0f; chaosFSmoothed_ = 0.0f;
	std::memset (chaosDelayBuf_, 0, sizeof (chaosDelayBuf_));
	chaosDelayWritePos_ = 0;

	lastPan_ = 0.5f;
	lastPanLeft_  = 0.70710678f;
	lastPanRight_ = 0.70710678f;
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

	if (chaosFilterEnabled_ && chaosAmtF_ > 0.01f)
	{
		const float octaveShift = chaosFSmoothed_ * smoothedChaosFilterMaxOct_;
		const float freqMult = std::exp2 (octaveShift);
		const float hpBase = wetFilterHpOn_ ? hpTarget : kFilterFreqMin;
		const float lpBase = wetFilterLpOn_ ? lpTarget : kFilterFreqMax;
		hpTarget = juce::jlimit (kFilterFreqMin, kFilterFreqMax, hpBase * freqMult);
		lpTarget = juce::jlimit (kFilterFreqMin, kFilterFreqMax, lpBase * freqMult);
	}

	smoothedFilterHpFreq_ += (hpTarget - smoothedFilterHpFreq_) * kGainSmoothStep;
	smoothedFilterLpFreq_ += (lpTarget - smoothedFilterLpFreq_) * kGainSmoothStep;

	if (--filterCoeffCountdown_ <= 0)
	{
		filterCoeffCountdown_ = kFilterCoeffUpdateInterval;
		updateFilterCoeffs (false, false);
	}

	const bool chaosFilterActive = chaosFilterEnabled_ && chaosAmtF_ > 0.01f;
	if (wetFilterHpOn_ || chaosFilterActive)
	{
		for (int s = 0; s < wetFilterNumSectionsHp_; ++s)
		{
			wetL = processBiquad (wetL, hpCoeffs_[s], wetFilterState_[0].hp[s]);
			wetR = processBiquad (wetR, hpCoeffs_[s], wetFilterState_[1].hp[s]);
		}
	}

	if (wetFilterLpOn_ || chaosFilterActive)
	{
		for (int s = 0; s < wetFilterNumSectionsLp_; ++s)
		{
			wetL = processBiquad (wetL, lpCoeffs_[s], wetFilterState_[0].lp[s]);
			wetR = processBiquad (wetR, lpCoeffs_[s], wetFilterState_[1].lp[s]);
		}
	}

	// Tilt filter
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
// WSOLA cross-correlation overlap search

int STRETRAudioProcessor::wsolaBestOverlapOffset (int nominalPos, int overlapLen, int ch0Weight) const
{
	if (overlapLen <= 0 || inputBufLen_ <= 0) return 0;

	const int len = inputBufLen_;
	// Limit seek range to half the overlap — sufficient for good matches,
	// avoids O(overlapLen²) blowup with large windows.
	const int seekWindow = juce::jmin (overlapLen / 2, len / 4);
	// Coarser inner step for large windows (8192→step 8 vs 4)
	const int step = (overlapLen >= 2048) ? 8 : 4;
	float bestCorr = -1e30f;
	int bestOffset = 0;

	// Pre-wrap nominalPos once
	const int nomWrapped = ((nominalPos % len) + len) % len;

	for (int off = -seekWindow; off <= seekWindow; ++off)
	{
		float corr = 0.0f;
		// Compute start indices once per offset — avoid modulo in inner loop
		int idxA = ((nomWrapped + off) % len + len) % len;
		int idxB = nomWrapped;

		for (int j = 0; j < overlapLen; j += step)
		{
			corr += inputBuf_[0][(size_t) idxA] * inputBuf_[0][(size_t) idxB];
			if (ch0Weight < 2)
				corr += inputBuf_[1][(size_t) idxA] * inputBuf_[1][(size_t) idxB];

			// Advance with conditional wrap — much cheaper than modulo
			idxA += step; if (idxA >= len) idxA -= len;
			idxB += step; if (idxB >= len) idxB -= len;
		}
		if (corr > bestCorr) { bestCorr = corr; bestOffset = off; }
	}
	return bestOffset;
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
                                              float pitchRate, bool reverseOn)
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
		// ── Analysis ──
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
					phaseDiff  = std::fmod (phaseDiff + pi, twoPi);
					if (phaseDiff < 0.0f) phaseDiff += twoPi;
					phaseDiff -= pi;
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

		// ── Synthesis ──
		std::memset (fftWork_, 0, sizeof (float) * (size_t) (fftSize * 2));

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
					freq  = stft_.lastFreq[ch][s0] * pitchRate;
				}
				if (s0 + 1 < numBins)
					mag += stft_.lastMag[ch][s0 + 1] * fr;
			}
			else
			{
				mag  = stft_.lastMag[ch][k];
				freq = stft_.lastFreq[ch][k];
			}

			stft_.synthPhase[ch][k] += freq * (float) synthesisHop;

			fftWork_[k * 2]     = mag * std::cos (stft_.synthPhase[ch][k]);
			fftWork_[k * 2 + 1] = mag * std::sin (stft_.synthPhase[ch][k]);
		}

		// Mirror conjugates for negative frequencies
		for (int k = 1; k < fftSize / 2; ++k)
		{
			fftWork_[(fftSize - k) * 2]     =  fftWork_[k * 2];
			fftWork_[(fftSize - k) * 2 + 1] = -fftWork_[k * 2 + 1];
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

	// ── Read parameters ──
	const float inputGainDb  = loadAtomicOrDefault (inputParam, kInputDefault);
	const float outputGainDb = loadAtomicOrDefault (outputParam, kOutputDefault);
	const float mixValue     = loadAtomicOrDefault (mixParam, kMixDefault);

	const int modeInVal  = loadIntParamOrDefault (modeInParam,  kModeInOutDefault);
	const int modeOutVal = loadIntParamOrDefault (modeOutParam, kModeInOutDefault);
	const int sumBusVal  = loadIntParamOrDefault (sumBusParam,  kSumBusDefault);

	const float targetInputGain  = fastDecibelsToGain (inputGainDb);
	const float targetOutputGain = fastDecibelsToGain (outputGainDb);

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

	// ── Engine params ──
	const int   engineVal  = loadIntParamOrDefault (engineParam, 0);
	const float amountVal  = loadAtomicOrDefault (amountParam, kAmountDefault);   // 0..100
	const float modVal     = loadAtomicOrDefault (modParam, kModDefault);         // 0..1
	const int   windowVal  = loadIntParamOrDefault (windowParam, (int) kWindowDefault);
	const int   styleVal   = loadIntParamOrDefault (styleParam, 1);
	const float grainMs    = loadAtomicOrDefault (grainParam, kGrainDefault);     // 1..500 ms
	const bool  reverseOn  = loadBoolParamOrDefault (reverseParam, false);
	const bool  triggerOn  = loadBoolParamOrDefault (triggerParam, false);

	// Amount → speed: 0%=1.0 (no stretch), 100%=0.0 (freeze)
	// Unified mapping across all three engines — true freeze at 100%.
	const float targetSpeed = juce::jmax (0.0f, 1.0f - amountVal / 100.0f);

	// Mod → pitch rate: center (0.5)=1.0x, 0=0.0625x, 1=16x
	const float targetPitchRate = std::exp2 ((modVal - 0.5f) * 4.0f);

	// Window → continuous size (21..8192), smoothed
	const float targetWindow = (float) juce::jlimit (kWindowMin, kWindowMax, windowVal);
	const int windowSamples = (int) smoothedWindow_;

	// Grain size in samples
	const int grainSamples = juce::jmax (4, (int) (grainMs * 0.001f * (float) currentSampleRate));

	// Granular read rate: how fast the grain spawn position advances
	// Granular read rate computed per-sample from smoothedSpeed_ below

	// ── Trigger edge detection: reset engines on trigger press ──
	if (triggerOn && ! triggerWasOn_)
	{
		// Trigger just pressed — capture current write position as starting read point
		const double capturePos = (double) ((inputBufWritePos_ - 1 + inputBufLen_) & inputBufMask_);
		wsola_ = {};
		wsola_.readPos = capturePos;
		wsola_.segInputStart = capturePos;
		wsola_.segRemaining = 0;

		for (int g = 0; g < kMaxGrains; ++g)
			grains_[g].active = false;
		grainReadPos_ = (float) capturePos;
		grainSpawnCountdown_ = 0;
		grainPrevOutL_ = 0.0f;
		grainPrevOutR_ = 0.0f;
	}
	triggerWasOn_ = triggerOn;

	// ── FFT engine setup (auto-active, no trigger needed) ──
	if (engineVal == 2 && inputBufLen_ > 0)
	{
		// FFT requires power-of-2 sizes — snap continuous window value
		const int fftSize = juce::jlimit (64, kMaxFftSize, nextPowerOf2 (windowSamples));
		ensureFft (fftSize);
	}

	// ── PDC and Align ──
	{
		const bool alignOn = loadBoolParamOrDefault (alignParam, false);
		const bool pdcOn   = loadBoolParamOrDefault (pdcParam, false);
		const int  fftLat  = (engineVal == 2 && stft_.activeFftSize > 0)
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
		const float spd  = loadAtomicOrDefault (chaosSpdParam, kChaosSpdDefault);
		chaosShPeriodD_  = (float) currentSampleRate / juce::jmax (0.01f, spd);
		chaosDelayMaxSamples_ = chaosAmtD_ * 0.01f * (float) kChaosDelayBufLen * 0.5f;
		chaosGainMaxDb_       = chaosAmtD_ * 0.06f;
	}

	// ── Per-sample processing ──
	for (int i = 0; i < numSamples; ++i)
	{
		// Smooth gains
		smoothedInputGain  += (targetInputGain  - smoothedInputGain)  * kGainSmoothStep;
		smoothedOutputGain += (targetOutputGain - smoothedOutputGain) * kGainSmoothStep;
		smoothedMix        += (mixValue         - smoothedMix)        * kGainSmoothStep;
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

		// ── Write input to circular buffer ──
		if (inputBufLen_ > 0)
		{
			inputBuf_[0][inputBufWritePos_] = inL;
			inputBuf_[1][inputBufWritePos_] = inR;
			inputBufWritePos_ = (inputBufWritePos_ + 1) & inputBufMask_;
		}

		float wetL = 0.0f;
		float wetR = 0.0f;

		// ── Engine dispatch ──
		if (engineVal == 2 && inputBufLen_ > 0 && stft_.activeFftSize > 0)
		{
			// ── Engine 2: FFT (Spectral Time Stretch / Phase Vocoder) ──
			// Always active — no trigger needed
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
				const int fftAnalysisHop = (int) ((float) fftSynthHop * speed);
				performStftCycle (stft_.activeFftSize, fftAnalysisHop,
				                  fftSynthHop, pitchRate, reverseOn);
			}
		}
		else if (! triggerOn)
		{
			// Trigger OFF → passthrough (engines 0 & 1)
			wetL = inL;
			wetR = inR;
		}
		else if (engineVal == 0 && inputBufLen_ > 0)
		{
			// ── Engine 0: WSOLA (elastic time stretch) ──
			// Within each segment: read at pitchRate (1.0 = no pitch change)
			// Between segments: analysis hop controls time stretch ratio
			const int segLen = juce::jmax (64, windowSamples);
			const int overlapLen = juce::jmax (16, segLen / 4);
			// analysisHop: how far to jump in input per segment
			// = segLen * speed * pitchRate
			// speed=1 → 1:1, speed=0 → freeze (analysisHop=0, reads same position)
			const double analysisHop = (double) segLen * (double) speed * (double) pitchRate;

			if (wsola_.segRemaining <= 0)
			{
				// Start new segment: advance segInputStart by analysisHop
				if (wsola_.segLen > 0)  // not the very first segment
				{
					const double direction = reverseOn ? -1.0 : 1.0;
					wsola_.segInputStart += analysisHop * direction;
				}

				wsola_.segLen = segLen;
				wsola_.overlapLen = overlapLen;

				// Find best overlap offset via cross-correlation
				int nomInt = ((int) wsola_.segInputStart % inputBufLen_ + inputBufLen_) % inputBufLen_;
				const int bestOff = wsolaBestOverlapOffset (nomInt, overlapLen,
				                                           (styleVal == 0) ? 2 : 0);
				wsola_.segInputStart = (double) (nomInt + bestOff);
				wsola_.readPos = wsola_.segInputStart;

				wsola_.segRemaining = segLen;
				wsola_.overlapRemain = overlapLen;
			}

			// Read from input buffer at current read position
			const double rp = wsola_.readPos;
			float sL = readInputBuf (0, rp);
			float sR = readInputBuf (1, rp);

			// Crossfade with previous segment tail at start of new segment
			const int posInSeg = wsola_.segLen - wsola_.segRemaining;
			if (posInSeg < wsola_.overlapLen && wsola_.overlapLen > 0)
			{
				const float fadeIn = (float) posInSeg / (float) wsola_.overlapLen;
				const float fadeOut = 1.0f - fadeIn;
				sL = sL * fadeIn + wsola_.prevTailL[posInSeg] * fadeOut;
				sR = sR * fadeIn + wsola_.prevTailR[posInSeg] * fadeOut;
			}

			// Save tail for next crossfade (last overlapLen samples of segment)
			const int tailStart = wsola_.segLen - wsola_.overlapLen;
			if (posInSeg >= tailStart && posInSeg < wsola_.segLen)
			{
				const int tailIdx = posInSeg - tailStart;
				if (tailIdx < 8192)
				{
					wsola_.prevTailL[tailIdx] = sL;
					wsola_.prevTailR[tailIdx] = sR;
				}
			}

			wetL = sL;
			wetR = sR;

			// Advance read position within segment at pitchRate
			// pitchRate=1.0 → read 1:1 → no pitch change (elastic!)
			// pitchRate>1 → read faster → higher pitch
			const double direction = reverseOn ? -1.0 : 1.0;
			wsola_.readPos += (double) pitchRate * direction;

			// Wrap read position
			if (wsola_.readPos >= (double) inputBufLen_)
				wsola_.readPos -= (double) inputBufLen_;
			else if (wsola_.readPos < 0.0)
				wsola_.readPos += (double) inputBufLen_;

			wsola_.segRemaining--;
		}
		else if (engineVal == 1 && inputBufLen_ > 0)
		{
			// ── Engine 1: GRANULAR ──
			// Advance grain spawn read position (slower = more stretch)
			const float direction = reverseOn ? -1.0f : 1.0f;
			grainReadPos_ += speed * direction;
			if (grainReadPos_ >= (float) inputBufLen_)
				grainReadPos_ -= (float) inputBufLen_;
			else if (grainReadPos_ < 0.0f)
				grainReadPos_ += (float) inputBufLen_;

			// Spawn new grains
			if (--grainSpawnCountdown_ <= 0)
			{
				// Overlap: spawn interval = grainSamples / density
				// density derived from window param: more overlap = smoother
				const int density = juce::jmax (1, windowSamples / 64);
				grainSpawnCountdown_ = juce::jmax (1, grainSamples / density);

				// Find an inactive slot
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
						grainNextSlot_ = (slot + 1) % kMaxGrains;
						break;
					}
				}
			}

			// Mix active grains
			float sumL = 0.0f, sumR = 0.0f;
			int activeCount = 0;
			for (int g = 0; g < kMaxGrains; ++g)
			{
				auto& gr = grains_[g];
				if (! gr.active) continue;
				activeCount++;

				// Hann window envelope
				const float phase = (float) gr.elapsed / (float) gr.length;
				const float env = hannWindow (phase);

				// Read position in input buffer
				const double bufPos = gr.readPos + gr.playPos;
				const float sL = readInputBuf (0, bufPos);
				const float sR = readInputBuf (1, bufPos);

				sumL += sL * env;
				sumR += sR * env;

				// Advance grain playback position
				if (gr.reverse)
					gr.playPos -= gr.rate;
				else
					gr.playPos += gr.rate;

				gr.elapsed++;
				if (gr.elapsed >= gr.length)
					gr.active = false;
			}

			// Normalize by sqrt of active grains to prevent excessive buildup
			if (activeCount > 1)
			{
				const float norm = invSqrtLut_[juce::jmin (activeCount, kMaxGrains)];
				sumL *= norm;
				sumR *= norm;
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
			if (styleVal == 2) // WIDE: exaggerate stereo
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
			// DUAL could differ in grain engine (independent L/R read positions) — future
		}

		// Chaos engines
		if (chaosFilterEnabled_) advanceChaosF();
		if (chaosDelayEnabled_)  advanceChaosD();

		// Wet-signal filter + tilt
		filterWetSample (wetL, wetR);

		// Chaos delay
		if (chaosDelayEnabled_ && chaosAmtD_ > 0.01f)
		{
			chaosDelayBuf_[0][chaosDelayWritePos_] = wetL;
			chaosDelayBuf_[1][chaosDelayWritePos_] = wetR;

			const float delaySamples = juce::jlimit (0.0f, (float)(kChaosDelayBufLen - 1),
			                                         chaosDSmoothed_ * smoothedChaosDelayMaxSamples_);
			const int delayInt = (int) delaySamples;
			const float delayFrac = delaySamples - (float) delayInt;
			const int readPos0 = (chaosDelayWritePos_ - delayInt + kChaosDelayBufLen) & (kChaosDelayBufLen - 1);
			const int readPos1 = (readPos0 - 1 + kChaosDelayBufLen) & (kChaosDelayBufLen - 1);
			wetL = chaosDelayBuf_[0][readPos0] + delayFrac * (chaosDelayBuf_[0][readPos1] - chaosDelayBuf_[0][readPos0]);
			wetR = chaosDelayBuf_[1][readPos0] + delayFrac * (chaosDelayBuf_[1][readPos1] - chaosDelayBuf_[1][readPos0]);

			const float chaosGainDb = chaosGSmoothed_ * smoothedChaosGainMaxDb_;
			const float chaosGain = fastDecibelsToGain (chaosGainDb);
			wetL *= chaosGain;
			wetR *= chaosGain;

			chaosDelayWritePos_ = (chaosDelayWritePos_ + 1) & (kChaosDelayBufLen - 1);
		}

		// Mode Out: M/S encode wet output
		if (numChannels >= 2 && modeOutVal != 0)
		{
			const float l = wetL, r = wetR;
			if (modeOutVal == 1)      { const float mid  = (l + r) * kSqrt2Over2; wetL = wetR = mid; }
			else /* modeOutVal==2 */   { const float side = (l - r) * kSqrt2Over2; wetL = wetR = side; }
		}

		// Mix dry/wet with Sum Bus routing
		const float dL = dryOrigL * (1.0f - smoothedMix);
		const float dR = dryOrigR * (1.0f - smoothedMix);
		const float wL = wetL * smoothedMix * smoothedOutputGain;
		const float wR = wetR * smoothedMix * smoothedOutputGain;

		if (sumBusVal == 0) // ST
		{
			channelL[i] = dL + wL;
			if (channelR != nullptr) channelR[i] = dR + wR;
		}
		else if (sumBusVal == 1) // →M
		{
			const float midBus = (wL + wR) * 0.5f;
			channelL[i] = dL + midBus;
			if (channelR != nullptr) channelR[i] = dR + midBus;
		}
		else // →S
		{
			const float sideBus = (wL - wR) * 0.5f;
			channelL[i] = dL + sideBus;
			if (channelR != nullptr) channelR[i] = dR - sideBus;
		}
	}

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
		juce::NormalisableRange<float> (kGrainMin, kGrainMax, 0.01f, 0.35f), kGrainDefault));
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

	// UI state
	params.push_back (std::make_unique<juce::AudioParameterInt> (kParamUiWidth, "UI Width", 360, 1600, 360));
	params.push_back (std::make_unique<juce::AudioParameterInt> (kParamUiHeight, "UI Height", 240, 1200, 480));
	params.push_back (std::make_unique<juce::AudioParameterBool> (kParamUiPalette, "UI Palette", false));
	params.push_back (std::make_unique<juce::AudioParameterBool> (kParamUiCrt, "UI CRT", false));
	params.push_back (std::make_unique<juce::AudioParameterInt> (kParamUiColor0, "UI Color 0", 0, 0xFFFFFF, 0xFFFFFF));
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
