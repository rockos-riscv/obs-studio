#include <obs-avc.h>
#ifdef ENABLE_HEVC
#include <obs-hevc.h>
#endif

#include "obs-ffmpeg-video-encoders.h"

struct esmpp_encoder {
	struct ffmpeg_video_encoder ffve;
#ifdef ENABLE_HEVC
	bool hevc;
#endif
	DARRAY(uint8_t) header;
	DARRAY(uint8_t) sei;
	int64_t dts_offset; // Revert when FFmpeg fixes b-frame DTS calculation
};

#define ENCODER_NAME_H264 "H264 ESMPP"
static const char *h264_esmpp_getname(void *unused)
{
	UNUSED_PARAMETER(unused);
	blog(LOG_INFO, "h264_esmpp_getname %s", ENCODER_NAME_H264);
	return ENCODER_NAME_H264;
}

#ifdef ENABLE_HEVC
#define ENCODER_NAME_HEVC "HEVC ESMPP"
static const char *hevc_esmpp_getname(void *unused)
{
	UNUSED_PARAMETER(unused);
	return ENCODER_NAME_HEVC;
}
#endif

static inline bool valid_format(enum video_format format)
{
	switch (format) {
	case VIDEO_FORMAT_I420:
	case VIDEO_FORMAT_NV12:
	case VIDEO_FORMAT_YVYU:
	case VIDEO_FORMAT_UYVY:
	case VIDEO_FORMAT_I010:
	case VIDEO_FORMAT_P010:
		return true;
	default:
		return false;
	}
}

static void esmpp_video_info(void *data, struct video_scale_info *info)
{
	struct esmpp_encoder *enc = data;
	enum video_format pref_format;

	pref_format = obs_encoder_get_preferred_video_format(enc->ffve.encoder);

	if (!valid_format(pref_format)) {
		pref_format = valid_format(info->format) ? info->format
							 : VIDEO_FORMAT_NV12;
	}

	info->format = pref_format;
	blog(LOG_INFO, "esmpp_video_info fmt %d", pref_format);
}

static bool esmpp_update(struct esmpp_encoder *enc, obs_data_t *settings)
{
	const char *rc = obs_data_get_string(settings, "rate_control");
	int bitrate = (int)obs_data_get_int(settings, "bitrate");
	int cqp = (int)obs_data_get_int(settings, "cqp");
	int keyint_sec = (int)obs_data_get_int(settings, "keyint_sec");
	const char *profile = obs_data_get_string(settings, "profile");
	int bf = (int)obs_data_get_int(settings, "bf"); // num of b frames

	video_t *video = obs_encoder_video(enc->ffve.encoder);
	const struct video_output_info *voi = video_output_get_info(video);
	struct video_scale_info info;

	info.format = voi->format;
	info.colorspace = voi->colorspace;
	info.range = voi->range;

	esmpp_video_info(enc, &info);

	av_opt_set(enc->ffve.context->priv_data, "rc_mode", rc, 0);
	av_opt_set(enc->ffve.context->priv_data, "profile", profile, 0);

	av_opt_set_int(enc->ffve.context->priv_data, "v_stride_align",
		       base_get_alignment(), 0);

	if (astrcmpi(rc, "cqp") == 0) {
		bitrate = 0;
		enc->ffve.context->global_quality = cqp;
	} else if (astrcmpi(rc, "vbr") != 0) { /* CBR by default */
		av_opt_set_int(enc->ffve.context->priv_data, "cbr", true, 0);
		const int64_t rate = bitrate * INT64_C(1000);
		enc->ffve.context->rc_max_rate = rate;
		enc->ffve.context->rc_min_rate = rate;
		cqp = 0;
	}

	enc->ffve.context->max_b_frames = bf;

	const char *ffmpeg_opts = obs_data_get_string(settings, "ffmpeg_opts");
	ffmpeg_video_encoder_update(&enc->ffve, bitrate, keyint_sec, voi, &info,
				    ffmpeg_opts);

	blog(LOG_INFO,
	     "settings:\n"
	     "\tencoder:      %s\n"
	     "\trate_control: %s\n"
	     "\tbitrate:      %d\n"
	     "\tcqp:          %d\n"
	     "\tkeyint:       %d\n"
	     "\tprofile:      %s\n"
	     "\twidth:        %d\n"
	     "\theight:       %d\n"
		 "\tfps:          %u-%u\n",
	     enc->ffve.enc_name, rc, bitrate, cqp, enc->ffve.context->gop_size,
	     profile, enc->ffve.context->width, enc->ffve.height, voi->fps_num, voi->fps_den);

	// info("settings:\n"
	//      "\tencoder:      %s\n"
	//      "\trate_control: %s\n"
	//      "\tbitrate:      %d\n"
	//      "\tcqp:          %d\n"
	//      "\tkeyint:       %d\n"
	//      "\tpreset:       %s\n"
	//      "\ttuning:       %s\n"
	//      "\tmultipass:    %s\n"
	//      "\tprofile:      %s\n"
	//      "\twidth:        %d\n"
	//      "\theight:       %d\n"
	//      "\tb-frames:     %d\n",
	//      enc->ffve.enc_name, rc, bitrate, cqp, enc->ffve.context->gop_size,
	//      preset2, tuning, multipass, profile, enc->ffve.context->width,
	//      enc->ffve.height, enc->ffve.context->max_b_frames);

	return ffmpeg_video_encoder_init_codec(&enc->ffve);
}

static bool esmpp_reconfigure(void *data, obs_data_t *settings)
{
	struct esmpp_encoder *enc = data;
	blog(LOG_INFO, "esmpp_reconfigure");

	const int64_t bitrate = obs_data_get_int(settings, "bitrate");
	const char *rc = obs_data_get_string(settings, "rate_control");
	bool cbr = astrcmpi(rc, "CBR") == 0;
	bool vbr = astrcmpi(rc, "VBR") == 0;
	if (cbr || vbr) {
		const int64_t rate = bitrate * 1000;
		enc->ffve.context->bit_rate = rate;
		enc->ffve.context->rc_max_rate = rate;
	}

	return true;
}

static void esmpp_destroy(void *data)
{
	struct esmpp_encoder *enc = data;

	blog(LOG_INFO, "esmpp_destroy start");
	ffmpeg_video_encoder_free(&enc->ffve);
	da_free(enc->header);
	da_free(enc->sei);
	bfree(enc);
	blog(LOG_INFO, "esmpp_destroy end");
}

static void on_init_error(void *data, int ret)
{
	struct esmpp_encoder *enc = data;
	struct dstr error_message = {0};

	blog(LOG_ERROR, "on_init_error");

	dstr_copy(&error_message, obs_module_text("ESMPP.Error"));
	dstr_replace(&error_message, "%1", av_err2str(ret));
	dstr_cat(&error_message, "<br><br>");

	obs_encoder_set_last_error(enc->ffve.encoder, error_message.array);
	dstr_free(&error_message);
}

static void on_first_packet(void *data, AVPacket *pkt, struct darray *da)
{
	struct esmpp_encoder *enc = data;

	darray_free(da);
#ifdef ENABLE_HEVC
	if (enc->hevc) {
		obs_extract_hevc_headers(pkt->data, pkt->size,
					 (uint8_t **)&da->array, &da->num,
					 &enc->header.array, &enc->header.num,
					 &enc->sei.array, &enc->sei.num);
	} else
#endif
	{
		obs_extract_avc_headers(pkt->data, pkt->size,
					(uint8_t **)&da->array, &da->num,
					&enc->header.array, &enc->header.num,
					&enc->sei.array, &enc->sei.num);
	}
	da->capacity = da->num;
	blog(LOG_INFO, "on_first_packet header num %ld, da num %ld",
	     enc->header.num, da->num);
}

static void *esmpp_create_internal(obs_data_t *settings, obs_encoder_t *encoder,
				   bool hevc)
{
	struct esmpp_encoder *enc = bzalloc(sizeof(*enc));

#ifdef ENABLE_HEVC
	enc->hevc = hevc;
	if (hevc) {
		if (!ffmpeg_video_encoder_init(
			    &enc->ffve, enc, encoder, "hevc_esmpp_encoder",
			    "h264_esmpp_encoder", ENCODER_NAME_HEVC,
			    on_init_error, on_first_packet))
			goto fail;
	} else
#else
	UNUSED_PARAMETER(hevc);
#endif
	{
		if (!ffmpeg_video_encoder_init(
			    &enc->ffve, enc, encoder, "h264_esmpp_encoder",
			    "h264_esmpp_encoder", ENCODER_NAME_H264,
			    on_init_error, on_first_packet))
			goto fail;
	}

	if (!esmpp_update(enc, settings))
		goto fail;

	return enc;

fail:
	esmpp_destroy(enc);
	return NULL;
}

static void *esmpp_create(obs_data_t *settings, obs_encoder_t *encoder)
{
	void *enc = esmpp_create_internal(settings, encoder, false);
	if ((enc == NULL)) {
		blog(LOG_ERROR, "[ESMPP encoder] esmpp_create_internal failed");
	}
	blog(LOG_INFO, "esmpp_create %s", enc ? "success" : "fail");
	return enc;
}

static bool esmpp_encode(void *data, struct encoder_frame *frame,
			 struct encoder_packet *packet, bool *received_packet)
{
	struct esmpp_encoder *enc = data;

	if (!ffmpeg_video_encode(&enc->ffve, frame, packet, received_packet))
		return false;

	// packet->dts += enc->dts_offset;
	blog(LOG_DEBUG, "esmpp_encode received_packet %d",
	     received_packet ? 1 : 0);
	return true;
}

enum codec_type {
	CODEC_H264,
	CODEC_HEVC,
	CODEC_AV1,
};

static void esmpp_defaults_base(enum codec_type codec, obs_data_t *settings)
{
	blog(LOG_INFO, "esmpp_defaults_base set");
	obs_data_set_default_int(settings, "bitrate", 2500);
	obs_data_set_default_int(settings, "max_bitrate", 5000);
	obs_data_set_default_int(settings, "keyint_sec", 0);
	obs_data_set_default_int(settings, "cqp", 20);
	obs_data_set_default_string(settings, "rate_control", "CBR");
	obs_data_set_default_string(settings, "profile",
				    codec != CODEC_H264 ? "main" : "high");
	obs_data_set_default_int(settings, "bf", 0);
}

void h264_esmpp_defaults(obs_data_t *settings)
{
	esmpp_defaults_base(CODEC_H264, settings);
}

void hevc_esmpp_defaults(obs_data_t *settings)
{
	esmpp_defaults_base(CODEC_HEVC, settings);
}

static bool rate_control_modified(obs_properties_t *ppts, obs_property_t *p,
				  obs_data_t *settings)
{
	const char *rc = obs_data_get_string(settings, "rate_control");
	bool cqp = astrcmpi(rc, "CQP") == 0;
	bool vbr = astrcmpi(rc, "VBR") == 0;

	p = obs_properties_get(ppts, "bitrate");
	obs_property_set_visible(p, !cqp);
	p = obs_properties_get(ppts, "max_bitrate");
	obs_property_set_visible(p, vbr);
	p = obs_properties_get(ppts, "cqp");
	obs_property_set_visible(p, cqp);

	return true;
}

obs_properties_t *esmpp_properties_internal(enum codec_type codec)
{
	obs_properties_t *props = obs_properties_create();
	obs_property_t *p;

	blog(LOG_INFO, "esmpp_properties");
	p = obs_properties_add_list(props, "rate_control",
				    obs_module_text("RateControl"),
				    OBS_COMBO_TYPE_LIST,
				    OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(p, "CBR", "CBR");
	obs_property_list_add_string(p, "CQP", "CQP");
	obs_property_list_add_string(p, "VBR", "VBR");

	obs_property_set_modified_callback(p, rate_control_modified);

	p = obs_properties_add_int(props, "bitrate", obs_module_text("Bitrate"),
				   50, 300000, 50);
	obs_property_int_set_suffix(p, " Kbps");
	p = obs_properties_add_int(props, "max_bitrate",
				   obs_module_text("MaxBitrate"), 50, 300000,
				   50);
	obs_property_int_set_suffix(p, " Kbps");

	obs_properties_add_int(props, "cqp", obs_module_text("ESMPP.CQLevel"),
			       1, 51, 1);

	p = obs_properties_add_int(props, "keyint_sec",
				   obs_module_text("KeyframeIntervalSec"), 0,
				   10, 1);
	obs_property_int_set_suffix(p, " s");

	p = obs_properties_add_list(props, "profile",
				    obs_module_text("Profile"),
				    OBS_COMBO_TYPE_LIST,
				    OBS_COMBO_FORMAT_STRING);

#define add_profile(val) obs_property_list_add_string(p, val, val)
	if (codec == CODEC_HEVC) {
		add_profile("main10");
		add_profile("main");
	} else {
		add_profile("high");
		add_profile("main");
		add_profile("baseline");
	}
#undef add_profile

	obs_properties_add_int(props, "bf", obs_module_text("BFrames"), 0, 4,
			       1);

	return props;
}

obs_properties_t *h264_esmpp_properties_ffmpeg(void *unused)
{
	UNUSED_PARAMETER(unused);
	return esmpp_properties_internal(CODEC_H264);
}

#ifdef ENABLE_HEVC
obs_properties_t *hevc_esmpp_properties_ffmpeg(void *unused)
{
	UNUSED_PARAMETER(unused);
	return esmpp_properties_internal(CODEC_HEVC);
}
#endif

static bool esmpp_extra_data(void *data, uint8_t **extra_data, size_t *size)
{
	struct esmpp_encoder *enc = data;
	blog(LOG_INFO, "esmpp_extra_data get");

	*extra_data = enc->header.array;
	*size = enc->header.num;

	return true;
}

struct obs_encoder_info h264_esmpp_encoder_info = {
	.id = "ffmpeg_h264_esmpp",
	.type = OBS_ENCODER_VIDEO,
	.codec = "h264",
	.get_name = h264_esmpp_getname,
	.create = esmpp_create,
	.destroy = esmpp_destroy,
	.encode = esmpp_encode,
	.get_defaults = h264_esmpp_defaults,
	.get_properties = h264_esmpp_properties_ffmpeg,
	.get_extra_data = esmpp_extra_data,
	.get_video_info = esmpp_video_info,
};

#ifdef ENABLE_HEVC
struct obs_encoder_info hevc_esmpp_encoder_info = {
	.id = "ffmpeg_hevc_esmpp",
	.type = OBS_ENCODER_VIDEO,
	.codec = "hevc",
	.get_name = hevc_esmpp_getname,
	.create = esmpp_create,
	.destroy = esmpp_destroy,
	.encode = esmpp_encode,
	.get_defaults = hevc_esmpp_defaults,
	.get_properties = hevc_esmpp_properties_ffmpeg,
	.get_extra_data = esmpp_extra_data,
	.get_video_info = esmpp_video_info,
};
#endif