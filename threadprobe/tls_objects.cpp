extern "C" void threadprobe_tls_note(void *context, unsigned id);

struct probe_tls_object {
  void *context;
  unsigned id;

  probe_tls_object() : context(nullptr), id(0) {}

  ~probe_tls_object()
  {
    threadprobe_tls_note(context, id);
  }
};

extern "C" void threadprobe_tls_objects(void *context)
{
  /* Separate first-use initialization makes registration order explicit. */
  static thread_local probe_tls_object first;
  first.context = context;
  first.id = 1;
  static thread_local probe_tls_object second;
  second.context = context;
  second.id = 2;
}
