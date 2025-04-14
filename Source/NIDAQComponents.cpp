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

#include <chrono>
#include <math.h>

#include "NIDAQComponents.h"

AnalogInput::AnalogInput (String name, NIDAQ::int32 termCfgs) : InputChannel (name)
{
    sourceTypes.clear();

    if (termCfgs & DAQmx_Val_Bit_TermCfg_RSE)
        sourceTypes.add (SOURCE_TYPE::RSE);

    if (termCfgs & DAQmx_Val_Bit_TermCfg_NRSE)
        sourceTypes.add (SOURCE_TYPE::NRSE);

    if (termCfgs & DAQmx_Val_Bit_TermCfg_Diff)
        sourceTypes.add (SOURCE_TYPE::DIFF);

    if (termCfgs & DAQmx_Val_Bit_TermCfg_PseudoDIFF)
        sourceTypes.add (SOURCE_TYPE::PSEUDO_DIFF);
}

static int32 GetTerminalNameWithDevPrefix (NIDAQ::TaskHandle taskHandle, const char terminalName[], char triggerName[]);

static int32 GetTerminalNameWithDevPrefix (NIDAQ::TaskHandle taskHandle, const char terminalName[], char triggerName[])
{
    NIDAQ::int32 error = 0;
    char device[256];
    NIDAQ::int32 productCategory;
    NIDAQ::uInt32 numDevices, i = 1;

    DAQmxErrChk (NIDAQ::DAQmxGetTaskNumDevices (taskHandle, &numDevices));
    while (i <= numDevices)
    {
        DAQmxErrChk (NIDAQ::DAQmxGetNthTaskDevice (taskHandle, i++, device, 256));
        DAQmxErrChk (NIDAQ::DAQmxGetDevProductCategory (device, &productCategory));
        if (productCategory != DAQmx_Val_CSeriesModule && productCategory != DAQmx_Val_SCXIModule)
        {
            *triggerName++ = '/';
            strcat (strcat (strcpy (triggerName, device), "/"), terminalName);
            break;
        }
    }

Error:
    return error;
}

void NIDAQmxDeviceManager::scanForDevices()
{
    devices.clear();

    char data[2048] = { 0 };
    NIDAQ::DAQmxGetSysDevNames (data, sizeof (data));

    StringArray deviceList;
    deviceList.addTokens (&data[0], ", ", "\"");

    StringArray deviceNames;
    StringArray productList;

    for (int i = 0; i < deviceList.size(); i++)
    {
        if (deviceList[i].length() > 0)
        {
            String deviceName = deviceList[i].toUTF8();

            /* Get product name */
            char pname[2048] = { 0 };
            NIDAQ::DAQmxGetDevProductType (STR2CHR (deviceName), &pname[0], sizeof (pname));
            devices.add (new NIDAQDevice (deviceName));
            devices.getLast()->productName = String (&pname[0]);
            NIDAQ::uInt32 slotNbr;
            NIDAQ::DAQmxGetDevPXISlotNum(STR2CHR (deviceName), &slotNbr);
            devices.getLast()->slotNbr = slotNbr;
        }
    }

    if (! devices.size())
        devices.add (new NIDAQDevice ("Simulated")); // TODO: Make SimulatedClass derived from NIDAQDevice
}

int NIDAQmxDeviceManager::getDeviceIndexFromName (String name)
{

    for (int i = 0; i < devices.size(); i++)
        if (devices[i]->getName() == name){
            return i;
        }
    return -1;
}

NIDAQmx::NIDAQmx (Array<NIDAQDevice*> devices_)
    : Thread ("HaeslerProbe"),
      devices(devices_)
{
    ai.clear();
    for (int i =0; i< devices.size(); i++) {
        connect(i);
    }

    LOGD(devices.size());
    LOGD(ai.size());
    LOGD(di.size());
    digitalReadSize = 32;

    // Pre-define reasonable sample rates
    float sample_rates[NUM_SAMPLE_RATES] = {
        1000.0f, 1250.0f, 1500.0f, 2000.0f, 2500.0f, 3000.0f, 3330.0f, 4000.0f, 5000.0f, 6250.0f, 8000.0f, 10000.0f, 12500.0f, 15000.0f, 20000.0f, 25000.0f, 30000.0f, 40000.0f, 62500.0f
    };

    sampleRates.clear();

    int idx = 0;
    while (sample_rates[idx] <= devices[0]->sampleRateRange.max && idx < NUM_SAMPLE_RATES)
        sampleRates.add (sample_rates[idx++]);

    // Default to highest sample rate
    sampleRateIndex = sampleRates.size() - 1;

    // Default to largest voltage range
    voltageRangeIndex = devices[0]->voltageRanges.size() - 1;
}

void NIDAQmx::connect(int index)
{
    NIDAQDevice* device = devices[index];

    String deviceName = device->getName();

    if (deviceName == "Simulated")
    {
        device->sampleRateRange = SettingsRange (1000.0f, 30000.0f);
        device->voltageRanges.add (SettingsRange (-10.0f, 10.0f));
        device->productName = String ("No Device Detected");
    }
    else
    {
        /* Get category type */
        NIDAQ::DAQmxGetDevProductCategory (STR2CHR (deviceName), &device->deviceCategory);
        LOGD ("Product Category: ", device->deviceCategory);

        device->digitalReadSize = 32;

        NIDAQ::DAQmxGetDevProductNum (STR2CHR (deviceName), &device->productNum);
        LOGD ("Product Num: ", device->productNum);

        NIDAQ::DAQmxGetDevSerialNum (STR2CHR (deviceName), &device->serialNum);
        LOGD ("Serial Num: ", device->serialNum);

        /* Get simultaneous sampling supported */
        NIDAQ::bool32 supported = false;
        NIDAQ::DAQmxGetDevAISimultaneousSamplingSupported (STR2CHR (deviceName), &supported);
        device->simAISamplingSupported = supported;
        LOGD ("Simultaneous sampling supported: ", supported ? "YES" : "NO");

        /* Get device sample rates */
        NIDAQ::float64 smin;
        NIDAQ::DAQmxGetDevAIMinRate (STR2CHR (deviceName), &smin);
        LOGD ("Min sample rate: ", smin);

        NIDAQ::float64 smaxs;
        NIDAQ::DAQmxGetDevAIMaxSingleChanRate (STR2CHR (deviceName), &smaxs);
        LOGD ("Max single channel sample rate: ", smaxs);

        NIDAQ::float64 smaxm;
        NIDAQ::DAQmxGetDevAIMaxMultiChanRate (STR2CHR (deviceName), &smaxm);
        LOGD ("Max multi channel sample rate: ", smaxm);

        NIDAQ::float64 data[512];
        NIDAQ::DAQmxGetDevAIVoltageRngs (STR2CHR (deviceName), &data[0], sizeof (data));

        // Get available voltage ranges
        device->voltageRanges.clear();
        LOGD ("Detected voltage ranges: \n");
        for (int i = 0; i < 512; i += 2)
        {
            NIDAQ::float64 vmin = data[i];
            NIDAQ::float64 vmax = data[i + 1];
            if (vmin == vmax || abs (vmin) < 1e-10 || vmax < 1e-2)
                break;
                device->voltageRanges.add (SettingsRange (vmin, vmax));
        }

        NIDAQ::int32 error = 0;
        char errBuff[ERR_BUFF_SIZE] = { '\0' };

        char ai_channel_data[2048];
        NIDAQ::DAQmxGetDevAIPhysicalChans (STR2CHR (device->getName()), &ai_channel_data[0], sizeof (ai_channel_data));

        StringArray channel_list;
        channel_list.addTokens (&ai_channel_data[0], ", ", "\"");

        device->numAIChannels = 0;
        

        LOGD ("Detected ", channel_list.size(), " analog input channels");
        int count = 0;
        for (int i = 0; i <  channel_list.size(); i++)
        {
            if (channel_list[i].length() > 0)
            {
                /* Get channel termination */
                NIDAQ::int32 termCfgs;
                NIDAQ::DAQmxGetPhysicalChanAITermCfgs (channel_list[i].toUTF8(), &termCfgs);

                String name = channel_list[i].toRawUTF8();

                String terminalConfigurations = String::toHexString (termCfgs);

                ai.add (new AnalogInput (name, termCfgs));

                if (device->numAIChannels++ <= numActiveAnalogInputs)
                {
                    ai.getLast()->setAvailable (true);
                    ai.getLast()->setEnabled (true);
                }

                LOGD ("Adding analog input channel: ", name, " with terminal config: ", " (", termCfgs, ") enabled: ", ai.getLast()->isEnabled() ? "YES" : "NO");
                count++;

                if(count>=8)
                    break;
            }
        }

        // Get ADC resolution for each voltage range (throwing error as is)
        NIDAQ::TaskHandle adcResolutionQuery;

        NIDAQ::DAQmxCreateTask ("ADCResolutionQuery", &adcResolutionQuery);

        SettingsRange vRange;

        for (int i = 0; i < device->voltageRanges.size(); i++)
        {
            vRange = device->voltageRanges[i];

            DAQmxErrChk (NIDAQ::DAQmxCreateAIVoltageChan (
                adcResolutionQuery, // task handle
                STR2CHR (ai[i]->getName()), // NIDAQ physical channel name (e.g. dev1/ai1)
                "", // user-defined channel name (optional)
                DAQmx_Val_Cfg_Default, // input terminal configuration
                vRange.min, // min input voltage
                vRange.max, // max input voltage
                DAQmx_Val_Volts, // voltage units
                NULL));

            NIDAQ::float64 adcResolution;
            DAQmxErrChk (NIDAQ::DAQmxGetAIResolution (adcResolutionQuery, STR2CHR (ai[i]->getName()), &adcResolution));

            device->adcResolutions.add (adcResolution);
        }

        NIDAQ::DAQmxStopTask (adcResolutionQuery);
        NIDAQ::DAQmxClearTask (adcResolutionQuery);

        // Get Digital Input Channels

        char di_channel_data[2048];
        // NIDAQ::DAQmxGetDevTerminals(STR2CHR(deviceName), &data[0], sizeof(data)); //gets all terminals
        // NIDAQ::DAQmxGetDevDIPorts(STR2CHR(deviceName), &data[0], sizeof(data));	//gets line name
        NIDAQ::DAQmxGetDevDILines (STR2CHR (deviceName), &di_channel_data[0], sizeof (di_channel_data)); // gets ports on line
        LOGD ("Found digital inputs: ");

        channel_list.clear();
        channel_list.addTokens (&di_channel_data[0], ", ", "\"");

        device->digitalPortNames.clear();
        device->digitalPortStates.clear();
        device->numDIChannels = 0;
        di.clear();
        

        for (int i = 0; i < channel_list.size(); i++)
        {
            StringArray channel_type;
            channel_type.addTokens (channel_list[i], "/", "\"");
            if (channel_list[i].length() > 0)
            {
                String fullName = channel_list[i].toRawUTF8();

                String lineName = fullName.fromFirstOccurrenceOf ("/", false, false);
                String portName = fullName.upToLastOccurrenceOf ("/", false, false);

                // Add port to list of ports
                if (! device->digitalPortNames.contains (portName.toRawUTF8()))
                {
                    device->digitalPortNames.add (portName.toRawUTF8());
                    if (device->numDIChannels < numActiveDigitalInputs)
                        device->digitalPortStates.add (true);
                    else
                    device->digitalPortStates.add (false);
                }

                di.add (new InputChannel (fullName));

                di.getLast()->setAvailable (true);
                if (device->numDIChannels < numActiveDigitalInputs)
                    di.getLast()->setEnabled (true);

                    device->numDIChannels++;
            }
        }
        LOGD (device->numDIChannels);

        // Set sample rate range
        NIDAQ::float64 smax = smaxm;
        if (! device->simAISamplingSupported)
            smax /= numActiveAnalogInputs;

            device->sampleRateRange = SettingsRange (smin, smax);

    Error:

        if (DAQmxFailed (error))
            NIDAQ::DAQmxGetExtendedErrorInfo (errBuff, ERR_BUFF_SIZE);

        if (adcResolutionQuery != 0)
        {
            // DAQmx Stop Code
            NIDAQ::DAQmxStopTask (adcResolutionQuery);
            NIDAQ::DAQmxClearTask (adcResolutionQuery);
        }

        if (DAQmxFailed (error))
            LOGE ("DAQmx Error: ", errBuff);
        fflush (stdout);

        return;
    }
}

uint32 NIDAQmx::getActiveDigitalLines()
{
    if (! getNumActiveDigitalInputs())
        return 0;

    uint32 linesEnabled = 0;
    for (int i = 0; i < di.size(); i++)
    {
        if (di[i]->isEnabled())
            linesEnabled += pow (2, i);
    }
    return linesEnabled;
}

void NIDAQmx::run()
{
    /* Derived from NIDAQmx: ANSI C Example program: ContAI-ReadDigChan.c */

    /* Single task to handle all analog inputs */
    std::vector<NIDAQ::TaskHandle> taskHandlesAI = std::vector<NIDAQ::TaskHandle>();

    /* Potentially multiple tasks to handle different digital line properties */
    std::vector<NIDAQ::TaskHandle> taskHandlesDI = std::vector<NIDAQ::TaskHandle>();

    NIDAQ::TaskHandle taskHandleDI_Read = 0;
    NIDAQ::int32 error = 0;
    char errBuff[ERR_BUFF_SIZE] = { '\0' };
    char trigName[256];
    NIDAQ::TaskHandle masterHandleAI = 0;
    /**************************************/
    /********CONFIG ANALOG CHANNELS********/
    /**************************************/
    /* Create an analog input task */
    for(int dev_i=0; dev_i < devices.size(); dev_i++){
        NIDAQ::TaskHandle taskHandleAI = 0;
        DAQmxErrChk (NIDAQ::DAQmxCreateTask (STR2CHR ("AITask_PXI" + getSlotNumber(dev_i)), &taskHandleAI));
    
        /* Create a voltage channel for each analog input */

        for (int i = 0; i < numActiveAnalogInputs; i++)
        {
            NIDAQ::int32 termConfig = DAQmx_Val_Diff;

            SettingsRange voltageRange = devices[dev_i]->voltageRanges[voltageRangeIndex];
            LOGD (ai[dev_i * numActiveAnalogInputs + i]->getName());

            DAQmxErrChk (NIDAQ::DAQmxCreateAIVoltageChan (
                taskHandleAI, // task handle
                STR2CHR (ai[dev_i*numActiveAnalogInputs + i]->getName()), // NIDAQ physical channel name (e.g. dev1/ai1)
                "", // user-defined channel name (optional)
                termConfig, // input terminal configuration
                voltageRange.min, // min input voltage
                voltageRange.max, // max input voltage
                DAQmx_Val_Volts, // voltage units
                NULL));
        }

        // Master: internal clock
        if (dev_i == 0){
            DAQmxErrChk (GetTerminalNameWithDevPrefix (taskHandleAI, "PXI_Trig0", trigName));

            DAQmxErrChk(NIDAQ::DAQmxCfgSampClkTiming(taskHandleAI,
                "",
                getSampleRate(),
                DAQmx_Val_Rising,
                DAQmx_Val_ContSamps,
                numActiveAnalogInputs * CHANNEL_BUFFER_SIZE));
    
            // Export master’s sample clock
            DAQmxErrChk(NIDAQ::DAQmxExportSignal(taskHandleAI,
                DAQmx_Val_SampleClock,
                trigName));
        }
        else
        {
            // Slaves: use master’s clock
           DAQmxErrChk (NIDAQ::DAQmxCfgSampClkTiming (taskHandleAI,
                                                       trigName,
                                                       getSampleRate(),
                                                       DAQmx_Val_Rising,
                                                       DAQmx_Val_ContSamps,
                                                       numActiveAnalogInputs * CHANNEL_BUFFER_SIZE));
        }
    
        taskHandlesAI.push_back (taskHandleAI);

        /************************************/
        /********CONFIG DIGITAL LINES********/
        /************************************/

        if (getSlotNumber(dev_i) == "2" || getSlotNumber(dev_i) == "3" || getSlotNumber(dev_i) == "4" || getSlotNumber(dev_i) == "5"){
            NIDAQ::TaskHandle taskHandleDI = 0;
    
            DAQmxErrChk(NIDAQ::DAQmxCreateTask(STR2CHR ("DITask" + getSlotNumber(dev_i)) , &taskHandleDI));
            DAQmxErrChk(NIDAQ::DAQmxCreateDOChan(taskHandleDI,
                    STR2CHR(devices[dev_i]->getName()+ "/port0/line0:7"),
                    "",
                    DAQmx_Val_ChanPerLine));
        
            DAQmxErrChk(NIDAQ::DAQmxCfgSampClkTiming(taskHandleDI,
                    trigName, getSampleRate(), DAQmx_Val_Rising,
                    DAQmx_Val_ContSamps, numActiveAnalogInputs * CHANNEL_BUFFER_SIZE));
            
            DAQmxErrChk(NIDAQ::DAQmxSetWriteRegenMode(taskHandleDI, DAQmx_Val_AllowRegen));

            taskHandlesDI.push_back (taskHandleDI);
        }

        if (getSlotNumber(dev_i) == "6") {
        
            DAQmxErrChk(NIDAQ::DAQmxCreateTask("DI_Read_Task", &taskHandleDI_Read));
            
            DAQmxErrChk(NIDAQ::DAQmxCreateDIChan(taskHandleDI_Read,
                STR2CHR(devices[dev_i]->getName() + "/port0/line23"),
                "",
                DAQmx_Val_ChanPerLine));
            
                DAQmxErrChk(NIDAQ::DAQmxCfgSampClkTiming(taskHandleDI_Read,
                    trigName, getSampleRate(), DAQmx_Val_Rising,
                    DAQmx_Val_ContSamps, numActiveAnalogInputs * CHANNEL_BUFFER_SIZE));
    
        }
        
    }

    const int numStations = taskHandlesDI.size();
    const int numLinesPerStation = 8;
    const int pulseLengthInSamples = 1; 
    
    NIDAQ::float64 timeout = 5.0;
    const int samplesPerStation = numLinesPerStation * pulseLengthInSamples;
    const int  totalSamples = samplesPerStation * numStations;
    
    for (int station_i = 0; station_i < numStations; ++station_i)
    {
        std::vector<NIDAQ::uInt32> waveform(totalSamples, 0);
    
        // Offset in time where this station should start its sequence
        int startSample = station_i * samplesPerStation;
        LOGD ("station: ", station_i);

        for (int line_i = 0; line_i < numLinesPerStation; ++line_i)
        {
            int sampleOffset = startSample + line_i * pulseLengthInSamples;
            NIDAQ::uInt8 bitMask = static_cast<NIDAQ::uInt8>(1 << line_i);
            waveform[sampleOffset] = bitMask;  // single-sample pulse
            LOGD ("index: ", sampleOffset, "value: ", waveform[sampleOffset]);
        }

        //LOGD(waveform);

        NIDAQ::int32 samplesWritten = 0;
        DAQmxErrChk(NIDAQ::DAQmxWriteDigitalU32(
            taskHandlesDI[station_i],
            totalSamples,
            0,                          // autoStart = false
            timeout,                   // timeout
            DAQmx_Val_GroupByChannel,
            waveform.data(),
            &samplesWritten,
            NULL));
    }
    
    LOGD("Number of digital output: ", taskHandlesDI.size());
    // This order is necessary to get the timing right
    for (auto& taskHandleAI : taskHandlesAI)
        DAQmxErrChk (NIDAQ::DAQmxTaskControl (taskHandleAI, DAQmx_Val_Task_Commit));

    for (auto& taskHandleDI : taskHandlesDI)
        DAQmxErrChk (NIDAQ::DAQmxTaskControl (taskHandleDI, DAQmx_Val_Task_Commit));
    
    DAQmxErrChk (NIDAQ::DAQmxTaskControl (taskHandleDI_Read, DAQmx_Val_Task_Commit));

    for (auto& taskHandleDI : taskHandlesDI)
        DAQmxErrChk (NIDAQ::DAQmxStartTask (taskHandleDI));

    DAQmxErrChk (NIDAQ::DAQmxStartTask (taskHandleDI_Read));

    for(int i=1; i<taskHandlesAI.size(); i++)
        DAQmxErrChk (NIDAQ::DAQmxStartTask (taskHandlesAI[i]));

    DAQmxErrChk (NIDAQ::DAQmxStartTask (taskHandlesAI[0]));  // Master task which trigger everything


    uint64 linesEnabled = 0;
    double ts;

    ai_timestamp = 0;
    NIDAQ::int32 di_read = 0;
    NIDAQ::int32 numSampsPerChan = CHANNEL_BUFFER_SIZE;
    NIDAQ::int32 ai_read = 0;

    aiBuffer->clear();

    LOGD ("Start acquisition");
    int digital_line_index  = 0;

    int arraySizeInSamps = numActiveAnalogInputs * numSampsPerChan*getNsample();

    while (! threadShouldExit())
    {   
        // start the thread to write on the digital line one pulse after each other with a duration of 1ms
        
        int numDevices = devices.size();
        std::vector<std::vector<NIDAQ::float64>> dev_ai_data (devices.size());
        for (int dev_i = 0; dev_i < numDevices; dev_i++) {
            if (numActiveAnalogInputs) {
                dev_ai_data[dev_i].resize(arraySizeInSamps);

                DAQmxErrChk(NIDAQ::DAQmxReadAnalogF64(
                    taskHandlesAI[dev_i],
                    numSampsPerChan*getNsample(),
                    timeout,
                    DAQmx_Val_GroupByChannel,
                    dev_ai_data[dev_i].data(),
                    arraySizeInSamps,
                    &ai_read,
                    NULL));  
            }
        }

       
        NIDAQ::int32 samplesRead = 0;

        std::vector<NIDAQ::uInt8> dev_di_data (numSampsPerChan * getNsample());

        dev_di_data.resize (numSampsPerChan);
        DAQmxErrChk(NIDAQ::DAQmxReadDigitalLines(
            taskHandleDI_Read,  // The last handle is for reading
            numSampsPerChan * getNsample(), // numSampsPerChan
            timeout,
            DAQmx_Val_GroupByChannel,
            dev_di_data.data(),
            numSampsPerChan * getNsample(),
            &samplesRead,
            NULL,
            NULL));

        int totalChannels = numActiveAnalogInputs * numDevices;
        int totalScans = numSampsPerChan*getNsample(); 
        int scanIdx = 0;
        int nbr_channel = numActiveAnalogInputs*numDevices*32;


        HeapBlock<float> output;
        output.allocate(nbr_channel, true);
        
        
        

        for (int nsample=0; nsample<getNsample(); nsample++){
            int writeIdx = 0;
            for (int station = 0; station < numDevices; ++station) {
                for (int ch = 0; ch < numActiveDigitalInputs; ++ch) {
                    for(int analogch=0; analogch<numActiveAnalogInputs; analogch++)
                        output[writeIdx++] = dev_ai_data[station][ch +analogch*numActiveDigitalInputs + nsample*numActiveDigitalInputs];  // step per sample
                    
                }
            }

            juce::uint64 eventCode = 0;
            // Update timestamp
            ai_timestamp++;
            // Now add the full scan (all devices) to the buffer
            aiBuffer->addToBuffer (output, &ai_timestamp, &ts, &eventCode, 1);
        }

    }
        // fflush(stdout);
    

    /*********************************************/
    // DAQmx Stop Code
    /*********************************************/
    for (int dev_i = 0; dev_i < taskHandlesAI.size(); dev_i++)
    {
        if (numActiveAnalogInputs)
            NIDAQ::DAQmxStopTask (taskHandlesAI[dev_i]);
        if (numActiveAnalogInputs)
            NIDAQ::DAQmxClearTask (taskHandlesAI[dev_i]);
        if (numActiveDigitalInputs)
        {
            for (auto& taskHandleDI : taskHandlesDI)
            {
                NIDAQ::DAQmxStopTask (taskHandleDI);
                NIDAQ::DAQmxClearTask (taskHandleDI);
            }
        }
        NIDAQ::DAQmxStopTask (taskHandleDI_Read);
        NIDAQ::DAQmxClearTask (taskHandleDI_Read);
    }

    return;

Error:
    LOGD ("Enter error zone!")
    if (DAQmxFailed (error))
        NIDAQ::DAQmxGetExtendedErrorInfo (errBuff, ERR_BUFF_SIZE);
    
        for (int dev_i = 0; dev_i < taskHandlesAI.size(); dev_i++)
    {
        if (taskHandlesAI[dev_i] != 0)
        {
            // DAQmx Stop Code
            NIDAQ::DAQmxStopTask (taskHandlesAI[dev_i]);
            NIDAQ::DAQmxClearTask (taskHandlesAI[dev_i]);
        }
    }

    if (taskHandlesDI.size() > 0)
    {
        for (auto& taskHandleDI : taskHandlesDI)
        {
            // DAQmx Stop Code
            NIDAQ::DAQmxStopTask (taskHandleDI);
            NIDAQ::DAQmxClearTask (taskHandleDI);
        }
        NIDAQ::DAQmxStopTask (taskHandleDI_Read);
        NIDAQ::DAQmxClearTask (taskHandleDI_Read);
    }

    if (DAQmxFailed (error))
        LOGE ("DAQmx Error: ", errBuff);

    fflush (stdout);

    return;

}