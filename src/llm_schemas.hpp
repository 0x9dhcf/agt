#pragma once

#include <agt/json.hpp>

namespace agt::schemas {

// The canonical request/response schemas are the contract every provider
// backend translates to and from. They are intentionally strict: every
// supported field has a defined shape and is validated on both ingress
// (in Llm::complete) and egress.
//
// Notably absent: any field that lets a caller smuggle un-translated,
// provider-specific JSON into the request (e.g. an "extra_tools" or
// "provider_tools" passthrough). That omission is deliberate. Server-side
// provider tools (Anthropic web_search, Gemini google_search, OpenAI
// web_search_preview / code_interpreter / file_search, Mistral connectors)
// each live in different request slots, take different tunables, lack a
// canonical response representation, and in some cases require endpoints
// agt does not target. A passthrough field would corrode the canonical
// model — once accepted, every future provider-specific feature arrives
// through the hatch rather than being designed against the contract, and
// Runner / hooks / sessions can no longer reason about what tools the
// model actually invoked.
//
// The supported way to add capabilities like web search is to implement
// them as an agt::Tool whose execute() runs locally (HTTP to a search API,
// an MCP server, etc.). That keeps the feature observable through the
// normal lifecycle hooks, replayable through sessions, and portable across
// providers. See README.md "Design" / "Non-goals" for the full rationale.

inline const Json llm_input_schema = R"({
    "type": "object",
    "required": ["messages"],
    "properties": {
        "system": { "type": "string" },
        "messages": {
            "type": "array",
            "items": {
                "type": "object",
                "required": ["role", "content"],
                "properties": {
                    "role": { "enum": ["user", "assistant", "tool"] },
                    "content": { "type": ["string", "null"] },
                    "call_id": { "type": "string" },
                    "calls": {
                        "type": "array",
                        "items": {
                            "type": "object",
                            "required": ["id", "name", "input"],
                            "properties": {
                                "id": { "type": "string" },
                                "name": { "type": "string" },
                                "input": { "type": "string" }
                            }
                        }
                    }
                }
            }
        },
        "tools": {
            "type": "array",
            "items": {
                "type": "object",
                "required": ["name", "description", "parameters"],
                "properties": {
                    "name": { "type": "string" },
                    "description": { "type": "string" },
                    "parameters": { "type": "object" }
                }
            }
        },
        "max_tokens": { "type": "integer" },
        "thinking_effort": { "enum": ["none", "low", "medium", "high"] }
    }
})"_json;

inline const Json llm_output_schema = R"({
    "type": "object",
    "required": ["stop_reason", "usage"],
    "properties": {
        "content": { "type": ["string", "null"] },
        "calls": {
            "type": "array",
            "items": {
                "type": "object",
                "required": ["id", "name", "input"],
                "properties": {
                    "id": { "type": "string" },
                    "name": { "type": "string" },
                    "input": { "type": "string" }
                }
            }
        },
        "stop_reason": { "enum": ["end", "max_tokens", "tool_use"] },
        "usage": {
            "type": "object",
            "required": ["input_tokens", "output_tokens"],
            "properties": {
                "input_tokens": { "type": "integer" },
                "output_tokens": { "type": "integer" }
            }
        }
    }
})"_json;

}
