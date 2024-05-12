
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

// Framework includes
#include <loader.h>
#include <remake.h>
#include <selector.h>

// Local includes
#include "ddr_tiny_small8x8.h"

#define UTILS_IMPLEMENTATION
#include "utils.h"

struct selector_info selector_information;

struct selector {
	struct loader_info *remakes;
	uint32_t star_x[120];
	int32_t old_mouse_x;
	int32_t old_mouse_y;
	struct rng_state rand_state;
	uint32_t remake_count;
	int32_t current_y;
};

void setup(struct loader_shared_state *state, struct loader_info *remakes, uint32_t remake_count) {
	state->selector_userdata = (struct selector *)calloc(1, sizeof(struct selector));
	struct selector *selector = (struct selector *)state->selector_userdata;

	xor_init_rng(&selector->rand_state, 0x44780142);

	selector->remake_count = remake_count;
	selector->remakes = remakes;
	selector->old_mouse_x = state->mouse_x;
	selector->old_mouse_y = state->mouse_y;

	for(uint32_t i = 0; i < 120; ++i) {
		selector->star_x[i] = xor_generate_random(&selector->rand_state) % state->buffer_width;
	}
}

void cleanup(struct loader_shared_state *state) {
	struct selector *selector = (struct selector *)state->selector_userdata;
	(void) selector;

	free(state->selector_userdata);
	state->selector_userdata = 0;
}

void key_callback(struct loader_shared_state *state, int key) {
	struct selector *selector = (struct selector *)state->selector_userdata;
	(void) selector;
	(void) key;
}

void pre_selector_run(struct loader_shared_state *state) {
	struct selector *selector = (struct selector *)state->selector_userdata;
	(void) selector;
}

void audio_callback(struct loader_shared_state *state, int16_t *audio_buffer, size_t frames) {
	struct selector *selector = (struct selector *)state->selector_userdata;
	(void) selector;

	memset(audio_buffer, 0, frames*2*sizeof(int16_t));
}

void calculate_lineposition_and_entry(uint32_t current_position, uint32_t total_entries, uint32_t visible_entries, uint32_t *first_row, uint32_t *selection_row, uint32_t *current_entry) {
	const uint32_t half_visible = visible_entries / 2;
	const uint32_t last_visible_index = visible_entries - 1;

	// Direct Calculation with Edge Case Handling
	*first_row = (current_position > half_visible && total_entries > visible_entries) ?
					 (current_position < total_entries - half_visible ? current_position - half_visible : total_entries - visible_entries) :
					 0;

	*selection_row = current_position - *first_row;

	// Combine and Clamp in One Step
	*current_entry = (*first_row + *selection_row < total_entries) ? *first_row + *selection_row : total_entries - 1;
	*selection_row = (*selection_row < visible_entries) ? *selection_row : last_visible_index;
}

static void render_stars(struct loader_shared_state *state) {
    struct selector *selector = state->selector_userdata;
    uint32_t *buffer_base = state->buffer + 79 * state->buffer_width;
    uint32_t star_colors[] = {0x444444ff, 0x777777ff, 0xaaaaaaff, 0xffffffff};

    // Loop unrolling for 8 rows (with one star per row)
    for (uint32_t row = 0; row < 8*9; row++) {
        uint32_t *dst = buffer_base + row * state->buffer_width + selector->star_x[row];
        *dst = star_colors[row % 4];  // Color based on row

        // Update star position with ternary operator
        selector->star_x[row] = (selector->star_x[row] -= (row & 3) + 1) > state->buffer_width
                              ? selector->star_x[row] + state->buffer_width
                              : selector->star_x[row];
    }
}

static void render_copper_line(struct loader_shared_state *state, uint32_t row) {
	struct selector *selector = (struct selector *)state->selector_userdata;
	(void) selector;

	uint32_t *dst = state->buffer + row * state->buffer_width;
	for(uint32_t i = 0; i < state->buffer_width; ++i) {
		dst[i] = 0x990000ff;
	}
}

#if 0
void render_text(struct loader_info *remakes, uint32_t line_count, uint32_t first_line, uint32_t *buffer, uint32_t buffer_width) {
	uint32_t x_offset, y_offset = buffer_width + 80 * buffer_width;  // Initial y offset

	for(uint32_t i = 0; i < line_count; ++i) {
		x_offset = 34;							// Reset x_offset for each new line
		uint8_t *current_line = (uint8_t *)remakes[i + first_line].display_name;
		while(*current_line) {
			uint8_t character = *current_line++;
			uint8_t *sprite = ddr_tiny_small8x8_data + ((character - 0x20) * 8 * 8);
			uint32_t *dest = buffer + x_offset + y_offset;

			for(uint32_t y = 0; y < 8; ++y) {
				for(uint32_t x = 0; x < 8; ++x) {
					uint8_t alpha = *sprite++;
					if(alpha) {
						dest[x] = ddr_tiny_small8x8_palette[alpha];
					}
				}
				dest += buffer_width;
			}
			x_offset += 8;						// Move to the next character position
		}
		y_offset += buffer_width * 8;		// Move to the next line position
	}
}
#else

#include "immintrin.h"
void render_text(struct loader_info *remakes, uint32_t line_count, uint32_t first_line, uint32_t *buffer, uint32_t buffer_width) {
	const uint32_t stride = buffer_width;

	for (uint32_t i = 0; i < line_count; ++i) {
		uint8_t *current_line = (uint8_t *)remakes[i + first_line].display_name;
		uint32_t y_offset = 81 * buffer_width + (i * 8 * buffer_width);
		uint32_t x_offset = 34;

		while (*current_line) {
			uint8_t character = *current_line++;
			uint8_t *sprite = ddr_tiny_small8x8_data + ((character - 0x20) * 8 * 8);
			uint32_t *dest = buffer + x_offset + y_offset;

			// Unrolled loop with transparency check
			for (uint32_t y = 0; y < 8; ++y) {
				dest[0] = sprite[0] ? ddr_tiny_small8x8_palette[sprite[0]] : dest[0];
				dest[1] = sprite[1] ? ddr_tiny_small8x8_palette[sprite[1]] : dest[1];
				dest[2] = sprite[2] ? ddr_tiny_small8x8_palette[sprite[2]] : dest[2];
				dest[3] = sprite[3] ? ddr_tiny_small8x8_palette[sprite[3]] : dest[3];
				dest[4] = sprite[4] ? ddr_tiny_small8x8_palette[sprite[4]] : dest[4];
				dest[5] = sprite[5] ? ddr_tiny_small8x8_palette[sprite[5]] : dest[5];
				dest[6] = sprite[6] ? ddr_tiny_small8x8_palette[sprite[6]] : dest[6];
				dest[7] = sprite[7] ? ddr_tiny_small8x8_palette[sprite[7]] : dest[7];
				dest += stride;
				sprite += 8;
			}
			x_offset += 8;
		}
	}
}


#endif

void render_selectionbar(struct loader_shared_state *state, uint32_t selection_row) {
	uint32_t select_color_bar[] = { 0x00660000, 0x00440000, 0x00550000, 0x00660000, 0x00550000, 0x00440000, 0x00330000, 0x00770000 };
	uint32_t *s = state->buffer + (selection_row * 8) * state->buffer_width + 80 * state->buffer_width;
	for(uint32_t i = 0; i < 8; ++i) {
		uint32_t col = select_color_bar[i];
		for(uint32_t j = 0; j < state->buffer_width; ++j) {
			*s++ = col;
		}
	}
}

/*
 * ESCAPE is used globally to exit everything.
 *
 * The return code from the main loop tells the loader what remake to load,
 */
const uint32_t SPEED_DIVISOR = 8;
uint32_t mainloop_callback(struct loader_shared_state *state) {
	struct selector *selector = state->selector_userdata;
	memset(state->buffer, 0, state->buffer_height * state->buffer_width * sizeof(uint32_t));

	// Update selector->old_mouse_y and adjust current_y based on mouse movement
	int32_t mouse_delta = state->mouse_y - selector->old_mouse_y;
	selector->old_mouse_y = state->mouse_y;
	selector->current_y += mouse_delta;

	// Retrieve max_entry and clamp selector->current_y within bounds
	uint32_t max_entry = selector->remake_count;
	int32_t max_y = (int32_t)(max_entry * SPEED_DIVISOR);
	selector->current_y = (selector->current_y < 0) ? 0 : (selector->current_y > max_y ? max_y : selector->current_y);

	// Calculate visible_entries (no casts needed since max_entry is uint32_t)
	uint32_t visible_entries = (max_entry < 9) ? max_entry : 9;

	// Calculate line position and entry
	uint32_t first_entry, selection_row, current_entry;
	calculate_lineposition_and_entry((uint32_t)selector->current_y / SPEED_DIVISOR, max_entry, visible_entries, &first_entry, &selection_row, &current_entry);

	// Render graphics and text
	render_copper_line(state, 78);
	render_stars(state);
	render_selectionbar(state, selection_row);
	render_text(selector->remakes, visible_entries, first_entry, state->buffer, state->buffer_width);
	render_copper_line(state, 78 + 8*9 + 3);

	// Handle mouse button input
	if(state->mouse_button_state[REMAKE_MOUSE_BUTTON_LEFT]) {
		return (current_entry << 8) | 1; // Use bitwise OR instead of addition
	}

	return 0;
}

struct selector_info selector_information = {
	.window_title = "MKS_first simple loader",
	.buffer_width = 368,
	.buffer_height = 276,
	.frames_per_second = 50,
	.setup = setup,
	.cleanup = cleanup,
	.key_callback = key_callback,
	.audio_callback = audio_callback,
	.mainloop_callback = mainloop_callback,
	.pre_selector_run = pre_selector_run,
};

// NOTE(peter): This is only for windows, as it's too lame to be able to getProcessAddress of the struct, like dlsym can on linux.
EXPORT struct selector_info *get_selector_information() { return &selector_information; }