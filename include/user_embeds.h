#ifndef USER_EMBEDS_H
#define USER_EMBEDS_H

#include <stddef.h>
#include <stdint.h>

struct user_embed {
    const char *name;
    const uint8_t *start;
    const uint8_t *end;
};

extern const struct user_embed user_embed_table[];
extern const size_t user_embed_count;

const struct user_embed *user_embed_lookup(const char *name);
size_t user_embed_size(const struct user_embed *emb);

#endif
