#pragma once

// std includes
#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <numeric>
#include <string>
#include <thread>
#include <chrono>
#include <random>

// 3rd party includes
#include <readerwriterqueue.h>
#include <spdlog/spdlog.h>

// my includes
#include "file_helpers.h"
#include "caen_helper.h"
#include "implot_helpers.h"
#include "include/caen_helper.h"
#include "include/timing_events.h"
#include "indicators.h"
#include "timing_events.h"

namespace SBCQueens {

	enum class CAENInterfaceStates {
		NullState = 0,
		Standby,
		AttemptConnection,
		OscilloscopeMode,
		StatisticsMode,
		RunMode,
		Disconnected,
		Closing
	};

	struct CAENInterfaceState {

		std::string RunDir = "";
		std::string RunName =  "";
		std::string SiPMParameters = "";

		CAENDigitizerModel Model;
		CAENGlobalConfig GlobalConfig;
		CAENChannelConfig ChannelConfig;

		int PortNum = 0;

		CAEN Port = nullptr;
		CAENInterfaceStates CurrentState =
			CAENInterfaceStates::NullState;

	};

	using CAENQueueType 
		= std::function < bool(CAENInterfaceState&) >;

	using CAENQueue 
		= moodycamel::ReaderWriterQueue< CAENQueueType >;

	template<typename... Queues>
	class CAENDigitizerInterface {

		std::tuple<Queues&...> _queues;
		CAENInterfaceState state_of_everything;

		DataFile<CAENEvent> _pulseFile;

		IndicatorSender<IndicatorNames> _plotSender;

		//tmp stuff
		uint16_t* data;
		size_t length;
		double* x_values, *y_values;
		CAENEvent osc_event, adj_osc_event;
		// 1024 because the caen can only hold 1024 no matter what model (so far)
		CAENEvent processing_evts[1024];

		// As long as we make the Func template argument a std::fuction
		// we can then use pointer magic to initialize them
		using CAENInterfaceState
			= BlockingTotalTimeEvent<
				std::function<bool(void)>,
				std::chrono::milliseconds
			>;

		// We need to turn the class BlockingTotalTimeEvent into a pointer
		// because it does not have an empty constructor and for a good reason
		// if its empty, bugs and crashes would happen all the time.
		using CAENInterfaceState_ptr = std::shared_ptr<CAENInterfaceState>;

		CAENInterfaceState_ptr main_loop_state;

		CAENInterfaceState_ptr standby_state;
		CAENInterfaceState_ptr attemptConnection_state;
		CAENInterfaceState_ptr oscilloscopeMode_state;
		CAENInterfaceState_ptr statisticsMode_state;
		CAENInterfaceState_ptr runMode_state;
		CAENInterfaceState_ptr disconnected_state;
		CAENInterfaceState_ptr closing_state;

public:
		explicit CAENDigitizerInterface(Queues&... queues) : 
			_queues(forward_as_tuple(queues...)),
			_plotSender(std::get<SiPMsPlotQueue&>(_queues)) {
			// This is possible because std::function can be assigned
			// to whatever std::bind returns
			standby_state = std::make_shared<CAENInterfaceState>(
				std::chrono::milliseconds(100),
				std::bind(&CAENDigitizerInterface::standby, this)
			);

			attemptConnection_state = std::make_shared<CAENInterfaceState>(
				std::chrono::milliseconds(1),
				std::bind(&CAENDigitizerInterface::attempt_connection, this)
			);

			oscilloscopeMode_state = std::make_shared<CAENInterfaceState>(
				std::chrono::milliseconds(100),
				std::bind(&CAENDigitizerInterface::oscilloscope, this)
			);

			statisticsMode_state = std::make_shared<CAENInterfaceState>(
				std::chrono::milliseconds(1),
				std::bind(&CAENDigitizerInterface::statistics_mode, this)
			);

			runMode_state = std::make_shared<CAENInterfaceState>(
				std::chrono::milliseconds(1),
				std::bind(&CAENDigitizerInterface::run_mode, this)
			);

			disconnected_state = std::make_shared<CAENInterfaceState>(
				std::chrono::milliseconds(1),
				std::bind(&CAENDigitizerInterface::disconnected_mode, this)
			);

			// auto g =
			closing_state = std::make_shared<CAENInterfaceState>(
				std::chrono::milliseconds(1),
				std::bind(&CAENDigitizerInterface::closing_mode, this)
			);
		}

		// No copying
		CAENDigitizerInterface(const CAENDigitizerInterface&) = delete;

		~CAENDigitizerInterface() {}

		void operator()() {

			spdlog::info("Initializing CAEN thread");

			main_loop_state = standby_state;

			// Actual loop!
			while(main_loop_state->operator()());

		}

private:
		void processing() {
			CAEN& port = state_of_everything.Port;

			static auto extract_for_gui = [&]() {
				if(port->Data.DataSize <= 0) {
					return;
				}

				// For the GUI
				std::default_random_engine generator;
				std::uniform_int_distribution<uint32_t>
					distribution(0,port->Data.NumEvents);
				uint32_t rdm_num = distribution(generator);

				extract_event(port, rdm_num, osc_event);

				auto buf = osc_event->Data->DataChannel[0];
				auto size = osc_event->Data->ChSize[0];

				x_values = new double[size];
				y_values = new double[size];

				for(uint32_t i = 0; i < size; i++) {
					x_values[i] = i*(1.0/port->GetSampleRate())*(1e9);
					y_values[i] = static_cast<double>(buf[i]);
				}

				_plotSender(IndicatorNames::SiPM_Plot,
					x_values,
					y_values,
					size);

				delete x_values;
				delete y_values;
			};

			static auto process_events = [&]() {
				bool isData = retrieve_data_until_n_events(port, 512);

				// While all of this is happening, the digitizer is taking data
				if(!isData) {
					// spdlog::info("Not enough events in buffer");
					return;
				}

				//double frequency = 0.0;
				for(uint32_t i = 0; i < port->Data.NumEvents; i++) {

				// 	// Extract event i
					extract_event(port, i, processing_evts[i]);

				// 	// Process events!
				// 	//frequency += extract_frequency(processing_evts[i]);
					if(_pulseFile){
						_pulseFile->Add(processing_evts[i]);
					}
				}

				//frequency /= port->Data.NumEvents;
				//_plotSender(IndicatorNames::FREQUENCY, frequency);
			};

			static auto extract_for_gui_nb = make_total_timed_event(
				std::chrono::milliseconds(200),
				extract_for_gui
			);

			static auto checkerror = make_total_timed_event(
				std::chrono::seconds(1),
				std::bind(&CAENDigitizerInterface::lec, this)
			);


			process_events();
			checkerror();
			extract_for_gui_nb();
			//extract_for_gui_nb();

		}

		double extract_frequency(CAENEvent& evt) {
			auto buf = evt->Data->DataChannel[0];
			auto size = evt->Data->ChSize[0];

			double counter = 0;
			auto offset =
				state_of_everything.Port->ChannelConfigs[0].TriggerThreshold;
			for(uint32_t i = 1; i < size; i++) {
				if((buf[i] > offset) && (buf[i-1] < offset)) {
					counter++;
				}
			}

			return state_of_everything.Port->GetSampleRate()*counter /
				(state_of_everything.Port->GlobalConfig.RecordLength);
		}
		
		// Local error checking. Checks if there was an error and prints it
		// using spdlog
		bool lec() {
			return check_error(state_of_everything.Port,
				[](const std::string& cmd) {
					spdlog::error(cmd);
			});
		}

		void switch_state(const CAENInterfaceStates& newState) {
			state_of_everything.CurrentState = newState;
			switch(state_of_everything.CurrentState) {
				case CAENInterfaceStates::Standby:
					main_loop_state = standby_state;
				break;

				case CAENInterfaceStates::AttemptConnection:
					main_loop_state = attemptConnection_state;
				break;

				case CAENInterfaceStates::OscilloscopeMode:
					main_loop_state = oscilloscopeMode_state;
				break;

				case CAENInterfaceStates::StatisticsMode:
					main_loop_state = statisticsMode_state;
				break;

				case CAENInterfaceStates::RunMode:
					main_loop_state = runMode_state;
				break;

				case CAENInterfaceStates::Disconnected:
					main_loop_state = disconnected_state;
				break;

				case CAENInterfaceStates::Closing:
					main_loop_state = closing_state;
				break;

				case CAENInterfaceStates::NullState:
				default:
					// do nothing other than set to standby state
					main_loop_state = standby_state;
			}
		}

		// Envelopes the logic that listens to an external queue
		// that can the data inside this thread.
		bool change_state() {
			// GUI -> CAEN
			CAENQueue& guiQueueOut = std::get<CAENQueue&>(_queues);
			CAENQueueType task;

			// If the queue does not return a valid function, this call will
			// do nothing and should return true always.
			// The tasks are essentially any GUI driven modification, example
			// setting the PID setpoints or constants
			// or an user driven reset
			if(guiQueueOut.try_dequeue(task)) {
				if(!task(state_of_everything)) {
					spdlog::warn("Something went wrong with a command!");
				} else {
					switch_state(state_of_everything.CurrentState);
					return true;
				}
			}

			return false;

		}

		// Does nothing other than wait 100ms to avoid clogging PC resources.
		bool standby() {
			change_state();

			return true;
		}

		// Attempts a connection to the CAEN digitizer, setups the channels
		// and starts acquisition
		bool attempt_connection() {
			CAEN& port = state_of_everything.Port;
			auto err = connect_usb(port,
				state_of_everything.Model,
				state_of_everything.PortNum);

			// If port resource was not created, it equals a failure!
			if(!port) {
				// Print what was the error
				check_error(err, [](const std::string& cmd) {
					spdlog::error(cmd);
				});

				// We disconnect because this will free resources (in case
				// they were created)
				disconnect(state_of_everything.Port);
				switch_state(CAENInterfaceStates::Standby);
				return true;
			}

			spdlog::info("Connected to CAEN Digitizer!");

			reset(port);
			setup(port,
				state_of_everything.GlobalConfig,
				{state_of_everything.ChannelConfig});


			enable_acquisition(port);

			// Allocate memory for events
			osc_event = std::make_shared<caenEvent>(port->Handle);

			for(CAENEvent& evt : processing_evts) {
				evt = std::make_shared<caenEvent>(port->Handle);
			}

			auto failed = lec();

			if(failed) {
				spdlog::warn("Failed to setup CAEN");
				err = disconnect(state_of_everything.Port);
				check_error(err, [](const std::string& cmd) {
					spdlog::error(cmd);
				});

				switch_state(CAENInterfaceStates::Standby);

			} else {
				spdlog::info("CAEN Setup complete!");
				switch_state(CAENInterfaceStates::OscilloscopeMode);
			}

			change_state();

			return true;
		}

		// While in this state it shares the data with the GUI but
		// no actual file saving is happening. It essentially serves
		// as a mode in where the user can see what is happening.
		// Similar to an oscilloscope
		bool oscilloscope() {

			CAEN& port = state_of_everything.Port;

			auto events = get_events_in_buffer(port);
			_plotSender(IndicatorNames::CAENBUFFEREVENTS, events);

			retrieve_data(port);

			if(port->Data.DataSize > 0 && port->Data.NumEvents > 0) {

				// spdlog::info("Total size buffer: {0}",  port->Data.TotalSizeBuffer);
				// spdlog::info("Data size: {0}", port->Data.DataSize);
				// spdlog::info("Num events: {0}",  port->Data.NumEvents);

				extract_event(port, 0, osc_event);
				// spdlog::info("Event size: {0}", osc_event->Info.EventSize);
				// spdlog::info("Event counter: {0}", osc_event->Info.EventCounter);
				// spdlog::info("Trigger Time Tag: {0}", osc_event->Info.TriggerTimeTag);

				auto buf = osc_event->Data->DataChannel[0];
				auto size = osc_event->Data->ChSize[0];

				x_values = new double[size];
				y_values = new double[size];

				for(uint32_t i = 0; i < size; i++) {
					x_values[i] = i*(1.0/port->GetSampleRate())*(1e9);
					y_values[i] = static_cast<double>(buf[i]);
				}

				_plotSender(IndicatorNames::SiPM_Plot,
					x_values,
					y_values,
					size);

				delete x_values;
				delete y_values;

				// if(port->Data.NumEvents >= 2) {
				// 	extract_event(port, 1, adj_osc_event);

				// 	auto t0 = osc_event->Info.TriggerTimeTag;
				// 	auto t1 = adj_osc_event->Info.TriggerTimeTag;

				// 	spdlog::info("Time difference between events: {0}", t1 - t0);
				// }


			}
			// Clear events in buffer
			clear_data(port);

			lec();
			change_state();
			return true;
		}

		// Does the SiPM pulse processing but no file saving
		// is done. Serves more for the user to know
		// how things are changing.
		bool statistics_mode() {
			processing();
			change_state();
			return true;
		}

		bool run_mode() {
			static bool isFileOpen = false;


			if(isFileOpen) {
				// spdlog::info("Saving SIPM data");
				save(_pulseFile, sbc_save_func);
			} else {
				open(_pulseFile,
					state_of_everything.RunDir
					+ "/" + state_of_everything.RunName
					+ "/" + state_of_everything.SiPMParameters + ".txt",

					// sbc_init_file is a function that saves the header
					// of the sbc data format as a function of record length
					// and number of channels
					sbc_init_file,
					state_of_everything.Port);
				isFileOpen = _pulseFile > 0;
			}

			processing();
			change_state();
			return true;
		}

		bool disconnected_mode() {
			spdlog::warn("Manually losing connection to the "
							"CAEN digitizer.");

			for(CAENEvent& evt : processing_evts) {
				evt.reset();
			}

			osc_event.reset();
			adj_osc_event.reset();

			auto err = disconnect(state_of_everything.Port);
			check_error(err, [](const std::string& cmd) {
				spdlog::error(cmd);
			});

			switch_state(CAENInterfaceStates::Standby);
			return true;
		}

		bool closing_mode() {
			spdlog::info("Going to close the CAEN thread.");
			for(CAENEvent& evt : processing_evts) {
				evt.reset();
			}

			osc_event.reset();
			adj_osc_event.reset();

			auto err = disconnect(state_of_everything.Port);
			check_error(err, [](const std::string& cmd) {
				spdlog::error(cmd);
			});

			return false;
		}
	};
} // namespace SBCQueens