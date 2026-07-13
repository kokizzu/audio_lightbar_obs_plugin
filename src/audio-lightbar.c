#include <obs/obs-module.h>
#include <obs/graphics/graphics.h>
#include <obs/graphics/vec4.h>
#include <obs/media-io/audio-io.h>
#include <obs/util/bmem.h>

#include <math.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

OBS_DECLARE_MODULE()

#define SOURCE_ID "audio_lightbar_source"
#define MAX_BARS 256
#define MIN_BARS 8
#define DEFAULT_BARS 96
#define MIN_BRICK_ROWS 10
#define MAX_BRICK_ROWS 100
#define DEFAULT_BRICK_ROWS 24
#define STYLE_BRICK 0
#define STYLE_SMOOTH 1
#define ATTACH_REFRESH_SECONDS 2.0f
#define MAX_ANALYSIS_SAMPLES 1024u
#define DEFAULT_MIN_FREQUENCY 60.0
#define DEFAULT_MAX_FREQUENCY 5000.0
#define DEFAULT_NOISE_FLOOR_DB -72.0
#define TWO_PI 6.28318530717958647692f

struct lightbar_source {
	obs_source_t *source;
	pthread_mutex_t lock;
	pthread_mutex_t attach_lock;

	char *audio_source_name;
	bool attach_all;
	bool attached;

	obs_weak_source_t **attached_sources;
	size_t attached_count;
	size_t attached_capacity;

	uint32_t width;
	uint32_t height;
	uint32_t bar_count;
	uint32_t brick_rows;
	uint32_t update_rate;
	int render_style;
	bool show_background;
	bool show_peak_caps;
	bool mirror;
	bool reverse_rainbow;
	bool vertical_rainbow;
	bool ignore_outside_frequency_range;
	double sensitivity;
	double decay;
	double cap_decay;
	double min_frequency;
	double max_frequency;
	double noise_floor_db;

	float bars[MAX_BARS];
	float caps[MAX_BARS];
	uint32_t analysis_frame_accumulator;
	float refresh_accumulator;
};

struct list_audio_sources_param {
	obs_property_t *property;
	obs_source_t *self;
};

static float clamp_float(float value, float min_value, float max_value)
{
	if (value < min_value)
		return min_value;
	if (value > max_value)
		return max_value;
	return value;
}

static uint32_t clamp_u32(uint32_t value, uint32_t min_value, uint32_t max_value)
{
	if (value < min_value)
		return min_value;
	if (value > max_value)
		return max_value;
	return value;
}

static void hsv_to_rgb(float hue, float saturation, float value, struct vec4 *out)
{
	const float scaled = hue * 6.0f;
	const int sector = (int)floorf(scaled);
	const float fraction = scaled - (float)sector;
	const float p = value * (1.0f - saturation);
	const float q = value * (1.0f - saturation * fraction);
	const float t = value * (1.0f - saturation * (1.0f - fraction));

	switch (sector % 6) {
	case 0:
		out->x = value;
		out->y = t;
		out->z = p;
		break;
	case 1:
		out->x = q;
		out->y = value;
		out->z = p;
		break;
	case 2:
		out->x = p;
		out->y = value;
		out->z = t;
		break;
	case 3:
		out->x = p;
		out->y = q;
		out->z = value;
		break;
	case 4:
		out->x = t;
		out->y = p;
		out->z = value;
		break;
	default:
		out->x = value;
		out->y = p;
		out->z = q;
		break;
	}

	out->w = 1.0f;
}

static void make_rainbow_color(float position, bool reverse, bool peak_cap, struct vec4 *out)
{
	position = clamp_float(position, 0.0f, 1.0f);
	if (reverse)
		position = 1.0f - position;

	hsv_to_rgb(position * 0.84f, 0.95f, 1.0f, out);

	if (peak_cap) {
		out->x = out->x * 0.45f + 0.55f;
		out->y = out->y * 0.45f + 0.55f;
		out->z = out->z * 0.45f + 0.55f;
		out->w = 1.0f;
	} else {
		out->w = 0.96f;
	}
}

static float vertical_color_position(float y, float height, float source_height)
{
	if (source_height <= 1.0f)
		return 0.0f;

	return clamp_float(1.0f - ((y + height * 0.5f) / source_height), 0.0f, 1.0f);
}

static void clear_levels(struct lightbar_source *lb)
{
	memset(lb->bars, 0, sizeof(lb->bars));
	memset(lb->caps, 0, sizeof(lb->caps));
	lb->analysis_frame_accumulator = 0;
}

static bool is_audio_source_candidate(obs_source_t *source, obs_source_t *self)
{
	if (!source || source == self)
		return false;

	return (obs_source_get_output_flags(source) & OBS_SOURCE_AUDIO) != 0;
}

static void attached_sources_clear(struct lightbar_source *lb)
{
	lb->attached_count = 0;
}

static bool attached_sources_push(struct lightbar_source *lb, obs_weak_source_t *weak)
{
	if (lb->attached_count == lb->attached_capacity) {
		const size_t new_capacity = lb->attached_capacity ? lb->attached_capacity * 2 : 8;
		obs_weak_source_t **new_items =
			brealloc(lb->attached_sources, new_capacity * sizeof(*new_items));

		if (!new_items)
			return false;

		lb->attached_sources = new_items;
		lb->attached_capacity = new_capacity;
	}

	lb->attached_sources[lb->attached_count++] = weak;
	return true;
}

static float goertzel_magnitude(const float *samples, uint32_t count, float frequency, uint32_t sample_rate)
{
	if (count == 0 || sample_rate == 0 || frequency <= 0.0f)
		return 0.0f;

	const float omega = TWO_PI * frequency / (float)sample_rate;
	const float coeff = 2.0f * cosf(omega);
	float q0 = 0.0f;
	float q1 = 0.0f;
	float q2 = 0.0f;

	for (uint32_t i = 0; i < count; i++) {
		q0 = coeff * q1 - q2 + samples[i];
		q2 = q1;
		q1 = q0;
	}

	float power = q1 * q1 + q2 * q2 - coeff * q1 * q2;
	if (power < 0.0f)
		power = 0.0f;

	return sqrtf(power) * (2.0f / (float)count);
}

static float magnitude_to_level(float magnitude, float noise_floor_db, float sensitivity)
{
	const float db = 20.0f * log10f(magnitude + 0.00000001f);
	float level = (db - noise_floor_db) / (0.0f - noise_floor_db);

	level = clamp_float(level * sensitivity, 0.0f, 1.0f);
	return powf(level, 0.72f);
}

static float max_frequency_probe(const float *samples, uint32_t count, uint32_t sample_rate, float f1, float f2)
{
	float magnitude = 0.0f;

	if (f1 > 0.0f)
		magnitude = goertzel_magnitude(samples, count, f1, sample_rate);

	if (f2 > 0.0f) {
		const float second = goertzel_magnitude(samples, count, f2, sample_rate);
		if (second > magnitude)
			magnitude = second;
	}

	return magnitude;
}

static void audio_capture_callback(void *param, obs_source_t *captured_source,
				   const struct audio_data *audio_data, bool muted)
{
	struct lightbar_source *lb = param;

	if (!lb || muted || !audio_data || audio_data->frames == 0)
		return;

	audio_t *audio = obs_get_audio();
	if (!audio)
		return;

	const uint32_t sample_rate = audio_output_get_sample_rate(audio);
	if (sample_rate == 0)
		return;

	size_t channels = audio_output_get_channels(audio);
	if (channels > MAX_AUDIO_CHANNELS)
		channels = MAX_AUDIO_CHANNELS;

	if (channels == 0)
		return;

	const uint32_t frames = audio_data->frames;
	uint32_t bar_count;
	double sensitivity;
	double min_frequency;
	double max_frequency;
	double noise_floor_db;
	bool ignore_outside_frequency_range;

	pthread_mutex_lock(&lb->lock);
	uint32_t configured_interval = lb->update_rate > 0 ? sample_rate / lb->update_rate : sample_rate / 45;
	if (configured_interval == 0)
		configured_interval = 1;
	lb->analysis_frame_accumulator += frames;

	if (lb->analysis_frame_accumulator < configured_interval) {
		pthread_mutex_unlock(&lb->lock);
		return;
	}

	lb->analysis_frame_accumulator -= configured_interval;
	bar_count = lb->bar_count;
	sensitivity = lb->sensitivity;
	min_frequency = lb->min_frequency;
	max_frequency = lb->max_frequency;
	noise_floor_db = lb->noise_floor_db;
	ignore_outside_frequency_range = lb->ignore_outside_frequency_range;
	pthread_mutex_unlock(&lb->lock);

	if (min_frequency < 20.0)
		min_frequency = 20.0;
	const double nyquist = (double)sample_rate * 0.48;
	if (max_frequency > nyquist)
		max_frequency = nyquist;
	if (max_frequency <= min_frequency)
		max_frequency = min_frequency * 2.0;

	const uint32_t sample_count = frames > MAX_ANALYSIS_SAMPLES ? MAX_ANALYSIS_SAMPLES : frames;
	const uint32_t start = frames - sample_count;
	float mono[MAX_ANALYSIS_SAMPLES];
	size_t active_channels = 0;

	for (size_t channel = 0; channel < channels; channel++) {
		if (audio_data->data[channel])
			active_channels++;
	}

	if (active_channels == 0)
		return;

	const float channel_scale = 1.0f / (float)active_channels;
	const float source_volume = obs_source_get_volume(captured_source);

	for (uint32_t frame = 0; frame < sample_count; frame++) {
		float mixed = 0.0f;

		for (size_t channel = 0; channel < channels; channel++) {
			const float *samples = (const float *)audio_data->data[channel];
			if (samples)
				mixed += samples[start + frame];
		}

		mono[frame] = mixed * channel_scale * source_volume;
	}

	float levels[MAX_BARS];
	const float log_min = logf((float)min_frequency);
	const float log_max = logf((float)max_frequency);
	const float log_range = log_max - log_min;

	for (uint32_t i = 0; i < bar_count; i++) {
		const float position = bar_count > 1 ? (float)i / (float)(bar_count - 1) : 0.0f;
		const float frequency = expf(log_min + log_range * position);
		const float magnitude = goertzel_magnitude(mono, sample_count, frequency, sample_rate);

		levels[i] = magnitude_to_level(magnitude, (float)noise_floor_db, (float)sensitivity);
	}

	if (!ignore_outside_frequency_range && bar_count > 0) {
		const float min_freq = (float)min_frequency;
		const float max_freq = (float)max_frequency;
		const float low_floor = 20.0f;
		const float high_ceiling = (float)nyquist;

		if (min_freq > low_floor) {
			const float low_mid = sqrtf(low_floor * min_freq);
			const float low_half = fmaxf(low_floor, min_freq * 0.5f);
			const float magnitude = max_frequency_probe(mono, sample_count, sample_rate, low_mid, low_half);
			const float level = magnitude_to_level(magnitude, (float)noise_floor_db, (float)sensitivity);

			if (level > levels[0])
				levels[0] = level;
		}

		if (max_freq < high_ceiling) {
			const float high_mid = sqrtf(max_freq * high_ceiling);
			const float high_next = fminf(high_ceiling, max_freq * 1.5f);
			const float magnitude =
				max_frequency_probe(mono, sample_count, sample_rate, high_mid, high_next);
			const float level = magnitude_to_level(magnitude, (float)noise_floor_db, (float)sensitivity);

			if (level > levels[bar_count - 1])
				levels[bar_count - 1] = level;
		}
	}

	pthread_mutex_lock(&lb->lock);
	for (uint32_t i = 0; i < bar_count; i++) {
		if (levels[i] > lb->bars[i])
			lb->bars[i] = levels[i];
		if (levels[i] > lb->caps[i])
			lb->caps[i] = levels[i];
	}
	pthread_mutex_unlock(&lb->lock);
}

static void detach_audio_sources_unlocked(struct lightbar_source *lb)
{
	for (size_t i = 0; i < lb->attached_count; i++) {
		obs_weak_source_t *weak = lb->attached_sources[i];
		obs_source_t *source = obs_weak_source_get_source(weak);

		if (source) {
			obs_source_remove_audio_capture_callback(source, audio_capture_callback, lb);
			obs_source_release(source);
		}

		obs_weak_source_release(weak);
	}

	attached_sources_clear(lb);
	lb->attached = false;
}

static void detach_audio_sources(struct lightbar_source *lb)
{
	pthread_mutex_lock(&lb->attach_lock);
	detach_audio_sources_unlocked(lb);
	pthread_mutex_unlock(&lb->attach_lock);
}

static bool attach_audio_source(struct lightbar_source *lb, obs_source_t *source)
{
	if (!is_audio_source_candidate(source, lb->source))
		return true;

	obs_weak_source_t *weak = obs_source_get_weak_source(source);
	if (!weak)
		return true;

	obs_source_add_audio_capture_callback(source, audio_capture_callback, lb);

	if (!attached_sources_push(lb, weak)) {
		obs_source_remove_audio_capture_callback(source, audio_capture_callback, lb);
		obs_weak_source_release(weak);
		return false;
	}

	return true;
}

static bool attach_audio_source_enum(void *param, obs_source_t *source)
{
	struct lightbar_source *lb = param;
	return attach_audio_source(lb, source);
}

static void refresh_audio_sources(struct lightbar_source *lb)
{
	pthread_mutex_lock(&lb->lock);
	const bool attach_all = lb->attach_all;
	char *source_name = bstrdup(lb->audio_source_name ? lb->audio_source_name : "");
	pthread_mutex_unlock(&lb->lock);

	pthread_mutex_lock(&lb->attach_lock);
	detach_audio_sources_unlocked(lb);

	if (attach_all) {
		obs_enum_sources(attach_audio_source_enum, lb);
	} else if (source_name && source_name[0] != '\0') {
		obs_source_t *source = obs_get_source_by_name(source_name);
		if (source) {
			attach_audio_source(lb, source);
			obs_source_release(source);
		}
	}

	lb->attached = lb->attached_count > 0;
	pthread_mutex_unlock(&lb->attach_lock);
	bfree(source_name);

	pthread_mutex_lock(&lb->lock);
	lb->refresh_accumulator = 0.0f;
	pthread_mutex_unlock(&lb->lock);
}

static bool add_audio_source_to_property(void *param, obs_source_t *source)
{
	struct list_audio_sources_param *list = param;

	if (!is_audio_source_candidate(source, list->self))
		return true;

	const char *name = obs_source_get_name(source);
	if (name && name[0] != '\0')
		obs_property_list_add_string(list->property, name, name);

	return true;
}

static const char *lightbar_get_name(void *unused)
{
	(void)unused;
	return "Audio Lightbar";
}

static void lightbar_update(void *data, obs_data_t *settings)
{
	struct lightbar_source *lb = data;
	const char *source_name = obs_data_get_string(settings, "audio_source");
	const uint32_t bar_count =
		clamp_u32((uint32_t)obs_data_get_int(settings, "bar_count"), MIN_BARS, MAX_BARS);

	pthread_mutex_lock(&lb->lock);
	bfree(lb->audio_source_name);
	lb->audio_source_name = bstrdup(source_name ? source_name : "");
	lb->attach_all = !lb->audio_source_name || lb->audio_source_name[0] == '\0';

	lb->width = clamp_u32((uint32_t)obs_data_get_int(settings, "width"), 32, 8192);
	lb->height = clamp_u32((uint32_t)obs_data_get_int(settings, "height"), 16, 8192);
	lb->bar_count = bar_count;
	lb->brick_rows =
		clamp_u32((uint32_t)obs_data_get_int(settings, "brick_rows"), MIN_BRICK_ROWS, MAX_BRICK_ROWS);
	lb->update_rate = clamp_u32((uint32_t)obs_data_get_int(settings, "update_rate"), 10, 120);
	lb->render_style = (int)obs_data_get_int(settings, "render_style");
	lb->sensitivity = obs_data_get_double(settings, "sensitivity");
	lb->decay = obs_data_get_double(settings, "decay");
	lb->cap_decay = obs_data_get_double(settings, "cap_decay");
	lb->min_frequency = obs_data_get_double(settings, "min_frequency");
	lb->max_frequency = obs_data_get_double(settings, "max_frequency");
	lb->noise_floor_db = obs_data_get_double(settings, "noise_floor_db");
	lb->show_background = obs_data_get_bool(settings, "show_background");
	lb->show_peak_caps = obs_data_get_bool(settings, "show_peak_caps");
	lb->mirror = obs_data_get_bool(settings, "mirror");
	lb->reverse_rainbow = obs_data_get_bool(settings, "reverse_rainbow");
	lb->vertical_rainbow = obs_data_get_bool(settings, "vertical_rainbow");
	lb->ignore_outside_frequency_range = obs_data_get_bool(settings, "ignore_outside_frequency_range");

	if (lb->sensitivity <= 0.0)
		lb->sensitivity = 1.0;
	if (lb->decay < 0.0)
		lb->decay = 0.0;
	if (lb->cap_decay < 0.0)
		lb->cap_decay = 0.0;
	if (lb->min_frequency <= 0.0)
		lb->min_frequency = DEFAULT_MIN_FREQUENCY;
	if (lb->max_frequency <= lb->min_frequency)
		lb->max_frequency = DEFAULT_MAX_FREQUENCY;
	if (lb->noise_floor_db >= -1.0)
		lb->noise_floor_db = DEFAULT_NOISE_FLOOR_DB;
	if (lb->render_style != STYLE_SMOOTH)
		lb->render_style = STYLE_BRICK;

	pthread_mutex_unlock(&lb->lock);

	if (obs_source_showing(lb->source))
		refresh_audio_sources(lb);
}

static void *lightbar_create(obs_data_t *settings, obs_source_t *source)
{
	struct lightbar_source *lb = bzalloc(sizeof(*lb));
	if (!lb)
		return NULL;

	lb->source = source;
	pthread_mutex_init(&lb->lock, NULL);
	pthread_mutex_init(&lb->attach_lock, NULL);
	lightbar_update(lb, settings);
	clear_levels(lb);

	return lb;
}

static void lightbar_destroy(void *data)
{
	struct lightbar_source *lb = data;
	if (!lb)
		return;

	detach_audio_sources(lb);
	bfree(lb->attached_sources);
	bfree(lb->audio_source_name);
	pthread_mutex_destroy(&lb->attach_lock);
	pthread_mutex_destroy(&lb->lock);
	bfree(lb);
}

static void lightbar_show(void *data)
{
	struct lightbar_source *lb = data;
	if (!lb)
		return;

	refresh_audio_sources(lb);
}

static void lightbar_hide(void *data)
{
	struct lightbar_source *lb = data;
	if (!lb)
		return;

	detach_audio_sources(lb);
}

static void lightbar_video_tick(void *data, float seconds)
{
	struct lightbar_source *lb = data;
	if (!lb)
		return;

	bool should_refresh = false;

	pthread_mutex_lock(&lb->lock);

	if (obs_source_showing(lb->source)) {
		lb->refresh_accumulator += seconds;
		if (lb->refresh_accumulator >= ATTACH_REFRESH_SECONDS)
			should_refresh = true;
	}

	const float decay_amount = (float)(lb->decay * seconds);
	const float cap_decay_amount = (float)(lb->cap_decay * seconds);
	for (uint32_t i = 0; i < lb->bar_count; i++) {
		lb->bars[i] = clamp_float(lb->bars[i] - decay_amount, 0.0f, 1.0f);
		lb->caps[i] = clamp_float(lb->caps[i] - cap_decay_amount, 0.0f, 1.0f);
		if (lb->caps[i] < lb->bars[i])
			lb->caps[i] = lb->bars[i];
	}

	pthread_mutex_unlock(&lb->lock);

	if (should_refresh)
		refresh_audio_sources(lb);
}

static void draw_solid_rect(float x, float y, float width, float height,
			    gs_effect_t *effect, gs_eparam_t *color_param, const struct vec4 *color)
{
	if (width <= 0.0f || height <= 0.0f)
		return;

	gs_effect_set_vec4(color_param, color);

	while (gs_effect_loop(effect, "Solid")) {
		gs_matrix_push();
		gs_matrix_translate3f(x, y, 0.0f);
		gs_matrix_scale3f(width, height, 1.0f);
		gs_draw_sprite(NULL, 0, 1, 1);
		gs_matrix_pop();
	}
}

static void lightbar_video_render(void *data, gs_effect_t *effect)
{
	struct lightbar_source *lb = data;
	(void)effect;

	if (!lb)
		return;

	float bars[MAX_BARS];
	float caps[MAX_BARS];
	uint32_t bar_count;
	uint32_t brick_rows;
	uint32_t source_width;
	uint32_t source_height;
	int render_style;
	bool show_background;
	bool show_peak_caps;
	bool mirror;
	bool reverse_rainbow;
	bool vertical_rainbow;

	pthread_mutex_lock(&lb->lock);
	memcpy(bars, lb->bars, sizeof(bars));
	memcpy(caps, lb->caps, sizeof(caps));
	bar_count = lb->bar_count;
	brick_rows = lb->brick_rows;
	source_width = lb->width;
	source_height = lb->height;
	render_style = lb->render_style;
	show_background = lb->show_background;
	show_peak_caps = lb->show_peak_caps;
	mirror = lb->mirror;
	reverse_rainbow = lb->reverse_rainbow;
	vertical_rainbow = lb->vertical_rainbow;
	pthread_mutex_unlock(&lb->lock);

	gs_effect_t *solid = obs_get_base_effect(OBS_EFFECT_SOLID);
	if (!solid)
		return;

	gs_eparam_t *color_param = gs_effect_get_param_by_name(solid, "color");
	if (!color_param)
		return;

	gs_blend_state_push();
	gs_enable_blending(true);
	gs_blend_function(GS_BLEND_SRCALPHA, GS_BLEND_INVSRCALPHA);

	if (show_background) {
		const struct vec4 background = {0.015f, 0.014f, 0.018f, 0.55f};
		draw_solid_rect(0.0f, 0.0f, (float)source_width, (float)source_height, solid,
				color_param, &background);
	}

	const float gap = bar_count > 120 ? 1.0f : 2.0f;
	const float total_gap = gap * (float)(bar_count - 1);
	const float bar_width = fmaxf(1.0f, ((float)source_width - total_gap) / (float)bar_count);
	const float drawable_height = mirror ? ((float)source_height * 0.5f) : (float)source_height;
	const float center_y = (float)source_height * 0.5f;
	const bool smooth = render_style == STYLE_SMOOTH;
	brick_rows = clamp_u32(brick_rows, MIN_BRICK_ROWS, MAX_BRICK_ROWS);

	float brick_gap =
		(!smooth && brick_rows > 1) ? fmaxf(1.0f, fminf(4.0f, drawable_height * 0.018f)) : 0.0f;
	float brick_height = (drawable_height - brick_gap * (float)(brick_rows - 1)) / (float)brick_rows;
	if (brick_height < 1.0f) {
		brick_gap = 0.0f;
		brick_height = drawable_height / (float)brick_rows;
	}

	for (uint32_t screen_index = 0; screen_index < bar_count; screen_index++) {
		const float value = clamp_float(bars[screen_index], 0.0f, 1.0f);
		const float cap_value = clamp_float(caps[screen_index], 0.0f, 1.0f);
		const float x = (float)screen_index * (bar_width + gap);
		const float horizontal_position =
			bar_count > 1 ? (float)screen_index / (float)(bar_count - 1) : 0.0f;
		uint32_t active_rows = (uint32_t)ceilf(value * (float)brick_rows);

		if (smooth && !vertical_rainbow) {
			struct vec4 color;
			struct vec4 cap_color;
			const float cap_height = fmaxf(2.0f, fminf(6.0f, (float)source_height * 0.035f));

			make_rainbow_color(horizontal_position, reverse_rainbow, false, &color);
			make_rainbow_color(horizontal_position, reverse_rainbow, true, &cap_color);

			if (value > 0.001f) {
				if (mirror) {
					const float half_height = fmaxf(1.0f, value * drawable_height);
					draw_solid_rect(x, center_y - half_height, bar_width, half_height, solid,
							color_param, &color);
					draw_solid_rect(x, center_y, bar_width, half_height, solid, color_param,
							&color);
				} else {
					const float bar_height = fmaxf(1.0f, value * drawable_height);
					draw_solid_rect(x, (float)source_height - bar_height, bar_width,
							bar_height, solid, color_param, &color);
				}
			}

			if (show_peak_caps && cap_value > 0.001f) {
				if (mirror) {
					const float cap_offset = cap_value * drawable_height;
					draw_solid_rect(x, center_y - cap_offset - cap_height, bar_width,
							cap_height, solid, color_param, &cap_color);
					draw_solid_rect(x, center_y + cap_offset, bar_width, cap_height, solid,
							color_param, &cap_color);
				} else {
					const float cap_y =
						(float)source_height - (cap_value * drawable_height) - cap_height;
					draw_solid_rect(x, cap_y, bar_width, cap_height, solid, color_param,
							&cap_color);
				}
			}

			continue;
		}

		if (active_rows > brick_rows)
			active_rows = brick_rows;

		for (uint32_t row = 0; row < active_rows; row++) {
			if (mirror) {
				const float top_y = center_y - ((float)(row + 1) * brick_height) -
						    ((float)row * brick_gap);
				const float bottom_y = center_y + ((float)row * (brick_height + brick_gap));
				const float top_position = vertical_rainbow
								   ? vertical_color_position(top_y, brick_height,
											     (float)source_height)
								   : horizontal_position;
				const float bottom_position = vertical_rainbow
								      ? vertical_color_position(bottom_y, brick_height,
												(float)source_height)
								      : horizontal_position;
				struct vec4 top_color;
				struct vec4 bottom_color;

				make_rainbow_color(top_position, reverse_rainbow, false, &top_color);
				make_rainbow_color(bottom_position, reverse_rainbow, false, &bottom_color);
				draw_solid_rect(x, top_y, bar_width, brick_height, solid, color_param, &top_color);
				draw_solid_rect(x, bottom_y, bar_width, brick_height, solid, color_param,
						&bottom_color);
			} else {
				const float y = (float)source_height - ((float)(row + 1) * brick_height) -
						((float)row * brick_gap);
				const float color_position = vertical_rainbow
								     ? vertical_color_position(y, brick_height,
											       (float)source_height)
								     : horizontal_position;
				struct vec4 color;

				make_rainbow_color(color_position, reverse_rainbow, false, &color);
				draw_solid_rect(x, y, bar_width, brick_height, solid, color_param, &color);
			}
		}

		if (show_peak_caps && cap_value > 0.001f) {
			uint32_t cap_row = (uint32_t)ceilf(cap_value * (float)brick_rows);
			if (cap_row == 0)
				cap_row = 1;
			if (cap_row > brick_rows)
				cap_row = brick_rows;
			cap_row--;

			if (mirror) {
				const float top_y = center_y - ((float)(cap_row + 1) * brick_height) -
						    ((float)cap_row * brick_gap);
				const float bottom_y = center_y + ((float)cap_row * (brick_height + brick_gap));
				const float top_position = vertical_rainbow
								   ? vertical_color_position(top_y, brick_height,
											     (float)source_height)
								   : horizontal_position;
				const float bottom_position = vertical_rainbow
								      ? vertical_color_position(bottom_y, brick_height,
												(float)source_height)
								      : horizontal_position;
				struct vec4 top_color;
				struct vec4 bottom_color;

				make_rainbow_color(top_position, reverse_rainbow, true, &top_color);
				make_rainbow_color(bottom_position, reverse_rainbow, true, &bottom_color);
				draw_solid_rect(x, top_y, bar_width, brick_height, solid, color_param, &top_color);
				draw_solid_rect(x, bottom_y, bar_width, brick_height, solid, color_param,
						&bottom_color);
			} else {
				const float cap_y = (float)source_height - ((float)(cap_row + 1) * brick_height) -
						    ((float)cap_row * brick_gap);
				const float color_position = vertical_rainbow
								     ? vertical_color_position(cap_y, brick_height,
											       (float)source_height)
								     : horizontal_position;
				struct vec4 cap_color;

				make_rainbow_color(color_position, reverse_rainbow, true, &cap_color);
				draw_solid_rect(x, cap_y, bar_width, brick_height, solid, color_param,
						&cap_color);
			}
		}
	}

	gs_blend_state_pop();
}

static uint32_t lightbar_get_width(void *data)
{
	struct lightbar_source *lb = data;
	if (!lb)
		return 900;

	pthread_mutex_lock(&lb->lock);
	const uint32_t width = lb->width;
	pthread_mutex_unlock(&lb->lock);

	return width;
}

static uint32_t lightbar_get_height(void *data)
{
	struct lightbar_source *lb = data;
	if (!lb)
		return 220;

	pthread_mutex_lock(&lb->lock);
	const uint32_t height = lb->height;
	pthread_mutex_unlock(&lb->lock);

	return height;
}

static void lightbar_get_defaults(obs_data_t *settings)
{
	obs_data_set_default_string(settings, "audio_source", "");
	obs_data_set_default_int(settings, "width", 900);
	obs_data_set_default_int(settings, "height", 220);
	obs_data_set_default_int(settings, "bar_count", DEFAULT_BARS);
	obs_data_set_default_int(settings, "brick_rows", DEFAULT_BRICK_ROWS);
	obs_data_set_default_int(settings, "update_rate", 45);
	obs_data_set_default_int(settings, "render_style", STYLE_BRICK);
	obs_data_set_default_double(settings, "sensitivity", 1.6);
	obs_data_set_default_double(settings, "decay", 3.0);
	obs_data_set_default_double(settings, "cap_decay", 0.85);
	obs_data_set_default_double(settings, "min_frequency", DEFAULT_MIN_FREQUENCY);
	obs_data_set_default_double(settings, "max_frequency", DEFAULT_MAX_FREQUENCY);
	obs_data_set_default_double(settings, "noise_floor_db", DEFAULT_NOISE_FLOOR_DB);
	obs_data_set_default_bool(settings, "show_background", true);
	obs_data_set_default_bool(settings, "show_peak_caps", true);
	obs_data_set_default_bool(settings, "mirror", false);
	obs_data_set_default_bool(settings, "reverse_rainbow", false);
	obs_data_set_default_bool(settings, "vertical_rainbow", false);
	obs_data_set_default_bool(settings, "ignore_outside_frequency_range", false);
}

static obs_properties_t *lightbar_get_properties(void *data)
{
	struct lightbar_source *lb = data;
	obs_properties_t *props = obs_properties_create();

	obs_property_t *source_list = obs_properties_add_list(props, "audio_source", "Audio Source",
							      OBS_COMBO_TYPE_LIST,
							      OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(source_list, "Auto: all audio sources", "");

	struct list_audio_sources_param list = {
		.property = source_list,
		.self = lb ? lb->source : NULL,
	};
	obs_enum_sources(add_audio_source_to_property, &list);

	obs_properties_add_int(props, "width", "Width", 32, 8192, 1);
	obs_properties_add_int(props, "height", "Height", 16, 8192, 1);
	obs_property_t *style_list = obs_properties_add_list(props, "render_style", "Render Style",
							     OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(style_list, "Stacked Brick", STYLE_BRICK);
	obs_property_list_add_int(style_list, "Smooth", STYLE_SMOOTH);
	obs_properties_add_int_slider(props, "bar_count", "Bars", MIN_BARS, MAX_BARS, 1);
	obs_properties_add_int_slider(props, "brick_rows", "Brick Rows", MIN_BRICK_ROWS, MAX_BRICK_ROWS, 1);
	obs_properties_add_int_slider(props, "update_rate", "Spectrum Updates Per Second", 10, 120, 1);
	obs_properties_add_float_slider(props, "sensitivity", "Sensitivity", 0.1, 8.0, 0.1);
	obs_properties_add_float_slider(props, "decay", "Bar Decay Per Second", 0.0, 6.0, 0.05);
	obs_properties_add_float_slider(props, "cap_decay", "Peak Cap Decay Per Second", 0.0, 3.0, 0.05);
	obs_properties_add_float(props, "min_frequency", "Lowest Frequency (default: 60 Hz)", 20.0, 2000.0, 1.0);
	obs_properties_add_float(props, "max_frequency", "Highest Frequency (default: 5 kHz)", 1000.0, 22000.0,
				 10.0);
	obs_properties_add_float_slider(props, "noise_floor_db", "Noise Floor dB", -96.0, -24.0, 1.0);
	obs_properties_add_bool(props, "ignore_outside_frequency_range",
				"Ignore Outside Frequency Range (default: off)");
	obs_properties_add_bool(props, "show_background", "Show Background");
	obs_properties_add_bool(props, "show_peak_caps", "Show Peak Caps");
	obs_properties_add_bool(props, "mirror", "Mirror From Center");
	obs_properties_add_bool(props, "reverse_rainbow", "Reverse Rainbow Order");
	obs_properties_add_bool(props, "vertical_rainbow", "Vertical Rainbow");

	return props;
}

static struct obs_source_info lightbar_source_info = {
	.id = SOURCE_ID,
	.type = OBS_SOURCE_TYPE_INPUT,
	.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW,
	.get_name = lightbar_get_name,
	.create = lightbar_create,
	.destroy = lightbar_destroy,
	.update = lightbar_update,
	.get_defaults = lightbar_get_defaults,
	.get_properties = lightbar_get_properties,
	.video_tick = lightbar_video_tick,
	.video_render = lightbar_video_render,
	.get_width = lightbar_get_width,
	.get_height = lightbar_get_height,
	.show = lightbar_show,
	.hide = lightbar_hide,
	.icon_type = OBS_ICON_TYPE_AUDIO_OUTPUT,
};

bool obs_module_load(void)
{
	obs_register_source(&lightbar_source_info);
	blog(LOG_INFO, "[audio-lightbar] loaded");
	return true;
}

const char *obs_module_name(void)
{
	return "Audio Lightbar";
}

const char *obs_module_description(void)
{
	return "A lightweight rainbow audio peak lightbar source for OBS.";
}

const char *obs_module_author(void)
{
	return "kokizzu";
}
