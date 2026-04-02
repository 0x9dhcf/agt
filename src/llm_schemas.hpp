#pragma once

#include <agt/json.hpp>

namespace agt::schemas {

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
