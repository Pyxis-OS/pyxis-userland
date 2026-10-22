# HTTP endpoint preparation

`http prepare GET|POST|PUT|PATCH URI [--headers-file PATH] [--timeout SECONDS] [--navigation]`
issues a reusable endpoint through capability output:

```text
api=$(http prepare POST http://10.0.2.2:18080/resource --headers-file home://api.headers --timeout 60)
first=$(printf '{}' > $api)
second=$(printf '{"next":true}' > $api)
cat $first
cat $second
cat $api
rm $api
unset api
```

The exact name before `://` selects one inherited environment capability.
Renamed roots and `json+http` work the same way; the command receives no network
authority. Preparing performs no network request. GET preparation requires OPEN_READ; write methods require OPEN_WRITE on the
root and retains only that root's READ, WRITE and REMOVE authority. Stock HTTPS
is read-only, and stock JSON roots omit REMOVE. A captured endpoint refuses
suffixes and further preparation. A GET-configured endpoint refuses WRITE.

Reading the endpoint performs GET. Writing starts a new transaction with the
selected method, and removing performs DELETE when the root grants REMOVE.
Each result is independent and retains its response snapshot. The example's
DELETE therefore requires a root provisioned with REMOVE authority.

Header input must resolve to a finite native READ FILE; TEXT bindings are refused.
The command checks its size, copies it once and checks EOF; partial input, extra
bytes, providers and streams fail. Supply CRLF-terminated HTTP header fields.
The complete private request, including the target and 48-byte prefix, must fit 4096 bytes. Temporary
header buffers are erased. Supplied fields are treated as credentials and are
dropped on cross-origin redirects. The endpoint keeps its root's existing trust
profile. Put credentials in the file rather than command arguments.

The timeout is a nonzero integer number of seconds, defaulting to 30. Overflow
fails. Each GET/DELETE starts a fresh duration at admission; writes start it at
OPEN, including staging, COMMIT and response spooling. An earlier caller deadline
still applies. Preparation uses a separate 30-second call deadline after native
header input completes.

Stdout must support owned capability handoff, such as a shell value capture.
Unsupported output fails and closes the result. The command prints no URI or
byte substitute, and diagnostics contain no header bytes.

Shell response capture uses the existing bounded staging worker for a held
provider at the nonappend final stdout. Up to seven producer stages are allowed;
the shell commits only after all stages and their subgroup retire successfully.
The worker has staging-WRITE, never COMMIT/ABORT. Failure binds a failed result;
publication failure keeps the old binding. This is not general capability-pipe
integration. Plain C/Lua can use `fopen`/`io.open` directly on an exported endpoint.
Header-file credentials and per-root trust are an interim design accepted
2026-10-15; credential storage remains a later decision.

`--navigation` selects an explicit response FILE prefix: a little-endian 32-bit
metadata length (at most 4 KiB), UTF-8 JSON `effective_url`, `media_type` and
`status`, then the raw response body. Ordinary reads stay raw. Links opts
into and validates this content; generic describe stays URI-free. The complete
encoded snapshot counts against the selected private backing budget. HTTP
failures remain failed outcomes, with framed diagnostics in their error body.
