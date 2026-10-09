#ifndef LIBTERM_INPUT_H
#define LIBTERM_INPUT_H

#include <term.h>

/* Stock editor path, retaining decoding with or without a startup clock. */
enum call_status term_read_editor_event(struct term_event_reader *reader,
    struct term_event *event);

#endif
