/***********************************************************************************************************************
*                                                                                                                      *
* ps6000d                                                                                                              *
*                                                                                                                      *
* Copyright (c) 2012-2022 Andrew D. Zonenberg                                                                          *
* All rights reserved.                                                                                                 *
*                                                                                                                      *
* Redistribution and use in source and binary forms, with or without modification, are permitted provided that the     *
* following conditions are met:                                                                                        *
*                                                                                                                      *
*    * Redistributions of source code must retain the above copyright notice, this list of conditions, and the         *
*      following disclaimer.                                                                                           *
*                                                                                                                      *
*    * Redistributions in binary form must reproduce the above copyright notice, this list of conditions and the       *
*      following disclaimer in the documentation and/or other materials provided with the distribution.                *
*                                                                                                                      *
*    * Neither the name of the author nor the names of any contributors may be used to endorse or promote products     *
*      derived from this software without specific prior written permission.                                           *
*                                                                                                                      *
* THIS SOFTWARE IS PROVIDED BY THE AUTHORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED   *
* TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL *
* THE AUTHORS BE HELD LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES        *
* (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR       *
* BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT *
* (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE       *
* POSSIBILITY OF SUCH DAMAGE.                                                                                          *
*                                                                                                                      *
***********************************************************************************************************************/

/**
	@file
	@author Andrew D. Zonenberg
	@brief SCPI server. Control plane traffic only, no waveform data.

	SCPI commands supported:

		*IDN?
			Returns a standard SCPI instrument identification string

		CHANS?
			Returns the number of channels on the instrument.

		[1|2]D:PRESENT?
			Returns 1 = MSO pod present, 0 = MSO pod not present

		[chan]:COUP [DC1M|AC1M|DC50]
			Sets channel coupling

		[chan]:HYS [mV]
			Sets MSO channel hysteresis to mV millivolts

		[chan]:OFF
			Turns the channel off

		[chan]:OFFS [num]
			Sets channel offset to num volts

		[chan]:ON
			Turns the channel on

		[chan]:RANGE [num]
			Sets channel full-scale range to num volts

		[chan]:THRESH [mV]
			Sets MSO channel threshold to mV millivolts

		BITS [num]
			Sets ADC bit depth

		DEPTH [num]
			Sets memory depth

		DEPTHS?
			Returns the set of available memory depths

		EXIT
			Terminates the connection

		FORCE
			Forces a single acquisition

		RATE [num]
			Sets sample rate

		RATES?
			Returns a comma separated list of sampling rates (in femtoseconds)

		SINGLE
			Arms the trigger in one-shot mode

		START
			Arms the trigger

		STOP
			Disarms the trigger

		TRIG:DELAY [delay]
			Sets trigger delay (in fs)

		TRIG:EDGE:DIR [direction]
			Sets trigger direction. Legal values are RISING, FALLING, or ANY.

		TRIG:LEV [level]
			Selects trigger level (in volts)

		TRIG:SOU [chan]
			Selects the channel as the trigger source

		TODO: SetDigitalPortInteractionCallback to determine when pods are connected/removed

		AWG:DUTY [duty cycle]
			Sets duty cycle of function generator output

		AWG:FREQ [freq]
			Sets function generator frequency, in Hz

		AWG:OFF [offset]
			Sets offset of the function generator output

		AWG:RANGE [range]
			Sets p-p voltage of the function generator output

		AWG:SHAPE [waveform type]
			Sets waveform type

		AWG:START
			Starts the function generator

		AWG:STOP
			Stops the function generator
 */

#include "ps6000d.h"
#include "PicoSCPIServer.h"
#include <string.h>
#include <math.h>

#define __USE_MINGW_ANSI_STDIO 1 // Required for MSYS2 mingw64 to support format "%z" ...

#define FS_PER_SECOND 1e15

using namespace std;

//Channel state
map<size_t, bool> g_channelOn;
map<size_t, PICO_COUPLING> g_coupling;
map<size_t, PICO_CONNECT_PROBE_RANGE> g_range;
map<size_t, enPS3000ARange> g_range_3000a;
map<size_t, double> g_roundedRange;
map<size_t, double> g_offset;
map<size_t, PICO_BANDWIDTH_LIMITER> g_bandwidth;
map<size_t, size_t> g_bandwidth_legacy;
size_t g_memDepth = 1000000;
int64_t g_sampleInterval = 0;	//in fs

//Copy of state at timestamp of last arm event
map<size_t, bool> g_channelOnDuringArm;
int64_t g_sampleIntervalDuringArm = 0;
size_t g_captureMemDepth = 0;
map<size_t, double> g_offsetDuringArm;

uint32_t g_timebase = 0;

bool g_triggerArmed = false;
bool g_triggerOneShot = false;
bool g_memDepthChanged = false;

//Trigger state (for now, only simple single-channel trigger supported)
int64_t g_triggerDelay = 0;
PICO_THRESHOLD_DIRECTION g_triggerDirection = PICO_RISING;
float g_triggerVoltage = 0;
size_t g_triggerChannel = 0;
size_t g_triggerSampleIndex;

//Thresholds for MSO pods
size_t g_numDigitalPods = 2;
int16_t g_msoPodThreshold[2][8] = { {0}, {0} };
PICO_DIGITAL_PORT_HYSTERESIS g_msoHysteresis[2] = {PICO_NORMAL_100MV, PICO_NORMAL_100MV};
bool g_msoPodEnabled[2] = {false};
bool g_msoPodEnabledDuringArm[2] = {false};

bool EnableMsoPod(size_t npod);

bool g_lastTriggerWasForced = false;

std::mutex g_mutex;

//AWG config
float g_awgRange = 0;
float g_awgOffset = 0;
bool g_awgOn = false;
double g_awgFreq = 1000;
PS3000A_EXTRA_OPERATIONS g_awgPS3000AOperation = PS3000A_ES_OFF;    // Noise and PRBS generation is not a WaveType
PS3000A_WAVE_TYPE g_awgPS3000AWaveType = PS3000A_SINE;              // Waveform must be set in ReconfigAWG(), holds the WaveType;

//Struct easily allows for adding ne models
struct WaveformType
{
	PICO_WAVE_TYPE type6000;
	PS3000A_WAVE_TYPE type3000;
	PS3000A_EXTRA_OPERATIONS op3000;
};
const map<string, WaveformType> g_waveformTypes =
{
	{"SINE",      {PICO_SINE,      PS3000A_SINE,      PS3000A_ES_OFF}},
	{"SQUARE",    {PICO_SQUARE,    PS3000A_SQUARE,    PS3000A_ES_OFF}},
	{"TRIANGLE",  {PICO_TRIANGLE,  PS3000A_TRIANGLE,  PS3000A_ES_OFF}},
	{"RAMP_UP",   {PICO_RAMP_UP,   PS3000A_RAMP_UP,   PS3000A_ES_OFF}},
	{"RAMP_DOWN", {PICO_RAMP_DOWN, PS3000A_RAMP_DOWN, PS3000A_ES_OFF}},
	{"SINC",      {PICO_SINC,      PS3000A_SINC,      PS3000A_ES_OFF}},
	{"GAUSSIAN",  {PICO_GAUSSIAN,  PS3000A_GAUSSIAN,  PS3000A_ES_OFF}},
	{"HALF_SINE", {PICO_HALF_SINE, PS3000A_HALF_SINE, PS3000A_ES_OFF}},
	{"DC",        {PICO_DC_VOLTAGE,PS3000A_DC_VOLTAGE,PS3000A_ES_OFF}},
	{"WHITENOISE",{PICO_WHITENOISE,PS3000A_SINE,      PS3000A_WHITENOISE}},
	{"PRBS",      {PICO_PRBS,      PS3000A_SINE,      PS3000A_PRBS}},
	{"ARBITRARY", {PICO_ARBITRARY, PS3000A_MAX_WAVE_TYPES, PS3000A_ES_OFF}}       //FIX: PS3000A_MAX_WAVE_TYPES is used as placeholder for arbitrary generation till a better workarround is found
};

#define SIG_GEN_BUFFER_SIZE         8192                            //TODO: allow model specific/variable buffer size. Must be power of 2 for most AWGs PS3000: 2^13,  PS3207B: 2^14  PS3206B: 2^15
int16_t* g_arbitraryWaveform = new int16_t[SIG_GEN_BUFFER_SIZE];    //
void GenerateSquareWave(int16_t* &waveform, size_t bufferSize, double dutyCycle, int16_t amplitude = 32767);

void ReconfigAWG();

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Construction / destruction

PicoSCPIServer::PicoSCPIServer(ZSOCKET sock)
	: BridgeSCPIServer(sock)
{
	//external trigger is fixed range of -1 to +1V
	g_roundedRange[PICO_TRIGGER_AUX] = 2;
	g_offset[PICO_TRIGGER_AUX] = 0;
}

PicoSCPIServer::~PicoSCPIServer()
{
	LogVerbose("Client disconnected\n");

	//Disable all channels when a client disconnects to put the scope in a "safe" state
	for(auto& it : g_channelOn)
	{
		switch(g_pico_type)
		{
			case PICO3000A:
				ps3000aSetChannel(g_hScope, (PS3000A_CHANNEL)it.first, 0, PS3000A_DC, PS3000A_1V, 0.0f);
				break;
			case PICO6000A:
				ps6000aSetChannelOff(g_hScope, (PICO_CHANNEL)it.first);
				break;
		}

		it.second = false;
		g_channelOnDuringArm[it.first] = false;
	}

	for(int i=0; i<2; i++)
	{
		switch(g_pico_type)
		{
			case PICO3000A:
				ps3000aSetDigitalPort(g_hScope, (PS3000A_DIGITAL_PORT)(PICO_PORT0 + i), 0, 0);
				break;
			case PICO6000A:
				ps6000aSetDigitalPortOff(g_hScope, (PICO_CHANNEL)(PICO_PORT0 + i));
				break;
		}
		g_msoPodEnabled[i] = false;
	}
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Command parsing

bool PicoSCPIServer::OnQuery(
	const string& line,
	const string& subject,
	const string& cmd)
{
	//Extract channel ID from subject and clamp bounds
	size_t channelId = 0;
	//size_t laneId = 0;
	//bool channelIsDigital = false;
	if(isalpha(subject[0]))
	{
		channelId = min(static_cast<size_t>(subject[0] - 'A'), g_numChannels);
		//channelIsDigital = false;
	}
	else if(isdigit(subject[0]))
	{
		channelId = min(subject[0] - '0', 2) - 1;
		//channelIsDigital = true;
		//if(subject.length() >= 3)
		//	laneId = min(subject[2] - '0', 7);
	}

	if(BridgeSCPIServer::OnQuery(line, subject, cmd))
	{
		return true;
	}
	else if(cmd == "PRESENT")
	{
		lock_guard<mutex> lock(g_mutex);

		switch(g_pico_type)
		{
			case PICO3000A:
			{
				//There's no API to test for presence of a MSO pod without trying to enable it.
				//If no pod is present, this call will return an error.
				PICO_CHANNEL podId = (PICO_CHANNEL)(PICO_PORT0 + channelId);
				auto status = ps3000aSetDigitalPort(g_hScope, (PS3000A_DIGITAL_PORT)podId, 1, g_msoPodThreshold[channelId][0]);
				if(status == PICO_OK)
				{
					// The pod is here. If we don't need it on, shut it back off
					if(!g_msoPodEnabled[channelId])
						ps3000aSetDigitalPort(g_hScope, (PS3000A_DIGITAL_PORT)podId, 0, 0);

					SendReply("1");
				}
				else
				{
					SendReply("0");
				}
			}
			break;

			case PICO6000A:
			{
				//There's no API to test for presence of a MSO pod without trying to enable it.
				//If no pod is present, this call will return PICO_NO_MSO_POD_CONNECTED.
				PICO_CHANNEL podId = (PICO_CHANNEL)(PICO_PORT0 + channelId);
				auto status = ps6000aSetDigitalPortOn(
								  g_hScope,
								  podId,
								  g_msoPodThreshold[channelId],
								  8,
								  g_msoHysteresis[channelId]);

				if(status == PICO_NO_MSO_POD_CONNECTED)
				{
					SendReply("0");
				}
				else
				{
					// The pod is here. If we don't need it on, shut it back off
					if(!g_msoPodEnabled[channelId])
						ps6000aSetDigitalPortOff(g_hScope, podId);

					SendReply("1");
				}
			}
			break;
		}
	}
	else
	{
		LogDebug("Unrecognized query received: %s\n", line.c_str());
	}
	return false;
}

string PicoSCPIServer::GetMake()
{
	return "Pico Technology";
}

string PicoSCPIServer::GetModel()
{
	return g_model;
}

string PicoSCPIServer::GetSerial()
{
	return g_serial;
}

string PicoSCPIServer::GetFirmwareVersion()
{
	return g_fwver;
}

size_t PicoSCPIServer::GetAnalogChannelCount()
{
	return g_numChannels;
}

vector<size_t> PicoSCPIServer::GetSampleRates()
{
	vector<size_t> rates;

	lock_guard<mutex> lock(g_mutex);

	//Enumerate timebases
	//Don't report every single legal timebase as there's way too many, the list box would be huge!
	//Report the first nine, then go to larger steps
	size_t vec[] =
	{
		0, 1, 2, 3, 4, 5, 6, 7, 8, 9,
		14, 29, 54, 104, 129, 254, 504, 629, 1254, 2504, 3129, 5004, 6254, 15629, 31254,
		62504, 156254, 312504, 625004, 1562504
	};
	for(auto i : vec)
	{
		double intervalNs;
		int32_t intervalNs_int;
		uint64_t maxSamples;
		int32_t maxSamples_int;
		PICO_STATUS status = PICO_RESERVED_1;

		switch(g_pico_type)
		{
			case PICO3000A:
				status = ps3000aGetTimebase(g_hScope, i, 1, &intervalNs_int, 0, &maxSamples_int, 0);
				intervalNs = intervalNs_int;
				maxSamples = maxSamples_int;
				break;
			case PICO6000A:
				status = ps6000aGetTimebase(g_hScope, i, 1, &intervalNs, &maxSamples, 0);
				break;
		}

		if(PICO_OK == status)
		{
			size_t intervalFs = intervalNs * 1e6f;
			rates.push_back(FS_PER_SECOND / intervalFs);
		}
		else if(PICO_INVALID_TIMEBASE == status)
		{
			//Requested timebase not possible
			//This is common and harmless if we ask for e.g. timebase 0 when too many channels are active.
			continue;
		}
		else
			LogWarning("GetTimebase failed, code %d / 0x%x\n", status, status);
	}

	return rates;
}

vector<size_t> PicoSCPIServer::GetSampleDepths()
{
	vector<size_t> depths;

	lock_guard<mutex> lock(g_mutex);
	double intervalNs;
	int32_t intervalNs_int;
	uint64_t maxSamples;
	int32_t maxSamples_int;

	PICO_STATUS status;
	status = PICO_RESERVED_1;

	//Ask for max memory depth at timebase number 10
	//We cannot use the first few timebases because those are sometimes not available depending on channel count etc
	int ntimebase = 10;
	switch(g_pico_type)
	{
		case PICO3000A:
			status = ps3000aGetTimebase(g_hScope, ntimebase, 1, &intervalNs_int, 0, &maxSamples_int, 0);
			maxSamples = maxSamples_int;
			break;
		case PICO6000A:
			status = ps6000aGetTimebase(g_hScope, ntimebase, 1, &intervalNs, &maxSamples, 0);
			break;
	}

	if(PICO_OK == status)
	{
		//Seems like there's no restrictions on actual memory depth other than an upper bound.
		//To keep things simple, report 1-2-5 series from 10K samples up to the actual max depth

		for(size_t base = 10000; base < maxSamples; base *= 10)
		{
			const size_t muls[] = {1, 2, 5};
			for(auto m : muls)
			{
				size_t depth = m * base;
				if(depth < maxSamples)
					depths.push_back(depth);
			}
		}

		depths.push_back(maxSamples);
	}

	return depths;
}

bool PicoSCPIServer::OnCommand(
	const string& line,
	const string& subject,
	const string& cmd,
	const vector<string>& args)
{
	//Function generator is different from normal channels
	//(uses range/offs commands so must go before normal bridge processing!)
	if(subject == "AWG")
	{
		if(cmd == "START")
		{
			lock_guard<mutex> lock(g_mutex);
			g_awgOn = true;
			ReconfigAWG();
		}

		else if(cmd == "STOP")
		{
			lock_guard<mutex> lock(g_mutex);
			if(g_pico_type == PICO3000A)
			{
				/*
				 * Special handling for Pico3000A series oscilloscopes:
				 * Since PS3000A lacks a dedicated stop command for signal generation,
				 * we achieve this by:
				 * 1. Temporarily setting AWG amplitude and offset to zero
				 * 2. Switching to software trigger mode
				 * 3. Restoring original AWG settings
				 *
				 * This ensures clean signal termination without residual voltage levels.
				 */
				float tempRange = g_awgRange;
				float tempOffset = g_awgOffset;
				g_awgRange = 0;
				g_awgOffset = 0;
				ReconfigAWG();

				auto status = ps3000aSetSigGenPropertiesBuiltIn(
								  g_hScope,
								  g_awgFreq,
								  g_awgFreq,
								  0,
								  0,
								  PS3000A_SWEEP_TYPE (0),
								  1,
								  0,
								  PS3000A_SIGGEN_RISING,
								  PS3000A_SIGGEN_SOFT_TRIG,
								  0
							  );

				if(status != PICO_OK)
					LogError("ps3000aSetSigGenPropertiesBuiltIn failed, code 0x%x \n", status);

				g_awgRange = tempRange;
				g_awgOffset = tempOffset;
			}
			else
			{
				g_awgOn = false;
				ReconfigAWG();
			}
		}

		else if(args.size() == 1)
		{
			if(cmd == "FREQ")
			{
				lock_guard<mutex> lock(g_mutex);
				g_awgFreq = stof(args[0]);
				switch(g_pico_type)
				{
					case PICO3000A:
					{
						//handled by ReconfigAWG()
					}
					break;

					case PICO6000A:
					{
						auto status = ps6000aSigGenFrequency(g_hScope, g_awgFreq);
						if(status != PICO_OK)
							LogError("ps6000aSigGenFrequency failed, code 0x%x (freq=%f)\n", status, g_awgFreq);
					}
					break;
				}
				ReconfigAWG();
			}

			else if(cmd == "DUTY")
			{
				lock_guard<mutex> lock(g_mutex);
				auto duty = stof(args[0]) * 100;

				switch(g_pico_type)
				{
					case PICO3000A:
					{
						/* DutyCycle of square wave can not be controlled in ps3000a built in generator,
						Must be implemented via Arbitrary*/
						if( g_awgPS3000AWaveType  == PS3000A_SQUARE )
							GenerateSquareWave(g_arbitraryWaveform, SIG_GEN_BUFFER_SIZE, (double) duty);
						else
							LogError("PICO3000A DUTY TODO code\n");
					}
					break;

					case PICO6000A:
					{
						auto status = ps6000aSigGenWaveformDutyCycle(g_hScope, duty);
						if(status != PICO_OK)
							LogError("ps6000aSigGenWaveformDutyCycle failed, code 0x%x\n", status);

						ReconfigAWG();
					}
					break;
				}
				ReconfigAWG();
			}

			else if(cmd == "OFFS")
			{
				lock_guard<mutex> lock(g_mutex);
				g_awgOffset = stof(args[0]);

				ReconfigAWG();
			}

			else if(cmd == "RANGE")
			{
				lock_guard<mutex> lock(g_mutex);
				g_awgRange = stof(args[0]);

				ReconfigAWG();
			}

			else if(cmd == "SHAPE")
			{
				lock_guard<mutex> lock(g_mutex);

				auto waveform = g_waveformTypes.find(args[0]);
				if(waveform == g_waveformTypes.end())
				{
					LogError("Invalid waveform type: %s\n", args[0].c_str());
					return true;
				}

				if( ( (args[0] == "WHITENOISE") || (args[0] == "RPBS") )
						&& (g_pico_type == PICO3000A)
						&& (g_model[4] == 'A' ) )
				{
					LogError("Noise/RPBS generation not supported by 3xxxA Models\n");
					return true;
				}
				if( (g_awgPS3000AWaveType  == PS3000A_SQUARE) && (g_pico_type == PICO3000A) )
				{
					GenerateSquareWave(g_arbitraryWaveform, SIG_GEN_BUFFER_SIZE, 50);
				}

				g_awgPS3000AWaveType = waveform->second.type3000;
				g_awgPS3000AOperation = waveform->second.op3000;

				if(args[0] == "ARBITRARY")
				{
					//TODO: find a more flexible way to specify arb buffer
					if(g_pico_type == PICO3000A)
						LogError("PICO3000A ARBITRARY TODO code\n");
					//TODO: ReconfigAWG() can handle this already, must only fill the buffer
					if(g_pico_type == PICO6000A)
						LogError("PICO6000A ARBITRARY TODO code\n");
				}

				if(g_pico_type == PICO6000A)
				{
					auto status = ps6000aSigGenWaveform(g_hScope, waveform->second.type6000, NULL, 0);
					if(PICO_OK != status)
						LogError("ps6000aSigGenWaveform failed, code 0x%x\n", status);

				}

				ReconfigAWG();
			}
			else
				LogError("Unrecognized AWG command %s\n", line.c_str());
		}
		else
			LogError("Unrecognized AWG command %s\n", line.c_str());
	}

	else if(BridgeSCPIServer::OnCommand(line, subject, cmd, args))
		return true;

	else if( (cmd == "BITS") && (args.size() == 1) )
	{

		lock_guard<mutex> lock(g_mutex);

		if(g_pico_type != PICO6000A)
			return false;

		ps6000aStop(g_hScope);

		//Even though we didn't actually change memory, apparently calling ps6000aSetDeviceResolution
		//will invalidate the existing buffers and make ps6000aGetValues() fail with PICO_BUFFERS_NOT_SET.
		g_memDepthChanged = true;

		int bits = stoi(args[0]);
		switch(bits)
		{
			case 8:
				ps6000aSetDeviceResolution(g_hScope, PICO_DR_8BIT);
				break;

			case 10:
				ps6000aSetDeviceResolution(g_hScope, PICO_DR_10BIT);
				break;

			case 12:
				ps6000aSetDeviceResolution(g_hScope, PICO_DR_12BIT);
				break;

			default:
				LogError("User requested invalid resolution (%d bits)\n", bits);
		}

		if(g_triggerArmed)
			StartCapture(false);
	}
	//TODO: bandwidth limiter
	else
	{
		LogDebug("Unrecognized command received: %s\n", line.c_str());
		LogIndenter li;
		LogDebug("Subject: %s\n", subject.c_str());
		LogDebug("Command: %s\n", cmd.c_str());
		for(auto arg : args)
			LogDebug("Arg: %s\n", arg.c_str());
		return false;
	}

	return true;
}

/**
	@brief Reconfigures the function generator
 */
void PicoSCPIServer::ReconfigAWG()
{
	double freq = g_awgFreq;
	double inc = 0;
	double dwell = 0;

	switch(g_pico_type)
	{
		case PICO3000A:
		{
			Stop(); // Need to stop aquesition when settign the AWG to avoid "PICO_BUSY" erros
			if( g_awgPS3000AWaveType == PS3000A_SQUARE || g_awgPS3000AWaveType == PS3000A_MAX_WAVE_TYPES)
			{
				uint32_t delta= 0;
				auto status = ps3000aSigGenFrequencyToPhase(g_hScope, g_awgFreq, PS3000A_SINGLE, SIG_GEN_BUFFER_SIZE, &delta);
				if(status != PICO_OK)
					LogError("ps3000aSigGenFrequencyToPhase failed, code 0x%x\n", status);

				status =  ps3000aSetSigGenArbitrary(
							  g_hScope,
							  g_awgOffset*1e6,
							  g_awgRange*1e6*2,
							  delta,
							  delta,
							  0,
							  0,
							  g_arbitraryWaveform,
							  SIG_GEN_BUFFER_SIZE,
							  PS3000A_UP,          // sweepType
							  PS3000A_ES_OFF,      // operation
							  PS3000A_SINGLE,      // indexMode
							  PS3000A_SHOT_SWEEP_TRIGGER_CONTINUOUS_RUN,
							  0,
							  PS3000A_SIGGEN_RISING,
							  PS3000A_SIGGEN_NONE,
							  0);
				if(status != PICO_OK)
					LogError("ps3000aSetSigGenArbitrary failed, code 0x%x\n", status);
			}
			else
			{
				auto status = ps3000aSetSigGenBuiltInV2(
								  g_hScope,
								  g_awgOffset*1e6,        //Offset Voltage in µV
								  g_awgRange *1e6*2,      // Peek to Peek Range in µV
								  g_awgPS3000AWaveType,
								  freq,
								  freq,
								  inc,
								  dwell,
								  PS3000A_UP,
								  g_awgPS3000AOperation,
								  PS3000A_SHOT_SWEEP_TRIGGER_CONTINUOUS_RUN,  //run forever
								  0,  //dont use sweeps
								  PS3000A_SIGGEN_RISING,
								  PS3000A_SIGGEN_NONE,
								  0);                         // Tigger level (-32767 to 32767 -> -5 to 5 V)
				if(PICO_OK != status)
					LogError("ps3000aSetSigGenBuiltInV2 failed, code 0x%x\n", status);
			}
			if(g_triggerArmed)
				StartCapture(false);
		}
		break;

		case PICO6000A:
		{
			auto status = ps6000aSigGenRange(g_hScope, g_awgRange, g_awgOffset);
			if(PICO_OK != status)
				LogError("ps6000aSigGenRange failed, code 0x%x\n", status);

			status = ps6000aSigGenApply(
						 g_hScope,
						 g_awgOn,
						 false,		//sweep enable
						 false,		//trigger enable
						 true,		//automatic DDS sample frequency
						 false,		//do not override clock and prescale
						 &freq,
						 &freq,
						 &inc,
						 &dwell);
			if(PICO_OK != status)
				LogError("ps6000aSigGenApply failed, code 0x%x\n", status);
		}
		break;
	}
}

bool PicoSCPIServer::GetChannelID(const std::string& subject, size_t& id_out)
{
	if(subject == "EX")
	{
		id_out = PICO_TRIGGER_AUX;
		return true;
	}

	//Extract channel ID from subject and clamp bounds
	size_t channelId = 0;
	size_t laneId = 0;
	bool channelIsDigital = false;
	if(isalpha(subject[0]))
	{
		channelId = min(static_cast<size_t>(subject[0] - 'A'), g_numChannels);
		channelIsDigital = false;
	}
	else if(isdigit(subject[0]))
	{
		channelId = min(subject[0] - '0', 2) - 1;
		channelIsDigital = true;
		if(subject.length() >= 3)
			laneId = min(subject[2] - '0', 7);
	}
	else
		return false;

	//Pack channel IDs into bytes
	//Byte 0: channel / pod ID
	//Byte 1: lane ID
	//Byte 2: digital flag
	id_out = channelId;
	if(channelIsDigital)
		id_out |= 0x800000 | (laneId << 8);

	return true;
}

BridgeSCPIServer::ChannelType PicoSCPIServer::GetChannelType(size_t channel)
{
	if(channel == PICO_TRIGGER_AUX)
		return CH_EXTERNAL_TRIGGER;
	else if(channel > 0xff)
		return CH_DIGITAL;
	else
		return CH_ANALOG;
}

void PicoSCPIServer::AcquisitionStart(bool oneShot)
{
	lock_guard<mutex> lock(g_mutex);

	if(g_triggerArmed)
	{
		LogVerbose("Ignoring START command because trigger is already armed\n");
		return;
	}

	//Make sure we've got something to capture
	bool anyChannels = false;
	for(size_t i=0; i<g_numChannels; i++)
	{
		if(g_channelOn[i])
		{
			anyChannels = true;
			break;
		}
	}
	for(size_t i=0; i<g_numDigitalPods; i++)
	{
		if(g_msoPodEnabled[i])
		{
			anyChannels = true;
			break;
		}
	}

	if(!anyChannels)
	{
		LogVerbose("Ignoring START command because no channels are active\n");
		return;
	}

	//Start the capture
	StartCapture(false);
	g_triggerOneShot = oneShot;
}

void PicoSCPIServer::AcquisitionForceTrigger()
{
	lock_guard<mutex> lock(g_mutex);

	//Clear out any old trigger config
	if(g_triggerArmed)
	{
		Stop();
		g_triggerArmed = false;
	}

	UpdateTrigger(true);
	StartCapture(true, true);
}

void PicoSCPIServer::AcquisitionStop()
{
	lock_guard<mutex> lock(g_mutex);

	Stop();

	//Convert any in-progress trigger to one shot.
	//This ensures that if a waveform is halfway through being downloaded, we won't re-arm the trigger after it finishes.
	g_triggerOneShot = true;
	g_triggerArmed = false;
}

void PicoSCPIServer::SetChannelEnabled(size_t chIndex, bool enabled)
{
	lock_guard<mutex> lock(g_mutex);

	if(GetChannelType(chIndex) == CH_DIGITAL)
	{
		int podIndex = (chIndex & 0xff);
		PICO_CHANNEL podId = (PICO_CHANNEL)(PICO_PORT0 + podIndex);

		if(enabled)
		{
			switch(g_pico_type)
			{
				case PICO3000A:
				{
					auto status = ps3000aSetDigitalPort(g_hScope, (PS3000A_DIGITAL_PORT)podId, 1, g_msoPodThreshold[podIndex][0]);
					if(status != PICO_OK)
						LogError("ps3000aSetDigitalPort failed with code %x\n", status);
					else
						g_msoPodEnabled[podIndex] = true;
				}
				break;

				case PICO6000A:
				{
					auto status = ps6000aSetDigitalPortOn(
									  g_hScope,
									  podId,
									  g_msoPodThreshold[podIndex],
									  8,
									  g_msoHysteresis[podIndex]);
					if(status != PICO_OK)
						LogError("ps6000aSetDigitalPortOn failed with code %x\n", status);
					else
						g_msoPodEnabled[podIndex] = true;
				}
				break;
			}
		}
		else
		{
			switch(g_pico_type)
			{
				case PICO3000A:
				{
					auto status = ps3000aSetDigitalPort(g_hScope, (PS3000A_DIGITAL_PORT)podId, 0, 0);
					if(status != PICO_OK)
						LogError("ps3000aSetDigitalPort failed with code %x\n", status);
					else
						g_msoPodEnabled[podIndex] = false;
				}
				break;

				case PICO6000A:
				{
					auto status = ps6000aSetDigitalPortOff(g_hScope, podId);
					if(status != PICO_OK)
						LogError("ps6000aSetDigitalPortOff failed with code %x\n", status);
					else
						g_msoPodEnabled[podIndex] = false;
				}
				break;
			}
		}
	}
	else
	{
		int chId = chIndex & 0xff;
		g_channelOn[chId] = enabled;
		UpdateChannel(chId);
	}

	//We need to allocate new buffers for this channel
	g_memDepthChanged = true;
}

void PicoSCPIServer::SetAnalogCoupling(size_t chIndex, const std::string& coupling)
{
	lock_guard<mutex> lock(g_mutex);
	int channelId = chIndex & 0xff;

	if(coupling == "DC1M")
		g_coupling[channelId] = PICO_DC;
	else if(coupling == "AC1M")
		g_coupling[channelId] = PICO_AC;
	else if(coupling == "DC50")
		g_coupling[channelId] = PICO_DC_50OHM;

	UpdateChannel(channelId);
}

void PicoSCPIServer::SetAnalogRange(size_t chIndex, double range_V)
{
	lock_guard<mutex> lock(g_mutex);
	size_t channelId = chIndex & 0xff;
	auto range = range_V;

	//If 50 ohm coupling, cap hardware voltage range to 5V
	if(g_coupling[channelId] == PICO_DC_50OHM)
		range = min(range, 5.0);

	if(range > 100 && g_pico_type == PICO6000A)
	{
		g_range[channelId] = PICO_X1_PROBE_200V;
		g_roundedRange[channelId] = 200;
	}
	else if(range > 50 && g_pico_type == PICO6000A)
	{
		g_range[channelId] = PICO_X1_PROBE_100V;
		g_roundedRange[channelId] = 100;
	}
	else if(range > 20)
	{
		g_range[channelId] = PICO_X1_PROBE_50V;
		g_range_3000a[channelId] = PS3000A_50V;
		g_roundedRange[channelId] = 50;
	}
	else if(range > 10)
	{
		g_range[channelId] = PICO_X1_PROBE_20V;
		g_range_3000a[channelId] = PS3000A_20V;
		g_roundedRange[channelId] = 20;
	}
	else if(range > 5)
	{
		g_range[channelId] = PICO_X1_PROBE_10V;
		g_range_3000a[channelId] = PS3000A_10V;
		g_roundedRange[channelId] = 10;
	}
	else if(range > 2)
	{
		g_range[channelId] = PICO_X1_PROBE_5V;
		g_range_3000a[channelId] = PS3000A_5V;
		g_roundedRange[channelId] = 5;
	}
	else if(range > 1)
	{
		g_range[channelId] = PICO_X1_PROBE_2V;
		g_range_3000a[channelId] = PS3000A_2V;
		g_roundedRange[channelId] = 2;
	}
	else if(range > 0.5)
	{
		g_range[channelId] = PICO_X1_PROBE_1V;
		g_range_3000a[channelId] = PS3000A_1V;
		g_roundedRange[channelId] = 1;
	}
	else if(range > 0.2)
	{
		g_range[channelId] = PICO_X1_PROBE_500MV;
		g_range_3000a[channelId] = PS3000A_500MV;
		g_roundedRange[channelId] = 0.5;
	}
	else if(range > 0.1)
	{
		g_range[channelId] = PICO_X1_PROBE_200MV;
		g_range_3000a[channelId] = PS3000A_200MV;
		g_roundedRange[channelId] = 0.2;
	}
	else if(range >= 0.05)
	{
		g_range[channelId] = PICO_X1_PROBE_100MV;
		g_range_3000a[channelId] = PS3000A_100MV;
		g_roundedRange[channelId] = 0.1;
	}
	else if(range >= 0.02)
	{
		g_range[channelId] = PICO_X1_PROBE_50MV;
		g_range_3000a[channelId] = PS3000A_50MV;
		g_roundedRange[channelId] = 0.05;
	}
	else if(range >= 0.01)
	{
		g_range[channelId] = PICO_X1_PROBE_20MV;
		g_range_3000a[channelId] = PS3000A_20MV;
		g_roundedRange[channelId] = 0.02;
	}
	else
	{
		g_range[channelId] = PICO_X1_PROBE_10MV;
		g_range_3000a[channelId] = PS3000A_10MV;
		g_roundedRange[channelId] = 0.01;
	}

	UpdateChannel(channelId);

	//Update trigger if this is the trigger channel.
	//Trigger is digital and threshold is specified in ADC counts.
	//We want to maintain constant trigger level in volts, not ADC counts.
	if(g_triggerChannel == channelId)
		UpdateTrigger();
}

void PicoSCPIServer::SetAnalogOffset(size_t chIndex, double offset_V)
{
	lock_guard<mutex> lock(g_mutex);

	int channelId = chIndex & 0xff;

	double maxoff;
	double minoff;
	float maxoff_f;
	float minoff_f;

	//Clamp to allowed range
	switch(g_pico_type)
	{
		case PICO3000A:
			ps3000aGetAnalogueOffset(g_hScope, g_range_3000a[channelId], (PS3000A_COUPLING)g_coupling[channelId], &maxoff_f, &minoff_f);
			maxoff = maxoff_f;
			minoff = minoff_f;
			break;
		case PICO6000A:
			ps6000aGetAnalogueOffsetLimits(g_hScope, g_range[channelId], g_coupling[channelId], &maxoff, &minoff);
			break;
	}
	offset_V = min(maxoff, offset_V);
	offset_V = max(minoff, offset_V);

	g_offset[channelId] = offset_V;
	UpdateChannel(channelId);
}

void PicoSCPIServer::SetDigitalThreshold(size_t chIndex, double threshold_V)
{
	int channelId = chIndex & 0xff;
	int laneId = (chIndex >> 8) & 0xff;

	int16_t code = round( (threshold_V * 32767) / 5.0);
	g_msoPodThreshold[channelId][laneId] = code;

	LogTrace("Setting MSO pod %d lane %d threshold to %f (code %d)\n", channelId, laneId, threshold_V, code);

	lock_guard<mutex> lock(g_mutex);

	//Update the pod if currently active
	if(g_msoPodEnabled[channelId])
		EnableMsoPod(channelId);
}

void PicoSCPIServer::SetDigitalHysteresis(size_t chIndex, double hysteresis)
{
	lock_guard<mutex> lock(g_mutex);

	int channelId = chIndex & 0xff;

	//Calculate hysteresis
	int level = hysteresis;
	if(level <= 50)
		g_msoHysteresis[channelId] = PICO_LOW_50MV;
	else if(level <= 100)
		g_msoHysteresis[channelId] = PICO_NORMAL_100MV;
	else if(level <= 200)
		g_msoHysteresis[channelId] = PICO_HIGH_200MV;
	else
		g_msoHysteresis[channelId] = PICO_VERY_HIGH_400MV;

	LogTrace("Setting MSO pod %d hysteresis to %d mV (code %d)\n",
			 channelId, level, g_msoHysteresis[channelId]);

	//Update the pod if currently active
	if(g_msoPodEnabled[channelId])
		EnableMsoPod(channelId);
}

void PicoSCPIServer::SetSampleRate(uint64_t rate_hz)
{
	lock_guard<mutex> lock(g_mutex);
	int timebase;
	double period_ns;

	switch(g_pico_type)
	{
		case PICO3000A:
		{
			//Convert sample rate to sample period
			g_sampleInterval = 1e15 / rate_hz;
			period_ns = 1e9 / rate_hz;

			//Find closest timebase setting
			double clkdiv = period_ns;
			if(period_ns < 1)
				timebase = 0;
			else
				timebase = round(log(clkdiv)/log(2));
		}
		break;

		case PICO6000A:
		{
			//Convert sample rate to sample period
			g_sampleInterval = 1e15 / rate_hz;
			period_ns = 1e9 / rate_hz;

			//Find closest timebase setting
			double clkdiv = period_ns / 0.2;
			if(period_ns < 5)
				timebase = round(log(clkdiv)/log(2));
			else
				timebase = round(clkdiv/32) + 4;
		}
		break;

		default: /* Unknown Pico Type */
		{

			g_sampleInterval = 1e15 / rate_hz;
			timebase = 0;
			LogError("SetSampleRate Error unknown g_pico_type\n");
		}
	}

	g_timebase = timebase;
}

void PicoSCPIServer::SetSampleDepth(uint64_t depth)
{
	lock_guard<mutex> lock(g_mutex);
	g_memDepth = depth;

	UpdateTrigger();
}

void PicoSCPIServer::SetTriggerDelay(uint64_t delay_fs)
{
	lock_guard<mutex> lock(g_mutex);

	g_triggerDelay = delay_fs;
	UpdateTrigger();
}

void PicoSCPIServer::SetTriggerSource(size_t chIndex)
{
	lock_guard<mutex> lock(g_mutex);

	auto type = GetChannelType(chIndex);
	switch(type)
	{
		case CH_ANALOG:
			g_triggerChannel = chIndex & 0xff;
			if(!g_channelOn[g_triggerChannel])
			{
				LogDebug("Trigger channel wasn't on, enabling it\n");
				g_channelOn[g_triggerChannel] = true;
				UpdateChannel(g_triggerChannel);
			}
			break;

		case CH_DIGITAL:
		{
			int npod = chIndex & 0xff;
			int nchan = (chIndex >> 8) & 0xff;
			g_triggerChannel = g_numChannels + npod*8 + nchan;

			if(!g_msoPodEnabled[npod])
			{
				LogDebug("Trigger pod wasn't on, enabling it\n");
				EnableMsoPod(npod);
			}
		}
		break;

		case CH_EXTERNAL_TRIGGER:
		{
			g_triggerChannel = PICO_TRIGGER_AUX;
			UpdateTrigger();
		};

		default:
			//TODO
			break;
	}

	bool wasOn = g_triggerArmed;
	Stop();

	UpdateTrigger();

	if(wasOn)
		StartCapture(false);
}

void PicoSCPIServer::SetTriggerLevel(double level_V)
{
	lock_guard<mutex> lock(g_mutex);

	g_triggerVoltage = level_V;
	UpdateTrigger();
}

void PicoSCPIServer::SetTriggerTypeEdge()
{
	//all triggers are edge, nothing to do here until we start supporting other trigger types
}

bool PicoSCPIServer::IsTriggerArmed()
{
	return g_triggerArmed;
}

void PicoSCPIServer::SetEdgeTriggerEdge(const string& edge)
{
	lock_guard<mutex> lock(g_mutex);

	if(edge == "RISING")
		g_triggerDirection = PICO_RISING;
	else if(edge == "FALLING")
		g_triggerDirection = PICO_FALLING;
	else if(edge == "ANY")
		g_triggerDirection = PICO_RISING_OR_FALLING;

	UpdateTrigger();
}

/**
	@brief Pushes channel configuration to the instrument
 */
void UpdateChannel(size_t chan)
{
	switch(g_pico_type)
	{
		case PICO3000A:
		{
			ps3000aSetChannel(g_hScope, (PS3000A_CHANNEL)chan, g_channelOn[chan],
							  (PS3000A_COUPLING)g_coupling[chan], g_range_3000a[chan], -g_offset[chan]);
			ps3000aSetBandwidthFilter(g_hScope, (PS3000A_CHANNEL)chan,
									  (PS3000A_BANDWIDTH_LIMITER)g_bandwidth_legacy[chan]);

			//We use software triggering based on raw ADC codes.
			//Any time we change the frontend configuration on the trigger channel, it has to be reconfigured.
			//TODO: handle multi-input triggers
			if(chan == g_triggerChannel)
				UpdateTrigger();
			return;
		}
		break;

		case PICO6000A:
		{
			if(g_channelOn[chan])
			{
				ps6000aSetChannelOn(g_hScope, (PICO_CHANNEL)chan,
									g_coupling[chan], g_range[chan], -g_offset[chan], g_bandwidth[chan]);

				//We use software triggering based on raw ADC codes.
				//Any time we change the frontend configuration on the trigger channel, it has to be reconfigured.
				//TODO: handle multi-input triggers
				if(chan == g_triggerChannel)
					UpdateTrigger();
			}
			else
				ps6000aSetChannelOff(g_hScope, (PICO_CHANNEL)chan);
		}
		break;
	}
}

/**
	@brief Pushes trigger configuration to the instrument
 */
void UpdateTrigger(bool force)
{
	//Timeout, in microseconds, before initiating a trigger
	//Force trigger is really just a one-shot auto trigger with a 1us delay.
	uint32_t timeout = 0;
	if(force)
	{
		timeout = 1;
		g_lastTriggerWasForced = true;
	}
	else
		g_lastTriggerWasForced = false;

	bool triggerIsAnalog = (g_triggerChannel < g_numChannels) || (g_triggerChannel == PICO_TRIGGER_AUX);

	//Convert threshold from volts to ADC counts
	float offset = 0;
	if(triggerIsAnalog)
		offset = g_offset[g_triggerChannel];
	float scale = 1;
	if(triggerIsAnalog)
	{
		scale = g_roundedRange[g_triggerChannel] / 32512;
		if(scale == 0)
			scale = 1;
	}
	float trig_code = (g_triggerVoltage - offset) / scale;

	//This can happen early on during initialization.
	//Bail rather than dividing by zero.
	if(g_sampleInterval == 0)
		return;

	//Add delay before start of capture if needed
	int64_t triggerDelaySamples = g_triggerDelay / g_sampleInterval;
	uint64_t delay = 0;
	if(triggerDelaySamples < 0)
		delay = -triggerDelaySamples;

	switch(g_pico_type)
	{
		case PICO3000A:
		{
			if(g_triggerChannel == PICO_TRIGGER_AUX)
			{
				/* TODO PICO_TRIGGER_AUX PICO3000A similarly to PICO6000A... */
				/*
				//Seems external trigger only supports zero crossing???
				trig_code = 0;

				//Remove old trigger conditions
				ps6000aSetTriggerChannelConditions(
					g_hScope,
					NULL,
					0,
					PICO_CLEAR_ALL);

				//Set up new conditions
				PICO_CONDITION cond;
				cond.source = PICO_TRIGGER_AUX;
				cond.condition = PICO_CONDITION_TRUE;
				int ret = ps6000aSetTriggerChannelConditions(
					g_hScope,
					&cond,
					1,
					PICO_ADD);
				if(ret != PICO_OK)
					LogError("ps6000aSetTriggerChannelConditions failed: %x\n", ret);

				PICO_DIRECTION dir;
				dir.channel = PICO_TRIGGER_AUX;
				dir.direction = PICO_RISING;
				dir.thresholdMode = PICO_LEVEL;
				ret = ps6000aSetTriggerChannelDirections(
					g_hScope,
					&dir,
					1);
				if(ret != PICO_OK)
					LogError("ps6000aSetTriggerChannelDirections failed: %x\n", ret);

				PICO_TRIGGER_CHANNEL_PROPERTIES prop;
				prop.thresholdUpper = trig_code;
				prop.thresholdUpperHysteresis = 32;
				prop.thresholdLower = 0;
				prop.thresholdLowerHysteresis = 0;
				prop.channel = PICO_TRIGGER_AUX;
				ret = ps6000aSetTriggerChannelProperties(
					g_hScope,
					&prop,
					1,
					0,
					0);
				if(ret != PICO_OK)
					LogError("ps6000aSetTriggerChannelProperties failed: %x\n", ret);

				if(force)
					LogWarning("Force trigger doesn't currently work if trigger source is external\n");
				*/
				/* API is same as 6000a API */
				int ret = ps3000aSetSimpleTrigger(
							  g_hScope,
							  1,
							  (PS3000A_CHANNEL)PICO_TRIGGER_AUX,
							  0,
							  (enPS3000AThresholdDirection)g_triggerDirection,
							  delay,
							  timeout);
				if(ret != PICO_OK)
					LogError("ps6000aSetSimpleTrigger failed: %x\n", ret);
			}
			else if(g_triggerChannel < g_numChannels)
			{
				/* API is same as 6000a API */
				int ret = ps3000aSetSimpleTrigger(
							  g_hScope,
							  1,
							  (PS3000A_CHANNEL)g_triggerChannel,
							  round(trig_code),
							  (enPS3000AThresholdDirection)g_triggerDirection, // same as 6000a api
							  delay,
							  timeout);
				if(ret != PICO_OK)
					LogError("ps3000aSetSimpleTrigger failed: %x\n", ret);
			}
			else
			{
				/* TODO PICO3000A Trigger Digital Chan */
				LogError("PICO3000A Trigger Digital Chan code TODO\n");
				/*
				//Remove old trigger conditions
				ps6000aSetTriggerChannelConditions(
					g_hScope,
					NULL,
					0,
					PICO_CLEAR_ALL);

				//Set up new conditions
				int ntrig = g_triggerChannel - g_numChannels;
				int trigpod = ntrig / 8;
				int triglane = ntrig % 8;
				PICO_CONDITION cond;
				cond.source = static_cast<PICO_CHANNEL>(PICO_PORT0 + trigpod);
				cond.condition = PICO_CONDITION_TRUE;
				ps6000aSetTriggerChannelConditions(
					g_hScope,
					&cond,
					1,
					PICO_ADD);

				//Set up configuration on the selected channel
				PICO_DIGITAL_CHANNEL_DIRECTIONS dirs;
				dirs.channel = static_cast<PICO_PORT_DIGITAL_CHANNEL>(PICO_PORT_DIGITAL_CHANNEL0 + triglane);
				dirs.direction = PICO_DIGITAL_DIRECTION_RISING;				//TODO: configurable
				ps6000aSetTriggerDigitalPortProperties(
					g_hScope,
					cond.source,
					&dirs,
					1);

				//ps6000aSetTriggerDigitalPortProperties doesn't have a timeout!
				//Should we call ps6000aSetTriggerChannelProperties with no elements to do this?
				if(force)
					LogWarning("Force trigger doesn't currently work if trigger source is digital\n");
				*/
			}
		}
		break;

		case PICO6000A:
		{
			if(g_triggerChannel == PICO_TRIGGER_AUX)
			{
				/*
				//Seems external trigger only supports zero crossing???
				trig_code = 0;

				//Remove old trigger conditions
				ps6000aSetTriggerChannelConditions(
					g_hScope,
					NULL,
					0,
					PICO_CLEAR_ALL);

				//Set up new conditions
				PICO_CONDITION cond;
				cond.source = PICO_TRIGGER_AUX;
				cond.condition = PICO_CONDITION_TRUE;
				int ret = ps6000aSetTriggerChannelConditions(
					g_hScope,
					&cond,
					1,
					PICO_ADD);
				if(ret != PICO_OK)
					LogError("ps6000aSetTriggerChannelConditions failed: %x\n", ret);

				PICO_DIRECTION dir;
				dir.channel = PICO_TRIGGER_AUX;
				dir.direction = PICO_RISING;
				dir.thresholdMode = PICO_LEVEL;
				ret = ps6000aSetTriggerChannelDirections(
					g_hScope,
					&dir,
					1);
				if(ret != PICO_OK)
					LogError("ps6000aSetTriggerChannelDirections failed: %x\n", ret);

				PICO_TRIGGER_CHANNEL_PROPERTIES prop;
				prop.thresholdUpper = trig_code;
				prop.thresholdUpperHysteresis = 32;
				prop.thresholdLower = 0;
				prop.thresholdLowerHysteresis = 0;
				prop.channel = PICO_TRIGGER_AUX;
				ret = ps6000aSetTriggerChannelProperties(
					g_hScope,
					&prop,
					1,
					0,
					0);
				if(ret != PICO_OK)
					LogError("ps6000aSetTriggerChannelProperties failed: %x\n", ret);

				if(force)
					LogWarning("Force trigger doesn't currently work if trigger source is external\n");
				*/

				int ret = ps6000aSetSimpleTrigger(
							  g_hScope,
							  1,
							  PICO_TRIGGER_AUX,
							  0,
							  g_triggerDirection,
							  delay,
							  timeout);
				if(ret != PICO_OK)
					LogError("ps6000aSetSimpleTrigger failed: %x\n", ret);
			}
			else if(g_triggerChannel < g_numChannels)
			{
				int ret = ps6000aSetSimpleTrigger(
							  g_hScope,
							  1,
							  (PICO_CHANNEL)g_triggerChannel,
							  round(trig_code),
							  g_triggerDirection,
							  delay,
							  timeout);
				if(ret != PICO_OK)
					LogError("ps6000aSetSimpleTrigger failed: %x\n", ret);
			}
			else
			{
				//Remove old trigger conditions
				ps6000aSetTriggerChannelConditions(
					g_hScope,
					NULL,
					0,
					PICO_CLEAR_ALL);

				//Set up new conditions
				int ntrig = g_triggerChannel - g_numChannels;
				int trigpod = ntrig / 8;
				int triglane = ntrig % 8;
				PICO_CONDITION cond;
				cond.source = static_cast<PICO_CHANNEL>(PICO_PORT0 + trigpod);
				cond.condition = PICO_CONDITION_TRUE;
				ps6000aSetTriggerChannelConditions(
					g_hScope,
					&cond,
					1,
					PICO_ADD);

				//Set up configuration on the selected channel
				PICO_DIGITAL_CHANNEL_DIRECTIONS dirs;
				dirs.channel = static_cast<PICO_PORT_DIGITAL_CHANNEL>(PICO_PORT_DIGITAL_CHANNEL0 + triglane);
				dirs.direction = PICO_DIGITAL_DIRECTION_RISING;				//TODO: configurable
				ps6000aSetTriggerDigitalPortProperties(
					g_hScope,
					cond.source,
					&dirs,
					1);

				//ps6000aSetTriggerDigitalPortProperties doesn't have a timeout!
				//Should we call ps6000aSetTriggerChannelProperties with no elements to do this?
				if(force)
					LogWarning("Force trigger doesn't currently work if trigger source is digital\n");
			}
		}
		break;
	}

	if(g_triggerArmed)
		StartCapture(true);
}

void Stop()
{
	switch(g_pico_type)
	{
		case PICO3000A:
			ps3000aStop(g_hScope);
			break;

		case PICO6000A:
			ps6000aStop(g_hScope);
			break;
	}
}

PICO_STATUS StartInternal()
{
	//Calculate pre/post trigger time configuration based on trigger delay
	int64_t triggerDelaySamples = g_triggerDelay / g_sampleInterval;
	size_t nPreTrigger = min(max(triggerDelaySamples, (int64_t)0L), (int64_t)g_memDepth);
	size_t nPostTrigger = g_memDepth - nPreTrigger;
	g_triggerSampleIndex = nPreTrigger;

	switch(g_pico_type)
	{
		case PICO3000A:
			// TODO: why the 1
			return ps3000aRunBlock(g_hScope, nPreTrigger, nPostTrigger, g_timebase, 1, NULL, 0, NULL, NULL);

		case PICO6000A:
			return ps6000aRunBlock(g_hScope, nPreTrigger, nPostTrigger, g_timebase, NULL, 0, NULL, NULL);

		default:
			return PICO_OK;
	}
}

void StartCapture(bool stopFirst, bool force)
{
	//If previous trigger was forced, we need to reconfigure the trigger to be not-forced now
	if(g_lastTriggerWasForced && !force)
	{
		Stop();
		UpdateTrigger();
	}

	g_offsetDuringArm = g_offset;
	g_channelOnDuringArm = g_channelOn;
	for(size_t i=0; i<g_numDigitalPods; i++)
		g_msoPodEnabledDuringArm[i] = g_msoPodEnabled[i];
	if(g_captureMemDepth != g_memDepth)
		g_memDepthChanged = true;
	g_captureMemDepth = g_memDepth;
	g_sampleIntervalDuringArm = g_sampleInterval;

	LogTrace("StartCapture stopFirst %d memdepth %zu\n", stopFirst, g_captureMemDepth);

	PICO_STATUS status;
	status = PICO_RESERVED_1;
	if(stopFirst)
		Stop();
	status = StartInternal();

	//not sure why this happens...
	while(status == PICO_HARDWARE_CAPTURING_CALL_STOP)
	{
		//Not sure what causes this, but seems to be harmless?
		//Demote to trace for now
		LogTrace("Got PICO_HARDWARE_CAPTURING_CALL_STOP (but scope should have been stopped already)\n");

		Stop();
		status = StartInternal();
	}

	//Don't choke if we couldn't start the block
	if(status != PICO_OK)
	{
		LogWarning("psXXXXRunBlock failed, code %d / 0x%x\n", status, status);
		g_triggerArmed = false;
		return;
	}

	g_triggerArmed = true;
}

bool EnableMsoPod(size_t npod)
{
	g_msoPodEnabled[npod] = true;
	PICO_CHANNEL podId = (PICO_CHANNEL)(PICO_PORT0 + npod);

	switch(g_pico_type)
	{
		case PICO3000A:
		{
			auto status = ps3000aSetDigitalPort(g_hScope, (PS3000A_DIGITAL_PORT)podId, 1, g_msoPodThreshold[npod][0]);
			if(status != PICO_OK)
			{
				LogError("ps3000aSetDigitalPort failed with code %x\n", status);
				return false;
			}
		}
		break;

		case PICO6000A:
		{
			auto status = ps6000aSetDigitalPortOn(
							  g_hScope,
							  podId,
							  g_msoPodThreshold[npod],
							  8,
							  g_msoHysteresis[npod]);
			if(status != PICO_OK)
			{
				LogError("ps6000aSetDigitalPortOn failed with code %x\n", status);
				return false;
			}
		}
		break;
	}
	return true;
}

void GenerateSquareWave(int16_t* &waveform, size_t bufferSize, double dutyCycle, int16_t amplitude)
{
	// Validate inputs
	if (!waveform || bufferSize == 0)
	{
		LogError("GenerateSquareWave has Invalid input \n");
	}

	// Calculate number of high samples based on duty cycle
	size_t highSamples = static_cast<size_t>(bufferSize * (dutyCycle / 100.0));

	// Generate square wave
	for (size_t i = 0; i < bufferSize; i++)
	{
		if (i < highSamples)
		{
			waveform[i] = amplitude;     // High level
		}
		else
		{
			waveform[i] = -amplitude;    // Low level
		}
	}
}
