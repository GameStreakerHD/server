/*
* Copyright (c) 2011 Sveriges Television AB <info@casparcg.com>
*
* This file is part of CasparCG (www.casparcg.com).
*
* CasparCG is free software: you can redistribute it and/or modify
* it under the terms of the GNU General Public License as published by
* the Free Software Foundation, either version 3 of the License, or
* (at your option) any later version.
*
* CasparCG is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with CasparCG. If not, see <http://www.gnu.org/licenses/>.
*
* Author: Robert Nagy, ronag89@gmail.com
*/

#include "../StdAfx.h"
 
#include "decklink_consumer.h"

#include "../util/util.h"

#include "../decklink_api.h"

#include <core/frame/frame.h>
#include <core/mixer/audio/audio_mixer.h>

#include <common/executor.h>
#include <common/lock.h>
#include <common/diagnostics/graph.h>
#include <common/except.h>
#include <common/memshfl.h>
#include <common/array.h>
#include <common/future.h>
#include <common/cache_aligned_vector.h>

#include <core/consumer/frame_consumer.h>
#include <core/diagnostics/call_context.h>

#include <tbb/concurrent_queue.h>

#include <common/assert.h>
#include <boost/lexical_cast.hpp>
#include <boost/circular_buffer.hpp>
#include <boost/timer.hpp>
#include <boost/property_tree/ptree.hpp>

namespace caspar { namespace decklink { 
	
struct configuration
{
	enum class keyer_t
	{
		internal_keyer,
		external_keyer,
		default_keyer
	};

	enum class latency_t
	{
		low_latency,
		normal_latency,
		default_latency
	};

	int			device_index		= 1;
	bool		embedded_audio		= true;
	keyer_t		keyer				= keyer_t::default_keyer;
	latency_t	latency				= latency_t::default_latency;
	bool		key_only			= false;
	int			base_buffer_depth	= 3;
	
	int buffer_depth() const
	{
		return base_buffer_depth + (latency == latency_t::low_latency ? 0 : 1) + (embedded_audio ? 1 : 0);
	}
};

class decklink_frame : public IDeckLinkVideoFrame
{
	tbb::atomic<int>				ref_count_;
	core::const_frame				frame_;
	const core::video_format_desc	format_desc_;

	const bool						key_only_;
	cache_aligned_vector<uint8_t>	data_;
public:
	decklink_frame(core::const_frame frame, const core::video_format_desc& format_desc, bool key_only)
		: frame_(frame)
		, format_desc_(format_desc)
		, key_only_(key_only)
	{
		ref_count_ = 0;
	}
	
	// IUnknown

	virtual HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, LPVOID*)
	{
		return E_NOINTERFACE;
	}
	
	virtual ULONG STDMETHODCALLTYPE AddRef()
	{
		return ++ref_count_;
	}

	virtual ULONG STDMETHODCALLTYPE Release()
	{
		if(--ref_count_ == 0)
			delete this;
		return ref_count_;
	}

	// IDecklinkVideoFrame

	virtual long STDMETHODCALLTYPE GetWidth()                   {return static_cast<long>(format_desc_.width);}
	virtual long STDMETHODCALLTYPE GetHeight()                  {return static_cast<long>(format_desc_.height);}
	virtual long STDMETHODCALLTYPE GetRowBytes()                {return static_cast<long>(format_desc_.width*4);}
	virtual BMDPixelFormat STDMETHODCALLTYPE GetPixelFormat()   {return bmdFormat8BitBGRA;}
	virtual BMDFrameFlags STDMETHODCALLTYPE GetFlags()			{return bmdFrameFlagDefault;}
		
	virtual HRESULT STDMETHODCALLTYPE GetBytes(void** buffer)
	{
		try
		{
			if(static_cast<int>(frame_.image_data().size()) != format_desc_.size)
			{
				data_.resize(format_desc_.size, 0);
				*buffer = data_.data();
			}
			else if(key_only_)
			{
				if(data_.empty())
				{
					data_.resize(frame_.image_data().size());
					aligned_memshfl(data_.data(), frame_.image_data().begin(), frame_.image_data().size(), 0x0F0F0F0F, 0x0B0B0B0B, 0x07070707, 0x03030303);
				}
				*buffer = data_.data();
			}
			else
				*buffer = const_cast<uint8_t*>(frame_.image_data().begin());
		}
		catch(...)
		{
			CASPAR_LOG_CURRENT_EXCEPTION();
			return E_FAIL;
		}

		return S_OK;
	}
		
	virtual HRESULT STDMETHODCALLTYPE GetTimecode(BMDTimecodeFormat format, IDeckLinkTimecode** timecode) {return S_FALSE;}
	virtual HRESULT STDMETHODCALLTYPE GetAncillaryData(IDeckLinkVideoFrameAncillary** ancillary)		  {return S_FALSE;}

	// decklink_frame	

	const core::audio_buffer& audio_data()
	{
		return frame_.audio_data();
	}
};

struct decklink_consumer : public IDeckLinkVideoOutputCallback, public IDeckLinkAudioOutputCallback, boost::noncopyable
{		
	const int											channel_index_;
	const configuration									config_;

	com_ptr<IDeckLink>									decklink_							= get_device(config_.device_index);
	com_iface_ptr<IDeckLinkOutput>						output_								= iface_cast<IDeckLinkOutput>(decklink_);
	com_iface_ptr<IDeckLinkConfiguration>				configuration_						= iface_cast<IDeckLinkConfiguration>(decklink_);
	com_iface_ptr<IDeckLinkKeyer>						keyer_								= iface_cast<IDeckLinkKeyer>(decklink_);
	com_iface_ptr<IDeckLinkAttributes>					attributes_							= iface_cast<IDeckLinkAttributes>(decklink_);

	tbb::spin_mutex                                     exception_mutex_;
	std::exception_ptr                                  exception_;

	tbb::atomic<bool>                                   is_running_;
		
	const std::wstring                                  model_name_							= get_model_name(decklink_);
	const core::video_format_desc                       format_desc_;
	const int                                           buffer_size_						= config_.buffer_depth(); // Minimum buffer-size 3.

	long long                                           video_scheduled_					= 0;
	long long                                           audio_scheduled_					= 0;

	int                                                 preroll_count_						= 0;
		
	boost::circular_buffer<std::vector<int32_t>>        audio_container_		{ buffer_size_ + 1 };

	tbb::concurrent_bounded_queue<core::const_frame>    video_frame_buffer_;
	tbb::concurrent_bounded_queue<core::const_frame>    audio_frame_buffer_;
	
	spl::shared_ptr<diagnostics::graph>                 graph_;
	boost::timer                                        tick_timer_;
	retry_task<bool>                                    send_completion_;

public:
	decklink_consumer(const configuration& config, const core::video_format_desc& format_desc, int channel_index) 
		: channel_index_(channel_index)
		, config_(config)
		, format_desc_(format_desc)
	{
		is_running_ = true;
				
		video_frame_buffer_.set_capacity(1);

		// Blackmagic calls RenderAudioSamples() 50 times per second
		// regardless of video mode so we sometimes need to give them
		// samples from 2 frames in order to keep up
		audio_frame_buffer_.set_capacity((format_desc.fps > 50.0) ? 2 : 1);

		graph_->set_color("tick-time", diagnostics::color(0.0f, 0.6f, 0.9f));	
		graph_->set_color("late-frame", diagnostics::color(0.6f, 0.3f, 0.3f));
		graph_->set_color("dropped-frame", diagnostics::color(0.3f, 0.6f, 0.3f));
		graph_->set_color("flushed-frame", diagnostics::color(0.4f, 0.3f, 0.8f));
		graph_->set_color("buffered-audio", diagnostics::color(0.9f, 0.9f, 0.5f));
		graph_->set_color("buffered-video", diagnostics::color(0.2f, 0.9f, 0.9f));
		graph_->set_text(print());
		diagnostics::register_graph(graph_);
		
		enable_video(get_display_mode(output_, format_desc_.format, bmdFormat8BitBGRA, bmdVideoOutputFlagDefault));
				
		if(config.embedded_audio)
			enable_audio();
		
		set_latency(config.latency);				
		set_keyer(config.keyer);
				
		if(config.embedded_audio)		
			output_->BeginAudioPreroll();		
		
		for(int n = 0; n < buffer_size_; ++n)
			schedule_next_video(core::const_frame::empty());

		if(!config.embedded_audio)
			start_playback();
	}

	~decklink_consumer()
	{		
		is_running_ = false;
		video_frame_buffer_.try_push(core::const_frame::empty());
		audio_frame_buffer_.try_push(core::const_frame::empty());

		if(output_ != nullptr) 
		{
			output_->StopScheduledPlayback(0, nullptr, 0);
			if(config_.embedded_audio)
				output_->DisableAudioOutput();
			output_->DisableVideoOutput();
		}
	}
			
	void set_latency(configuration::latency_t latency)
	{		
		if(latency == configuration::latency_t::low_latency)
		{
			configuration_->SetFlag(bmdDeckLinkConfigLowLatencyVideoOutput, true);
			CASPAR_LOG(info) << print() << L" Enabled low-latency mode.";
		}
		else if(latency == configuration::latency_t::normal_latency)
		{			
			configuration_->SetFlag(bmdDeckLinkConfigLowLatencyVideoOutput, false);
			CASPAR_LOG(info) << print() << L" Disabled low-latency mode.";
		}
	}

	void set_keyer(configuration::keyer_t keyer)
	{
		if(keyer == configuration::keyer_t::internal_keyer) 
		{
			BOOL value = true;
			if(SUCCEEDED(attributes_->GetFlag(BMDDeckLinkSupportsInternalKeying, &value)) && !value)
				CASPAR_LOG(error) << print() << L" Failed to enable internal keyer.";	
			else if(FAILED(keyer_->Enable(FALSE)))			
				CASPAR_LOG(error) << print() << L" Failed to enable internal keyer.";			
			else if(FAILED(keyer_->SetLevel(255)))			
				CASPAR_LOG(error) << print() << L" Failed to set key-level to max.";
			else
				CASPAR_LOG(info) << print() << L" Enabled internal keyer.";		
		}
		else if(keyer == configuration::keyer_t::external_keyer)
		{
			BOOL value = true;
			if(SUCCEEDED(attributes_->GetFlag(BMDDeckLinkSupportsExternalKeying, &value)) && !value)
				CASPAR_LOG(error) << print() << L" Failed to enable external keyer.";	
			else if(FAILED(keyer_->Enable(TRUE)))			
				CASPAR_LOG(error) << print() << L" Failed to enable external keyer.";	
			else if(FAILED(keyer_->SetLevel(255)))			
				CASPAR_LOG(error) << print() << L" Failed to set key-level to max.";
			else
				CASPAR_LOG(info) << print() << L" Enabled external keyer.";			
		}
	}
	
	void enable_audio()
	{
		if(FAILED(output_->EnableAudioOutput(bmdAudioSampleRate48kHz, bmdAudioSampleType32bitInteger, 2, bmdAudioOutputStreamTimestamped)))
				CASPAR_THROW_EXCEPTION(caspar_exception() << msg_info(u8(print()) + " Could not enable audio output."));
				
		if(FAILED(output_->SetAudioCallback(this)))
			CASPAR_THROW_EXCEPTION(caspar_exception() << msg_info(u8(print()) + " Could not set audio callback."));

		CASPAR_LOG(info) << print() << L" Enabled embedded-audio.";
	}

	void enable_video(BMDDisplayMode display_mode)
	{
		if(FAILED(output_->EnableVideoOutput(display_mode, bmdVideoOutputFlagDefault))) 
			CASPAR_THROW_EXCEPTION(caspar_exception() << msg_info(u8(print()) + " Could not enable video output."));
		
		if(FAILED(output_->SetScheduledFrameCompletionCallback(this)))
			CASPAR_THROW_EXCEPTION(caspar_exception() 
									<< msg_info(u8(print()) + " Failed to set playback completion callback.")
									<< boost::errinfo_api_function("SetScheduledFrameCompletionCallback"));
	}

	void start_playback()
	{
		if(FAILED(output_->StartScheduledPlayback(0, format_desc_.time_scale, 1.0))) 
			CASPAR_THROW_EXCEPTION(caspar_exception() << msg_info(u8(print()) + " Failed to schedule playback."));
	}
	
	virtual HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, LPVOID*)	{return E_NOINTERFACE;}
	virtual ULONG STDMETHODCALLTYPE AddRef()					{return 1;}
	virtual ULONG STDMETHODCALLTYPE Release()				{return 1;}
	
	virtual HRESULT STDMETHODCALLTYPE ScheduledPlaybackHasStopped()
	{
		is_running_ = false;
		CASPAR_LOG(info) << print() << L" Scheduled playback has stopped.";
		return S_OK;
	}

	virtual HRESULT STDMETHODCALLTYPE ScheduledFrameCompleted(IDeckLinkVideoFrame* completed_frame, BMDOutputFrameCompletionResult result)
	{
		if(!is_running_)
			return E_FAIL;
		
		try
		{
			if(result == bmdOutputFrameDisplayedLate)
			{
				graph_->set_tag("late-frame");
				video_scheduled_ += format_desc_.duration;
				audio_scheduled_ += reinterpret_cast<decklink_frame*>(completed_frame)->audio_data().size()/format_desc_.audio_channels;
				//++video_scheduled_;
				//audio_scheduled_ += format_desc_.audio_cadence[0];
				//++audio_scheduled_;
			}
			else if(result == bmdOutputFrameDropped)
				graph_->set_tag("dropped-frame");
			else if(result == bmdOutputFrameFlushed)
				graph_->set_tag("flushed-frame");

			auto frame = core::const_frame::empty();	
			video_frame_buffer_.pop(frame);
			send_completion_.try_completion();
			schedule_next_video(frame);	
			
			UINT32 buffered;
			output_->GetBufferedVideoFrameCount(&buffered);
			graph_->set_value("buffered-video", static_cast<double>(buffered)/format_desc_.fps);
		}
		catch(...)
		{
			lock(exception_mutex_, [&]
			{
				exception_ = std::current_exception();
			});
			return E_FAIL;
		}

		return S_OK;
	}
		
	virtual HRESULT STDMETHODCALLTYPE RenderAudioSamples(BOOL preroll)
	{
		if(!is_running_)
			return E_FAIL;
		
		try
		{	
			if(preroll)
			{
				if(++preroll_count_ >= buffer_size_)
				{
					output_->EndAudioPreroll();
					start_playback();				
				}
				else
				{
					schedule_next_audio(core::audio_buffer(format_desc_.audio_cadence[preroll % format_desc_.audio_cadence.size()] * format_desc_.audio_channels, 0));
				}
			}
			else
			{
				auto frame = core::const_frame::empty();

				while(audio_frame_buffer_.try_pop(frame))
				{
					send_completion_.try_completion();
					schedule_next_audio(frame.audio_data());
				}
			}

			UINT32 buffered;
			output_->GetBufferedAudioSampleFrameCount(&buffered);
			graph_->set_value("buffered-audio", static_cast<double>(buffered) / (format_desc_.audio_cadence[0] * format_desc_.audio_channels * 2));
		}
		catch(...)
		{
			tbb::spin_mutex::scoped_lock lock(exception_mutex_);
			exception_ = std::current_exception();
			return E_FAIL;
		}

		return S_OK;
	}

	template<typename T>
	void schedule_next_audio(const T& audio_data)
	{
		auto sample_frame_count = static_cast<int>(audio_data.size()/format_desc_.audio_channels);

		audio_container_.push_back(std::vector<int32_t>(audio_data.begin(), audio_data.end()));

		if(FAILED(output_->ScheduleAudioSamples(audio_container_.back().data(), sample_frame_count, audio_scheduled_, format_desc_.audio_sample_rate, nullptr)))
			CASPAR_LOG(error) << print() << L" Failed to schedule audio.";

		audio_scheduled_ += sample_frame_count;
	}
			
	void schedule_next_video(core::const_frame frame)
	{
		auto frame2 = wrap_raw<com_ptr, IDeckLinkVideoFrame>(new decklink_frame(frame, format_desc_, config_.key_only));
		if(FAILED(output_->ScheduleVideoFrame(get_raw(frame2), video_scheduled_, format_desc_.duration, format_desc_.time_scale)))
			CASPAR_LOG(error) << print() << L" Failed to schedule video.";

		video_scheduled_ += format_desc_.duration;

		graph_->set_value("tick-time", tick_timer_.elapsed()*format_desc_.fps*0.5);
		tick_timer_.restart();
	}

	std::future<bool> send(core::const_frame frame)
	{
		auto exception = lock(exception_mutex_, [&]
		{
			return exception_;
		});

		if(exception != nullptr)
			std::rethrow_exception(exception);		

		if(!is_running_)
			CASPAR_THROW_EXCEPTION(caspar_exception() << msg_info(u8(print()) + " Is not running."));
		
		bool audio_ready = !config_.embedded_audio;
		bool video_ready = false;

		auto enqueue_task = [audio_ready, video_ready, frame, this]() mutable -> boost::optional<bool>
		{
			if (!audio_ready)
				audio_ready = audio_frame_buffer_.try_push(frame);

			if (!video_ready)
				video_ready = video_frame_buffer_.try_push(frame);

			if (audio_ready && video_ready)
				return true;
			else
				return boost::optional<bool>();
		};
		
		if (enqueue_task())
			return make_ready_future(true);

		send_completion_.set_task(enqueue_task);

		return send_completion_.get_future();
	}
	
	std::wstring print() const
	{
		return model_name_ + L" [" + boost::lexical_cast<std::wstring>(channel_index_) + L"-" +
			boost::lexical_cast<std::wstring>(config_.device_index) + L"|" +  format_desc_.name + L"]";
	}
};

struct decklink_consumer_proxy : public core::frame_consumer
{
	core::monitor::subject				monitor_subject_;
	const configuration					config_;
	std::unique_ptr<decklink_consumer>	consumer_;
	executor							executor_;
public:

	decklink_consumer_proxy(const configuration& config)
		: config_(config)
		, executor_(L"decklink_consumer[" + boost::lexical_cast<std::wstring>(config.device_index) + L"]")
	{
		auto ctx = core::diagnostics::call_context::for_thread();
		executor_.begin_invoke([=]
		{
			core::diagnostics::call_context::for_thread() = ctx;
			com_initialize();
		});
	}

	~decklink_consumer_proxy()
	{
		executor_.invoke([=]
		{
			consumer_.reset();
			com_uninitialize();
		});
	}

	// frame_consumer
	
	void initialize(const core::video_format_desc& format_desc, int channel_index) override
	{
		executor_.invoke([=]
		{
			consumer_.reset();
			consumer_.reset(new decklink_consumer(config_, format_desc, channel_index));			
		});
	}
	
	std::future<bool> send(core::const_frame frame) override
	{
		return consumer_->send(frame);
	}
	
	std::wstring print() const override
	{
		return consumer_ ? consumer_->print() : L"[decklink_consumer]";
	}		

	std::wstring name() const override
	{
		return L"decklink";
	}

	boost::property_tree::wptree info() const override
	{
		boost::property_tree::wptree info;
		info.add(L"type", L"decklink");
		info.add(L"key-only", config_.key_only);
		info.add(L"device", config_.device_index);
		info.add(L"low-latency", config_.latency == configuration::latency_t::low_latency);
		info.add(L"embedded-audio", config_.embedded_audio);
		//info.add(L"internal-key", config_.internal_key);
		return info;
	}

	int buffer_depth() const override
	{
		return config_.buffer_depth();
	}

	int index() const override
	{
		return 300 + config_.device_index;
	}

	core::monitor::subject& monitor_output()
	{
		return monitor_subject_;
	}
};	

spl::shared_ptr<core::frame_consumer> create_consumer(
		const std::vector<std::wstring>& params, core::interaction_sink*)
{
	if(params.size() < 1 || params[0] != L"DECKLINK")
		return core::frame_consumer::empty();
	
	configuration config;
		
	if(params.size() > 1)
		config.device_index = boost::lexical_cast<int>(params[1]);
	
	if(std::find(params.begin(), params.end(), L"INTERNAL_KEY")			!= params.end())
		config.keyer = configuration::keyer_t::internal_keyer;
	else if(std::find(params.begin(), params.end(), L"EXTERNAL_KEY")	!= params.end())
		config.keyer = configuration::keyer_t::external_keyer;
	else
		config.keyer = configuration::keyer_t::default_keyer;

	if(std::find(params.begin(), params.end(), L"LOW_LATENCY")	 != params.end())
		config.latency = configuration::latency_t::low_latency;

	config.embedded_audio	= std::find(params.begin(), params.end(), L"EMBEDDED_AUDIO") != params.end();
	config.key_only			= std::find(params.begin(), params.end(), L"KEY_ONLY")		 != params.end();

	return spl::make_shared<decklink_consumer_proxy>(config);
}

spl::shared_ptr<core::frame_consumer> create_preconfigured_consumer(
		const boost::property_tree::wptree& ptree, core::interaction_sink*)
{
	configuration config;

	auto keyer = ptree.get(L"keyer", L"default");
	if(keyer == L"external")
		config.keyer = configuration::keyer_t::external_keyer;
	else if(keyer == L"internal")
		config.keyer = configuration::keyer_t::internal_keyer;

	auto latency = ptree.get(L"latency", L"normal");
	if(latency == L"low")
		config.latency = configuration::latency_t::low_latency;
	else if(latency == L"normal")
		config.latency = configuration::latency_t::normal_latency;

	config.key_only				= ptree.get(L"key-only",		config.key_only);
	config.device_index			= ptree.get(L"device",			config.device_index);
	config.embedded_audio		= ptree.get(L"embedded-audio",	config.embedded_audio);
	config.base_buffer_depth	= ptree.get(L"buffer-depth",	config.base_buffer_depth);

	return spl::make_shared<decklink_consumer_proxy>(config);
}

}}

/*
##############################################################################
Pre-rolling

Mail: 2011-05-09

Yoshan
BMD Developer Support
developer@blackmagic-design.com

-----------------------------------------------------------------------------

Thanks for your inquiry. The minimum number of frames that you can preroll 
for scheduled playback is three frames for video and four frames for audio. 
As you mentioned if you preroll less frames then playback will not start or
playback will be very sporadic. From our experience with Media Express, we 
recommended that at least seven frames are prerolled for smooth playback. 

Regarding the bmdDeckLinkConfigLowLatencyVideoOutput flag:
There can be around 3 frames worth of latency on scheduled output.
When the bmdDeckLinkConfigLowLatencyVideoOutput flag is used this latency is
reduced  or removed for scheduled playback. If the DisplayVideoFrameSync() 
method is used, the bmdDeckLinkConfigLowLatencyVideoOutput setting will 
guarantee that the provided frame will be output as soon the previous 
frame output has been completed.
################################################################################
*/

/*
##############################################################################
Async DMA Transfer without redundant copying

Mail: 2011-05-10

Yoshan
BMD Developer Support
developer@blackmagic-design.com

-----------------------------------------------------------------------------

Thanks for your inquiry. You could try subclassing IDeckLinkMutableVideoFrame 
and providing a pointer to your video buffer when GetBytes() is called. 
This may help to keep copying to a minimum. Please ensure that the pixel 
format is in bmdFormat10BitYUV, otherwise the DeckLink API / driver will 
have to colourspace convert which may result in additional copying.
################################################################################
*/
