// SPDX-License-Identifier: GPL-3.0-or-later

// Fuzz target: the MCP JSON-RPC envelope parser and member scanner (#601).
//
// This is the first thing that touches an MCP request body, and those bytes arrive from the network
// before any tool runs. The scanner is depth-aware precisely because a flat substring search would
// match a key spelled inside an attacker-controlled STRING VALUE -- and a scanner with that much
// index arithmetic is exactly the kind that walks off the end of a buffer when the input stops being
// well-formed halfway through.
//
// Everything driven here must fail closed: an unterminated string, a value truncated mid-escape, a
// nesting depth that never unwinds, a key with no colon, an over-long field, embedded NULs.

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include <McpProtocol.h>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    const std::string_view body(reinterpret_cast<const char*>(data), size);

    // The envelope, as the endpoint parses it. On success the params span is re-scanned exactly the
    // way the dispatcher does, so the nested lookups get the same bytes the real path would.
    fl::mcp::Request req;
    std::string errorBody;
    if (fl::mcp::parseRequest(body, req, errorBody)) {
        for (const char* key : {"name", "arguments", "uri", "absent"}) {
            const std::string_view span = fl::mcp::objectMember(req.params, key);
            (void)fl::mcp::stringValue(span);
            (void)fl::mcp::intValue(span);
            (void)fl::mcp::boolValue(span);
        }
        // Two levels deep, which is as far as any real tool reads.
        const std::string_view args = fl::mcp::objectMember(req.params, "arguments");
        for (const char* key : {"command", "yaml", "after", "max"}) {
            const std::string_view span = fl::mcp::objectMember(args, key);
            (void)fl::mcp::stringValue(span);
            (void)fl::mcp::intValue(span);
        }
        // Echoing an attacker-supplied id back into a response is a real path: the id token is
        // copied verbatim, so anything that could break out of the JSON would do it here.
        (void)fl::mcp::resultResponse(req.id, "{}");
        (void)fl::mcp::errorResponse(req.id, fl::mcp::RpcError::Internal, req.method);
    }

    // Scan the raw bytes as an object too, so malformed inputs that parseRequest rejects early still
    // exercise the scanner itself rather than stopping at the envelope check.
    for (const char* key : {"jsonrpc", "id", "method", "params"})
        (void)fl::mcp::objectMember(body, key);
    (void)fl::mcp::stringValue(body);
    (void)fl::mcp::isObject(body);

    // The allowlist takes the decoded command line, which is attacker-influenced whenever a token is
    // compromised -- and it is the last check before dispatch.
    const std::vector<std::string> allow{"status", "peers"};
    (void)fl::mcp::commandAllowed(allow, body);
    (void)fl::mcp::commandVerb(body);
    return 0;
}
