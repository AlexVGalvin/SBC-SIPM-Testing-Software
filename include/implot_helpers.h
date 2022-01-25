#pragma once

// std includes
#include <algorithm>
#include <ios>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>
#include <sstream>
#include <deque>

// 3rd party includes
#include <imgui.h>
#include <implot.h>
#include <spdlog/spdlog.h>
#include <readerwriterqueue.h>
#include <concurrentqueue.h>

// my includes


namespace SBCQueens {

	template<typename T, typename DATA = double>
	class IndicatorReceiver;
	template<typename T, typename DATA = double>
	class IndicatorSender;

	// Well, I will leave this as this until I can come up with 
	// a better solution.
	// ImPlots are 2D by default. X, Y are the types of those two
	// axis which by default are float but they can be changed.
	// unsigned int is a number that instructs the consumer
	// to which graph is should go to.
	// T is reserved for the user, it could be an enum type (my preference)
	// or anything they want to distinguish them.
	template<typename T, typename DATA = double>
	struct IndicatorVector {

		using type = T;
		using data_type = DATA;

		IndicatorVector() {}
		IndicatorVector(const T& id, const DATA& x, const DATA& y)
			: ID(id), x(x), y(y) {}
		IndicatorVector(const T& id, const DATA& x)
			: ID(id), x(x) {}

		T ID;
		DATA x;
		DATA y;
	};

	template<typename T, typename DATA = double>
	using IndicatorsQueue
		= moodycamel::ConcurrentQueue< IndicatorVector<T, DATA> >;

	struct PlotFlags {

		bool ClearOnNewData;

	};

	enum class NumericFormat {
		Default, Scientific, HexFloat, Fixed
	};

	template<typename T>
	class Indicator {

		std::string _imgui_stack;
		std::string _display;
		unsigned int _precision;
		NumericFormat _format;

protected:

		T ID;

public:

		Indicator() {}

		explicit Indicator(T&& id,
			unsigned int&& precision = 6,
			NumericFormat&& format = NumericFormat::Default)
			: 	_imgui_stack("##" + std::to_string(static_cast<int>(id))),
				_display(_imgui_stack), _precision(precision), _format(format),
				ID(id) {

		}

		// Moving allowed
		Indicator(Indicator&&) = default;
		Indicator& operator=(Indicator&&) = default;

		// No copying
		Indicator(const Indicator&) = delete;

		virtual ~Indicator() {}

		template<typename OFFDATA>
		// Adds element newVal.x to show in this indicator. newVal.y is ignored
		void add(const IndicatorVector<T, OFFDATA>& newVal) {

			if(newVal.ID == ID) {

				std::ostringstream out;
				out.precision(_precision);
				if (_format == NumericFormat::Scientific) {
					out << std::scientific;
				} else if (_format == NumericFormat::HexFloat) {
					out << std::hexfloat;
				} else if (_format == NumericFormat::Fixed) {
					out << std::fixed;
				}

				out << newVal.x;
				_display = "";

				if(!out.str().empty()) {
					_display = out.str();
				}

				_display += _imgui_stack;

			}
		}

		virtual void operator()() {
			ImGui::Button(_display.c_str());
		}

		// For indicators, it does nothing.
		virtual void ExecuteAttributes() {

		}

		// For indicators, it does nothing.
		virtual void ClearAttributes() {

		}

		virtual T GetID() const {
			return ID;
		}

	};

	template<typename T>
	// A plot indicator. It inherits from indicator but it is really to keep
	// the code tight and clear but it was really not necessary.
	// Data is the type of the internal arrays but ImPlot turns everything
	// into a double internally. x and y have to be the same type.
	class Plot : public Indicator<T> {

		std::vector< double > _x;
		std::vector< double > _y;

public:
		using type = T;
		using data_type = double;

		bool ClearOnNewData = false;
		bool Cleared = false;

		Plot() { }

		explicit Plot(T&& id)
			: Indicator<T>(std::forward<T>(id)) {
		}

		// No moving
		Plot(Plot&&) = default;
		Plot& operator=(Plot&&) = default;

		// No copying
		Plot(const Plot&) = delete;

		template<typename OFFDATA>
		void add(const IndicatorVector<T, OFFDATA>& v) {

			if(v.ID == Indicator<T>::ID){
				_x.push_back( static_cast<double>(v.x) );
				_y.push_back( static_cast<double>(v.y) );
			}

		}

		template<typename OFFDATA>
		void add(const OFFDATA& x, const OFFDATA& y) {
			_x.push_back( static_cast<double>(x) );
			_y.push_back( static_cast<double>(y) );
		}

		void clear() {
			_x.clear();
			_y.clear();
		}

		// Returns the start of the X values
		auto begin() const {
			_x.begin();
		}

		// Returns the end of the X values
		auto end() const {
			_x.begin();
		}

		// Executes attributes. For plots, it clears on new data.
		void ExecuteAttributes() {
			if(ClearOnNewData) {
				if(!Cleared) {

					clear();
					Cleared = true;
				}
			}
		}

		// Clears attributes. For plots, it resets if it should clear.
		// on new data.
		void ClearAttributes() {
			Cleared = false;
		}

		// Wraps ImPlot::PlotLine
		void operator()(const std::string& label) {
			ImPlot::PlotLine(label.c_str(), &_x.front(), &_y.front(), _x.size());
		}

	};


	template<typename T, typename DATA>
	class IndicatorReceiver  {

		IndicatorsQueue<T, DATA>& _q;
		std::unordered_map<T, Indicator<T>&> _indicators;

public:
		using type = T;
		using data_type = DATA;

		explicit IndicatorReceiver(IndicatorsQueue<T, DATA>& q) : _q(q) { }

		// No copying
		IndicatorReceiver(const IndicatorReceiver&) = delete;

		// Add the plot, p, by reference
		// void add(Indicator<T>& p) {
		// 	_indicators.insert({p.GetID(), 
		// 		std::make_unique<Indicator<T>>(p)
		// 	});
		// }

		void add(Indicator<T>& p) {
			_indicators.insert({p.GetID(), p});
		}

		// This is meant to be run in the single consumer.
		// Updates all the plots arrays from the queue. 
		void operator()() {

			auto approx_length = _q.size_approx();
			std::vector< IndicatorVector<T, DATA> > temp_items(approx_length);
			_q.try_dequeue_bulk(temp_items.data(), approx_length);

			for(IndicatorVector<T, DATA> item : temp_items) {

				Indicator<T>& indicator = _indicators.at(item.ID);
				indicator.ExecuteAttributes();

				auto pl = dynamic_cast<Plot<T>*>(&indicator);
				if(pl) {
					pl->add(item);
				} else {
					indicator.add(item);
				}

			}

			for(auto indicator : _indicators) {
				indicator.second.ClearAttributes();
			}

		}

		void operator()(const T& type, const DATA& x, const DATA& y) {
			IndicatorVector<T, DATA> newP(type, x, y);
			_q.enqueue(newP);
		}

	};


	template<typename T, typename DATA>
	class IndicatorSender  {

		IndicatorsQueue<T, DATA>& _q;

public:
		using type = T;
		using data_type = DATA;

		explicit IndicatorSender(IndicatorsQueue<T, DATA>& q) : _q(q) { }

		// No copying
		IndicatorSender(const IndicatorSender&) = delete;

		// Sends value x to indicator of type T
		void operator()(const T& type, const DATA& x) {
			IndicatorVector<T, DATA> newP(type, x);
			_q.enqueue(newP);
		}

		// Sends value pair (x,y) to plot/indicator of type T
		void operator()(const T& type, const DATA& x, const DATA& y) {
			IndicatorVector<T, DATA> newP(type, x, y);
			_q.enqueue(newP);
		}

		// Sends value pair (x,y) to plot/indicator of type T
		// Note if an indicator is selected, only the latest value
		// will be shown
		void operator()(const IndicatorVector<T, DATA>& pv) {
			_q.enqueue(pv);
		}

		// Sends a list of pairs (x,y) to plot/indicator of type T
		// Note if an indicator is selected, only the latest value
		// will be shown and the list y will be ignored
		template<typename OFFDATA>
		void operator()(const T& type, const std::vector<OFFDATA>& x,
			const std::vector<OFFDATA>& y) {

			std::vector< IndicatorVector<T, DATA> > items(x.size());
			for(unsigned int i = 0; i < x.size(); i++ ) {
				items.emplace(i, type, 
					static_cast<DATA>(x[i]), 
					static_cast<DATA>(y[i]));
			}

			_q.enqueue_bulk(items.begin(), items.size());
		}

		template<typename OFFDATA>
		// Sends a list of pairs (x,y) to plot/indicator of type T
		// Note if an indicator is selected, only the latest value
		// will be shown and the list y will be ignored
		void operator()(const T& type,
			OFFDATA* x_data,
			OFFDATA* y_data,
			const size_t& size) {

			std::vector< IndicatorVector<T, DATA> > items(size);
			for(unsigned int i = 0; i < size; i++ ) {
				items[i] = IndicatorVector<T, DATA>(type, 
					static_cast<DATA>(x_data[i]), 
					static_cast<DATA>(y_data[i]));
			}

			_q.enqueue_bulk(items.begin(), items.size());

		}

	};


	template<typename T>
	// Creates indicator with Key and adds it to Receiver q
	inline Indicator<T> make_indicator(T&& key, unsigned int&& precision = 6,
			NumericFormat&& format = NumericFormat::Default) {
		return Indicator<T>(std::forward<T>(key),
			std::forward<unsigned int>(precision),
			std::forward<NumericFormat>(format));
	}

	template<typename T>
	inline Plot<T> make_plot(T&& key) {
		return Plot<T>(std::forward<T>(key));
	}


} // namespace SBCQueens