#include "replay.h"
#include "render.h"
#include "../xfer/hash.h"
#include <stdlib.h>
#include <string.h>

#define PREFIX "pyxis-config-plan 1\n"
#define SUFFIX "\nend\n"

static bool quoted(FILE *stream, const char *text)
{
  struct config_literal_value value = {
    .kind = CONFIG_LITERAL_STRING,
    .as.string = {.bytes = (char *)text, .size = strlen(text)},
  };
  char *bytes;
  size_t size;
  if (!config_render_value(&value, &bytes, &size)) {
    return false;
  }
  bool ok = fwrite(bytes, 1, size, stream) == size;
  free(bytes);
  return ok;
}

bool config_replay_write(FILE *stream, const char *target,
    const struct config_replay_input inputs[CONFIG_REPLAY_INPUTS],
    const struct config_delta_plan *plan)
{
  if (!target || !*target || strlen(target) > CONFIG_REPLAY_TARGET_MAX ||
      fputs(PREFIX "return { target = ", stream) == EOF || !quoted(stream, target) ||
      fputs(", inputs = {\n", stream) == EOF) {
    return false;
  }
  for (size_t i = 0; i < CONFIG_REPLAY_INPUTS; ++i) {
    if (!digest_valid(inputs[i].sha256) || fprintf(stream,
        "  { present = %s, sha256 = \"%s\" },\n",
        inputs[i].present ? "true" : "false", inputs[i].sha256) < 0) {
      return false;
    }
  }
  if (fputs("}, choices = {\n", stream) == EOF) {
    return false;
  }
  for (size_t i = 0; i < plan->count; ++i) {
    const struct config_plan_change *change = &plan->changes[i];
    const char *id = change->entry ? change->entry->id : change->old_entry->id;
    if (fputs("  { id = ", stream) == EOF || !quoted(stream, id) ||
        fprintf(stream, ", choice = \"%s\" },\n", config_plan_choice_name(change->choice)) < 0) {
      return false;
    }
  }
  return fputs("} }" SUFFIX, stream) != EOF && fflush(stream) == 0;
}

static bool keys(const struct config_literal_value *table, const char *const *allowed, size_t count)
{
  if (!table || table->kind != CONFIG_LITERAL_TABLE || table->as.table.count != count) {
    return false;
  }
  for (const struct config_literal_entry *entry = table->as.table.entries; entry; entry = entry->next) {
    if (entry->key->kind != CONFIG_LITERAL_STRING) {
      return false;
    }
    bool found = false;
    for (size_t i = 0; i < count; ++i) {
      if (entry->key->as.string.size == strlen(allowed[i]) &&
          !memcmp(entry->key->as.string.bytes, allowed[i], strlen(allowed[i]))) {
        found = true;
        break;
      }
    }
    if (!found) {
      return false;
    }
  }
  return true;
}

static bool text(const struct config_literal_value *value, char *output, size_t capacity)
{
  if (!value || value->kind != CONFIG_LITERAL_STRING || !value->as.string.size ||
      value->as.string.size >= capacity || memchr(value->as.string.bytes, '\0', value->as.string.size)) {
    return false;
  }
  memcpy(output, value->as.string.bytes, value->as.string.size);
  output[value->as.string.size] = '\0';
  return true;
}

static const struct config_literal_value *index_value(const struct config_literal_value *array, size_t index)
{
  for (const struct config_literal_entry *entry = array->as.table.entries; entry; entry = entry->next) {
    if ((entry->key->kind == CONFIG_LITERAL_INTEGER && entry->key->as.integer == (lua_Integer)index) ||
        (entry->key->kind == CONFIG_LITERAL_NUMBER && entry->key->as.number == (lua_Number)index)) {
      return entry->value;
    }
  }
  return NULL;
}

static bool decode(const struct config_literal *document, struct config_replay *replay)
{
  const char *root_keys[] = {"target", "inputs", "choices"};
  if (!keys(document->root, root_keys, 3) ||
      !text(config_literal_field(document->root, "target"), replay->target, sizeof(replay->target))) {
    return false;
  }
  const struct config_literal_value *inputs = config_literal_field(document->root, "inputs");
  if (!inputs || inputs->kind != CONFIG_LITERAL_TABLE || inputs->as.table.count != CONFIG_REPLAY_INPUTS) {
    return false;
  }
  for (size_t i = 0; i < CONFIG_REPLAY_INPUTS; ++i) {
    const struct config_literal_value *item = index_value(inputs, i + 1);
    const char *input_keys[] = {"present", "sha256"};
    if (!keys(item, input_keys, 2)) {
      return false;
    }
    const struct config_literal_value *present = config_literal_field(item, "present");
    if (!present || present->kind != CONFIG_LITERAL_BOOLEAN ||
        !text(config_literal_field(item, "sha256"), replay->inputs[i].sha256, 65) ||
        !digest_valid(replay->inputs[i].sha256)) {
      return false;
    }
    replay->inputs[i].present = present->as.boolean;
  }
  const struct config_literal_value *choices = config_literal_field(document->root, "choices");
  if (!choices || choices->kind != CONFIG_LITERAL_TABLE || choices->as.table.count > CONFIG_PLAN_CHANGE_LIMIT) {
    return false;
  }
  replay->count = choices->as.table.count;
  for (size_t i = 0; i < replay->count; ++i) {
    const struct config_literal_value *item = index_value(choices, i + 1);
    const char *choice_keys[] = {"id", "choice"};
    char choice[16];
    if (!keys(item, choice_keys, 2) ||
        !text(config_literal_field(item, "id"), replay->choices[i].id, sizeof(replay->choices[i].id)) ||
        !config_entry_id_valid(replay->choices[i].id, strlen(replay->choices[i].id)) ||
        !text(config_literal_field(item, "choice"), choice, sizeof(choice))) {
      return false;
    }
    bool known = false;
    for (unsigned j = CONFIG_CHOICE_KEEP; j <= CONFIG_CHOICE_REMOVE; ++j) {
      if (!strcmp(choice, config_plan_choice_name(j))) {
        replay->choices[i].choice = j;
        known = true;
        break;
      }
    }
    if (!known) {
      return false;
    }
    for (size_t j = 0; j < i; ++j) {
      if (!strcmp(replay->choices[i].id, replay->choices[j].id)) {
        return false;
      }
    }
  }
  return true;
}

bool config_replay_read(FILE *stream, struct config_replay *replay)
{
  memset(replay, 0, sizeof(*replay));
  char *bytes = malloc(CONFIG_LITERAL_SOURCE_LIMIT + 1);
  if (!bytes) {
    return false;
  }
  size_t size = 0;
  bool framed = false;
  while (size <= CONFIG_LITERAL_SOURCE_LIMIT) {
    int byte = fgetc(stream);
    if (byte == EOF) {
      break;
    }
    bytes[size++] = (char)byte;
    if (size >= strlen(PREFIX) + strlen(SUFFIX) &&
        !memcmp(bytes + size - strlen(SUFFIX), SUFFIX, strlen(SUFFIX))) {
      framed = true;
      break;
    }
  }
  bool ok = framed && !ferror(stream) && size <= CONFIG_LITERAL_SOURCE_LIMIT &&
      size >= strlen(PREFIX) + strlen(SUFFIX) && !memcmp(bytes, PREFIX, strlen(PREFIX)) &&
      !memcmp(bytes + size - strlen(SUFFIX), SUFFIX, strlen(SUFFIX));
  struct config_literal *document = NULL;
  struct config_literal_diagnostic diagnostic;
  if (ok) {
    ok = config_literal_parse(bytes + strlen(PREFIX), size - strlen(PREFIX) - strlen(SUFFIX),
        &document, &diagnostic) && decode(document, replay);
  }
  config_literal_free(document);
  free(bytes);
  if (!ok) {
    memset(replay, 0, sizeof(*replay));
    fputs("config: invalid or oversized framed plan\n", stderr);
  }
  return ok;
}

bool config_replay_matches(const struct config_replay *replay, const char *target,
    const struct config_replay_input inputs[CONFIG_REPLAY_INPUTS], struct config_delta_plan *plan)
{
  if (!target || strcmp(replay->target, target) || replay->count != plan->count) {
    return false;
  }
  for (size_t i = 0; i < CONFIG_REPLAY_INPUTS; ++i) {
    if (replay->inputs[i].present != inputs[i].present ||
        !digest_equal(replay->inputs[i].sha256, inputs[i].sha256)) {
      return false;
    }
  }
  for (size_t i = 0; i < plan->count; ++i) {
    const struct config_plan_change *change = &plan->changes[i];
    const char *id = change->entry ? change->entry->id : change->old_entry->id;
    if (strcmp(id, replay->choices[i].id) ||
        !config_delta_plan_choose(plan, i, replay->choices[i].choice)) {
      return false;
    }
  }
  return true;
}
