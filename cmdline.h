#ifndef CMDLINE_H
#define CMDLINE_H

#include "gbuf.h"

extern struct gbuf cmdline;
extern unsigned int cmdline_pos;

void cmdline_insert(unsigned int u);
void cmdline_delete(void);
void cmdline_backspace(void);
void cmdline_erase_word(void);
void cmdline_delete_bol(void);
void cmdline_prev_char(void);
void cmdline_next_char(void);
void cmdline_clear(void);
void cmdline_set_text(const char *text);
void cmdline_insert_bytes(const char *buf, int size);

#endif
