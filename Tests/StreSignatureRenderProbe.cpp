#include "../Source/PluginProcessor.h"

#include <iostream>
#include <stdexcept>

#if JUCE_WINDOWS
#include <windows.h>
#endif

namespace
{
void require(bool value, const std::string& message)
{
    if (!value) throw std::runtime_error(message);
}

juce::Component* findById(juce::Component& parent, const juce::String& id)
{
    if (parent.getComponentID() == id) return &parent;
    for (auto* child : parent.getChildren())
        if (auto* result = findById(*child, id)) return result;
    return nullptr;
}

void dispatchPendingMessages()
{
#if JUCE_WINDOWS
    const auto deadline = juce::Time::getMillisecondCounter() + 90;
    MSG message {};
    while (juce::Time::getMillisecondCounter() < deadline)
    {
        bool dispatched = false;
        while (PeekMessage(&message, nullptr, 0, 0, PM_REMOVE) != FALSE)
        {
            TranslateMessage(&message);
            DispatchMessage(&message);
            dispatched = true;
        }
        if (!dispatched) juce::Thread::sleep(1);
    }
#endif
}

void setPlain(STRETRAudioProcessor& processor, const char* id, float value)
{
    auto* parameter = processor.apvts.getParameter(id);
    require(parameter != nullptr, std::string("Missing parameter: ") + id);
    parameter->setValueNotifyingHost(parameter->convertTo0to1(value));
}

void processTone(STRETRAudioProcessor& processor, int blocks, float frequency,
                 double& phase, bool silent)
{
    constexpr int blockSize = 512;
    constexpr double sampleRate = 48000.0;
    juce::AudioBuffer<float> buffer(2, blockSize);
    juce::MidiBuffer midi;
    const auto increment = juce::MathConstants<double>::twoPi * frequency / sampleRate;
    for (int block = 0; block < blocks; ++block)
    {
        for (int sample = 0; sample < blockSize; ++sample)
        {
            const auto value = silent ? 0.0f : static_cast<float>(0.34 * std::sin(phase));
            phase += increment;
            if (phase >= juce::MathConstants<double>::twoPi)
                phase -= juce::MathConstants<double>::twoPi;
            buffer.setSample(0, sample, value);
            buffer.setSample(1, sample, value);
        }
        processor.processBlock(buffer, midi);
    }
}

void writePng(const juce::Image& image, const juce::File& output)
{
    output.deleteFile();
    std::unique_ptr<juce::FileOutputStream> stream(output.createOutputStream());
    require(stream != nullptr && juce::PNGImageFormat().writeImageToStream(image, *stream),
            "Could not write STRE signature snapshot");
}

double imageDifference(const juce::Image& left, const juce::Image& right)
{
    require(left.getBounds() == right.getBounds(), "Signature image geometry changed");
    double difference = 0.0;
    for (int y = 0; y < left.getHeight(); ++y)
        for (int x = 0; x < left.getWidth(); ++x)
        {
            const auto a = left.getPixelAt(x, y);
            const auto b = right.getPixelAt(x, y);
            difference += std::abs(a.getFloatRed() - b.getFloatRed())
                        + std::abs(a.getFloatGreen() - b.getFloatGreen())
                        + std::abs(a.getFloatBlue() - b.getFloatBlue());
        }
    return difference / static_cast<double>(left.getWidth() * left.getHeight());
}
}

int main(int argc, char** argv)
{
    try
    {
        juce::ScopedJuceInitialiser_GUI juceInitialiser;
        const auto output = argc > 1
            ? juce::File::getCurrentWorkingDirectory().getChildFile(argv[1])
            : juce::File::getCurrentWorkingDirectory().getChildFile("analysis_out/StreSignatureFinal");
        output.createDirectory();

        static STRETRAudioProcessor processor;
        processor.prepareToPlay(48000.0, 512);
        std::unique_ptr<juce::AudioProcessorEditor> editor(processor.createEditor());
        editor->addToDesktop(juce::ComponentPeer::windowIsTemporary);
        editor->setVisible(true);
        dispatchPendingMessages();
        auto* signature = findById(*editor, "visual-signature");
        require(signature != nullptr && signature->getWidth() > 300 && signature->getHeight() >= 60,
                "STRE signature band is missing or incorrectly sized");
        auto* windowMacro = findById(*editor, "macro-window");
        require(windowMacro != nullptr && windowMacro->isEnabled(),
                "STRE WINDOW macro is missing or unavailable in Stretch");
        processor.setStoredWindowForEngine(0, 1024);
        processor.setStoredWindowForEngine(1, 256);
        setPlain(processor, STRETRAudioProcessor::kParamEngine, 1.0f);
        dispatchPendingMessages();
        require(windowMacro->isVisible() && !windowMacro->isEnabled(),
                "Grain must keep WINDOW visible but unavailable (visible="
                    + std::to_string(windowMacro->isVisible()) + ", enabled="
                    + std::to_string(windowMacro->isEnabled()) + ", engine="
                    + std::to_string(processor.apvts.getRawParameterValue(
                                         STRETRAudioProcessor::kParamEngine)->load()) + ")");
        const auto grainWindow = processor.apvts.getRawParameterValue(
            STRETRAudioProcessor::kParamWindow)->load();
        dispatchPendingMessages();
        require(std::abs(processor.apvts.getRawParameterValue(
                            STRETRAudioProcessor::kParamWindow)->load() - grainWindow) < 0.01f,
                "Disabled Grain WINDOW mutates without user interaction");
        setPlain(processor, STRETRAudioProcessor::kParamEngine, 0.0f);
        dispatchPendingMessages();
        require(windowMacro->isEnabled()
                    && std::abs(processor.apvts.getRawParameterValue(
                                    STRETRAudioProcessor::kParamWindow)->load() - 1024.0f) < 0.01f,
                "Stretch did not restore its stored WINDOW value");

        setPlain(processor, STRETRAudioProcessor::kParamAmount, 68.0f);
        setPlain(processor, STRETRAudioProcessor::kParamPitch, 0.5f);
        setPlain(processor, STRETRAudioProcessor::kParamWindow, 1024.0f);
        setPlain(processor, STRETRAudioProcessor::kParamGrain, 90.0f);

        double phase = 0.0;
        setPlain(processor, STRETRAudioProcessor::kParamTrigger, 0.0f);
        processTone(processor, 6, 330.0f, phase, false);
        dispatchPendingMessages();
        const auto silentImage = signature->createComponentSnapshot(signature->getLocalBounds(), true, 1.0f);
        writePng(silentImage, output.getChildFile("stre-signature-silent.png"));

        std::array<juce::Image, 4> images;
        const std::array<const char*, 4> names { "stretch", "grain", "fft1", "fft2" };
        setPlain(processor, STRETRAudioProcessor::kParamTrigger, 1.0f);
        for (int engine = 0; engine < 4; ++engine)
        {
            setPlain(processor, STRETRAudioProcessor::kParamTrigger, 0.0f);
            setPlain(processor, STRETRAudioProcessor::kParamEngine, static_cast<float>(engine));
            processTone(processor, 6, 330.0f, phase, false);
            dispatchPendingMessages();
            setPlain(processor, STRETRAudioProcessor::kParamTrigger, 1.0f);
            if (engine == 3)
            {
                for (const auto frequency : { 220.0f, 330.0f, 440.0f })
                {
                    processTone(processor, 30, frequency, phase, false);
                    dispatchPendingMessages();
                }
            }
            else
            {
                processTone(processor, 90, 330.0f, phase, false);
                dispatchPendingMessages();
            }
            images[static_cast<std::size_t>(engine)] =
                signature->createComponentSnapshot(signature->getLocalBounds(), true, 1.0f);
            writePng(images[static_cast<std::size_t>(engine)],
                     output.getChildFile(juce::String("stre-signature-") + names[engine] + ".png"));
            writePng(editor->createComponentSnapshot(editor->getLocalBounds(), true, 1.0f),
                     output.getChildFile(juce::String("stre-editor-") + names[engine] + ".png"));
            const auto minimumDifference = engine < 2 ? 0.01 : 0.002;
            require(imageDifference(silentImage, images[static_cast<std::size_t>(engine)])
                        > minimumDifference,
                    std::string(names[engine]) + " signature did not reveal measured wet audio");
        }
        require(imageDifference(images[0], images[1]) > 0.002,
                "STRETCH and GRAIN produced indistinguishable measured signatures");
        require(imageDifference(images[2], images[3]) > 0.001,
                "FFT1 and FFT2 produced indistinguishable measured signatures");

        editor->setVisible(false);
        editor->removeFromDesktop();
        std::cout << "STRE real-telemetry signature render probe passed.\n";
        return 0;
    }
    catch (const std::exception& exception)
    {
        std::cerr << "STRE signature render probe failed: " << exception.what() << '\n';
        return 1;
    }
}
