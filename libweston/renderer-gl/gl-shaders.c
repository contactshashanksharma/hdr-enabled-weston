/*
 * Copyright © 2019 Harish Krupo
 * Copyright © 2019 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial
 * portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT.  IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include <stdlib.h>
#include <string.h>
#include <assert.h>


#include <libweston/libweston.h>
#include "gl-renderer-private.h"
#include "shared/helpers.h"
#include "libweston/weston-log.h"

struct gl_shader_generator {
	struct weston_log_scope *debug;
};

static const char vertex_shader[] =
	"uniform mat4 proj;\n"
	"attribute vec2 position;\n"
	"attribute vec2 texcoord;\n"
	"varying vec2 v_texcoord;\n"
	"void main()\n"
	"{\n"
	"   gl_Position = proj * vec4(position, 0.0, 1.0);\n"
	"   v_texcoord = texcoord;\n"
	"}\n";

#define FRAGMENT_CONVERT_YUV						\
	"    y *= alpha;\n"						\
	"    u *= alpha;\n"						\
	"    v *= alpha;\n"						\
	"    gl_FragColor.r = y + 1.59602678 * v;\n"			\
	"    gl_FragColor.g = y - 0.39176229 * u - 0.81296764 * v;\n"	\
	"    gl_FragColor.b = y + 2.01723214 * u;\n"			\
	"    gl_FragColor.a = alpha;\n"

static const char external_extension[] =
	"#extension GL_OES_EGL_image_external : require\n";

static const char fragment_header[] =
	"precision mediump float;\n"
	"varying vec2 v_texcoord;\n"
	"uniform float alpha;\n";

static const char uniform_tex_external[] = "uniform samplerExternalOES tex;\n";

static const char uniform_color[] = "uniform vec4 color;\n";

static const char uniform_tex2[] = "uniform sampler2D tex2;\n";

static const char uniform_tex1[] = "uniform sampler2D tex1;\n";

static const char uniform_tex[] = "uniform sampler2D tex;\n";

static const char fragment_main_open[] =
	"void main()\n"
	"{\n";

static const char fragment_debug[] =
	"    gl_FragColor = vec4(0.0, 0.3, 0.0, 0.2) + gl_FragColor * 0.8;\n";

static const char texture_fragment_shader_rgba[] =
	"    gl_FragColor = alpha * texture2D(tex, v_texcoord);\n"
	;

static const char texture_fragment_shader_rgbx[] =
	"    gl_FragColor.rgb = alpha * texture2D(tex, v_texcoord).rgb;\n"
	"    gl_FragColor.a = alpha;\n"
	;

static const char texture_fragment_shader_external[] =
	"    gl_FragColor = alpha * texture2D(tex, v_texcoord);\n"
	;

static const char texture_fragment_shader_y_uv[] =
	"    float y = 1.16438356 * (texture2D(tex, v_texcoord).x - 0.0625);\n"
	"    vec2 uv = texture2D(tex1, v_texcoord).rg;\n"
	"    float u = uv.r - 0.5;\n"
	"    float v = uv.g - 0.5;\n"
	FRAGMENT_CONVERT_YUV
	;

static const char texture_fragment_shader_y_u_v[] =
	"    float y = 1.16438356 * (texture2D(tex, v_texcoord).x - 0.0625);\n"
	"    float u = texture2D(tex1, v_texcoord).x - 0.5;\n"
	"    float v = texture2D(tex2, v_texcoord).x - 0.5;\n"
	FRAGMENT_CONVERT_YUV
	;

static const char texture_fragment_shader_y_xuxv[] =
	"    float y = 1.16438356 * (texture2D(tex, v_texcoord).x - 0.0625);\n"
	"    vec2 uv = texture2D(tex1, v_texcoord).ga;\n"
	"    float u = uv.r - 0.5;\n"
	"    float v = uv.g - 0.5;\n"
	FRAGMENT_CONVERT_YUV
	;

static const char texture_fragment_shader_y_xyuv[] =
	"    float y = 1.16438356 * (texture2D(tex, v_texcoord).b - 0.0625);\n"
	"    float u = texture2D(tex, v_texcoord).g - 0.5;\n"
	"    float v = texture2D(tex, v_texcoord).r - 0.5;\n"
	FRAGMENT_CONVERT_YUV
	;

static const char solid_fragment_shader[] =
	"    gl_FragColor = alpha * color;\n"
	;

static const char fragment_brace[] = "}\n";

/* eotfs */
static const char eotf_srgb[] =
	"float eotf_srgb_single(float c) {\n"
	"    return c < 0.04045 ? c / 12.92 : pow(((c + 0.055) / 1.055), 2.4);\n"
	"}\n"
	"\n"
	"vec3 eotf_srgb(vec3 color) {\n"
	"    float r = eotf_srgb_single(color.r);\n"
	"    float g = eotf_srgb_single(color.g);\n"
	"    float b = eotf_srgb_single(color.b);\n"
	"    return vec3(r, g, b);\n"
	"}\n"
	"\n"
	"vec3 eotf(vec3 color) {\n"
	"    return sign(color) * eotf_srgb(abs(color.rgb));\n"
	"}\n"
	"\n"
	;

static const char eotf_pq[] =
	"vec3 eotf(vec3 v) {\n"
	"    float m1 = 0.25 * 2610.0 / 4096.0;\n"
	"    float m2 = 128.0 * 2523.0 / 4096.0;\n"
	"    float c3 = 32.0 * 2392.0 / 4096.0;\n"
	"    float c2 = 32.0 * 2413.0 / 4096.0;\n"
	"    float c1 = c3 - c2 + 1.0;\n"
	"    vec3 n = pow(v, vec3(1.0 / m2));\n"
	"    return pow(max(n - c1, 0.0) / (c2 - c3 * n), vec3(1.0 / m1));\n"
	"}\n"
	"\n"
	;

static const char eotf_hlg[] =
	"vec3 eotf(vec3 l) {\n"
	"    float a = 0.17883277;\n"
	"    float b = 1.0 - 4.0 * a;\n"
	"    float c = 0.5 - a * log(4.0 * a);\n"
	"    float x = step(1.0 / 2.0, l);\n"
	"    vec3 v0 = pow(l, 2.0) / 3.0;\n"
	"    vec3 v1 = (exp((l - c) / a) + b) / 12.0;\n"
	"    return mix(v0, v1, x);\n"
	"}\n"
	"\n"
	;

static const char eotf_default[] =
	"vec3 eotf(vec3 color) {\n"
	"    return color;\n"
	"}\n"
	"\n"
	;

/* oetfs */
static const char oetf_srgb[] =
	"float oetf_srgb_single(float c) {\n"
	"    float ret = 0.0;\n"
	"    if (c < 0.0031308) {\n"
	"        ret = 12.92 * c;\n"
	"    } else {\n"
	"        ret = 1.055 * pow(c, 1.0 / 2.4) - 0.055;\n"
	"    }\n"
	"    return ret;\n"
	"}\n"
	"\n"
	"vec3 oetf_srgb(vec3 color) {\n"
	"    float r = oetf_srgb_single(color.r);\n"
	"    float g = oetf_srgb_single(color.g);\n"
	"    float b = oetf_srgb_single(color.b);\n"
	"    return vec3(r, g, b);\n"
	"}\n"
	"\n"
	"vec3 oetf(vec3 linear) {\n"
	"    return sign(linear) * oetf_srgb(abs(linear.rgb));\n"
	"}\n"
	"\n"
	;

static const char oetf_pq[] =
	"vec3 oetf(vec3 l) {\n"
	"    float m1 = 0.25 * 2610.0 / 4096.0;\n"
	"    float m2 = 128.0 * 2523.0 / 4096.0;\n"
	"    float c3 = 32.0 * 2392.0 / 4096.0;\n"
	"    float c2 = 32.0 * 2413.0 / 4096.0;\n"
	"    float c1 = c3 - c2 + 1.0;\n"
	"    vec3 n = pow(l, vec3(m1));\n"
	"    return pow((c1 + c2 * n) / (1.0 + c3 * n), vec3(m2));\n"
	"}\n"
	"\n"
	;

static const char oetf_hlg[] =
	"vec3 oetf(vec3 l) {\n"
	"    float a = 0.17883277;\n"
	"    float b = 1.0 - 4.0 * a;\n"
	"    float c = 0.5 - a * log(4.0 * a);\n"
	"    float x = step(1.0 / 12.0, l);\n"
	"    vec3 v0 = a * log(12.0 * l - b) + c;\n"
	"    vec3 v1 = sqrt(3.0 * l);\n"
	"    return mix(v0, v1, x);\n"
	"}\n"
	"\n"
	;

static const char oetf_default[] =
	"vec3 oetf(vec3 color) {\n"
	"    return color;\n"
	"}\n"
	"\n"
	;

static const char eotf_shader[] =
	"    gl_FragColor.rgb = eotf(gl_FragColor.rgb);\n"
	;

static const char oetf_shader[] =
	"    gl_FragColor.rgb = oetf(gl_FragColor.rgb);\n"
	;

static const char csc_shader[] =
	"    gl_FragColor.rgb = clamp((csc * gl_FragColor.rgb), 0.0, 1.0);\n"
	;

static const char hdr_uniforms[] =
	"uniform float display_max_luminance;\n"
	"uniform float content_max_luminance;\n"
	"uniform float content_min_luminance;\n"
	;

#define LUMINANCE_FROM_RGB					\
	"    // These are ITU 2100 recommendations\n"		\
	"    float kr = 0.2627;\n"				\
	"    float kb = 0.0593;\n"				\
	"    float kg = 1.0 - kr - kb;\n"			\
	"    float luma = dot(color, vec3(kr, kg, kb));\n"	\

/* Luminance scaling */

static const char sl_srgb[] =
	"vec3 ScaleLuminance(vec3 color) {\n"
	"    return color * display_max_luminance;\n"
	"}\n"
	"\n"
	;

static const char sl_pq[] =
	"vec3 ScaleLuminance(vec3 color) {\n"
	"    return color * 10000.0;\n"
	"}\n"
	"\n"
	;

static const char sl_hlg[] =
	"vec3 ScaleLuminance(vec3 color) {\n"
	LUMINANCE_FROM_RGB
	"    return color * 1000.0 * pow(luma, 0.2);\n"
	"}\n"
	"\n"
	;

/* Luminance Normalization */

static const char nl_srgb[] =
	"vec3 NormalizeLuminance(vec3 color) {\n"
	"    return color / display_max_luminance;\n"
	"}\n"
	"\n"
	;

static const char nl_pq[] =
	"vec3 NormalizeLuminance(vec3 color) {\n"
	"    return color / 10000.0;\n"
	"}\n"
	"\n"
	;

static const char nl_hlg[] =
	"vec3 NormalizeLuminance(vec3 color) {\n"
	LUMINANCE_FROM_RGB
	"    return (color / 1000.0) * pow(luma, -0.2);\n"
	"}\n"
	"\n"
	;

static const char sl_shader[] =
	"    gl_FragColor.rgb = ScaleLuminance(gl_FragColor.rgb);\n"
	;

static const char nl_shader[] =
	"    gl_FragColor.rgb = NormalizeLuminance(gl_FragColor.rgb);\n"
	;

/* Tone mapping Shaders */

static const char hdr_shader[] =
	"    gl_FragColor.rgb = tone_mapping(gl_FragColor.rgb);\n"
	;

/* No tone mapping */
static const char noop_tm[] =
	"vec3 tone_mapping(vec3 color) {\n"
	"    return color;\n"
	"}\n"
	"\n"
	;

/* HDR->SDR */
static const char hdr_to_sdr_tm[] =
	"vec3 hable_curve(vec3 c) {\n"
	"    float A = 0.15;\n"
	"    float B = 0.50;\n"
	"    float C = 0.10;\n"
	"    float D = 0.20;\n"
	"    float E = 0.02;\n"
	"    float F = 0.30;\n"
	"    vec3 numerator = (c * (A * c + C * B) + D * E);\n"
	"    vec3 denominator = (c * (A * c + B) + D * F);\n"
	"    c = (numerator / denominator) - E / F;\n"
	"    return c;\n"
	"}\n"
	"\n"
	"vec3 tone_mapping(vec3 color) {\n"
	"    float W = 11.2;\n"
	"    float exposure = 100.0;\n"
	"    color *= exposure;\n"
	"    color = hable_curve(color);\n"
	"    float white = hable_curve(vec3(W, 0, 0)).x;\n"
	"    color /= white;\n"
	"    return color;\n"
	"}\n"
	"\n"
	;

static const char sdr_to_hdr_tm[] =
	"vec3 tone_mapping(vec3 color) {\n"
	LUMINANCE_FROM_RGB
	"    highp float tone_mapped_luma = 0.0;"
	"\n"
	"    if (luma > 5.0) {\n"
	"        tone_mapped_luma = luma / display_max_luminance;\n"
	"        tone_mapped_luma = pow(tone_mapped_luma, 1.5);\n"
	"        tone_mapped_luma *= display_max_luminance;\n"
	"        color *= tone_mapped_luma / luma;\n"
	"    }\n"
	"    return color;\n"
	"}\n"
	"\n"
	;

static const char hdr_to_hdr_tm[] =
	"vec3 tone_mapping(vec3 color) {\n"
	"    float range = content_max_luminance - content_min_luminance;\n"
	LUMINANCE_FROM_RGB
	"    float tone_mapped_luma = luma - content_min_luminance;\n"
	"    tone_mapped_luma /= range;\n"
	"    tone_mapped_luma *= display_max_luminance;\n"
	"    color *= tone_mapped_luma / luma;\n"
	"    return color;\n"
	"}\n"
	"\n"
	;

struct gl_shader_source {
	const char *parts[64];
	uint32_t len;
};

static inline void
gl_shader_source_add(struct gl_shader_source *shader_source, const char *str)
{
	shader_source->parts[shader_source->len++] = str;
	assert(shader_source->len < ARRAY_LENGTH(shader_source->parts));
}

static void
generate_fs_hdr_shader(struct gl_shader_source *shader_source,
		       struct gl_shader_requirements *requirements)
{
	// Write the hdr uniforms
	if (requirements->csc_matrix)
		gl_shader_source_add(shader_source, "uniform mat3 csc;\n");

	gl_shader_source_add(shader_source, hdr_uniforms);

	// Choose the EOTF
	switch (requirements->degamma) {
	case SHADER_DEGAMMA_SRGB:
		gl_shader_source_add(shader_source, eotf_srgb);
		gl_shader_source_add(shader_source, sl_srgb);
		break;
	case SHADER_DEGAMMA_PQ:
		gl_shader_source_add(shader_source, eotf_pq);
		gl_shader_source_add(shader_source, sl_pq);
		break;
	case SHADER_DEGAMMA_HLG:
		gl_shader_source_add(shader_source, eotf_hlg);
		gl_shader_source_add(shader_source, sl_hlg);
		break;
	default:
		gl_shader_source_add(shader_source, eotf_default);
		break;
	}

	// Choose the OETF
	switch (requirements->gamma | requirements->nl_variant) {
	case SHADER_GAMMA_SRGB:
		gl_shader_source_add(shader_source, oetf_srgb);
		gl_shader_source_add(shader_source, nl_srgb);
		break;
	case SHADER_GAMMA_PQ:
		gl_shader_source_add(shader_source, oetf_pq);
		gl_shader_source_add(shader_source, nl_pq);
		break;
	case SHADER_GAMMA_HLG:
		gl_shader_source_add(shader_source, oetf_hlg);
		gl_shader_source_add(shader_source, nl_hlg);
		break;
	default:
		gl_shader_source_add(shader_source, oetf_default);
		break;
	}

	// Pick the tone mapping shader variant
	switch (requirements->tone_mapping) {
	case SHADER_TONE_MAP_NONE:
		gl_shader_source_add(shader_source, noop_tm);
		break;
	case SHADER_TONE_MAP_HDR_TO_SDR:
		gl_shader_source_add(shader_source, hdr_to_sdr_tm);
		break;
	case SHADER_TONE_MAP_SDR_TO_HDR:
		gl_shader_source_add(shader_source, sdr_to_hdr_tm);
		break;
	case SHADER_TONE_MAP_HDR_TO_HDR:
		gl_shader_source_add(shader_source, hdr_to_hdr_tm);
		break;
	}
}

static void
generate_hdr_process_shader(struct gl_shader_source *shader_source,
			    struct gl_shader_requirements *requirements)
{
	uint32_t need_range_increment =
		(requirements->tone_mapping == SHADER_TONE_MAP_HDR_TO_HDR) ||
		(requirements->tone_mapping == SHADER_TONE_MAP_SDR_TO_HDR);

	if (requirements->degamma)
		gl_shader_source_add(shader_source, eotf_shader);

	if (requirements->csc_matrix)
		gl_shader_source_add(shader_source, csc_shader);

	if (requirements->degamma && need_range_increment)
		gl_shader_source_add(shader_source, sl_shader);

	if (requirements->tone_mapping)
		gl_shader_source_add(shader_source, hdr_shader);

	if (requirements->nl_variant)
		gl_shader_source_add(shader_source, nl_shader);

	if (requirements->gamma)
		gl_shader_source_add(shader_source, oetf_shader);

}

static void
generate_fs_uniforms(struct gl_shader_source *shader_source,
		     struct gl_shader_requirements *requirements)
{
	/* write the header */
	/* require extension for EXTERNAL SHADERS: */
	if (requirements->variant == SHADER_VARIANT_EXTERNAL) {
		gl_shader_source_add(shader_source, external_extension);
	}

	gl_shader_source_add(shader_source, fragment_header);

	/* Generate uniforms based on variant */
	switch (requirements->variant) {
	case SHADER_VARIANT_EXTERNAL:
		gl_shader_source_add(shader_source, uniform_tex_external);
		break;
	case SHADER_VARIANT_SOLID:
		gl_shader_source_add(shader_source, uniform_color);
		break;
	case SHADER_VARIANT_Y_U_V:
		gl_shader_source_add(shader_source, uniform_tex2);

		/* fallthrough */
	case SHADER_VARIANT_Y_UV:
	case SHADER_VARIANT_Y_XUXV:
	case SHADER_VARIANT_Y_XYUV:
		gl_shader_source_add(shader_source, uniform_tex1);

		/* fallthrough */
	case SHADER_VARIANT_RGBX:
	case SHADER_VARIANT_RGBA:
		/* fallthrough */
	default:
		gl_shader_source_add(shader_source, uniform_tex);
		break;
	}

}

static void
generate_fs_variants(struct gl_shader_source *shader_source,
		     struct gl_shader_requirements *requirements)
{
	/* Generate the shader based on variant */
	switch (requirements->variant) {
	case SHADER_VARIANT_Y_U_V:
		gl_shader_source_add(shader_source,
				     texture_fragment_shader_y_u_v);
		break;
	case SHADER_VARIANT_Y_UV:
		gl_shader_source_add(shader_source,
				     texture_fragment_shader_y_uv);
		break;
	case SHADER_VARIANT_Y_XUXV:
		gl_shader_source_add(shader_source,
				     texture_fragment_shader_y_xuxv);
		break;
	case SHADER_VARIANT_Y_XYUV:
		gl_shader_source_add(shader_source,
				     texture_fragment_shader_y_xyuv);
		break;
	case SHADER_VARIANT_RGBX:
		gl_shader_source_add(shader_source,
				     texture_fragment_shader_rgbx);
		break;
	case SHADER_VARIANT_RGBA:
		gl_shader_source_add(shader_source,
				     texture_fragment_shader_rgba);
		break;
	case SHADER_VARIANT_EXTERNAL:
		gl_shader_source_add(shader_source,
				     texture_fragment_shader_external);
		break;
	case SHADER_VARIANT_SOLID:
		gl_shader_source_add(shader_source, solid_fragment_shader);
		break;
	case SHADER_VARIANT_NONE:
		break;
	}

}

static void
log_shader(struct gl_shader_generator *sg,
	   struct gl_shader_source *shader_source)
{
	char *str;
	FILE *fp;
	size_t len;
	uint32_t i;

	fp = open_memstream(&str, &len);
	assert(fp);

	fprintf(fp, "Generated shader length: %d, shader:\n", shader_source->len);
	for(i = 0; i < shader_source->len; i++) {
		fprintf(fp, "%s", shader_source->parts[i]);
	}
	fprintf(fp, "\n");
	fclose(fp);

	weston_log_scope_printf(sg->debug, "%s", str);
	free(str);
}

static void
generate_fragment_shader(struct gl_shader_generator *sg,
			 struct gl_shader_source *shader_source,
			 struct gl_shader_requirements *requirements)
{
	/* Write the header and required uniforms */
	generate_fs_uniforms(shader_source, requirements);

	// Write shaders needed for HDR
	generate_fs_hdr_shader(shader_source, requirements);

	/* begin main function */
	gl_shader_source_add(shader_source, fragment_main_open);

	/* Generate the shader based on variant */
	generate_fs_variants(shader_source, requirements);

	generate_hdr_process_shader(shader_source, requirements);

	if (requirements->debug)
		gl_shader_source_add(shader_source, fragment_debug);

	gl_shader_source_add(shader_source, fragment_brace);

	log_shader(sg, shader_source);
}

void
gl_shader_requirements_init(struct gl_shader_requirements *requirements)
{
	memset(requirements, 0, sizeof(struct gl_shader_requirements));
}

void
gl_shader_destroy(struct gl_shader *shader)
{
	glDeleteShader(shader->vertex_shader);
	glDeleteShader(shader->fragment_shader);
	glDeleteProgram(shader->program);

	shader->vertex_shader = 0;
	shader->fragment_shader = 0;
	shader->program = 0;
	wl_list_remove(&shader->link);
	free(shader);
}

static int
compile_shader(GLenum type, int count, const char **sources)
{
	GLuint s;
	char msg[512];
	GLint status;

	s = glCreateShader(type);
	glShaderSource(s, count, sources, NULL);
	glCompileShader(s);
	glGetShaderiv(s, GL_COMPILE_STATUS, &status);
	if (!status) {
		glGetShaderInfoLog(s, sizeof msg, NULL, msg);
		weston_log("shader info: %s\n", msg);
		return GL_NONE;
	}

	return s;
}

struct gl_shader *
gl_shader_create(struct gl_shader_generator *sg,
		 struct gl_shader_requirements *requirements)
{
	struct gl_shader *shader = NULL;
	char msg[512];
	GLint status;
	const char *vertex_source[1];
	struct gl_shader_source fragment_source;

	shader = zalloc(sizeof *shader);
	if (!shader) {
		weston_log("could not create shader\n");
		return NULL;
	}

	memcpy(&shader->key, requirements,
	       sizeof(struct gl_shader_requirements));

	vertex_source[0] = vertex_shader;

	fragment_source.len = 0;
	generate_fragment_shader(sg, &fragment_source, requirements);

	shader->vertex_shader = compile_shader(GL_VERTEX_SHADER, 1,
					       vertex_source);

	shader->fragment_shader = compile_shader(GL_FRAGMENT_SHADER,
						 fragment_source.len,
						 fragment_source.parts);

	shader->program = glCreateProgram();
	glAttachShader(shader->program, shader->vertex_shader);
	glAttachShader(shader->program, shader->fragment_shader);
	glBindAttribLocation(shader->program, 0, "position");
	glBindAttribLocation(shader->program, 1, "texcoord");

	glLinkProgram(shader->program);
	glGetProgramiv(shader->program, GL_LINK_STATUS, &status);
	if (!status) {
		glGetProgramInfoLog(shader->program, sizeof msg, NULL, msg);
		weston_log("link info: %s\n", msg);
		return NULL;
	}

	shader->proj_uniform = glGetUniformLocation(shader->program, "proj");
	shader->tex_uniforms[0] = glGetUniformLocation(shader->program, "tex");
	shader->tex_uniforms[1] = glGetUniformLocation(shader->program, "tex1");
	shader->tex_uniforms[2] = glGetUniformLocation(shader->program, "tex2");
	shader->alpha_uniform = glGetUniformLocation(shader->program, "alpha");
	shader->color_uniform = glGetUniformLocation(shader->program, "color");
	shader->csc_uniform = glGetUniformLocation(shader->program, "csc");
	shader->display_max_luminance =
		glGetUniformLocation(shader->program, "display_max_luminance");
	shader->content_max_luminance =
		glGetUniformLocation(shader->program, "content_max_luminance");
	shader->content_min_luminance =
		glGetUniformLocation(shader->program, "content_min_luminance");

	return shader;
}

struct gl_shader_generator *
gl_shader_generator_create(struct weston_compositor *compositor)
{
	struct gl_shader_generator *sg = zalloc(sizeof *sg);
	sg->debug = weston_compositor_add_log_scope(compositor, "gl-shader-generator",
						      "Debug messages from GL renderer",
						      NULL, NULL, NULL);
	return sg;
}

void
gl_shader_generator_destroy(struct gl_shader_generator *sg)
{
	weston_log_scope_destroy(sg->debug);
	sg->debug = NULL;
	free(sg);
}
