#include "StreUiDefinition.h"

#include <utility>

namespace TR::StreUIV2
{
namespace V2 = SimpleUIV2;

namespace
{
std::string tooltipFor(const std::string& parameter, const std::string& label)
{
    const std::pair<const char*, const char*> descriptions[] {
        { "amount", "Time-stretch amount; extreme 4x stretch and pitch combinations may reach the real-time capture limit" },
        { "pitch", "Pitch shift in semitones; extreme 4x stretch and pitch combinations may reach the real-time capture limit" },
        { "window", "Processing window for the selected engine" }, { "mix", "Dry and processed signal balance" },
        { "engine", "Select stretch, grain or FFT processing" }, { "grain", "Grain duration" },
        { "jitter", "Engine-aware window, anchor, pitch and rapid variation" }, { "style", "Stereo processing topology" },
        { "trigger", "Capture and sustain the current material" }, { "reverse", "Reverse processed material" },
        { "align", "Align FFT processing to the declared latency" }, { "pdc", "Report FFT latency to the host" },
        { "sidechain", "Enable external sidechain control" }, { "input", "Level entering the stretch engine" },
        { "output", "Final plugin output level" }, { "pan", "Wet-signal stereo position" },
        { "mix_mode", "Choose insert or send signal flow" }, { "lim_mode", "Limiter position in the signal path" },
        { "lim_quality", "Limiter processing quality" },
        { "filter_pos", "Filter and tilt position around processing" }
    };
    for (const auto& [id, text] : descriptions)
        if (parameter == id) return text;
    if (label == "FILTER OPTIONS") return "Open wet-path filter and tilt controls";
    if (label == "ROUTING") return "Open input, output and polarity routing";
    return label;
}

void addParameter(V2::SimplePluginDefinition& d, std::string id, V2::ParameterAccess access,
                  std::string target = {}, V2::StateDomain domain = V2::StateDomain::musicalParameter,
                  std::string backendJustification = {})
{
    const auto stableId = id;
    d.parameters.push_back({ std::move(id), domain, access, std::move(target),
                             std::move(backendJustification) });
    if (domain == V2::StateDomain::musicalParameter) d.preset.parameterWhitelist.push_back(stableId);
    if (domain == V2::StateDomain::musicalState) d.preset.musicalStateWhitelist.push_back(stableId);
}

V2::SimpleControlSpec control(std::string id, std::string parameter, std::string label,
                              V2::ControlRole role = V2::ControlRole::knob)
{
    V2::SimpleControlSpec result;
    result.controlId = std::move(id);
    result.parameterId = std::move(parameter);
    result.label = std::move(label);
    result.role = role;
    result.tooltip = tooltipFor(result.parameterId, result.label);
    return result;
}

V2::SimpleControlSpec formatted(V2::SimpleControlSpec result, int decimals, double scale,
                                std::string suffix, double offset = 0.0)
{
    result.valueFormat = { true, decimals, scale, std::move(suffix), offset };
    return result;
}

V2::SimpleControlSpec frequency(V2::SimpleControlSpec result)
{
    result.valueFormat = { true, 1, 1.0, "", 0.0, V2::ValueStyle::frequency };
    return result;
}

V2::SimpleGroupSpec hiddenGroup(std::string id, std::vector<V2::SimpleControlSpec> controls,
                                unsigned depth = 0)
{
    return { std::move(id), {}, std::move(controls), {}, depth, V2::GroupLabelVisibility::hidden };
}

V2::SimpleGroupSpec group(std::string id, std::string label, std::vector<V2::SimpleControlSpec> controls)
{
    return { std::move(id), std::move(label), std::move(controls), {}, 0,
             V2::GroupLabelVisibility::automatic };
}

V2::SimplePluginDefinition buildDefinition()
{
    V2::SimplePluginDefinition d;
    d.product = { "com.tr.audio.stre", "STRE-TR", "1.4.0", "https://github.com/lmaser/STRE-TR/issues" };
    d.capabilities = { true, true, true, true };

    for (const auto* id : { "amount", "pitch", "window", "mix" })
        addParameter(d, id, V2::ParameterAccess::direct);
    for (int macro = 1; macro <= 8; ++macro)
    {
        const auto id = "mod_macro_" + std::to_string(macro);
        addParameter(d, id, V2::ParameterAccess::backendOnly, {},
                     V2::StateDomain::musicalParameter,
                     "Automatable Macro value exposed by the shared MACROS workspace.");
        d.preset.missingParameterDefaults.push_back({ id, 0.0 });
    }
    addParameter(d, "modulation_v1", V2::ParameterAccess::backendOnly, {},
                 V2::StateDomain::musicalState,
                 "Macro names, routes, source settings and transfer curves.");
    d.preset.missingMusicalStateDefaults.push_back({ "modulation_v1", 0.0 });
    auto mixMacro = formatted(control("macro-mix", "mix", "MIX", V2::ControlRole::macro),
                              1, 100.0, "%");
    mixMacro.parameterAlternatives = { "wet_level" };
    auto windowMacro = formatted(control("macro-window", "window", "WINDOW", V2::ControlRole::macro),
                                 0, 1.0, "");
    windowMacro.enabledWhen = { { "engine", V2::Comparison::notEqual, 1.0 } };
    windowMacro.unavailableReason =
        "Grain uses the Grain duration control; Window does not apply to this engine";
    d.macros = {
        formatted(control("macro-amount", "amount", "AMOUNT", V2::ControlRole::macro), 1, 1.0, "%"),
        formatted(control("macro-pitch", "pitch", "PITCH", V2::ControlRole::macro), 1, 48.0, " st", -24.0),
        std::move(windowMacro),
        std::move(mixMacro)
    };

    for (const auto* id : { "engine", "grain", "style" })
        addParameter(d, id, V2::ParameterAccess::direct);
    addParameter(d, "jitter", V2::ParameterAccess::backendOnly, {},
                 V2::StateDomain::musicalParameter,
                 "Legacy Jitter parameter retained for presets and host automation; new editing uses a MACROS motion recipe.");
    auto engine = control("engine-control", "engine", "ENGINE", V2::ControlRole::choice);
    engine.choiceLabels = { "STRETCH", "GRAIN", "FFT1", "FFT2" };
    engine.choicePresentation = V2::ChoicePresentation::rail;
    auto grain = formatted(control("grain-control", "grain", "GRAIN"), 1, 1.0, " ms");
    grain.visibleWhen.push_back({ "engine", V2::Comparison::equal, 1.0 });
    auto style = control("style-control", "style", "STYLE", V2::ControlRole::choice);
    style.choiceLabels = { "MONO", "STEREO", "WIDE", "DUAL" };
    style.choicePresentation = V2::ChoicePresentation::rail;
    addParameter(d, "trigger", V2::ParameterAccess::direct);
    addParameter(d, "triggerDelayMs", V2::ParameterAccess::prompt, "trigger-options", V2::StateDomain::musicalState);
    addParameter(d, "reverse", V2::ParameterAccess::direct);
    addParameter(d, "chaos", V2::ParameterAccess::direct);
    addParameter(d, "chaos_d", V2::ParameterAccess::direct);
    for (const auto* id : { "chaos_amt_filter", "chaos_spd_filter" })
        addParameter(d, id, V2::ParameterAccess::inspector, "chaos-filter-inspector");
    for (const auto* id : { "chaos_amt", "chaos_spd" })
        addParameter(d, id, V2::ParameterAccess::inspector, "chaos-delay-inspector");
    auto trigger = control("trigger-control", "trigger", "TRIGGER", V2::ControlRole::toggle);
    trigger.promptId = "trigger-options";
    auto chaosFilter = control("chaos-filter-control", "chaos", "CHAOS FILTER", V2::ControlRole::toggle);
    chaosFilter.inspectorId = "chaos-filter-inspector";
    auto chaosDelay = control("chaos-delay-control", "chaos_d", "CHAOS DELAY", V2::ControlRole::toggle);
    chaosDelay.inspectorId = "chaos-delay-inspector";
    auto reverse = control("reverse-control", "reverse", "REVERSE", V2::ControlRole::toggle);
    auto sidechain = control("sidechain-control", "sidechain", "SIDECHAIN", V2::ControlRole::toggle);
    sidechain.capability = V2::CapabilityTag::sidechain;
    sidechain.promptId = "sidechain-options";

    addParameter(d, "align", V2::ParameterAccess::prompt, "latency-options");
    addParameter(d, "pdc", V2::ParameterAccess::prompt, "latency-options");
    addParameter(d, "max_window", V2::ParameterAccess::prompt, "latency-options");
    addParameter(d, "sidechain", V2::ParameterAccess::direct);
    for (const auto* id : { "sidechain_gain", "sidechain_smooth", "sidechain_pol", "sidechain_hp", "sidechain_lp",
                            "sidechain_hp_on", "sidechain_lp_on", "sidechain_hp_slope", "sidechain_lp_slope" })
        addParameter(d, id, V2::ParameterAccess::backendOnly, {},
                     V2::StateDomain::musicalParameter,
                     "Legacy sidechain sub-parameter retained for preset and automation compatibility; active control migrated to MACROS workspace.");
    auto alignmentMode = V2::makeCompensatedAlignmentControl();
    alignmentMode.tooltip += ". FFT engine compensation is active in FFT1 and FFT2; the mode remains preconfigurable";
    auto align = control("align-control", "align", "DRY/WET ALIGN", V2::ControlRole::toggle);
    align.enabledWhen.push_back({ "engine", V2::Comparison::greaterOrEqual, 2.0 });
    align.unavailableReason = "Available for FFT1 and FFT2 engines";
    auto pdc = control("pdc-control", "pdc", "HOST COMP", V2::ControlRole::toggle);
    pdc.enabledWhen.push_back({ "engine", V2::Comparison::greaterOrEqual, 2.0 });
    pdc.unavailableReason = "Available for FFT1 and FFT2 engines";
    V2::SimplePageSpec main { V2::TaskId::core, "MAIN", {
        hiddenGroup("main-controls", { engine, grain, style, reverse,
                                        chaosFilter, chaosDelay })
    } };
    main.signatureActions = { trigger };

    for (const auto* id : { "input", "output", "pan", "mix_mode", "dry_level", "wet_level", "lim_mode", "lim_quality", "lim_threshold" })
        addParameter(d, id, V2::ParameterAccess::direct);
    for (const auto* id : { "filter_hp_on", "filter_hp_freq", "filter_hp_slope", "filter_lp_on", "filter_lp_freq",
                            "filter_lp_slope", "tilt", "filter_pos" })
        addParameter(d, id, V2::ParameterAccess::prompt, "filter-options");
    for (const auto* id : { "mode_in", "mode_out", "sum_bus", "inv_pol", "inv_str" })
        addParameter(d, id, V2::ParameterAccess::prompt, "routing-options");

    auto filterAction = control("filter-options-action", {}, "FILTER OPTIONS", V2::ControlRole::action);
    filterAction.domain = V2::StateDomain::uiInstance;
    filterAction.promptId = "filter-options";
    auto routingAction = V2::makeCanonicalRoutingAction();
    auto latencyAction = control("latency-options-action", {}, "ALIGNMENT", V2::ControlRole::action);
    latencyAction.domain = V2::StateDomain::uiInstance;
    latencyAction.promptId = "latency-options";
    auto input = formatted(control("input-control", "input", "INPUT", V2::ControlRole::fader), 1, 1.0, " dB");
    input.meterSource = V2::MeterSource::input;
    auto output = formatted(control("output-control", "output", "OUTPUT", V2::ControlRole::fader), 1, 1.0, " dB");
    output.meterSource = V2::MeterSource::output;
    V2::SimplePageSpec io { V2::TaskId::io, "I/O", V2::makeCommonIoGroups(input, output) };
    io.fixedActions = { filterAction, routingAction, latencyAction };
    d.pages = { std::move(main), std::move(io) };
    d.auxiliaryControls = { sidechain };

    for (const auto* id : { "stretchWindow", "grainWindow", "fft1Window", "fft2Window" })
        addParameter(d, id, V2::ParameterAccess::backendOnly, {}, V2::StateDomain::musicalState,
                     "Processor-owned engine window state consumed during STRE engine changes");

    auto triggerDelay = formatted(control("trigger-delay-control", "triggerDelayMs", "DELAY"), 0, 1.0, " ms");
    triggerDelay.domain = V2::StateDomain::musicalState;
    triggerDelay.manualRange = { true, 0.0, 100.0, 1.0, 0.0 };
    auto maxWindow = formatted(control("max-window-control", "max_window", "MAX WINDOW"), 0, 1.0, "");
    auto sidechainGain = formatted(control("sidechain-gain-control", "sidechain_gain", "GAIN"), 1, 1.0, " dB");
    auto sidechainSmooth = formatted(control("sidechain-smooth-control", "sidechain_smooth", "SMOOTH"), 0, 100.0, "%");
    auto sidechainPolarity = formatted(control("sidechain-polarity-control", "sidechain_pol", "POLARITY"), 2, 1.0, "");
    auto sidechainHp = frequency(control("sidechain-hp-control", "sidechain_hp", "HP"));
    auto sidechainLp = frequency(control("sidechain-lp-control", "sidechain_lp", "LP"));
    sidechainHp.inlineToggle = V2::SimpleControlSpec::InlineToggleSpec {
        "sidechain_hp_on", "ON", "Enable the sidechain high-pass band",
        V2::StateDomain::musicalParameter, true };
    sidechainHp.inlineChoice = V2::SimpleControlSpec::InlineChoiceSpec {
        "sidechain_hp_slope", { "6", "12", "24" }, "High-pass slope in dB per octave",
        V2::StateDomain::musicalParameter };
    sidechainLp.inlineToggle = V2::SimpleControlSpec::InlineToggleSpec {
        "sidechain_lp_on", "ON", "Enable the sidechain low-pass band",
        V2::StateDomain::musicalParameter, true };
    sidechainLp.inlineChoice = V2::SimpleControlSpec::InlineChoiceSpec {
        "sidechain_lp_slope", { "6", "12", "24" }, "Low-pass slope in dB per octave",
        V2::StateDomain::musicalParameter };

    auto filterControls = V2::makeCanonicalFilterStageControls("prompt-filter", {
        "filter_hp_on", "filter_hp_freq", "filter_hp_slope", "filter_lp_on",
        "filter_lp_freq", "filter_lp_slope", "tilt" });
    auto filterPosition = control("prompt-filter-position", "filter_pos", "F / T POSITION", V2::ControlRole::choice);
    filterPosition.choiceLabels = { "POST/POST", "PRE/PRE", "PRE/POST", "POST/PRE" };
    filterControls.push_back(filterPosition);

    d.prompts = {
        { "trigger-options", "Trigger", { "triggerDelayMs" }, { triggerDelay } },
        { "latency-options", "Compensated Alignment", { "align", "pdc", "max_window" },
          { alignmentMode, align, pdc, maxWindow } },
        { "filter-options", "Filter / Wet Path", { "filter_hp_on", "filter_hp_freq", "filter_hp_slope", "filter_lp_on",
            "filter_lp_freq", "filter_lp_slope", "tilt", "filter_pos" }, std::move(filterControls) },
        { "sidechain-options", "Sidechain", {
            "sidechain_gain", "sidechain_smooth", "sidechain_pol", "sidechain_hp", "sidechain_lp",
            "sidechain_hp_on", "sidechain_lp_on", "sidechain_hp_slope", "sidechain_lp_slope" },
          { sidechainGain, sidechainSmooth, sidechainPolarity, sidechainHp, sidechainLp } },
        V2::makeCanonicalRoutingPrompt()
    };

    d.inspectors = {
        { "chaos-filter-inspector", "Chaos filter", { hiddenGroup("chaos-filter-detail", {
            formatted(control("chaos-filter-amount", "chaos_amt_filter", "AMOUNT"), 1, 1.0, "%"),
            frequency(control("chaos-filter-speed", "chaos_spd_filter", "SPEED")) }, 1) } },
        { "chaos-delay-inspector", "Chaos delay", { hiddenGroup("chaos-delay-detail", {
            formatted(control("chaos-delay-amount", "chaos_amt", "AMOUNT"), 1, 1.0, "%"),
            frequency(control("chaos-delay-speed", "chaos_spd", "SPEED")) }, 1) } }
    };

    d.signatureModel = V2::SignatureModel::temporalMaterial;
    d.signature.clear();
    d.hiddenCompatibilityInputs = { "sidechain" };
    return d;
}
}

const V2::SimplePluginDefinition& definition()
{
    static const auto value = buildDefinition();
    return value;
}

const std::vector<std::string>& retiredUiParameterIds()
{
    static const std::vector<std::string> ids { "ui_width", "ui_height", "ui_palette", "ui_fx_tail", "ui_io_fx",
                                                 "ui_color0", "ui_color1", "ui_color2", "ui_color3" };
    return ids;
}
}
