/*
 * Copyright 2018 Advanced Micro Devices, Inc.
 * All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * on the rights to use, copy, modify, merge, publish, distribute, sub
 * license, and/or sell copies of the Software, and to permit persons to whom
 * the Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHOR(S) AND/OR THEIR SUPPLIERS BE LIABLE FOR ANY CLAIM,
 * DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
 * OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE
 * USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#include "si_pipe.h"
#include "tgsi/tgsi_text.h"
#include "tgsi/tgsi_ureg.h"

void *si_get_blitter_vs(struct si_context *sctx, enum blitter_attrib_type type,
			unsigned num_layers)
{
	unsigned vs_blit_property;
	void **vs;

	switch (type) {
	case UTIL_BLITTER_ATTRIB_NONE:
		vs = num_layers > 1 ? &sctx->vs_blit_pos_layered :
				      &sctx->vs_blit_pos;
		vs_blit_property = SI_VS_BLIT_SGPRS_POS;
		break;
	case UTIL_BLITTER_ATTRIB_COLOR:
		vs = num_layers > 1 ? &sctx->vs_blit_color_layered :
				      &sctx->vs_blit_color;
		vs_blit_property = SI_VS_BLIT_SGPRS_POS_COLOR;
		break;
	case UTIL_BLITTER_ATTRIB_TEXCOORD_XY:
	case UTIL_BLITTER_ATTRIB_TEXCOORD_XYZW:
		assert(num_layers == 1);
		vs = &sctx->vs_blit_texcoord;
		vs_blit_property = SI_VS_BLIT_SGPRS_POS_TEXCOORD;
		break;
	default:
		assert(0);
		return NULL;
	}
	if (*vs)
		return *vs;

	struct ureg_program *ureg = ureg_create(PIPE_SHADER_VERTEX);
	if (!ureg)
		return NULL;

	/* Tell the shader to load VS inputs from SGPRs: */
	ureg_property(ureg, TGSI_PROPERTY_VS_BLIT_SGPRS, vs_blit_property);
	ureg_property(ureg, TGSI_PROPERTY_VS_WINDOW_SPACE_POSITION, true);

	/* This is just a pass-through shader with 1-3 MOV instructions. */
	ureg_MOV(ureg,
		 ureg_DECL_output(ureg, TGSI_SEMANTIC_POSITION, 0),
		 ureg_DECL_vs_input(ureg, 0));

	if (type != UTIL_BLITTER_ATTRIB_NONE) {
		ureg_MOV(ureg,
			 ureg_DECL_output(ureg, TGSI_SEMANTIC_GENERIC, 0),
			 ureg_DECL_vs_input(ureg, 1));
	}

	if (num_layers > 1) {
		struct ureg_src instance_id =
			ureg_DECL_system_value(ureg, TGSI_SEMANTIC_INSTANCEID, 0);
		struct ureg_dst layer =
			ureg_DECL_output(ureg, TGSI_SEMANTIC_LAYER, 0);

		ureg_MOV(ureg, ureg_writemask(layer, TGSI_WRITEMASK_X),
			 ureg_scalar(instance_id, TGSI_SWIZZLE_X));
	}
	ureg_END(ureg);

	*vs = ureg_create_shader_and_destroy(ureg, &sctx->b);
	return *vs;
}

/**
 * This is used when TCS is NULL in the VS->TCS->TES chain. In this case,
 * VS passes its outputs to TES directly, so the fixed-function shader only
 * has to write TESSOUTER and TESSINNER.
 */
void *si_create_fixed_func_tcs(struct si_context *sctx)
{
	struct ureg_src outer, inner;
	struct ureg_dst tessouter, tessinner;
	struct ureg_program *ureg = ureg_create(PIPE_SHADER_TESS_CTRL);

	if (!ureg)
		return NULL;

	outer = ureg_DECL_system_value(ureg,
				       TGSI_SEMANTIC_DEFAULT_TESSOUTER_SI, 0);
	inner = ureg_DECL_system_value(ureg,
				       TGSI_SEMANTIC_DEFAULT_TESSINNER_SI, 0);

	tessouter = ureg_DECL_output(ureg, TGSI_SEMANTIC_TESSOUTER, 0);
	tessinner = ureg_DECL_output(ureg, TGSI_SEMANTIC_TESSINNER, 0);

	ureg_MOV(ureg, tessouter, outer);
	ureg_MOV(ureg, tessinner, inner);
	ureg_END(ureg);

	return ureg_create_shader_and_destroy(ureg, &sctx->b);
}

/* Create the compute shader that is used to collect the results.
 *
 * One compute grid with a single thread is launched for every query result
 * buffer. The thread (optionally) reads a previous summary buffer, then
 * accumulates data from the query result buffer, and writes the result either
 * to a summary buffer to be consumed by the next grid invocation or to the
 * user-supplied buffer.
 *
 * Data layout:
 *
 * CONST
 *  0.x = end_offset
 *  0.y = result_stride
 *  0.z = result_count
 *  0.w = bit field:
 *          1: read previously accumulated values
 *          2: write accumulated values for chaining
 *          4: write result available
 *          8: convert result to boolean (0/1)
 *         16: only read one dword and use that as result
 *         32: apply timestamp conversion
 *         64: store full 64 bits result
 *        128: store signed 32 bits result
 *        256: SO_OVERFLOW mode: take the difference of two successive half-pairs
 *  1.x = fence_offset
 *  1.y = pair_stride
 *  1.z = pair_count
 *
 * BUFFER[0] = query result buffer
 * BUFFER[1] = previous summary buffer
 * BUFFER[2] = next summary buffer or user-supplied buffer
 */
void *si_create_query_result_cs(struct si_context *sctx)
{
	/* TEMP[0].xy = accumulated result so far
	 * TEMP[0].z = result not available
	 *
	 * TEMP[1].x = current result index
	 * TEMP[1].y = current pair index
	 */
	static const char text_tmpl[] =
		"COMP\n"
		"PROPERTY CS_FIXED_BLOCK_WIDTH 1\n"
		"PROPERTY CS_FIXED_BLOCK_HEIGHT 1\n"
		"PROPERTY CS_FIXED_BLOCK_DEPTH 1\n"
		"DCL BUFFER[0]\n"
		"DCL BUFFER[1]\n"
		"DCL BUFFER[2]\n"
		"DCL CONST[0][0..1]\n"
		"DCL TEMP[0..5]\n"
		"IMM[0] UINT32 {0, 31, 2147483647, 4294967295}\n"
		"IMM[1] UINT32 {1, 2, 4, 8}\n"
		"IMM[2] UINT32 {16, 32, 64, 128}\n"
		"IMM[3] UINT32 {1000000, 0, %u, 0}\n" /* for timestamp conversion */
		"IMM[4] UINT32 {256, 0, 0, 0}\n"

		"AND TEMP[5], CONST[0][0].wwww, IMM[2].xxxx\n"
		"UIF TEMP[5]\n"
			/* Check result availability. */
			"LOAD TEMP[1].x, BUFFER[0], CONST[0][1].xxxx\n"
			"ISHR TEMP[0].z, TEMP[1].xxxx, IMM[0].yyyy\n"
			"MOV TEMP[1], TEMP[0].zzzz\n"
			"NOT TEMP[0].z, TEMP[0].zzzz\n"

			/* Load result if available. */
			"UIF TEMP[1]\n"
				"LOAD TEMP[0].xy, BUFFER[0], IMM[0].xxxx\n"
			"ENDIF\n"
		"ELSE\n"
			/* Load previously accumulated result if requested. */
			"MOV TEMP[0], IMM[0].xxxx\n"
			"AND TEMP[4], CONST[0][0].wwww, IMM[1].xxxx\n"
			"UIF TEMP[4]\n"
				"LOAD TEMP[0].xyz, BUFFER[1], IMM[0].xxxx\n"
			"ENDIF\n"

			"MOV TEMP[1].x, IMM[0].xxxx\n"
			"BGNLOOP\n"
				/* Break if accumulated result so far is not available. */
				"UIF TEMP[0].zzzz\n"
					"BRK\n"
				"ENDIF\n"

				/* Break if result_index >= result_count. */
				"USGE TEMP[5], TEMP[1].xxxx, CONST[0][0].zzzz\n"
				"UIF TEMP[5]\n"
					"BRK\n"
				"ENDIF\n"

				/* Load fence and check result availability */
				"UMAD TEMP[5].x, TEMP[1].xxxx, CONST[0][0].yyyy, CONST[0][1].xxxx\n"
				"LOAD TEMP[5].x, BUFFER[0], TEMP[5].xxxx\n"
				"ISHR TEMP[0].z, TEMP[5].xxxx, IMM[0].yyyy\n"
				"NOT TEMP[0].z, TEMP[0].zzzz\n"
				"UIF TEMP[0].zzzz\n"
					"BRK\n"
				"ENDIF\n"

				"MOV TEMP[1].y, IMM[0].xxxx\n"
				"BGNLOOP\n"
					/* Load start and end. */
					"UMUL TEMP[5].x, TEMP[1].xxxx, CONST[0][0].yyyy\n"
					"UMAD TEMP[5].x, TEMP[1].yyyy, CONST[0][1].yyyy, TEMP[5].xxxx\n"
					"LOAD TEMP[2].xy, BUFFER[0], TEMP[5].xxxx\n"

					"UADD TEMP[5].y, TEMP[5].xxxx, CONST[0][0].xxxx\n"
					"LOAD TEMP[3].xy, BUFFER[0], TEMP[5].yyyy\n"

					"U64ADD TEMP[4].xy, TEMP[3], -TEMP[2]\n"

					"AND TEMP[5].z, CONST[0][0].wwww, IMM[4].xxxx\n"
					"UIF TEMP[5].zzzz\n"
						/* Load second start/end half-pair and
						 * take the difference
						 */
						"UADD TEMP[5].xy, TEMP[5], IMM[1].wwww\n"
						"LOAD TEMP[2].xy, BUFFER[0], TEMP[5].xxxx\n"
						"LOAD TEMP[3].xy, BUFFER[0], TEMP[5].yyyy\n"

						"U64ADD TEMP[3].xy, TEMP[3], -TEMP[2]\n"
						"U64ADD TEMP[4].xy, TEMP[4], -TEMP[3]\n"
					"ENDIF\n"

					"U64ADD TEMP[0].xy, TEMP[0], TEMP[4]\n"

					/* Increment pair index */
					"UADD TEMP[1].y, TEMP[1].yyyy, IMM[1].xxxx\n"
					"USGE TEMP[5], TEMP[1].yyyy, CONST[0][1].zzzz\n"
					"UIF TEMP[5]\n"
						"BRK\n"
					"ENDIF\n"
				"ENDLOOP\n"

				/* Increment result index */
				"UADD TEMP[1].x, TEMP[1].xxxx, IMM[1].xxxx\n"
			"ENDLOOP\n"
		"ENDIF\n"

		"AND TEMP[4], CONST[0][0].wwww, IMM[1].yyyy\n"
		"UIF TEMP[4]\n"
			/* Store accumulated data for chaining. */
			"STORE BUFFER[2].xyz, IMM[0].xxxx, TEMP[0]\n"
		"ELSE\n"
			"AND TEMP[4], CONST[0][0].wwww, IMM[1].zzzz\n"
			"UIF TEMP[4]\n"
				/* Store result availability. */
				"NOT TEMP[0].z, TEMP[0]\n"
				"AND TEMP[0].z, TEMP[0].zzzz, IMM[1].xxxx\n"
				"STORE BUFFER[2].x, IMM[0].xxxx, TEMP[0].zzzz\n"

				"AND TEMP[4], CONST[0][0].wwww, IMM[2].zzzz\n"
				"UIF TEMP[4]\n"
					"STORE BUFFER[2].y, IMM[0].xxxx, IMM[0].xxxx\n"
				"ENDIF\n"
			"ELSE\n"
				/* Store result if it is available. */
				"NOT TEMP[4], TEMP[0].zzzz\n"
				"UIF TEMP[4]\n"
					/* Apply timestamp conversion */
					"AND TEMP[4], CONST[0][0].wwww, IMM[2].yyyy\n"
					"UIF TEMP[4]\n"
						"U64MUL TEMP[0].xy, TEMP[0], IMM[3].xyxy\n"
						"U64DIV TEMP[0].xy, TEMP[0], IMM[3].zwzw\n"
					"ENDIF\n"

					/* Convert to boolean */
					"AND TEMP[4], CONST[0][0].wwww, IMM[1].wwww\n"
					"UIF TEMP[4]\n"
						"U64SNE TEMP[0].x, TEMP[0].xyxy, IMM[4].zwzw\n"
						"AND TEMP[0].x, TEMP[0].xxxx, IMM[1].xxxx\n"
						"MOV TEMP[0].y, IMM[0].xxxx\n"
					"ENDIF\n"

					"AND TEMP[4], CONST[0][0].wwww, IMM[2].zzzz\n"
					"UIF TEMP[4]\n"
						"STORE BUFFER[2].xy, IMM[0].xxxx, TEMP[0].xyxy\n"
					"ELSE\n"
						/* Clamping */
						"UIF TEMP[0].yyyy\n"
							"MOV TEMP[0].x, IMM[0].wwww\n"
						"ENDIF\n"

						"AND TEMP[4], CONST[0][0].wwww, IMM[2].wwww\n"
						"UIF TEMP[4]\n"
							"UMIN TEMP[0].x, TEMP[0].xxxx, IMM[0].zzzz\n"
						"ENDIF\n"

						"STORE BUFFER[2].x, IMM[0].xxxx, TEMP[0].xxxx\n"
					"ENDIF\n"
				"ENDIF\n"
			"ENDIF\n"
		"ENDIF\n"

		"END\n";

	char text[sizeof(text_tmpl) + 32];
	struct tgsi_token tokens[1024];
	struct pipe_compute_state state = {};

	/* Hard code the frequency into the shader so that the backend can
	 * use the full range of optimizations for divide-by-constant.
	 */
	snprintf(text, sizeof(text), text_tmpl,
		 sctx->screen->info.clock_crystal_freq);

	if (!tgsi_text_translate(text, tokens, ARRAY_SIZE(tokens))) {
		assert(false);
		return NULL;
	}

	state.ir_type = PIPE_SHADER_IR_TGSI;
	state.prog = tokens;

	return sctx->b.create_compute_state(&sctx->b, &state);
}
