#pragma once

#include "synth/AppRegistry.hpp"

#include <juce_gui_extra/juce_gui_extra.h>

#include <cstddef>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace synth_sheaf_patch {

// Resolves a launcher command-line argument to one of `apps`'s
// registrations by stable appId. Returns nullptr -- the "show the picker"
// sentinel -- when `argument` is empty or does not match any registered
// appId, so callers can factor direct-launch selection from the picker's
// own row-lookup logic and unit test it without constructing a
// LauncherComponent or launching a real window.
inline const synth::SynthAppRegistration* ResolveDirectLaunchApp(
    const std::vector<synth::SynthAppRegistration>& apps, std::string_view argument) {
    if (argument.empty()) {
        return nullptr;
    }

    for (const auto& app : apps) {
        if (app.manifest.appId == argument) {
            return &app;
        }
    }
    return nullptr;
}

class LauncherComponent final : public juce::Component {
public:
    LauncherComponent(std::vector<synth::SynthAppRegistration> apps, std::filesystem::path dataRoot)
        : dataRoot_(std::move(dataRoot)), apps_(std::move(apps)) {
        synth::SortSynthAppRegistrationsById(apps_);

        title_.setText("Sheaf Patch", juce::dontSendNotification);
        title_.setJustificationType(juce::Justification::centredLeft);
        title_.setColour(juce::Label::textColourId, juce::Colours::white);
        title_.setFont(juce::Font(juce::FontOptions(24.0f, juce::Font::bold)));
        addAndMakeVisible(title_);

        subtitle_.setText("Select an app", juce::dontSendNotification);
        subtitle_.setJustificationType(juce::Justification::centredLeft);
        subtitle_.setColour(juce::Label::textColourId, juce::Colours::lightgrey);
        addAndMakeVisible(subtitle_);

        rowButtons_.reserve(apps_.size());
        for (const auto& app : apps_) {
            auto button = std::make_unique<juce::TextButton>();
            button->setButtonText(RowText(app.manifest));
            button->setComponentID(RowComponentId(app.manifest.appId));
            button->setName(app.manifest.appId);
            button->setColour(juce::TextButton::buttonColourId, juce::Colour(0xff20242a));
            button->setColour(juce::TextButton::buttonOnColourId, juce::Colour(0xff2d333b));
            button->setColour(juce::TextButton::textColourOffId, juce::Colours::white);
            button->setColour(juce::TextButton::textColourOnId, juce::Colours::white);
            button->onClick = [this, appId = app.manifest.appId] {
                ActivateApp(appId);
            };
            addAndMakeVisible(*button);
            rowButtons_.push_back(std::move(button));
        }
    }

    void paint(juce::Graphics& g) override {
        g.fillAll(juce::Colour(0xff111418));
    }

    void resized() override {
        auto bounds = getLocalBounds().reduced(24);
        title_.setBounds(bounds.removeFromTop(32));
        subtitle_.setBounds(bounds.removeFromTop(24));
        bounds.removeFromTop(16);

        constexpr int rowHeight = 72;
        constexpr int rowGap = 10;
        for (auto& button : rowButtons_) {
            button->setBounds(bounds.removeFromTop(rowHeight));
            bounds.removeFromTop(rowGap);
        }
    }

    std::size_t AppCountForTesting() const {
        return apps_.size();
    }

    std::string AppIdForTesting(std::size_t index) const {
        if (index >= apps_.size()) {
            throw std::out_of_range("launcher app index out of range");
        }
        return apps_[index].manifest.appId;
    }

    juce::String RowTextForTesting(std::string_view appId) const {
        const std::size_t index = FindAppIndex(appId);
        if (index == apps_.size()) {
            return {};
        }
        return rowButtons_[index]->getButtonText();
    }

    juce::TextButton* RowButtonForTesting(std::string_view appId) {
        const std::size_t index = FindAppIndex(appId);
        if (index == apps_.size()) {
            return nullptr;
        }
        return rowButtons_[index].get();
    }

private:
    static juce::String RowComponentId(std::string_view appId) {
        return juce::String("app-row-") + juce::String(std::string(appId));
    }

    static juce::String RowText(const synth::SynthAppManifest& manifest) {
        return juce::String(manifest.displayName)
            + " | Author: " + juce::String(manifest.author)
            + " | Category: " + juce::String(manifest.category)
            + " | Minimum encoders: " + juce::String(manifest.hardware.minEncoders);
    }

    std::size_t FindAppIndex(std::string_view appId) const {
        for (std::size_t i = 0; i < apps_.size(); ++i) {
            if (apps_[i].manifest.appId == appId) {
                return i;
            }
        }
        return apps_.size();
    }

    void ActivateApp(std::string_view appId) {
        const std::size_t index = FindAppIndex(appId);
        if (index == apps_.size()) {
            return;
        }

        auto& registration = apps_[index];
        if (registration.launch) {
            registration.launch(synth::SheafPatchDataPathsForApp(dataRoot_, registration.manifest.appId));
        }
    }

    std::filesystem::path dataRoot_;
    std::vector<synth::SynthAppRegistration> apps_;
    juce::Label title_;
    juce::Label subtitle_;
    std::vector<std::unique_ptr<juce::TextButton>> rowButtons_;
};

}  // namespace synth_sheaf_patch
