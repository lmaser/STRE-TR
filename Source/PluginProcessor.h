#pragma once

#include <JuceHeader.h>
#include <array>
#include <atomic>
#include <vector>
#include "PerfTrace.h"

// Compile-time gate for the temporary FFT amount/click CSV dump.
// Keep disabled for release builds unless explicitly diagnosing this path.
#ifndef STRETR_ENABLE_FFT1_CLICK_DUMP
#define STRETR_ENABLE_FFT1_CLICK_DUMP 0
#endif

#if STRETR_ENABLE_FFT1_CLICK_DUMP
struct StretrDumpStageDelta
{
	int   sample = -1;
	float absDeltaL = 0.0f;
	float absDeltaR = 0.0f;
	float prevL = 0.0f;
	float prevR = 0.0f;
	float currL = 0.0f;
	float currR = 0.0f;
};
#endif

class STRETRAudioProcessor : public juce::AudioProcessor
{
public:
	STRETRAudioProcessor();
	~STRETRAudioProcessor() override;

    // Parameter IDs
	static constexpr const char* kParamAmount    = "amount";
	static constexpr const char* kParamPitch     = "pitch";
	static constexpr const char* kParamGrain     = "grain";
    static constexpr const char* kParamEngine    = "engine";     // 0=STRETCH 1=GRAIN 2=FFT1 3=FFT2
	static constexpr const char* kParamWindow    = "window";     // 16..8192; FFT engines snap to powers of two
	static constexpr const char* kParamMaxWindow = "max_window"; // FFT1/FFT2 max window for fixed PDC/ALIGN budget
	static constexpr const char* kParamJitter    = "jitter";
	static constexpr const char* kParamStyle     = "style";      // 0=MONO 1=STEREO 2=WIDE 3=DUAL
	static constexpr const char* kParamInput     = "input";
	static constexpr const char* kParamOutput    = "output";
	static constexpr const char* kParamMix       = "mix";
	static constexpr const char* kParamModeIn    = "mode_in";
	static constexpr const char* kParamModeOut   = "mode_out";
	static constexpr const char* kParamSumBus    = "sum_bus";
	static constexpr const char* kParamLimThreshold = "lim_threshold";
	static constexpr const char* kParamLimMode      = "lim_mode";
	static constexpr const char* kParamInvPol       = "inv_pol";
	static constexpr const char* kParamInvStr       = "inv_str";
	static constexpr const char* kParamAlign     = "align";
	static constexpr const char* kParamPdc       = "pdc";
	static constexpr const char* kParamReverse   = "reverse";
	static constexpr const char* kParamTrigger   = "trigger";

	// Filter parameter IDs
	static constexpr const char* kParamFilterHpFreq  = "filter_hp_freq";
	static constexpr const char* kParamFilterLpFreq  = "filter_lp_freq";
	static constexpr const char* kParamFilterHpSlope = "filter_hp_slope";
	static constexpr const char* kParamFilterLpSlope = "filter_lp_slope";
	static constexpr const char* kParamFilterHpOn    = "filter_hp_on";
	static constexpr const char* kParamFilterLpOn    = "filter_lp_on";

	// Mix Mode + Dry/Wet levels (SEND mode)
	static constexpr const char* kParamMixMode  = "mix_mode";
	static constexpr const char* kParamDryLevel = "dry_level";
	static constexpr const char* kParamWetLevel = "wet_level";

	// Filter position
	static constexpr const char* kParamFilterPos = "filter_pos";

	// Tilt / Pan
	static constexpr const char* kParamTilt = "tilt";
	static constexpr const char* kParamPan  = "pan";

	// Chaos parameter IDs
	static constexpr const char* kParamChaos          = "chaos";
	static constexpr const char* kParamChaosD         = "chaos_d";
	static constexpr const char* kParamChaosAmt       = "chaos_amt";
	static constexpr const char* kParamChaosSpd       = "chaos_spd";
	static constexpr const char* kParamChaosAmtFilter = "chaos_amt_filter";
	static constexpr const char* kParamChaosSpdFilter = "chaos_spd_filter";

	// UI state parameters (hidden from DAW automation)
	// Limiter constants
	static constexpr float kLimThresholdMin     = -36.0f;
	static constexpr float kLimThresholdMax     =   0.0f;
	static constexpr float kLimThresholdDefault =   0.0f;
	static constexpr int   kLimModeDefault      =   0;
	static constexpr int   kMixModeDefault   = 0;   // 0=INSERT, 1=SEND
	static constexpr float kDryLevelDefault  = 0.0f;
	static constexpr float kWetLevelDefault  = 1.0f;
	static constexpr int   kFilterPosDefault = 0;   // 0=POST, 1=PRE

	static constexpr const char* kParamUiWidth    = "ui_width";
	static constexpr const char* kParamUiHeight   = "ui_height";
	static constexpr const char* kParamUiPalette  = "ui_palette";
	static constexpr const char* kParamUiCrt      = "ui_fx_tail";
	static constexpr const char* kParamUiColor0   = "ui_color0";
	static constexpr const char* kParamUiColor1   = "ui_color1";

    // Parameter ranges and defaults
	static constexpr float kAmountMin     = 0.0f;
	static constexpr float kAmountMax     = 100.0f;
	static constexpr float kAmountDefault = 0.0f;

	static constexpr float kPitchMin     = 0.0f;
	static constexpr float kPitchMax     = 1.0f;
	static constexpr float kPitchDefault = 0.5f;

	static constexpr float kGrainMin     = 1.0f;
	static constexpr float kGrainMax     = 500.0f;
	static constexpr float kGrainDefault = 100.0f;

	static constexpr int   kEngineMin     = 0;
	static constexpr int   kEngineMax     = 3;
	static constexpr float kEngineDefault = 0.0f;

	static constexpr int   kWindowMin     = 16;
	static constexpr int   kWindowMax     = 8192;
	static constexpr int   kFftWindowMin  = 64;
	static constexpr int   kFftMaxWindowDefault = 8192;
	static constexpr int   kNumFftWindows = 8;
	static constexpr int   kFftWindows[kNumFftWindows] = { 64, 128, 256, 512, 1024, 2048, 4096, 8192 };
	static constexpr float kWindowDefault = 1024.0f;

	static constexpr float kJitterMin     = 0.0f;
	static constexpr float kJitterMax     = 100.0f;
	static constexpr float kJitterDefault = 0.0f;

	static constexpr int   kStyleMin     = 0;
	static constexpr int   kStyleMax     = 3;
	static constexpr float kStyleDefault = 1.0f;

	static constexpr float kGainFloorDb  = -144.0f;
	static constexpr float kGainMaxDb    =   24.0f;
	static constexpr float kGainDefaultDb =   0.0f;
	static constexpr float kGainSkew     = 4.4965561056f; // 0 dB at the fader midpoint

	static constexpr float kInputMin     = kGainFloorDb;
	static constexpr float kInputMax     = kGainMaxDb;
	static constexpr float kInputDefault = kGainDefaultDb;

	static constexpr float kOutputMin     = kGainFloorDb;
	static constexpr float kOutputMax     = kGainMaxDb;
	static constexpr float kOutputDefault = kGainDefaultDb;

	static constexpr float kMixMin     = 0.0f;
	static constexpr float kMixMax     = 1.0f;
	static constexpr float kMixDefault = 1.0f;

	static constexpr int   kModeInOutDefault = 0;
	static constexpr int   kSumBusDefault    = 0;
	static constexpr int   kInvPolDefault    = 0;   // 0=NONE  1=WET  2=GLOBAL
	static constexpr int   kInvStrDefault    = 0;   // 0=NONE  1=WET  2=GLOBAL
	static constexpr float kSqrt2Over2       = 0.707106781f;

	static constexpr float kFilterFreqMin       = 20.0f;
	static constexpr float kFilterFreqMax       = 20000.0f;
	static constexpr float kFilterHpFreqDefault = 250.0f;
	static constexpr float kFilterLpFreqDefault = 2000.0f;
	static constexpr int   kFilterSlopeMin      = 0;
	static constexpr int   kFilterSlopeMax      = 2;
	static constexpr int   kFilterSlopeDefault  = 1;

	static constexpr float kTiltMin     = -6.0f;
	static constexpr float kTiltMax     =  6.0f;
	static constexpr float kTiltDefault =  0.0f;

	static constexpr float kPanMin     = 0.0f;
	static constexpr float kPanMax     = 1.0f;
	static constexpr float kPanDefault = 0.5f;

	static constexpr float kChaosAmtMin     = 0.0f;
	static constexpr float kChaosAmtMax     = 100.0f;
	static constexpr float kChaosAmtDefault = 50.0f;
	static constexpr float kChaosSpdMin     = 0.01f;
	static constexpr float kChaosSpdMax     = 100.0f;
	static constexpr float kChaosSpdDefault = 5.0f;

	// Round up to nearest power of 2 (for FFT engine)
	static int nextPowerOf2 (int v)
	{
		int p = 1;
		while (p < v) p <<= 1;
		return p;
	}

	static int getCanonicalFftWindow (int windowValue) noexcept
	{
		const int clamped = juce::jlimit (kFftWindowMin, kWindowMax, windowValue);
		return juce::jlimit (kFftWindowMin, kWindowMax, nextPowerOf2 (clamped));
	}

	static int getFftWindowLane (int windowValue) noexcept
	{
		const int canonical = getCanonicalFftWindow (windowValue);
		for (int i = 0; i < kNumFftWindows; ++i)
			if (kFftWindows[i] == canonical)
				return i;
		return kNumFftWindows - 1;
	}

	static int getCanonicalWindowForEngine (int engineVal, int windowValue) noexcept
	{
		const int clamped = juce::jlimit (kWindowMin, kWindowMax, windowValue);
		return (engineVal == 2 || engineVal == 3) ? getCanonicalFftWindow (clamped) : clamped;
	}

    // AudioProcessor overrides
	void prepareToPlay (double sampleRate, int samplesPerBlock) override;
	void releaseResources() override;

#if ! JucePlugin_PreferredChannelConfigurations
	bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
#endif

	void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

	juce::AudioProcessorEditor* createEditor() override;
	bool hasEditor() const override;

	const juce::String getName() const override;
	bool acceptsMidi() const override;
	bool producesMidi() const override;
	bool isMidiEffect() const override;
	double getTailLengthSeconds() const override;

	int getNumPrograms() override;
	int getCurrentProgram() override;
	void setCurrentProgram (int index) override;
	const juce::String getProgramName (int index) override;
	void changeProgramName (int index, const juce::String& newName) override;

	void getStateInformation (juce::MemoryBlock& destData) override;
	void setStateInformation (const void* data, int sizeInBytes) override;
	void getCurrentProgramStateInformation (juce::MemoryBlock& destData) override;
	void setCurrentProgramStateInformation (const void* data, int sizeInBytes) override;

	void setUiEditorSize (int width, int height);
	int  getUiEditorWidth() const noexcept;
	int  getUiEditorHeight() const noexcept;

	void setUiUseCustomPalette (bool shouldUseCustomPalette);
	bool getUiUseCustomPalette() const noexcept;

	void setUiCrtEnabled (bool enabled);
	bool getUiCrtEnabled() const noexcept;

	void setUiIoExpanded (bool expanded);
	bool getUiIoExpanded() const noexcept;

	int getStoredWindowForEngine (int engineVal) const noexcept;
	void setStoredWindowForEngine (int engineVal, int windowValue) noexcept;
	void syncWindowParameterToEngine (int engineVal);
	int getCurrentMaxFftWindow() const noexcept;
	void clampFftWindowFamiliesToMax (int maxWindow) noexcept;

	void setUiCustomPaletteColour (int index, juce::Colour colour);
	juce::Colour getUiCustomPaletteColour (int index) const noexcept;

	juce::AudioProcessorValueTreeState apvts;
	static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

	struct WetFilterBiquadCoeffs { float b0 = 1.0f, b1 = 0.0f, b2 = 0.0f, a1 = 0.0f, a2 = 0.0f; };
	struct WetFilterBiquadState  { float z1 = 0.0f, z2 = 0.0f; };

	PerfTrace perfTrace;

private:
	struct UiStateKeys
	{
		static constexpr const char* editorWidth      = "uiEditorWidth";
		static constexpr const char* editorHeight     = "uiEditorHeight";
		static constexpr const char* useCustomPalette = "uiUseCustomPalette";
		static constexpr const char* crtEnabled       = "uiFxTailEnabled";
		static constexpr const char* ioExpanded       = "uiIoExpanded";
		static constexpr const char* stretchWindow    = "stretchWindow";
		static constexpr const char* grainWindow      = "grainWindow";
		static constexpr const char* fft1Window       = "fft1Window";
		static constexpr const char* fft2Window       = "fft2Window";
		static constexpr const char* fftWindow        = "fftWindow";
		static constexpr std::array<const char*, 2> customPalette {
			"uiCustomPalette0", "uiCustomPalette1"
		};
	};

	enum class WindowFamily : int
	{
		stretch = 0,
		grain   = 1,
		fft1    = 2,
		fft2    = 3
	};

	double currentSampleRate = 44100.0;

    // Circular input buffer (shared by all engines)
	static constexpr int kInputBufMaxLen = 2097152;  // 2^21, ~47.6s @ 44100
	std::vector<float> inputBuf_[2];    // L, R
	int inputBufWritePos_ = 0;
	int inputBufLen_ = 0;
	int inputBufMask_ = 0;  // power-of-2 bitmask for fast wrapping
	double inputBufWriteAbsPos_ = 0.0;

	bool  triggerWasOn_ = false;  // tracks previous trigger state for edge detection
	bool  transportWasPlaying_ = false;
	bool  transportHasSamplePos_ = false;
	juce::int64 transportLastSamplePos_ = 0;

	static constexpr int kWsolaOutBufLen = 32768; // 2^15, enough for max segment + overlap scheduling

    // WSOLA engine state
	struct WsolaState
	{
		double segInputStart        = 0.0;  // nominal input position for next L segment
		double segInputStartR       = 0.0;  // DUAL: nominal input position for next R segment
		int    segLen               = 0;
		int    overlapLen           = 0;
		int    synthesisHop         = 0;
		int    samplesUntilNextSeg  = 0;
		int    outputReadPos        = 0;
		int    nextSynthPos         = 0;
		int    lastBestOffset       = 0;
		int    lastBestOffsetR      = 0;
		double lastAnalysisHop      = 0.0;
		bool   hasPrevTail          = false; // true once at least one segment has been scheduled
		float  outputAccumL[kWsolaOutBufLen] = {};
		float  outputAccumR[kWsolaOutBufLen] = {};
	};
	WsolaState wsola_;
	bool wsolaUnityBypassActive_ = false;
	int  stretchBootstrapSegments_ = 0;
	int  stretchTransitionRemaining_ = 0;
	int  stretchTransitionTotal_ = 0;
	bool stretchTransitionToUnity_ = false;

	struct WsolaMatchResult
	{
		int   bestOffset    = 0;
		float bestScore     = 0.0f;
		float bestNormCorr  = 0.0f;
		float centerPenalty = 0.0f;
		float driftPenalty  = 0.0f;
		float startDeltaL   = 0.0f;
		float startDeltaR   = 0.0f;
		float overlapRmseL  = 0.0f;
		float overlapRmseR  = 0.0f;
	};

	class DebugCsvTraceSupport
	{
	public:
		void setAutoDumpPath (const juce::String& path) { autoDumpPath = path; }

	protected:
		static int nextRingIndex (std::atomic<int>& writeIndex, int ringSize) noexcept
		{
			return writeIndex.fetch_add (1, std::memory_order_relaxed) & (ringSize - 1);
		}

		static int ringTotal (int writeCount, int ringSize) noexcept
		{
			return juce::jmin (writeCount, ringSize);
		}

		static int ringStart (int writeCount, int ringSize) noexcept
		{
			return writeCount - ringTotal (writeCount, ringSize);
		}

		void enableDesktopAutoDump (const juce::String& filename)
		{
			auto desktop = juce::File::getSpecialLocation (juce::File::userDesktopDirectory);
			setAutoDumpPath (desktop.getChildFile (filename).getFullPathName());
		}

		bool shouldAutoDump (int writeCount) const
		{
			return autoDumpPath.isNotEmpty() && writeCount > 0;
		}

		const juce::String& getAutoDumpPath() const { return autoDumpPath; }

	private:
		juce::String autoDumpPath;
	};

#if JUCE_DEBUG
	struct StretchDebugEntry
	{
		int    blockIndex      = 0;
		int    sampleIndex     = 0;
		float  amount          = 0.0f;
		float  pitch             = 0.0f;
		float  speed           = 0.0f;
		float  pitchRate       = 0.0f;
		int    windowSamples   = 0;
		int    segLen          = 0;
		int    overlapLen      = 0;
		double analysisHop     = 0.0;
		double segInputStart   = 0.0;
		int    nominalPos      = 0;
		int    prevBestOffset  = 0;
		int    bestOffset      = 0;
		int    style           = 0;
		int    eventType       = 0; // 0=segment, 1=unity_bypass, 2=transport_reset
		int    reverseOn       = 0;
		int    triggerOn       = 0;
		int    hasPrevTail     = 0;
		int    nearUnity       = 0;
		float  bestScore       = 0.0f;
		float  bestNormCorr    = 0.0f;
		float  centerPenalty   = 0.0f;
		float  driftPenalty    = 0.0f;
		float  startDeltaL     = 0.0f;
		float  startDeltaR     = 0.0f;
		float  overlapRmseL    = 0.0f;
		float  overlapRmseR    = 0.0f;
	};

	class StretchDebugTrace : private DebugCsvTraceSupport
	{
	public:
		static constexpr int kRingSize = 32768;
		using DebugCsvTraceSupport::setAutoDumpPath;

		void record (const StretchDebugEntry& entry) noexcept
		{
			const int idx = nextRingIndex (writeIndex, kRingSize);
			ring[idx] = entry;
		}

		void enableDesktopAutoDump (const juce::String& filename = "stretr_stretch_dump.csv")
		{
			DebugCsvTraceSupport::enableDesktopAutoDump (filename);
		}

		bool dumpToFile (const juce::String& filePath) const
		{
			juce::File f (filePath);
			if (f.existsAsFile())
				f.deleteFile();

			if (auto stream = f.createOutputStream())
			{
				stream->writeText (
					"block_index,sample_index,event,amount,pitch,speed,pitch_rate,window_samples,seg_len,overlap_len,"
					"analysis_hop,seg_input_start,nominal_pos,prev_best_offset,best_offset,style,reverse_on,trigger_on,"
					"has_prev_tail,near_unity,best_score,best_norm_corr,center_penalty,drift_penalty,start_delta_l,"
					"start_delta_r,overlap_rmse_l,overlap_rmse_r\n",
					false, false, nullptr);

				const int writeCount = writeIndex.load (std::memory_order_relaxed);
				const int total = ringTotal (writeCount, kRingSize);
				const int startIdx = ringStart (writeCount, kRingSize);
				for (int i = 0; i < total; ++i)
				{
					const auto& e = ring[(startIdx + i) & (kRingSize - 1)];
					const juce::String eventName = (e.eventType == 1) ? "unity_bypass"
						: (e.eventType == 2) ? "transport_reset"
						: "segment";
					juce::String line;
					line << e.blockIndex << ","
					     << e.sampleIndex << ","
					     << eventName << ","
					     << juce::String (e.amount, 4) << ","
					     << juce::String (e.pitch, 4) << ","
					     << juce::String (e.speed, 6) << ","
					     << juce::String (e.pitchRate, 6) << ","
					     << e.windowSamples << ","
					     << e.segLen << ","
					     << e.overlapLen << ","
					     << juce::String (e.analysisHop, 6) << ","
					     << juce::String (e.segInputStart, 6) << ","
					     << e.nominalPos << ","
					     << e.prevBestOffset << ","
					     << e.bestOffset << ","
					     << e.style << ","
					     << e.reverseOn << ","
					     << e.triggerOn << ","
					     << e.hasPrevTail << ","
					     << e.nearUnity << ","
					     << juce::String (e.bestScore, 6) << ","
					     << juce::String (e.bestNormCorr, 6) << ","
					     << juce::String (e.centerPenalty, 6) << ","
					     << juce::String (e.driftPenalty, 6) << ","
					     << juce::String (e.startDeltaL, 6) << ","
					     << juce::String (e.startDeltaR, 6) << ","
					     << juce::String (e.overlapRmseL, 6) << ","
					     << juce::String (e.overlapRmseR, 6) << "\n";
					stream->writeText (line, false, false, nullptr);
				}

				stream->flush();
				return true;
			}
			return false;
		}

		~StretchDebugTrace()
		{
			if (shouldAutoDump (writeIndex.load (std::memory_order_relaxed)))
				dumpToFile (getAutoDumpPath());
		}

	private:
		StretchDebugEntry ring[kRingSize] {};
		std::atomic<int> writeIndex { 0 };
	};

	StretchDebugTrace stretchDebugTrace_;
	int stretchDebugBlockCounter_ = 0;
#endif

	void resetWsolaAtPos (double capturePos) noexcept;
	WsolaMatchResult wsolaBestOverlapOffset (int channel, double nominalPos, int overlapLen,
	                                         int prevBestOffset, bool nearUnity,
	                                         float readRate, int synthPos) const;
	double currentCaptureAbsPos() const noexcept;
	double computeGrainLookBehind (int grainSamples, float pitchRate,
	                               bool reverseOn, bool wideMode) const noexcept;
	double clampGrainSpawnPos (double desiredPos, double capturePos,
	                           double lookBehind) const noexcept;
	void resetGrainAtCapturePos (double capturePos, int grainSamples, float pitchRate,
	                             bool reverseOn, bool wideMode) noexcept;
	int countActiveGrains() const noexcept;

#if JUCE_DEBUG
	struct GrainDebugEntry
	{
		int    blockIndex     = 0;
		int    sampleIndex    = 0;
		int    eventType      = 0; // 0=spawn, 1=trigger_reset, 2=engine_reset, 3=transport_reset
		float  amount         = 0.0f;
		float  pitch            = 0.0f;
		float  speed          = 0.0f;
		float  pitchRate      = 0.0f;
		int    windowSamples  = 0;
		int    grainSamples   = 0;
		int    density        = 0;
		int    spawnInterval  = 0;
		int    style          = 0;
		int    reverseOn      = 0;
		int    triggerOn      = 0;
		int    activeGrains   = 0;
		double capturePos     = 0.0;
		double readPosBefore  = 0.0;
		double spawnPos       = 0.0;
		double readPosAfter   = 0.0;
		double lookBehind     = 0.0;
		double futureMargin   = 0.0;
	};

	class GrainDebugTrace : private DebugCsvTraceSupport
	{
	public:
		static constexpr int kRingSize = 32768;
		using DebugCsvTraceSupport::setAutoDumpPath;

		void record (const GrainDebugEntry& entry) noexcept
		{
			const int idx = nextRingIndex (writeIndex, kRingSize);
			ring[idx] = entry;
		}

		void enableDesktopAutoDump (const juce::String& filename = "stretr_grain_dump.csv")
		{
			DebugCsvTraceSupport::enableDesktopAutoDump (filename);
		}

		bool dumpToFile (const juce::String& filePath) const
		{
			juce::File f (filePath);
			if (f.existsAsFile())
				f.deleteFile();

			if (auto stream = f.createOutputStream())
			{
				stream->writeText (
					"block_index,sample_index,event,amount,pitch,speed,pitch_rate,window_samples,grain_samples,"
					"density,spawn_interval,style,reverse_on,trigger_on,active_grains,capture_pos,"
					"read_pos_before,spawn_pos,read_pos_after,look_behind,future_margin\n",
					false, false, nullptr);

				const int writeCount = writeIndex.load (std::memory_order_relaxed);
				const int total = ringTotal (writeCount, kRingSize);
				const int startIdx = ringStart (writeCount, kRingSize);
				for (int i = 0; i < total; ++i)
				{
					const auto& e = ring[(startIdx + i) & (kRingSize - 1)];
					const juce::String eventName = (e.eventType == 1) ? "trigger_reset"
						: (e.eventType == 2) ? "engine_reset"
						: (e.eventType == 3) ? "transport_reset"
						: "spawn";
					juce::String line;
					line << e.blockIndex << ","
					     << e.sampleIndex << ","
					     << eventName << ","
					     << juce::String (e.amount, 4) << ","
					     << juce::String (e.pitch, 4) << ","
					     << juce::String (e.speed, 6) << ","
					     << juce::String (e.pitchRate, 6) << ","
					     << e.windowSamples << ","
					     << e.grainSamples << ","
					     << e.density << ","
					     << e.spawnInterval << ","
					     << e.style << ","
					     << e.reverseOn << ","
					     << e.triggerOn << ","
					     << e.activeGrains << ","
					     << juce::String (e.capturePos, 6) << ","
					     << juce::String (e.readPosBefore, 6) << ","
					     << juce::String (e.spawnPos, 6) << ","
					     << juce::String (e.readPosAfter, 6) << ","
					     << juce::String (e.lookBehind, 6) << ","
					     << juce::String (e.futureMargin, 6) << "\n";
					stream->writeText (line, false, false, nullptr);
				}

				stream->flush();
				return true;
			}
			return false;
		}

		~GrainDebugTrace()
		{
			if (shouldAutoDump (writeIndex.load (std::memory_order_relaxed)))
				dumpToFile (getAutoDumpPath());
		}

	private:
		GrainDebugEntry ring[kRingSize] {};
		std::atomic<int> writeIndex { 0 };
	};

	GrainDebugTrace grainDebugTrace_;
#endif

	struct FftDebugContext
	{
		int   blockIndex    = 0;
		int   sampleIndex   = 0;
		int   engine        = 0;
		float amount        = 0.0f;
		float pitch           = 0.0f;
		float speed         = 0.0f;
		float pitchRate     = 1.0f;
		float targetAnalysisHop = 0.0f;
		float filteredAnalysisHop = 0.0f;
		float analysisHopQuantError = 0.0f;
		int   lastAnalysisHop = -1;
		int   freezeEntryWarmupCycles = 0;
		int   fftStartupWarmupRemainingCycles = 0;
		int   fftExplicitFreezeActive = 0;
		int   fftExplicitFreezeCapturePending = 0;
		int   fftTargetFreeze = 0;
		int   analysisHopDebug = -1;
		int   windowSamples = 0;
		int   style         = 0;
		int   reverseOn     = 0;
		int   triggerOn     = 0;
		int   alignOn       = 0;
		int   pdcOn         = 0;
		int   reportedLatency = 0;
		int   dryDelayLen   = 0;
		int   fftOutputPadLen = 0;
		float smoothedWindow = 0.0f;
		float targetWindow = 0.0f;
		int   windowTransitionActive = 0;
		float windowTransitionProgress = 0.0f;
		int   fftOutputFadeActive = 0;
		float fftOutputFadeProgress = 0.0f;
		float fftWetPreWindowFadeL = 0.0f;
		float fftWetPostWindowFadeL = 0.0f;
		float fftWetPreOutputFadeL = 0.0f;
		float fftWetPostOutputFadeL = 0.0f;
		float fftWetPreWindowDeltaL = 0.0f;
		float fftWetPostWindowDeltaL = 0.0f;
		float fftWetPostOutputDeltaL = 0.0f;
		int   rawWindowChanged = 0;
		int   rawAmountChanged = 0;
		int   fftWindowMotionActive = 0;
		int   fftWindowApplyDelayRemaining = 0;
		int   fftWindowCaptureRemaining = 0;
		float fftDuckGain = 1.0f;
		float engineFadeOldOutL = 0.0f;
		float engineFadeOldMix = 0.0f;
		float engineFadeNewMix = 0.0f;
		float fftOutputFadeOldOutL = 0.0f;
		float fftOutputFadeOldMix = 0.0f;
		float fftOutputFadeNewMix = 0.0f;
		int   fftCycleSerial = 0;
		int   fftRuntimeRoute = 0; // 0=none, 1=stft, 2=spectral_hold
		int   fft1FreezeHoldRoute = 0;
		int   signedAnalysisHop = 0;
		int   freezeAnalysisInput = 0;
		float spectralHoldCoeff = 0.0f;
		double analysisReadBefore = 0.0;
		double analysisReadAfter = 0.0;
	};

#if JUCE_DEBUG
	struct FftDebugEntry
	{
		int    blockIndex         = 0;
		int    sampleIndex        = 0;
		int    eventType          = 0; // 0=cycle, 1=trigger_reset, 2=engine_reset, 3=size_reset, 4=unity_exit_reset, 5=window_change, 6=amount_change, 7=window_trace, 8=amount_trace, 9=fft1_reentry_trace
		int    engine             = 0;
		float  amount             = 0.0f;
		float  pitch                = 0.0f;
		float  speed              = 0.0f;
		float  pitchRate          = 1.0f;
		int    windowSamples      = 0;
		int    fftSize            = 0;
		float  targetAnalysisHop  = 0.0f;
		float  filteredAnalysisHop = 0.0f;
		float  analysisHopQuantError = 0.0f;
		int    lastAnalysisHop    = -1;
		int    freezeEntryWarmupCycles = 0;
		int    fftStartupWarmupRemainingCycles = 0;
		int    fftExplicitFreezeActive = 0;
		int    fftExplicitFreezeCapturePending = 0;
		int    fftTargetFreeze = 0;
		int    analysisHop        = 0;
		int    synthesisHop       = 0;
		int    style              = 0;
		int    reverseOn          = 0;
		int    triggerOn          = 0;
		int    wideMode           = 0;
		int    passthrough        = 0;
		int    peakCountL         = 0;
		int    peakCountR         = 0;
		int    lockedBinsL        = 0;
		int    lockedBinsR        = 0;
		double analysisReadBefore = 0.0;
		double analysisReadAfter  = 0.0;
		float  frameRmsL          = 0.0f;
		float  frameRmsR          = 0.0f;
		float  outputRmsL         = 0.0f;
		float  outputRmsR         = 0.0f;
		float  outputStartDeltaL  = 0.0f;
		float  outputStartDeltaR  = 0.0f;
		float  outputNormAtRead   = 0.0f;
		float  previewOutL        = 0.0f;
		float  previewOutR        = 0.0f;
		float  identityRefRmsL    = 0.0f;
		float  identityRefRmsR    = 0.0f;
		float  identityErrRmsL    = 0.0f;
		float  identityErrRmsR    = 0.0f;
		float  identityMaxAbsErrL = 0.0f;
		float  identityMaxAbsErrR = 0.0f;
		float  spectralFluxL      = 0.0f;
		float  spectralFluxR      = 0.0f;
		float  phaseResetMixL     = 0.0f;
		float  phaseResetMixR     = 0.0f;
		float  lockStrengthMeanL  = 0.0f;
		float  lockStrengthMeanR  = 0.0f;
		float  cycleDurationUs    = 0.0f;
		float  cycleRealtimeCpuPct = 0.0f;
		float  forwardFftUs       = 0.0f;
		float  binAnalysisUs      = 0.0f;
		float  pitchMapUs         = 0.0f;
		float  phaseLockUs        = 0.0f;
		float  ifftOlaUs          = 0.0f;
		double analysisLagSamples = 0.0;
		int    cyclesSinceReset   = 0;
		int    alignOn            = 0;
		int    pdcOn              = 0;
		int    reportedLatency    = 0;
		int    dryDelayLen        = 0;
		int    fftOutputPadLen    = 0;
		float  smoothedWindow     = 0.0f;
		float  targetWindow       = 0.0f;
		int    windowTransitionActive = 0;
		float  windowTransitionProgress = 0.0f;
		int    fftOutputFadeActive = 0;
		float  fftOutputFadeProgress = 0.0f;
		float  fftWetPreWindowFadeL = 0.0f;
		float  fftWetPostWindowFadeL = 0.0f;
		float  fftWetPreOutputFadeL = 0.0f;
		float  fftWetPostOutputFadeL = 0.0f;
		float  fftWetPreWindowDeltaL = 0.0f;
		float  fftWetPostWindowDeltaL = 0.0f;
		float  fftWetPostOutputDeltaL = 0.0f;
		int    rawWindowChanged = 0;
		int    rawAmountChanged = 0;
		int    fftWindowMotionActive = 0;
		int    fftWindowApplyDelayRemaining = 0;
		int    fftWindowCaptureRemaining = 0;
		float  fftDuckGain = 1.0f;
		float  engineFadeOldOutL = 0.0f;
		float  engineFadeOldMix = 0.0f;
		float  engineFadeNewMix = 0.0f;
		float  fftOutputFadeOldOutL = 0.0f;
		float  fftOutputFadeOldMix = 0.0f;
		float  fftOutputFadeNewMix = 0.0f;
	};

	class FftDebugTrace : private DebugCsvTraceSupport
	{
	public:
		static constexpr int kRingSize = 16384;
		using DebugCsvTraceSupport::setAutoDumpPath;

		void record (const FftDebugEntry& entry) noexcept
		{
			const int idx = nextRingIndex (writeIndex, kRingSize);
			ring[idx] = entry;
		}

		void enableDesktopAutoDump (const juce::String& filename = "stretr_fft_dump.csv")
		{
			DebugCsvTraceSupport::enableDesktopAutoDump (filename);
		}

		bool dumpToFile (const juce::String& filePath) const
		{
			juce::File f (filePath);
			if (f.existsAsFile())
				f.deleteFile();

			if (auto stream = f.createOutputStream())
			{
				stream->writeText (
					"block_index,sample_index,event,engine,amount,pitch,speed,pitch_rate,window_samples,fft_size,"
					"target_analysis_hop,filtered_analysis_hop,analysis_hop_quant_error,last_analysis_hop,freeze_entry_warmup_cycles,fft_startup_warmup_remaining,fft_explicit_freeze_active,fft_explicit_freeze_capture_pending,fft_target_freeze,analysis_hop,synthesis_hop,style,reverse_on,trigger_on,wide_mode,passthrough,peak_count_l,"
					"peak_count_r,locked_bins_l,locked_bins_r,analysis_read_before,analysis_read_after,frame_rms_l,"
					"frame_rms_r,output_rms_l,output_rms_r,output_start_delta_l,output_start_delta_r,"
					"output_norm_at_read,preview_out_l,preview_out_r,identity_ref_rms_l,identity_ref_rms_r,"
					"identity_err_rms_l,identity_err_rms_r,identity_max_abs_err_l,identity_max_abs_err_r,"
					"spectral_flux_l,spectral_flux_r,phase_reset_mix_l,phase_reset_mix_r,lock_strength_mean_l,lock_strength_mean_r,"
					"cycle_duration_us,cycle_realtime_cpu_pct,forward_fft_us,bin_analysis_us,pitch_map_us,phase_lock_us,ifft_ola_us,"
					"analysis_lag_samples,cycles_since_reset,"
					"align_on,pdc_on,reported_latency,dry_delay_len,fft_output_pad_len,"
					"smoothed_window,target_window,window_transition_active,window_transition_progress,"
					"fft_output_fade_active,fft_output_fade_progress,"
					"fft_wet_pre_window_fade_l,fft_wet_post_window_fade_l,fft_wet_pre_output_fade_l,fft_wet_post_output_fade_l,"
					"fft_wet_pre_window_delta_l,fft_wet_post_window_delta_l,fft_wet_post_output_delta_l,"
					"raw_window_changed,raw_amount_changed,fft_window_motion_active,fft_window_apply_delay_remaining,fft_window_capture_remaining,"
					"fft_duck_gain,"
					"engine_fade_old_out_l,engine_fade_old_mix,engine_fade_new_mix,"
					"fft_output_fade_old_out_l,fft_output_fade_old_mix,fft_output_fade_new_mix\n",
					false, false, nullptr);

				const int writeCount = writeIndex.load (std::memory_order_relaxed);
				const int total = ringTotal (writeCount, kRingSize);
				const int startIdx = ringStart (writeCount, kRingSize);
				for (int i = 0; i < total; ++i)
				{
					const auto& e = ring[(startIdx + i) & (kRingSize - 1)];
					const juce::String eventName = (e.eventType == 1) ? "trigger_reset"
						: (e.eventType == 2) ? "engine_reset"
						: (e.eventType == 3) ? "size_reset"
						: (e.eventType == 4) ? "unity_exit_reset"
						: (e.eventType == 5) ? "window_change"
						: (e.eventType == 6) ? "amount_change"
						: (e.eventType == 7) ? "window_trace"
						: (e.eventType == 8) ? "amount_trace"
						: (e.eventType == 9) ? "fft1_reentry_trace"
						: "cycle";
					juce::String line;
					line << e.blockIndex << ","
					     << e.sampleIndex << ","
					     << eventName << ","
					     << e.engine << ","
					     << juce::String (e.amount, 4) << ","
					     << juce::String (e.pitch, 4) << ","
					     << juce::String (e.speed, 6) << ","
					     << juce::String (e.pitchRate, 6) << ","
					     << e.windowSamples << ","
					     << e.fftSize << ","
					     << juce::String (e.targetAnalysisHop, 6) << ","
					     << juce::String (e.filteredAnalysisHop, 6) << ","
					     << juce::String (e.analysisHopQuantError, 6) << ","
					     << e.lastAnalysisHop << ","
					     << e.freezeEntryWarmupCycles << ","
					     << e.fftStartupWarmupRemainingCycles << ","
					     << e.fftExplicitFreezeActive << ","
					     << e.fftExplicitFreezeCapturePending << ","
					     << e.fftTargetFreeze << ","
					     << e.analysisHop << ","
					     << e.synthesisHop << ","
					     << e.style << ","
					     << e.reverseOn << ","
					     << e.triggerOn << ","
					     << e.wideMode << ","
					     << e.passthrough << ","
					     << e.peakCountL << ","
					     << e.peakCountR << ","
					     << e.lockedBinsL << ","
					     << e.lockedBinsR << ","
					     << juce::String (e.analysisReadBefore, 6) << ","
					     << juce::String (e.analysisReadAfter, 6) << ","
					     << juce::String (e.frameRmsL, 6) << ","
					     << juce::String (e.frameRmsR, 6) << ","
					     << juce::String (e.outputRmsL, 6) << ","
					     << juce::String (e.outputRmsR, 6) << ","
					     << juce::String (e.outputStartDeltaL, 6) << ","
					     << juce::String (e.outputStartDeltaR, 6) << ","
					     << juce::String (e.outputNormAtRead, 6) << ","
					     << juce::String (e.previewOutL, 6) << ","
					     << juce::String (e.previewOutR, 6) << ","
					     << juce::String (e.identityRefRmsL, 6) << ","
					     << juce::String (e.identityRefRmsR, 6) << ","
					     << juce::String (e.identityErrRmsL, 6) << ","
					     << juce::String (e.identityErrRmsR, 6) << ","
					     << juce::String (e.identityMaxAbsErrL, 6) << ","
					     << juce::String (e.identityMaxAbsErrR, 6) << ","
					     << juce::String (e.spectralFluxL, 6) << ","
					     << juce::String (e.spectralFluxR, 6) << ","
					     << juce::String (e.phaseResetMixL, 6) << ","
					     << juce::String (e.phaseResetMixR, 6) << ","
					     << juce::String (e.lockStrengthMeanL, 6) << ","
					     << juce::String (e.lockStrengthMeanR, 6) << ","
					     << juce::String (e.cycleDurationUs, 6) << ","
					     << juce::String (e.cycleRealtimeCpuPct, 6) << ","
					     << juce::String (e.forwardFftUs, 6) << ","
					     << juce::String (e.binAnalysisUs, 6) << ","
					     << juce::String (e.pitchMapUs, 6) << ","
					     << juce::String (e.phaseLockUs, 6) << ","
					     << juce::String (e.ifftOlaUs, 6) << ","
					     << juce::String (e.analysisLagSamples, 6) << ","
					     << e.cyclesSinceReset << ","
					     << e.alignOn << ","
					     << e.pdcOn << ","
					     << e.reportedLatency << ","
					     << e.dryDelayLen << ","
					     << e.fftOutputPadLen << ","
					     << juce::String (e.smoothedWindow, 6) << ","
					     << juce::String (e.targetWindow, 6) << ","
					     << e.windowTransitionActive << ","
					     << juce::String (e.windowTransitionProgress, 6) << ","
					     << e.fftOutputFadeActive << ","
					     << juce::String (e.fftOutputFadeProgress, 6) << ","
					     << juce::String (e.fftWetPreWindowFadeL, 6) << ","
					     << juce::String (e.fftWetPostWindowFadeL, 6) << ","
					     << juce::String (e.fftWetPreOutputFadeL, 6) << ","
					     << juce::String (e.fftWetPostOutputFadeL, 6) << ","
					     << juce::String (e.fftWetPreWindowDeltaL, 6) << ","
					     << juce::String (e.fftWetPostWindowDeltaL, 6) << ","
					     << juce::String (e.fftWetPostOutputDeltaL, 6) << ","
					     << e.rawWindowChanged << ","
					     << e.rawAmountChanged << ","
					     << e.fftWindowMotionActive << ","
					     << e.fftWindowApplyDelayRemaining << ","
					     << e.fftWindowCaptureRemaining << ","
					     << juce::String (e.fftDuckGain, 6) << ","
					     << juce::String (e.engineFadeOldOutL, 6) << ","
					     << juce::String (e.engineFadeOldMix, 6) << ","
					     << juce::String (e.engineFadeNewMix, 6) << ","
					     << juce::String (e.fftOutputFadeOldOutL, 6) << ","
					     << juce::String (e.fftOutputFadeOldMix, 6) << ","
					     << juce::String (e.fftOutputFadeNewMix, 6) << "\n";
					stream->writeText (line, false, false, nullptr);
				}

				stream->flush();
				return true;
			}
			return false;
		}

		~FftDebugTrace()
		{
			if (shouldAutoDump (writeIndex.load (std::memory_order_relaxed)))
				dumpToFile (getAutoDumpPath());
		}

	private:
		std::unique_ptr<FftDebugEntry[]> ring = std::make_unique<FftDebugEntry[]>(kRingSize);
		std::atomic<int> writeIndex { 0 };
	};

	FftDebugTrace fftDebugTrace_;
#endif
	FftDebugContext fftDebugContext_;

#if STRETR_ENABLE_FFT1_CLICK_DUMP
	struct Fft1AmountFreezeDumpEntry
	{
		int   blockIndex = 0;
		int   engine = 0;
		int   triggerOn = 0;
		int   alignOn = 0;
		int   pdcOn = 0;
		int   triggerEdge = 0;
		int   fftWindowMotionActive = 0;
		int   fftAmountMotionActive = 0;
		int   fftSizeChanged = 0;
		int   fftOutputFadePos = 0;
		int   fftOutputFadeTotal = 0;
		int   fftDuckHoldStart = 0;
		int   fftDuckHoldEnd = 0;
		int   fftDuckBridgeRemaining = 0;
		int   fftDuckBridgeTotal = 0;
		float fftDuckGainStart = 1.0f;
		float fftDuckGainEnd = 1.0f;
		int   style = 0;
		int   reverseOn = 0;
		int   wideMode = 0;
		int   dualMode = 0;
		float amount = 0.0f;
		float pitch = 0.0f;
		float speed = 0.0f;
		float smoothedSpeed = 0.0f;
		float pitchRate = 1.0f;
		float jitterTarget = 0.0f;
		float jitterSmoothed = 0.0f;
		float jitterAmountScale = 0.0f;
		float effectivePitchRateL = 1.0f;
		float effectivePitchRateR = 1.0f;
		int   windowSamples = 0;
		int   fftSize = 0;
		int   rawWindowParam = 0;
		int   storedWindow = 0;
		int   effectiveWindow = 0;
		float targetWindow = 0.0f;
		float smoothedWindow = 0.0f;
		int   capturedWindow = 0;
		int   pendingWindow = 0;
		int   fftWindowCaptureRemaining = 0;
		int   fftWindowApplyDelayRemaining = 0;
		int   fft2GeometryWindow = 0;
		float fft2GeometryLog2Window = 0.0f;
		int   desiredFftSize = 0;
		int   requestedFftSize = 0;
		int   previousFftSize = 0;
		int   activeFftSize = 0;
		int   fft2AmountZeroHoldBypassActive = 0;
		int   fft2TargetFullHold = 0;
		int   fft2SmoothedFullHold = 0;
		int   reportedLatency = 0;
		int   dryDelayLen = 0;
		int   fftTargetFreeze = 0;
		int   fftExplicitFreezeActive = 0;
		int   fftExplicitFreezeCapturePending = 0;
		int   lastAnalysisHop = -1;
		int   freezeEntryWarmupCycles = 0;
		int   fftTransitionRemaining = 0;
		int   fftTransitionTotal = 0;
		int   fftFreezeTransitionRemaining = 0;
		int   fftFreezeTransitionTotal = 0;
		int   windowTransitionRemaining = 0;
		int   windowTransitionTotal = 0;
		int   fftUnityBypassActive = 0;
		int   fftTransitionToUnity = 0;
		int   fft1AmountUnityBypassActive = 0;
		float fft2HoldCoeff = 0.0f;
		float fft2TargetHoldCoeff = 0.0f;
		int   fftCycleCount = 0;
		int   fftRuntimeRoute = 0;
		int   fft1FreezeHoldRoute = 0;
		int   signedAnalysisHop = 0;
		int   freezeAnalysisInput = 0;
		float spectralHoldCoeff = 0.0f;
		double analysisReadBefore = 0.0;
		double analysisReadAfter = 0.0;
		double analysisReadDelta = 0.0;
		float engineWetRmsL = 0.0f;
		float engineWetRmsR = 0.0f;
		float engineWetPeakL = 0.0f;
		float engineWetPeakR = 0.0f;
		float finalWetRmsL = 0.0f;
		float finalWetRmsR = 0.0f;
		float finalWetPeakL = 0.0f;
		float finalWetPeakR = 0.0f;
		float outRmsL = 0.0f;
		float outRmsR = 0.0f;
		float outPeakL = 0.0f;
		float outPeakR = 0.0f;
		float postDuckOutRmsL = 0.0f;
		float postDuckOutRmsR = 0.0f;
		float postDuckOutPeakL = 0.0f;
		float postDuckOutPeakR = 0.0f;
		int   modeIn = 0;
		int   modeOut = 0;
		int   sumBus = 0;
		int   mixMode = 0;
		int   filterPre = 0;
		int   tiltPre = 0;
		int   wetFilterHpOn = 0;
		int   wetFilterLpOn = 0;
		int   chaosFilterOn = 0;
		int   chaosDelayOn = 0;
		float chaosAmtF = 0.0f;
		float chaosAmtD = 0.0f;
		float tiltDb = 0.0f;
		float filterHpFreq = 0.0f;
		float filterLpFreq = 0.0f;
		int   maxFftWetDeltaSample = -1;
		float maxFftWetAbsDeltaL = 0.0f;
		float maxFftWetAbsDeltaR = 0.0f;
		float maxFftWetPrevL = 0.0f;
		float maxFftWetPrevR = 0.0f;
		float maxFftWetCurrL = 0.0f;
		float maxFftWetCurrR = 0.0f;
		float maxFftWetNorm = 0.0f;
		int   maxFftWetOutputReadPos = -1;
		int   maxFftWetSynthCounter = -1;
		int   maxEngineWetDeltaSample = -1;
		float maxEngineWetAbsDeltaL = 0.0f;
		float maxEngineWetAbsDeltaR = 0.0f;
		float maxEngineWetPrevL = 0.0f;
		float maxEngineWetPrevR = 0.0f;
		float maxEngineWetCurrL = 0.0f;
		float maxEngineWetCurrR = 0.0f;
		int   maxFinalWetDeltaSample = -1;
		float maxFinalWetAbsDeltaL = 0.0f;
		float maxFinalWetAbsDeltaR = 0.0f;
		float maxFinalWetPrevL = 0.0f;
		float maxFinalWetPrevR = 0.0f;
		float maxFinalWetCurrL = 0.0f;
		float maxFinalWetCurrR = 0.0f;
		int   maxOutDeltaSample = -1;
		float maxOutAbsDeltaL = 0.0f;
		float maxOutAbsDeltaR = 0.0f;
		float maxOutPrevL = 0.0f;
		float maxOutPrevR = 0.0f;
		float maxOutCurrL = 0.0f;
		float maxOutCurrR = 0.0f;
		int   maxPostDuckOutDeltaSample = -1;
		float maxPostDuckOutAbsDeltaL = 0.0f;
		float maxPostDuckOutAbsDeltaR = 0.0f;
		float maxPostDuckOutPrevL = 0.0f;
		float maxPostDuckOutPrevR = 0.0f;
		float maxPostDuckOutCurrL = 0.0f;
		float maxPostDuckOutCurrR = 0.0f;
		StretrDumpStageDelta maxPreStyleWet;
		StretrDumpStageDelta maxPostStyleWet;
		StretrDumpStageDelta maxPostFilterWet;
		StretrDumpStageDelta maxPostChaosWet;
		StretrDumpStageDelta maxPreDcWet;
		StretrDumpStageDelta maxPostDcWet;
		float maxPostDcPrevDcInL = 0.0f;
		float maxPostDcPrevDcInR = 0.0f;
		float maxPostDcPrevDcOutL = 0.0f;
		float maxPostDcPrevDcOutR = 0.0f;
		float maxPostDcInputL = 0.0f;
		float maxPostDcInputR = 0.0f;
	};

	class Fft1AmountFreezeDumpTrace : private DebugCsvTraceSupport
	{
	public:
		static constexpr int kRingSize = 8192;
		using DebugCsvTraceSupport::setAutoDumpPath;

		void record (const Fft1AmountFreezeDumpEntry& entry) noexcept
		{
			const int idx = nextRingIndex (writeIndex, kRingSize);
			ring[idx] = entry;
		}

		void enableDesktopAutoDump (const juce::String& filename = "stretr_fft1_amount_freeze_dump.csv")
		{
			DebugCsvTraceSupport::enableDesktopAutoDump (filename);
		}

		bool dumpToFile (const juce::String& filePath) const
		{
			juce::File f (filePath);
			if (f.existsAsFile())
				f.deleteFile();

			if (auto stream = f.createOutputStream())
			{
				stream->writeText (
					"block_index,engine,trigger_on,align_on,pdc_on,trigger_edge,fft_window_motion_active,fft_amount_motion_active,"
					"fft_size_changed,fft_output_fade_pos,fft_output_fade_total,fft_duck_hold_start,"
					"fft_duck_hold_end,fft_duck_bridge_remaining,fft_duck_bridge_total,fft_duck_gain_start,fft_duck_gain_end,"
					"amount,pitch,speed,pitch_rate,jitter_target,jitter_smoothed,jitter_amount_scale,"
					"effective_pitch_rate_l,effective_pitch_rate_r,window_samples,fft_size,"
					"raw_window_param,stored_window,effective_window,target_window,smoothed_window,"
					"captured_window,pending_window,fft_window_capture_remaining,fft_window_apply_delay_remaining,"
					"fft2_geometry_window,fft2_geometry_log2_window,desired_fft_size,requested_fft_size,"
					"previous_fft_size,active_fft_size,fft2_amount_zero_hold_bypass_active,"
					"fft2_target_full_hold,fft2_smoothed_full_hold,"
					"reported_latency,dry_delay_len,fft_target_freeze,fft_explicit_freeze_active,"
					"fft_explicit_freeze_capture_pending,last_analysis_hop,freeze_entry_warmup_cycles,"
					"fft_transition_remaining,fft_transition_total,fft_freeze_transition_remaining,"
					"fft_freeze_transition_total,style,reverse_on,wide_mode,dual_mode,smoothed_speed,"
					"window_transition_remaining,window_transition_total,fft_unity_bypass_active,fft_transition_to_unity,"
					"fft1_amount_unity_bypass_active,fft2_hold_coeff,fft2_target_hold_coeff,"
					"fft_cycle_count,fft_runtime_route,fft1_freeze_hold_route,signed_analysis_hop,"
					"freeze_analysis_input,spectral_hold_coeff,analysis_read_before,analysis_read_after,analysis_read_delta,"
					"engine_wet_rms_l,engine_wet_rms_r,engine_wet_peak_l,engine_wet_peak_r,"
					"final_wet_rms_l,final_wet_rms_r,final_wet_peak_l,final_wet_peak_r,out_rms_l,out_rms_r,out_peak_l,out_peak_r,"
					"post_duck_out_rms_l,post_duck_out_rms_r,post_duck_out_peak_l,post_duck_out_peak_r,"
					"mode_in,mode_out,sum_bus,mix_mode,filter_pre,tilt_pre,wet_filter_hp_on,wet_filter_lp_on,"
					"chaos_filter_on,chaos_delay_on,chaos_amt_f,chaos_amt_d,tilt_db,filter_hp_freq,filter_lp_freq,"
					"max_fft_wet_delta_sample,max_fft_wet_abs_delta_l,max_fft_wet_abs_delta_r,max_fft_wet_prev_l,max_fft_wet_prev_r,"
					"max_fft_wet_curr_l,max_fft_wet_curr_r,max_fft_wet_norm,max_fft_wet_output_read_pos,max_fft_wet_synth_counter,"
					"max_engine_wet_delta_sample,max_engine_wet_abs_delta_l,max_engine_wet_abs_delta_r,max_engine_wet_prev_l,max_engine_wet_prev_r,"
					"max_engine_wet_curr_l,max_engine_wet_curr_r,max_final_wet_delta_sample,max_final_wet_abs_delta_l,max_final_wet_abs_delta_r,"
					"max_final_wet_prev_l,max_final_wet_prev_r,max_final_wet_curr_l,max_final_wet_curr_r,"
					"max_out_delta_sample,max_out_abs_delta_l,max_out_abs_delta_r,max_out_prev_l,max_out_prev_r,max_out_curr_l,max_out_curr_r,"
					"max_post_duck_out_delta_sample,max_post_duck_out_abs_delta_l,max_post_duck_out_abs_delta_r,"
					"max_post_duck_out_prev_l,max_post_duck_out_prev_r,max_post_duck_out_curr_l,max_post_duck_out_curr_r,"
					"max_pre_style_wet_delta_sample,max_pre_style_wet_abs_delta_l,max_pre_style_wet_abs_delta_r,"
					"max_pre_style_wet_prev_l,max_pre_style_wet_prev_r,max_pre_style_wet_curr_l,max_pre_style_wet_curr_r,"
					"max_post_style_wet_delta_sample,max_post_style_wet_abs_delta_l,max_post_style_wet_abs_delta_r,"
					"max_post_style_wet_prev_l,max_post_style_wet_prev_r,max_post_style_wet_curr_l,max_post_style_wet_curr_r,"
					"max_post_filter_wet_delta_sample,max_post_filter_wet_abs_delta_l,max_post_filter_wet_abs_delta_r,"
					"max_post_filter_wet_prev_l,max_post_filter_wet_prev_r,max_post_filter_wet_curr_l,max_post_filter_wet_curr_r,"
					"max_post_chaos_wet_delta_sample,max_post_chaos_wet_abs_delta_l,max_post_chaos_wet_abs_delta_r,"
					"max_post_chaos_wet_prev_l,max_post_chaos_wet_prev_r,max_post_chaos_wet_curr_l,max_post_chaos_wet_curr_r,"
					"max_pre_dc_wet_delta_sample,max_pre_dc_wet_abs_delta_l,max_pre_dc_wet_abs_delta_r,"
					"max_pre_dc_wet_prev_l,max_pre_dc_wet_prev_r,max_pre_dc_wet_curr_l,max_pre_dc_wet_curr_r,"
					"max_post_dc_wet_delta_sample,max_post_dc_wet_abs_delta_l,max_post_dc_wet_abs_delta_r,"
					"max_post_dc_wet_prev_l,max_post_dc_wet_prev_r,max_post_dc_wet_curr_l,max_post_dc_wet_curr_r,"
					"max_post_dc_prev_dc_in_l,max_post_dc_prev_dc_in_r,max_post_dc_prev_dc_out_l,max_post_dc_prev_dc_out_r,"
					"max_post_dc_input_l,max_post_dc_input_r\n",
					false, false, nullptr);

				const int writeCount = writeIndex.load (std::memory_order_relaxed);
				const int total = ringTotal (writeCount, kRingSize);
				const int startIdx = ringStart (writeCount, kRingSize);
				for (int i = 0; i < total; ++i)
				{
					const auto& e = ring[(startIdx + i) & (kRingSize - 1)];
					juce::String line;
					auto appendStageDelta = [] (juce::String& target, const StretrDumpStageDelta& d)
					{
						target << d.sample << ","
						       << d.absDeltaL << ","
						       << d.absDeltaR << ","
						       << d.prevL << ","
						       << d.prevR << ","
						       << d.currL << ","
						       << d.currR << ",";
					};
					line << e.blockIndex << ","
					     << e.engine << ","
					     << e.triggerOn << ","
					     << e.alignOn << ","
					     << e.pdcOn << ","
					     << e.triggerEdge << ","
					     << e.fftWindowMotionActive << ","
					     << e.fftAmountMotionActive << ","
					     << e.fftSizeChanged << ","
					     << e.fftOutputFadePos << ","
					     << e.fftOutputFadeTotal << ","
					     << e.fftDuckHoldStart << ","
					     << e.fftDuckHoldEnd << ","
					     << e.fftDuckBridgeRemaining << ","
					     << e.fftDuckBridgeTotal << ","
					     << e.fftDuckGainStart << ","
					     << e.fftDuckGainEnd << ","
					     << e.amount << ","
					     << e.pitch << ","
					     << e.speed << ","
					     << e.pitchRate << ","
					     << e.jitterTarget << ","
					     << e.jitterSmoothed << ","
					     << e.jitterAmountScale << ","
					     << e.effectivePitchRateL << ","
					     << e.effectivePitchRateR << ","
					     << e.windowSamples << ","
					     << e.fftSize << ","
					     << e.rawWindowParam << ","
					     << e.storedWindow << ","
					     << e.effectiveWindow << ","
					     << e.targetWindow << ","
					     << e.smoothedWindow << ","
					     << e.capturedWindow << ","
					     << e.pendingWindow << ","
					     << e.fftWindowCaptureRemaining << ","
					     << e.fftWindowApplyDelayRemaining << ","
					     << e.fft2GeometryWindow << ","
					     << e.fft2GeometryLog2Window << ","
					     << e.desiredFftSize << ","
					     << e.requestedFftSize << ","
					     << e.previousFftSize << ","
					     << e.activeFftSize << ","
					     << e.fft2AmountZeroHoldBypassActive << ","
					     << e.fft2TargetFullHold << ","
					     << e.fft2SmoothedFullHold << ","
					     << e.reportedLatency << ","
					     << e.dryDelayLen << ","
					     << e.fftTargetFreeze << ","
					     << e.fftExplicitFreezeActive << ","
					     << e.fftExplicitFreezeCapturePending << ","
					     << e.lastAnalysisHop << ","
					     << e.freezeEntryWarmupCycles << ","
					     << e.fftTransitionRemaining << ","
					     << e.fftTransitionTotal << ","
					     << e.fftFreezeTransitionRemaining << ","
					     << e.fftFreezeTransitionTotal << ","
					     << e.style << ","
					     << e.reverseOn << ","
					     << e.wideMode << ","
					     << e.dualMode << ","
					     << e.smoothedSpeed << ","
					     << e.windowTransitionRemaining << ","
					     << e.windowTransitionTotal << ","
					     << e.fftUnityBypassActive << ","
					     << e.fftTransitionToUnity << ","
					     << e.fft1AmountUnityBypassActive << ","
					     << e.fft2HoldCoeff << ","
					     << e.fft2TargetHoldCoeff << ","
					     << e.fftCycleCount << ","
					     << e.fftRuntimeRoute << ","
					     << e.fft1FreezeHoldRoute << ","
					     << e.signedAnalysisHop << ","
					     << e.freezeAnalysisInput << ","
					     << e.spectralHoldCoeff << ","
					     << e.analysisReadBefore << ","
					     << e.analysisReadAfter << ","
					     << e.analysisReadDelta << ","
					     << e.engineWetRmsL << ","
					     << e.engineWetRmsR << ","
					     << e.engineWetPeakL << ","
					     << e.engineWetPeakR << ","
					     << e.finalWetRmsL << ","
					     << e.finalWetRmsR << ","
					     << e.finalWetPeakL << ","
					     << e.finalWetPeakR << ","
					     << e.outRmsL << ","
					     << e.outRmsR << ","
					     << e.outPeakL << ","
					     << e.outPeakR << ","
					     << e.postDuckOutRmsL << ","
					     << e.postDuckOutRmsR << ","
					     << e.postDuckOutPeakL << ","
					     << e.postDuckOutPeakR << ","
					     << e.modeIn << ","
					     << e.modeOut << ","
					     << e.sumBus << ","
					     << e.mixMode << ","
					     << e.filterPre << ","
					     << e.tiltPre << ","
					     << e.wetFilterHpOn << ","
					     << e.wetFilterLpOn << ","
					     << e.chaosFilterOn << ","
					     << e.chaosDelayOn << ","
					     << e.chaosAmtF << ","
					     << e.chaosAmtD << ","
					     << e.tiltDb << ","
					     << e.filterHpFreq << ","
					     << e.filterLpFreq << ","
					     << e.maxFftWetDeltaSample << ","
					     << e.maxFftWetAbsDeltaL << ","
					     << e.maxFftWetAbsDeltaR << ","
					     << e.maxFftWetPrevL << ","
					     << e.maxFftWetPrevR << ","
					     << e.maxFftWetCurrL << ","
					     << e.maxFftWetCurrR << ","
					     << e.maxFftWetNorm << ","
					     << e.maxFftWetOutputReadPos << ","
					     << e.maxFftWetSynthCounter << ","
					     << e.maxEngineWetDeltaSample << ","
					     << e.maxEngineWetAbsDeltaL << ","
					     << e.maxEngineWetAbsDeltaR << ","
					     << e.maxEngineWetPrevL << ","
					     << e.maxEngineWetPrevR << ","
					     << e.maxEngineWetCurrL << ","
					     << e.maxEngineWetCurrR << ","
					     << e.maxFinalWetDeltaSample << ","
					     << e.maxFinalWetAbsDeltaL << ","
					     << e.maxFinalWetAbsDeltaR << ","
					     << e.maxFinalWetPrevL << ","
					     << e.maxFinalWetPrevR << ","
					     << e.maxFinalWetCurrL << ","
					     << e.maxFinalWetCurrR << ","
					     << e.maxOutDeltaSample << ","
					     << e.maxOutAbsDeltaL << ","
					     << e.maxOutAbsDeltaR << ","
					     << e.maxOutPrevL << ","
					     << e.maxOutPrevR << ","
					     << e.maxOutCurrL << ","
					     << e.maxOutCurrR << ","
					     << e.maxPostDuckOutDeltaSample << ","
					     << e.maxPostDuckOutAbsDeltaL << ","
					     << e.maxPostDuckOutAbsDeltaR << ","
					     << e.maxPostDuckOutPrevL << ","
					     << e.maxPostDuckOutPrevR << ","
					     << e.maxPostDuckOutCurrL << ","
					     << e.maxPostDuckOutCurrR << ",";
					appendStageDelta (line, e.maxPreStyleWet);
					appendStageDelta (line, e.maxPostStyleWet);
					appendStageDelta (line, e.maxPostFilterWet);
					appendStageDelta (line, e.maxPostChaosWet);
					appendStageDelta (line, e.maxPreDcWet);
					appendStageDelta (line, e.maxPostDcWet);
					line << e.maxPostDcPrevDcInL << ","
					     << e.maxPostDcPrevDcInR << ","
					     << e.maxPostDcPrevDcOutL << ","
					     << e.maxPostDcPrevDcOutR << ","
					     << e.maxPostDcInputL << ","
					     << e.maxPostDcInputR << "\n";
					stream->writeText (line, false, false, nullptr);
				}
				stream->flush();
				return true;
			}
			return false;
		}

		~Fft1AmountFreezeDumpTrace()
		{
			if (shouldAutoDump (writeIndex.load (std::memory_order_relaxed)))
				dumpToFile (getAutoDumpPath());
		}

	private:
		Fft1AmountFreezeDumpEntry ring[kRingSize] {};
		std::atomic<int> writeIndex { 0 };
	};

	Fft1AmountFreezeDumpTrace fft1AmountFreezeDumpTrace_;
	int fft1AmountFreezeDumpBlockCounter_ = 0;
#endif

    // Granular engine state
	struct Grain
	{
		double readPos   = 0.0;   // start position in input buffer
		double playPos   = 0.0;   // current position within grain (fractional)
		double rate      = 1.0;   // playback rate (for pitch playback)
		int    length    = 0;     // grain length in samples
		int    elapsed   = 0;     // samples played so far
		int    dualCh    = -1;    // -1=both, 0=L-only, 1=R-only (DUAL mode)
		bool   active    = false;
		bool   reverse   = false;
	};
	static constexpr int kMaxGrains = 64;
	Grain grains_[kMaxGrains];
	int   grainNextSlot_       = 0;
	int   grainSpawnCountdown_ = 0;
	double grainReadPos_       = 0.0;   // current absolute read position for spawning grains
	float grainPrevOutL_       = 0.0f;  // DC-block state
	float grainPrevOutR_       = 0.0f;
	bool  grainUnityBypassActive_ = false;
	int   grainTransitionRemaining_ = 0;
	int   grainTransitionTotal_ = 0;
	bool  grainTransitionToUnity_ = false;
	bool  grainFreezeHoldActive_ = false;

	// FFT / phase vocoder engine state
	static constexpr int kMaxFftSize    = 8192;
	static constexpr int kMaxFftBins    = kMaxFftSize / 2 + 1;
	static constexpr int kStftOutBufLen = kMaxFftSize * 2;
	static constexpr int kDryDelayBufLen = kStftOutBufLen;

	struct StftState
	{
		float prevPhase[2][kMaxFftBins]      = {};
		float synthPhase[2][kMaxFftBins]     = {};
		float prevMag[2][kMaxFftBins]        = {};
		float lastMag[2][kMaxFftBins]        = {};
		float lastFreq[2][kMaxFftBins]       = {};
		float heldMag[2][kMaxFftBins]        = {};
		float heldFreq[2][kMaxFftBins]       = {};
		float outputAccum[2][kStftOutBufLen] = {};
		float outputNormAccum[kStftOutBufLen] = {};
		double identityErrSqAccum[2] = {};
		double identityRefSqAccum[2] = {};
		float  identityMaxAbsErr[2] = {};
		int    identitySampleCount = 0;
		int   outputReadPos    = 0;
		int   synthCounter     = 0;
		double analysisReadPos = 0.0;
		double filteredAnalysisHop = -1.0;
		double analysisHopQuantError = 0.0;
		int    lastAnalysisHop = -1;
		int    freezeEntryWarmupCycles = 0;
		float  analysisHopSlewNorm = 0.0f;
		float  analysisHopStepNorm = 0.0f;
		bool  hasFrame         = false;
		int   activeFftSize    = 0;
		int   cyclesSinceReset = 0;
	};

	struct Fft1FreezeSnapshot
	{
		float prevPhase[2][kMaxFftBins] = {};
		float synthPhase[2][kMaxFftBins] = {};
		float prevMag[2][kMaxFftBins] = {};
		float lastMag[2][kMaxFftBins] = {};
		float lastFreq[2][kMaxFftBins] = {};
		float heldMag[2][kMaxFftBins] = {};
		float heldFreq[2][kMaxFftBins] = {};
		int   fftSize = 0;
		int   style = 0;
		bool  reverseOn = false;
		bool  explicitFreeze = false;
		bool  hasFrame = false;
		bool  valid = false;
	};
	StftState stft_;
	StftState stftResizeScratch_;
	Fft1FreezeSnapshot fft1FreezeSnapshot_;

	std::unique_ptr<juce::dsp::FFT> fft_;
	int   currentFftOrder_ = -1;
	float fftWindow_[kMaxFftSize]     = {};
	float fftWork_[kMaxFftSize * 2]   = {};
	float fftOutputPadBuf_[2][kMaxFftSize] = {};
	static constexpr int kFftWetHistoryLen = 8192;
	float fftWetHistory_[2][kFftWetHistoryLen] = {};
	int   fftWetHistoryWritePos_ = 0;
	float fftPrevWetPreWindowL_ = 0.0f;
	float fftPrevWetPostWindowL_ = 0.0f;
	float fftPrevWetPostOutputL_ = 0.0f;
	float fftLastStableWetL_ = 0.0f;
	float fftLastStableWetR_ = 0.0f;
	bool  fftLastStableWetValid_ = false;
	float fftParamDuckGain_ = 1.0f;
	int   fftParamDuckHoldRemaining_ = 0;
	float fftDuckBridgeStartL_ = 0.0f;
	float fftDuckBridgeStartR_ = 0.0f;
	float fftLastPostDuckOutL_ = 0.0f;
	float fftLastPostDuckOutR_ = 0.0f;
#if STRETR_ENABLE_FFT1_CLICK_DUMP
	float fftDumpPrevFftWetL_ = 0.0f;
	float fftDumpPrevFftWetR_ = 0.0f;
	float fftDumpPrevPreStyleWetL_ = 0.0f;
	float fftDumpPrevPreStyleWetR_ = 0.0f;
	float fftDumpPrevPostStyleWetL_ = 0.0f;
	float fftDumpPrevPostStyleWetR_ = 0.0f;
	float fftDumpPrevPostFilterWetL_ = 0.0f;
	float fftDumpPrevPostFilterWetR_ = 0.0f;
	float fftDumpPrevPostChaosWetL_ = 0.0f;
	float fftDumpPrevPostChaosWetR_ = 0.0f;
	float fftDumpPrevPreDcWetL_ = 0.0f;
	float fftDumpPrevPreDcWetR_ = 0.0f;
	float fftDumpPrevPostDcWetL_ = 0.0f;
	float fftDumpPrevPostDcWetR_ = 0.0f;
	float fftDumpPrevEngineWetL_ = 0.0f;
	float fftDumpPrevEngineWetR_ = 0.0f;
	float fftDumpPrevFinalWetL_ = 0.0f;
	float fftDumpPrevFinalWetR_ = 0.0f;
	float fftDumpPrevOutL_ = 0.0f;
	float fftDumpPrevOutR_ = 0.0f;
	float fftDumpPrevPostDuckOutL_ = 0.0f;
	float fftDumpPrevPostDuckOutR_ = 0.0f;
#endif
	int   fftDuckBridgeRemaining_ = 0;
	int   fftDuckBridgeTotal_ = 0;
	int   fftWindowApplyDelayRemaining_ = 0;
	int   fftWindowCaptureRemaining_ = 0;
	int   fftCapturedWindowVal_ = (int) kWindowDefault;
	int   fftPendingWindowVal_ = (int) kWindowDefault;
	int   fftWindowTraceRemaining_ = 0;
	int   fftAmountTraceRemaining_ = 0;
	int   fft1ReentryTraceRemaining_ = 0;
	float fft2HoldCoeffSmoothed_ = 0.0f;
	float fft2AudioHoldCoeffSmoothed_ = 0.0f;
	bool  fft2AmountZeroHoldBypassActive_ = false;
	int   prevFftDuckWindowVal_ = 0;
	float prevFftDuckAmountVal_ = 0.0f;
	int   prevFftDuckEngineVal_ = -1;
	bool  prevFftDuckTriggerOn_ = false;
	int   fft1WindowTransitionRemaining_ = 0;
	int   fft1WindowTransitionTotal_ = 0;
	int   fftOutputPadWritePos_ = 0;
	bool  fftUnityBypassActive_ = false;
	bool  fft1AmountUnityBypassActive_ = false;
	int   fftStartupWarmupRemainingCycles_ = 0;
	int   fftTransitionRemaining_ = 0;
	int   fftTransitionTotal_ = 0;
	int   fftTransitionHoldSamples_ = 0;
	bool  fftTransitionToUnity_ = false;
	int   fftFreezeTransitionRemaining_ = 0;
	int   fftFreezeTransitionTotal_ = 0;
	int   fftFreezeTransitionReadPos_ = 0;
	int   fft2WindowTransitionRemaining_ = 0;
	int   fft2WindowTransitionTotal_ = 0;
	bool  fftExplicitFreezeActive_ = false;
	bool  fftExplicitFreezeCapturePending_ = false;

	static constexpr int kWetOutputHistoryLen = 8192;
	float wetOutputHistory_[2][kWetOutputHistoryLen] = {};
	int   wetOutputHistoryWritePos_ = 0;
	int   engineFadeHoldSamples_ = 0;
	float engineFadeStartL_ = 0.0f;
	float engineFadeStartR_ = 0.0f;
	int   fftOutputFadeReadPos_ = 0;
	int   fftOutputFadeHoldSamples_ = 0;

	void  ensureFft (int fftSize);
	void  resetStftAtPos (double capturePos, int fftSize) noexcept;
	void  resizeStftAtPos (double capturePos, int fftSize) noexcept;
	void  clearStftOutputResidueForResize() noexcept;
	void  resizeFft2StateAtPos (double capturePos, int fftSize, bool freezeTarget) noexcept;
	int   recommendedFftSynthHop (int fftSize) const noexcept;
	int   samplesForMs (double ms) const noexcept;
	int   recommendedFftWindowCrossfadeSamples() const noexcept;
	int   recommendedFft2WindowCrossfadeSamples (int fromFftSize, int toFftSize) const noexcept;
	int   recommendedFftTriggerDuckHoldSamples (int fftSize) const noexcept;
	int   recommendedFftFreezeTransitionSamples (int fftSize) const noexcept;
	int   recommendedEngineCrossfadeSamples() const noexcept;
	WindowFamily getWindowFamilyForEngineInternal (int engineVal) const noexcept;
	int   getCanonicalWindowForFamily (WindowFamily family, int windowValue) const noexcept;
	int   getStoredWindowForFamily (WindowFamily family) const noexcept;
	void  setStoredWindowForFamily (WindowFamily family, int windowValue) noexcept;
	void  initialiseWindowFamilies (int fallbackWindow) noexcept;
	void  restoreWindowFamilyStateFromTree() noexcept;
	void  writeWindowFamilyStateToTree (juce::ValueTree& state) const;
	void  resetFftWindowDuckPrepareState (int capturedWindowVal, float amountVal,
	                                     int engineVal, bool triggerOn) noexcept;
	void  clearFftWindowDuckRuntimeState() noexcept;
	void  resetEngineFadeState() noexcept;
	void  clearEngineFadeState() noexcept;
	void  resetFftOutputFadeState() noexcept;
	void  clearFftOutputFadeState() noexcept;
	void  clearFft1FreezeSnapshot() noexcept;
	void  captureFft1FreezeSnapshot (int styleVal, bool reverseOn) noexcept;
	bool  canRestoreFft1FreezeSnapshot (int fftSize, int styleVal, bool reverseOn,
	                                    bool triggerOn, bool targetFreeze,
	                                    float targetSpeed) const noexcept;
	void  restoreFft1FreezeSnapshot (bool targetFreeze) noexcept;
	int   getWindowTransitionRemainingForEngine (int engineVal) const noexcept;
	int   getWindowTransitionTotalForEngine (int engineVal) const noexcept;
	bool  isWindowTransitionActiveForEngine (int engineVal) const noexcept;
	float getWindowTransitionProgressForEngine (int engineVal) const noexcept;
	void  startWindowTransitionForEngine (int engineVal, int totalSamples) noexcept;
	void  clearWindowTransitionForEngine (int engineVal) noexcept;
	void  decrementWindowTransitionForEngine (int engineVal) noexcept;
	void  performStftCycle (int fftSize, int analysisHop, int synthesisHop,
	                        float pitchRate, bool reverseOn, float pitchRateR = -1.0f,
	                        bool wideMode = false);
	void  performStftCycleSpectralHold (int fftSize, int synthesisHop,
	                                    float holdCoeff, float pitchRate, bool reverseOn,
	                                    bool freezeAnalysisInput = false,
	                                    float pitchRateR = -1.0f, bool wideMode = false);

    // Precomputed 1/sqrt(n) for granular normalization
	float invSqrtLut_[kMaxGrains + 1] = {};

	// Dry delay buffer for ALIGN when FFT latency is active
	float dryDelayBuf_[2][kDryDelayBufLen] = {};
	int   dryDelayWritePos_ = 0;
	int   dryDelayLen_       = 0;

	static constexpr int kHannLutSize = 2048;
	float hannLut_[kHannLutSize + 1] = {};

	inline float hannWindow (float phase) const noexcept
	{
		const float idx = phase * (float) kHannLutSize;
		const int i0 = ((int) idx) & (kHannLutSize - 1);
		const float frac = idx - (float) i0;
		return hannLut_[i0] + frac * (hannLut_[i0 + 1] - hannLut_[i0]);
	}

	inline float readInputBuf (int ch, double pos) const noexcept
	{
		const int i0 = (int) pos & inputBufMask_;
		const int im1 = (i0 - 1) & inputBufMask_;
		const int i1 = (i0 + 1) & inputBufMask_;
		const int i2 = (i0 + 2) & inputBufMask_;
		const float t = (float) (pos - std::floor (pos));
		const float xm1 = inputBuf_[ch][(size_t) im1];
		const float x0  = inputBuf_[ch][(size_t) i0];
		const float x1  = inputBuf_[ch][(size_t) i1];
		const float x2  = inputBuf_[ch][(size_t) i2];
		const float c1 = 0.5f * (x1 - xm1);
		const float c2 = xm1 - 2.5f * x0 + 2.0f * x1 - 0.5f * x2;
		const float c3 = 0.5f * (x2 - xm1) + 1.5f * (x0 - x1);
		return ((c3 * t + c2) * t + c1) * t + x0;
	}

	float smoothedInputGain  = 1.0f;
	float smoothedOutputGain = 1.0f;
	float smoothedMix        = 0.5f;
	float smoothedDryLevel   = kDryLevelDefault;
	float smoothedWetLevel   = kWetLevelDefault;
	float smoothedLimThreshold = 1.0f;
	bool  filterPre_  = false;
	bool  tiltPre_    = false;
	float smoothedWindow_    = (float) kWindowDefault;  // smoothed window size in samples
	std::array<float, 4> smoothedWindowByFamily_ {
		kWindowDefault, kWindowDefault, kWindowDefault, kWindowDefault
	};
	float fft2GeometryLog2Window_ = 0.0f;               // FFT2-only geometry smoother in log2 domain
	float smoothedSpeed_     = 1.0f;   // smoothed stretch speed (0=freeze, 1=normal)
	float smoothedPitchRate_ = 1.0f;   // smoothed pitch ratio
	float smoothedGrainLogMs_ = 4.60517019f; // ln(100 ms), smoothed before sample quantization
	float windowSmoothStep_  = 0.0045f;
	std::atomic<int> windowFamilyValues_[4] {};
	std::atomic<int> activeWindowFamily_ { (int) WindowFamily::stretch };
	std::atomic<int> lastObservedWindowParam_ { (int) kWindowDefault };
	std::atomic<bool> windowFamiliesInitialised_ { false };

	// JIT engine: deterministic drift/S&H sources shared by the per-engine mappings.
	struct JitterEngine
	{
		float driftPhaseA = 0.0f;
		float driftPhaseB = 0.0f;
		float driftRateHzA = 0.0f;
		float driftRateHzB = 0.0f;
		float shCurr = 0.0f;
		float shNext = 0.0f;
		float shPhase = 0.0f;
		juce::Random rng;
	};

	struct JitterRuntimeValues
	{
		float pitchScale = 1.0f;
		float lengthScale = 1.0f;
		double anchorOffsetSamples = 0.0;
	};

	JitterEngine jitterWindow_[2];
	JitterEngine jitterAnchor_[2];
	JitterEngine jitterPitch_[2];
	JitterEngine jitterRapid_[2];
	float jitterWindowOut_[2] = {};
	float jitterAnchorOut_[2] = {};
	float jitterPitchOut_[2] = {};
	float jitterRapidOut_[2] = {};
	float jitterSmoothed_ = 0.0f;       // normalized 0..1
	float jitterSmoothStep_ = 0.001f;
	float stretchJitterPitchScaleSmoothed_[2] = { 1.0f, 1.0f };
	void resetJitterEngines() noexcept;
	float advanceJitterEngine (JitterEngine& engine, float fastRateHz, float fastBlend,
	                           float maxFastRateHz = 32.0f, float maxBlend = 0.35f) noexcept;
	void advanceJitterEngines (float amount) noexcept;
	JitterRuntimeValues makeJitterRuntimeValues (int lane, float referenceSamples,
	                                             float pitchAmountScale, float motionAmountScale,
	                                             bool allowAnchor) const noexcept;
	JitterRuntimeValues makeStretchJitterRuntimeValues (int lane) const noexcept;
	JitterRuntimeValues makeFftJitterRuntimeValues (int lane) const noexcept;

	struct WetFilterChannelState
	{
		WetFilterBiquadState hp[2];
		WetFilterBiquadState lp[2];
		void reset() { hp[0] = hp[1] = lp[0] = lp[1] = {}; }
	};
	WetFilterChannelState wetFilterState_[2];
	WetFilterBiquadCoeffs hpCoeffs_[2];
	WetFilterBiquadCoeffs lpCoeffs_[2];
	WetFilterBiquadCoeffs hpCoeffsR_[2];      // per-section HP coeffs (R, stereo chaos)
	WetFilterBiquadCoeffs lpCoeffsR_[2];      // per-section LP coeffs (R, stereo chaos)
	float smoothedFilterHpFreq_ = kFilterHpFreqDefault;
	float smoothedFilterLpFreq_ = kFilterLpFreqDefault;
	float lastCalcHpFreq_ = -1.0f, lastCalcLpFreq_ = -1.0f;
	int   lastCalcHpSlope_ = -1,   lastCalcLpSlope_ = -1;
	int   filterCoeffCountdown_ = 0;
	static constexpr int kFilterCoeffUpdateInterval = 32;
	void updateFilterCoeffs (bool forceHp, bool forceLp);

	bool  wetFilterHpOn_ = false;
	bool  wetFilterLpOn_ = false;
	float wetFilterTargetHpFreq_ = kFilterHpFreqDefault;
	float wetFilterTargetLpFreq_ = kFilterLpFreqDefault;
	int   wetFilterNumSectionsHp_ = 0;
	int   wetFilterNumSectionsLp_ = 0;
	void  filterWetSample (float& wetL, float& wetR);
	void  tiltWetSample   (float& wetL, float& wetR);

	float tiltDb_ = 0.0f;
	float tiltB0_ = 1.0f, tiltB1_ = 0.0f, tiltA1_ = 0.0f;
	float tiltTargetB0_ = 1.0f, tiltTargetB1_ = 0.0f, tiltTargetA1_ = 0.0f;
	float tiltState_[2] = { 0.0f, 0.0f };
	float lastTiltDb_   = 0.0f;
	float tiltSmoothSc_ = 0.0f;

    // Chaos state (smooth S&H + drift, per-channel D/G, quadrature F)
	bool  chaosFilterEnabled_ = false;
	bool  chaosDelayEnabled_  = false;
	bool  chaosStereo_        = false;   // true when style >= 1 (per-channel D/G)

	// CHS D parameters
	float chaosAmtD_                    = 0.0f;
	float chaosAmtNormD_                = 0.0f;   // cached amtD * 0.01
	float chaosShPeriodD_               = 8820.0f;
	float smoothedChaosShPeriodD_       = 8820.0f;
	float chaosDelayMaxSamples_         = 0.0f;
	float smoothedChaosDelayMaxSamples_ = 0.0f;
	float chaosGainMaxDb_               = 0.0f;
	float smoothedChaosGainMaxDb_       = 0.0f;
	float chaosDelaySmoothedSamples_[2] = {};
	bool  chaosDelaySmoothReady_[2]     = {};
	float chaosDriveAmtSmoothed_        = 0.0f;
	float chaosDriveSpdSmoothed_        = 0.0f;
	bool  chaosDriveParamSmoothReady_   = false;

	// CHS D smooth S&H + Drift: delay (per-channel for stereo styles)
	float chaosDPrev_[2]         = {};
	float chaosDCurr_[2]         = {};
	float chaosDNext_[2]         = {};
	float chaosDPhase_[2]        = {};
	float chaosDDriftPhase_[2]   = {};
	float chaosDDriftFreqHz_[2]  = {};
	float chaosDOut_[2]          = {};
	juce::Random chaosDRng_[2];

	// CHS D smooth S&H + Drift: gain (per-channel, decorrelated)
	float chaosGPrev_[2]         = {};
	float chaosGCurr_[2]         = {};
	float chaosGNext_[2]         = {};
	float chaosGPhase_[2]        = {};
	float chaosGDriftPhase_[2]   = {};
	float chaosGDriftFreqHz_[2]  = {};
	float chaosGOut_[2]          = {};
	juce::Random chaosGRng_[2];

	// CHS F parameters
	float chaosAmtF_                 = 0.0f;
	float chaosShPeriodF_            = 8820.0f;
	float smoothedChaosShPeriodF_    = 8820.0f;
	float chaosFilterMaxOct_         = 0.0f;
	float smoothedChaosFilterMaxOct_ = 0.0f;
	float chaosFilterAmtSmoothed_    = 0.0f;
	float chaosFilterSpdSmoothed_    = 0.0f;
	bool  chaosFilterParamSmoothReady_ = false;

	// CHS F smooth S&H + Drift: filter (mono S&H + quadrature drift)
	float chaosFPrev_            = 0.0f;
	float chaosFCurr_            = 0.0f;
	float chaosFNext_            = 0.0f;
	float chaosFPhase_           = 0.0f;
    float chaosFDriftPhase_      = 0.0f;   // single phase; R = +90 deg offset
	float chaosFDriftFreqHz_     = 0.0f;
	float chaosFOut_[2]          = {};     // [0]=L, [1]=R (quadrature when stereo)
	juce::Random chaosFRng_;

	float chaosParamSmoothCoeff_ = 0.999f;

	// Precomputed sampleRate-dependent smooth coefficients (set in prepareToPlay)
	float cachedChaosParamSmoothCoeff_   = 0.999f;
	float chaosDelaySmoothStep_          = 0.001f;

	static constexpr int kChaosDelayBufLen = 1024;
	float chaosDelayBuf_[2][kChaosDelayBufLen] = {};
	int   chaosDelayWritePos_ = 0;

	static constexpr float kChaosDriftAmp = 0.3f;
	static constexpr float kTwoPi = 6.283185307f;

	float lastPan_      = 0.5f;
	float lastPanLeft_  = 0.70710678f;
	float lastPanRight_ = 0.70710678f;

	// Engine crossfade state
	int   prevEngineVal_   = -1;
	int   engineFadePos_   = 0;
	int   engineFadeTotal_ = 0;
	int   fftOutputFadePos_ = 0;
	int   fftOutputFadeTotal_ = 0;
	int   lastReportedLatency_ = -1;

	// DC blocker state (1-pole HP ~5 Hz)
	float dcBlockR_        = 0.9997f;
	float dcBlockPrevIn_[2]  = {};
	float dcBlockPrevOut_[2] = {};

	std::atomic<int> uiEditorWidth  { 360 };
	std::atomic<int> uiEditorHeight { 540 };
	std::atomic<int> uiUseCustomPalette { 0 };
	std::atomic<int> uiCrtEnabled  { 0 };
	std::atomic<juce::uint32> uiCustomPalette[2] {};

	std::atomic<float>* amountParam      = nullptr;
	std::atomic<float>* pitchParam       = nullptr;
	std::atomic<float>* grainParam       = nullptr;
	std::atomic<float>* engineParam      = nullptr;
	std::atomic<float>* windowParam      = nullptr;
	std::atomic<float>* maxWindowParam   = nullptr;
	std::atomic<float>* jitterParam      = nullptr;
	std::atomic<float>* styleParam       = nullptr;
	std::atomic<float>* inputParam       = nullptr;
	std::atomic<float>* outputParam      = nullptr;
	std::atomic<float>* mixParam         = nullptr;
	std::atomic<float>* modeInParam      = nullptr;
	std::atomic<float>* modeOutParam     = nullptr;
	std::atomic<float>* sumBusParam      = nullptr;
	std::atomic<float>* limThresholdParam = nullptr;
	std::atomic<float>* limModeParam     = nullptr;
	std::atomic<float>* invPolParam      = nullptr;
	std::atomic<float>* invStrParam      = nullptr;
	std::atomic<float>* mixModeParam     = nullptr;
	std::atomic<float>* dryLevelParam    = nullptr;
	std::atomic<float>* wetLevelParam    = nullptr;
	std::atomic<float>* filterPosParam   = nullptr;
	std::atomic<float>* reverseParam     = nullptr;
	std::atomic<float>* triggerParam     = nullptr;
	std::atomic<float>* alignParam       = nullptr;
	std::atomic<float>* pdcParam         = nullptr;

	std::atomic<float>* filterHpFreqParam  = nullptr;
	std::atomic<float>* filterLpFreqParam  = nullptr;
	std::atomic<float>* filterHpSlopeParam = nullptr;
	std::atomic<float>* filterLpSlopeParam = nullptr;
	std::atomic<float>* filterHpOnParam    = nullptr;
	std::atomic<float>* filterLpOnParam    = nullptr;
	std::atomic<float>* tiltParam    = nullptr;
	std::atomic<float>* panParam     = nullptr;
	std::atomic<float>* chaosParam   = nullptr;
	std::atomic<float>* chaosDelayParam   = nullptr;
	std::atomic<float>* chaosAmtParam     = nullptr;
	std::atomic<float>* chaosSpdParam     = nullptr;
	std::atomic<float>* chaosAmtFilterParam = nullptr;
	std::atomic<float>* chaosSpdFilterParam = nullptr;

	std::atomic<float>* uiWidthParam   = nullptr;
	std::atomic<float>* uiHeightParam  = nullptr;
	std::atomic<float>* uiPaletteParam = nullptr;
	std::atomic<float>* uiCrtParam     = nullptr;
	std::atomic<float>* uiColorParams[2] = { nullptr, nullptr };

	// Generic smooth S&H + Drift chaos engine (per-sample advance)
	inline void advanceChaosEngine (
		float& prev, float& curr, float& next, float& phase,
		float& driftPhase, float& driftFreqHz, float& output,
		juce::Random& rng, float period, float amtNorm, float sr) noexcept
	{
		const float safePeriod = juce::jmax (1.0f, period);
		phase += 1.0f / safePeriod;
		if (phase >= 1.0f)
		{
			phase -= std::floor (phase);
			prev = curr;
			curr = next;
			next = rng.nextFloat() * 2.0f - 1.0f;
			const float driftBase = sr / safePeriod * 0.37f;
			driftFreqHz = driftBase * (0.88f + rng.nextFloat() * 0.24f);
		}
		const float t = juce::jlimit (0.0f, 1.0f, phase);
		const float t2 = t * t;
		const float t3 = t2 * t;
		const float u = t3 * (t * (t * 6.0f - 15.0f) + 10.0f);
		const float shValue = curr + (next - curr) * u;

		driftPhase += driftFreqHz / sr;
		if (driftPhase > 1e6f) driftPhase -= 1e6f;
		const float driftValue = std::sin (driftPhase * kTwoPi) * kChaosDriftAmp;

		const float shWeight = juce::jlimit (0.0f, 1.0f, amtNorm * 1.5f - 0.15f);
		output = driftValue + shValue * shWeight;
	}

	inline void advanceChaosD() noexcept
	{
		const float sr = (float) currentSampleRate;
		const float smoothStep = 1.0f - chaosParamSmoothCoeff_;
		const float targetAmt = juce::jlimit (kChaosAmtMin, kChaosAmtMax, chaosAmtD_);
		const float targetSpd = juce::jlimit (kChaosSpdMin, kChaosSpdMax, sr / juce::jmax (1.0f, chaosShPeriodD_));

		if (! chaosDriveParamSmoothReady_)
		{
			chaosDriveParamSmoothReady_ = true;
			if (chaosDriveSpdSmoothed_ <= 0.0f)
				chaosDriveSpdSmoothed_ = targetSpd;
		}

		chaosDriveAmtSmoothed_ += (targetAmt - chaosDriveAmtSmoothed_) * smoothStep;
		const float spdLog = std::log (juce::jmax (kChaosSpdMin, chaosDriveSpdSmoothed_));
		const float targetSpdLog = std::log (targetSpd);
		chaosDriveSpdSmoothed_ = std::exp (spdLog + (targetSpdLog - spdLog) * smoothStep);

		chaosAmtNormD_ = chaosDriveAmtSmoothed_ * 0.01f;
		smoothedChaosDelayMaxSamples_ = chaosAmtNormD_ * 0.005f * sr;
		smoothedChaosGainMaxDb_ = chaosAmtNormD_ * 1.0f;
		smoothedChaosShPeriodD_ = sr / juce::jmax (kChaosSpdMin, chaosDriveSpdSmoothed_);

		const float period = smoothedChaosShPeriodD_;
		const int nCh = chaosStereo_ ? 2 : 1;

		for (int c = 0; c < nCh; ++c)
		{
			advanceChaosEngine (chaosDPrev_[c], chaosDCurr_[c], chaosDNext_[c], chaosDPhase_[c],
				chaosDDriftPhase_[c], chaosDDriftFreqHz_[c], chaosDOut_[c],
				chaosDRng_[c], period, chaosAmtNormD_, sr);

			advanceChaosEngine (chaosGPrev_[c], chaosGCurr_[c], chaosGNext_[c], chaosGPhase_[c],
				chaosGDriftPhase_[c], chaosGDriftFreqHz_[c], chaosGOut_[c],
				chaosGRng_[c], period, chaosAmtNormD_, sr);
		}

		// Delay modulation stays mono-linked to avoid mono-sum phaser/comb artifacts.
		// Gain modulation may stay stereo for width when the style supports it.
		chaosDOut_[1] = chaosDOut_[0];
		if (! chaosStereo_)
			chaosGOut_[1] = chaosGOut_[0];
	}

	inline void advanceChaosF() noexcept
	{
		const float sr       = (float) currentSampleRate;
		const float smoothStep = 1.0f - chaosParamSmoothCoeff_;
		const float targetAmt = juce::jlimit (kChaosAmtMin, kChaosAmtMax, chaosAmtF_);
		const float targetSpd = juce::jlimit (kChaosSpdMin, kChaosSpdMax, sr / juce::jmax (1.0f, chaosShPeriodF_));

		if (! chaosFilterParamSmoothReady_)
		{
			chaosFilterParamSmoothReady_ = true;
			if (chaosFilterSpdSmoothed_ <= 0.0f)
				chaosFilterSpdSmoothed_ = targetSpd;
		}

		chaosFilterAmtSmoothed_ += (targetAmt - chaosFilterAmtSmoothed_) * smoothStep;
		const float spdLog = std::log (juce::jmax (kChaosSpdMin, chaosFilterSpdSmoothed_));
		const float targetSpdLog = std::log (targetSpd);
		chaosFilterSpdSmoothed_ = std::exp (spdLog + (targetSpdLog - spdLog) * smoothStep);

		const float amtNormF = chaosFilterAmtSmoothed_ * 0.01f;
		smoothedChaosFilterMaxOct_ = amtNormF * 2.0f;
		smoothedChaosShPeriodF_ = sr / juce::jmax (kChaosSpdMin, chaosFilterSpdSmoothed_);
		const float period = smoothedChaosShPeriodF_;

		const float safePeriod = juce::jmax (1.0f, period);
		chaosFPhase_ += 1.0f / safePeriod;
		if (chaosFPhase_ >= 1.0f)
		{
			chaosFPhase_ -= std::floor (chaosFPhase_);
			chaosFPrev_ = chaosFCurr_;
			chaosFCurr_ = chaosFNext_;
			chaosFNext_ = chaosFRng_.nextFloat() * 2.0f - 1.0f;
			const float driftBase = sr / safePeriod * 0.37f;
			chaosFDriftFreqHz_ = driftBase * (0.88f + chaosFRng_.nextFloat() * 0.24f);
		}

		const float t = juce::jlimit (0.0f, 1.0f, chaosFPhase_);
		const float t2 = t * t;
		const float t3 = t2 * t;
		const float u = t3 * (t * (t * 6.0f - 15.0f) + 10.0f);
		const float shValue = chaosFCurr_ + (chaosFNext_ - chaosFCurr_) * u;

		chaosFDriftPhase_ += chaosFDriftFreqHz_ / sr;
		if (chaosFDriftPhase_ > 1e6f) chaosFDriftPhase_ -= 1e6f;
		const float driftL = std::sin (chaosFDriftPhase_ * kTwoPi) * kChaosDriftAmp;

		const float shWeight = juce::jlimit (0.0f, 1.0f, amtNormF * 1.5f - 0.15f);
		chaosFOut_[0] = driftL + shValue * shWeight;

		if (chaosStereo_)
		{
			const float driftR = std::sin (chaosFDriftPhase_ * kTwoPi + kTwoPi * 0.25f) * kChaosDriftAmp;
			chaosFOut_[1] = driftR + shValue * shWeight;
		}
		else
		{
			chaosFOut_[1] = chaosFOut_[0];
		}
	}

	inline void applyChaosDelay (float& wetL, float& wetR) noexcept
	{
		const int wp = chaosDelayWritePos_;
		chaosDelayBuf_[0][wp] = wetL;
		chaosDelayBuf_[1][wp] = wetR;

		const float centerDelay = smoothedChaosDelayMaxSamples_;
		const int mask = kChaosDelayBufLen - 1;

		for (int ch = 0; ch < 2; ++ch)
		{
			const float targetDelaySamp = juce::jlimit (0.0f, (float)(kChaosDelayBufLen - 2),
				centerDelay + chaosDOut_[ch] * smoothedChaosDelayMaxSamples_);
			float& delaySamp = chaosDelaySmoothedSamples_[ch];
			if (! chaosDelaySmoothReady_[ch])
			{
				delaySamp = targetDelaySamp;
				chaosDelaySmoothReady_[ch] = true;
			}
			else
			{
				delaySamp += (targetDelaySamp - delaySamp) * chaosDelaySmoothStep_;
			}

			const float readPos = (float) wp - delaySamp;
			const int iPos = (int) std::floor (readPos);
			const float frac = readPos - (float) iPos;

			const float p0 = chaosDelayBuf_[ch][(iPos - 1) & mask];
			const float p1 = chaosDelayBuf_[ch][(iPos    ) & mask];
			const float p2 = chaosDelayBuf_[ch][(iPos + 1) & mask];
			const float p3 = chaosDelayBuf_[ch][(iPos + 2) & mask];
			const float c0 = p1;
			const float c1 = p2 - (1.0f / 3.0f) * p0 - 0.5f * p1 - (1.0f / 6.0f) * p3;
			const float c2 = 0.5f * (p0 + p2) - p1;
			const float c3 = (1.0f / 6.0f) * (p3 - p0) + 0.5f * (p1 - p2);
			float& wet = (ch == 0) ? wetL : wetR;
			wet = ((c3 * frac + c2) * frac + c1) * frac + c0;
		}

		chaosDelayWritePos_ = (wp + 1) & mask;

		// Per-channel gain modulation
		for (int ch = 0; ch < 2; ++ch)
		{
			const float gainDb  = chaosGOut_[ch] * smoothedChaosGainMaxDb_;
			const float ex = gainDb * 0.16609640474f;
			const float exln2 = ex * 0.6931472f;
			const float gainLin = 1.0f + exln2 * (1.0f + exln2 * 0.5f);
			float& wet = (ch == 0) ? wetL : wetR;
			wet *= gainLin;
		}
	}

    // Limiter state
	static constexpr float kLimFloor = 1.0e-12f;
	float limEnv1_[2] = { kLimFloor, kLimFloor };
	float limEnv2_[2] = { kLimFloor, kLimFloor };
	float limAtt1_  = 0.0f;
	float limRel1_  = 0.0f;
	float limRel2_  = 0.0f;

	inline void applyLimiter (float* leftData, float* rightData, int numSamples,
	                         float thresholdGain) noexcept
	{
		applyLimiter (leftData, rightData, numSamples, thresholdGain, thresholdGain);
	}

	inline void applyLimiter (float* leftData, float* rightData, int numSamples,
	                         float thresholdGainStart, float thresholdGainEnd) noexcept
	{
		const float thresholdStep = (numSamples > 1)
			? (thresholdGainEnd - thresholdGainStart) / (float) (numSamples - 1)
			: 0.0f;
		float thresholdGain = thresholdGainStart;

		for (int i = 0; i < numSamples; ++i)
		{
			const float peakL = std::abs (leftData[i]);
			const float peakR = std::abs (rightData[i]);

            // Stage 1 - leveler (2 ms attack, 10 ms release)
			for (int ch = 0; ch < 2; ++ch)
			{
				const float p = (ch == 0) ? peakL : peakR;
				if (p > limEnv1_[ch])
					limEnv1_[ch] = limAtt1_ * limEnv1_[ch] + (1.0f - limAtt1_) * p;
				else
					limEnv1_[ch] = limRel1_ * limEnv1_[ch] + (1.0f - limRel1_) * p;
				if (limEnv1_[ch] < kLimFloor) limEnv1_[ch] = kLimFloor;
			}

            // Stage 2 - brickwall (instant attack, 100 ms release)
			for (int ch = 0; ch < 2; ++ch)
			{
				const float p = (ch == 0) ? peakL : peakR;
				if (p > limEnv2_[ch])
					limEnv2_[ch] = p;
				else
					limEnv2_[ch] = limRel2_ * limEnv2_[ch] + (1.0f - limRel2_) * p;
				if (limEnv2_[ch] < kLimFloor) limEnv2_[ch] = kLimFloor;
			}

			// Stereo-linked gain reduction
			float gr = 1.0f;
			const float maxEnv1 = juce::jmax (limEnv1_[0], limEnv1_[1]);
			const float maxEnv2 = juce::jmax (limEnv2_[0], limEnv2_[1]);
			if (maxEnv1 > thresholdGain)
				gr = juce::jmin (gr, thresholdGain / maxEnv1);
			if (maxEnv2 > thresholdGain)
				gr = juce::jmin (gr, thresholdGain / maxEnv2);

			leftData[i]  *= gr;
			rightData[i] *= gr;
			thresholdGain += thresholdStep;
		}
	}

	inline void applyLimiterSample (float& sampleL, float& sampleR, float thresholdGain) noexcept
	{
		const float peakL = std::abs (sampleL);
		const float peakR = std::abs (sampleR);

		for (int ch = 0; ch < 2; ++ch)
		{
			const float p = (ch == 0) ? peakL : peakR;
			if (p > limEnv1_[ch])
				limEnv1_[ch] = limAtt1_ * limEnv1_[ch] + (1.0f - limAtt1_) * p;
			else
				limEnv1_[ch] = limRel1_ * limEnv1_[ch] + (1.0f - limRel1_) * p;
			if (limEnv1_[ch] < kLimFloor) limEnv1_[ch] = kLimFloor;
		}

		for (int ch = 0; ch < 2; ++ch)
		{
			const float p = (ch == 0) ? peakL : peakR;
			if (p > limEnv2_[ch])
				limEnv2_[ch] = p;
			else
				limEnv2_[ch] = limRel2_ * limEnv2_[ch] + (1.0f - limRel2_) * p;
			if (limEnv2_[ch] < kLimFloor) limEnv2_[ch] = kLimFloor;
		}

		float gr = 1.0f;
		const float maxEnv1 = juce::jmax (limEnv1_[0], limEnv1_[1]);
		const float maxEnv2 = juce::jmax (limEnv2_[0], limEnv2_[1]);
		if (maxEnv1 > thresholdGain)
			gr = juce::jmin (gr, thresholdGain / maxEnv1);
		if (maxEnv2 > thresholdGain)
			gr = juce::jmin (gr, thresholdGain / maxEnv2);

		sampleL *= gr;
		sampleR *= gr;
	}

	JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (STRETRAudioProcessor)
};
