#pragma once

struct mp_filter;

// Internal automatic filter used only when a Mangetsu ASS track contains \\blend.
struct mp_filter *mp_mangetsu_blend_create(struct mp_filter *parent);
