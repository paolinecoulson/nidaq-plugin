/*
    ------------------------------------------------------------------

    This file is part of the Open Ephys GUI
    Copyright (C) 2024 Open Ephys

    ------------------------------------------------------------------

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.

*/

#include "NIDAQEditor.h"
#include "NIDAQThread.h"

EditorBackground::EditorBackground () {}

void EditorBackground::paint (Graphics& g)
{

   float settingsOffsetX = 10;
   g.setColour (findColour (ThemeColours::defaultText));
   g.setFont (10);

   g.drawText (String ("SAMPLE RATE"), settingsOffsetX, 10, 100, 10, Justification::centredLeft);
   g.drawText (String ("AI VOLTAGE RANGE"), settingsOffsetX, 60, 100, 10, Justification::centredLeft);
}

BackgroundLoader::BackgroundLoader (NIDAQThread* thread, NIDAQEditor* editor)
    : Thread ("NIDAQ Loader"), t (thread), e (editor)
{
}

BackgroundLoader::~BackgroundLoader()
{
}

void BackgroundLoader::run()
{
    /* This process is used to initiate processor loading in the background to prevent this plugin from blocking the main GUI*/

    /* Let the main GUI know the plugin is done initializing */
    MessageManagerLock mml;
    CoreServices::updateSignalChain (e);
    CoreServices::sendStatusMessage ("NIDAQ plugin ready for acquisition!");
}

NIDAQEditor::NIDAQEditor (GenericProcessor* parentNode, NIDAQThread* t)
    : GenericEditor (parentNode), thread (t), currentConfigWindow (nullptr)
{
    draw();
}

void NIDAQEditor::draw()
{
    NIDAQThread* t = thread;

    int xOffset = 10;

    sampleRateSelectBox = new ComboBox ("SampleRateSelectBox");
    sampleRateSelectBox->setBounds (xOffset,35, 85, 20);
    Array<NIDAQ::float64> sampleRates = t->getSampleRates();
    for (int i = 0; i < sampleRates.size(); i++)
    {
        sampleRateSelectBox->addItem (String (sampleRates[i]) + " S/s", i + 1);
    }
    sampleRateSelectBox->setSelectedItemIndex (t->getSampleRateIndex(), false);
    sampleRateSelectBox->addListener (this);
    addAndMakeVisible (sampleRateSelectBox);

    voltageRangeSelectBox = new ComboBox ("VoltageRangeSelectBox");
    voltageRangeSelectBox->setBounds (xOffset, 75, 85, 20);
    Array<SettingsRange> voltageRanges = t->getVoltageRanges();
    for (int i = 0; i < voltageRanges.size(); i++)
    {
        String rangeString = String (voltageRanges[i].min) + " to " + String (voltageRanges[i].max) + " V";
        voltageRangeSelectBox->addItem (rangeString, i + 1);
    }
    voltageRangeSelectBox->setSelectedItemIndex (t->getVoltageRangeIndex(), false);
    voltageRangeSelectBox->addListener (this);
    addAndMakeVisible (voltageRangeSelectBox);

    configureDeviceButton = new UtilityButton ("...");
    configureDeviceButton->setFont (FontOptions ((12.0f)));
    configureDeviceButton->setBounds (xOffset + 60, 25, 24, 12);
    configureDeviceButton->addListener (this);
    configureDeviceButton->setAlpha (0.5f);
    addAndMakeVisible (configureDeviceButton);


    
    background = new EditorBackground ();
    background->setBounds (0, 15, 1000, 150);
    addAndMakeVisible (background);
    background->toBack();
    background->repaint();
}

void NIDAQEditor::update (int numAnalog, int numDigital, int digitalReadSize)
{
    if (numAnalog != thread->getNumActiveAnalogInputs())
    {
        thread->setNumActiveAnalogChannels (numAnalog);
        thread->updateAnalogChannels();

        CoreServices::updateSignalChain (this);

        ((CallOutBox*) currentConfigWindow->getParentComponent())->dismiss();
    }

    if (numDigital != thread->getNumActiveDigitalInputs())
    {
        thread->setNumActiveDigitalChannels (numDigital);
        thread->updateDigitalChannels();

        ((CallOutBox*) currentConfigWindow->getParentComponent())->dismiss();
    }

    if (digitalReadSize != thread->getDigitalReadSize())
    {
        thread->setDigitalReadSize (digitalReadSize);
    }

    draw();
}

NIDAQEditor::~NIDAQEditor()
{
}

void NIDAQEditor::startAcquisition()
{
    sampleRateSelectBox->setEnabled (false);
    voltageRangeSelectBox->setEnabled (false);
    configureDeviceButton->setEnabled (false);
}

void NIDAQEditor::stopAcquisition()
{

    sampleRateSelectBox->setEnabled (true);
    voltageRangeSelectBox->setEnabled (true);

    //Enable device config button
    configureDeviceButton->setEnabled (true);
}

/** Respond to button presses */
void NIDAQEditor::buttonClicked (Button* button)
{
    // Ignore any button presses if there is no input source
    if (thread->foundInputSource())
        buttonEvent (button);
}

void NIDAQEditor::comboBoxChanged (ComboBox* comboBox)
{
    if (comboBox == sampleRateSelectBox)
    {
        if (! thread->isThreadRunning())
        {
            thread->setSampleRate (comboBox->getSelectedId() - 1);
            CoreServices::updateSignalChain (this);
        }
        else
        {
            comboBox->setSelectedItemIndex (thread->getSampleRateIndex());
        }
    }
    else // (comboBox == voltageRangeSelectBox)
    {
        if (! thread->isThreadRunning())
        {
            thread->setVoltageRange (comboBox->getSelectedId() - 1);
            CoreServices::updateSignalChain (this);
        }
        else
        {
            comboBox->setSelectedItemIndex (thread->getVoltageRangeIndex());
        }
    }
}

void NIDAQEditor::buttonEvent (Button* button)
{
     if (button == configureDeviceButton)
    {
        if (! thread->isThreadRunning())
        {
            currentConfigWindow = new PopupConfigurationWindow (this);

            CallOutBox& myBox = CallOutBox::launchAsynchronously (std::unique_ptr<Component> (currentConfigWindow),
                                                                  button->getScreenBounds(),
                                                                  nullptr);

            myBox.setDismissalMouseClicksAreAlwaysConsumed (true);

            return;
        }
    }
}

void NIDAQEditor::saveCustomParametersToXml (XmlElement* xml)
{
    xml->setAttribute ("deviceName", thread->getDeviceName());
    xml->setAttribute ("sampleRate", thread->getSampleRate());
    xml->setAttribute ("voltageRange", thread->getVoltageRangeIndex());
    xml->setAttribute ("numAnalog", thread->getNumActiveAnalogInputs());
    xml->setAttribute ("numDigital", thread->getNumActiveDigitalInputs());
    xml->setAttribute ("digitalReadSize", thread->getDigitalReadSize());

    String digitalPortStates = "";
    for (int i = 0; i < thread->getNumPorts(); i++)
        digitalPortStates += thread->getPortState (i) ? "1" : "0";
    xml->setAttribute ("digitalPortStates", digitalPortStates);
}

void NIDAQEditor::loadCustomParametersFromXml (XmlElement* xml)
{
    String deviceToLoad = xml->getStringAttribute ("deviceName", "NIDAQmx");

    // Load device
    if (! deviceToLoad.equalsIgnoreCase ("NIDAQmx"))
    {
        int deviceIdx = thread->swapConnection (deviceToLoad);
        if (deviceIdx >= 0)
        {
            thread->setDeviceIndex (deviceIdx);
            //deviceSelectBox->setSelectedItemIndex (thread->getDeviceIndex(), false);
            draw();
        }
    }

    float sampleRate = xml->getStringAttribute ("sampleRate", "0.0").getFloatValue();

    // Load sample rate
    if (sampleRate > 0.0f)
    {
        int idx = 0;
        for (auto& sr : thread->getSampleRates())
        {
            if (sr == sampleRate)
            {
                LOGD ("Setting saved sample rate: " + String (sampleRate) + " (" + String (idx) + ")");
                thread->setSampleRate (idx);
                sampleRateSelectBox->setSelectedItemIndex (thread->getSampleRateIndex(), false);
                break;
            }
            idx++;
        }
    }

    // Load voltage range
    int voltageRangeIndex = xml->getStringAttribute ("voltageRange", "-1").getIntValue();

    if (voltageRangeIndex >= 0)
    {
        thread->setVoltageRange (voltageRangeIndex);
        voltageRangeSelectBox->setSelectedItemIndex (thread->getVoltageRangeIndex(), false);
    }

    // Load number of active analog channels
    int numAnalog = xml->getStringAttribute ("numAnalog", "0").getIntValue();

    if (numAnalog >= 0)
    {
        thread->setNumActiveAnalogChannels (numAnalog);
        thread->updateAnalogChannels();
    }

    // Load number of active digital channels
    int numDigital = xml->getStringAttribute ("numDigital", "0").getIntValue();

    if (numDigital >= 0)
    {
        thread->setNumActiveDigitalChannels (numDigital);
        thread->updateDigitalChannels();
    }

    // Load digital read size
    int digitalReadSize = xml->getStringAttribute ("digitalReadSize", "0").getIntValue();

    if (digitalReadSize >= 0)
    {
        thread->setDigitalReadSize (digitalReadSize);
    }

    String digitalPortStates = xml->getStringAttribute ("digitalPortStates", "000");

    for (int i = 0; i < digitalPortStates.length(); i++)
        thread->setPortState (i, digitalPortStates[i] == '1');

    draw();
}

PopupConfigurationWindow::PopupConfigurationWindow (NIDAQEditor* editor_)
    : editor (editor_)
{
    //tableHeader.reset(new TableHeaderComponent());

    analogLabel = new Label ("Analog", "Analog Inputs: ");
    analogLabel->setFont (Font (16.0f, Font::bold));
    analogLabel->setColour (Label::textColourId, Colours::white);
    analogLabel->setBounds (2, 8, 110, 20);
    addAndMakeVisible (analogLabel);

    int activeAnalogCount = editor->getNumActiveAnalogInputs();
    analogChannelCountSelect = new ComboBox ("Analog Count Selector");
    for (int i = 4; i <= editor->getTotalAvailableAnalogInputs(); i += 4)
    {
        analogChannelCountSelect->addItem (String (i), i / 4);
        if (i == activeAnalogCount)
            analogChannelCountSelect->setSelectedId (i / 4, dontSendNotification);
    }
    analogChannelCountSelect->setBounds (115, 8, 60, 20);
    analogChannelCountSelect->addListener (this);
    addAndMakeVisible (analogChannelCountSelect);

    digitalLabel = new Label ("Digital", "Digital Inputs: ");
    digitalLabel->setColour (Label::textColourId, Colours::white);
    digitalLabel->setBounds (2, 33, 110, 20);
    addAndMakeVisible (digitalLabel);

    int activeDigitalCount = editor->getNumActiveDigitalInputs();
    digitalChannelCountSelect = new ComboBox ("Digital Count Selector");
    for (int i = 0; i <= editor->getTotalAvailableDigitalInputs(); i += 4)
    {
        digitalChannelCountSelect->addItem (String (i), i / 4 + 1);
        if (i == activeDigitalCount)
            digitalChannelCountSelect->setSelectedId (i / 4 + 1, dontSendNotification);
    }
    digitalChannelCountSelect->setBounds (115, 33, 60, 20);
    digitalChannelCountSelect->addListener (this);
    addAndMakeVisible (digitalChannelCountSelect);

    digitalReadLabel = new Label ("Digital Read", "Digital Read: ");
    digitalReadLabel->setColour (Label::textColourId, Colours::white);
    digitalReadLabel->setBounds (2, 58, 110, 20);
    addAndMakeVisible (digitalReadLabel);

    digitalReadSelect = new ComboBox ("Digital Read Selector");
    Array<int> digitalReadOptions = { 8, 16, 32 };
    for (int i = 0; i < digitalReadOptions.size(); i++)
    {
        digitalReadSelect->addItem (String (digitalReadOptions[i]) + " bits", i + 1);
        if (digitalReadOptions[i] == editor->getDigitalReadSize())
            digitalReadSelect->setSelectedId (i + 1, dontSendNotification);
    }
    digitalReadSelect->setBounds (115, 58, 60, 20);
    digitalReadSelect->addListener (this);
    addAndMakeVisible (digitalReadSelect);

    for (int i = 0; i < editor->getNumPorts(); i++)
    {
        ToggleButton* button = new ToggleButton ("P" + String (i));
        button->setBounds (i * 60 + 5, 85, 58, 20);
        button->addListener (this);
        button->setToggleState (editor->getPortState (i), juce::dontSendNotification);
        addAndMakeVisible (button);
        digitalPortButtons.add (button);
    }

    setSize (180, 110);
}

void PopupConfigurationWindow::comboBoxChanged (ComboBox* comboBox)
{
    int numAnalogInputs = int (analogChannelCountSelect->getItemText (analogChannelCountSelect->getSelectedId() - 1).getFloatValue());
    int numDigitalInputs = int (digitalChannelCountSelect->getItemText (digitalChannelCountSelect->getSelectedId() - 1).getFloatValue());
    int digitalRead = int (digitalReadSelect->getItemText (digitalReadSelect->getSelectedId() - 1).getFloatValue());

    editor->update (numAnalogInputs, numDigitalInputs, digitalRead);
}

void PopupConfigurationWindow::paint (juce::Graphics& g)
{
    // Set the background color of toggle buttons based on their state
    for (int i = 0; i < digitalPortButtons.size(); ++i)
    {
        ToggleButton* button = digitalPortButtons[i];
        g.setColour (button->getToggleState() ? juce::Colours::green : juce::Colours::red);
        g.fillRect (button->getBounds());
    }
}

void PopupConfigurationWindow::buttonClicked (juce::Button* button)
{
    int portIdx = button->getName().getLastCharacter() - '0';
    editor->setPortState (portIdx, button->getToggleState());
    repaint();
}