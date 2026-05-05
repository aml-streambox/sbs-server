#include "sbs/compositor_scene.h"

#include <string.h>

void sbs_comp_scene_state_init(sbs_comp_scene_state_t *state)
{
    if (!state) {
        return;
    }
    memset(state, 0, sizeof(*state));
    state->background_rgba[3] = 1.0f;
    state->transition_progress = 1.0f;
}
