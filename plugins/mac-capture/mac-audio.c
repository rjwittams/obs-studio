#include <AudioUnit/AudioUnit.h>
#include <CoreFoundation/CFString.h>
#include <CoreAudio/CoreAudio.h>
#include <unistd.h>
#include <errno.h>

#include <obs-module.h>
#include <util/threading.h>
#include <util/c99defs.h>
#include <util/apple/cfstring-utils.h>

#include "audio-device-enum.h"

#define PROPERTY_DEFAULT_DEVICE kAudioHardwarePropertyDefaultInputDevice
#define PROPERTY_FORMATS kAudioStreamPropertyAvailablePhysicalFormats

#define SCOPE_OUTPUT kAudioUnitScope_Output
#define SCOPE_INPUT kAudioUnitScope_Input
#define SCOPE_GLOBAL kAudioUnitScope_Global

#define BUS_OUTPUT 0
#define BUS_INPUT 1

#define MAX_DEVICES 20
#define MIN_SELECTED_CHANNELS 1

#define set_property AudioUnitSetProperty
#define get_property AudioUnitGetProperty

#define TEXT_AUDIO_INPUT obs_module_text("CoreAudio.InputCapture");
#define TEXT_AUDIO_OUTPUT obs_module_text("CoreAudio.OutputCapture");
#define TEXT_DEVICE obs_module_text("CoreAudio.Device")
#define TEXT_DEVICE_DEFAULT obs_module_text("CoreAudio.Device.Default")
#define TEXT_SELECTED_CHANNELS obs_module_text("CoreAudio.SelectedChannels")

struct coreaudio_data {
	char *device_name;
	char *device_uid;
	AudioUnit unit;
	AudioDeviceID device_id;
	AudioBufferList *buf_list;
	bool au_initialized;
	bool active;
	bool default_device;
	bool input;
	bool no_devices;

	uint32_t sample_rate;
	uint8_t available_channels;
	char **channel_names;
	uint64_t selected_channels_bitset;
	enum audio_format format;
	enum speaker_layout speakers;

	pthread_t reconnect_thread;
	os_event_t *exit_event;
	volatile bool reconnecting;
	unsigned long retry_time;

	obs_source_t *source;
};

static bool get_default_output_device(struct coreaudio_data *ca)
{
	struct device_list list;

	memset(&list, 0, sizeof(struct device_list));
	coreaudio_enum_devices(&list, false);

	if (!list.items.num)
		return false;

	bfree(ca->device_uid);
	ca->device_uid = bstrdup(list.items.array[0].value.array);

	device_list_free(&list);
	return true;
}

static bool find_device_id_by_uid(struct coreaudio_data *ca)
{
	UInt32 size = sizeof(AudioDeviceID);
	CFStringRef cf_uid = NULL;
	CFStringRef qual = NULL;
	UInt32 qual_size = 0;
	OSStatus stat;
	bool success;

	AudioObjectPropertyAddress addr = {
		.mScope = kAudioObjectPropertyScopeGlobal,
		.mElement = kAudioObjectPropertyElementMaster};

	if (!ca->device_uid)
		ca->device_uid = bstrdup("default");

	ca->default_device = false;
	ca->no_devices = false;

	/* have to do this because mac output devices don't actually exist */
	if (astrcmpi(ca->device_uid, "default") == 0) {
		if (ca->input) {
			ca->default_device = true;
		} else {
			if (!get_default_output_device(ca)) {
				ca->no_devices = true;
				return false;
			}
		}
	}

	cf_uid = CFStringCreateWithCString(NULL, ca->device_uid,
					   kCFStringEncodingUTF8);

	if (ca->default_device) {
		addr.mSelector = PROPERTY_DEFAULT_DEVICE;
		stat = AudioObjectGetPropertyData(kAudioObjectSystemObject,
						  &addr, qual_size, &qual,
						  &size, &ca->device_id);
		success = (stat == noErr);
	} else {
		success = coreaudio_get_device_id(cf_uid, &ca->device_id);
	}

	if (cf_uid)
		CFRelease(cf_uid);

	return success;
}

static inline void ca_warn(struct coreaudio_data *ca, const char *func,
			   const char *format, ...)
{
	va_list args;
	struct dstr str = {0};

	va_start(args, format);

	dstr_printf(&str, "[%s]:[device '%s'] ", func, ca->device_name);
	dstr_vcatf(&str, format, args);
	blog(LOG_WARNING, "%s", str.array);
	dstr_free(&str);

	va_end(args);
}

static inline bool ca_success(OSStatus stat, struct coreaudio_data *ca,
			      const char *func, const char *action)
{
	if (stat != noErr) {
		blog(LOG_WARNING, "[%s]:[device '%s'] %s failed: %d", func,
		     ca->device_name, action, (int)stat);
		return false;
	}

	return true;
}

enum coreaudio_io_type {
	IO_TYPE_INPUT,
	IO_TYPE_OUTPUT,
};

static inline bool enable_io(struct coreaudio_data *ca,
			     enum coreaudio_io_type type, bool enable)
{
	UInt32 enable_int = enable;
	return set_property(ca->unit, kAudioOutputUnitProperty_EnableIO,
			    (type == IO_TYPE_INPUT) ? SCOPE_INPUT
						    : SCOPE_OUTPUT,
			    (type == IO_TYPE_INPUT) ? BUS_INPUT : BUS_OUTPUT,
			    &enable_int, sizeof(enable_int));
}

static inline enum audio_format convert_ca_format(UInt32 format_flags,
						  UInt32 bits)
{
	bool planar = (format_flags & kAudioFormatFlagIsNonInterleaved) != 0;

	if (format_flags & kAudioFormatFlagIsFloat)
		return planar ? AUDIO_FORMAT_FLOAT_PLANAR : AUDIO_FORMAT_FLOAT;

	if (!(format_flags & kAudioFormatFlagIsSignedInteger) && bits == 8)
		return planar ? AUDIO_FORMAT_U8BIT_PLANAR : AUDIO_FORMAT_U8BIT;

	/* not float?  not signed int?  no clue, fail */
	if ((format_flags & kAudioFormatFlagIsSignedInteger) == 0)
		return AUDIO_FORMAT_UNKNOWN;

	if (bits == 16)
		return planar ? AUDIO_FORMAT_16BIT_PLANAR : AUDIO_FORMAT_16BIT;
	else if (bits == 32)
		return planar ? AUDIO_FORMAT_32BIT_PLANAR : AUDIO_FORMAT_32BIT;

	return AUDIO_FORMAT_UNKNOWN;
}

static inline enum speaker_layout convert_ca_speaker_layout(UInt32 channels)
{
	switch (channels) {
	case 1:
		return SPEAKERS_MONO;
	case 2:
		return SPEAKERS_STEREO;
	case 3:
		return SPEAKERS_2POINT1;
	case 4:
		return SPEAKERS_4POINT0;
	case 5:
		return SPEAKERS_4POINT1;
	case 6:
		return SPEAKERS_5POINT1;
	case 8:
		return SPEAKERS_7POINT1;
	}

	return SPEAKERS_UNKNOWN;
}

static char **coreaudio_get_channel_names(struct coreaudio_data *ca)
{
	char **channel_names = bzalloc(sizeof(char *) * ca->available_channels);

	for (uint32_t i = 0; i < ca->available_channels; i++) {
		CFStringRef cf_chan_name = NULL;
		UInt32 dataSize = sizeof(cf_chan_name);
		AudioObjectPropertyAddress pa;
		pa.mSelector = kAudioObjectPropertyElementName;
		pa.mScope = kAudioDevicePropertyScopeInput;
		pa.mElement = i + 1;
		OSStatus stat = AudioObjectGetPropertyData(
			ca->device_id, &pa, 0, NULL, &dataSize, &cf_chan_name);

		struct dstr name;
		dstr_init(&name);
		if (ca_success(stat, ca, "coreaudio_init_format",
			       "get channel names") &&
		    CFStringGetLength(cf_chan_name)) {
			dstr_printf(
				&name, "%d - %s", i + 0,
				CFStringGetCStringPtr(cf_chan_name,
						      kCFStringEncodingUTF8));
		} else {
			dstr_printf(&name, "%d - Channel %d [unnamed]", i + 1,
				    i + 1);
		}
		channel_names[i] = bstrdup_n(name.array, name.len + 1);
		dstr_free(&name);

		if (cf_chan_name) {
			CFRelease(cf_chan_name);
		}
	}
	return channel_names;
}

static uint8_t coreaudio_fill_channel_map(const struct coreaudio_data *ca,
					  SInt32 *channel_map)
{
	uint8_t num_channels_sel = 0;
	for (uint8_t i = 0;
	     i < ca->available_channels && num_channels_sel < MAX_AV_PLANES;
	     i++) {
		if (ca->selected_channels_bitset & (1UL << i)) {
			channel_map[num_channels_sel++] = i;
		}
	}
	// Select at least the first channel
	if (num_channels_sel == 0) {
		channel_map[num_channels_sel++] = 0;
	}
	return num_channels_sel;
}

static bool coreaudio_init_format(struct coreaudio_data *ca)
{
	AudioStreamBasicDescription desc;
	OSStatus stat;
	UInt32 size = sizeof(desc);
	stat = get_property(ca->unit, kAudioUnitProperty_StreamFormat,
			    SCOPE_INPUT, BUS_INPUT, &desc, &size);
	if (!ca_success(stat, ca, "coreaudio_init_format", "get input format"))
		return false;

	ca->available_channels = desc.mChannelsPerFrame;

	ca->channel_names = coreaudio_get_channel_names(ca);

	// Work out the channel map - up to the max number of outputs
	SInt32 channel_map[MAX_AV_PLANES];
	uint8_t num_channels_sel = coreaudio_fill_channel_map(ca, channel_map);

	// Always work out the format from the actual number of channels selected
	desc.mChannelsPerFrame = num_channels_sel;
	desc.mBytesPerFrame = num_channels_sel * desc.mBitsPerChannel / 8;
	desc.mBytesPerPacket = desc.mFramesPerPacket * desc.mBytesPerFrame;

	stat = set_property(ca->unit, kAudioOutputUnitProperty_ChannelMap,
			    SCOPE_INPUT, BUS_INPUT, channel_map,
			    sizeof(channel_map[0]) * num_channels_sel);
	if (!ca_success(stat, ca, "coreaudio_init_format", "set channel map"))
		return false;

	stat = set_property(ca->unit, kAudioUnitProperty_StreamFormat,
			    SCOPE_OUTPUT, BUS_INPUT, &desc, size);
	if (!ca_success(stat, ca, "coreaudio_init_format", "set output format"))
		return false;

	if (desc.mFormatID != kAudioFormatLinearPCM) {
		ca_warn(ca, "coreaudio_init_format", "format is not PCM");
		return false;
	}

	ca->format = convert_ca_format(desc.mFormatFlags, desc.mBitsPerChannel);
	if (ca->format == AUDIO_FORMAT_UNKNOWN) {
		ca_warn(ca, "coreaudio_init_format",
			"unknown format flags: "
			"%u, bits: %u",
			(unsigned int)desc.mFormatFlags,
			(unsigned int)desc.mBitsPerChannel);
		return false;
	}

	ca->sample_rate = (uint32_t)desc.mSampleRate;
	ca->speakers = convert_ca_speaker_layout(num_channels_sel);

	//	if (ca->speakers == SPEAKERS_UNKNOWN) {
	//		ca_warn(ca, "coreaudio_init_format",
	//			"unknown speaker layout: "
	//			"%u channels",
	//			(unsigned int)num_channels_sel);
	//		return false;
	//	}

	return true;
}

static bool coreaudio_init_buffer(struct coreaudio_data *ca)
{
	UInt32 buf_size = 0;
	UInt32 size = 0;
	UInt32 frames = 0;
	OSStatus stat;

	AudioObjectPropertyAddress addr = {
		kAudioDevicePropertyStreamConfiguration,
		kAudioDevicePropertyScopeInput,
		kAudioObjectPropertyElementMaster};

	stat = AudioObjectGetPropertyDataSize(ca->device_id, &addr, 0, NULL,
					      &buf_size);
	if (!ca_success(stat, ca, "coreaudio_init_buffer", "get list size"))
		return false;

	size = sizeof(frames);
	stat = get_property(ca->unit, kAudioDevicePropertyBufferFrameSize,
			    SCOPE_GLOBAL, 0, &frames, &size);
	if (!ca_success(stat, ca, "coreaudio_init_buffer", "get frame size"))
		return false;

	/* ---------------------- */

	ca->buf_list = bmalloc(buf_size);

	stat = AudioObjectGetPropertyData(ca->device_id, &addr, 0, NULL,
					  &buf_size, ca->buf_list);
	if (!ca_success(stat, ca, "coreaudio_init_buffer", "allocate")) {
		bfree(ca->buf_list);
		ca->buf_list = NULL;
		return false;
	}

	for (UInt32 i = 0; i < ca->buf_list->mNumberBuffers; i++) {
		size = ca->buf_list->mBuffers[i].mDataByteSize;
		ca->buf_list->mBuffers[i].mData = bmalloc(size);
	}

	return true;
}

static void buf_list_free(AudioBufferList *buf_list)
{
	if (buf_list) {
		for (UInt32 i = 0; i < buf_list->mNumberBuffers; i++)
			bfree(buf_list->mBuffers[i].mData);

		bfree(buf_list);
	}
}

static OSStatus input_callback(void *data,
			       AudioUnitRenderActionFlags *action_flags,
			       const AudioTimeStamp *ts_data, UInt32 bus_num,
			       UInt32 frames, AudioBufferList *ignored_buffers)
{
	struct coreaudio_data *ca = data;
	OSStatus stat;
	struct obs_source_audio audio;

	stat = AudioUnitRender(ca->unit, action_flags, ts_data, bus_num, frames,
			       ca->buf_list);
	if (!ca_success(stat, ca, "input_callback", "audio retrieval"))
		return noErr;

	for (UInt32 i = 0; i < ca->buf_list->mNumberBuffers; i++)
		audio.data[i] = ca->buf_list->mBuffers[i].mData;

	audio.frames = frames;
	audio.speakers = ca->speakers;
	audio.format = ca->format;
	audio.samples_per_sec = ca->sample_rate;
	audio.timestamp = ts_data->mHostTime;

	obs_source_output_audio(ca->source, &audio);

	UNUSED_PARAMETER(ignored_buffers);
	return noErr;
}

static void coreaudio_stop(struct coreaudio_data *ca);
static bool coreaudio_init(struct coreaudio_data *ca);
static void coreaudio_uninit(struct coreaudio_data *ca);

static void *reconnect_thread(void *param)
{
	struct coreaudio_data *ca = param;

	ca->reconnecting = true;

	while (os_event_timedwait(ca->exit_event, ca->retry_time) ==
	       ETIMEDOUT) {
		if (coreaudio_init(ca))
			break;
	}

	blog(LOG_DEBUG, "coreaudio: exit the reconnect thread");
	ca->reconnecting = false;
	return NULL;
}

static void coreaudio_begin_reconnect(struct coreaudio_data *ca)
{
	int ret;

	if (ca->reconnecting)
		return;

	ret = pthread_create(&ca->reconnect_thread, NULL, reconnect_thread, ca);
	if (ret != 0)
		blog(LOG_WARNING,
		     "[coreaudio_begin_reconnect] failed to "
		     "create thread, error code: %d",
		     ret);
}

static OSStatus
notification_callback(AudioObjectID id, UInt32 num_addresses,
		      const AudioObjectPropertyAddress addresses[], void *data)
{
	struct coreaudio_data *ca = data;

	coreaudio_stop(ca);
	coreaudio_uninit(ca);

	if (addresses[0].mSelector == PROPERTY_DEFAULT_DEVICE)
		ca->retry_time = 300;
	else
		ca->retry_time = 2000;

	blog(LOG_INFO,
	     "coreaudio: device '%s' disconnected or changed.  "
	     "attempting to reconnect",
	     ca->device_name);

	coreaudio_begin_reconnect(ca);

	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(num_addresses);

	return noErr;
}

static OSStatus add_listener(struct coreaudio_data *ca, UInt32 property)
{
	AudioObjectPropertyAddress addr = {property,
					   kAudioObjectPropertyScopeGlobal,
					   kAudioObjectPropertyElementMaster};

	return AudioObjectAddPropertyListener(ca->device_id, &addr,
					      notification_callback, ca);
}

static bool coreaudio_init_hooks(struct coreaudio_data *ca)
{
	OSStatus stat;
	AURenderCallbackStruct callback_info = {.inputProc = input_callback,
						.inputProcRefCon = ca};

	stat = add_listener(ca, kAudioDevicePropertyDeviceIsAlive);
	if (!ca_success(stat, ca, "coreaudio_init_hooks",
			"set disconnect callback"))
		return false;

	stat = add_listener(ca, PROPERTY_FORMATS);
	if (!ca_success(stat, ca, "coreaudio_init_hooks",
			"set format change callback"))
		return false;

	if (ca->default_device) {
		AudioObjectPropertyAddress addr = {
			PROPERTY_DEFAULT_DEVICE,
			kAudioObjectPropertyScopeGlobal,
			kAudioObjectPropertyElementMaster};

		stat = AudioObjectAddPropertyListener(kAudioObjectSystemObject,
						      &addr,
						      notification_callback,
						      ca);
		if (!ca_success(stat, ca, "coreaudio_init_hooks",
				"set device change callback"))
			return false;
	}

	stat = set_property(ca->unit, kAudioOutputUnitProperty_SetInputCallback,
			    SCOPE_GLOBAL, 0, &callback_info,
			    sizeof(callback_info));
	if (!ca_success(stat, ca, "coreaudio_init_hooks", "set input callback"))
		return false;

	return true;
}

static void coreaudio_remove_hooks(struct coreaudio_data *ca)
{
	AURenderCallbackStruct callback_info = {.inputProc = NULL,
						.inputProcRefCon = NULL};

	AudioObjectPropertyAddress addr = {kAudioDevicePropertyDeviceIsAlive,
					   kAudioObjectPropertyScopeGlobal,
					   kAudioObjectPropertyElementMaster};

	AudioObjectRemovePropertyListener(ca->device_id, &addr,
					  notification_callback, ca);

	addr.mSelector = PROPERTY_FORMATS;
	AudioObjectRemovePropertyListener(ca->device_id, &addr,
					  notification_callback, ca);

	if (ca->default_device) {
		addr.mSelector = PROPERTY_DEFAULT_DEVICE;
		AudioObjectRemovePropertyListener(kAudioObjectSystemObject,
						  &addr, notification_callback,
						  ca);
	}

	set_property(ca->unit, kAudioOutputUnitProperty_SetInputCallback,
		     SCOPE_GLOBAL, 0, &callback_info, sizeof(callback_info));
}

static bool coreaudio_get_device_name(struct coreaudio_data *ca)
{
	CFStringRef cf_name = NULL;
	UInt32 size = sizeof(CFStringRef);
	char *name = NULL;

	const AudioObjectPropertyAddress addr = {
		kAudioDevicePropertyDeviceNameCFString,
		kAudioObjectPropertyScopeInput,
		kAudioObjectPropertyElementMaster};

	OSStatus stat = AudioObjectGetPropertyData(ca->device_id, &addr, 0,
						   NULL, &size, &cf_name);
	if (stat != noErr) {
		blog(LOG_WARNING,
		     "[coreaudio_get_device_name] failed to "
		     "get name: %d",
		     (int)stat);
		return false;
	}

	name = cfstr_copy_cstr(cf_name, kCFStringEncodingUTF8);
	if (!name) {
		blog(LOG_WARNING, "[coreaudio_get_device_name] failed to "
				  "convert name to cstr for some reason");
		return false;
	}

	bfree(ca->device_name);
	ca->device_name = name;

	if (cf_name)
		CFRelease(cf_name);

	return true;
}

static bool coreaudio_start(struct coreaudio_data *ca)
{
	OSStatus stat;

	if (ca->active)
		return true;

	stat = AudioOutputUnitStart(ca->unit);
	return ca_success(stat, ca, "coreaudio_start", "start audio");
}

static void coreaudio_stop(struct coreaudio_data *ca)
{
	OSStatus stat;

	if (!ca->active)
		return;

	ca->active = false;

	stat = AudioOutputUnitStop(ca->unit);
	ca_success(stat, ca, "coreaudio_stop", "stop audio");
}

static bool coreaudio_init_unit(struct coreaudio_data *ca)
{
	AudioComponentDescription desc = {
		.componentType = kAudioUnitType_Output,
		.componentSubType = kAudioUnitSubType_HALOutput};

	AudioComponent component = AudioComponentFindNext(NULL, &desc);
	if (!component) {
		ca_warn(ca, "coreaudio_init_unit", "find component failed");
		return false;
	}

	OSStatus stat = AudioComponentInstanceNew(component, &ca->unit);
	if (!ca_success(stat, ca, "coreaudio_init_unit", "instance unit"))
		return false;

	ca->au_initialized = true;
	return true;
}

static bool coreaudio_init(struct coreaudio_data *ca)
{
	OSStatus stat;

	if (ca->au_initialized)
		return true;

	if (!find_device_id_by_uid(ca))
		return false;
	if (!coreaudio_get_device_name(ca))
		return false;
	if (!coreaudio_init_unit(ca))
		return false;

	stat = enable_io(ca, IO_TYPE_INPUT, true);
	if (!ca_success(stat, ca, "coreaudio_init", "enable input io"))
		goto fail;

	stat = enable_io(ca, IO_TYPE_OUTPUT, false);
	if (!ca_success(stat, ca, "coreaudio_init", "disable output io"))
		goto fail;

	stat = set_property(ca->unit, kAudioOutputUnitProperty_CurrentDevice,
			    SCOPE_GLOBAL, 0, &ca->device_id,
			    sizeof(ca->device_id));
	if (!ca_success(stat, ca, "coreaudio_init", "set current device"))
		goto fail;

	if (!coreaudio_init_format(ca))
		goto fail;
	if (!coreaudio_init_buffer(ca))
		goto fail;
	if (!coreaudio_init_hooks(ca))
		goto fail;

	stat = AudioUnitInitialize(ca->unit);
	if (!ca_success(stat, ca, "coreaudio_initialize", "initialize"))
		goto fail;

	if (!coreaudio_start(ca))
		goto fail;

	blog(LOG_INFO, "coreaudio: device '%s' initialized", ca->device_name);
	return ca->au_initialized;

fail:
	coreaudio_uninit(ca);
	return false;
}

static void coreaudio_try_init(struct coreaudio_data *ca)
{
	if (!coreaudio_init(ca)) {
		blog(LOG_INFO,
		     "coreaudio: failed to find device "
		     "uid: %s, waiting for connection",
		     ca->device_uid);

		ca->retry_time = 2000;

		if (ca->no_devices)
			blog(LOG_INFO, "coreaudio: no device found");
		else
			coreaudio_begin_reconnect(ca);
	}
}

static void coreaudio_uninit(struct coreaudio_data *ca)
{
	if (!ca->au_initialized)
		return;

	if (ca->unit) {
		coreaudio_stop(ca);

		OSStatus stat = AudioUnitUninitialize(ca->unit);
		ca_success(stat, ca, "coreaudio_uninit", "uninitialize");

		coreaudio_remove_hooks(ca);

		stat = AudioComponentInstanceDispose(ca->unit);
		ca_success(stat, ca, "coreaudio_uninit", "dispose");

		ca->unit = NULL;
	}

	ca->au_initialized = false;

	buf_list_free(ca->buf_list);
	ca->buf_list = NULL;

	if (ca->channel_names) {
		for (uint8_t i = 0; i < ca->available_channels; i++) {
			bfree(ca->channel_names[i]);
		}
		bfree(ca->channel_names);
		ca->channel_names = NULL;
	}
}

/* ------------------------------------------------------------------------- */

static const char *coreaudio_input_getname(void *unused)
{
	UNUSED_PARAMETER(unused);
	return TEXT_AUDIO_INPUT;
}

static const char *coreaudio_output_getname(void *unused)
{
	UNUSED_PARAMETER(unused);
	return TEXT_AUDIO_OUTPUT;
}

static void coreaudio_shutdown(struct coreaudio_data *ca)
{
	if (ca->reconnecting) {
		os_event_signal(ca->exit_event);
		pthread_join(ca->reconnect_thread, NULL);
		os_event_reset(ca->exit_event);
	}

	coreaudio_uninit(ca);

	if (ca->unit)
		AudioComponentInstanceDispose(ca->unit);
}

static void coreaudio_destroy(void *data)
{
	struct coreaudio_data *ca = data;

	if (ca) {
		coreaudio_shutdown(ca);

		os_event_destroy(ca->exit_event);
		bfree(ca->device_name);
		bfree(ca->device_uid);
		bfree(ca);
	}
}

static void coreaudio_set_channels(struct coreaudio_data *ca,
				   obs_data_t *settings)
{
	ca->selected_channels_bitset = 0;
	for (uint8_t i = 0; i < MAX_DEVICE_INPUT_CHANNELS; i++) {
		char setting_name[32];
		snprintf(setting_name, 32, "channel-%i", i + 1);
		bool enabled =
			obs_data_has_user_value(settings, setting_name)
				? obs_data_get_bool(settings, setting_name)
				: false;
		if (enabled) {
			ca->selected_channels_bitset |= 1UL << i;
		}
	}
}

static void coreaudio_update(void *data, obs_data_t *settings)
{
	struct coreaudio_data *ca = data;

	coreaudio_shutdown(ca);

	bfree(ca->device_uid);
	ca->device_uid = bstrdup(obs_data_get_string(settings, "device_id"));
	coreaudio_set_channels(ca, settings);

	coreaudio_try_init(ca);
}

static void coreaudio_defaults(obs_data_t *settings)
{
	obs_data_set_default_string(settings, "device_id", "default");
}

static void *coreaudio_create(obs_data_t *settings, obs_source_t *source,
			      bool input)
{
	struct coreaudio_data *ca = bzalloc(sizeof(struct coreaudio_data));

	if (os_event_init(&ca->exit_event, OS_EVENT_TYPE_MANUAL) != 0) {
		blog(LOG_ERROR,
		     "[coreaudio_create] failed to create "
		     "semephore: %d",
		     errno);
		bfree(ca);
		return NULL;
	}

	ca->device_uid = bstrdup(obs_data_get_string(settings, "device_id"));
	ca->source = source;
	ca->input = input;
	coreaudio_set_channels(ca, settings);

	if (!ca->device_uid)
		ca->device_uid = bstrdup("default");

	coreaudio_try_init(ca);
	return ca;
}

static void *coreaudio_create_input_capture(obs_data_t *settings,
					    obs_source_t *source)
{
	return coreaudio_create(settings, source, true);
}

static void *coreaudio_create_output_capture(obs_data_t *settings,
					     obs_source_t *source)
{
	return coreaudio_create(settings, source, false);
}

static void coreaudio_properties_warning(obs_properties_t *props,
					 obs_data_t *settings,
					 const char *warning)
{
	obs_property_t *warn_prop = obs_properties_get(props, "warning");
	obs_property_set_visible(warn_prop, warning != NULL);
	obs_data_set_default_string(settings, "warning",
				    warning ? warning : "");
}

static bool coreaudio_device_changed(void *data, obs_properties_t *props,
				     obs_property_t *p, obs_data_t *settings)
{
	const int max_channels = MAX_AV_PLANES;
	// This could be determined by the global speaker layout.
	// But that would preclude subsequent filters from mixing down to fewer
	struct coreaudio_data *ca = data;
	const char *prop_val =
		obs_data_get_string(settings, obs_property_name(p));
	obs_properties_remove_by_name(props, "selected_channels");
	if (ca && ca->au_initialized) {

		blog(LOG_INFO, "core audio: device changed %s - ca device %s",
		     prop_val, ca->device_name);

		obs_properties_t *chan_props = obs_properties_create();
		obs_properties_add_group(props, "selected_channels",
					 TEXT_SELECTED_CHANNELS,
					 OBS_GROUP_NORMAL, chan_props);

		uint8_t sel_count = 0;

		for (uint32_t i = 0; i < ca->available_channels; i++) {
			char name[32];
			snprintf(name, 32, "channel-%i", i + 1);
			obs_property_t *chan_prop = obs_properties_add_bool(
				chan_props, name, ca->channel_names[i]);

			bool is_selected;
			if (!obs_data_has_user_value(settings, name)) {
				is_selected = sel_count < max_channels;
				obs_data_set_bool(settings, name, is_selected);
			} else {
				is_selected = obs_data_get_bool(settings, name);
				if (sel_count >= max_channels) {
					is_selected = false;
					obs_data_set_bool(settings, name,
							  is_selected);
				}
			}
			sel_count += is_selected;
			obs_property_set_modified_callback2(
				chan_prop, coreaudio_device_changed, ca);
		}
		bool at_max = sel_count >= max_channels;
		bool at_min = sel_count <= MIN_SELECTED_CHANNELS;

		// If we are at max or min disable props that would make us go over
		if (at_max || at_min) {
			obs_property_t *chan_prop =
				obs_properties_first(chan_props);
			do {
				bool is_selected = obs_data_get_bool(
					settings, obs_property_name(chan_prop));
				blog(LOG_INFO,
				     "enabling is_selected %d min %d max %d %s",
				     is_selected, at_min, at_max,
				     obs_property_name(chan_prop));
				if ((at_max && !is_selected) ||
				    (at_min && is_selected)) {
					obs_property_set_enabled(chan_prop,
								 false);
					blog(LOG_INFO, "disabling %s ",
					     obs_property_name(chan_prop));
				}
			} while (obs_property_next(&chan_prop));
		}
		enum speaker_layout speakers =
			convert_ca_speaker_layout(sel_count);

		if (speakers == SPEAKERS_UNKNOWN) {
			char buf[64];
			snprintf(buf, 64, "Unknown speaker layout: %u channels",
				 sel_count);
			coreaudio_properties_warning(props, settings, buf);
		} else {
			char *warning =
				at_max ? "Maximum channels selected"
				       : at_min ? "Minimum channels selected"
						: NULL;
			coreaudio_properties_warning(props, settings, warning);
		}
	} else {
		coreaudio_properties_warning(
			props, settings,
			"Core audio unit could not be initialised");
	}

	return true;
}

static obs_properties_t *coreaudio_properties(bool input, void *data)
{
	struct coreaudio_data *ca = data;
	obs_properties_t *props = obs_properties_create();
	obs_property_t *property;
	struct device_list devices;

	memset(&devices, 0, sizeof(struct device_list));

	property = obs_properties_add_list(props, "device_id", TEXT_DEVICE,
					   OBS_COMBO_TYPE_LIST,
					   OBS_COMBO_FORMAT_STRING);

	coreaudio_enum_devices(&devices, input);

	if (devices.items.num)
		obs_property_list_add_string(property, TEXT_DEVICE_DEFAULT,
					     "default");

	for (size_t i = 0; i < devices.items.num; i++) {
		struct device_item *item = devices.items.array + i;
		obs_property_list_add_string(property, item->name.array,
					     item->value.array);
	}

	obs_property_t *warn_prop = obs_properties_add_text(
		props, "warning", "Warning", OBS_TEXT_DEFAULT);
	obs_property_set_enabled(warn_prop, false);
	obs_property_set_visible(warn_prop, false);

	obs_property_set_modified_callback2(property, coreaudio_device_changed,
					    ca);

	device_list_free(&devices);
	return props;
}

static obs_properties_t *coreaudio_input_properties(void *data)
{
	return coreaudio_properties(true, data);
}

static obs_properties_t *coreaudio_output_properties(void *data)
{
	return coreaudio_properties(false, data);
}

struct obs_source_info coreaudio_input_capture_info = {
	.id = "coreaudio_input_capture",
	.type = OBS_SOURCE_TYPE_INPUT,
	.output_flags = OBS_SOURCE_AUDIO | OBS_SOURCE_DO_NOT_DUPLICATE,
	.get_name = coreaudio_input_getname,
	.create = coreaudio_create_input_capture,
	.destroy = coreaudio_destroy,
	.update = coreaudio_update,
	.get_defaults = coreaudio_defaults,
	.get_properties = coreaudio_input_properties,
	.icon_type = OBS_ICON_TYPE_AUDIO_INPUT,
};

struct obs_source_info coreaudio_output_capture_info = {
	.id = "coreaudio_output_capture",
	.type = OBS_SOURCE_TYPE_INPUT,
	.output_flags = OBS_SOURCE_AUDIO | OBS_SOURCE_DO_NOT_DUPLICATE |
			OBS_SOURCE_DO_NOT_SELF_MONITOR,
	.get_name = coreaudio_output_getname,
	.create = coreaudio_create_output_capture,
	.destroy = coreaudio_destroy,
	.update = coreaudio_update,
	.get_defaults = coreaudio_defaults,
	.get_properties = coreaudio_output_properties,
	.icon_type = OBS_ICON_TYPE_AUDIO_OUTPUT,
};
